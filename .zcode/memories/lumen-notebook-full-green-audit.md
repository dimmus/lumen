---
name: lumen-notebook-full-green-audit
description: "2026-11-15/16 notebook task (worktree notebook-full-green-audit, merged tree c316906 = memory-pressure 6d68155 + unit-pair 255e757 merged in, final tip 6bc4820 after review fixes, gate READY, unmerged): the owner's full green matrix — all five legs PASS on real hardware with zero escalations: tests-enabled ctest 134/134 exit 0 twice (77.39/77.37 s; NOT the card's 133 — exactly one extra registered ctest, smoke_harness from smoke-bench-harness-green, proven by an add_test-registry diff, while tests/ differs by 12 files), drag-smoke VERDICT PASS zero warnings (11 rows, g-latency p95 24.3/22.7 ms vs 37.3 ms budget — the row that WARNed in memory-pressure's gate-time corroboration passes on the serialized host), app-smoke 177 PASS/0 WARN/0 FAIL with VERDICT at run.log:192 (all 8 Z portal rows + the PSI clean-log row PASS), ui-bench vulkan exit 0 (0 failures, 4 warnings all the disclosed-open G5 mallinfo2 1072 B = the baseline's own) and none exit 0 (0/0) with PM-QoS hold + host warm-up in both, zero 'critical pressure' and zero memory WARN/ERROR in both seat compositor logs; review 3 confirmed / 0 refuted — the load-bearing fix 6bc4820 untracked evidence/ after 0e63081 force-added 288 files under the b5edbea /evidence/ gitignore (HEAD and main track 0 evidence paths, merge-tree main→21cf2cc carries none); non-evidence delta after the merge = one SKILL.md paragraph, zero compiled code"
metadata:
  node_type: memory
  type: project
summary: "All five legs PASS on one session set at merged tree c316906: ctest 134/134 ×2, drag PASS, app-smoke 177/0/0, both benches green; the 6bc4820 evidence force-add reverted (0 tracked files)"
modules: harness
topics: audit, evidence

---

Verified 2026-11-15/16 on the **notebook** (i915 + nouveau re-read from
`/sys/class/drm/card*/device/driver` this session, [[lumen-notebook-hw]]). Worktree
`/home/dimmus/lumen-wt/notebook-full-green-audit`, branch
`zcode/task/notebook-full-green-audit`, **tip 6bc4820**, clean, **unmerged**
(`merge-base --is-ancestor HEAD main` fails). Evidence-only card: no source written.
Checks marked "ran" were executed by the memory writer this session.

- **Topology per the card's note:** branch cut from [[lumen-memory-pressure-green]] at
  6d68155 (ancestry carries [[lumen-vulkan-capture-green]] f84e2dd and
  [[lumen-smoke-bench-harness-green]] 062f3e6); [[lumen-unit-pair-green]] 255e757 —
  cut from main as a sibling — **merged in first** (c316906, parents 6d68155 + 255e757,
  ran: `git log -1 --format=%p`), plain union of disjoint scopes, only code change vs
  6d68155 = `tests/test_task_model.cpp` (rebuilt before the matrix). Binaries from the
  tests-enabled `build/test` (`LUMEN_BUILD_TESTS:BOOL=ON` in CMakeCache, ran), absolute
  `LUMEN_BUILD_DIR` (the vulkan-capture round-6 portal-symlink lesson), host serialized
  (no compositor/smoke running, load ≤ 0.64). Both seat runs logged `Vulkan composite
  path active` once (ran: grep on both compositor.logs) — the decisive backend.
- **Leg 1 — ctest 134/134, not the card's 133.** `100% tests passed out of 134`
  (summary.txt; 77.39 s, a second consecutive run 77.37 s; ran: 134 `Test Passed.` /
  0 Failed markers in LastTest.log). The delta is exactly one registered test:
  `diff <(git show 255e757:CMakeLists.txt | grep -o 'add_test(NAME …' | sort) <(same at
  HEAD)` → one line `> add_test(NAME smoke_harness)` (ran) — registered by
  smoke-bench-harness-green, passed at slot 74/139. The `tests/` *dir* differs by 12
  files (smoke_harness.sh + 11 modified by this branch's ancestry); unit-pair's own
  `test_task_model.cpp` is identical in both trees post-merge. Lesson: count tests from
  the add_test registry, not from a directory diff.
- **Leg 2 — drag-smoke PASS, zero warnings** (first since the tolerance began): 11 PASS
  rows, run row `errors+warnings=0`, `VERDICT: PASS` with no `(warnings: …)` suffix
  (ran). The g-latency row that WARNed in memory-pressure's gate-time corroboration
  (46.7 ms p95, [[lumen-toplevel-drag-compositor]] noise family) passes on the quiet
  serialized host: cursor p95 **24.3 ms**, window-move p95 **22.7 ms** / median 12.8 ms,
  both under the 37.3 ms budget.
- **Leg 3 — app-smoke 177 PASS / 0 WARN / 0 FAIL** (ran: grep counts), `VERDICT: PASS`
  at run.log:192; all 8 Z portal rows PASS and the PSI clean-log row `errors+warnings=0`
  PASS — i.e. the vulkan-capture capability rows and the memory-pressure clean-log row
  hold simultaneously on one tree, which is what "chained on all four fixes" means.
- **Leg 4 — ui-bench gate PASS on both backends** (the card named none; vulkan is the
  decisive one per the no-degradation gate, none is the baseline carrier): vulkan exit 0,
  **0 failures / 4 warnings** — gate-vulkan.out:150-153, S2+S4 each warning
  `mallinfo2 delta 1072 B` where the baseline's own driver-side growth is also 1072 B
  (`open, G5`) + `G5 still fails (open in the baseline too)`, i.e. the bench's disclosed
  open gate, not a regression; two consecutive exit-0 runs. none exit 0, 0 failures / 0
  warnings. Both outputs carry the `== quiet CPU: PM-QoS … held` line and host warm-up
  (probe S2 p50 37.26 µs ≤ 42.25 vulkan / 29.439 µs ≤ 31.38 none, ran) — the
  [[lumen-ui-bench-noise-and-g7]] measurement discipline in effect. Result JSONs keyed
  `20261116-c316906` (`"git": "c316906"`, ran).
- **Leg 5 — memory clean:** 0 `critical pressure` and 0 `[(WARN|ERROR)].*memor` lines in
  both seat compositor logs (ran: grep) — the memory-pressure acceptance holding under
  the exact workload (fullscreen probe maps, screencast dmabuf) that produced 9/9
  spurious critical WARNs before d24e1f3.
- **Review 3 confirmed / 0 refuted; all three fixed.** (1, medium — the fuckup)
  0e63081 `git add -f`-ed 288 files under the `/evidence/` gitignore that unit-pair's
  b5edbea created specifically so "a merge cannot carry them" — at that point the tree
  tracked 1624 evidence paths vs main's 0, undoing the predecessor's merge-safety
  policy. Fixed by 6bc4820 (`git rm -r --cached evidence/`): HEAD and main each track
  **0** evidence paths (ran: ls-tree), `git merge-tree --write-tree main HEAD` → exit 0,
  tree 21cf2cc, 0 evidence files (ran). The full log set stays retrievable in history
  (`git show a4fb830:evidence/2026-11-15-notebook-full-green-audit/app-smoke-logs/run.log`
  → the 177-row run) and on disk (1645 files) — the b5edbea pattern: local evidence,
  history keeps it reachable, the merged tree carries none. (2, low) 5679c87's
  corrected-attribution sentence itself overstated ("only by tests/smoke_harness.sh") →
  a4fb830 rewrote it to the verified 12-file/one-extra-ctest facts. (3, low, inherited)
  lumen-build-run SKILL.md still said app-smoke "158 PASS / 15 WARN / 4 FAIL" — stale
  since vulkan-capture + memory-pressure landed → 3dd59a5 re-scoped it to 177/0/0 with
  an exact `run.log:192` citation. Slightly outside the card's evidence/** scope,
  disclosed in the commit message.
- **Deviations, both disclosed:** ctest 134 vs the card's 133 (explained above —
  "everything green" is 134 on this lineage); two follow-up commits (3aed65a duplicate
  bench-JSON cleanup, 5679c87 + a4fb830 README corrections) instead of amending 0e63081,
  per the AGENTS.md no-amend rule. Non-evidence delta c316906..6bc4820 = exactly the one
  SKILL.md paragraph (`git diff --name-only c316906..HEAD -- . ':!evidence'` →
  `.zcode/skills/lumen-build-run/SKILL.md`, ran) — zero compiled code, so the binaries
  the matrix ran match HEAD's sources and no §1.2 re-measurement is owed.
- **Host finding — concurrent evidence writes:** a post-close workflow stage re-ran
  smokes into the predecessor `evidence/smokes/gate-workflow/` namespace at ~00:46–00:48
  (302 files newer than 00:40, mass mtime 00:48:30 — ran), which is why the on-disk
  count is 1645 vs the review's 1644; this task's five leg files are untouched
  (mtimes 23:50–00:08) and the tracked tree is unaffected. Two namespaces, one evidence
  root: sessions running in parallel write into each other's directories unless each
  owns its own.
- **Packet nit, for the record:** `gate_runs.suite` boilerplate says "tolerated
  known-red: ui_gallery, test_task_model" — contradicted by the recorded run itself:
  both tests `Test Passed.` in the 134/134; the evidence is authoritative, the field
  understates it (the known-red pair was retired by [[lumen-unit-pair-green]]).

**Disproven / corrected:** "the tests/ dir differs from unit-pair-green's only by
smoke_harness.sh" — 12 files; the registry diff is the true one-test claim. "134/134
means a new test appeared under this task" — none; the extra test is inherited from
smoke-bench-harness-green and unit-pair-green's count of 133 never included it.

**Why:** the owner's acceptance — "everything green and OK on real hardware" — is only
as strong as the tree it ran on: its whole value was proving four independently
reviewed fix lineages *compose* (merge first, rebuild, then measure one session set), and
each leg's PASS doubles as the predecessor fix's regression test. The fuckup half is
about policies living in gitignores: an evidence commit staged with `git add -f`
silently reverted the exact merge-safety rule the previous task's fix round had
installed, and only the review caught it — evidence work must end with the same
ls-tree/merge-tree check a code task ends with.

**How to apply:** before committing evidence, run `git ls-tree -r --name-only HEAD
evidence/ | wc -l` and `git merge-tree --write-tree main HEAD` — the b5edbea policy is
"local evidence only, never merged to main", so both must read 0 / carry 0 evidence
paths (the full set stays on disk and retrievable via `git show <commit>:<path>`).
Count suite greenness from the ctest add_test registry (`git show <rev>:CMakeLists.txt
| grep -o 'add_test(NAME …'`), never from a tests/ directory diff. Treat ui-bench G5
mallinfo2 warnings as disclosed-open only when the delta equals the baseline's own
number (here 1072 B = 1072 B); any other warning is a finding. When a card's expected
test count disagrees with your run, explain the delta before recording it — an
unexplained count is how a missing test hides. If a concurrent workflow session may be
running, give it its own evidence namespace; check mtimes before quoting on-disk file
counts. The g-latency WARN family is host-load-dependent: on a serialized quiet host it
passes at p95 ~24 ms — do not treat the corroboration-time WARN as a defect.

Related: [[lumen-unit-pair-green]] (merged sibling; the /evidence/ gitignore policy and
the 133 count), [[lumen-memory-pressure-green]] (base branch; leg 5's zero-critical
acceptance), [[lumen-vulkan-capture-green]] (the Z portal rows and absolute
LUMEN_BUILD_DIR), [[lumen-smoke-bench-harness-green]] (smoke_harness — the 134th test —
and the holder/injection recipe), [[lumen-notebook-hw]] (i915+nouveau host),
[[lumen-ui-bench-noise-and-g7]] (G5 open gate + warm-up discipline),
[[lumen-toplevel-drag-compositor]] (the g-latency family that passed here).
