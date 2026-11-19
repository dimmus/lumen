---
name: lumen-unit-pair-green-closeout
description: "Close-out chronology split from [[lumen-unit-pair-green]] — every dated re-run, merge-tree proof and gate re-certification, byte-preserved."
metadata:
  node_type: memory
  type: project
---

## 2026-11-15 close — final gate at b5edbea, review 4/4 dispositioned, merge safety proven (branch tip and every command below re-run by the memory writer this session)

- **Task closed; the branch tip is now b5edbea** ("evidence: untrack the gate captures
  and ignore /evidence/ so a merge cannot carry them"), 7 commits over merge-base
  fa11b38. Re-verified: `git ls-files -- evidence/` → **0**; `git check-ignore -v
  evidence/smokes/gate/drag-smoke-logs/run.log` → `.gitignore:14:/evidence/` (comment at
  :13 "local evidence only, never merged to main"); `git diff-tree -r b5edbea` = only
  `.gitignore` M + evidence/ D; and `git diff --stat 6b7c81e b5edbea -- .zcode/ tests/
  modules/ docs/` is **empty**, so the fix-round delta touches no compiled or test input —
  every suite/golden number above carries to the tip bit-for-bit.
- **Merge carries no evidence, proven not assumed:** `git merge-tree --write-tree main
  zcode/task/unit-pair-green` → tree **7d15912**, exit 0 (no conflicts); `git ls-tree -r
  7d15912 --name-only -- evidence/` → **0** files while the two goldens,
  tests/test_task_model.cpp and .gitignore are present; main never tracked evidence/
  (`ls-tree`/`log -- evidence/` both empty). Nuance the b5edbea title oversells: "cannot
  carry them" is true of the **merged tree** — the 5efebfc/6b7c81e blobs stay reachable
  through merged history, which is by design what keeps the valid Nov-14 PASS retrievable.
- **Independent review (final round): 4 findings confirmed, 0 refuted, all
  dispositioned.** (1, medium) the degraded zero-input 6b7c81e re-capture at tip + the
  unenforced merge drop → fixed by b5edbea (verified above); (2, medium) the
  golden-substitution acceptance → resolved by the recorded owner decision fdd75f2
  (acceptance at lumen-unit-pair-green.md:118 of that commit, owed VM check at :124);
  (3, low) 4bcf390's message cites the orphaned 42c2aa3 — deliberately left as an owner
  call (rewording = another history rewrite); trees verified equal
  (`42c2aa3^{tree}` = `e29ba04^{tree}` = 7dbac1d7…, `git branch --contains` empty);
  (4, low) the reviewer's own `ctest -N` dry-run **overwrote the gate-run LastTest.log**
  the handoff cited (a dry run still rewrites `build/test/Testing/Temporary/`); their
  full-suite rerun then rewrote it with a passing record — the durable evidence (memory
  commit 8abd92f on main) was never at risk.
- **Final gate numbers (gatekeeper-run at b5edbea, from the close packet — not re-run by
  the memory writer):** `ctest --test-dir build/test --output-on-failure -LE bench` →
  "100% tests passed out of 133", 75.95 s, exit 0 (ninja no-op, LUMEN_BUILD_TESTS=ON);
  pair `ctest -R 'test_task_model|ui_gallery'` → 2/2; test_task_model → 24 tests 0
  failed; `lumen-ui-gallery --check` → cpu 0/255 both themes, gl 1/255. Known-red
  retirement re-checked **per tree** (re-run by me): the branch tree has zero
  `ui_gallery` mentions in `.zcode/` outside `memories/` — SKILL.md:59-61 says "the dev
  tree carries none … both fixed on 2026-11-14" and only the generic `known_red` default-`[]`
  mechanism text remains — but **main (fdd75f2) still carries the stale example**
  (SKILL.md:60 "carries `ui_gallery` + `test_task_model`", .dwf.ts:32
  `["ui_gallery","test_task_model"]`) until the owner merges; 4bcf390 travels with the
  branch. The owed VM golden check is recorded **only here** — `.zcode/TODO.md` has no
  unit-pair row. *(Superseded the same day: 255e757 adds the row — post-close section below.)*
- **Acceptance caveat the owner owns:** the card's "host-independent evidence" is met by
  construction (pinned bundled font/icons/hinting, tests/ui_golden.hpp:65-73) plus
  this-host determinism — not by a cross-host render proof; the system FreeType/HarfBuzz
  version remains the one unpinned input.

## 2026-11-15 post-close — review FIXED AND CLEAN, owed VM check tracked in .zcode/TODO.md, gate READY re-run at final tip 255e757 (branch tip and every command below re-run by the memory writer this session)

- **Post-close review round: verdict FIXED AND CLEAN, 1 confirmed / 0 refuted.** The
  confirmed finding (low): the owed VM golden check — the only open condition of the
  owner's golden-substitution acceptance — was tracked **only in this memory**;
  `grep -n -i 'unit-pair|ui.gallery|ui_gallery|golden'` over main's 93-line
  `.zcode/TODO.md` returned nothing, so the next VM session had no actionable reminder
  before the branch merges. **Fixed by 255e757** ("docs(zcode): track the owed VM
  ui-gallery golden check in TODO", 2026-11-15 11:50 +0500): exactly `.zcode/TODO.md`
  +7 lines, an open `- [ ]` row in the Now section (`:18-24`, before the Host-limited
  gates section) carrying the exact command (`./build/test/lumen-ui-gallery --check`),
  where to run it (dev VM, tests-enabled build of the unit-pair-green tree "or main
  once it merges"), the expected result (cpu 0/255 both themes, gl ≤ 3 vs CPU per G10),
  the closing action (record the result, check the row off), and the failure
  disposition (a new FreeType-version-sensitivity finding, "not a regression of the
  substitution"). The commit honestly states "Not run this session: the check itself" —
  this host is the notebook.
- **Gate READY re-verified at the new tip 255e757** (notebook, nouveau card0 + i915
  card1 re-read from `/sys/class/drm/card*/device/driver`): `cmake --build build/test
  -j8` → "ninja: no work to do" (255e757 touches no compiled file); suite definition
  `ctest -N -LE bench` → Total Tests: **133** with `LUMEN_BUILD_TESTS:BOOL=ON` in
  CMakeCache (the gating build); `ctest --test-dir build/test --output-on-failure -LE
  bench` → **"100% tests passed out of 133"**, 76.85 s; pair
  `ctest -R 'test_task_model|ui_gallery'` → 2/2 (0.35 s); `./build/test/lumen-ui-gallery
  --check` → cpu max **0/255** both themes, gl **1/255** vs CPU (G10 ≤ 3), exit 0.
- **Merge safety re-proven at the new tip:** `git merge-tree --write-tree main
  zcode/task/unit-pair-green` → tree **44479c2**, exit 0; main has not touched
  `.zcode/TODO.md` since fa11b38 (`git log fa11b38..main -- .zcode/TODO.md` empty) and
  only 255e757 touches it on the branch, so the row merges without conflict and its
  citation of `.zcode/memories/lumen-unit-pair-green.md` resolves post-merge (that file
  lives on main, absent from the branch — inherent to memory-on-main). Branch **not yet
  merged** (`merge-base --is-ancestor` fails) and main's SKILL.md:60 still carries the
  stale known-red pair — the branch is the truth until merge. Net branch diff outside
  evidence/ is now **7 files** (adds .zcode/TODO.md +7), still zero compiled code.
- **New low wart (left as-is under the 42c2aa3 precedent):** 255e757's message cites a
  "Gate re-verify" entry of this memory file that exists nowhere (`grep -c 'Gate
  re-verify'` → 0; the real entries are the dated refresh/close sections and the
  "Owner decision" bullet) — message-only, fixable only by another history rewrite of
  the unmerged branch, an owner call like the orphaned-hash wart.

## 2026-11-15 final gate — review fully clean (0 findings), READY re-certified at 255e757 after main advanced; the packet's seat/bench reds audited to harness/baseline noise (suite numbers re-run by the memory writer this session)

- **Final gate verdict: READY at 255e757 — certifies this gate only, not permission to
  merge, publish or push.** Branch still unmerged (`git merge-base --is-ancestor
  255e757 main` fails; main tip cf05213, advanced past 44479c2 by the intervening
  memory commits).
- **Independent review's final round: 0 confirmed / 0 refuted** (verdict "Ready for
  gate/merge") after the post-close round's single finding was fixed by 255e757; its
  distinctive claims were spot-verified by the gate and re-run by me: 133/133;
  ui-gallery cpu 0/255 / gl 1/255; `42c2aa3^{tree}` = `e29ba04^{tree}` = 7dbac1d7
  (`git rev-parse`); `git status --porcelain` empty; zero `ui_gallery` mentions in
  branch `.zcode/` outside `memories/` (SKILL.md:59 "the dev tree carries none").
- **Final suite numbers (re-run by me):** `cmake --build build/test -j8` → ninja no-op
  (LUMEN_BUILD_TESTS=ON); `ctest -R 'test_task_model|ui_gallery'` → 2/2 (0.34 s);
  `./build/test/test_task_model` → 24 tests, 0 failed; `./build/test/lumen-ui-gallery
  --check` → cpu **0/255** both themes, gl **1/255** vs CPU (G10 ≤ 3), exit 0; full
  `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed
  out of 133"**, 75.66 s, exit 0 (the gate's own run: 76.39 s).
- **Merge safety re-proven against the moved main:** `git merge-tree --write-tree main
  zcode/task/unit-pair-green` → tree **592ede7**, exit 0, `git ls-tree -r 592ede7 --
  evidence/` → **0** files (supersedes 44479c2, which the main advance obsoleted).
  Engine/compositor binary stays code-identical to main's: `git diff --stat
  5efebfc..HEAD -- . ':!evidence'` = `.gitignore` +3, `.zcode/TODO.md` +7, nothing
  else — so the gate-time seat/bench outcomes below cannot be regressions of this
  change.
- **The packet's three gate-time reds audited to not-this-change** (gatekeeper audit of
  the committed logs, consistent with the close section): the drag-smoke exit 1 was
  **injector misconfiguration** — run.log shows `/dev/input/event2` bound ("has no
  absolute axis 0"), inject.jsonl 2 lines, every delta +0,+0 n=0/0, so zero input
  reached the scenarios while the compositor-health row still PASSes
  (errors+warnings=0, exit=0); the notebook recipe requires the virtual ABS mouse from
  `scripts/uinput-mouse.py`. The gate-time app-smoke **timed out** (partial logs
  16:01–16:16, no verdict) → contributes no evidence; the complete 5efebfc run
  (142 PASS / 34 FAIL, drag PASS with 603 real inject lines, latency n=150/150 median
  10.3/15.0 ms) matches the documented pre-REL families of the unmerged
  [[lumen-smoke-bench-harness-green]]. The ui-bench exit 1 is **both-direction drift
  vs the 2026-11-04 baselines** (tests/bench/results/iritur-08fd3c-none/
  20261115-255e757-2.json vs the 97f59a4 baseline: S1 update −63% "improved" while S4
  layout +986%) — impossible for a code-identical binary and exactly the
  unreproducible-baseline signature of [[lumen-ui-bench-noise-and-g7]]. Per §1.2 no
  A/B regression of this change exists to take to the user.
- **The owed VM golden check stays open where it belongs:** the row lives on the branch
  at `.zcode/TODO.md:18-24` (255e757); main's `.zcode/TODO.md` does not carry it until
  the branch merges (on main tip cf05213 lines 14-26 go straight from the ADR-013 rows
  to "## Host-limited gates").


## 2026-11-15 terminal handoff — task finished, gate READY re-confirmed at main 700f7f5; every number below re-run by the memory writer this session

- **The workflow's closing packet certifies gate READY at 255e757 and nothing moved on
  the branch:** tip still 255e757, `git status --porcelain` empty, host re-identified as
  the notebook (nouveau card0 + i915 card1 via `/sys/class/drm/card*/device/driver`).
  The branch remains **unmerged** (`git merge-base --is-ancestor 255e757 main` fails);
  the only open item is still the owed VM golden check, carried by the branch at
  `.zcode/TODO.md:18-24` (re-read this session: exact command `./build/test/
  lumen-ui-gallery --check`, dev-VM venue, cpu 0/255 both themes expected, gl ≤ 3 per
  G10, failure disposition = new FreeType-version-sensitivity finding, not a
  regression of the substitution).
- **Suite re-run at the tip (all first-hand):** `cmake --build build/test -j8` → ninja
  no-op with `LUMEN_BUILD_TESTS:BOOL=ON` in CMakeCache; `ctest --test-dir build/test
  --output-on-failure -LE bench` → **"100% tests passed out of 133"**, 75.66 s, exit 0
  (last test index #138 confirms 138 registered / 133 executed under `-LE bench`);
  `./build/test/test_task_model` → 24 tests, 0 failed, exit 0;
  `./build/test/lumen-ui-gallery --check` → cpu **0/255** both themes (1808 primitives
  each), gl **1/255** vs CPU (G10 ≤ 3), exit 0. This re-run rewrote the worktree's
  `Testing/Temporary/LastTest.log` a **4th** time — the gate's cited copy (Nov 15
  17:00–17:01, 133 Passed / 0 Failed) was captured before the rewrite; the numbers live
  in this record, never in that log.
- **Merge safety re-proven against the moved main:** main tip is now **700f7f5**
  (advanced past cf05213 by the [[lumen-smoke-bench-harness-green]] post-close memory
  commit; main's delta since merge-base fa11b38 is memory commits only, zero
  non-memory commits), so `git merge-tree --write-tree main
  zcode/task/unit-pair-green` → tree **f52c647**, exit 0; `git ls-tree -r f52c647 --
  evidence/` → **0** files while `.gitignore`, `.zcode/TODO.md`, both goldens and
  `tests/test_task_model.cpp` are present (supersedes 592ede7 vs cf05213). Branch
  delta outside evidence/ re-measured: `git diff --stat fa11b38..255e757 -- . 
  ':!evidence'` → **7 files, +20/−10, zero compiled code** — the engine binary stays
  code-identical to main's, so no seat/bench outcome can be this change's regression.
- **All six stored bench JSONs decoded, not taken on faith**
  (`tests/bench/results/iritur-08fd3c-none/`: 20261114-de92ce6, 20261115-6b7c81e,
  20261115-b5edbea, 20261115-255e757, -2 and -3): every file reports scenarios S1–S4
  with **2 gates each, all-pass** (git field spot-checked = 255e757 in -3.json) — the
  gate-time ui-bench exit-1 is confined to baseline comparison against the 2026-11-04
  baselines that [[lumen-ui-bench-noise-and-g7]] proved unreproducible by their own
  binary. No §1.2 A/B owed.
- **Known-red retirement re-verified per tree:** branch SKILL.md:59-61 "the dev tree
  carries none … both fixed on 2026-11-14", `lumen-development-studio.dwf.ts:176`
  builds `known_red` from args (defaults `[]`), and a grep for `ui_gallery` over
  `.zcode/` excluding `memories/` returns nothing on the branch. Main's SKILL.md:60
  still shows the stale pair example until the merge — the branch is the truth.

## 2026-11-15 closing-packet re-check at main b5b58b0 — nothing moved on the branch; every number below re-run by the memory writer this session

- **This ask's packet (implementation + review APPROVE 0 confirmed / 0 refuted +
  gate_runs + gate verdict READY) adds no new claims** — its content is recorded by the
  2026-11-15 final-gate and terminal-handoff sections above; this section re-verifies at
  the current state. Branch: tip still **255e757**, `git status --porcelain` empty,
  **unmerged** (`git merge-base --is-ancestor 255e757 main` rc=1); main tip is now
  **b5b58b0**, advanced past 700f7f5 by the [[lumen-vulkan-capture-green]] and
  [[lumen-memory-pressure-green]] memory commits — `git log fa11b38..main --oneline |
  grep -cv 'memory:'` → **0**, so main's delta since merge-base fa11b38 is still memory
  commits only. Host re-identified as the notebook (nouveau card0 + i915 card1 via
  `readlink /sys/class/drm/card*/device/driver`).
- **Suite re-run at the tip (all first-hand):** `LUMEN_BUILD_TESTS:BOOL=ON` in
  build/test/CMakeCache; `cmake --build build/test -j8` → ninja no-op;
  `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed
  out of 133"**, 75.66 s (acceptance #1); `./build/test/test_task_model` → 24 tests,
  0 failed; `./build/test/lumen-ui-gallery --check` → cpu **0/255** both themes
  (1808 primitives each), gl **1/255** vs CPU (G10 ≤ 3), exit code **0** captured
  directly (`echo $?` = 0). This run rewrote the worktree's
  `Testing/Temporary/LastTest.log` a **5th** time (mtime 2026-11-15 23:34:50 +0500) —
  the numbers live in this record, never in that log.
- **Merge safety re-proven against the moved main:** `git merge-tree --write-tree main
  zcode/task/unit-pair-green` → tree **1f4beed**, exit 0 (supersedes f52c647 vs
  700f7f5); `git ls-tree -r 1f4beed --name-only -- evidence/` → **0** files while all
  seven change files are present (root `.gitignore`, `.zcode/TODO.md`,
  lumen-build-run/SKILL.md, studio `.dwf.ts`, both goldens,
  `tests/test_task_model.cpp`). Branch delta re-measured: `git diff --stat
  fa11b38..HEAD -- . ':!evidence'` → **7 files, +20/−10**, zero compiled code — the
  engine binary stays code-identical to main's.
- **Known-red retirement and the owed check re-read per tree:** branch
  SKILL.md:58-61 "the dev tree carries none … both fixed on 2026-11-14" while **main's
  copy (:58-61) still shows the stale `ui_gallery` + `test_task_model` example** until
  the merge; `lumen-development-studio.dwf.ts:176` builds `knownRed` from args
  (defaults `[]`); the owed VM golden check is still the open `- [ ]` row at branch
  `.zcode/TODO.md:18-24`.
- **Bench results re-decoded, not taken from the packet's bare "bench: run (see
  findings)" row:** **seven** local JSONs now under `tests/bench/results/iritur-08fd3c-none/`
  — a `20261115-255e757-4.json` exists beyond the six the terminal handoff recorded;
  all seven decode to scenarios S1–S4 with 2 gates per scenario (the scenario's own
  G1–G4 plus G5), **all 'pass'**, git fields matching their filenames. All seven are
  local-only by design: `tests/bench/results/.gitignore` rule `*` ignores them
  (`git check-ignore -v` verified; only that .gitignore is tracked), so the new file
  is not a tree change and no merge carries it. The gate-time ui-bench exit-1 stays
  confined to the 2026-11-04 baseline comparison ([[lumen-ui-bench-noise-and-g7]]) —
  no §1.2 A/B regression owed.

**Why:** a tolerated-red set outlives its cause — a65d06b landed Nov 11 and the stale
checks sat red-tolerated for three days while verifying nothing, and "golden drift" got
blamed on hosts when the goldens were simply older than a reviewed fix. Tolerance without
a retirement task turns every future suite run into archaeology, and the first reflex on
golden failure should be `git log` on the golden, not font config. And a record written
before the merge is provisional: the studio's local rewrite orphaned four of the six
hashes this page first cited, and its strongest host-independence claim was one
unrecorded provenance away from false — refresh the record at close. And an owed
condition accepted by the owner must live in the tracker, not only in a memory —
memories are history a session has to know to search; `.zcode/TODO.md` is where the
next VM session actually looks first (the review confirmed this gap before the merge).

**How to apply:** `known_red` is empty once the branch (tip 255e757) merges — any suite
red is reportable immediately (until then main's SKILL.md still shows the stale pair
example; the branch is the truth); do
not re-tolerate without an owner decision plus a retirement task. At the next dev-VM
session run `./build/test/lumen-ui-gallery --check` (tests-enabled build) and check off
the "Owed VM golden check (unit-pair-green)" row in `.zcode/TODO.md` — a VM failure is
a new FreeType-version-sensitivity finding, not a regression of the golden
substitution. When a golden test goes
red: localize the pixel diff to blobs first, then `git log --follow
docs/images/<golden>.png` and compare the last-regeneration commit against commits
touching the rendered widget code before suspecting fonts/hinting/hosts. When a change
removes a struct field or changes a geometry constant, grep the test file for expectations
*derived* from it, not just its name (`armed` vanished; the count math and the diff inputs
derived from it silently). Logout semantics today: `panel_logout` is `{shown, pressed}` —
no armed two-click field; damage tests press via `pressed` and expect exactly 1 rect.
Cross-host golden claims need full-image bit-agreement evidence **with the provenance of
both runs recorded** (which host wrote each side) — same-host agreement across time is
determinism, not cross-host stability. Records written mid-run cite hashes that the
studio's pre-merge local rewrite can orphan: at close, re-check the branch tip and
refresh the record (or record the tree-equivalence, `git diff <old> <new>` empty, that
keeps the evidence reachable). Never overwrite a committed evidence run with a worse
re-run — append or replace the README/verdict pointers too. Never cite a worktree's
`Testing/Temporary/LastTest.log` as durable evidence: even a `ctest -N` dry run rewrites
it — commit the numbers (or rely on the memory record) instead.

Related: [[lumen-toplevel-drag-protocol-registration]] (origin of the tolerance),
[[lumen-toplevel-drag-compositor]], [[lumen-toplevel-drag-drag-smoke]],
[[lumen-smoke-bench-harness-green]] (unmerged harness fixes; ui-bench baseline story),
[[lumen-close-ghost-fix]], [[lumen-panel-blur-fix]] (both recorded the same pair as
tolerated), [[lumen-notebook-hw]], [[lumen-app-smoke-known-noise]],
[[lumen-ui-bench-noise-and-g7]], [[lumen-no-degradation-rule]].
