---
name: lumen-memory-pressure-green
description: "2026-11-15 notebook task (worktree memory-pressure-green, d24e1f3..6d68155, gate READY, unmerged): spurious 'compositor.memory: critical pressure' under app-smoke load fixed by fixing the measurement, not the line — staging pressure now judges shm_upload_pool::bytes_in_flight() (the upload backlog) instead of bytes_mapped(), which counted power-of-2 size-class rounding (23.04 MB fullscreen update → 32 MiB block) plus cached spare blocks, reading two mapped 32 MiB blocks as exactly 67108864 = 100% of the 64 MiB budget on a host with ~9.5–12 GiB free; root cause measured live: PSI produced zero transitions (1 startup line only), hasvk GPU budget ~2.4%, 60 ms lumen-ctl poll caught staging_bytes pinned at exactly 67108864 for 53 consecutive critical samples and 9/9 battery warns follow fullscreen probe maps (line distances 9–335); thresholds untouched (64 MiB / 75% / 90% — the budget now equals the pool's own drop point); final run 177 PASS / 0 WARN / 0 FAIL with zero critical lines, render_us p50 429→229 (thrash gone), suite 132/134 tolerated pair; traps: the session's own 60 ms poller caused an N8 FAIL, the diagnosis TSV was overwritten in /tmp (disclosed, conclusion re-pinned on committed evidence), packet says test_runtime_psi 12/0 but the binary runs 11/0"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-15 on the **notebook** (iritur i915/hasvk, [[lumen-notebook-hw]]).
Worktree `/home/dimmus/lumen-wt/memory-pressure-green`, branch
`zcode/task/memory-pressure-green` off f84e2dd ([[lumen-vulkan-capture-green]] tip),
HEAD **6d68155**, **unmerged** (ran: `git merge-base --is-ancestor HEAD main` → not an
ancestor); worktree clean. One code commit **d24e1f3** (6 files +101/−5:
compositor_impl.cpp, protocols.cppm, lx.health/types.cppm, two test files,
docs/subsystems/memory-management.md); evidence **094c48f** amended to **5da8656**
during the review fix (README numbers corrected toward the data, AuthorDate preserved),
**ddfd164** appended (provenance disclosure, README-only — ran:
`git diff 094c48f..ddfd164 --stat` = evidence README +14/−3, nothing else), **6d68155**
gate-time smoke corroboration in its own `evidence/smokes/gate-workflow/` namespace.
Checks marked "ran" were executed by the memory writer this session.

- **Root cause — the measurement, not the thresholds.** `tick_ui` and the health report
  fed `shm_store_.bytes_held() + shm_pool_.bytes_mapped()` into the 64 MiB
  `staging_budget_bytes` (memory.cppm: `staging_budget_bytes = 64 MiB`,
  `pressure_moderate_used_pct = 75`, `pressure_critical_used_pct = 90` — all unchanged
  by d24e1f3). `bytes_mapped()` counts (a) power-of-2 size-class rounding — a
  3200×1800×4 fullscreen shm update is 23.04 MB but takes a 32 MiB block (+39%) — and
  (b) cached spare blocks held ≤ 1 s (`k_staging_idle_ns`) by design. A fullscreen
  client maps two 32 MiB blocks = **exactly 67,108,864 = 100% of budget** → past the 90%
  critical line on a host with ~9.5–12 GiB free. Measured eliminations (all ran this
  session): exactly **1 `runtime.psi` line** in the failing battery log (the startup
  arming line) — PSI never fired; hasvk budget 8.36 GiB vs ~207 MB i915 usage ≈ **2.4%**
  — the GPU pool cannot trip; arenas near empty. Scripted correlation re-run: **9/9**
  critical warns in evidence/2026-11-15-vulkan-capture/final-head each preceded by a
  `map id=… lumen-probe w=3200 h=1800` at line distances 9/87/95/174/250/258/265/271/335.
  Live proof (before-fix binary): 60 ms `lumen-ctl --json status` poll caught
  `memory.pressure=critical` with `staging_bytes` pinned at exactly 67108864 for **53
  consecutive samples** (TSV not retained — see fuckups).
- **The fix (d24e1f3): judge the backlog, keep the line.** `tick_ui`, the health report
  and the critical warn now use `shm_upload_pool::bytes_in_flight()` (bytes staged, not
  yet uploaded; protocols.cppm:176-184) — a cached spare block is capacity, not
  consumption, and the 64 MiB budget becomes the pool's own drop point
  (`k_max_in_flight_bytes`, protocols.cppm:152, where `acquire` declines updates at
  :1087): 100% means updates are being dropped, 90% means drops are imminent. The warn
  keeps its grep-stable prefix `critical pressure — evicting caches` and appends
  `(staged X KiB of Y budget, gpu …)` so the next wrong verdict is a grep. The doc's
  "no more than 64 MiB is staged and not yet uploaded" now matches the code.
- **The response had been self-amplifying:** each critical transition `trim()`s the
  cached 32 MiB block, the next fullscreen commit re-mmaps and re-faults it, mapped
  climbs back to 64 MiB → critical again (4–9 warns/run), plus pointless import-cache
  evictions (staging is host anon memory). After the fix the backlog never exceeded
  23,040,000 (one fullscreen frame, **34.3%**, single 60 ms sample with zeros either
  side; every other sample ≤ 3,413,124; **0 samples at 67108864**) — ran: full parse of
  committed `after-fix-health-poll.tsv` (5875 JSON samples, all `normal`, 59 `down`
  before the compositor served). import_cache stays populated (max 9 of 512) instead of
  being thrashed.
- **A/B (§1.2, same host/seat/scenarios; recomputed by the gate, README table
  cross-checked):** before (f84e2dd binaries) 176 PASS / 0 WARN / 1 FAIL — the FAIL the
  clean-log row, errors+warnings=4, all four the warning; **after: 177 PASS / 0 WARN /
  0 FAIL, VERDICT PASS, clean-log row `errors+warnings=0 banned=0`, 0 `critical
  pressure` lines** in final-run/compositor.log and after-fix-compositor.log (ran:
  grep). Fullscreen-probe window render_us p50/p95 429/9445 → **229/6473** (thrash
  gone); composite_us 189/4255 → 197/4250 and encode_us 222/6177 → 222/6235 unchanged
  within host noise. The signal survives where real: thresholds test green
  (50/64 → moderate, 61/64 → critical) and the PSI path is untouched
  (`external_pressure_combines_with_the_pools` green).
- **Tests (all ran green at HEAD, build a ninja no-op, LUMEN_BUILD_TESTS=ON):**
  test_shm_staging **25/0** (new: `pool_in_flight_counts_the_backlog_not_spare_capacity`
  — backlog 300 KiB, not the 512 KiB size class, released-but-cached stays mapped
  without counting; `pool_backlog_reaches_the_cap_where_updates_are_declined` — in-flight
  reaches exactly 64 MiB and acquire declines past it), test_runtime_memory **9/0** (new:
  `staging_pressure_judges_the_backlog_against_the_drop_point`), test_runtime_psi
  **11/0**. Suite recorded **132/134**, failures exactly {17:ui_gallery, 82:
  test_task_model} (LastTestsFailed.log read; full ctest **not re-run by me** — it
  rewrites Testing/Temporary, [[lumen-unit-pair-green]]'s trap). Gate-time corroboration
  (6d68155, ran): app-smoke 177 PASS / 0 FAIL / VERDICT PASS / 0 critical lines;
  drag-smoke `VERDICT: PASS (warnings: g latency)` — the known noise family
  ([[lumen-toplevel-drag-compositor]]).
- **Fuckups:** (1) the diagnosis poller truncated and reused a single /tmp output path,
  so the after-fix run **overwrote the before-fix TSV** — the 53-sample pin is
  live-observed, not re-derivable; disclosed in ddfd164 and re-pinned on committed
  evidence (size-class arithmetic + 9/9 battery correlation). Ran: `cmp
  /tmp/mp-diag/health-poll.tsv <worktree>/after-fix-health-poll.tsv` → byte-identical;
  diag summary `errors+warnings=4` with the WARN, matching the README. (2) the session's
  own 60 ms poller caused an **N8 idle-sleep FAIL** (~17 control-socket wakes/10 s =
  the poll rate) — final run dropped the poller; an observer can fail the thing it
  watches. (3) review finding fixed by amending 5da8656: the README had understated the
  after-fix peak backlog as 3.3 MB when the committed TSV holds one 23,040,000 sample —
  correction moved the claim toward the data.
- **Packet inaccuracies, for the record (none breaks an acceptance row):**
  `test_runtime_psi 12/0` — the binary runs **11/0** (ran twice: gate + me);
  `gate_runs.seat` "ui-bench (engine, stored baseline) → exit 0" and `gate_runs.bench`
  "run" are unsubstantiated — evidence/ holds only inherited 2026-11-14-ui-bench-* dirs,
  no bench artifact from this task (the implementer's deviations state ui-bench was not
  run and belongs to the studio workflow after the gate); the drag-smoke row is
  substantiated by the gate-time 6d68155 run, not by the implementer; the packet's
  evidence-commit hash 094c48f is the pre-amend form of 5da8656.

**Disproven / corrected (do not re-chase):** "the warning is PSI/tmpfs pressure" — on
the notebook PSI produced zero transitions in every failing run; the
[[lumen-app-smoke-known-noise]] PSI attribution stays true for the VM's genuine spikes
only. "The GPU/Vulkan budget tripped" — hasvk sits at ~2.4%. "The thresholds need
recalibrating" — 64 MiB / 75% / 90% are unchanged; the input signal was wrong. "Fix it
by silencing the line" — rejected by the card; the line survives with more context.

**Why:** a health metric that sums a pool's *footprint* instead of its *backlog* turns
normal working-set behavior (size-class allocation, cached reuse) into a false alarm,
and the alarm's own response (trim) recreates the condition — a feedback loop that
looks like memory pressure on a host with gigabytes free. Fix the instrument before
touching the thresholds: here the same 64 MiB budget became meaningful for free because
it coincides with the pool's own drop point.

**How to apply:** staging pressure means `bytes_in_flight()` (staged, not yet uploaded);
never judge capacity by `bytes_mapped()` — size-class rounding and cached spare blocks
are by-design footprint. Do not recalibrate the 64 MiB / 75% / 90% config without new
measurement; the budget equals `k_max_in_flight_bytes`, so 100% = updates declined. The
warn line's prefix `critical pressure — evicting caches` is grep-stable — scripts may
key on it, the appended pool context is additive. When smoke-testing with a `lumen-ctl`
poller, expect it to perturb idle-sleep rows (N8) — run the certified pass without the
poller, and never let two runs share one /tmp output path. Discrepancy rule that caught
the review finding: recount the committed TSV/log yourself before quoting a max.

Related: [[lumen-app-smoke-known-noise]] (the VM-side PSI row this corrects for the
notebook), [[lumen-vulkan-capture-green]] (base branch; the battery logs whose 9 warns
correlated), [[lumen-notebook-hw]] (hasvk/i915 host), [[lumen-no-degradation-rule]]
(the A/B render_us evidence), [[lumen-unit-pair-green]] (tolerated pair; the
Testing/Temporary trap), [[lumen-toplevel-drag-compositor]] (g-latency noise family),
[[lumen-smoke-bench-harness-green]] (gate-workflow namespace rule).
