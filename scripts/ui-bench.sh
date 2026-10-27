#!/usr/bin/env bash
# UI engine benchmark and regression gate (ADR-010 §8.5).
#
# Builds lumen-ui-bench in the release build, runs every scenario (S1–S4) on the backend
# (`none`: nothing renders, the stages through encode are measured; `cpu`: the reference
# renderer rasterises each frame's damage too, the `raster` stage; `gl`: the GLES 3.1
# renderer draws it on /dev/dri/renderD128, the `submit` and `gpu` stages — refused on a
# software GL_RENDERER; `vulkan`: the Vulkan renderer, the same two stages — on a CPU device
# such as lavapipe it runs, but the rows say "vulkan-software" and are stored only: never
# compared, never a baseline), stores
# the JSON under tests/bench/results/<hostname>-<driver>/<YYYYMMDD>-<sha>.json and compares
# it with tests/bench/baseline/<hostname>-<driver>.json:
#
#   FAIL           a stage's p95 more than 10 % above the baseline's — and at least
#                  ABS_FLOOR_US slower: a sub-microsecond stage jitters by more than 10 %
#                  on its own; any heap allocation or non-zero mallinfo2 delta in a steady
#                  frame of any scenario (G5); more batches than the scenario's cap (S2, S3,
#                  S4: 8); a gate the bench judged `fail` that the baseline passed (or has no
#                  verdict for); a stage whose sample count went from zero to non-zero or
#                  back (S1 producing frames, say); a scenario, stage or gate of the
#                  baseline missing from the run
#   WARN           a stage's p95 more than 5 % above the baseline's; a gate with too few
#                  samples to judge (`insufficient`); a gate that fails in the run and in
#                  the baseline (an open gate: never silent); a scenario the baseline lacks
#
# Exit status: 0 PASS (possibly with warnings), 1 FAIL, 2 could not run (a bad option, no
# python3, no configured build, no bench binary).
#
#   ./scripts/ui-bench.sh                     # build, run, store, compare
#   ./scripts/ui-bench.sh --update-baseline   # ... and rewrite the baseline (a reviewed change)
#   ./scripts/ui-bench.sh --compare FILE      # compare against FILE instead of the baseline
#
# Options:
#   --backend B         the backend to measure: none (default), cpu, gl or vulkan; each has its own
#                       baseline, tests/bench/baseline/<hostname>-<backend>.json
#   --frames N          measured frames for every scenario (default: S1 600, S2 36000, S3 1800,
#                       S4 1800 at the bench's 1 ms frame clock; S2 changes its clock every
#                       60th frame, so 36000 is the 600 produced frames §8.1 takes a p95 over)
#   --warmup N          warm-up frames (default 120)
#   --pin CPU           CPU the bench is pinned to (default: the last one — 1 on a 2-core host)
#   --no-build          skip the build step
#   --client            the client scenarios instead (S1 S5 S7 S9 S10, `lumen-ui-bench --client`):
#                       lx.app windows against a headless compositor the bench starts, on the
#                       tier --backend names (cpu by default; gl, vulkan); results under
#                       tests/bench/results/<hostname>-client-<tier>/, baseline
#                       tests/bench/baseline/<hostname>-client-<tier>.json
#   --short             with --client: the quick run (120 frames, 3 s idle, 3 start-ups, 10 resizes)
#   --e2e               S9 on the real display (ADR-010 §8.5): lumen-compositor on the KMS console
#                       at LUMEN_LOG=debug, `lumen-ui-bench --client --connect --scenario S9` on it,
#                       scripts/inject-input.py moving the pointer across the bench's button at
#                       125 Hz through the kernel (as root), the injector's CLOCK_MONOTONIC
#                       timestamps joined with the window's wp_presentation times and the
#                       compositor's `compositor.latency: present` lines — G6 (median ≤ 1 refresh
#                       period + 2 ms). Seizes the display and input like scripts/drag-smoke.sh:
#                       hands off. Exit 0 PASS, 1 FAIL, 2 could not run, 3 INVALID (foreign input)
#   --dry-run           with --e2e: print the plan without running anything, then run the join
#                       and verdict on a recorded sample (--sample DIR, default
#                       tests/bench/e2e-sample: a headless `lumen-ui-bench --client --scenario S9
#                       --record-e2e` run) and check it reproduces the bench's own numbers
#   --sample DIR        the recorded logs --dry-run parses
#   --join-seat-session with --e2e from SSH: run the compositor in the active seat0 session's cgroup
#   --socket NAME       with --e2e: the Wayland socket (default lumen-ui-e2e)
#   --logs DIR          with --e2e: the log directory (default <repo>/ui-e2e-logs)
#   LUMEN_BUILD_DIR     the build directory (default build/release)
#
# The bench runs with address-space randomisation off (`setarch -R`, when util-linux is
# there): S3's layout pass walks 10 000 heap nodes and its p50 moves 10 % with where the heap
# landed, so a baseline is only comparable at a fixed layout. The comparison runs in
# `python3 -I`; jq is not required.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
BENCH="${BUILD}/lumen-ui-bench"
ABS_FLOOR_US=1.0

backend=none
backend_given=0
frames=""
warmup=120
warmup_given=0
pin=""
build=1
update_baseline=0
compare=""
client=0
short=0
e2e=0
dry_run=0
sample="${ROOT}/tests/bench/e2e-sample"
join_seat=0
e2e_socket="lumen-ui-e2e"
e2e_logs="${ROOT}/ui-e2e-logs"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) backend="$2"; backend_given=1; shift 2 ;;
    --frames) frames="$2"; shift 2 ;;
    --warmup) warmup="$2"; warmup_given=1; shift 2 ;;
    --pin) pin="$2"; shift 2 ;;
    --no-build) build=0; shift ;;
    --update-baseline) update_baseline=1; shift ;;
    --compare) compare="$2"; shift 2 ;;
    --client) client=1; shift ;;
    --short) short=1; shift ;;
    --e2e) e2e=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --sample) sample="$2"; shift 2 ;;
    --join-seat-session) join_seat=1; shift ;;
    --socket) e2e_socket="$2"; shift 2 ;;
    --logs) e2e_logs="$2"; shift 2 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
done

if ! command -v python3 >/dev/null; then
  echo "FAIL: python3 is needed for the JSON comparison"
  exit 2
fi

# ── --e2e: S9 on the real display (and --dry-run on a recorded sample) ─────────
#
# The join, shared by the live run and the dry run. A hover transition is a motion whose
# `inside` differs from the motion before it; its sample is the first frame the window
# started at or after the injection that was presented, presented − injected (the rule
# lumen-ui-bench's headless S9 applies, tests/bench_ui_client.cpp `join_hover`; percentiles
# nearest-rank, as the bench). Every clock is CLOCK_MONOTONIC: the injector's write, the
# window's frame start and wp_presentation time, the compositor's flips.
e2e_verdict() { # DIR MODE(live|sample) [POINTER_DEV KEYBOARD_DEV]
  python3 -I - "$@" <<'PY'
import json, math, os, re, sys

log_dir, mode = sys.argv[1], sys.argv[2]
pointer_dev = sys.argv[3] if len(sys.argv) > 3 else ""
keyboard_dev = sys.argv[4] if len(sys.argv) > 4 else ""

def path(name):
    return os.path.join(log_dir, name)

def lines(name):
    try:
        with open(path(name), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []

def pct(values, p):
    v = sorted(values)
    if not v:
        return 0
    rank = min(len(v), max(1, math.ceil(p * len(v))))
    return v[rank - 1]

problems = []
inj = [json.loads(l) for l in lines("inject.jsonl") if l.startswith("{")]
motions = [r for r in inj if r.get("kind") == "motion" and r.get("phase") == "hover"]
try:
    with open(path("client.json")) as f:
        client = json.load(f)
except (OSError, ValueError) as e:
    print(f"FAIL: {path('client.json')}: {e}")
    sys.exit(2)
frames = client.get("frames", [])
comp = lines("compositor.log")

if not motions:
    problems.append("no hover motions in inject.jsonl")
if not frames:
    problems.append("no frames in client.json")

samples = []  # (t_us, start - t, commit - t, presented - t) in ns
transitions = 0
for prev, cur in zip(motions, motions[1:]):
    if bool(cur.get("inside")) == bool(prev.get("inside")):
        continue
    transitions += 1
    t_ns = int(cur["t_us"]) * 1000
    for fr in frames:
        if fr["start_ns"] < t_ns:
            continue
        if fr.get("presented") and t_ns <= fr["presented_ns"] < t_ns + 500_000_000:
            samples.append((int(cur["t_us"]), fr["start_ns"] - t_ns, fr["commit_ns"] - t_ns,
                            fr["presented_ns"] - t_ns))
        break

refresh_ns = next((fr["refresh_ns"] for fr in frames if fr.get("refresh_ns")), 0)
mode_line = next((l for l in comp if "output 0 mode=" in l), "")
m = re.search(r"refresh_mhz=(\d+)", mode_line)
if not refresh_ns and m:
    refresh_ns = int(1e12 / int(m.group(1)))
refresh_ns = refresh_ns or 16_666_667
period_us = refresh_ns / 1000.0

lat = [s[3] / 1000.0 for s in samples]
to_commit = [s[2] / 1000.0 for s in samples]
to_start = [s[1] / 1000.0 for s in samples]
p50, p95 = pct(lat, 0.5), pct(lat, 0.95)

# The compositor's side: its flips (`compositor.presentation: flip … t=S.NNNNNNNNN`), and on a
# KMS display its latency lines (`compositor.latency: present kind=… input_us= present_us=`).
FLIP = re.compile(r"compositor\.presentation: flip frame=\d+ t=(\d+)\.(\d{9}) .*presented=1")
PRESENT = re.compile(r"compositor\.latency: present kind=(\w+) input_us=(\d+) present_us=(\d+) "
                     r"latency_us=(\d+)(?: submit_us=(\d+) fence_us=(\d+))?")
flips = set()
presents = []
for l in comp:
    if (f := FLIP.search(l)):
        flips.add(int(f.group(1)) * 1_000_000_000 + int(f.group(2)))
    elif (p := PRESENT.search(l)):
        presents.append((p.group(1), int(p.group(2)), int(p.group(3)), int(p.group(4))))
present_us = {p[2] for p in presents}
matched = 0
for s in samples:
    presented = s[0] * 1000 + s[3]
    if any(abs(presented - f) <= 1000 for f in flips) or (presented // 1000) in present_us:
        matched += 1
geom = next((g for g in (re.search(r"compositor\.wm: map id=\d+ app_id=org\.lumen\.bench x=(-?\d+) "
                                   r"y=(-?\d+) w=(\d+) h=(\d+)", l) for l in comp) if g), None)

print(f"   motions {len(motions)}, transitions {transitions}, samples {len(samples)}, frames "
      f"{len(frames)}; refresh {period_us / 1000:.2f} ms")
print(f"   input → presented  p50 {p50 / 1000:.2f} ms ({p50 / period_us:.2f} periods)  p95 "
      f"{p95 / 1000:.2f} ms ({p95 / period_us:.2f} periods)  max {max(lat or [0]) / 1000:.2f} ms")
print(f"   input → frame start p50 {pct(to_start, .5) / 1000:.2f} ms, → commit p50 "
      f"{pct(to_commit, .5) / 1000:.2f} ms")
print(f"   presented times found among the compositor's flips/presents: {matched} of {len(samples)}"
      f" ({len(flips)} flip lines, {len(presents)} latency lines)")
if presents:
    content = [p[3] for p in presents if p[0] == "content"]
    print(f"   compositor newest-input → flip (content) p50 {pct(content, .5) / 1000:.2f} ms over "
          f"{len(content)} presents")
if geom:
    print(f"   window at {geom.group(1)},{geom.group(2)} {geom.group(3)}x{geom.group(4)}")

if transitions and len(samples) < max(20, int(0.8 * transitions)):
    problems.append(f"only {len(samples)} of {transitions} hover transitions reached a presented frame")
if samples and matched < len(samples):
    problems.append(f"{len(samples) - matched} presented times are not among the compositor's "
                    f"flips: the clocks disagree")

status = 0
if mode == "sample":
    # The recorded run's own numbers (tests/bench_ui_client.cpp): the join must reproduce them.
    try:
        with open(path("bench.json")) as f:
            bench = [r for r in json.load(f) if r.get("scenario") == "S9"]
    except (OSError, ValueError):
        bench = []
    if not bench:
        problems.append("no S9 record in bench.json to check the join against")
    else:
        st = bench[0]["stages"]["input_to_present"]
        same = (st["samples"] == len(samples) and abs(st["p50"] - p50) < 0.002 and
                abs(st["p95"] - p95) < 0.002)
        print(f"   lumen-ui-bench's own join: p50 {st['p50'] / 1000:.3f} ms, p95 "
              f"{st['p95'] / 1000:.3f} ms over {st['samples']} samples — "
              f"{'reproduced' if same else 'NOT reproduced'}")
        if not same:
            problems.append("the join does not reproduce lumen-ui-bench's input_to_present")
    if not presents:
        print("   (headless sample: a virtual output's flips carry no hw_clock, so the compositor "
              "writes no `compositor.latency: present` lines; on the KMS display it does)")
    # The live run's other input: a line as the KMS compositor writes it (drag-smoke.sh's
    # log of 2026-10-26, compositor_impl.cpp `note_present_latency`) must parse.
    kms = ("[DEBUG] compositor.latency: present kind=content input_us=1944703656 "
           "present_us=1944725278 latency_us=21622 submit_us=1944710655 fence_us=1944724943 output=0")
    if not PRESENT.search(kms):
        problems.append("the compositor.latency regex no longer parses the compositor's line")
    for p_ in problems:
        print(f"FAIL: {p_}")
    print("== dry run: the join " + ("FAILED" if problems else "parses the sample and agrees with the bench"))
    sys.exit(1 if problems else 0)

# Live: G6, and foreign input (as drag-smoke.sh: every frame the kernel delivered against what
# the injector wrote).
budget = period_us + 2000.0
tail = 2 * period_us + 4000.0
watch = [json.loads(l) for l in lines("watch.jsonl") if l.startswith("{")]
inj_ax = {r["ax"] for r in inj if r.get("kind") == "motion" and "ax" in r}
inj_ay = {r["ay"] for r in inj if r.get("kind") == "motion" and "ay" in r}
foreign = []
for w in watch:
    if w.get("kind") == "dropped":
        foreign.append(f"{w['dev']}: SYN_DROPPED")
    if w.get("kind") != "seen":
        continue
    for etype, code, value in w["ev"]:
        if w["dev"] != pointer_dev and etype != 4:
            foreign.append(f"{w['dev']}: type={etype} code={code} value={value}")
        elif etype == 3 and code in (0, 1) and value not in (inj_ax if code == 0 else inj_ay):
            foreign.append(f"{w['dev']}: {'ABS_X' if code == 0 else 'ABS_Y'}={value}")
        elif etype not in (3, 4):
            foreign.append(f"{w['dev']}: type={etype} code={code} value={value}")
if foreign:
    print(f"VERDICT: INVALID — {len(foreign)} input event(s) the injector did not send; first: {foreign[0]}")
    sys.exit(3)
if p50 > budget:
    problems.append(f"G6: median {p50 / 1000:.2f} ms over 1 period + 2 ms = {budget / 1000:.2f} ms")
warn = p95 > tail
for p_ in problems:
    print(f"FAIL: {p_}")
if problems:
    print("VERDICT: FAIL")
    sys.exit(1)
print("VERDICT: PASS (G6: median within 1 period + 2 ms)" +
      (f"; WARN p95 {p95 / 1000:.2f} ms over 2 periods + 4 ms" if warn else ""))
sys.exit(0)
PY
}

if [[ "$e2e" -eq 1 ]]; then
  COMP="${BUILD}/lumen-compositor"
  INJ="${ROOT}/scripts/inject-input.py"
  POINTER_DEV="${LUMEN_INJECT_POINTER:-/dev/input/event2}"
  KEYBOARD_DEV="${LUMEN_INJECT_KEYBOARD:-/dev/input/event1}"
  e2e_tier="${backend}"
  [[ "$backend_given" -eq 0 || "$backend" == "none" ]] && e2e_tier=auto
  HOVER_S=3
  BENCH_MS=$(( (HOVER_S + 5) * 1000 ))
  if [[ "$dry_run" -eq 1 ]]; then
    echo "== ui-bench --e2e --dry-run: the plan (nothing is started)"
    echo "   1. preconditions: ${COMP}, ${BENCH}, passwordless sudo, ${POINTER_DEV} (absolute"
    echo "      pointer), the active seat0 session$([[ "$join_seat" -eq 1 ]] && echo ' (joined: --join-seat-session)')"
    echo "   2. sudo python3 -I ${INJ} watch > ${e2e_logs}/watch.jsonl   (foreign input → INVALID)"
    echo "   3. LUMEN_LOG=debug ${COMP} ${e2e_socket} > ${e2e_logs}/compositor.log   (the KMS display)"
    echo "   4. WAYLAND_DISPLAY=${e2e_socket} ${BENCH} --client --connect --scenario S9 --tier ${e2e_tier} \\"
    echo "        --s9-ms ${BENCH_MS} --frames-out ${e2e_logs}/client.json > ${e2e_logs}/client.log"
    echo "   5. wait for 'lumen-ui-bench s9: ready … in=X,Y out=X,Y' and the compositor's"
    echo "      'compositor.wm: map … app_id=org.lumen.bench x= y=' line; global = map origin + local"
    echo "   6. sudo python3 -I ${INJ} --pointer ${POINTER_DEV} --layout ${e2e_logs}/compositor.log \\"
    echo "        --tag S9 hover XIN YIN XOUT YOUT --hz 125 --duration-s ${HOVER_S} --toggle-every 4 \\"
    echo "        > ${e2e_logs}/inject.jsonl"
    echo "   7. the bench exits after ${BENCH_MS} ms; the compositor gets SIGTERM; the join below"
    echo
    echo "== the join on the recorded sample ${sample#"$ROOT"/}"
    for f in inject.jsonl client.json compositor.log bench.json; do
      [[ -f "${sample}/${f}" ]] || { echo "FAIL: ${sample}/${f} is missing"; exit 2; }
    done
    e2e_verdict "$sample" sample
    exit $?
  fi

  # ── the live run (the real display; never from an automated test) ──
  if [[ "$build" -eq 1 && -f "${BUILD}/CMakeCache.txt" ]]; then
    cmake --build "$BUILD" --target lumen-ui-bench lumen-compositor
  fi
  for bin in "$COMP" "$BENCH"; do
    [[ -x "$bin" ]] || { echo "FAIL: $bin is missing (cmake --build --preset release)"; exit 2; }
  done
  if ! sudo -n true 2>/dev/null; then
    echo "FAIL: the injector writes to evdev nodes as root and needs passwordless sudo"
    exit 2
  fi
  [[ -c "$POINTER_DEV" ]] || { echo "FAIL: $POINTER_DEV is not an input device node"; exit 2; }
  SEAT_SCOPE=""
  sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
  if [[ -n "$sid" && "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" == "$(id -u)" ]]; then
    scope=$(loginctl show-session "$sid" -p Scope --value 2>/dev/null || true)
    [[ -n "$scope" ]] && SEAT_SCOPE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/${scope}"
  fi
  MY_CGROUP="/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)"
  IN_SEAT=0
  [[ -n "$SEAT_SCOPE" && "$MY_CGROUP" == "$SEAT_SCOPE" ]] && IN_SEAT=1
  if (( ! IN_SEAT && join_seat )) && [[ -z "$SEAT_SCOPE" || ! -e "$SEAT_SCOPE/cgroup.procs" ]]; then
    echo "FAIL: --join-seat-session: no active seat0 session owned by $(id -un)"
    exit 2
  fi
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  SOCKET_PATH="$XDG_RUNTIME_DIR/$e2e_socket"
  if [[ -e "${SOCKET_PATH}.lock" ]] && ! flock -n "${SOCKET_PATH}.lock" true; then
    echo "FAIL: socket '$e2e_socket' is held by a running compositor — pick another --socket"
    exit 2
  fi
  rm -f "$SOCKET_PATH" "${SOCKET_PATH}.lock"
  mkdir -p "$e2e_logs"
  for f in compositor.log client.log client.json inject.jsonl watch.jsonl; do : > "${e2e_logs}/$f"; done
  COMP_PID=""; BENCH_PID=""; WATCH_PID=""
  e2e_cleanup() {
    [[ -n "$BENCH_PID" ]] && { kill -TERM "$BENCH_PID" 2>/dev/null; wait "$BENCH_PID" 2>/dev/null; }
    if [[ -n "$COMP_PID" ]]; then
      kill -TERM "$COMP_PID" 2>/dev/null
      for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
      kill -0 "$COMP_PID" 2>/dev/null && kill -9 "$COMP_PID" 2>/dev/null
      wait "$COMP_PID" 2>/dev/null
    fi
    [[ -n "$WATCH_PID" ]] && { kill -TERM "$WATCH_PID" 2>/dev/null; wait "$WATCH_PID" 2>/dev/null; }
    BENCH_PID=""; COMP_PID=""; WATCH_PID=""
    rm -f "$SOCKET_PATH" "${SOCKET_PATH}.lock"
  }
  trap e2e_cleanup EXIT
  trap 'e2e_cleanup; exit 130' INT TERM
  wait_for() { # file pattern seconds
    local deadline=$(( $(date +%s%N) + ${3}000000000 ))
    while (( $(date +%s%N) < deadline )); do grep -qE "$2" "$1" && return 0; sleep 0.05; done
    return 1
  }
  echo "HANDS OFF for about 15 s: the pointer is driven through ${POINTER_DEV}."
  sleep 2
  sudo -n python3 -I "$INJ" watch >"${e2e_logs}/watch.jsonl" 2>&1 &
  WATCH_PID=$!
  wait_for "${e2e_logs}/watch.jsonl" '"kind": "watch"' 3 || { echo "FAIL: the input watcher did not start"; exit 2; }
  if (( ! IN_SEAT && join_seat )); then
    bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug "$2" "$3"' \
      _ "$SEAT_SCOPE" "$COMP" "$e2e_socket" >"${e2e_logs}/compositor.log" 2>&1 &
  else
    LUMEN_LOG=debug "$COMP" "$e2e_socket" >"${e2e_logs}/compositor.log" 2>&1 &
  fi
  COMP_PID=$!
  if ! wait_for "${e2e_logs}/compositor.log" "output 0 mode=" 10 ||
     ! wait_for "${e2e_logs}/compositor.log" "libinput seat opened" 3; then
    echo "FAIL: the compositor did not report an output mode and an input seat"; exit 2
  fi
  for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
  WAYLAND_DISPLAY="$e2e_socket" "$BENCH" --client --connect --scenario S9 --tier "$e2e_tier" \
    --s9-ms "$BENCH_MS" --frames-out "${e2e_logs}/client.json" >"${e2e_logs}/client.log" 2>&1 &
  BENCH_PID=$!
  if ! wait_for "${e2e_logs}/client.log" "lumen-ui-bench s9: ready" 10 ||
     ! wait_for "${e2e_logs}/compositor.log" "map id=[0-9]+ app_id=org.lumen.bench " 5; then
    echo "FAIL: the bench window did not map"; cat "${e2e_logs}/client.log"; exit 2
  fi
  read -r LX LY OX OY < <(sed -nE 's/.*in=(-?[0-9]+),(-?[0-9]+) out=(-?[0-9]+),(-?[0-9]+).*/\1 \2 \3 \4/p' \
    "${e2e_logs}/client.log" | head -1)
  read -r WX WY < <(grep -m1 -oE "map id=[0-9]+ app_id=org.lumen.bench x=-?[0-9]+ y=-?[0-9]+" \
    "${e2e_logs}/compositor.log" | sed -E 's/.*x=(-?[0-9]+) y=(-?[0-9]+)/\1 \2/')
  sudo -n python3 -I "$INJ" --pointer "$POINTER_DEV" --layout "${e2e_logs}/compositor.log" --tag S9 \
    hover $((WX + LX)) $((WY + LY)) $((WX + OX)) $((WY + OY)) --hz 125 --duration-s "$HOVER_S" \
    --toggle-every 4 >"${e2e_logs}/inject.jsonl"
  wait "$BENCH_PID"; BENCH_PID=""
  kill -TERM "$COMP_PID" 2>/dev/null; wait "$COMP_PID" 2>/dev/null; COMP_PID=""
  kill -TERM "$WATCH_PID" 2>/dev/null; wait "$WATCH_PID" 2>/dev/null; WATCH_PID=""
  echo "== S9 end to end (logs: ${e2e_logs#"$ROOT"/})"
  e2e_verdict "$e2e_logs" live "$POINTER_DEV" "$KEYBOARD_DEV"
  exit $?
fi

if [[ -z "$pin" ]]; then
  pin=$(( $(nproc) - 1 ))
fi

if [[ "$build" -eq 1 ]]; then
  if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
    if [[ "$BUILD" == "${ROOT}/build/release" ]]; then
      (cd "$ROOT" && cmake --preset release)
    else
      echo "FAIL: ${BUILD} is not configured"
      exit 2
    fi
  fi
  cmake --build "$BUILD" --target lumen-ui-bench
fi

if [[ ! -x "$BENCH" ]]; then
  echo "FAIL: ${BENCH} not built (cmake --build --preset release --target lumen-ui-bench)"
  exit 2
fi

build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "${BUILD}/CMakeCache.txt")"
if [[ "$build_type" != "Release" ]]; then
  echo "NOTE: ${BUILD} is CMAKE_BUILD_TYPE='${build_type}', not Release — numbers are not comparable"
fi
preset="$(basename "$BUILD")"

host="$(uname -n)"
sha="$(git -C "$ROOT" rev-parse --short HEAD)"
dirty=""
if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then
  dirty="-dirty"
fi
day="$(date +%Y%m%d)"
driver="$backend"

# ── --client: S1 S5 S7 S9 S10 as lx.app windows against a headless compositor ─
if [[ "$client" -eq 1 ]]; then
  tier="$backend"
  [[ "$tier" == "none" ]] && tier=cpu
  driver="client-${tier}"
  results_dir="${ROOT}/tests/bench/results/${host}-${driver}"
  baseline="${ROOT}/tests/bench/baseline/${host}-${driver}.json"
  mkdir -p "$results_dir" "${ROOT}/tests/bench/baseline"
  out="${results_dir}/${day}-${sha}${dirty}.json"
  n=2
  while [[ -e "$out" ]]; do out="${results_dir}/${day}-${sha}${dirty}-${n}.json"; n=$((n + 1)); done
  args=(--client --tier "$tier" --json "$out" --git "${sha}${dirty}" --preset "$preset")
  [[ "$short" -eq 1 ]] && args+=(--short)
  [[ -n "$frames" ]] && args+=(--frames "$frames")
  [[ "$warmup_given" -eq 1 ]] && args+=(--warmup "$warmup")
  [[ -n "$pin" ]] && args+=(--pin-client "$pin")
  echo "== lumen-ui-bench --client: tier=${tier} load=$(cut -d' ' -f1 /proc/loadavg) -> ${out#"$ROOT"/}"
  rc=0
  "$BENCH" "${args[@]}" || rc=$?
  if [[ "$rc" -eq 2 || ! -s "$out" ]]; then
    echo "FAIL: the client bench could not run (exit ${rc})"
    exit 2
  fi
  software="$(python3 -I -c 'import json, sys
print(int(any(r.get("driver", "").endswith("-software") for r in json.load(open(sys.argv[1])))))' "$out")"
  if [[ "$software" -eq 1 ]]; then
    echo
    echo "== ${tier}: a software device — correctness only: not compared, never a baseline"
    exit 0
  fi
  ref="$baseline"
  [[ -n "$compare" ]] && ref="$compare"
  status=0
  if [[ -f "$ref" ]]; then
    echo
    echo "== compare against ${ref#"$ROOT"/}"
    # Client rows run through a compositor and the host's scheduler on both sides. On this VM
    # about one frame in twenty is slowed 5-10x by the host (a fixed CPU job at the same cadence
    # is slowed as often: S7's control), so a client stage's p95 sits on that edge and flips
    # between runs (S5's frame p95: 187 or 540 us, same tree): a stage FAILS when its p50 rises
    # 30 % (and at least 50 us) and WARNS when its p95 rises 25 % (and at least 200 us). Gates
    # and G5 as the headless rows: a gate that passed and now fails, any operator new in a
    # steady client frame.
    python3 -I - "$out" "$ref" <<'PY' || status=$?
import json
import sys

new_path, ref_path = sys.argv[1], sys.argv[2]
P50_FAIL_PCT, P50_FLOOR_US = 30.0, 50.0
P95_WARN_PCT, P95_FLOOR_US = 25.0, 200.0
new = [r for r in json.load(open(new_path)) if "skipped" not in r]
ref = {r["scenario"]: r for r in json.load(open(ref_path)) if "skipped" not in r}
fails, warns = [], []
ids = {r["scenario"] for r in new}
for sid in ref:
    if sid not in ids:
        fails.append(f"{sid}: in the baseline, missing from this run")
for r in new:
    sid = r["scenario"]
    mem = r.get("memory", {})
    if mem.get("allocations", 0) > 0:
        fails.append(f"{sid}: {mem['allocations']} operator new on the UI thread in steady frames (G5)")
    base = ref.get(sid)
    gates = r.get("gates", {})
    base_gates = base.get("gates", {}) if base else {}
    for gid, verdict in gates.items():
        was = base_gates.get(gid)
        if verdict == "fail" and was != "fail":
            fails.append(f"{sid}: gate {gid} fail (baseline: {was or 'none'})")
        elif verdict == "fail":
            warns.append(f"{sid}: gate {gid} still fails (open in the baseline too)")
        elif verdict in ("jitter", "insufficient"):
            warns.append(f"{sid}: gate {gid} {verdict}")
    if base is None:
        warns.append(f"{sid}: not in the baseline")
        continue
    for name, st in r.get("stages", {}).items():
        bs = base.get("stages", {}).get(name)
        if not bs or not st["samples"] or not bs["samples"] or bs["p95"] <= 0:
            continue
        delta = st["p95"] - bs["p95"]
        pct = 100.0 * delta / bs["p95"]
        d50 = st["p50"] - bs["p50"]
        pct50 = 100.0 * d50 / bs["p50"] if bs["p50"] > 0 else 0.0
        line = (f"{sid:3} {name:22} p50 {bs['p50']:10.1f} -> {st['p50']:10.1f} us {pct50:+6.1f} %, "
                f"p95 {bs['p95']:10.1f} -> {st['p95']:10.1f} us {pct:+6.1f} %")
        if d50 >= P50_FLOOR_US and pct50 > P50_FAIL_PCT:
            fails.append(line)
            print("  FAIL " + line)
        elif delta >= P95_FLOOR_US and pct > P95_WARN_PCT:
            warns.append(line)
            print("  WARN " + line)
        else:
            print("  ok   " + line)
for w in warns:
    print("WARN: " + w)
for f_ in fails:
    print("FAIL: " + f_)
print(f"== {len(fails)} failure(s), {len(warns)} warning(s)")
sys.exit(1 if fails else 0)
PY
  else
    echo
    echo "== no baseline at ${ref#"$ROOT"/}: nothing to compare (run with --update-baseline to create it)"
  fi
  if [[ "$update_baseline" -eq 1 ]]; then
    cp "$out" "$baseline"
    echo "== baseline written: ${baseline#"$ROOT"/}"
  fi
  [[ "$rc" -eq 1 && "$status" -eq 0 ]] && status=1
  exit "$status"
fi

results_dir="${ROOT}/tests/bench/results/${host}-${driver}"
baseline="${ROOT}/tests/bench/baseline/${host}-${driver}.json"
mkdir -p "$results_dir" "${ROOT}/tests/bench/baseline"
out="${results_dir}/${day}-${sha}${dirty}.json"
# A second run of the same tree on the same day keeps the first one.
n=2
while [[ -e "$out" ]]; do
  out="${results_dir}/${day}-${sha}${dirty}-${n}.json"
  n=$((n + 1))
done

runner=()
if command -v setarch >/dev/null; then
  runner=(setarch "$(uname -m)" -R)
else
  echo "NOTE: setarch not found — heap layout differs per run, expect a few percent of spread"
fi

frames_for() {
  case "$1" in
    S1) echo 600 ;;
    S2) echo 36000 ;;
    S3) echo 1800 ;;
    S4) echo 1800 ;;
  esac
}

echo "== lumen-ui-bench: backend=${backend} warmup=${warmup} pin=${pin} -> ${out#"$ROOT"/}"
parts=()
for scenario in S1 S2 S3 S4; do
  n="${frames:-$(frames_for "$scenario")}"
  part="${out%.json}-${scenario}.json"
  "${runner[@]}" "$BENCH" --scenario "$scenario" --backend "$backend" --frames "$n" \
    --warmup "$warmup" --pin "$pin" --json "$part" --git "${sha}${dirty}" --preset "$preset"
  parts+=("$part")
done
# One file per run, the scenarios in order.
python3 -I - "$out" "${parts[@]}" <<'PY'
import json
import sys

merged = []
for path in sys.argv[2:]:
    with open(path) as f:
        merged.extend(json.load(f))
with open(sys.argv[1], "w") as f:
    json.dump(merged, f, indent=2)
    f.write("\n")
PY
rm -f "${parts[@]}"

# A CPU Vulkan device measures the CPU through a graphics API, not a GPU: its rows are kept
# for the record and nothing more.
software="$(python3 -I -c 'import json, sys
print(int(any(r.get("driver") == "vulkan-software" for r in json.load(open(sys.argv[1])))))' "$out")"
if [[ "$software" -eq 1 ]]; then
  echo
  echo "== ${backend}: a software device (driver vulkan-software) — correctness only: not compared, never a baseline"
  exit 0
fi

ref="$baseline"
if [[ -n "$compare" ]]; then
  ref="$compare"
fi

status=0
if [[ -f "$ref" ]]; then
  echo
  echo "== compare against ${ref#"$ROOT"/}"
  python3 -I - "$out" "$ref" "$ABS_FLOOR_US" <<'PY' || status=$?
import json
import sys

new_path, ref_path, floor = sys.argv[1], sys.argv[2], float(sys.argv[3])
FAIL_PCT, WARN_PCT = 10.0, 5.0
BATCH_CAPS = {"S2": 8, "S3": 8, "S4": 8}

with open(new_path) as f:
    new = json.load(f)
with open(ref_path) as f:
    ref = json.load(f)
by_id = {r["scenario"]: r for r in ref}
new_ids = {r["scenario"] for r in new}

fails, warns = [], []
for sid in by_id:
    if sid not in new_ids:
        fails.append(f"{sid}: in the baseline, missing from this run")
for r in new:
    sid = r["scenario"]
    mem, cnt = r["memory"], r["counters"]
    # G5 holds for every scenario: neither operator new nor net malloc growth.
    if mem.get("allocations_per_steady_frame", 0) > 0 or mem.get("allocations", 0) > 0:
        fails.append(f"{sid}: {mem['allocations']} heap allocations in {r['frames']} steady frames (G5)")
    if mem.get("mallinfo2_uordblks_delta", 0) != 0:
        fails.append(f"{sid}: mallinfo2 delta {mem['mallinfo2_uordblks_delta']} B over the steady frames (G5)")
    cap = BATCH_CAPS.get(sid)
    if cap is not None and cnt.get("batches", 0) > cap:
        fails.append(f"{sid}: {cnt['batches']} batches, cap {cap}")
    base = by_id.get(sid)
    gates = r.get("gates", {})
    base_gates = base.get("gates", {}) if base else {}
    for gid, verdict in gates.items():
        was = base_gates.get(gid)
        if verdict == "fail" and was != "fail":
            fails.append(f"{sid}: gate {gid} fail (baseline: {was or 'none'})")
        elif verdict == "fail":
            warns.append(f"{sid}: gate {gid} still fails (open in the baseline too)")
        elif verdict == "insufficient":
            warns.append(f"{sid}: gate {gid} has too few samples to judge")
    for gid in base_gates:
        if gid not in gates:
            fails.append(f"{sid}: gate {gid} in the baseline, missing from this run")
    if base is None:
        warns.append(f"{sid}: not in the baseline")
        continue
    stages = r["stages"]
    for name in base["stages"]:
        if name not in stages:
            fails.append(f"{sid}: stage {name} in the baseline, missing from this run")
    for name, st in stages.items():
        bs = base["stages"].get(name)
        if not bs:
            continue
        if (st["samples"] == 0) != (bs["samples"] == 0):
            line = f"{sid:3} {name:18} samples {bs['samples']} -> {st['samples']}"
            fails.append(line)
            print("  FAIL " + line)
            continue
        if st["samples"] == 0 or bs["p95"] <= 0:
            continue
        delta = st["p95"] - bs["p95"]
        pct = 100.0 * delta / bs["p95"]
        line = f"{sid:3} {name:18} p95 {bs['p95']:10.3f} -> {st['p95']:10.3f} us  {pct:+7.1f} %"
        if delta >= floor and pct > FAIL_PCT:
            fails.append(line)
            print("  FAIL " + line)
        elif delta >= floor and pct > WARN_PCT:
            warns.append(line)
            print("  WARN " + line)
        else:
            print("  ok   " + line)

for w in warns:
    print("WARN: " + w)
for f_ in fails:
    print("FAIL: " + f_)
print(f"== {len(fails)} failure(s), {len(warns)} warning(s)")
sys.exit(1 if fails else 0)
PY
else
  echo
  echo "== no baseline at ${ref#"$ROOT"/}: nothing to compare (run with --update-baseline to create it)"
fi

if [[ "$update_baseline" -eq 1 ]]; then
  cp "$out" "$baseline"
  echo "== baseline written: ${baseline#"$ROOT"/}"
fi

exit "$status"
