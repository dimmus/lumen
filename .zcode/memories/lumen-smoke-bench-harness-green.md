---
name: lumen-smoke-bench-harness-green
description: "2026-11-14/15 notebook harness task (worktree smoke-bench-harness-green, f1d619f..8861dc0, gate NOT_READY twice on the same evidence-overwrite pattern): round 1 = injection autodetect by device name (REL-capable uinput mouse, LUMEN_INJECT_* overrides), build-system freshness (build-fresh.sh), host-aware app-smoke geometry, 15 backend-limited WARNs (Vulkan-capture contract), quiet-CPU PM-QoS hold fail-closed + 5 iritur-08fd3c baselines; round 2 = vulkan ~2x bench mode root-caused to what it is NOT (hasvk-workers-off-core refuted by all-core interleaved A/B 73.6/70.1/71.0/53.1 vs cpu3-only 71.0/70.5/78.2/72.2 us; wandering vulkan-only ~2x on memory-bound stages, MSR 0x620 the knob, its 'EIO' claim retracted bb59b9a) and the gate now warms + verifies the measured state (bench-warmup.sh, settle = baseline S2 p50 x1.15, exit-2 refusal; 3 green vulkan gate runs S2 36.06/40.16/35.88 vs 36.74 us); smoke_harness 48 checks; round 3 fuckup = aaedada's acceptance-level seat evidence (158/15/4 + zero-warning drag) overwritten by 28e2ef3 with dirty re-runs; round 4 closeout (2026-11-15) re-did it green at b3ac4fb (drag zero-warning PASS row-g p95 25.6 ms, app-smoke 158/15/4, Z2–Z5 flake root-caused to relative LUMEN_BUILD_DIR dangling portal-session.sh:113 symlink) and 8861dc0 then overwrote it again with below-level runs (drag g-latency p95 47.5 ms, app-smoke 154/15/8) — the b3ac4fb tree and /tmp/gate-r4 hold the good evidence; round 5 (2026-11-15, tip 062f3e6) closed gate READY as a pure RESTORE: 49fa712 re-checked out b3ac4fb's evidence/smokes/gate/ byte-identical (drag bare PASS 25.6 ms, app-smoke 158/15/4, review 0 findings), the gate-time step was re-namespaced to evidence/smokes/gate-workflow/ so it can never clobber the acceptance evidence again; post-close re-check same day (tip still 062f3e6): worktree clean — uncommitted precheck leak removed, merge blocker cleared (helper's gate/ write path unchanged in code), 3 stored none-backend bench JSONs (8861dc0 02:14 / 062f3e6-dirty 04:04 / 062f3e6 16:35) every gate pass across S1–S4 (clean-tip S2 36.556 µs vs baseline 36.74), packet seat exit-1s = the acceptance evidence's own 4 owner-owned FAIL rows + designated gate-workflow noise (drag g-latency, 154/15/8), suite 134 from a tests-enabled build with the tolerated pair"
metadata:
  node_type: memory
  type: project
summary: "Harness task: injection autodetect by name, build-fresh kills mtime false-trips, host-aware geometry, quiet-CPU PM-QoS fail-closed, bench warm-up gate; overwrite loop ended by namespace split"
modules: scripts, harness
topics: smoke, bench, uinput

---

Verified 2026-11-14 on the **notebook** (iritur, i7-5500U 4 CPUs, kernel 7.2.15 — read
from tests/bench/results/iritur-08fd3c-vulkan/20261114-143237a.json's host block).
Worktree `/home/dimmus/lumen-wt/smoke-bench-harness-green`, branch
`zcode/task/smoke-bench-harness-green` off 7c3e3a1, HEAD **143237a**; commits f1d619f
(injection/freshness), 87d97de (quiet-CPU + rebaseline), 1e4d39c (host-aware geometry),
edf4ec9 (seat-session.sh), 8b83a08+6feaa92 (WARN contract + skill/TODO), review fixes
e60c016/a878118/d5c4881/ef9650c/fe343d3, gate evidence 143237a. `git diff
7c3e3a1..HEAD --name-status`: only scripts/**, the 5 iritur-08fd3c baselines,
tests/smoke_harness.sh, a 6-line CMakeLists.txt test registration, .zcode/TODO.md, the
build-run skill, evidence/ — **no compiled code**.

- **Injection autodetect (f1d619f, scripts/inject-devices.sh):** the smoke scripts find
  the virtual "Lumen Inject Mouse" and the AT keyboard by device name wherever the
  kernel enumerated them (two same-named mice → REL-capable node preferred); explicit
  `LUMEN_INJECT_*` still overrides; nothing found falls back to the VM's event2/3/1.
  Kills the exit-2 `not an input device node` preflight death of the close-ghost gate
  (event16→event18 re-enumeration, [[lumen-close-ghost-fix]]). scripts/uinput-mouse.py
  now declares REL_X/REL_Y beside the absolute axes (:31,:83-86) so acceleration and
  relative-motion rows (R1/R4/W1/W2/W4) have a mouse — **a holder started from an older
  tree cannot pass them** (the coordinator's event16 holder predates REL). inject-input.py
  writes pixel-exact values on 1:1 virtual axes (centre-aim rounding had landed every
  other pixel on the neighbour — S1's last-column hit-test left the window) and gained
  KEY_EQUAL=13 plus punctuation/lock/keypad names; every name checked against
  /usr/include/linux/input-event-codes.h by the hermetic test.
- **Dependency-aware staleness (f1d619f, scripts/build-fresh.sh):** binary freshness is
  the build system's own determination — CONFIGURE_DEPENDS glob stamp check +
  `ninja -t query build.ninja` for the manifest, then a dry run of the targets through a
  wrapper manifest (a plain `ninja -n` stops at "Re-running CMake…" and never reaches the
  target). A touched module the binary does not link is no longer staleness — exactly the
  **dwfrun-5eff3d6b BLOCKED false trip** (lx.shell.cppm touched; lumen-compositor does
  not link lx.shell) — while a stale binary or changed CMakeLists.txt still fails.
- **Host-aware app-smoke geometry (1e4d39c):** the ~36 rows that embedded the dev VM's
  1914x924 output (TODO.md:21; 140/176 PASS on the notebook) now derive from the run —
  B6/B7 Super-drag to the margins its points were found at, D1's drag distance, D6's
  title height from the compositor's own decoration frame rect (the hardcoded 28 was
  stale since 0a67e97), H1's description from the logged output identity. Seat result
  **158 PASS / 15 WARN / 4 FAIL, twice in a row with identical rows** (was 145/31/1);
  reviewer re-run 157/15/5 — the 5th is N8 ("shell 3" context switches), the one-rerun
  flake the card pre-named ([[lumen-panel-blur-fix]] documents it). The 4 FAIL: Z1 Z6 Z7
  + the PSI run-clean-log row — owned by vulkan-capture-green / portal tasks, not this
  harness.
- **The 15 backend-limited WARNs (8b83a08 + a878118, owner decision 2026-11-14):** rows
  the Vulkan composite path cannot satisfy WARN naming gap + refusing code and count
  green, derived at run time via `record_limited(rest=)` (scripts/app-smoke.sh:3971-3983)
  — the WARN replaces failure **only when the backend-judgeable half passed** (rest
  fail-closed), so a second real defect inside those rows still fails. Rows: SIGUSR1
  read-back GL-only (S2 T4 T7 L2 L3 Q4 O2 O6 M1 N6 Y1), window capture refused on Vulkan
  (Y2 Y5 Q6, `wire_capture.cpp create_session`), capture dmabuf GL-only (Y4). The product
  work is TODO.md:21's **"Vulkan capture green"** task; the harness contract lives in the
  build-run skill (6feaa92).
- **Quiet-CPU hold + rebaseline (87d97de + e60c016, scripts/ui-bench.sh):** interleaved
  A/B (4 binaries × 3 rounds, evidence/2026-11-14-ui-bench-ab/) root-caused the "host
  drift" — the 1 ms frame clock lets the pinned core drop into cache-flushing C-states on
  some runs (one binary's S2 frame p50 29 vs 75 µs, tick_idle 0.17 vs 4 µs) plus
  schedutil wander (27–43 µs). Fix: a 10 µs PM-QoS request held on /dev/cpu_dma_latency
  (C1/C1E stay; nothing deeper) + performance governor on the pinned CPU; runs then agree
  within ~2 % (that table is --backend none). e60c016 makes it **fail-closed**: the hold
  is verified in effect (4-byte write mark, governor reads back "performance") or the run
  exits 2 without measuring — no passwordless sudo, dead holder or governor that did not
  take cannot produce a silent unheld verdict. All five notebook baselines re-recorded
  (tests/bench/baseline/iritur-08fd3c-{none,cpu,gl,vulkan,client-cpu}.json); **the
  committed 2026-11-03 ones are unreproducible by their own binary** (proven by the
  prescribed interleaved A/B). G5's mallinfo2 check is judged against the baseline's own
  driver-side growth (kernel 7.2.15, reproduced on the 2026-11-03 binary,
  g5-ab.txt) + 1 kB slack; d5c4881 names the driver-side cause only when the baseline
  recorded growth (scripts/ui-bench.sh:853-860 — the none/cpu baselines record 0).
  G5's operator-new check stays absolute.
- **seat-session.sh (edf4ec9):** the generic half of an interactive seat session, its
  run dir an argument — the cross-session trampling the close-ghost memory documents is
  fixed; byte-identical config restore covered by the hermetic test.
- **tests/smoke_harness.sh (ctest `smoke_harness`, CMakeLists.txt:1226-1227):** 28
  hermetic checks — build-fresh on a synthetic Ninja tree (fresh/stale/unlinked-module/
  manifest, the dwfrun trip included), inject-devices against a sysfs fixture (two
  same-named mice, capability preference, env override), seat-session run-dir isolation,
  key table vs input-event-codes.h, pixel-exact 1:1 mapping. Re-ran it this session:
  **PASS**. Suite 133→134 non-bench; **132/134** with only the tolerated pair
  {ui_gallery, test_task_model} (ctest at final HEAD per the gate verdict — not re-run
  by me; smoke_harness was).

**Round-1 gate verdict NOT_READY (superseded by the rounds 2–3 section below) — all
three blockers were provisioning/re-run remedies, no code repairs named.** (1) The committed final-HEAD seat run
(evidence/smokes/gate/app-smoke-logs/run.log, 143237a) is **env-pinned to the
coordinator's pre-REL holder** (line 1: `pointer /dev/input/event16
(LUMEN_INJECT_POINTER)`; the header NOTE names the missing REL) → VERDICT FAIL, 150
PASS / 15 WARN / 12 FAIL with non-tolerated R4 W1 W2 W4 red, while the REL-capable
holder (pid 1766725, started from this branch) was live — and none of the
acceptance-conforming runs (158/15/4 ×2, 157/15/5) is committed anywhere. (2) The
committed drag-smoke gate run is `PASS (warnings: g latency)` — row g p95 46.8 ms over
the 37.3 ms threshold; the zero-warning runs are claimed but uncommitted. (3)
**ui-bench's vulkan bimodality survives the CPU-only hold at final HEAD**: I extracted
S2 frame p50 from tests/bench/results/iritur-08fd3c-vulkan/20261114-143237a{,-2,-3}.json
= **70.988 / 34.718 / 33.527 µs vs baseline 36.737** — run 1 FAILs 9 rows (+93 %, the
exact bimodal family the hold targets), runs 2-3 exit 0; the ~2 % agreement was measured
on --backend none only. Everything else the gate verified green at HEAD: ctest 132/134
tolerated pair, smoke_harness 28 checks, all four review fixes in code, scope clean, no
regression vs pre-task (even the mis-provisioned run shows 150 PASS vs 145/31/1, and
geometry rows pass host-aware).

**Also found and reported, not acted on:** S3 order +26-43 % is 43d2831's `-march=native`
(reproducible on both code versions; A/B binaries left under gitignored build/ trees);
the vulkan-backend bimodality above is open and must be re-gated per the interleaved
A/B discipline before the next vulkan bench verdict.

**Disproven / closed (do not re-chase):** "ui-bench host drift is unfixable noise" —
root-caused to C-state/schedutil modes and removed on --backend none by the hold
([[lumen-ui-bench-noise-and-g7]]'s discipline found it). "G5 mallinfo2 growth is Lumen
code" — driver-side under kernel 7.2.15, present in the pre-task binary. "Newest-source
mtime" as a freshness oracle — disproven by dwfrun-5eff3d6b. "The vulkan ~2x mode is
hasvk's CPU workers escaping the held core" — refuted by the all-core interleaved A/B
(rounds/s2-B-1..4) and by `ps -L` showing no such threads. "MSR 0x620 refuses writes on
this host" — retracted (bb59b9a); same-value write succeeds. NOT disproven, open: the
mode itself — wandering, vulkan-only, ~2x on memory-bound stages, onset not
reproducible; holding it via a raised UNCORE_RATIO_LIMIT min_ratio is untried.

<!-- The full close-out chronology moved to [[lumen-smoke-bench-harness-green-rounds]] (record split 2026-11-19); the summary facts, the Why and the How below are the durable content. -->
