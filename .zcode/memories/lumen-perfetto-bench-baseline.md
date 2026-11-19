---
name: lumen-perfetto-bench-baseline
description: "2026-11-18 notebook: ui-bench.sh --perfetto records traced S1–S4 (LUMEN_TRACE_FILE per scenario process, never a baseline); first baseline traces on hasvk vulkan at 9aa7b4d (117k events S2); spans bracketing their own clock reads keep untraced numbers identical; gate READY at 0b45038, review noise 2/2/0/0, seat smokes red = dead uinput holder."
metadata:
  node_type: memory
  type: project
summary: "ui-bench --perfetto = traced pass refusing measurement flags; bench timeline instrumented (spans bracket their own clock reads); first hasvk baseline traces at 9aa7b4d"
modules: lx.trace, tests/bench
topics: perfetto, bench

---

Verified 2026-11-18 on the notebook `iritur` (hasvk HD 5500, machine key `iritur-08fd3c`),
task `perfetto-bench-baseline` (worktree `/home/dimmus/lumen-wt/perfetto-bench-baseline`,
branch `zcode/task/perfetto-bench-baseline`, 7 commits on f99d195, tip 0b45038, gate
READY, unmerged: 46454ee bench instrumentation + 12f608a script/tests + 9aa7b4d docs +
c4febd1 memory + 99cfe0a encode-window fix + 4fa2f31 corrigendum + 0b45038 gitignore
one-line):

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
- **Span placement is what keeps the numbers honest**: each stage opens its measured
  window on its own `now_ns()` read — never on the previous stage's closing one — so the
  measured deltas exclude span cost entirely (a span inside the window would tax
  tick_idle's ~170 ns p50 by %). Corrigendum (independent review, fixed at 99cfe0a): at
  9aa7b4d the encode window opened on order's closing read `t3`, so order's dtor, the
  `ui.batches` counter check and encode's ctor ran inside the measured `[t3, t4]` —
  bounded at single-digit ns (lx.trace.cppm:573-593: disabled ctor = one relaxed
  `timeline_enabled()` load + `should_log` compare, dtor short-circuits on
  `start_ns_ == 0`) against encode p50s of 11.3-79 µs, and the re-run vulkan gate passed
  (S4 encode +4.9 % = 0.56 µs < the 1.0 µs ABS_FLOOR_US), so the recorded numbers stand;
  encode now brackets its own reads like every other stage.
  Proof: pre/post-change `--scenario all --frames 60 --backend none` JSONs have identical
  key structure and identical deterministic integers (counters, sample counts, gates); only
  timings + rss/faults/preemptions move, which move between any two runs (re-proven for
  99cfe0a the same way: 362 leaf paths identical, only timings + involuntary_switches
  moved, and a traced S3 pass still emits every stage span); full vulkan gate
  PASS (0 failures, the 4 disclosed-open G5 warnings = the baseline's own 1072 B driver
  growth; warm-up settled probe 1 at 36.674 µs vs the 36.74 µs baseline).
- Baseline traces recorded on hasvk vulkan at 9aa7b4d (S1 2 177 / S2 117 398 / S3 32 643
  / S4 32 644 events; S2 12 MB) with sha256s, argv and the same-tree bench JSON under
  `evidence/2026-11-18-perfetto-baseline/`. LUMEN_TRACE_EVENTS bound 2 000 000 (lx.trace's
  default) is ~10× headroom for the default frame counts (S2 ≈ 101k events).
- No `trace_processor`/`trace_processor_shell`/`perfetto` binary exists on this host —
  loadability is JSON-parse + field-consistency evidence only; a ui.perfetto.dev load was
  never performed and must not be claimed.

**Final review + gate (2026-11-18, tip 0b45038):**

- Review noise **2 raised / 2 confirmed / 0 refuted / 0 preexisting**. Both confirmed
  findings fixed: (1) the encode window above (99cfe0a, honestly disclosed in the 4fa2f31
  corrigendum — the recorded numbers stand, bounded by the disabled-span cost); (2) low —
  the root .gitignore entry was four lines (three of them a comment admitting redundancy)
  where the acceptance asked for one: 0b45038 keeps exactly
  `/tests/bench/results/**/*.trace.json` (.gitignore:32); `git check-ignore -v` still
  resolves traces via the pre-existing `*` rule in `tests/bench/results/.gitignore` — the
  root line names the policy, the results-local rule does the matching.
- Gate READY at 0b45038 (clean tree). The packet's suite-green claim had no backing run
  (implementer and reviewer both declined; the only LastTest.log was empty — the
  [[lumen-snprintf-log-migration]] Testing/Temporary trap), so the gatekeeper ran the full
  tests-enabled suite itself: `ctest --test-dir build/test --output-on-failure -LE bench
  -j2` = **136/136 passed (38.6 s)**, bench_ui + smoke_harness 2/2;
  `LUMEN_BUILD_DIR=$PWD/build/test ./scripts/ui-bench.sh --backend vulkan --no-build` rc 0,
  "0 failure(s), 9 warning(s)" = the 4 disclosed-open G5 driver-growth warnings
  (baseline-equal) + 5 nanosecond-scale S2 idle rows (p50 0.060→0.192 µs, 'ok' in both
  prior same-tree runs — the recorded non-repeating S2/S4 noise family, not a regression).
  Re-verified at record time: `sha256sum -c SHA256SUMS` = 5/5 OK in the evidence dir, and
  my independent JSON parses of the stored traces match the recorded counts exactly (S1
  2 177 events = 2 metadata + 1 448 X spans + 727 C counters; S3 32 643 = 17 281 + 15 360
  + 2; span names S1|S3/tick/order/encode/submit/ui.describe/ui.emit/scene.order).
- **Seat smokes FAILED (drag-smoke and app-smoke exit 1) and are NOT converted to a
  pass**: root cause is the dead-uinput-holder family again (see
  [[lumen-snprintf-log-migration]]) — no 'Lumen Inject Mouse' holder alive, a stale event3
  node without REL_X/REL_Y, the injector falling back to event2 which has no absolute
  axes, so every injection returned EINVAL and zero input was delivered. The change cannot
  have caused it: `git diff f99d195..0b45038` over drag-smoke.sh/app-smoke.sh/
  inject-input.py/uinput-mouse.py/lx.input/lx.compositor is **empty** (re-run at record
  time; the only scripts/ path in the whole diff is ui-bench.sh). This task's acceptance
  defines its gate as ctest + bench PASS because the touched surface is test-path only —
  but seat verification remains **owed** before any change that touches the display/input
  surface (restart the uinput holder from this tree's `scripts/uinput-mouse.py` first).

**Why:** a Perfetto capture of the bench was previously impossible (empty trace) and had
no defined artifact home or separation from the compared numbers; the separation is
structural (separate pass + refusals), not a promise.

**How to apply:** capture with `LUMEN_BUILD_DIR=<dir> ./scripts/ui-bench.sh --perfetto
--backend vulkan`; treat the output as timeline evidence only — never compare it, never
baseline it; copy artifacts + sha256 + argv under `evidence/<date>-<name>/` per the
lumen-testing contract. Compositor-side needs no flag: `LUMEN_TRACE_FILE=<path>
lumen-compositor` (see docs/subsystems/errors-and-logging.md §Timeline). When instrumenting
a measured loop, put spans around the clock reads, never between them, and open each
stage's window on its own read — a window that opens on the previous stage's closing
read silently swallows that stage's dtor and its own ctor. Before any seat-touching task,
regenerate the uinput holder (`scripts/uinput-mouse.py`) — seat smokes on this host fail
with EINVAL injections when it is dead, and environmental failure never converts to a
pass. See [[lumen-ui-bench-noise-and-g7]] for the S2/S4 noise family and
[[lumen-notebook-hw]] for the host.
