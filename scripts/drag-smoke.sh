#!/usr/bin/env bash
# Unattended interactive move/resize check on the real console.
#
# Runs the compositor at LUMEN_LOG=debug with lumen-square, drives drags and clicks by
# writing evdev events (scripts/inject-input.py, as root), and judges each scenario from
# BOTH sides: the compositor's window-manager log and lumen-square's stdout. It also
# measures input-to-present latency from the injector's CLOCK_MONOTONIC timestamps and the
# compositor's page-flip timestamps, so the latency claim is checked rather than asserted.
#
#   ./scripts/drag-smoke.sh                       # from the TTY you are logged into
#   ./scripts/drag-smoke.sh --join-seat-session   # from SSH: run the compositor inside
#                                                 # the active seat0 session (sudo)
#
# Options:
#   --join-seat-session   move the compositor into the active seat0 logind session's
#                         cgroup, so logind lets libinput open input devices. Not needed
#                         when the caller is already in that session.
#   --socket NAME         Wayland socket name (default lumen-drag-smoke)
#   --logs DIR            log directory (default <repo>/drag-smoke-logs)
#
# Needs: a build (LUMEN_BUILD_DIR, default build/release) with lumen-compositor and
# lumen-square, passwordless sudo for the injector, an absolute pointer at
# /dev/input/event2 and a keyboard at /dev/input/event1 (override with
# LUMEN_INJECT_POINTER / LUMEN_INJECT_KEYBOARD). The VMware/QEMU console mouse is absolute.
#
# The scenarios move the pointer and press buttons on the real devices, so do not touch
# the mouse or keyboard while it runs (about 30 s), and keep the host pointer outside the
# VM window: a VM console forwards it through the very device the injector writes to. The
# script says so before it starts, then watches every input node for the whole run
# (inject-input.py watch); any frame the injector did not write makes the run INVALID
# rather than a list of misleading FAILs.
#
# Exit status: 0 PASS (possibly with warnings), 1 FAIL, 2 could not run, 3 INVALID.
#
#   a  client move:  press inside the square, drag (+200,+120)   → origin moved, size kept
#   b  client resize, bottom-right corner, (+150,+100)          → resizing configures,
#                                                                 final one without it
#   c  client resize, top-left corner, (-150,-100)              → bottom-right corner fixed
#   d  min-size clamp: shrink far past 120x90                   → stops at 120x90
#   e  Super+left drag move                                      → moves; client sees no
#                                                                 press/release
#   f  click-to-focus/raise with two squares                     → raise + keyboard focus,
#                                                                 hit-test follows stacking
#   g  latency: injected motion → first page flip that shows it (cursor; moved window).
#      FAIL when a median exceeds 1.5 refresh periods + 4 ms: that is Lumen's own pacing.
#      WARN when a p95 exceeds 2 periods + 4 ms: tails here are often the host's. Each
#      sample is split from the compositor's present lines into: queued (waiting for the
#      previous frame's flip — only one is in flight), compositor (frame pacing + render,
#      up to the commit), GPU (commit → composite fence signalled) and display (→ flip),
#      so a WARN says whose time it was.
#   h  frame-perfect resize over b–d: one configure in flight, every committed geometry
#      matches its configure, no frame without one
#   i  client killed mid-grab (Super-drag)                       → grab ends, compositor lives
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SQUARE="${BUILD}/lumen-square"
INJ="${ROOT}/scripts/inject-input.py"
POINTER_DEV="${LUMEN_INJECT_POINTER:-/dev/input/event2}"
KEYBOARD_DEV="${LUMEN_INJECT_KEYBOARD:-/dev/input/event1}"

SOCKET="lumen-drag-smoke"
LOG_DIR="${ROOT}/drag-smoke-logs"
JOIN_SEAT=0
COUNTDOWN=3
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --countdown) COUNTDOWN="${2:?--countdown needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done

COMP_LOG="${LOG_DIR}/compositor.log"
SQ1_LOG="${LOG_DIR}/square1.log"
SQ2_LOG="${LOG_DIR}/square2.log"
INJ_LOG="${LOG_DIR}/inject.jsonl"
SCEN_LOG="${LOG_DIR}/scenarios.jsonl"
WATCH_LOG="${LOG_DIR}/watch.jsonl"
RUN_LOG="${LOG_DIR}/run.log"

mkdir -p "$LOG_DIR"
for f in "$COMP_LOG" "$SQ1_LOG" "$SQ2_LOG" "$INJ_LOG" "$SCEN_LOG" "$WATCH_LOG"; do : > "$f"; done

# Everything printed also goes to run.log: the compositor owns the console while this
# runs, so whatever scrolls by is otherwise lost.
exec > >(tee "$RUN_LOG") 2>&1
TEE_PID=$!
close_log() {
  if [[ -n "${TEE_PID:-}" ]]; then
    exec >&- 2>&-
    wait "$TEE_PID" 2>/dev/null
    TEE_PID=""
  fi
}
trap close_log EXIT

# ── Preconditions ────────────────────────────────────────────────────────────
for bin in "$COMP" "$SQUARE"; do
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: $bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done

# A binary older than its sources would test yesterday's code and look like a pass.
# lumen-square is standalone, so it is only compared with its own directory.
newest_of() {
  { git -C "$ROOT" ls-files -z -- "$@"
    git -C "$ROOT" ls-files -z --others --exclude-standard -- "$@"; } |
    (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1
}
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  for pair in "$COMP:modules executables/lumen-compositor" "$SQUARE:executables/lumen-square"; do
    bin=${pair%%:*}
    # shellcheck disable=SC2086 # the path list is meant to split
    src=$(newest_of ${pair#*:})
    if [[ -n "$src" && "$ROOT/$src" -nt "$bin" ]]; then
      echo "FAIL: $bin is older than $src. Rebuild:  cmake --build --preset release"
      exit 2
    fi
  done
fi

if ! sudo -n true 2>/dev/null; then
  echo "FAIL: the injector writes to evdev nodes as root and needs passwordless sudo"
  echo "      (or run this script as root)."
  exit 2
fi
for dev in "$POINTER_DEV" "$KEYBOARD_DEV"; do
  [[ -c "$dev" ]] || { echo "FAIL: $dev is not an input device node"; exit 2; }
done

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "WARN: another session is running (WAYLAND_DISPLAY='${WAYLAND_DISPLAY:-}'"
  echo "      DISPLAY='${DISPLAY:-}'); the compositor needs DRM master and the console."
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
      echo "FAIL: --join-seat-session: no active seat0 session owned by $(id -un)"
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      libinput will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
# libwayland flocks <socket>.lock while a compositor serves that name; removing a live
# compositor's socket would strand it, so only clear a lock nobody holds.
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another --socket"
  exit 2
fi
rm -f "$SOCKET_PATH" "$LOCK_PATH"

# ── Processes and cleanup ────────────────────────────────────────────────────
COMP_PID=""; SQ1_PID=""; SQ2_PID=""; BG_INJ_PID=""; WATCH_PID=""
COMP_RC="not-stopped"

stop_compositor() {
  [[ -n "$COMP_PID" ]] || return 0
  # SIGTERM, never SIGKILL first: teardown restores the console's text mode.
  kill -TERM "$COMP_PID" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$COMP_PID" 2>/dev/null; then
    echo "WARN: compositor ignored SIGTERM for 5 s; killing it (console may stay graphical)"
    kill -9 "$COMP_PID" 2>/dev/null
  fi
  wait "$COMP_PID" 2>/dev/null
  COMP_RC=$?
  COMP_PID=""
}

# The watcher runs under sudo; signalling sudo's PID relays the signal to it.
stop_watch() {
  [[ -n "$WATCH_PID" ]] || return 0
  kill -TERM "$WATCH_PID" 2>/dev/null
  wait "$WATCH_PID" 2>/dev/null
  WATCH_PID=""
}

cleanup() {
  [[ -n "$BG_INJ_PID" ]] && { kill -TERM "$BG_INJ_PID" 2>/dev/null; wait "$BG_INJ_PID" 2>/dev/null; }
  for pid in "$SQ1_PID" "$SQ2_PID"; do
    [[ -n "$pid" ]] && { kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }
  done
  SQ1_PID=""; SQ2_PID=""; BG_INJ_PID=""
  stop_compositor
  stop_watch
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
  close_log
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ── Helpers ──────────────────────────────────────────────────────────────────
lines() { wc -l < "$1" | tr -d ' '; }

wait_for() { # file pattern seconds
  local deadline=$(( $(date +%s%N) + ${3}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    grep -qE "$2" "$1" && return 0
    sleep 0.05
  done
  return 1
}

# Last known geometry of a toplevel: "x y w h".
geom() {
  grep -E "compositor.wm: (geometry|map) id=$1 " "$COMP_LOG" | tail -1 |
    sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}

# Waits until the window manager is quiet for the toplevel: the grab has ended and the last
# configure was acked, with no new configure/ack/geometry line for 300 ms.
settle() {
  local id=$1 prev="" cur last deadline=$(( $(date +%s%N) + 5000000000 ))
  while (( $(date +%s%N) < deadline )); do
    cur=$(grep -cE "compositor.wm: (configure|ack|geometry|grab end) id=$id " "$COMP_LOG")
    last=$(grep -oE "compositor.wm: configure id=$id serial=[0-9]+" "$COMP_LOG" | tail -1 |
           sed 's/.*serial=//')
    if [[ "$cur" == "$prev" ]] && { [[ -z "$last" ]] ||
         grep -qE "compositor.wm: ack id=$id serial=$last\$" "$COMP_LOG"; }; then
      return 0
    fi
    prev=$cur
    sleep 0.3
  done
  echo "WARN: toplevel $id did not settle within 5 s"
  return 1
}

SCEN=""
inject() {
  sudo -n python3 -I "$INJ" --pointer "$POINTER_DEV" --keyboard "$KEYBOARD_DEV" \
    --output "${OUT_W}x${OUT_H}" --tag "$SCEN" "$@" >>"$INJ_LOG"
}

declare -A BEGIN
begin() {
  SCEN=$1
  BEGIN[comp]=$(lines "$COMP_LOG"); BEGIN[sq1]=$(lines "$SQ1_LOG")
  BEGIN[sq2]=$(lines "$SQ2_LOG"); BEGIN[inj]=$(lines "$INJ_LOG")
}
# finish key=value ... — numeric parameters the verdict needs.
finish() {
  local extra="" kv
  for kv in "$@"; do extra+=",\"${kv%%=*}\":${kv#*=}"; done
  printf '{"name":"%s","comp":[%s,%s],"sq1":[%s,%s],"sq2":[%s,%s],"inj":[%s,%s]%s}\n' \
    "$SCEN" "${BEGIN[comp]}" "$(lines "$COMP_LOG")" "${BEGIN[sq1]}" "$(lines "$SQ1_LOG")" \
    "${BEGIN[sq2]}" "$(lines "$SQ2_LOG")" "${BEGIN[inj]}" "$(lines "$INJ_LOG")" "$extra" \
    >>"$SCEN_LOG"
}

# ── Start ────────────────────────────────────────────────────────────────────
echo "┌──────────────────────────────────────────────────────────────────────────┐"
echo "│ HANDS OFF for about 30 s: do not touch the mouse or keyboard, and keep   │"
echo "│ the host pointer OUTSIDE the VM window. Real input mixes with injected   │"
echo "│ input on the same device; if any is seen, the run is reported INVALID.   │"
echo "└──────────────────────────────────────────────────────────────────────────┘"
for (( n = COUNTDOWN; n > 0; n-- )); do printf '     starting in %d ...\r' "$n"; sleep 1; done
(( COUNTDOWN > 0 )) && printf '%-30s\r' ""

# Every frame on every input node from here on, injected or not.
sudo -n python3 -I "$INJ" watch >"$WATCH_LOG" 2>&1 &
WATCH_PID=$!
if ! wait_for "$WATCH_LOG" '"kind": "watch"' 3; then
  echo "FAIL: the input watcher did not start:"; cat "$WATCH_LOG"; exit 2
fi

echo "1/3  compositor on $SOCKET (LUMEN_LOG=debug) ..."
if (( ! IN_SEAT && JOIN_SEAT )); then
  # The shell moves itself into the seat session's cgroup, then becomes the compositor:
  # same PID, so $! is the compositor and SIGTERM reaches it.
  bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug "$2" "$3"' \
    _ "$SEAT_SCOPE" "$COMP" "$SOCKET" >"$COMP_LOG" 2>&1 &
else
  LUMEN_LOG=debug "$COMP" "$SOCKET" >"$COMP_LOG" 2>&1 &
fi
COMP_PID=$!

if ! wait_for "$COMP_LOG" "output 0 mode=" 10 || ! wait_for "$COMP_LOG" "libinput seat opened" 3; then
  echo "FAIL: the compositor did not report an output mode and an input seat."
  grep -E "ERROR|WARN|seat|output" "$COMP_LOG" | tail -15
  exit 2
fi
for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
[[ -S "$SOCKET_PATH" ]] || { echo "FAIL: no socket at $SOCKET_PATH"; exit 2; }
read -r OUT_W OUT_H REFRESH_MHZ < <(grep -m1 -oE "output 0 mode=[0-9]+x[0-9]+ refresh_mhz=[0-9]+" "$COMP_LOG" |
  sed -E 's/.*mode=([0-9]+)x([0-9]+) refresh_mhz=([0-9]+)/\1 \2 \3/')
echo "     output ${OUT_W}x${OUT_H} @ $((REFRESH_MHZ / 1000)) Hz; $(grep -m1 -oE '(hardware|software) cursor[^"]*' "$COMP_LOG")"

echo "2/3  lumen-square ..."
WAYLAND_DISPLAY="$SOCKET" "$SQUARE" >"$SQ1_LOG" 2>&1 &
SQ1_PID=$!
if ! wait_for "$COMP_LOG" "map id=1 " 5; then
  echo "FAIL: lumen-square did not map"; cat "$SQ1_LOG"; exit 2
fi
settle 1
# Park the pointer on the background, away from where windows go.
SCEN=setup; inject move 20 $((OUT_H - 20))
sleep 0.2

echo "3/3  scenarios ..."

# a — client-initiated move from the interior.
begin a
read -r X Y W H < <(geom 1)
CX=$((X + W / 2)); CY=$((Y + H / 2))
inject drag "$CX" "$CY" $((CX + 200)) $((CY + 120)) --button left --steps 60
settle 1
finish id=1 dx=200 dy=120

# b — client-initiated resize from the bottom-right corner (lumen-square: 12 px band).
begin b
read -r X Y W H < <(geom 1)
PX=$((X + W - 3)); PY=$((Y + H - 3))
inject drag "$PX" "$PY" $((PX + 150)) $((PY + 100)) --button left --steps 50
settle 1
finish id=1 dx=150 dy=100

# c — resize from the top-left corner: the bottom-right corner must not move.
begin c
read -r X Y W H < <(geom 1)
PX=$((X + 3)); PY=$((Y + 3))
inject drag "$PX" "$PY" $((PX - 150)) $((PY - 100)) --button left --steps 50
settle 1
finish id=1 dx=-150 dy=-100

# d — shrink far past lumen-square's 120x90 minimum.
begin d
read -r X Y W H < <(geom 1)
PX=$((X + W - 3)); PY=$((Y + H - 3))
TX=$((PX - W - 300)); TY=$((PY - H - 300))
(( TX < 4 )) && TX=4; (( TY < 4 )) && TY=4
inject drag "$PX" "$PY" "$TX" "$TY" --button left --steps 60
settle 1
finish id=1 dx=$((TX - PX)) dy=$((TY - PY))

# e — Super+left drag. Lands the window so the second square (centred) half-covers it.
begin e
read -r X Y W H < <(geom 1)
X2=$(( (OUT_W - 400) / 2 )); Y2=$(( (OUT_H - 300) / 2 ))
DX=$(( X2 - 60 - X )); DY=$(( Y2 - 54 - Y ))
CX=$((X + W / 2)); CY=$((Y + H / 2))
inject drag "$CX" "$CY" $((CX + DX)) $((CY + DY)) --button left --super --steps 60
settle 1
finish id=1 dx=$DX dy=$DY

# f — two windows: hit-testing follows stacking, a click raises and focuses.
WAYLAND_DISPLAY="$SOCKET" "$SQUARE" >"$SQ2_LOG" 2>&1 &
SQ2_PID=$!
if ! wait_for "$COMP_LOG" "map id=2 " 5; then
  echo "FAIL: the second lumen-square did not map"; cat "$SQ2_LOG"; exit 2
fi
settle 2
read -r X1 Y1 W1 H1 < <(geom 1)
read -r X2 Y2 W2 H2 < <(geom 2)
# The centre of the overlap, and a point of window 1 that window 2 does not cover.
OX=$(( ( (X1 > X2 ? X1 : X2) + ((X1 + W1) < (X2 + W2) ? X1 + W1 : X2 + W2) ) / 2 ))
OY=$(( ( (Y1 > Y2 ? Y1 : Y2) + ((Y1 + H1) < (Y2 + H2) ? Y1 + H1 : Y2 + H2) ) / 2 ))
VX=$((X1 + 20)); VY=$((Y1 + 20))
begin f1; inject move "$OX" "$OY"; sleep 0.3
finish ox=$OX oy=$OY vx=$VX vy=$VY
begin f2; inject move "$VX" "$VY"; sleep 0.2; inject button right click; sleep 0.4
finish ox=$OX oy=$OY vx=$VX vy=$VY
begin f3; inject move 20 $((OUT_H - 20)); sleep 0.2; inject move "$OX" "$OY"; sleep 0.3
finish ox=$OX oy=$OY vx=$VX vy=$VY

# g — hover sweep over the background for cursor latency (moves come from a and e).
begin g
inject drag 40 20 $((OUT_W - 40)) 20 --button none --steps 150
sleep 0.3
finish

# i — kill the client mid-grab.
begin i
read -r X Y W H < <(geom 2)
CX=$((X + W / 2)); CY=$((Y + H / 2))
inject drag "$CX" "$CY" $((CX + 200)) $((CY + 100)) --button left --super --steps 80 &
BG_INJ_PID=$!
sleep 0.4
kill -9 "$SQ2_PID" 2>/dev/null; wait "$SQ2_PID" 2>/dev/null; SQ2_PID=""
wait "$BG_INJ_PID"; BG_INJ_PID=""
sleep 0.4
ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
finish alive=$ALIVE

# ── Teardown ─────────────────────────────────────────────────────────────────
kill -TERM "$SQ1_PID" 2>/dev/null; wait "$SQ1_PID" 2>/dev/null; SQ1_RC=$?; SQ1_PID=""
stop_compositor
stop_watch
rm -f "$SOCKET_PATH" "$LOCK_PATH"

# KDGETMODE on the foreground VT: 0 = text, 1 = graphics.
VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
CONSOLE=$(sudo -n python3 -I -c 'import fcntl,os,struct,sys
fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY); b=bytearray(4)
fcntl.ioctl(fd,0x4B3B,b); print("text" if struct.unpack("i",b)[0]==0 else "graphics")' "/dev/$VT" 2>/dev/null || echo unknown)

# ── Verdict ──────────────────────────────────────────────────────────────────
python3 -I - "$LOG_DIR" "$REFRESH_MHZ" "$COMP_RC" "$SQ1_RC" "$CONSOLE" "$POINTER_DEV" "$KEYBOARD_DEV" <<'PY'
import bisect, json, os, re, sys

log_dir, refresh_mhz, comp_rc, sq1_rc, console = sys.argv[1:6]
refresh_mhz = int(refresh_mhz or 60000)
period_us = 1e9 / refresh_mhz

def read(name):
    with open(os.path.join(log_dir, name), encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()

comp = read("compositor.log")
sq = {"sq1": read("square1.log"), "sq2": read("square2.log")}
inj = [json.loads(l) for l in read("inject.jsonl") if l.startswith("{")]
scen = {s["name"]: s for s in (json.loads(l) for l in read("scenarios.jsonl"))}

R = lambda p: re.compile(r"compositor\.wm: " + p)
GEOM = R(r"(?:geometry|map) id=(\d+) (?:app_id=\S* )?x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)(?: cause=(\w+) serial=(\d+))?")
CFG = R(r"configure id=(\d+) serial=(\d+) w=(-?\d+) h=(-?\d+) states=(\S+) edges=(\d+) watchdog=(\d)")
ACK = R(r"ack id=(\d+) serial=(\d+)$")
GBEGIN = R(r"grab begin id=(\d+) mode=(\w+) edges=(\d+) source=(\w+)")
GEND = R(r"grab end id=(\d+) mode=(\w+) reason=(\w+)")
PRESENT = re.compile(r"compositor\.latency: present kind=(\w+) input_us=(\d+) present_us=(\d+) latency_us=(\d+)"
                     r"(?: submit_us=(\d+) fence_us=(\d+))?")
CCFG = re.compile(r"^configure serial=(\d+) size=(\d+)x(\d+) states=(\S+)")
CCOMMIT = re.compile(r"^commit size=(\d+)x(\d+)")

def rng(s, key="comp"):
    a, b = s[key]
    return range(a, b)

def matches(rx, lines, r):
    for i in r:
        m = rx.search(lines[i])
        if m:
            yield i, m

def geometry_before(tid, upto):
    """Geometry of toplevel `tid` as of compositor log line `upto` (exclusive)."""
    g = None
    for i in range(upto):
        m = GEOM.search(comp[i])
        if m and int(m.group(1)) == tid:
            g = tuple(int(m.group(k)) for k in (2, 3, 4, 5))
    return g

results = []  # (name, status, detail); status is PASS, WARN or FAIL

def record(name, ok, detail, warn=False):
    results.append((name, "FAIL" if not ok else "WARN" if warn else "PASS", detail))

def need(name):
    if name not in scen:
        record(name, False, "scenario did not run")
        return None
    return scen[name]

def grab_lines(s, tid):
    b = [m for _, m in matches(GBEGIN, comp, rng(s)) if int(m.group(1)) == tid]
    e = [m for _, m in matches(GEND, comp, rng(s)) if int(m.group(1)) == tid]
    return b, e

def client_lines(s, key, prefix):
    lines = sq[key]
    return [lines[i] for i in rng(s, key) if lines[i].startswith(prefix)]

# ── a: client move ──────────────────────────────────────────────────────────
s = need("a")
if s:
    g0, g1 = geometry_before(1, s["comp"][0]), geometry_before(1, s["comp"][1])
    b, e = grab_lines(s, 1)
    dx, dy = g1[0] - g0[0], g1[1] - g0[1]
    ok = (len(b) == 1 and b[0].group(2) == "move" and b[0].group(4) == "client"
          and e and e[-1].group(3) == "release"
          and abs(dx - s["dx"]) <= 2 and abs(dy - s["dy"]) <= 2 and g0[2:] == g1[2:]
          and any("-> move" in l for l in client_lines(s, "sq1", "press")))
    record("a client move", ok,
           f"origin {g0[:2]} -> {g1[:2]} (delta {dx:+d},{dy:+d}, want {s['dx']:+d},{s['dy']:+d}), "
           f"size {g0[2]}x{g0[3]} -> {g1[2]}x{g1[3]}, "
           f"grab={b[0].group(4) + '/' + b[0].group(2) if b else 'none'}")

def resize_case(name, label, want_edges, check):
    s = need(name)
    if not s:
        return
    g0, g1 = geometry_before(1, s["comp"][0]), geometry_before(1, s["comp"][1])
    b, e = grab_lines(s, 1)
    cfgs = [m for _, m in matches(CFG, comp, rng(s)) if int(m.group(1)) == 1]
    ccfgs = client_lines(s, "sq1", "configure")
    ok = (len(b) == 1 and b[0].group(2) == "resize" and b[0].group(4) == "client"
          and int(b[0].group(3)) == want_edges and e and e[-1].group(3) == "release")
    ok = ok and any("resizing" in m.group(5) for m in cfgs)
    ok = ok and bool(cfgs) and "resizing" not in cfgs[-1].group(5)
    ok = ok and any("resizing" in l for l in ccfgs) and bool(ccfgs) and "resizing" not in ccfgs[-1]
    commits = client_lines(s, "sq1", "commit")
    last_commit = tuple(int(v) for v in CCOMMIT.match(commits[-1]).groups()) if commits else None
    ok = ok and last_commit == (g1[2], g1[3])
    ok2, detail = check(s, g0, g1)
    record(f"{name} {label}", ok and ok2,
           f"{g0[2]}x{g0[3]}@{g0[0]},{g0[1]} -> {g1[2]}x{g1[3]}@{g1[0]},{g1[1]}; {detail}; "
           f"edges={b[0].group(3) if b else '-'} configures={len(cfgs)} "
           f"(client saw {sum('resizing' in l for l in ccfgs)} with resizing, last "
           f"{'without' if ccfgs and 'resizing' not in ccfgs[-1] else 'WITH'} it), "
           f"client last commit {last_commit}")

def check_b(s, g0, g1):
    dw, dh = g1[2] - g0[2], g1[3] - g0[3]
    ok = abs(dw - s["dx"]) <= 2 and abs(dh - s["dy"]) <= 2 and g1[:2] == g0[:2]
    return ok, f"size delta {dw:+d},{dh:+d} (want {s['dx']:+d},{s['dy']:+d}), origin kept={g1[:2] == g0[:2]}"

def check_c(s, g0, g1):
    right, bottom = g0[0] + g0[2], g0[1] + g0[3]
    worst = 0
    n = 0
    for _, m in matches(GEOM, comp, rng(s)):
        if int(m.group(1)) != 1:
            continue
        x, y, w, h = (int(m.group(k)) for k in (2, 3, 4, 5))
        worst = max(worst, abs(x + w - right), abs(y + h - bottom))
        n += 1
    dw, dh = g1[2] - g0[2], g1[3] - g0[3]
    ok = worst <= 2 and n > 0 and abs(dw + s["dx"]) <= 2 and abs(dh + s["dy"]) <= 2
    return ok, (f"bottom-right ({right},{bottom}) max drift {worst}px over {n} geometry updates, "
                f"size delta {dw:+d},{dh:+d}")

def check_d(s, g0, g1):
    low = [m for _, m in matches(CFG, comp, rng(s))
           if int(m.group(1)) == 1 and (0 < int(m.group(3)) < 120 or 0 < int(m.group(4)) < 90)]
    ok = (g1[2], g1[3]) == (120, 90) and not low
    return ok, f"final {g1[2]}x{g1[3]} (want 120x90), configures below the minimum: {len(low)}"

resize_case("b", "client resize bottom-right", 10, check_b)
resize_case("c", "client resize top-left", 5, check_c)
resize_case("d", "min-size clamp", 10, check_d)

# ── e: Super+left move, client sees no buttons ─────────────────────────────
s = need("e")
if s:
    g0, g1 = geometry_before(1, s["comp"][0]), geometry_before(1, s["comp"][1])
    b, e = grab_lines(s, 1)
    dx, dy = g1[0] - g0[0], g1[1] - g0[1]
    leaked = client_lines(s, "sq1", "press") + client_lines(s, "sq1", "release")
    ok = (len(b) == 1 and b[0].group(2) == "move" and b[0].group(4) == "keybinding"
          and e and e[-1].group(3) == "release" and not leaked
          and abs(dx - s["dx"]) <= 2 and abs(dy - s["dy"]) <= 2 and g0[2:] == g1[2:])
    record("e Super+left move", ok,
           f"delta {dx:+d},{dy:+d} (want {s['dx']:+d},{s['dy']:+d}), "
           f"grab={b[0].group(4) if b else 'none'}, button lines reaching the client: {len(leaked)}")

# ── f: hit-testing follows stacking; click raises and focuses ──────────────
def last_pointer_focus(s):
    f = None
    for i in rng(s):
        m = re.search(r"compositor\.wm: focus pointer (?:id=(\d+)|none)", comp[i])
        if m:
            f = int(m.group(1)) if m.group(1) else 0
    return f

f1, f2, f3 = (scen.get(n) for n in ("f1", "f2", "f3"))
if f1 and f2 and f3:
    before = last_pointer_focus(f1)
    raised = any(re.search(r"compositor\.wm: raise id=1 ", comp[i]) for i in rng(f2))
    kbd = any(re.search(r"compositor\.wm: focus keyboard id=1$", comp[i]) for i in rng(f2))
    after = last_pointer_focus(f3)
    enter1 = client_lines(f3, "sq1", "pointer enter")
    enter2 = client_lines(f3, "sq2", "pointer enter")
    # lumen-square has no keyboard, so focus is checked on the client side through the
    # xdg 'activated' state the click moves from square 2 to square 1.
    act1 = [l for l in client_lines(f2, "sq1", "configure") if "activated" in l]
    deact2 = [l for l in client_lines(f2, "sq2", "configure") if "activated" not in l]
    ok = (before == 2 and raised and kbd and after == 1 and enter1 and not enter2
          and act1 and deact2)
    record("f click-to-focus/raise", ok,
           f"overlap ({f1['ox']},{f1['oy']}) under pointer: id={before} before, id={after} after "
           f"the click; raise id=1: {raised}, keyboard focus id=1: {kbd}; "
           f"square1 activated: {bool(act1)}, square2 deactivated: {bool(deact2)}; "
           f"square1 enter: {len(enter1)}, square2 enter: {len(enter2)}")
else:
    record("f click-to-focus/raise", False, "scenario did not run")

# ── g: latency ─────────────────────────────────────────────────────────────
presents = []  # (input_us, present_us, kind, reported_latency, submit_us, fence_us)
for line in comp:
    m = PRESENT.search(line)
    if m:
        presents.append((int(m.group(2)), int(m.group(3)), m.group(1), int(m.group(4)),
                         int(m.group(5) or 0), int(m.group(6) or 0)))
presents.sort()
keys = [p[0] for p in presents]
flips = sorted(p[1] for p in presents)

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(q * (len(v) - 1))))] if v else 0

def latency(tags, phase, kinds):
    motions = [r for r in inj if r["kind"] == "motion" and r.get("tag") in tags and r.get("phase") == phase]
    # (latency, reported, queued, compositor, gpu, display) in µs; split parts None if unknown
    samples = []
    for r in motions:
        t = r["t_us"]
        i = bisect.bisect_left(keys, t)
        while i < len(presents) and presents[i][2] not in kinds:
            i += 1
        if i < len(presents) and presents[i][1] - t < 500_000:
            _, present, _, reported, submit, fence = presents[i]
            q_us = comp_us = gpu_us = disp_us = None
            if submit:
                # One flip in flight: a commit cannot go out before the previous flip lands,
                # so time until then is the previous frame's, not this one's.
                j = bisect.bisect_left(flips, submit) - 1
                prev = flips[j] if j >= 0 else 0
                q_us = max(0, prev - t)
                comp_us = submit - max(t, prev)
                gpu_us = max(0, fence - submit) if fence else None
                disp_us = present - max(submit, fence)
            samples.append((present - t, reported, q_us, comp_us, gpu_us, disp_us))
    return motions, samples

PARTS = ("queued", "compositor", "gpu", "display")

def split(samples):
    """Median queued/compositor/GPU/display split."""
    parts = [s for s in samples if s[2] is not None]
    med = lambda k: pct([s[k] for s in parts if s[k] is not None], .5) / 1000
    has_fence = any(s[4] is not None for s in parts)
    text = (f"median split queued={med(2):.1f} compositor={med(3):.1f} "
            + (f"gpu={med(4):.1f} " if has_fence else "gpu=n/a(no fence) ")
            + f"display={med(5):.1f}ms")
    return parts, text

median_budget = 1.5 * period_us + 4000
tail_budget = 2 * period_us + 4000
lat_ok, lat_warn = True, False
lat_detail = []
for label, tags, phase, kinds in (("cursor (hover)", ("g",), "hover", ("cursor", "content")),
                                  ("window move", ("a", "e"), "drag", ("content",))):
    motions, samples = latency(tags, phase, kinds)
    lat = [s[0] for s in samples]
    reported = [s[1] for s in samples]
    covered = len(lat) / len(motions) if motions else 0
    notes = []
    if len(lat) < 20 or covered < 0.8:
        lat_ok = False
        notes.append(f"FAIL: only {len(lat)} of {len(motions)} motions reached a flip")
    if pct(lat, .5) > median_budget:
        lat_ok = False
        notes.append(f"FAIL: median over {median_budget/1000:.1f}ms")
    parts, split_text = split(samples)
    if pct(lat, .95) > tail_budget:
        lat_warn = True
        slow = [s for s in parts if s[0] > tail_budget]
        whose = {k: 0 for k in PARTS}
        for s in slow:
            values = (s[2], s[3], s[4] or 0, s[5])
            whose[PARTS[max(range(4), key=lambda k: values[k])]] += 1
        unsplit = sum(1 for s in samples if s[0] > tail_budget) - len(slow)
        notes.append(f"WARN: p95 over {tail_budget/1000:.1f}ms; slow samples by largest part: "
                     + ", ".join(f"{k} {v}" for k, v in whose.items())
                     + (f", unsplit {unsplit}" if unsplit else ""))
    lat_detail.append(f"{label}: n={len(lat)}/{len(motions)} median={pct(lat, .5)/1000:.1f}ms "
                      f"p95={pct(lat, .95)/1000:.1f}ms max={max(lat or [0])/1000:.1f}ms; "
                      f"{split_text}; newest-input→flip median={pct(reported, .5)/1000:.1f}ms"
                      + "".join(f"; {n}" for n in notes))
cursor_kinds = {}
for p in presents:
    cursor_kinds[p[2]] = cursor_kinds.get(p[2], 0) + 1
record("g latency", lat_ok, " | ".join(lat_detail), warn=lat_warn)

# ── h: frame-perfect resize ────────────────────────────────────────────────
sent = {}
for m in (CFG.search(l) for l in comp):
    if m:
        sent[(int(m.group(1)), int(m.group(2)))] = (int(m.group(3)), int(m.group(4)), m.group(5))
h_ok, problems, n_cfg, n_commit, n_motion = True, [], 0, 0, 0
for name in ("b", "c", "d"):
    s = scen.get(name)
    if not s:
        h_ok = False
        problems.append(f"{name}: did not run")
        continue
    seq = []  # ordered events for toplevel 1
    for i in rng(s):
        line = comp[i]
        if (m := CFG.search(line)) and int(m.group(1)) == 1:
            seq.append(("cfg", int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(7))))
        elif (m := ACK.search(line)) and int(m.group(1)) == 1:
            seq.append(("ack", int(m.group(2))))
        elif (m := GEOM.search(line)) and int(m.group(1)) == 1 and m.group(6) == "commit":
            seq.append(("geom", int(m.group(7)), int(m.group(4)), int(m.group(5))))
    outstanding = None
    for ev in seq:
        if ev[0] == "cfg":
            n_cfg += 1
            if ev[4]:
                problems.append(f"{name}: configure {ev[1]} sent by the watchdog")
            if outstanding is not None:
                problems.append(f"{name}: configure {ev[1]} sent while {outstanding} was not acked")
            outstanding = ev[1]
        elif ev[0] == "ack":
            if outstanding is not None and ev[1] >= outstanding:
                outstanding = None
        else:
            n_commit += 1
            want = sent.get((1, ev[1]))
            if ev[1] == 0 or want is None:
                problems.append(f"{name}: geometry {ev[2]}x{ev[3]} committed without a configure")
            elif want[0] and want[1] and (want[0], want[1]) != (ev[2], ev[3]):
                problems.append(f"{name}: serial {ev[1]} configured {want[0]}x{want[1]} "
                                f"but presented {ev[2]}x{ev[3]}")
    if outstanding is not None:
        problems.append(f"{name}: last configure {outstanding} never acked")
    # The client side: each configure is answered by a commit of exactly that size.
    lines = [sq["sq1"][i] for i in rng(s, "sq1")]
    pending = None
    for line in lines:
        if (m := CCFG.match(line)):
            if pending is not None:
                problems.append(f"{name}: client got configure {m.group(1)} before committing {pending[0]}")
            pending = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        elif (m := CCOMMIT.match(line)) and pending is not None:
            w, h = int(m.group(1)), int(m.group(2))
            if pending[1] and pending[2] and (w, h) != (pending[1], pending[2]):
                problems.append(f"{name}: client committed {w}x{h} for a {pending[1]}x{pending[2]} configure")
            pending = None
    n_motion += sum(1 for r in inj if r["kind"] == "motion" and r.get("tag") == name and r.get("phase") == "drag")
# A frame drawn without its texture, or a client starved of frame callbacks, would also be
# a visible glitch during resize even with the configure sequence right.
for line in comp:
    if "unregistered texture" in line or "no present in 250ms" in line:
        problems.append(line.split(": ", 1)[-1][:120])
        break
record("h frame-perfect resize", h_ok and not problems and n_cfg > 0,
       (f"{n_cfg} configures for {n_motion} pointer motions, {n_commit} committed sizes, "
        f"all matched, never more than one in flight" if not problems else "; ".join(problems[:6])))

# ── i: client killed mid-grab ──────────────────────────────────────────────
s = need("i")
if s:
    b, e = grab_lines(s, 2)
    unmapped = any(re.search(r"compositor\.wm: unmap id=2 reason=destroyed", comp[i]) for i in rng(s))
    ok = b and e and e[-1].group(3) == "destroyed" and unmapped and s.get("alive") == 1
    record("i client killed mid-grab", ok,
           f"grab end reason={e[-1].group(3) if e else '-'}, unmapped={unmapped}, "
           f"compositor alive={s.get('alive') == 1}")

# ── Whole run ──────────────────────────────────────────────────────────────
# Warnings fail the run, except the 600-frame budget summaries: on a host whose GPU
# uploads slowly (SVGA3D) an interactive resize is expected to exceed them, and they are
# reported below rather than hidden.
BUDGET = ("over budget in recent frames",)
errors = [l for l in comp if l.startswith("[ERROR]") or
          (l.startswith("[WARN]") and not any(b in l for b in BUDGET))]
budget_warn = sum(1 for l in comp if l.startswith("[WARN]") and any(b in l for b in BUDGET))
record("run: no error/warning, clean exit, console text",
       not errors and comp_rc == "0" and console == "text",
       f"errors+warnings={len(errors)} budget-summaries={budget_warn} compositor exit={comp_rc} "
       f"lumen-square exit={sq1_rc} console={console}"
       + (f"; first: {errors[0]}" if errors else ""))

# ── Foreign input ──────────────────────────────────────────────────────────
# Every frame the kernel delivered, against what the injector wrote. Writes reach every
# reader of a node, so injected frames appear in the watch log too, with the same raw
# values; anything else came from a person or from the host. Absolute values are matched
# per axis (the kernel drops an axis whose value did not change), buttons and keys by
# count per (code, value), ignoring the kernel's own autorepeat (value 2).
pointer_dev, keyboard_dev = sys.argv[6], sys.argv[7]
watch = [json.loads(l) for l in read("watch.jsonl") if l.startswith("{")]
inj_ax = {r["ax"] for r in inj if r["kind"] == "motion" and "ax" in r}
inj_ay = {r["ay"] for r in inj if r["kind"] == "motion" and "ay" in r}
BTN = {"left": 0x110, "right": 0x111, "middle": 0x112}
inj_keys = {}
for r in inj:
    if r["kind"] == "button":
        k = (pointer_dev, BTN[r["button"]], 1 if r["pressed"] else 0)
    elif r["kind"] == "key":
        k = (keyboard_dev, r["code"], 1 if r["pressed"] else 0)
    else:
        continue
    inj_keys[k] = inj_keys.get(k, 0) + 1
seen_keys, foreign = {}, []  # foreign: (t_us, what)
for w in watch:
    if w.get("kind") == "dropped":
        foreign.append((w["t_us"], f"{w['dev']}: SYN_DROPPED (events lost; cannot vouch)"))
        continue
    if w.get("kind") != "seen":
        continue
    dev = w["dev"]
    for etype, code, value in w["ev"]:
        if dev not in (pointer_dev, keyboard_dev):
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))
        elif etype == 3 and dev == pointer_dev and code in (0, 1):
            if value not in (inj_ax if code == 0 else inj_ay):
                foreign.append((w["t_us"], f"{dev}: {'ABS_X' if code == 0 else 'ABS_Y'}={value}"))
        elif etype == 1 and value != 2:
            k = (dev, code, value)
            seen_keys[k] = seen_keys.get(k, 0) + 1
            if seen_keys[k] > inj_keys.get(k, 0):
                foreign.append((w["t_us"], f"{dev}: key/button {code}={value}"))
        elif etype == 4:
            pass  # EV_MSC: scan codes ride along with a real key, which is counted above
        elif etype != 1:
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))

def scenario_at(t):
    spans = {}
    for r in inj:
        tag = r.get("tag")
        if tag:
            a, b = spans.get(tag, (r["t_us"], r["t_us"]))
            spans[tag] = (min(a, r["t_us"]), max(b, r["t_us"]))
    best = None
    for tag, (a, b) in spans.items():
        if a - 500_000 <= t <= b + 1_000_000:
            best = tag
    return best or "between scenarios"

width = max(len(r[0]) for r in results)
print()
print("─" * 78)
for name, status, detail in results:
    print(f"{status}  {name:<{width}}  {detail}")
print("─" * 78)
print(f"presents logged: {cursor_kinds}; refresh period {period_us/1000:.2f} ms")
print(f"logs: {log_dir}")
print()
if foreign:
    where = {}
    for t, _ in foreign:
        s = scenario_at(t)
        where[s] = where.get(s, 0) + 1
    print(f"VERDICT: INVALID — {len(foreign)} input event(s) the injector did not send, during: "
          + ", ".join(f"{k} ({v})" for k, v in where.items()) + ".")
    print(f"         first: {foreign[0][1]}")
    print("         Real input mixed with the test's; the results above are not a judgement")
    print("         of the compositor. Re-run hands off, host pointer outside the VM window.")
    sys.exit(3)
if any(r[1] == "FAIL" for r in results):
    print("VERDICT: FAIL")
    sys.exit(1)
warned = [r[0] for r in results if r[1] == "WARN"]
print("VERDICT: PASS" + (f" (warnings: {', '.join(warned)})" if warned else ""))
sys.exit(0)
PY
exit $?
