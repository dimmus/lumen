#!/usr/bin/env bash
# vkmark as a Wayland client on Lumen: the commit path, stutter, and what the shell costs.
#
# Starts the compositor on the real console at LUMEN_LOG=debug (as drag-smoke.sh does), runs
# vkmark on it twice per mode, and judges each scene from both sides: vkmark's own FPS, and
# the compositor's `compositor.render: frame=` lines (rects, area, draws, render_us of every
# composite), `compositor.presentation: flip` lines (one per page flip that presented a frame
# a client asked wp_presentation feedback for, CLOCK_MONOTONIC) and CPU time (/proc). vkmark's
# output is timestamped as it is written — it prints a scene's name when the scene starts and
# its FPS when it ends — and each timestamp records how far the compositor's and the shell's
# logs had grown, so every line is attributed to the scene it belongs to (the first 0.5 s of a
# scene, its warm-up, is left out).
#
#   1. --present-mode immediate: vkmark draws and commits as fast as buffers come back, so its
#      FPS is the rate the commit → upload/import → wl_buffer.release path sustains. At most
#      one composite per refresh shows the newest commit; the others are replaced unseen.
#   2. --present-mode fifo: one frame per refresh (wp_fifo_v1 / commit-timing when the WSI uses
#      them). Stutter is the spread of the intervals between presented frames: stddev, p99, and
#      the frames that came later than 1.5 refresh periods after the previous one (each one a
#      refresh with no new frame on screen). Mesa's WSI asks for presentation feedback in this
#      mode, so the flip lines are vkmark's frames.
#
# Each mode starts its own compositor (a stopped shell leaves its panel's zone held, ADR 011):
#   bare   no shell: vkmark's window over the compositor's clear colour
#   panel  lumen-shell --no-background: the 32 px top panel and nothing else
#   shell  lumen-shell as a session runs it: the panel and the built-in background
# A static panel or background must add nothing to a frame that only vkmark's window changed:
# the same damage (rects, area), the same number of draws, render_us within noise, and no
# panel redraw while vkmark runs. The summary compares panel and shell with bare and says
# WARN when the damage or the draws differ.
#
# vkmark draws with whatever Vulkan device it finds. On a host without a hardware Vulkan
# driver that is lavapipe (llvmpipe, software), and the WSI hands the compositor shm buffers:
# the numbers then are the CPU's and the shm path's, and only comparable with runs of the same
# script on the same host (the summary names the device). The compositor composites with
# whatever backend `present_backend::automatic` picks (its log says which).
#
#   ./scripts/vkmark-bench.sh                       # from the TTY you are logged into
#   ./scripts/vkmark-bench.sh --join-seat-session   # from SSH (see drag-smoke.sh)
#
# Options:
#   --join-seat-session   run the compositor inside the active seat0 logind session's cgroup
#   --modes LIST          comma list of bare, panel, shell (default: all three)
#   --size WxH            vkmark's window size (default 800x600)
#   --imm-duration S      seconds per immediate-mode scene (default 5)
#   --fifo-duration S     seconds per fifo-mode scene (default 10)
#   --socket NAME         Wayland socket name (default lumen-vkmark-bench)
#   --logs DIR            log directory (default <repo>/vkmark-bench-logs)
#
# Scenes: immediate runs clear, cube, texture and desktop (cheap to heavy for the client);
# fifo runs clear and cube, which lavapipe draws well inside a refresh period, so a late frame
# is the present path's and not the client's.
#
# Logs: <logs>/<mode>/{compositor,shell,vkmark-immediate,vkmark-fifo}.log (the vkmark logs are
# JSON lines: each chunk of its output with the time it was written, the compositor's and
# shell's log sizes and CPU time then, and a "warm" mark at the end of each warm-up),
# <logs>/summary.txt, <logs>/summary.json.
#
# Needs: vkmark (with its wayland window-system plugin), a build (LUMEN_BUILD_DIR, default
# build/release) with lumen-compositor and lumen-shell, and passwordless sudo for
# --join-seat-session. Takes the display for about 3.5 minutes with the defaults; no input
# is injected, but moving the mouse adds cursor commits to the log, so leave it alone.
#
# Exit status: 0 done (possibly with warnings), 1 a run failed (vkmark exited non-zero, a
# scene without composites or, in fifo, without presented frames, the compositor died),
# 2 could not run.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SHELL_BIN="${BUILD}/lumen-shell"

SOCKET="lumen-vkmark-bench"
LOG_DIR="${ROOT}/vkmark-bench-logs"
JOIN_SEAT=0
MODES="bare,panel,shell"
SIZE="800x600"
IMM_DURATION=5
FIFO_DURATION=10
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --modes) MODES="${2:?--modes needs a list}"; shift ;;
    --size) SIZE="${2:?--size needs WxH}"; shift ;;
    --imm-duration) IMM_DURATION="${2:?--imm-duration needs seconds}"; shift ;;
    --fifo-duration) FIFO_DURATION="${2:?--fifo-duration needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done

IFS=, read -r -a MODE_LIST <<<"$MODES"
for m in "${MODE_LIST[@]}"; do
  case "$m" in bare|panel|shell) ;; *) echo "unknown mode: $m (bare, panel, shell)"; exit 2 ;; esac
done
[[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]] || { echo "--size wants WxH, got $SIZE"; exit 2; }
for d in "$IMM_DURATION" "$FIFO_DURATION"; do
  [[ "$d" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "a duration wants seconds, got $d"; exit 2; }
done

RUN_LOG="${LOG_DIR}/run.log"
mkdir -p "$LOG_DIR"
: > "$RUN_LOG"
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
if ! command -v vkmark >/dev/null; then
  echo "FAIL: vkmark is not installed"
  exit 2
fi
need_shell=0
for m in "${MODE_LIST[@]}"; do [[ "$m" != bare ]] && need_shell=1; done
for bin in "$COMP" $( (( need_shell )) && echo "$SHELL_BIN"); do
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: $bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done

# A binary older than its sources would measure yesterday's code.
newest_of() {
  { git -C "$ROOT" ls-files -z -- "$@"
    git -C "$ROOT" ls-files -z --others --exclude-standard -- "$@"; } |
    (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1
}
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  for pair in "$COMP:modules executables/lumen-compositor" \
              "$SHELL_BIN:modules/lx.shell executables/lumen-shell"; do
    bin=${pair%%:*}
    [[ -e "$bin" ]] || continue
    # shellcheck disable=SC2086 # the path list is meant to split
    src=$(newest_of ${pair#*:})
    if [[ -n "$src" && "$ROOT/$src" -nt "$bin" ]]; then
      echo "FAIL: $bin is older than $src. Rebuild:  cmake --build --preset release"
      exit 2
    fi
  done
fi

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "WARN: another session is running (WAYLAND_DISPLAY='${WAYLAND_DISPLAY:-}'"
  echo "      DISPLAY='${DISPLAY:-}'); the compositor needs DRM master and the console."
fi

# The compositor takes the display through logind, which only the active seat session may do.
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
    if ! sudo -n true 2>/dev/null; then
      echo "FAIL: --join-seat-session moves the compositor into the seat session's cgroup"
      echo "      and needs passwordless sudo."
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      the display will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another --socket"
  exit 2
fi

# ── Processes and cleanup ────────────────────────────────────────────────────
COMP_PID=""; SHELL_PID=""
FAILED=0

stop_pid() { # pid: SIGTERM, then SIGKILL after 5 s; returns its exit status
  local pid=$1
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$pid" 2>/dev/null; then
    echo "WARN: pid $pid ignored SIGTERM for 5 s; killing it"
    kill -9 "$pid" 2>/dev/null
  fi
  wait "$pid" 2>/dev/null
}

cleanup() {
  [[ -n "$SHELL_PID" ]] && { stop_pid "$SHELL_PID"; SHELL_PID=""; }
  # SIGTERM, never SIGKILL first: teardown restores the console's text mode.
  [[ -n "$COMP_PID" ]] && { stop_pid "$COMP_PID"; COMP_PID=""; }
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
  close_log
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

# Copies vkmark's output to a JSON-lines log as it is written: each chunk with its
# CLOCK_MONOTONIC time (the clock of the compositor's flip timestamps), the sizes of the
# compositor's and shell's logs then (neither log has timestamps: these place the scene's
# lines), and their CPU time summed over all threads. A "warm" record WARMUP_S after a scene
# starts marks where its measured part begins.
STAMPER='
import json, os, re, select, sys, time
WARMUP_S = 0.5
logs = dict(a.split("=", 1) for a in sys.argv[1:] if a.split("=", 1)[1])
pids = {k[:-4]: int(v) for k, v in logs.items() if k.endswith("_pid")}
files = {k[:-4]: v for k, v in logs.items() if k.endswith("_log")}
def cpu_ns(pid):
    total = 0
    try:
        for tid in os.listdir(f"/proc/{pid}/task"):
            try:
                with open(f"/proc/{pid}/task/{tid}/schedstat") as f:
                    total += int(f.read().split()[0])
            except OSError:
                pass
    except OSError:
        return None
    return total
def emit(**rec):
    rec["t"] = time.clock_gettime(time.CLOCK_MONOTONIC)
    for k, path in files.items():
        try:
            rec[k + "_log_bytes"] = os.stat(path).st_size
        except OSError:
            rec[k + "_log_bytes"] = None
    for k, pid in pids.items():
        rec[k + "_cpu_ns"] = cpu_ns(pid)
    print(json.dumps(rec), flush=True)
fd = sys.stdin.fileno()
text, warm_at, scenes = "", None, 0
while True:
    timeout = None if warm_at is None else max(0.0, warm_at - time.clock_gettime(time.CLOCK_MONOTONIC))
    if not select.select([fd], [], [], timeout)[0]:
        emit(mark="warm")
        warm_at = None
        continue
    b = os.read(fd, 65536)
    if not b:
        break
    chunk = b.decode("utf-8", "replace")
    emit(text=chunk)
    text += chunk
    n = len(re.findall(r"\[\w+\] [^\n]*?:", text))
    if n > scenes:
        scenes = n
        warm_at = time.clock_gettime(time.CLOCK_MONOTONIC) + WARMUP_S
'

# run_vkmark mode present duration scene...
run_vkmark() {
  local dir=$1 pm=$2 dur=$3; shift 3
  local args=() s log="$dir/vkmark-$pm.log"
  for s in "$@"; do args+=(-b "$s:duration=$dur"); done
  WAYLAND_DISPLAY="$SOCKET" vkmark --winsys wayland --present-mode "$pm" --size "$SIZE" "${args[@]}" 2>&1 |
    python3 -I -c "$STAMPER" "compositor_pid=$COMP_PID" "compositor_log=$dir/compositor.log" \
      "shell_pid=$SHELL_PID" "shell_log=$( [[ -n "$SHELL_PID" ]] && echo "$dir/shell.log")" >"$log"
  local rc=${PIPESTATUS[0]}
  printf '{"present":"%s","rc":%d}\n' "$pm" "$rc" >>"$dir/runs.jsonl"
  if (( rc != 0 )); then
    echo "     FAIL: vkmark --present-mode $pm exited $rc"
    FAILED=1
  fi
  python3 -I - "$log" <<'PY'
import json, re, sys
text = "".join(json.loads(l).get("text", "") for l in open(sys.argv[1]) if l.startswith("{"))
for m in re.finditer(r"\[(\w+)\][^\n]*?FPS: (\d+) FrameTime: ([\d.]+) ms", text):
    print(f"       {m.group(1):<9} {m.group(2):>5} FPS  {m.group(3):>7} ms")
PY
}

# ── Runs ─────────────────────────────────────────────────────────────────────
echo "vkmark on Lumen: modes ${MODES}, window ${SIZE}, immediate ${IMM_DURATION} s/scene," \
     "fifo ${FIFO_DURATION} s/scene; logs in ${LOG_DIR}"
step=0
for mode in "${MODE_LIST[@]}"; do
  step=$((step + 1))
  dir="$LOG_DIR/$mode"
  rm -rf "$dir"; mkdir -p "$dir"
  : > "$dir/compositor.log"; : > "$dir/runs.jsonl"
  rm -f "$SOCKET_PATH" "$LOCK_PATH"

  echo "$step/${#MODE_LIST[@]}  $mode: compositor on $SOCKET (LUMEN_LOG=debug) ..."
  if (( ! IN_SEAT && JOIN_SEAT )); then
    # Same PID as the compositor: the shell moves itself into the seat session's cgroup,
    # then execs it, so SIGTERM reaches the compositor.
    bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug "$2" "$3"' \
      _ "$SEAT_SCOPE" "$COMP" "$SOCKET" >"$dir/compositor.log" 2>&1 &
  else
    LUMEN_LOG=debug "$COMP" "$SOCKET" >"$dir/compositor.log" 2>&1 &
  fi
  COMP_PID=$!
  if ! wait_for "$dir/compositor.log" "output 0 mode=" 10; then
    echo "FAIL: the compositor did not report an output mode."
    grep -E "ERROR|WARN|output" "$dir/compositor.log" | tail -15
    exit 2
  fi
  for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
  [[ -S "$SOCKET_PATH" ]] || { echo "FAIL: no socket at $SOCKET_PATH"; exit 2; }
  if (( step == 1 )); then
    echo "     $(grep -m1 -oE 'output 0 mode=[0-9]+x[0-9]+ refresh_mhz=[0-9]+' "$dir/compositor.log");" \
         "$(grep -m1 -oE '(GL|Vulkan|CPU) composite path active[^"]*' "$dir/compositor.log" ||
            echo 'composite path not logged')"
  fi

  if [[ "$mode" != bare ]]; then
    shell_args=()
    [[ "$mode" == panel ]] && shell_args+=(--no-background)
    WAYLAND_DISPLAY="$SOCKET" LUMEN_LOG=debug "$SHELL_BIN" "${shell_args[@]}" >"$dir/shell.log" 2>&1 &
    SHELL_PID=$!
    if ! wait_for "$dir/compositor.log" "layer map surface=[0-9]+ layer=top namespace=lumen-panel " 10; then
      echo "FAIL: lumen-shell's panel did not map"; tail -15 "$dir/shell.log"; exit 2
    fi
    if [[ "$mode" == shell ]] &&
       ! wait_for "$dir/compositor.log" "layer map surface=[0-9]+ layer=background " 10; then
      echo "FAIL: lumen-shell's background did not map"; tail -15 "$dir/shell.log"; exit 2
    fi
    sleep 1 # the panel's first frames and the bridge snapshot settle
  fi

  echo "     vkmark --present-mode immediate (clear cube texture desktop) ..."
  run_vkmark "$dir" immediate "$IMM_DURATION" clear cube texture desktop
  sleep 0.5
  echo "     vkmark --present-mode fifo (clear cube) ..."
  run_vkmark "$dir" fifo "$FIFO_DURATION" clear cube

  if [[ -n "$SHELL_PID" ]]; then
    stop_pid "$SHELL_PID"; SHELL_PID=""
  fi
  if ! kill -0 "$COMP_PID" 2>/dev/null; then
    echo "     FAIL: the compositor exited during the run"
    FAILED=1
  fi
  stop_pid "$COMP_PID"; rc=$?; COMP_PID=""
  echo "$rc" > "$dir/compositor.rc"
  if (( rc != 0 )); then
    echo "     FAIL: the compositor exited $rc"
    FAILED=1
  fi
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
done

# ── Summary ──────────────────────────────────────────────────────────────────
python3 -I - "$LOG_DIR" "$MODES" <<'PY'
import bisect, json, os, re, statistics, sys

log_dir, modes = sys.argv[1], sys.argv[2].split(",")

RENDER = re.compile(r"compositor\.render: frame=(\d+) composite=(\d) rects=(\d+) area=(\d+) "
                    r"commit=(\S+) render_us=(\d+) output=(\d+)(?: draws=(\d+))?")
FLIP = re.compile(r"compositor\.presentation: flip frame=(\d+) t=(\d+)\.(\d+) seq=\d+ "
                  r"refresh_ns=(\d+) flags=0x[0-9a-f]+ presented=(\d+) discarded=(\d+) output=0")
MODE = re.compile(r"output 0 mode=(\d+)x(\d+) refresh_mhz=(\d+)")
SCENE_START = re.compile(r"\[(\w+)\] [^\n]*?:")
SCENE_END = re.compile(r"\[(\w+)\] [^\n]*?: FPS: (\d+) FrameTime: ([\d.]+) ms")
DEVICE = re.compile(r"Device Name:\s+(.+)")
PANEL_DRAW = re.compile(r"shell\.panel: draw ")

def pct(xs, p):
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]

class Log:
    """A log's lines, and the line a byte offset (a size the stamper saw) falls on."""
    def __init__(self, path):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            data = b""
        self.lines = data.decode("utf-8", "replace").splitlines()
        self.ends = [i for i, c in enumerate(data) if c == 10]
    def line_at(self, offset):
        return bisect.bisect_left(self.ends, offset or 0)

def read_jsonl(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [json.loads(l) for l in f if l.startswith("{")]
    except OSError:
        return []

device, refresh_mhz = None, None
rows, redraws, problems = [], [], []
for mode in modes:
    d = os.path.join(log_dir, mode)
    comp = Log(os.path.join(d, "compositor.log"))
    shell = Log(os.path.join(d, "shell.log"))
    for l in comp.lines:
        m = MODE.search(l)
        if m:
            refresh_mhz = int(m.group(3))
            break
    period_ms = 1e6 / refresh_mhz if refresh_mhz else 1000 / 60
    for run in read_jsonl(os.path.join(d, "runs.jsonl")):
        pm = run["present"]
        if run["rc"] != 0:
            problems.append(f"{mode} {pm}: vkmark exited {run['rc']}")
        recs = read_jsonl(os.path.join(d, f"vkmark-{pm}.log"))
        chunks = [r for r in recs if "text" in r]
        text, offs = "", []
        for c in chunks:  # where each chunk starts in the whole output
            offs.append(len(text))
            text += c["text"]
        chunk_at = lambda off: chunks[bisect.bisect_right(offs, off) - 1]
        if device is None:
            m = DEVICE.search(text)
            device = m.group(1).strip() if m else None
        scenes = []
        for m in SCENE_START.finditer(text):
            scenes.append({"name": m.group(1), "start": chunk_at(m.start())})
        for m in SCENE_END.finditer(text):
            for sc in scenes:
                if sc["name"] == m.group(1):
                    sc.update(end=chunk_at(m.end() - 1), fps=int(m.group(2)),
                              frame_ms=float(m.group(3)))
        warms = [r for r in recs if r.get("mark") == "warm"]
        for sc in scenes:
            name = sc["name"]
            if "end" not in sc:
                problems.append(f"{mode} {pm} {name}: vkmark did not finish the scene")
                continue
            # The measured part: from the warm-up mark to the scene's FPS line.
            w = [r for r in warms if sc["start"]["t"] < r["t"] < sc["end"]["t"]]
            if not w:
                problems.append(f"{mode} {pm} {name}: shorter than the warm-up")
                continue
            a, b = w[0], sc["end"]
            dur = b["t"] - a["t"]
            la, lb = comp.line_at(a.get("compositor_log_bytes")), comp.line_at(b.get("compositor_log_bytes"))
            seg = comp.lines[la:lb]
            flips, renders = [], []
            for l in seg:
                m = FLIP.search(l)
                if m:
                    flips.append((int(m.group(2)) + int(m.group(3)) / 1e9, int(m.group(5)), int(m.group(6))))
                    continue
                m = RENDER.search(l)
                if m and m.group(2) == "1" and m.group(7) == "0":
                    renders.append(m)
            r_us = [int(m.group(6)) for m in renders]
            areas = [int(m.group(4)) for m in renders]
            rects = [int(m.group(3)) for m in renders]
            draws = [int(m.group(8)) for m in renders if m.group(8) is not None]
            row = {"mode": mode, "present": pm, "scene": name, "fps": sc["fps"],
                   "frame_ms": sc["frame_ms"], "seconds": round(dur, 3),
                   "composites": len(renders),
                   "composites_per_s": round(len(renders) / dur, 1) if dur > 0 else None,
                   "presented": sum(f[1] for f in flips), "discarded": sum(f[2] for f in flips),
                   "render_us_p50": pct(r_us, 50), "render_us_p95": pct(r_us, 95),
                   "area_p50": pct(areas, 50), "area_max": max(areas) if areas else None,
                   "rects_p50": pct(rects, 50), "rects_max": max(rects) if rects else None,
                   "draws_p50": pct(draws, 50), "draws_max": max(draws) if draws else None}
            if a.get("compositor_cpu_ns") is not None and b.get("compositor_cpu_ns") is not None:
                ms = (b["compositor_cpu_ns"] - a["compositor_cpu_ns"]) / 1e6
                row["comp_cpu_ms_per_s"] = round(ms / dur, 1) if dur > 0 else None
                row["comp_cpu_us_per_frame"] = round(ms * 1000 / (sc["fps"] * dur), 1) if sc["fps"] and dur > 0 else None
            if len(flips) >= 2:
                iv = [(flips[k][0] - flips[k - 1][0]) * 1000 for k in range(1, len(flips))]
                span = flips[-1][0] - flips[0][0]
                row.update({
                    "flips": len(flips),
                    "flips_per_s": round(len(iv) / span, 2) if span > 0 else None,
                    "interval_mean_ms": round(statistics.fmean(iv), 3),
                    "interval_stddev_ms": round(statistics.pstdev(iv), 3),
                    "interval_p50_ms": round(pct(iv, 50), 3),
                    "interval_p99_ms": round(pct(iv, 99), 3),
                    "interval_max_ms": round(max(iv), 3),
                    "late": sum(1 for x in iv if x > 1.5 * period_ms),
                    "intervals": len(iv)})
            elif pm == "fifo":
                problems.append(f"{mode} {pm} {name}: {len(flips)} presented frames")
            if not renders:
                problems.append(f"{mode} {pm} {name}: no composite")
            if mode != "bare" and shell.lines:
                sa = shell.line_at(a.get("shell_log_bytes"))
                sb = shell.line_at(b.get("shell_log_bytes"))
                row["panel_redraws"] = sum(1 for l in shell.lines[sa:sb] if PANEL_DRAW.search(l))
            rows.append(row)

def f(v, w, p=None):
    if v is None:
        return f"{'-':>{w}}"
    return f"{v:>{w}.{p}f}" if p is not None else f"{v:>{w}}"

out = []
soft = device and re.search(r"llvmpipe|lavapipe|softpipe|swiftshader", device, re.I)
out.append(f"vkmark device: {device or 'unknown'}" +
           (" — SOFTWARE: the numbers compare runs on this host, nothing more" if soft else ""))
out.append(f"refresh {refresh_mhz / 1000 if refresh_mhz else '?'} Hz; each scene measured from 0.5 s "
           f"after it starts; late = a presented frame more than 1.5 refresh periods after the last")
out.append("")
hdr = (f"{'mode':<6} {'present':<9} {'scene':<8} {'FPS':>5} {'comp/s':>6} {'disc':>5} "
       f"{'iv mean':>7} {'sd':>5} {'p99':>6} {'max':>6} {'late':>4} "
       f"{'rnd p50':>7} {'p95':>6} {'area':>7} {'rects':>5} {'draws':>5} {'cpu ms/s':>8} {'us/frm':>7}")
out.append(hdr)
out.append("-" * len(hdr))
for r in rows:
    out.append(f"{r['mode']:<6} {r['present']:<9} {r['scene']:<8} {f(r['fps'], 5)} "
               f"{f(r.get('composites_per_s'), 6, 1)} {f(r.get('discarded'), 5)} "
               f"{f(r.get('interval_mean_ms'), 7, 2)} {f(r.get('interval_stddev_ms'), 5, 2)} "
               f"{f(r.get('interval_p99_ms'), 6, 2)} {f(r.get('interval_max_ms'), 6, 1)} "
               f"{f(r.get('late'), 4)} {f(r.get('render_us_p50'), 7)} {f(r.get('render_us_p95'), 6)} "
               f"{f(r.get('area_p50'), 7)} {f(r.get('rects_p50'), 5)} {f(r.get('draws_p50'), 5)} "
               f"{f(r.get('comp_cpu_ms_per_s'), 8, 1)} {f(r.get('comp_cpu_us_per_frame'), 7, 1)}")
out.append("")
out.append("FPS: vkmark's own. comp/s: composites per second. disc: frames wp_presentation reported "
           "discarded (never shown). iv: interval between presented frames, ms (fifo: the WSI asks for "
           "presentation feedback only there). rnd: compositor render_us; area, rects, draws: p50 per "
           "composite. cpu: the compositor's CPU time, all threads: per second, and per vkmark frame.")

# Panel / shell against bare: the same damage and draws per composite, no panel redraw.
warns = []
by = {(r["mode"], r["present"], r["scene"]): r for r in rows}
for (mode, pm, scene), r in by.items():
    if r.get("panel_redraws"):
        warns.append(f"{mode} {pm} {scene}: the panel redrew {r['panel_redraws']} times while vkmark ran")
    b = by.get(("bare", pm, scene))
    if mode == "bare" or not b:
        continue
    for k in ("area_p50", "rects_p50", "draws_p50"):
        if r.get(k) is not None and b.get(k) is not None and r[k] != b[k]:
            warns.append(f"{mode} {pm} {scene}: {k} {r[k]} vs bare {b[k]}")
    if r.get("render_us_p50") and b.get("render_us_p50"):
        out.append(f"{mode} vs bare, {pm} {scene}: render_us p50 {r['render_us_p50']} vs "
                   f"{b['render_us_p50']}, cpu us/frame {r.get('comp_cpu_us_per_frame')} vs "
                   f"{b.get('comp_cpu_us_per_frame')}, FPS {r['fps']} vs {b['fps']}")
out.append("")
for w in warns:
    out.append(f"WARN: {w}")
for p in problems:
    out.append(f"FAIL: {p}")
if "bare" in modes and len(modes) > 1 and not warns and not problems:
    out.append("panel/shell vs bare: the same damage and draws per composite, no panel redraw")
text = "\n".join(out)
print(text)
with open(os.path.join(log_dir, "summary.txt"), "w") as fo:
    fo.write(text + "\n")
with open(os.path.join(log_dir, "summary.json"), "w") as fo:
    json.dump({"device": device, "refresh_mhz": refresh_mhz, "rows": rows,
               "warnings": warns, "problems": problems}, fo, indent=1)
sys.exit(1 if problems else 0)
PY
summary_rc=$?
(( summary_rc != 0 )) && FAILED=1
echo "logs: $LOG_DIR (summary.txt, summary.json)"
exit $(( FAILED ? 1 : 0 ))
