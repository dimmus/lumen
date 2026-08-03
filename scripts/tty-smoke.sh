#!/usr/bin/env bash
# TTY smoke: run compositor on a free socket and attach client(s).
# Usage (from a TTY with DRM master, not nested GNOME):
#   ./scripts/tty-smoke.sh [socket-name]
#   LUMEN_BUILD_DIR=build/release ./scripts/tty-smoke.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
SOCKET="${1:-lumen-smoke}"
COMP="${BUILD}/lumen-compositor"
CLOUDS="${BUILD}/lumen-clouds"
CLIENT_SECONDS=5

# A plain `cmake -B build` leaves CMAKE_BUILD_TYPE empty, i.e. no optimisation at all,
# which costs roughly 3x the frame rate. Say so rather than let it look like a bug.
if [[ -f "${BUILD}/CMakeCache.txt" ]]; then
  build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "${BUILD}/CMakeCache.txt")"
  if [[ -z "$build_type" || "$build_type" == "Debug" ]]; then
    echo "NOTE: ${BUILD} is unoptimised (CMAKE_BUILD_TYPE='${build_type}') — expect low fps."
    echo "      For real numbers: cmake --preset release && LUMEN_BUILD_DIR=build/release $0"
  fi
fi

if [[ ! -x "$COMP" ]]; then
  echo "build compositor first: cmake -B build && cmake --build build --target lumen-compositor"
  exit 1
fi

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "WARN: nested session (WAYLAND_DISPLAY='${WAYLAND_DISPLAY:-}' DISPLAY='${DISPLAY:-}')"
  echo "WARN: run from a TTY for DRM master — scanout stays disabled here"
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="$SOCKET"

SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"

# libwayland guards each socket name with an flock on <socket>.lock. A held lock
# means a compositor is live on this name, and unlinking its socket would strand it.
if [[ -e "$LOCK_PATH" ]]; then
  if command -v flock >/dev/null; then
    if ! flock -n "$LOCK_PATH" true; then
      echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another name"
      exit 1
    fi
  else
    echo "WARN: flock not found — cannot verify that '$SOCKET' is free"
  fi
fi

rm -f "$SOCKET_PATH" "$LOCK_PATH"

echo "Starting compositor on $SOCKET_PATH ..."
LUMEN_LOG=info "$COMP" "$SOCKET" &
PID=$!
CLOUDS_PID=""

cleanup() {
  if [[ -n "$CLOUDS_PID" ]] && kill -0 "$CLOUDS_PID" 2>/dev/null; then
    kill -INT "$CLOUDS_PID" 2>/dev/null || true
    wait "$CLOUDS_PID" 2>/dev/null || true
  fi
  if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$PID" 2>/dev/null || true
  fi
  wait "$PID" 2>/dev/null || true
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
}
trap cleanup EXIT

comp_rc=0

for _ in $(seq 1 50); do
  [[ -S "$SOCKET_PATH" ]] && break
  if ! kill -0 "$PID" 2>/dev/null; then
    wait "$PID" || comp_rc=$?
    echo "FAIL: compositor exited ($comp_rc) before creating the socket"
    exit 1
  fi
  sleep 0.1
done

if [[ ! -S "$SOCKET_PATH" ]]; then
  echo "FAIL: compositor socket not created"
  exit 1
fi

status=0

if [[ -x "$CLOUDS" ]]; then
  echo "Running lumen-clouds (${CLIENT_SECONDS}s, then Ctrl+C) ..."
  "$CLOUDS" &
  CLOUDS_PID=$!
  sleep "$CLIENT_SECONDS"
  # SIGINT is exactly what Ctrl+C delivers: the demo must shut down and exit 0.
  kill -INT "$CLOUDS_PID" 2>/dev/null || true
  clouds_rc=0
  wait "$CLOUDS_PID" || clouds_rc=$?
  CLOUDS_PID=""
  if (( clouds_rc != 0 )); then
    echo "FAIL: lumen-clouds exited $clouds_rc on SIGINT"
    status=1
  fi
else
  echo "SKIP: lumen-clouds not built"
fi

if command -v weston-simple-shm >/dev/null; then
  echo "Running weston-simple-shm (${CLIENT_SECONDS}s) ..."
  client_rc=0
  timeout "$CLIENT_SECONDS" weston-simple-shm || client_rc=$?
  # 124 means the client was still drawing when the timeout fired — the good case.
  if (( client_rc != 0 && client_rc != 124 )); then
    echo "FAIL: weston-simple-shm exited $client_rc"
    status=1
  fi
else
  echo "SKIP: weston-simple-shm not installed"
fi

if ! kill -0 "$PID" 2>/dev/null; then
  wait "$PID" || comp_rc=$?
  echo "FAIL: compositor exited ($comp_rc) during the client run"
  status=1
fi

if (( status != 0 )); then
  echo "Smoke FAILED."
  exit "$status"
fi

echo "Smoke complete."
