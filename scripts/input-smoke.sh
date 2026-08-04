#!/usr/bin/env bash
# End-to-end input check: does a keystroke reach a Wayland client?
#
# lumen-input-probe proves the stack below the compositor — devices, libinput, xkb. This
# proves the half above it: that input_router delivers to the focused surface with correct
# enter/leave. Between them they cover the whole path.
#
# Run it from the TTY you are logged into:
#   ./scripts/input-smoke.sh            # 30 seconds
#   ./scripts/input-smoke.sh 60
#
# Everything runs unattended and writes to logs, because there is nowhere to watch it: the
# compositor takes DRM master and puts the console into graphics mode, so a second terminal
# is not available and stdout is not visible while it runs. Type and move the mouse during
# the countdown; the verdict is printed after the console comes back.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
SECONDS_TO_RUN="${1:-30}"
SOCKET="lumen-input-smoke"

COMP="${BUILD}/lumen-compositor"
LOG_DIR="${LUMEN_LOG_DIR:-${ROOT}/input-smoke-logs}"
COMP_LOG="${LOG_DIR}/compositor.log"
WEV_LOG="${LOG_DIR}/wev.log"
PROBE_LOG="${LOG_DIR}/probe.log"

mkdir -p "$LOG_DIR"
: > "$COMP_LOG"; : > "$WEV_LOG"

PROBE="${BUILD}/lumen-input-probe"

if [[ ! -x "$COMP" ]]; then
  echo "build first:  cmake --preset release && cmake --build --preset release"
  echo "  (or LUMEN_BUILD_DIR=build/debug $0)"
  exit 1
fi

# The probe is not optional. It is the only stage that can tell a dead input stack from a
# routing problem, and skipping it silently — as this script first did — turns a missing
# binary into a mysterious empty wev log.
if [[ ! -x "$PROBE" ]]; then
  echo "FAIL: $PROBE is missing."
  echo "  It is built by the same command as the compositor, so its absence means this"
  echo "  build directory predates the input work. Rebuild:"
  echo "      cmake --build --preset release"
  exit 1
fi

# A stale binary is the worst failure mode here, because everything appears to run and the
# result is simply wrong. Compare against the newest tracked source: if any of them is
# newer than the compositor, the test would be measuring yesterday's code.
newest_src=""
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  newest_src=$(git -C "$ROOT" ls-files -z -- modules executables \
               | xargs -0 ls -t 2>/dev/null | head -1)
fi
if [[ -n "$newest_src" && "$ROOT/$newest_src" -nt "$COMP" ]]; then
  echo "FAIL: $COMP is older than the sources."
  echo "  newest source: $newest_src"
  echo "  Running it would test a build that predates your changes — which is exactly how"
  echo "  an afternoon disappears. Rebuild:"
  echo "      cmake --build --preset release"
  exit 1
fi

if ! command -v wev >/dev/null; then
  echo "wev is not installed, and it is the only part of this that can prove a client"
  echo "received the events. Install it and re-run:"
  echo "    sudo pacman -S wev        # Arch"
  echo "    sudo apt install wev      # Debian/Ubuntu"
  exit 1
fi

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "WARN: a session is already running (WAYLAND_DISPLAY='${WAYLAND_DISPLAY:-}'"
  echo "      DISPLAY='${DISPLAY:-}'). Run this from a TTY — without DRM master the"
  echo "      compositor cannot take the console, and input will go to the other session."
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
rm -f "$SOCKET_PATH" "${SOCKET_PATH}.lock"

COMP_PID=""
WEV_PID=""

cleanup() {
  [[ -n "$WEV_PID" ]] && kill -TERM "$WEV_PID" 2>/dev/null
  if [[ -n "$COMP_PID" ]]; then
    # SIGTERM, not SIGKILL: teardown is what restores the console mode, and skipping it
    # leaves the TTY in graphics mode with no way to read anything.
    kill -TERM "$COMP_PID" 2>/dev/null
    for _ in $(seq 1 30); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$COMP_PID" 2>/dev/null
  fi
  wait 2>/dev/null
  rm -f "$SOCKET_PATH" "${SOCKET_PATH}.lock"
}
trap cleanup EXIT INT TERM

# The probe first, on its own: if the devices cannot be opened there is no point starting a
# compositor, and the reason is far clearer from the probe's own diagnostics.
echo "1/3  checking the input stack (2s) ..."
"$PROBE" 2 -o "$PROBE_LOG" >/dev/null 2>&1
if (( $? != 0 )); then
  echo
  echo "The input stack itself is not working, so a client test would only be confusing."
  echo "Verdict from $PROBE_LOG:"
  sed -n '/VERDICT/,$p' "$PROBE_LOG"
  exit 1
fi
echo "     ok: devices opened, keymap compiled"

echo "2/3  starting compositor on $SOCKET ..."
LUMEN_LOG=info "$COMP" "$SOCKET" >"$COMP_LOG" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 60); do
  [[ -S "$SOCKET_PATH" ]] && break
  if ! kill -0 "$COMP_PID" 2>/dev/null; then
    echo "FAIL: compositor exited before creating the socket. Last lines:"
    tail -20 "$COMP_LOG"
    exit 1
  fi
  sleep 0.1
done
[[ -S "$SOCKET_PATH" ]] || { echo "FAIL: no socket after 6s"; tail -20 "$COMP_LOG"; exit 1; }

# The compositor opens its own seat, separately from the probe. If that line is absent the
# client cannot receive anything and the rest of the run is noise.
sleep 0.5
if ! grep -q "libinput seat opened" "$COMP_LOG"; then
  echo "FAIL: the compositor did not open an input seat."
  echo "  The probe could open devices, so this is the compositor's own attempt failing."
  grep -iE "input|seat" "$COMP_LOG" | tail -10
  echo "  (no 'compositor.input' lines at all usually means a stale binary)"
  exit 1
fi
echo "     compositor opened its seat"

echo "3/3  starting wev; type and move the mouse for ${SECONDS_TO_RUN}s ..."
WAYLAND_DISPLAY="$SOCKET" wev >"$WEV_LOG" 2>&1 &
WEV_PID=$!
sleep 1
if ! kill -0 "$WEV_PID" 2>/dev/null; then
  echo "FAIL: wev exited immediately. Its output:"
  cat "$WEV_LOG"
  exit 1
fi

sleep "$SECONDS_TO_RUN"

kill -TERM "$WEV_PID" 2>/dev/null; wait "$WEV_PID" 2>/dev/null; WEV_PID=""
kill -TERM "$COMP_PID" 2>/dev/null
for _ in $(seq 1 30); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
COMP_PID=""

# ── Verdict ──────────────────────────────────────────────────────────────────
# Counted separately because they fail independently and mean different things.
enter=$(grep -c "wl_keyboard.enter"   "$WEV_LOG" 2>/dev/null || echo 0)
keys=$(grep -c  "wl_keyboard.key"     "$WEV_LOG" 2>/dev/null || echo 0)
mods=$(grep -c  "wl_keyboard.modifiers" "$WEV_LOG" 2>/dev/null || echo 0)
ptr=$(grep -c   "wl_pointer.motion"   "$WEV_LOG" 2>/dev/null || echo 0)
btn=$(grep -c   "wl_pointer.button"   "$WEV_LOG" 2>/dev/null || echo 0)

echo
echo "──────────────────────────────────────────────────────────────"
echo "keyboard.enter=$enter  key=$keys  modifiers=$mods"
echo "pointer.motion=$ptr    button=$btn"
echo
echo "logs: $COMP_LOG"
echo "      $WEV_LOG"
[[ -f "$PROBE_LOG" ]] && echo "      $PROBE_LOG"
echo

if (( enter == 0 )); then
  echo "VERDICT: the client never received keyboard focus. Events cannot arrive without"
  echo "it, so this is focus, not delivery — check that the toplevel mapped and that"
  echo "input_router::set_keyboard_focus ran. Compositor log:"
  grep -iE "seat|input|focus|keymap" "$COMP_LOG" | tail -20
  exit 2
fi
if (( keys == 0 )); then
  echo "VERDICT: focus arrived but no keys did. The break is between the libinput feed"
  echo "and input_router::send_key — the probe already showed the events exist."
  exit 2
fi
echo "VERDICT: input reaches the client. Phase 0 is proven end to end."
echo
echo "Worth checking by eye in $WEV_LOG: the first wl_keyboard.enter should carry any"
echo "keys held at the moment of focus, and be followed immediately by modifiers. That"
echo "path is what leaves a client with a stuck Shift when it is wrong."
