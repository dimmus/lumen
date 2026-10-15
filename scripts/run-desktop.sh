#!/usr/bin/env bash
# Run a Lumen session on the console: compositor, lumen-shell's panel, and one client.
#
#   ./scripts/run-desktop.sh                      # foot, from the TTY you are logged into
#   ./scripts/run-desktop.sh -- gtk4-widget-factory
#   ./scripts/run-desktop.sh -t 60 -- alacritty
#   ./scripts/run-desktop.sh --join-seat-session  # from SSH: join the active seat session
#
# The session ends when the client exits — type `exit` in foot, or use its close button —
# or after the time limit, whichever comes first. The limit is a safety net: the
# compositor owns the console while it runs, so a client that cannot be closed would
# otherwise leave no way back to the text console. Teardown stops everything with SIGTERM,
# which is what restores the console's text mode.
#
# Output goes to log files, because the console is not readable while the compositor
# holds it; a summary is printed once the console is back.
#
# Options:
#   -t SECONDS            time limit (default 600; 0 = none — only when you are sure the
#                         client can be closed)
#   --socket NAME         Wayland socket name (default lumen-0)
#   --decorations=MODE    server (Lumen draws window frames) or client; passed to the
#                         compositor
#   --no-shell            do not start lumen-shell
#   --debug               LUMEN_LOG=debug for the compositor and the shell (default info)
#   --logs DIR            log directory (default <repo>/run-desktop-logs)
#   --join-seat-session   move the compositor into the active seat0 logind session's
#                         cgroup, so logind lets libinput open input devices; needed when
#                         started from SSH (uses sudo)
#   -- CMD [ARGS...]      the client to run (default: foot)
#
# Needs a build (LUMEN_BUILD_DIR, default build/release) with lumen-compositor and
# lumen-shell. lumen-shell must sit next to lumen-compositor: the compositor accepts
# zlm_shell_v1 only from that binary.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SHELL_BIN="${BUILD}/lumen-shell"

SOCKET="lumen-0"
LIMIT=600
LOG_DIR="${ROOT}/run-desktop-logs"
LOG_LEVEL=info
WITH_SHELL=1
JOIN_SEAT=0
COMP_ARGS=()
CLIENT=()
while (( $# > 0 )); do
  case "$1" in
    -t) LIMIT="${2:?-t needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --decorations=server|--decorations=client) COMP_ARGS+=("$1") ;;
    --no-shell) WITH_SHELL=0 ;;
    --debug) LOG_LEVEL=debug ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --join-seat-session) JOIN_SEAT=1 ;;
    --) shift; CLIENT=("$@"); break ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
(( ${#CLIENT[@]} > 0 )) || CLIENT=(foot)
[[ "$LIMIT" =~ ^[0-9]+$ ]] || { echo "-t needs a whole number of seconds"; exit 2; }

# ── Preconditions ────────────────────────────────────────────────────────────
for bin in "$COMP" $( (( WITH_SHELL )) && echo "$SHELL_BIN"); do
  if [[ ! -x "$bin" ]]; then
    echo "$bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done
if ! command -v "${CLIENT[0]}" >/dev/null; then
  echo "client '${CLIENT[0]}' not found"
  exit 2
fi

# A binary older than its sources runs yesterday's code; say so, but let it run.
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  newest=$( { git -C "$ROOT" ls-files -z -- modules executables
              git -C "$ROOT" ls-files -z --others --exclude-standard -- modules executables; } |
            (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1)
  if [[ -n "$newest" && "$ROOT/$newest" -nt "$COMP" ]]; then
    echo "NOTE: $COMP is older than $newest — rebuild to run the current code:"
    echo "      cmake --build --preset release"
  fi
fi

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "NOTE: another graphical session is running; the compositor needs DRM master and"
  echo "      the console, so run this from a text console (or over SSH with"
  echo "      --join-seat-session)."
fi

# libinput opens devices through logind TakeDevice, which only the active seat session
# may do. Find that session's cgroup and compare it with ours.
SEAT_SCOPE=""
sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
if [[ -n "$sid" && "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" == "$(id -u)" ]]; then
  scope=$(loginctl show-session "$sid" -p Scope --value 2>/dev/null || true)
  [[ -n "$scope" ]] && SEAT_SCOPE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/${scope}"
fi
MY_CGROUP="/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)"
IN_SEAT=0
[[ -n "$SEAT_SCOPE" && "$MY_CGROUP" == "$SEAT_SCOPE" ]] && IN_SEAT=1
if (( ! IN_SEAT )); then
  if (( JOIN_SEAT )); then
    if [[ -z "$SEAT_SCOPE" || ! -e "$SEAT_SCOPE/cgroup.procs" ]]; then
      echo "--join-seat-session: no active seat0 session owned by $(id -un)"
      exit 2
    fi
    if ! sudo -n true 2>/dev/null; then
      echo "--join-seat-session needs passwordless sudo to move into the seat session"
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      input devices will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
# libwayland flocks <socket>.lock while a compositor serves that name; removing a live
# compositor's socket would strand it, so only clear a lock nobody holds.
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "socket '$SOCKET' is held by a running compositor — pick another with --socket"
  exit 2
fi
rm -f "$SOCKET_PATH" "$LOCK_PATH"

mkdir -p "$LOG_DIR"
COMP_LOG="$LOG_DIR/compositor.log"
SHELL_LOG="$LOG_DIR/shell.log"
CLIENT_LOG="$LOG_DIR/client.log"
: >"$COMP_LOG"; : >"$SHELL_LOG"; : >"$CLIENT_LOG"

# ── Processes and cleanup ────────────────────────────────────────────────────
COMP_PID=""; SHELL_PID=""; CLIENT_PID=""
COMP_RC=""; CLIENT_RC=""; ENDED_BY=""

stop() { # pid — SIGTERM, wait up to 5 s, then SIGKILL
  local pid=$1
  [[ -n "$pid" ]] || return 0
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

cleanup() {
  stop "$CLIENT_PID"; CLIENT_PID=""
  stop "$SHELL_PID"; SHELL_PID=""
  if [[ -n "$COMP_PID" ]]; then
    # SIGTERM, never SIGKILL first: the compositor's teardown restores text mode.
    kill -TERM "$COMP_PID" 2>/dev/null
    for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$COMP_PID" 2>/dev/null; then
      echo "WARN: compositor ignored SIGTERM for 5 s; killing it (console may stay graphical)"
      kill -9 "$COMP_PID" 2>/dev/null
    fi
    wait "$COMP_PID" 2>/dev/null
    COMP_RC=$?
    COMP_PID=""
  fi
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
}
trap cleanup EXIT
trap 'ENDED_BY=${ENDED_BY:-signal}; cleanup; exit 130' INT TERM

# ── Start ────────────────────────────────────────────────────────────────────
echo "Starting Lumen on $SOCKET with ${CLIENT[*]}$( (( LIMIT )) && echo " (time limit ${LIMIT}s)")."
echo "The console switches to the compositor now; logs go to $LOG_DIR."

if (( ! IN_SEAT && JOIN_SEAT )); then
  # The shell moves itself into the seat session's cgroup, then becomes the compositor:
  # same PID, so $! is the compositor and SIGTERM reaches it.
  bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && shift && exec env LUMEN_LOG="$1" "${@:2}"' \
    _ "$SEAT_SCOPE" "$LOG_LEVEL" "$COMP" "$SOCKET" "${COMP_ARGS[@]}" >"$COMP_LOG" 2>&1 &
else
  LUMEN_LOG="$LOG_LEVEL" "$COMP" "$SOCKET" "${COMP_ARGS[@]}" >"$COMP_LOG" 2>&1 &
fi
COMP_PID=$!

# Wait for the socket rather than a fixed sleep: ready means ready, and a compositor that
# died on startup is reported instead of waited on.
for _ in $(seq 1 100); do
  [[ -S "$SOCKET_PATH" ]] && break
  kill -0 "$COMP_PID" 2>/dev/null || break
  sleep 0.05
done
if [[ ! -S "$SOCKET_PATH" ]]; then
  ENDED_BY="compositor did not start"
else
  export WAYLAND_DISPLAY="$SOCKET"
  # Clients go to Lumen, not to whatever else the environment points at.
  unset DISPLAY
  export GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland

  if (( WITH_SHELL )); then
    LUMEN_LOG="$LOG_LEVEL" "$SHELL_BIN" >"$SHELL_LOG" 2>&1 &
    SHELL_PID=$!
    # Let the panel reserve its strip before the first window is placed, or that window
    # is placed against the whole output and sits under the panel. Bounded: a shell that
    # fails to come up must not stop the session.
    for _ in $(seq 1 60); do
      grep -q "shell.panel: configure" "$SHELL_LOG" 2>/dev/null && break
      kill -0 "$SHELL_PID" 2>/dev/null || break
      sleep 0.05
    done
  fi
  "${CLIENT[@]}" >"$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!

  # Until the client exits, the compositor dies, or the limit passes.
  deadline=$(( $(date +%s) + LIMIT ))
  while :; do
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null; CLIENT_RC=$?; CLIENT_PID=""
      ENDED_BY="client exited"
      break
    fi
    if ! kill -0 "$COMP_PID" 2>/dev/null; then
      ENDED_BY="compositor exited"
      break
    fi
    if (( LIMIT > 0 && $(date +%s) >= deadline )); then
      ENDED_BY="time limit reached"
      break
    fi
    sleep 0.2
  done
fi

cleanup
trap - EXIT

# ── Summary (the console is back) ────────────────────────────────────────────
echo "Session ended: $ENDED_BY."
echo "  compositor exit=${COMP_RC:-?}${CLIENT_RC:+  ${CLIENT[0]} exit=$CLIENT_RC}"
errors=$(grep -cE '^\[(ERROR|WARN)\]' "$COMP_LOG" 2>/dev/null || true)
(( errors > 0 )) && { echo "  compositor errors/warnings: $errors (first ones below)"; grep -m5 -E '^\[(ERROR|WARN)\]' "$COMP_LOG" | sed 's/^/    /'; }
[[ "$ENDED_BY" == "compositor did not start" ]] && tail -15 "$COMP_LOG" | sed 's/^/    /'
echo "  logs: $COMP_LOG"
(( WITH_SHELL )) && echo "        $SHELL_LOG"
echo "        $CLIENT_LOG"
[[ "$ENDED_BY" == "client exited" && "${COMP_RC:-1}" == "0" ]]
