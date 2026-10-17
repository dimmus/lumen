#!/usr/bin/env bash
# Fractional output scale on the real console: wp_fractional_scale_v1, wp_viewporter and the
# scene's output projection, judged from screenshots of the composited frame and from the
# clients' own output. One compositor per scale (the scale is fixed at start, `--scale`);
# app-smoke.sh covers the same protocols at scale 1 inside its single run (group S).
#
#   ./scripts/scale-smoke.sh --join-seat-session            # from SSH
#   ./scripts/scale-smoke.sh --scales 1.25,1.5 --skip-gtk
#
# Options:
#   --join-seat-session   run the compositor inside the active seat0 logind session
#   --scales LIST         comma list of output scales (default 1.25,1.5,1.75,2)
#   --socket NAME         Wayland socket name (default lumen-scale-smoke)
#   --logs DIR            log directory (default <repo>/scale-smoke-logs)
#   --skip-gtk            skip gtk4-widget-factory
#
# Per scale S (log lines from LUMEN_LOG=debug; screenshots from SIGUSR1):
#   P1 lumen-probe scale --size 401x301: preferred_scale = round(120·S) arrives before the
#      first configure; it renders round(401·S)xround(301·S) with a viewport destination of
#      401x301; the window is drawn at round(x·S),round(y·S) exactly that size, and every
#      pixel of it in the screenshot is the probe's one-pixel black/white checker (no
#      resampling anywhere: pixel-exact)
#   P2 hit-testing at the window's right and bottom edges: the pointer at the last logical
#      column/row is on the window (the probe sees x in [w−1, w)), half a pixel further it
#      is not (leave)
#   P3 a viewport crop scaled up at S: the top-right quadrant of a 200x100 buffer shown at
#      300x150 logical: interior pure green, frame white, nothing of the other quadrants
#   P4 foot: no "fractional scaling" warning, a buffer of round(size·S) (1:1), and the
#      server-side title rasterized at S (decoration title line)
#   P5 gtk4-widget-factory: a buffer of round(dst·S) with the destination = its size (1:1)
#   Global: no ERROR/WARN line (except host present stalls), compositor exits 0, console in
#   text mode.
#
# Exit status: 0 PASS, 1 FAIL, 2 could not run.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
PROBE="${BUILD}/lumen-probe"
INJ="${ROOT}/scripts/inject-input.py"

SOCKET="lumen-scale-smoke"
LOG_DIR="${ROOT}/scale-smoke-logs"
JOIN_SEAT=0
SCALES="1.25,1.5,1.75,2"
SKIP_GTK=0
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --scales) SCALES="${2:?--scales needs a list}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --skip-gtk) SKIP_GTK=1 ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done

for bin in "$COMP" "$PROBE"; do
  [[ -x "$bin" ]] || { echo "FAIL: $bin is missing. Build first."; exit 2; }
done
command -v foot >/dev/null || { echo "FAIL: foot is not installed"; exit 2; }
if (( ! SKIP_GTK )) && ! command -v gtk4-widget-factory >/dev/null; then
  echo "NOTE: gtk4-widget-factory not installed; P5 skipped"; SKIP_GTK=1
fi
sudo -n true 2>/dev/null || { echo "FAIL: the injector needs passwordless sudo"; exit 2; }

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
rm -f "$LOG_DIR"/*.log "$LOG_DIR"/*.ppm "$LOG_DIR"/results.txt

COMP_PID=""; CLIENT_PIDS=()
stop_pid() {
  [[ -n "$1" ]] || return 0
  kill -TERM "$1" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$1" 2>/dev/null && kill -9 "$1" 2>/dev/null
  wait "$1" 2>/dev/null
}
cleanup() {
  local p
  for p in "${CLIENT_PIDS[@]}"; do stop_pid "$p"; done
  CLIENT_PIDS=()
  if [[ -n "$COMP_PID" ]]; then stop_pid "$COMP_PID"; COMP_PID=""; fi
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

wait_for() { # file pattern seconds
  local deadline=$(( $(date +%s%N) + ${3}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    grep -qE "$2" "$1" 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}
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
inject() { sudo -n python3 -I "$INJ" --output "${OUT_W}x${OUT_H}" "$@" >/dev/null; }
geom() { # log app_id — "x y w h" of the newest map of that app_id
  grep -oE "compositor.wm: map id=[0-9]+ app_id=$2 x=-?[0-9]+ y=-?[0-9]+ w=[0-9]+ h=[0-9]+" "$1" |
    tail -1 | sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+)/\1 \2 \3 \4/'
}
screenshot() { # comp-log — path of the screenshot written after this call, or empty
  local before
  before=$(grep -c "screenshot .* written to" "$1")
  kill -USR1 "$COMP_PID" 2>/dev/null
  local deadline=$(( $(date +%s%N) + 4000000000 ))
  while (( $(date +%s%N) < deadline )); do
    if (( $(grep -c "screenshot .* written to" "$1") > before )); then
      grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$1" | tail -1 | sed 's/.* to //'
      return 0
    fi
    sleep 0.05
  done
  return 1
}
# Physical pixel whose centre maps to logical coordinate L (the injector aims at pixel
# centres; the compositor maps the absolute device onto the logical rect).
phys() { # "L±d" mode_px logical_px
  python3 -I -c "import math,re,sys
m=re.fullmatch(r'(-?\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)?', sys.argv[1])
L=float(m.group(1))+(float(m.group(2)) if m.group(2) else 0.0)
print(int(math.floor(L*float(sys.argv[2])/float(sys.argv[3]))))" "$1" "$2" "$3"
}

RESULTS="$LOG_DIR/results.txt"
record() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$RESULTS"; }

echo "HANDS OFF: input is injected into the VM's devices for a minute per scale."
for SCALE in ${SCALES//,/ }; do
  TAG="s${SCALE}"
  CLOG="$LOG_DIR/compositor-$TAG.log"
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
  echo "── scale $SCALE"
  if (( JOIN_SEAT )); then
    bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$2" "$3" "$4" --scale "$5"' \
      _ "$SEAT_SCOPE" "$LOG_DIR" "$COMP" "$SOCKET" "$SCALE" >"$CLOG" 2>&1 &
  else
    LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$LOG_DIR" "$COMP" "$SOCKET" --scale "$SCALE" >"$CLOG" 2>&1 &
  fi
  COMP_PID=$!
  if ! wait_for "$CLOG" "output 0 mode=" 10; then
    record "$TAG start" FAIL "no output mode line"; cleanup; continue
  fi
  for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
  read -r OUT_W OUT_H LOG_W LOG_H S120 < <(grep -m1 -oE "output 0 mode=[0-9]+x[0-9]+ .*\(([0-9]+)/120\) logical=[0-9]+x[0-9]+" "$CLOG" |
    sed -E 's/.*mode=([0-9]+)x([0-9]+) .*\(([0-9]+)\/120\) logical=([0-9]+)x([0-9]+)/\1 \2 \4 \5 \3/')
  echo "   output ${OUT_W}x${OUT_H} logical ${LOG_W}x${LOG_H} scale ${S120}/120"
  inject move 5 $((OUT_H - 5))

  # P1 + P2 — the probe's checker, then its edges.
  PLOG="$LOG_DIR/probe-$TAG.log"
  start_client "$PLOG" "$PROBE" scale --size 401x301 --exit-after 60
  P_PID=$LAST_PID
  wait_for "$CLOG" "map id=[0-9]+ app_id=lumen-probe " 5
  sleep 0.6
  read -r X Y W H < <(geom "$CLOG" lumen-probe)
  SHOT=$(screenshot "$CLOG")
  python3 -I - "$PLOG" "$CLOG" "${SHOT:-}" "$SCALE" "$X" "$Y" "$W" "$H" "$S120" >>"$RESULTS" <<'PY'
import re, sys
plog, clog, shot, scale, X, Y, W, H, s120 = sys.argv[1:10]
X, Y, W, H, s120 = int(X), int(Y), int(W), int(H), int(s120)
s = s120 / 120.0
p = open(plog).read().splitlines()
c = open(clog).read().splitlines()
tag = f"s{scale}"
pref = [l for l in p if l.startswith("preferred-scale ")]
i_pref = next((i for i, l in enumerate(p) if l.startswith("preferred-scale ")), 10**9)
i_cfg = next((i for i, l in enumerate(p) if l.startswith("toplevel configure ")), -1)
buf = [l for l in p if l.startswith("buffer ")]
m = re.match(r"buffer (\d+)x(\d+) dst (\d+)x(\d+)", buf[-1]) if buf else None
bw, bh = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
want_w, want_h = int(W * s120 / 120 + 0.5), int(H * s120 / 120 + 0.5)
ok = bool(pref) and pref[0] == f"preferred-scale {s120}" and i_pref < i_cfg and \
     (bw, bh) == (want_w, want_h) and m and (int(m.group(3)), int(m.group(4))) == (W, H)
print(f"{tag} P1a probe told {s120}/120 first, renders round(size*S)\t{'PASS' if ok else 'FAIL'}\t"
      f"{pref[0] if pref else 'no preferred-scale'} (before configure={i_pref < i_cfg}); "
      f"{buf[-1] if buf else 'no buffer'}; want {want_w}x{want_h} dst {W}x{H}")
# The screenshot: the window drawn at round(X*s), round(Y*s), bw x bh, a pure checker.
import math
def rnd(v): return int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)
px0, py0 = rnd(X * s), rnd(Y * s)
detail, ok = "no screenshot", False
if shot:
    data = open(shot, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i+1].isspace(): i += 1
        j = i
        while not data[j:j+1].isspace(): j += 1
        parts.append(data[i:j]); i = j
    w, h = int(parts[1]), int(parts[2]); px = data[i+1:]
    def at(x, y):
        o = (y * w + x) * 3
        return px[o], px[o+1], px[o+2]
    bad = alt = 0
    first = None
    vis_w, vis_h = min(bw, w - px0), min(bh, h - py0)
    for y in range(py0, py0 + vis_h):
        for x in range(px0, px0 + vis_w):
            r, g, b = at(x, y)
            # The checker's phase: buffer pixel (0,0) is black.
            want = 255 if ((x - px0) ^ (y - py0)) & 1 else 0
            if not (r == g == b == want):
                bad += 1
                if first is None: first = (x - px0, y - py0, (r, g, b), want)
    # Nothing of the checker outside its rect (the desktop is black: a white pixel beside it
    # would be a column drawn too far).
    outside = [at(px0 + vis_w, py0 + k) for k in range(0, vis_h, 2) if px0 + vis_w < w] + \
              [at(px0 + k, py0 + vis_h) for k in range(0, vis_w, 2) if py0 + vis_h < h]
    spill = sum(1 for o in outside if o != (0, 0, 0))
    ok = vis_w > 0 and bad == 0 and spill == 0
    detail = (f"drawn at {px0},{py0} {vis_w}x{vis_h} of {bw}x{bh}: pixels off the checker={bad}"
              + (f" first={first}" if first else "") + f"; spill past the right/bottom edge={spill}")
print(f"{tag} P1b pixel-exact: the screenshot is the probe's checker, texel for pixel\t"
      f"{'PASS' if ok else 'FAIL'}\t{detail}")
PY
  # P2: last logical column / row inside, half a pixel past the edge outside.
  for spec in "right $((X + W)) $((Y + H / 2))" "bottom $((X + W / 2)) $((Y + H))"; do
    read -r EDGE LX LY <<<"$spec"
    if [[ $EDGE == right ]]; then
      IN_X=$(phys "$LX-0.5" "$OUT_W" "$LOG_W"); IN_Y=$(phys "$LY" "$OUT_H" "$LOG_H")
      OUT_X=$(phys "$LX+0.5" "$OUT_W" "$LOG_W"); OUT_Y=$IN_Y
    else
      IN_X=$(phys "$LX" "$OUT_W" "$LOG_W"); IN_Y=$(phys "$LY-0.5" "$OUT_H" "$LOG_H")
      OUT_X=$IN_X; OUT_Y=$(phys "$LY+0.5" "$OUT_H" "$LOG_H")
    fi
    N0=$(wc -l <"$PLOG")
    inject move $((OUT_W / 2)) $((OUT_H / 2)); sleep 0.25
    inject move "$IN_X" "$IN_Y"; sleep 0.3
    N1=$(wc -l <"$PLOG")
    inject move "$OUT_X" "$OUT_Y"; sleep 0.3
    python3 -I - "$PLOG" "$N0" "$N1" "$EDGE" "$W" "$H" "$SCALE" >>"$RESULTS" <<'PY'
import re, sys
plog, n0, n1, edge, W, H, scale = sys.argv[1:8]
p = open(plog).read().splitlines()
n0, n1, W, H = int(n0), int(n1), int(W), int(H)
pos = [l for l in p[n0:n1] if l.startswith("pointer position ")]
last = pos[-1] if pos else ""
m = re.search(r"x=(-?[\d.]+) y=(-?[\d.]+)", last)
v = (float(m.group(1)) if edge == "right" else float(m.group(2))) if m else None
lim = W if edge == "right" else H
inside = v is not None and lim - 1 <= v < lim
left = "pointer leave" in p[n1:]
ok = inside and left
print(f"s{scale} P2 hit-testing at the {edge} edge: last logical pixel on, the next off\t"
      f"{'PASS' if ok else 'FAIL'}\tinside: {last or 'no position'} (want {lim - 1} <= v < {lim}); "
      f"then leave={left}")
PY
  done
  stop_client "$P_PID"
  inject move 5 $((OUT_H - 5))

  # P3 — a viewport crop scaled up at this scale.
  VLOG="$LOG_DIR/probe-crop-$TAG.log"
  start_client "$VLOG" "$PROBE" scale --buffer 200x100 --crop 100,0,100,50 --dst 300x150 --exit-after 60
  V_PID=$LAST_PID
  wait_for "$CLOG" "map id=[0-9]+ app_id=lumen-probe x=-?[0-9]+ y=-?[0-9]+ w=300 h=150" 5
  sleep 0.6
  read -r X Y W H < <(geom "$CLOG" lumen-probe)
  SHOT=$(screenshot "$CLOG")
  python3 -I - "${SHOT:-}" "$SCALE" "$X" "$Y" "$W" "$H" "$S120" >>"$RESULTS" <<'PY'
import math, sys
shot, scale, X, Y, W, H, s120 = sys.argv[1:8]
X, Y, W, H = int(X), int(Y), int(W), int(H)
s = int(s120) / 120.0
def rnd(v): return int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)
x0, y0 = rnd(X * s), rnd(Y * s)
x1, y1 = x0 + rnd(W * s), y0 + rnd(H * s)
ok, detail = False, "no screenshot"
if shot:
    data = open(shot, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i+1].isspace(): i += 1
        j = i
        while not data[j:j+1].isspace(): j += 1
        parts.append(data[i:j]); i = j
    w, h = int(parts[1]), int(parts[2]); px = data[i+1:]
    def at(x, y):
        o = (y * w + x) * 3
        return tuple(px[o:o+3])
    m = math.ceil(2.5 * 1.5 * s) + 1  # the white frame texel, magnified 1.5x·S, filtered
    interior = [at(x, y) for y in range(y0 + m, y1 - m, 5) for x in range(x0 + m, x1 - m, 5)]
    green = sum(1 for c in interior if c == (0, 255, 0))
    frame = [at(x0, (y0 + y1) // 2), at(x1 - 1, (y0 + y1) // 2), at((x0 + x1) // 2, y0),
             at((x0 + x1) // 2, y1 - 1)]
    frame_ok = all(min(c) >= 200 for c in frame)
    ok = W == 300 and H == 150 and interior and green == len(interior) and frame_ok
    detail = f"window {W}x{H} at {x0},{y0}..{x1},{y1}; interior green {green}/{len(interior)}; frame {frame}"
print(f"s{scale} P3 viewport crop scaled up: only the green quadrant, framed\t"
      f"{'PASS' if ok else 'FAIL'}\t{detail}")
PY
  stop_client "$V_PID"

  # P4 — foot: 1:1 buffer, no warning, the title at the output scale.
  FLOG="$LOG_DIR/foot-$TAG.log"
  N0=$(wc -l <"$CLOG")
  LC_ALL=C.UTF-8 start_client "$FLOG" foot --config=/dev/null -o "main.font=monospace:size=10"
  F_PID=$LAST_PID
  wait_for "$CLOG" "map id=[0-9]+ app_id=foot " 8
  sleep 1
  python3 -I - "$CLOG" "$N0" "$FLOG" "$SCALE" "$S120" >>"$RESULTS" <<'PY'
import re, sys
clog, n0, flog, scale, s120 = sys.argv[1:6]
c = open(clog).read().splitlines()[int(n0):]
f = open(flog, errors="replace").read().splitlines()
geo = [m for l in c if (m := re.search(r"surface geometry surface=\d+ buffer=(\d+)x(\d+) scale=(\d+) transform=\d+ src=\S+ dst=(\S+) size=(\d+)x(\d+)", l))]
sized = [m for m in geo if int(m.group(1)) > 0]
s = int(s120)
one_to_one = bool(sized) and all(int(m.group(1)) == (int(m.group(5)) * s + 60) // 120 and
                                  int(m.group(2)) == (int(m.group(6)) * s + 60) // 120 for m in sized)
warned = [l for l in f if re.search(r"fractional scal", l, re.I)]
title = any("decoration title id=" in l for l in c)
ok = one_to_one and not warned and title
print(f"s{scale} P4 foot: round(size*S) buffers (1:1), no fractional-scaling warning, title rasterized\t"
      f"{'PASS' if ok else 'FAIL'}\t"
      f"{len(sized)} buffers, first {sized[0].group(0) if sized else '-'}; warnings={len(warned)}; title={title}")
PY
  stop_client "$F_PID"

  # P5 — GTK 4.
  if (( ! SKIP_GTK )); then
    GLOG="$LOG_DIR/gtk-$TAG.log"
    N0=$(wc -l <"$CLOG")
    GDK_BACKEND=wayland start_client "$GLOG" gtk4-widget-factory
    G_PID=$LAST_PID
    wait_for "$CLOG" "map id=[0-9]+ app_id=org.gtk.WidgetFactory4 " 12
    sleep 1.5
    python3 -I - "$CLOG" "$N0" "$SCALE" "$S120" >>"$RESULTS" <<'PY'
import re, sys
clog, n0, scale, s120 = sys.argv[1:5]
c = open(clog).read().splitlines()[int(n0):]
geo = [m for l in c if (m := re.search(r"surface geometry surface=\d+ buffer=(\d+)x(\d+) scale=(\d+) transform=\d+ src=\S+ dst=(\d+)x(\d+) size=(\d+)x(\d+)", l))]
s = int(s120)
big = [m for m in geo if int(m.group(4)) >= 200]  # the toplevel (popups/cursors are small)
ok = bool(big) and all(int(m.group(1)) == (int(m.group(4)) * s + 60) // 120 and
                       int(m.group(2)) == (int(m.group(5)) * s + 60) // 120 and
                       m.group(4) == m.group(6) and m.group(5) == m.group(7) for m in big)
print(f"s{scale} P5 gtk4-widget-factory: buffer round(dst*S), destination = size (1:1)\t"
      f"{'PASS' if ok else 'FAIL'}\t{len(big)} buffers, first {big[0].group(0) if big else '-'}")
PY
    stop_client "$G_PID"
  fi

  stop_pid "$COMP_PID"; RC=$?; COMP_PID=""
  python3 -I - "$CLOG" "$SCALE" "$RC" >>"$RESULTS" <<'PY'
import re, sys
clog, scale, rc = sys.argv[1:4]
bad = [l for l in open(clog, errors="replace").read().splitlines()
       if (l.startswith("[ERROR]") or l.startswith("[WARN]"))
       and "no present in" not in l and "budget" not in l]
ok = not bad and rc == "0"
print(f"s{scale} run: clean log, clean exit\t{'PASS' if ok else 'FAIL'}\t"
      f"errors+warnings={len(bad)}" + (f" (first: {bad[0][:120]})" if bad else "") + f"; exit={rc}")
PY
done
rm -f "$SOCKET_PATH" "$LOCK_PATH"

VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
CONSOLE=$(sudo -n python3 -I -c 'import fcntl,os,struct,sys
fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY); b=bytearray(4)
fcntl.ioctl(fd,0x4B3B,b); print("text" if struct.unpack("i",b)[0]==0 else "graphics")' "/dev/$VT" 2>/dev/null || echo unknown)
[[ "$CONSOLE" == text ]] && record "console" PASS "$VT in text mode" || record "console" FAIL "$VT is $CONSOLE"

echo
echo "──────────────────────────────────────────────────────────────────────────────"
awk -F'\t' '{ printf "%s  %-72s  %s\n", $2, $1, $3 }' "$RESULTS"
echo "──────────────────────────────────────────────────────────────────────────────"
echo "logs: $LOG_DIR"
if grep -qP '\tFAIL\t' "$RESULTS"; then echo "VERDICT: FAIL"; exit 1; fi
echo "VERDICT: PASS"
exit 0
