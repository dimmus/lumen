---
name: lumen-perfetto-bench-baseline
description: "2026-11-18 notebook: ui-bench.sh --perfetto records traced S1–S4 (LUMEN_TRACE_FILE per scenario process, never a baseline); first baseline traces on hasvk vulkan at 9aa7b4d (117k events S2); spans placed outside the timing reads keep untraced numbers identical."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-18 on the notebook `iritur` (hasvk HD 5500, machine key `iritur-08fd3c`),
task `perfetto-bench-baseline` (branch `zcode/task/perfetto-bench-baseline`, 46454ee
instrumentation + 12f608a script/tests + 9aa7b4d docs):

- `scripts/ui-bench.sh --perfetto` is a **traced pass that replaces the measured one**:
  it exits before quiet-CPU hold / warm-up / the measured loop, compares nothing, writes
  no baseline, and refuses `--update-baseline`, `--compare`, `--client`, `--e2e` (exit 2;
  the last two because `LUMEN_TRACE_FILE` is per-process — a child compositor writing the
  same file corrupts it; client-mode tracing stays out of scope). Traces land beside the
  bench JSONs as `tests/bench/results/<machine>-<backend>/<date>-<sha>[-N]-S<k>.trace.json`
  (per-scenario files: each scenario is its own bench process), validated in-pass by the
  script (parses, non-zero spans+counters) and sha256-printed.
- The bench needed the process-side switch: lumen-ui-bench linked lx.trace and the engine
  already emitted `ui.describe/ui.emit/ui.layout/ui.reducers` spans + `scene.*` counters,
  but nothing called `timeline_start_from_env` — a bench trace was empty before this.
  Instrumentation: start at bench start, `name_thread("bench")`, LX_TRACE_SCOPE around
  each scenario/reshape/layout_solve and the stages (update, tick, order, encode, draw as
  raster|submit, none omitted), counters `ui.frame_us/idle_us/raster_us|submit_us/gpu_us`,
  flush after the bench thread joins.
- **Span placement is what keeps the numbers honest**: every span's ctor runs before the
  stage's first `now_ns()` and its dtor after the second, so the measured deltas exclude
  span cost entirely (a span inside the window would tax tick_idle's ~170 ns p50 by %).
  Proof: pre/post-change `--scenario all --frames 60 --backend none` JSONs have identical
  key structure and identical deterministic integers (counters, sample counts, gates); only
  timings + rss/faults/preemptions move, which move between any two runs; full vulkan gate
  PASS (0 failures, the 4 disclosed-open G5 warnings = the baseline's own 1072 B driver
  growth; warm-up settled probe 1 at 36.674 µs vs the 36.74 µs baseline).
- Baseline traces recorded on hasvk vulkan at 9aa7b4d (S1 2 177 / S2 117 398 / S3 32 643
  / S4 32 644 events; S2 12 MB) with sha256s, argv and the same-tree bench JSON under
  `evidence/2026-11-18-perfetto-baseline/`. LUMEN_TRACE_EVENTS bound 2 000 000 (lx.trace's
  default) is ~10× headroom for the default frame counts (S2 ≈ 101k events).
- No `trace_processor`/`trace_processor_shell`/`perfetto` binary exists on this host —
  loadability is JSON-parse + field-consistency evidence only; a ui.perfetto.dev load was
  never performed and must not be claimed.

**Why:** a Perfetto capture of the bench was previously impossible (empty trace) and had
no defined artifact home or separation from the compared numbers; the separation is
structural (separate pass + refusals), not a promise.

**How to apply:** capture with `LUMEN_BUILD_DIR=<dir> ./scripts/ui-bench.sh --perfetto
--backend vulkan`; treat the output as timeline evidence only — never compare it, never
baseline it; copy artifacts + sha256 + argv under `evidence/<date>-<name>/` per the
lumen-testing contract. Compositor-side needs no flag: `LUMEN_TRACE_FILE=<path>
lumen-compositor` (see docs/subsystems/errors-and-logging.md §Timeline). When instrumenting
a measured loop, put spans around the clock reads, never between them.
