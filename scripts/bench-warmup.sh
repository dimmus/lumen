# The host's measured state, warmed and verified before the measured scenarios. Sourced
# by ui-bench.sh (the headless path and --client) and tests/smoke_harness.sh; not run
# on its own.
#
#   warm_s2_frame_p50 JSON   the file's S2 frame p50, empty when it has none
#   warm_host BACKEND [REF]  warm the host into its measured state or exit 2; REF is
#                            the JSON holding the S2 frame p50 to settle against —
#                            given, it beats both $baseline and $compare (ui-bench's
#                            --client path passes the tier's headless baseline: the
#                            client rows have no S2 to settle against)
#
# Expects the caller's context: $BENCH (lumen-ui-bench), $runner (its invocation
# wrapper, e.g. setarch), $baseline (the compared reference; $compare overrides it),
# $frames, $warmup, $pin, $preset and $ROOT.
#
# On the notebook's GPU backends the host has a second mode besides the idle states and
# frequency scaling ui-bench.sh's quiet-CPU hold pins: a wandering state, minutes-long
# eras, in which every memory-bound stage reads
# ~2x (S2 frame p50 71 us against a 36.74 us baseline — layout, emit, submit all
# uniformly 2.1x) while update, tick_idle and layout_solve.warm stay at baseline and
# --backend none is immune; the core's clock (APERF/MPERF-sampled), the package
# temperature, the iGPU's rps frequency and the other CPUs' governors are all
# identical in both modes (interleaved A/Bs, evidence/2026-11-14-ui-bench-vulkan-ab/).
# Its onset is not reproducible (idle gaps do not trigger it), but sustained bench
# load usually relaxes it within a few rounds — and no hold for it is known to work
# (MSR 0x620, UNCORE_RATIO_LIMIT, is writable on this host; whether the hardware
# honors a raised min ratio is untested). So the gate warms the host into the state
# the baseline was recorded in and verifies it got there — a short unmeasured S2 probe
# of this backend first, full-length unmeasured warm-up rounds while the probe reads
# above the settle threshold, and exit 2 when the pole is not reached: a slow-mode run
# is the host's, not the code's, and is neither a regression nor green.
warm_s2_frame_p50() { # JSON_FILE — the file's S2 frame p50, empty when it has none
  python3 -I - "$1" <<'PY'
import json
import sys

try:
    with open(sys.argv[1]) as f:
        rows = json.load(f)
except (OSError, ValueError):
    sys.exit(0)
for r in rows:
    if r.get("scenario") == "S2":
        st = r.get("stages", {}).get("frame", {})
        if st.get("samples", 0) > 0 and st.get("p50", 0) > 0:
            print(st["p50"])
        break
PY
}
warm_host() { # BACKEND [REF_JSON] — REF overrides the baseline/--compare settle reference
  local backend=$1 ref="${2:-}" probe_dir probe_json base p50 settle n fp
  [[ -z "$ref" ]] && ref="$baseline"
  [[ -z "${2:-}" && -n "$compare" ]] && ref="$compare"
  base="$(warm_s2_frame_p50 "$ref")"
  if [[ -z "$base" ]]; then
    echo "== host warm-up: no S2 frame p50 in ${ref#"$ROOT"/} to settle against — skipped"
    return 0
  fi
  settle=$(awk -v b="$base" 'BEGIN { printf "%.2f", b * 1.15 }')
  fp=${frames:-6000}
  (( fp > 6000 )) && fp=6000
  probe_dir="$(mktemp -d)"
  for n in 1 2 3; do
    probe_json="${probe_dir}/probe-${n}.json"
    "${runner[@]}" "$BENCH" --scenario S2 --backend "$backend" --frames "$fp" \
      --warmup "$warmup" --pin "$pin" --json "$probe_json" --git warmup --preset "$preset" \
      >/dev/null 2>&1 || true
    p50="$(warm_s2_frame_p50 "$probe_json")"
    if [[ -z "$p50" ]]; then
      echo "== host warm-up: probe $n produced no S2 frame samples — measuring anyway"
      rm -rf "$probe_dir"
      return 0
    fi
    echo "== host warm-up: probe $n S2 frame p50 ${p50} us (settles at <= ${settle})"
    if awk -v p="$p50" -v s="$settle" 'BEGIN { exit !(p <= s) }'; then
      rm -rf "$probe_dir"
      return 0
    fi
    if (( n < 3 )); then
      echo "              warming: one full-length unmeasured S2 round"
      "${runner[@]}" "$BENCH" --scenario S2 --backend "$backend" --frames "${frames:-36000}" \
        --warmup "$warmup" --pin "$pin" --json "${probe_dir}/warm-${n}.json" \
        --git warmup --preset "$preset" >/dev/null 2>&1 || true
    fi
  done
  rm -rf "$probe_dir"
  echo "FAIL: the host would not settle into its measured state — three probes read"
  echo "      ${p50} us against the baseline's ${base} us (settle threshold ${settle})."
  echo "      After idle this host's GPU backends read every memory-bound stage ~2x"
  echo "      (evidence/2026-11-14-ui-bench-vulkan-ab/README.md); that is the host's"
  echo "      mode, not the code's — not measuring."
  exit 2
}
