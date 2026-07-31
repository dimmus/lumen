#!/usr/bin/env bash
# TTY smoke: run compositor on a free socket and attach weston-simple-shm.
# Usage (from a TTY with DRM master, not nested GNOME):
#   ./scripts/tty-smoke.sh [socket-name]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"
SOCKET="${1:-lumen-smoke}"
COMP="${BUILD}/lumen-compositor"

if [[ ! -x "$COMP" ]]; then
  echo "build compositor first: cmake -B build && cmake --build build --target lumen-compositor"
  exit 1
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="$SOCKET"

rm -f "$XDG_RUNTIME_DIR/$SOCKET"

echo "Starting compositor on $XDG_RUNTIME_DIR/$SOCKET ..."
LUMEN_LOG=info "$COMP" "$SOCKET" &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  [[ -S "$XDG_RUNTIME_DIR/$SOCKET" ]] && break
  sleep 0.1
done

if [[ ! -S "$XDG_RUNTIME_DIR/$SOCKET" ]]; then
  echo "FAIL: compositor socket not created"
  exit 1
fi

if command -v weston-simple-shm >/dev/null; then
  echo "Running weston-simple-shm (5s) ..."
  timeout 5 weston-simple-shm || true
else
  echo "SKIP: weston-simple-shm not installed"
fi

echo "Smoke complete."
