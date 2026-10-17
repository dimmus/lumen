#!/usr/bin/env bash
# Monitor hotplug on the real console (ADR 008 Part B, design §D.3). The VMware guest's
# virtual monitors are added, resized and removed under a running Lumen with
# lumen-vmw-layout (DRM_VMW_UPDATE_LAYOUT — what open-vm-tools does when the host window or
# topology changes: one `change` uevent with HOTPLUG=1), and every step is judged from the
# compositor's log, lumen-shell's, the probes' own output and per-output screenshots.
#
#   ./scripts/monitor-smoke.sh --join-seat-session --countdown 0   # from SSH
#   ./scripts/monitor-smoke.sh --only add,resize,remove
#
# Options:
#   --join-seat-session   run the compositor inside the active seat0 logind session
#   --countdown S         seconds of warning before input starts (default 3)
#   --socket NAME         Wayland socket name (default lumen-monitor-smoke)
#   --logs DIR            log directory (default <repo>/monitor-smoke-logs)
#   --only LIST           comma list of groups (default: all, in this order)
#
# Groups (W0xH0 is Virtual-1's size when the run starts; it is put back at the end):
#   add      Virtual-2 1024x768 to its right: `output N added name=Virtual-2`, its pipeline
#            modesets on its own CRTC, a probe sees a second wl_output 1024x768 at x=W0,
#            lumen-shell maps a panel on it, a screenshot of each output has its size
#   pointer  the pointer crosses into Virtual-2 and back: cursor-plane commits on both CRTCs
#   place    the pointer on Virtual-2, a probe window maps there and enters Virtual-2 only; a
#            maximized probe there is configured to Virtual-2's usable area (1024x736)
#   resize   Virtual-1 becomes 1600x900 (a VM window resize): `output 0 mode 1600x900`, one
#            modeset of output 0, wl_output.mode 1600x900 to the probe, Virtual-2 moves to
#            x=1600, a screenshot of output 0 is 1600x900
#   remove   Virtual-2 goes: the probe sees `wl_output removed`, its window enters Virtual-1,
#            lumen-shell's panel there is closed, the pipeline is retired (CRTC off)
#   return   Virtual-2 comes back: the window returns to it (home)
#   vt       VT away, Virtual-1 back to W0xH0 while away, VT back: the resume probe switches
#            the mode before the resume modeset (`— resume`), `session resumed`, a screenshot
#            of output 0 is W0xH0 (a resize while away no longer leaves the display off)
#   power    Virtual-2 powered off, Virtual-3 plugged in: it comes up lit, input wakes
#            Virtual-2; every output off, Virtual-3 plugged again: it stays off until input,
#            which wakes all three
#   lock     a locker on every output, then Virtual-3 plugged in: the locker gets a configure
#            for it, `locked` holds, and a screenshot of it is the lock colour throughout
#   randr    wlr-randr (zwlr_output_manager_v1, ADR 008 Part C): lists both heads; moves,
#            scales, re-modes (one modeset of that output after its TEST_ONLY), turns (a
#            screenshot shows the panel on the left edge), disables (CRTC off) and enables
#            Virtual-2; an invalid configuration (--dryrun 16000x16000) fails its TEST_ONLY
#            without touching the screen; a valid --dryrun changes nothing; outputs.toml is
#            written (the run's own: LUMEN_OUTPUT_CONFIG=<logs>/outputs.toml)
#   swap     Virtual-2 off and a maximized window on Virtual-1, then one configuration turns
#            Virtual-1 off and Virtual-2 on: no HEADLESS-1 stand-in flashes up, the window is
#            taken in by Virtual-2 (re-fitted to its usable area: configured 1024x736); Virtual-1
#            on again: the window returns home, maximized to Virtual-1's usable area
#   mgmt-errors  every protocol error of a configuration, each killing only its client
#            (lumen-probe output-mgmt --error KIND), and a stale serial is `cancelled`
#   kanshi   kanshi with a profile for {Virtual-1, Virtual-2}: plugging Virtual-2 in
#            applies it (scale 1.25)
#   idle     two outputs idle 10 s: ≤ 4 wakeups/s of the compositor's UI thread; the same
#            layout three times: `hotplug: no change`, no output event reaches a client
#   persist  a second compositor started with the run's outputs.toml comes up as it was
#            left (Virtual-2 at scale 1.25), with one combined modeset; kanshi's apply then
#            finds nothing to change
#   Global: no ERROR/WARN (host present stalls aside), compositor exits 0, the layout is
#   back, the console is in text mode on the VT it was on.
# A step whose layout differs afterwards from what was set (vmtoolsd pushed the host's) is
# INVALID — re-run then. Do not touch the VM's mouse or window during the run.
#
# Exit status: 0 PASS, 1 FAIL, 2 could not run, 3 INVALID.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SHELL_BIN="${BUILD}/lumen-shell"
PROBE="${BUILD}/lumen-probe"
VMW="${BUILD}/lumen-vmw-layout"
INJ="${ROOT}/scripts/inject-input.py"

SOCKET="lumen-monitor-smoke"
LOG_DIR="${ROOT}/monitor-smoke-logs"
JOIN_SEAT=0
COUNTDOWN=3
ONLY="add,pointer,place,resize,remove,return,vt,power,lock,randr,swap,mgmt-errors,kanshi,idle,persist"
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --countdown) COUNTDOWN="${2:?--countdown needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --only) ONLY="${2:?--only needs a list}"; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
want() { [[ ",$ONLY," == *",$1,"* ]]; }

for bin in "$COMP" "$SHELL_BIN" "$PROBE"; do
  [[ -x "$bin" ]] || { echo "FAIL: $bin is missing. Build first."; exit 2; }
done
[[ -x "$VMW" ]] || { echo "SKIP: $VMW is not built (no <libdrm/vmwgfx_drm.h>)"; exit 2; }
sudo -n true 2>/dev/null || { echo "FAIL: needs passwordless sudo (layout ioctl, injector)"; exit 2; }
if "$VMW" --show 2>/dev/null | grep -q "Virtual-1"; then :; else
  echo "SKIP: no vmwgfx Virtual-N connectors (not a VMware guest)"; exit 2
fi
if pgrep -x lumen-composito >/dev/null; then
  echo "SKIP: another lumen-compositor runs (a user session?) — not touching it"; exit 2
fi

SEAT_SCOPE=""
sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
if [[ -n "$sid" && "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" == "$(id -u)" ]]; then
  scope=$(loginctl show-session "$sid" -p Scope --value 2>/dev/null || true)
  [[ -n "$scope" ]] && SEAT_SCOPE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/${scope}"
fi
if (( JOIN_SEAT )) && [[ -z "$SEAT_SCOPE" || ! -e "$SEAT_SCOPE/cgroup.procs" ]]; then
  echo "FAIL: --join-seat-session: no active seat0 session owned by $(id -un)"; exit 2
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another --socket"; exit 2
fi
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log "$LOG_DIR"/*.ppm "$LOG_DIR"/results.txt "$LOG_DIR"/*.jsonl \
  "$LOG_DIR"/outputs.toml* "$LOG_DIR"/kanshi.cfg
# The persisted output configuration of this run (never the user's ~/.config/lumen).
OUTCFG="$LOG_DIR/outputs.toml"
RESULTS="$LOG_DIR/results.txt"
record() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$RESULTS"; }
check() { # name condition-exit-status detail
  if [[ "$2" == 0 ]]; then record "$1" PASS "$3"; else record "$1" FAIL "$3"; fi
}

# The layout at the start: every connected Virtual-N with its first (preferred) mode, in
# connector order — put back on exit.
START_VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
mapfile -t ORIG < <("$VMW" --show | awk '$2 == "connected" { print $3 }')
(( ${#ORIG[@]} > 0 )) || { echo "FAIL: no connected Virtual-N"; exit 2; }
W0=${ORIG[0]%x*}; H0=${ORIG[0]#*x}
echo "start layout: ${ORIG[*]} (vt $START_VT)"

COMP_PID=""; CLIENT_PIDS=()
stop_pid() {
  [[ -n "$1" ]] || return 0
  kill -TERM "$1" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$1" 2>/dev/null && kill -9 "$1" 2>/dev/null
  wait "$1" 2>/dev/null
}
restore_layout() {
  sudo -n "$VMW" "${ORIG[@]}" >/dev/null 2>&1
}
cleanup() {
  local p
  for p in "${CLIENT_PIDS[@]}"; do stop_pid "$p"; done
  CLIENT_PIDS=()
  if [[ -n "$COMP_PID" ]]; then stop_pid "$COMP_PID"; COMP_PID=""; fi
  # kanshi's control socket (named after the display) is left behind when it is killed.
  rm -f "$SOCKET_PATH" "$LOCK_PATH" "$XDG_RUNTIME_DIR/fr.emersion.kanshi.$SOCKET"
  if [[ "$(cat /sys/class/tty/tty0/active 2>/dev/null)" != "$START_VT" ]]; then
    sudo -n chvt "${START_VT#tty}" 2>/dev/null
  fi
  restore_layout
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

start_client() { # log command...
  local log="$1"; shift
  WAYLAND_DISPLAY="$SOCKET" "$@" >"$log" 2>&1 &
  LAST_PID=$!
  CLIENT_PIDS+=("$LAST_PID")
}
stop_client() {
  stop_pid "$1"
  local keep=() p
  for p in "${CLIENT_PIDS[@]}"; do [[ "$p" == "$1" ]] || keep+=("$p"); done
  CLIENT_PIDS=("${keep[@]}")
}
CLOG="$LOG_DIR/compositor.log"
mark() { wc -l <"$1"; }
# Waits until PATTERN appears in FILE after line MARK; SECS at most.
wait_after() { # file mark pattern secs
  local deadline=$(( $(date +%s%N) + ${4}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null | grep -qE "$3" && return 0
    sleep 0.05
  done
  return 1
}
after() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; } # file mark
inject() { sudo -n python3 -I "$INJ" --layout "$CLOG" "$@" >>"$LOG_DIR/inject.jsonl"; }
INVALID=0
# Sets the layout (WxH[+X+Y] ...) and checks that it holds: vmtoolsd may push the host's.
layout() {
  sudo -n "$VMW" "$@" >/dev/null 2>"$LOG_DIR/vmw-err.log" || { record "layout $*" FAIL "$(cat "$LOG_DIR/vmw-err.log")"; return 1; }
  sleep 0.3
  local got want=()
  for r in "$@"; do want+=("${r%%+*}"); done
  got=$("$VMW" --show | awk '$2 == "connected" { print $3 }' | tr '\n' ' ')
  # sysfs lists modes as of the last probe: wait for the compositor's to land first.
  sleep 0.7
  got=$("$VMW" --show | awk '$2 == "connected" { print $3 }' | tr '\n' ' ')
  if [[ "${got% }" != "${want[*]}" ]]; then
    record "layout $*" INVALID "the host changed it (vmtoolsd?): ${got}"
    INVALID=1
  fi
}
screenshot() { # → one path per output, newest request
  local before
  before=$(grep -c "screenshot .* written to" "$CLOG")
  kill -USR1 "$COMP_PID" 2>/dev/null
  sleep 1.5
  after "$CLOG" "$(( 0 ))" | grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" | tail -n +"$(( before + 1 ))" | sed 's/.* to //'
}
shot_size() { head -c 64 "$1" | python3 -I -c 'import sys; p=sys.stdin.buffer.read().split(b"\n"); print(p[1].decode())' 2>/dev/null; }
shot_colour() { # path → "#rrggbb:pct" of the most common colour
  python3 -I - "$1" <<'PY'
import sys
from collections import Counter
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
c = Counter(px[i*3:i*3+3] for i in range(0, w * h, 97))
k, v = c.most_common(1)[0]
print(f"#{k.hex()}:{v * 100 // sum(c.values())}")
PY
}
ui_ctxt() { awk '/^voluntary_ctxt_switches|^nonvoluntary_ctxt_switches/ {s += $2} END {print s}' "/proc/$COMP_PID/status"; }

echo "HANDS OFF: monitors are added and removed and input is injected for about two minutes."
for (( i = COUNTDOWN; i > 0; i-- )); do echo "  starting in $i"; sleep 1; done

rm -f "$SOCKET_PATH" "$LOCK_PATH"
layout "${W0}x${H0}+0+0"
start_compositor() { # log
  if (( JOIN_SEAT )); then
    bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$2" LUMEN_OUTPUT_CONFIG="$6" "$3" "$4" --shell "$5"' \
      _ "$SEAT_SCOPE" "$LOG_DIR" "$COMP" "$SOCKET" "$SHELL_BIN" "$OUTCFG" >"$1" 2>&1 &
  else
    LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$LOG_DIR" LUMEN_OUTPUT_CONFIG="$OUTCFG" "$COMP" "$SOCKET" --shell "$SHELL_BIN" >"$1" 2>&1 &
  fi
  COMP_PID=$!
}
start_compositor "$CLOG"
if ! wait_after "$CLOG" 0 "frame clocks armed" 10; then
  record "start" FAIL "the compositor did not come up"; exit 1
fi
for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
check "start: udev hotplug monitor" "$(grep -q 'hotplug: udev monitor on drm' "$CLOG"; echo $?)" \
  "$(grep -m1 -oE 'hotplug: udev monitor on drm \S+' "$CLOG" || echo 'no monitor line')"
start_client "$LOG_DIR/shell.log" env LUMEN_LOG=debug "$SHELL_BIN"
SH_PID=$LAST_PID
wait_after "$LOG_DIR/shell.log" 0 "panel requested on output Virtual-1" 5
inject move 200 $(( H0 - 100 ))
start_client "$LOG_DIR/probe-outputs.log" "$PROBE" outputs --title watch --size 200x120 --exit-after 400
WATCH_PID=$LAST_PID
wait_after "$CLOG" 0 "map id=[0-9]+ app_id=lumen-probe" 5
sleep 0.5
TWO=("${W0}x${H0}+0+0" "1024x768+${W0}+0")

# ── add ──────────────────────────────────────────────────────────────────────────────
if want add; then
  M=$(mark "$CLOG"); MS=$(mark "$LOG_DIR/shell.log"); MP=$(mark "$LOG_DIR/probe-outputs.log")
  layout "${TWO[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ added name=Virtual-2" 5
  wait_after "$CLOG" "$M" "modeset output=[0-9]+ frame" 3
  wait_after "$LOG_DIR/shell.log" "$MS" "panel requested on output Virtual-2" 5
  sleep 0.5
  A=$(after "$CLOG" "$M")
  SLOT=$(grep -oE "output [0-9]+ added name=Virtual-2" <<<"$A" | head -1 | awk '{print $2}')
  check "add: output added, its own CRTC, one modeset" \
    "$([[ -n "$SLOT" ]] && grep -qE "output $SLOT pipeline crtc=" <<<"$A" && [[ $(grep -cE "modeset output=$SLOT " <<<"$A") == 1 ]]; echo $?)" \
    "$(grep -oE "output [0-9]+ added name=Virtual-2 .*logical=\S+|output [0-9]+ pipeline crtc=[0-9]+" <<<"$A" | tr '\n' ' ')"
  P=$(after "$LOG_DIR/probe-outputs.log" "$MP")
  check "add: a client sees the new wl_output (1024x768 at x=${W0})" \
    "$(grep -q "wl_output name=Virtual-2 mode=1024x768" <<<"$P" && grep -q "xdg_output position=${W0},0 size=1024x768 name=Virtual-2" <<<"$P"; echo $?)" \
    "$(grep -E "Virtual-2" <<<"$P" | head -2 | tr '\n' ' ')"
  check "add: lumen-shell puts a panel on it" \
    "$(after "$LOG_DIR/shell.log" "$MS" | grep -q "panel requested on output Virtual-2"; echo $?)" \
    "$(after "$LOG_DIR/shell.log" "$MS" | grep -m1 -oE "panel requested on output Virtual-2.*")"
  mapfile -t SHOTS < <(screenshot)
  sizes=""; for s in "${SHOTS[@]}"; do sizes+="$(basename "$s"):$(shot_size "$s") "; done
  check "add: a screenshot per output at its own size" \
    "$([[ "$sizes" == *"Virtual-1.ppm:${W0} ${H0}"* && "$sizes" == *"Virtual-2.ppm:1024 768"* ]]; echo $?)" "$sizes"
fi

ensure_two() {
  if ! grep -qE "output [0-9]+ name=Virtual-2 " <(tail -n 40 "$CLOG") || \
     [[ "$("$VMW" --show | awk '$2 == "connected"' | wc -l)" != 2 ]]; then
    local m; m=$(mark "$CLOG")
    layout "${TWO[@]}"
    wait_after "$CLOG" "$m" "output [0-9]+ added name=Virtual-2" 5
    sleep 1
  fi
}

# ── pointer ──────────────────────────────────────────────────────────────────────────
if want pointer; then
  ensure_two
  M=$(mark "$CLOG")
  inject drag $(( W0 - 300 )) 400 $(( W0 + 300 )) 400 --button none --steps 30 --interval-ms 12
  sleep 0.3
  inject drag $(( W0 + 300 )) 400 $(( W0 - 300 )) 400 --button none --steps 30 --interval-ms 12
  sleep 0.5
  A=$(after "$CLOG" "$M")
  c0=$(grep -cE "commit=cursor .*output=0$" <<<"$A"); c1=$(grep -cE "commit=cursor .*output=1$" <<<"$A")
  check "pointer: crosses between the outputs, cursor-plane commits on both CRTCs" \
    "$( (( c0 > 0 && c1 > 0 )); echo $?)" "cursor commits output 0: $c0, output 1: $c1"
fi

# ── place ────────────────────────────────────────────────────────────────────────────
if want place; then
  ensure_two
  inject move $(( W0 + 512 )) 400; sleep 0.3
  M=$(mark "$CLOG")
  start_client "$LOG_DIR/probe-place.log" "$PROBE" outputs --title place --size 300x200 --exit-after 400
  PLACE_PID=$LAST_PID
  wait_after "$LOG_DIR/probe-place.log" 0 "surface enter surface=toplevel" 5
  sleep 0.5
  P=$(cat "$LOG_DIR/probe-place.log")
  check "place: a window opens on the pointer's output and enters it only" \
    "$(grep "surface enter surface=toplevel" <<<"$P" | grep -qv "output=Virtual-2"; [[ $? != 0 ]] && grep -q "surface enter surface=toplevel output=Virtual-2" <<<"$P"; echo $?)" \
    "$(grep -E "surface (enter|leave)" <<<"$P" | tr '\n' ' ')"
  start_client "$LOG_DIR/probe-max.log" "$PROBE" presentation --maximize --frames 60 --title max
  MAX_PID=$LAST_PID
  wait_after "$CLOG" "$M" "maximize id=[0-9]+ on=1" 5
  sleep 1.2
  CFG=$(grep -oE "toplevel configure serial=[0-9]+ w=[0-9]+ h=[0-9]+" "$LOG_DIR/probe-max.log" | tail -1)
  check "place: maximize fills that output's usable area (1024x736 under the panel)" \
    "$([[ "$CFG" == *"w=1024 h=736" ]]; echo $?)" "${CFG:-no configure}"
  stop_client "$MAX_PID"
fi

# ── resize ───────────────────────────────────────────────────────────────────────────
if want resize; then
  ensure_two
  M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
  layout "1600x900+0+0" "1024x768+1600+0"
  wait_after "$CLOG" "$M" "output 0 mode 1600x900" 5
  wait_after "$CLOG" "$M" "modeset output=0 frame" 3
  sleep 0.8
  A=$(after "$CLOG" "$M")
  check "resize: output 0 is modeset to the new preferred mode, once" \
    "$(grep -q "output 0 mode 1600x900@[0-9]* (was ${W0}x${H0}) — hotplug" <<<"$A" && [[ $(grep -c "modeset output=0 " <<<"$A") == 1 ]]; echo $?)" \
    "$(grep -oE "output 0 mode 1600x900\S* \(was [0-9x]+\)|output 0 switched to \S+|modeset output=0 frame=[0-9]+" <<<"$A" | tr '\n' ' ')"
  P=$(after "$LOG_DIR/probe-outputs.log" "$MP")
  check "resize: clients get the new mode, the output to its right moves to x=1600" \
    "$(grep -q "wl_output name=Virtual-1 mode=1600x900" <<<"$P" && grep -q "xdg_output position=1600,0 size=1024x768 name=Virtual-2" <<<"$P"; echo $?)" \
    "$(grep -E "Virtual" <<<"$P" | head -4 | tr '\n' ' ')"
  mapfile -t SHOTS < <(screenshot)
  sizes=""; for s in "${SHOTS[@]}"; do sizes+="$(basename "$s"):$(shot_size "$s") "; done
  check "resize: the screenshot of output 0 is 1600x900" \
    "$([[ "$sizes" == *"Virtual-1.ppm:1600 900"* ]]; echo $?)" "$sizes"
  TWO=("1600x900+0+0" "1024x768+1600+0")
fi

# ── remove ───────────────────────────────────────────────────────────────────────────
if want remove; then
  ensure_two
  M=$(mark "$CLOG"); MS=$(mark "$LOG_DIR/shell.log")
  MPL=$( [[ -f "$LOG_DIR/probe-place.log" ]] && mark "$LOG_DIR/probe-place.log" || echo 0)
  layout "${TWO[0]}"
  wait_after "$CLOG" "$M" "output [0-9]+ removed name=Virtual-2" 5
  wait_after "$CLOG" "$M" "pipeline retired" 3
  sleep 0.8
  A=$(after "$CLOG" "$M")
  check "remove: the output goes, its pipeline is retired" \
    "$(grep -qE "output [0-9]+ removed name=Virtual-2" <<<"$A" && grep -q "pipeline retired" <<<"$A" && ! grep -qE "^\[(ERROR|WARN)\]" <<<"$A"; echo $?)" \
    "$(grep -oE "output [0-9]+ removed .*|evacuate .*|output [0-9]+ pipeline retired" <<<"$A" | tr '\n' ' ')"
  if [[ -f "$LOG_DIR/probe-place.log" ]]; then
    P=$(after "$LOG_DIR/probe-place.log" "$MPL")
    check "remove: the window on it moves to Virtual-1 (enter) and the wl_output is removed" \
      "$(grep -q "surface enter surface=toplevel output=Virtual-1" <<<"$P" && grep -q "wl_output removed name=Virtual-2" <<<"$P"; echo $?)" \
      "$(grep -E "surface (enter|leave)|wl_output removed" <<<"$P" | tr '\n' ' ')"
  fi
  check "remove: lumen-shell's panel there is closed" \
    "$(after "$LOG_DIR/shell.log" "$MS" | grep -q "output Virtual-2 removed: its panel goes"; echo $?)" \
    "$(after "$LOG_DIR/shell.log" "$MS" | grep -m2 -E "closed|removed" | tr '\n' ' ')"
fi

# ── return ───────────────────────────────────────────────────────────────────────────
if want return; then
  M=$(mark "$CLOG")
  MPL=$( [[ -f "$LOG_DIR/probe-place.log" ]] && mark "$LOG_DIR/probe-place.log" || echo 0)
  layout "${TWO[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ added name=Virtual-2" 5
  sleep 1
  A=$(after "$CLOG" "$M")
  if [[ -f "$LOG_DIR/probe-place.log" ]]; then
    P=$(after "$LOG_DIR/probe-place.log" "$MPL")
    check "return: the window goes back to Virtual-2 (home)" \
      "$(grep -qE "home output=[0-9]+ identifier=\"connector:Virtual-2\" windows=[1-9]" <<<"$A" && grep -q "surface enter surface=toplevel output=Virtual-2" <<<"$P"; echo $?)" \
      "$(grep -oE "home output=.*|added name=Virtual-2 .*windows_returned=[0-9]+" <<<"$A" | tr '\n' ' ') $(grep -E "surface (enter|leave)" <<<"$P" | tr '\n' ' ')"
  else
    record "return: the window goes back to Virtual-2 (home)" SKIP "needs the place group"
  fi
fi

# ── vt ───────────────────────────────────────────────────────────────────────────────
if want vt; then
  ensure_two
  M=$(mark "$CLOG")
  OTHER=$(( ${START_VT#tty} == 2 ? 3 : 2 ))
  sudo -n chvt "$OTHER"
  wait_after "$CLOG" "$M" "session paused" 5
  sleep 0.5
  sudo -n "$VMW" "${W0}x${H0}+0+0" "1024x768+${W0}+0" >/dev/null
  sleep 1.5
  sudo -n chvt "${START_VT#tty}"
  wait_after "$CLOG" "$M" "session resumed" 6
  sleep 1.5
  A=$(after "$CLOG" "$M")
  # The switch to the new mode comes before output 0's one resume modeset: no frame of the
  # old size is committed (and no modeset of a mode the connector may no longer offer).
  first_modeset=$(grep -nE "modeset output=0 " <<<"$A" | head -1 | cut -d: -f1)
  switch_line=$(grep -nE "output 0 switched to ${W0}x${H0}" <<<"$A" | head -1 | cut -d: -f1)
  check "vt: a resize while away is probed at resume, switched before the one resume modeset" \
    "$(grep -q "output 0 mode ${W0}x${H0}@[0-9]* (was 1600x900) — resume" <<<"$A" && grep -q "session resumed" <<<"$A" && ! grep -q "resume modeset rejected" <<<"$A" && [[ -n "$switch_line" && -n "$first_modeset" && $switch_line -lt $first_modeset && $(grep -c "modeset output=0 " <<<"$A") == 1 ]]; echo $?)" \
    "$(grep -oE "session (paused|resumed).*|output 0 mode \S+ \(was \S+\) — \S+|output 0 switched to \S+|modeset output=[0-9]+ frame=[0-9]+ outputs=[0-9]+|resume modeset rejected" <<<"$A" | tr '\n' ' ')"
  mapfile -t SHOTS < <(screenshot)
  sizes=""; for s in "${SHOTS[@]}"; do sizes+="$(basename "$s"):$(shot_size "$s") "; done
  check "vt: the display is lit at the new size (screenshot ${W0}x${H0})" \
    "$([[ "$sizes" == *"Virtual-1.ppm:${W0} ${H0}"* ]]; echo $?)" "$sizes"
  TWO=("${W0}x${H0}+0+0" "1024x768+${W0}+0")
fi
THREE=("${TWO[@]}" "800x600+$(( ${TWO[1]%%x*} + ${W0} ))+0")
[[ "${TWO[0]%%x*}" != "$W0" ]] && THREE=("${TWO[@]}" "800x600+$(( 1600 + 1024 ))+0")

# ── power ────────────────────────────────────────────────────────────────────────────
if want power; then
  ensure_two
  M=$(mark "$CLOG")
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" power --output Virtual-2 --set off --exit-after 1.5 >"$LOG_DIR/probe-power.log" 2>&1
  wait_after "$CLOG" "$M" "display power off .*Virtual-2" 3
  layout "${THREE[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ added name=Virtual-3" 5
  sleep 1
  A=$(after "$CLOG" "$M")
  S3=$(grep -oE "output [0-9]+ added name=Virtual-3" <<<"$A" | tail -1 | awk '{print $2}')
  check "power: a monitor plugged in while another is off comes up lit" \
    "$([[ -n "$S3" ]] && grep -qE "modeset output=$S3 " <<<"$A" && ! grep -qE "display power off .*output $S3 " <<<"$A"; echo $?)" \
    "$(grep -oE "display power off .*|output [0-9]+ added name=Virtual-3|modeset output=[0-9]+ frame=[0-9]+" <<<"$A" | tr '\n' ' ')"
  M=$(mark "$CLOG")
  inject move 300 300; sleep 0.8
  check "power: input wakes the output that was off" \
    "$(after "$CLOG" "$M" | grep -qE "display power on .*Virtual-2"; echo $?)" \
    "$(after "$CLOG" "$M" | grep -oE "display (woken|power on).*" | tr '\n' ' ')"
  M=$(mark "$CLOG")
  OFF_PIDS=()
  for o in Virtual-1 Virtual-2 Virtual-3; do
    WAYLAND_DISPLAY="$SOCKET" "$PROBE" power --output "$o" --set off --exit-after 1.5 >>"$LOG_DIR/probe-power.log" 2>&1 &
    OFF_PIDS+=($!)
  done
  wait "${OFF_PIDS[@]}" 2>/dev/null
  sleep 0.3
  layout "${TWO[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ removed name=Virtual-3" 4
  layout "${THREE[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ added name=Virtual-3" 5
  sleep 1
  A=$(after "$CLOG" "$M")
  S3=$(grep -oE "output [0-9]+ added name=Virtual-3" <<<"$A" | tail -1 | awk '{print $2}')
  check "power: with every output off, a new one stays off" \
    "$([[ -n "$S3" ]] && grep -qE "display power off .*output $S3 Virtual-3" <<<"$A"; echo $?)" \
    "$(grep -cE "display power off" <<<"$A") power-offs; $(grep -oE "output [0-9]+ added name=Virtual-3" <<<"$A" | tail -1)"
  # Resized while blanked: clients hear the new mode now, the CRTC stays off, and the wake
  # lights it at the new size.
  M=$(mark "$CLOG")
  layout "1600x900+0+0" "1024x768+1600+0" "800x600+2624+0"
  wait_after "$CLOG" "$M" "output 0 mode 1600x900" 5
  sleep 0.5
  A=$(after "$CLOG" "$M")
  check "power: a resize while blanked switches the mode, nothing is lit" \
    "$(grep -q "output 0 mode 1600x900" <<<"$A" && ! grep -qE "modeset output=" <<<"$A"; echo $?)" \
    "$(grep -oE "output 0 mode \S+ \(was \S+\)|modeset output=[0-9]+ frame=[0-9]+" <<<"$A" | tr '\n' ' ')"
  M=$(mark "$CLOG")
  inject move 400 300; sleep 1.2
  check "power: input wakes all three" \
    "$( [[ $(after "$CLOG" "$M" | grep -cE "display power on") == 3 ]]; echo $?)" \
    "$(after "$CLOG" "$M" | grep -oE "display power on .*" | tr '\n' ' ')"
  mapfile -t SHOTS < <(screenshot)
  sizes=""; for s in "${SHOTS[@]}"; do sizes+="$(basename "$s"):$(shot_size "$s") "; done
  check "power: woken, the resized output is lit at its new size (1600x900)" \
    "$([[ "$sizes" == *"Virtual-1.ppm:1600 900"* ]] && after "$CLOG" "$M" | grep -q "output 0 switched to 1600x900"; echo $?)" "$sizes"
  layout "${TWO[@]}"
  sleep 1
fi

# ── lock ─────────────────────────────────────────────────────────────────────────────
if want lock; then
  ensure_two
  M=$(mark "$CLOG")
  start_client "$LOG_DIR/probe-lock.log" "$PROBE" lock --all-outputs --unlock-after 12 --colour 0xff336633
  LOCK_PID=$LAST_PID
  wait_after "$CLOG" "$M" "session lock: locking -> locked" 4
  ML=$(mark "$LOG_DIR/probe-lock.log")
  layout "${THREE[@]}"
  wait_after "$CLOG" "$M" "output [0-9]+ added name=Virtual-3" 5
  wait_after "$CLOG" "$M" "lock surface map surface=[0-9]+ output=" 3
  sleep 0.6
  mapfile -t SHOTS < <(screenshot)
  A=$(after "$CLOG" "$M")
  cols=""; all_lock=0
  for s in "${SHOTS[@]}"; do c=$(shot_colour "$s"); cols+="$(basename "$s"):$c "; [[ "$c" == "#336633:100" ]] || all_lock=1; done
  S3=$(grep -oE "output [0-9]+ added name=Virtual-3" <<<"$A" | tail -1 | awk '{print $2}')
  check "lock: a monitor plugged in while locked gets a lock surface, every output shows the lock" \
    "$( (( all_lock == 0 && ${#SHOTS[@]} == 3 )) && grep -qE "lock surface create surface=[0-9]+ output=$S3 " <<<"$A" && ! grep -q "locked -> unlocked" <<<"$A"; echo $?)" \
    "$cols; $(grep -oE "lock surface (create|map) surface=[0-9]+ output=[0-9]+" <<<"$A" | tr '\n' ' ')"
  check "lock: the locker is configured for the new output" \
    "$(after "$LOG_DIR/probe-lock.log" "$ML" | grep -q "lock configure 800x600"; echo $?)" \
    "$(after "$LOG_DIR/probe-lock.log" "$ML" | grep -E "wl_output|lock configure" | tr '\n' ' ')"
  # Resized while locked: the lock surface is configured to the new size, nothing unlocked
  # shows at any moment.
  M=$(mark "$CLOG"); ML=$(mark "$LOG_DIR/probe-lock.log")
  layout "1600x900+0+0" "1024x768+1600+0" "800x600+2624+0"
  wait_after "$LOG_DIR/probe-lock.log" "$ML" "lock configure 1600x900" 4
  sleep 0.8
  mapfile -t SHOTS < <(screenshot)
  cols=""; all_lock=0
  for s in "${SHOTS[@]}"; do c=$(shot_colour "$s"); cols+="$(basename "$s"):$(shot_size "$s"):$c "; [[ "$c" == "#336633:100" ]] || all_lock=1; done
  check "lock: a resize while locked re-configures the lock surface, still locked everywhere" \
    "$( (( all_lock == 0 )) && after "$LOG_DIR/probe-lock.log" "$ML" | grep -q "lock configure 1600x900" && [[ "$cols" == *"Virtual-1.ppm:1600 900"* ]] && ! after "$CLOG" "$M" | grep -q "locked -> unlocked"; echo $?)" \
    "$cols"
  wait "$LOCK_PID" 2>/dev/null
  stop_client "$LOCK_PID"
  layout "${TWO[@]}"
  sleep 1
fi

# ── randr ────────────────────────────────────────────────────────────────────────────
RANDR=$(command -v wlr-randr || true)
rr() { WAYLAND_DISPLAY="$SOCKET" "$RANDR" "$@" >>"$LOG_DIR/wlr-randr.log" 2>&1; }
strip_colour() { # path x y w h → "#rrggbb", the most common colour in that rect
  python3 -I - "$@" <<'PY'
import sys
from collections import Counter
path, x0, y0, w, h = sys.argv[1], *map(int, sys.argv[2:6])
d = open(path, 'rb').read()
parts = d.split(b'\n', 3)
W, H = map(int, parts[1].split()); px = parts[3]
c = Counter()
for y in range(y0, min(H, y0 + h), 3):
    for x in range(x0, min(W, x0 + w), 3):
        i = (y * W + x) * 3
        c[px[i:i+3]] += 1
print("#" + c.most_common(1)[0][0].hex() if c else "none")
PY
}
shot_of() { # output name → the newest screenshot of it
  local f p=""
  for f in "${SHOTS[@]}"; do [[ "$f" == *"$1.ppm" ]] && p="$f"; done
  echo "$p"
}
if want randr; then
  if [[ -z "$RANDR" ]]; then
    record "randr" SKIP "wlr-randr is not installed"
  else
    ensure_two
    LIST=$(WAYLAND_DISPLAY="$SOCKET" "$RANDR" 2>&1)
    check "randr: wlr-randr lists both heads, each with its current mode" \
      "$([[ $(grep -cE '^Virtual-[12] ' <<<"$LIST") == 2 ]] && grep -q "1024x768 px.*current" <<<"$LIST" && grep -q "${W0}x${H0} px.*current" <<<"$LIST"; echo $?)" \
      "$(grep -E '^Virtual|current' <<<"$LIST" | tr -s ' ' | tr '\n' ' ')"
    SLOT2=$(grep -oE "output [0-9]+ name=Virtual-2 " "$CLOG" | tail -1 | awk '{print $2}')
    # Position: logical only.
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
    rr --output Virtual-2 --pos 0,"$H0"; RC=$?
    wait_after "$LOG_DIR/probe-outputs.log" "$MP" "xdg_output position=0,${H0} " 3
    sleep 0.3
    check "randr: --pos 0,${H0} moves Virtual-2 (no modeset), clients see it" \
      "$( (( RC == 0 )) && after "$CLOG" "$M" | grep -q "apply succeeded.*Virtual-2 .* at 0,${H0} " && ! after "$CLOG" "$M" | grep -q "modeset output=" && after "$LOG_DIR/probe-outputs.log" "$MP" | grep -q "xdg_output position=0,${H0} size=1024x768 name=Virtual-2"; echo $?)" \
      "rc=$RC $(after "$LOG_DIR/probe-outputs.log" "$MP" | grep -m1 -oE "xdg_output position=\S+ size=\S+ name=Virtual-2")"
    # Scale: logical only.
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
    rr --output Virtual-2 --scale 1.5; RC=$?
    wait_after "$LOG_DIR/probe-outputs.log" "$MP" "size=683x512 name=Virtual-2" 3
    check "randr: --scale 1.5: logical 683x512, no modeset" \
      "$( (( RC == 0 )) && after "$LOG_DIR/probe-outputs.log" "$MP" | grep -q "size=683x512 name=Virtual-2" && ! after "$CLOG" "$M" | grep -q "modeset output="; echo $?)" \
      "rc=$RC $(after "$LOG_DIR/probe-outputs.log" "$MP" | grep -m1 -oE "xdg_output \S+ size=\S+ name=Virtual-2")"
    # Mode: TEST_ONLY with a real buffer, then one modeset of that output.
    M=$(mark "$CLOG")
    rr --output Virtual-2 --mode 800x600; RC=$?
    wait_after "$CLOG" "$M" "output configuration apply (succeeded|failed)" 4
    sleep 0.5
    A=$(after "$CLOG" "$M")
    check "randr: --mode 800x600: Virtual-2 alone is modeset, once, after its test" \
      "$( (( RC == 0 )) && grep -qE "output $SLOT2 mode 800x600@[0-9]+ \(was 1024x768\) — output configuration" <<<"$A" && [[ $(grep -c "modeset output=$SLOT2 " <<<"$A") == 1 && $(grep -c "modeset output=" <<<"$A") == 1 ]] && grep -q "apply succeeded.*(committed)" <<<"$A"; echo $?)" \
      "rc=$RC $(grep -oE "output [0-9]+ mode \S+ \(was \S+\)|modeset output=[0-9]+ frame=[0-9]+|\((committed|no modeset needed|nothing to change)\)" <<<"$A" | tr '\n' ' ')"
    mapfile -t SHOTS < <(screenshot)
    before=$(shot_of Virtual-2)
    check "randr: the screenshot of Virtual-2 is 800x600" \
      "$([[ -n "$before" && "$(shot_size "$before")" == "800 600" ]]; echo $?)" "$( [[ -n "$before" ]] && shot_size "$before")"
    # Transform: the panel along the top of the content runs along the left edge of the
    # output's pixels at 90° (counter-clockwise, as wl_output.transform says).
    PANEL=$( [[ -n "$before" ]] && strip_colour "$before" 100 2 500 20)
    BG=$( [[ -n "$before" ]] && strip_colour "$before" 100 300 500 200)
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
    rr --output Virtual-2 --transform 90; RC=$?
    wait_after "$LOG_DIR/probe-outputs.log" "$MP" "size=400x533 name=Virtual-2" 3
    sleep 0.5
    mapfile -t SHOTS < <(screenshot)
    turned=$(shot_of Virtual-2)
    LEFT=$( [[ -n "$turned" ]] && strip_colour "$turned" 2 100 20 400)
    TOP=$( [[ -n "$turned" ]] && strip_colour "$turned" 100 2 500 20)
    check "randr: --transform 90: logical 400x533, the panel turns to the left edge, no modeset" \
      "$( (( RC == 0 )) && [[ -n "$PANEL" && "$PANEL" != "$BG" && "$LEFT" == "$PANEL" && "$TOP" != "$PANEL" && "$(shot_size "$turned")" == "800 600" ]] && after "$LOG_DIR/probe-outputs.log" "$MP" | grep -q "size=400x533 name=Virtual-2" && ! after "$CLOG" "$M" | grep -q "modeset output="; echo $?)" \
      "rc=$RC panel=$PANEL background=$BG; turned: left=$LEFT top=$TOP size=$( [[ -n "$turned" ]] && shot_size "$turned")"
    rr --output Virtual-2 --transform normal --scale 1
    # Off and on.
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
    rr --output Virtual-2 --off; RC=$?
    wait_after "$CLOG" "$M" "output $SLOT2 pipeline retired" 4
    sleep 0.3
    check "randr: --off: Virtual-2 disabled, its CRTC off, clients lose its wl_output" \
      "$( (( RC == 0 )) && after "$CLOG" "$M" | grep -q "output $SLOT2 disabled name=Virtual-2" && after "$CLOG" "$M" | grep -q "output $SLOT2 pipeline retired" && after "$LOG_DIR/probe-outputs.log" "$MP" | grep -q "wl_output removed name=Virtual-2"; echo $?)" \
      "rc=$RC $(after "$CLOG" "$M" | grep -oE "output [0-9]+ (disabled name=\S+|pipeline retired)" | tr '\n' ' ')"
    M=$(mark "$CLOG")
    rr --output Virtual-2 --on; RC=$?
    wait_after "$CLOG" "$M" "output configuration apply (succeeded|failed)" 4
    sleep 0.5
    A=$(after "$CLOG" "$M")
    check "randr: --on: Virtual-2 comes back with the mode it had (800x600), one modeset" \
      "$( (( RC == 0 )) && grep -qE "output [0-9]+ added name=Virtual-2 .*mode=800x600" <<<"$A" && [[ $(grep -c "modeset output=" <<<"$A") == 1 ]]; echo $?)" \
      "rc=$RC $(grep -oE "output [0-9]+ added name=Virtual-2 \S+ mode=\S+|modeset output=[0-9]+ frame=[0-9]+" <<<"$A" | tr '\n' ' ')"
    # An invalid configuration is refused by its TEST_ONLY; nothing on screen changes.
    M=$(mark "$CLOG")
    WAYLAND_DISPLAY="$SOCKET" "$RANDR" --dryrun --output Virtual-1 --custom-mode 16000x16000 >"$LOG_DIR/randr-invalid.log" 2>&1; RC=$?
    sleep 0.5
    A=$(after "$CLOG" "$M")
    check "randr: an invalid configuration (16000x16000) fails its test, nothing is modeset" \
      "$( (( RC != 0 )) && grep -q "output configuration test failed.*TEST_ONLY refused" <<<"$A" && ! grep -qE "modeset output=|switched to|output [0-9]+ mode " <<<"$A"; echo $?)" \
      "rc=$RC $(tr '\n' ' ' <"$LOG_DIR/randr-invalid.log")$(grep -oE "\(TEST_ONLY[^)]*\)" <<<"$A")"
    M=$(mark "$CLOG")
    WAYLAND_DISPLAY="$SOCKET" "$RANDR" --dryrun --output Virtual-1 --custom-mode 1700x800 >>"$LOG_DIR/wlr-randr.log" 2>&1; RC=$?
    sleep 0.5
    A=$(after "$CLOG" "$M")
    check "randr: a valid --dryrun (custom 1700x800) passes its TEST_ONLY, changes nothing" \
      "$( (( RC == 0 )) && grep -q "output configuration test succeeded.*TEST_ONLY passed" <<<"$A" && ! grep -qE "modeset output=|output [0-9]+ mode " <<<"$A"; echo $?)" "rc=$RC"
    # Back to its preferred mode and place; the file is written 2 s later.
    rr --output Virtual-2 --mode 1024x768 --pos "$W0",0
    sleep 2.6
    check "randr: outputs.toml holds the setup (a VM's monitors keep no mode or position)" \
      "$(grep -q '^\[\[setup\]\]' "$OUTCFG" 2>/dev/null && grep -q 'connector:Virtual-2' "$OUTCFG" && ! grep -qE '^ *(mode|position) =' "$OUTCFG"; echo $?)" \
      "$(grep -cE '^\[\[(output|setup)\]\]' "$OUTCFG" 2>/dev/null) tables in $OUTCFG"
  fi
fi

# ── swap ─────────────────────────────────────────────────────────────────────────────
if want swap; then
  if [[ -z "$RANDR" ]]; then
    record "swap" SKIP "wlr-randr is not installed"
  else
    ensure_two
    rr --output Virtual-2 --off
    sleep 1
    inject move 400 400; sleep 0.3
    start_client "$LOG_DIR/probe-swap.log" "$PROBE" presentation --maximize --frames 0 --title swap --exit-after 60
    SWAP_PID=$LAST_PID
    wait_after "$LOG_DIR/probe-swap.log" 0 "toplevel configure serial=[0-9]+ w=${W0} " 5
    sleep 0.8
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-swap.log")
    rr --output Virtual-1 --off --output Virtual-2 --on; RC=$?
    wait_after "$CLOG" "$M" "output configuration apply (succeeded|failed)" 5
    wait_after "$LOG_DIR/probe-swap.log" "$MP" "toplevel configure serial=[0-9]+ w=1024 h=736" 4
    sleep 0.5
    A=$(after "$CLOG" "$M"); P=$(after "$LOG_DIR/probe-swap.log" "$MP")
    check "swap: Virtual-1 off + Virtual-2 on in one configuration: no stand-in, Virtual-2 takes the window in (re-fitted)" \
      "$( (( RC == 0 )) && grep -q "output configuration apply succeeded" <<<"$A" && ! grep -q "HEADLESS-1" <<<"$A" && grep -qE "stranded windows taken in by output=[0-9]+ windows=[1-9]" <<<"$A" && grep -qE "toplevel configure serial=[0-9]+ w=1024 h=736" <<<"$P"; echo $?)" \
      "rc=$RC $(grep -oE "evacuate output=.*|stranded windows .*|HEADLESS-1.*|apply (succeeded|failed).*" <<<"$A" | cut -c1-120 | tr '\n' ' ') $(grep -E "toplevel configure" <<<"$P" | tail -3 | tr '\n' ' ')"
    M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-swap.log")
    rr --output Virtual-1 --on; RC=$?
    wait_after "$CLOG" "$M" "output configuration apply (succeeded|failed)" 5
    wait_after "$LOG_DIR/probe-swap.log" "$MP" "toplevel configure serial=[0-9]+ w=${W0} h=$(( H0 - 32 ))" 4
    sleep 0.5
    A=$(after "$CLOG" "$M"); P=$(after "$LOG_DIR/probe-swap.log" "$MP")
    check "swap: Virtual-1 on again: the window returns home, maximized to its usable area" \
      "$( (( RC == 0 )) && grep -qE "home output=[0-9]+ identifier=\"connector:Virtual-1\" windows=[1-9]" <<<"$A" && grep -qE "toplevel configure serial=[0-9]+ w=${W0} h=$(( H0 - 32 ))" <<<"$P"; echo $?)" \
      "rc=$RC $(grep -oE "home output=.*" <<<"$A" | tr '\n' ' ') $(grep -E "toplevel configure" <<<"$P" | tail -3 | tr '\n' ' ')"
    stop_client "$SWAP_PID"
    rr --output Virtual-2 --on --mode 1024x768 --pos "$W0",0
    sleep 1
  fi
fi

# ── mgmt-errors ──────────────────────────────────────────────────────────────────────
if want mgmt-errors; then
  ensure_two
  bad=""
  for kind in already_configured_head unconfigured_head already_used already_set already_set_custom \
              invalid_mode invalid_custom_mode invalid_transform invalid_scale invalid_adaptive_sync_state; do
    out=$(WAYLAND_DISPLAY="$SOCKET" "$PROBE" output-mgmt --error "$kind" 2>&1); rc=$?
    echo "$kind: $out" >>"$LOG_DIR/probe-mgmt-errors.log"
    (( rc == 0 )) || bad+="$kind "
  done
  STALE=$(WAYLAND_DISPLAY="$SOCKET" "$PROBE" output-mgmt --apply "Virtual-2:scale=2" --stale-serial 2>&1 | grep -oE "result \S+")
  check "mgmt-errors: every protocol error is raised (10 kinds), a stale serial is cancelled" \
    "$([[ -z "$bad" && "$STALE" == "result cancelled" ]] && kill -0 "$COMP_PID" 2>/dev/null; echo $?)" \
    "${bad:+wrong: $bad; }stale: ${STALE:-none}; $(grep -c 'protocol error' "$LOG_DIR/probe-mgmt-errors.log") errors seen"
fi

# ── kanshi ───────────────────────────────────────────────────────────────────────────
KANSHI=$(command -v kanshi || true)
printf 'profile two {\n  output Virtual-1 position 0,0 scale 1\n  output Virtual-2 position %s,0 scale 1.25\n}\nprofile one {\n  output Virtual-1 scale 1\n}\n' \
  "$W0" >"$LOG_DIR/kanshi.cfg"
if want kanshi; then
  if [[ -z "$KANSHI" || -z "$RANDR" ]]; then
    record "kanshi" SKIP "kanshi or wlr-randr is not installed"
  else
    ensure_two
    layout "${W0}x${H0}+0+0"
    sleep 1
    start_client "$LOG_DIR/kanshi.log" "$KANSHI" -c "$LOG_DIR/kanshi.cfg"
    KANSHI_PID=$LAST_PID
    sleep 1
    M=$(mark "$CLOG")
    layout "${W0}x${H0}+0+0" "1024x768+${W0}+0"
    wait_after "$LOG_DIR/kanshi.log" 0 "profile 'two' applied" 6
    wait_after "$CLOG" "$M" "apply succeeded.*Virtual-2 .* x1.25 " 3
    SC=$(WAYLAND_DISPLAY="$SOCKET" "$RANDR" 2>&1 | awk '/^Virtual-2/{f=1} f && /Scale/{print $2; exit}')
    check "kanshi: plugging Virtual-2 in applies its profile (scale 1.25)" \
      "$(grep -q "profile 'two' applied" "$LOG_DIR/kanshi.log" && after "$CLOG" "$M" | grep -q "apply succeeded.*Virtual-2 .* x1.25 " && [[ "$SC" == 1.25* ]]; echo $?)" \
      "$(grep -E "applied|cancelled|failed" "$LOG_DIR/kanshi.log" | tr '\n' ' ')scale=$SC"
    stop_client "$KANSHI_PID"
    sleep 2.6 # the debounced save
  fi
fi

# ── idle ─────────────────────────────────────────────────────────────────────────────
if want idle; then
  ensure_two
  [[ -n "${PLACE_PID:-}" ]] && stop_client "$PLACE_PID"
  inject move 300 300
  sleep 3
  C0=$(ui_ctxt); N0=$(mark "$CLOG")
  sleep 10
  C1=$(ui_ctxt); N1=$(mark "$CLOG")
  check "idle: two idle outputs, ≤ 4 wakeups/s of the UI thread" \
    "$( (( C1 - C0 <= 40 )); echo $?)" "$(( C1 - C0 )) context switches in 10 s; $(( N1 - N0 )) log lines"
  M=$(mark "$CLOG"); MP=$(mark "$LOG_DIR/probe-outputs.log")
  for _ in 1 2 3; do sudo -n "$VMW" "${TWO[@]}" >/dev/null; sleep 0.6; done
  sleep 0.5
  A=$(after "$CLOG" "$M")
  check "idle: the same layout again is probed and changes nothing" \
    "$(grep -q "hotplug: no change" <<<"$A" && ! grep -qE "output [0-9]+ (added|removed|mode )|output [0-9]+ name=" <<<"$A" && [[ -z "$(after "$LOG_DIR/probe-outputs.log" "$MP" | grep -E "wl_output|xdg_output")" ]]; echo $?)" \
    "$(grep -c "hotplug: no change" <<<"$A") no-change probes of $(grep -c "hotplug: probe" <<<"$A"); output events to the client: $(after "$LOG_DIR/probe-outputs.log" "$MP" | grep -cE "wl_output|xdg_output")"
fi

# ── end ──────────────────────────────────────────────────────────────────────────────
for p in "${CLIENT_PIDS[@]}"; do stop_pid "$p"; done
CLIENT_PIDS=()
stop_pid "$COMP_PID"; RC=$?; COMP_PID=""
rm -f "$SOCKET_PATH" "$LOCK_PATH"

# ── persist ──────────────────────────────────────────────────────────────────────────
CLOG2="$LOG_DIR/compositor2.log"
if want persist; then
  if [[ ! -s "$OUTCFG" ]]; then
    record "persist" SKIP "nothing was persisted (randr or kanshi did not run)"
  else
    layout "${W0}x${H0}+0+0" "1024x768+${W0}+0"
    sleep 0.5
    start_compositor "$CLOG2"
    wait_after "$CLOG2" 0 "frame clocks armed" 10
    for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
    sleep 1.5
    WANT_SCALE=$(awk '/identifier = "connector:Virtual-2"/{f=1} f && /^scale/{print $3; exit}' "$OUTCFG")
    A=$(cat "$CLOG2")
    check "persist: a new compositor comes up as it was left (scale ${WANT_SCALE:-?}), one combined modeset" \
      "$(grep -q "output configuration at start: 2 monitors, setup restored" <<<"$A" && grep -qE "output [0-9]+ name=Virtual-2 .*scale=${WANT_SCALE:-x} " <<<"$A" && [[ $(grep -c "modeset output=" <<<"$A") == 2 && $(grep -c "frame=1 outputs=2" <<<"$A") == 2 ]] && ! grep -q "switched to" <<<"$A"; echo $?)" \
      "$(grep -oE "output configuration at start.*|output [0-9]+ name=Virtual-2 \S+ \S+ scale=\S+|modeset output=[0-9]+ frame=[0-9]+ outputs=[0-9]+" <<<"$A" | tr '\n' ' ')"
    if [[ -n "$KANSHI" ]] && want kanshi; then
      M=$(mark "$CLOG2")
      start_client "$LOG_DIR/kanshi2.log" "$KANSHI" -c "$LOG_DIR/kanshi.cfg"
      K2=$LAST_PID
      wait_after "$LOG_DIR/kanshi2.log" 0 "profile 'two' applied" 5
      sleep 0.5
      check "persist: kanshi's apply at login finds nothing to change (no modeset)" \
        "$(after "$CLOG2" "$M" | grep -q "apply succeeded.*(nothing to change)" && ! after "$CLOG2" "$M" | grep -q "modeset output="; echo $?)" \
        "$(after "$CLOG2" "$M" | grep -oE "apply succeeded.*" | sed 's/.*(/(/' | tr '\n' ' ')"
      stop_client "$K2"
    fi
    stop_pid "$COMP_PID"; RC2=$?; COMP_PID=""
    (( RC2 != 0 )) && RC=$RC2
    rm -f "$SOCKET_PATH" "$LOCK_PATH"
  fi
fi
restore_layout
sleep 0.5
NOW=$("$VMW" --show | awk '$2 == "connected" { print $3 }' | tr '\n' ' ')
check "layout restored" "$([[ "${NOW% }" == "${ORIG[*]}" ]]; echo $?)" "now: ${NOW}; at start: ${ORIG[*]}"
python3 -I - "$CLOG" "$RC" "$CLOG2" >>"$RESULTS" <<'PY'
import os, sys
clog, rc, clog2 = sys.argv[1:4]
text = open(clog, errors="replace").read()
if os.path.exists(clog2):
    text += open(clog2, errors="replace").read()
bad = [l for l in text.splitlines()
       if (l.startswith("[ERROR]") or l.startswith("[WARN]"))
       and "no present in" not in l and "budget" not in l]
ok = not bad and rc == "0"
print(f"run: clean log, clean exit\t{'PASS' if ok else 'FAIL'}\t"
      f"errors+warnings={len(bad)}" + (f" (first: {bad[0][:140]})" if bad else "") + f"; exit={rc}")
PY
VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
CONSOLE=$(sudo -n python3 -I -c 'import fcntl,os,struct,sys
fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY); b=bytearray(4)
fcntl.ioctl(fd,0x4B3B,b); print("text" if struct.unpack("i",b)[0]==0 else "graphics")' "/dev/$VT" 2>/dev/null || echo unknown)
check "console" "$([[ "$CONSOLE" == text && "$VT" == "$START_VT" ]]; echo $?)" "$VT in $CONSOLE mode"

echo
echo "──────────────────────────────────────────────────────────────────────────────"
awk -F'\t' '{ printf "%-7s %-78s  %s\n", $2, $1, $3 }' "$RESULTS"
echo "──────────────────────────────────────────────────────────────────────────────"
echo "logs: $LOG_DIR"
if grep -qP '\tFAIL\t' "$RESULTS"; then echo "VERDICT: FAIL"; exit 1; fi
if (( INVALID )); then echo "VERDICT: INVALID (re-run)"; exit 3; fi
echo "VERDICT: PASS"
exit 0
