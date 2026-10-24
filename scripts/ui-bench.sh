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
#   --e2e               reserved (ADR-010 §8.5): no client mode before Phase 4
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
frames=""
warmup=120
pin=""
build=1
update_baseline=0
compare=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) backend="$2"; shift 2 ;;
    --frames) frames="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --pin) pin="$2"; shift 2 ;;
    --no-build) build=0; shift ;;
    --update-baseline) update_baseline=1; shift ;;
    --compare) compare="$2"; shift 2 ;;
    --e2e) echo "FAIL: --e2e needs the client runtime (ADR-010 Phase 4)"; exit 2 ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
done

if ! command -v python3 >/dev/null; then
  echo "FAIL: python3 is needed for the JSON comparison"
  exit 2
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
