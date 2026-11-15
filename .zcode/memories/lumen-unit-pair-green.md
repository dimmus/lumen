---
name: lumen-unit-pair-green
description: "2026-11-14/15 notebook task (worktree unit-pair-green, rewritten in place before merge to 1ca2bf9..6b7c81e, gate READY, 133/133 re-run both days): the owner-tolerated known-red pair retired with zero production code — test_task_model's two CHECKs were stale expectations left by a65d06b (logout popup shrank k_logout_width 84→28 and dropped panel_logout.armed; count 5/5 → 6/4 via (360−44)/52; the arming check compared identical inputs so panel_diff was 0 → now drives `pressed`, exactly 1 damage rect; 3917292 cells[l.count−1] rightmost bound); ui_gallery was NOT font-rasterization host drift — the goldens (2c443b0, Nov 5) predate the select-chevron attach fix 1a44c01 (Nov 8); diff = three 8×6 px chevron blobs (72/76 px, max 111/130 per 255), GL-vs-CPU ≤1/255; goldens regenerated (e29ba04, tree-identical to the orphaned 42c2aa3) — the 'VM-era cross-host bit-agreement 1,320,728/1,320,800 px' claim is RETRACTED: the regold host of 2c443b0 is recorded nowhere, the agreement proves this-host determinism only; 4bcf390 cleared the SKILL.md known-red note — known_red is now empty; the 6b7c81e smoke re-capture overwrote the Nov-14 committed drag-smoke PASS with a zero-input FAIL run (PASS survives at 5efebfc); owner ACCEPTED the golden substitution (2026-11-15) — cross-host check owed: run ./build/test/lumen-ui-gallery --check on the dev VM at the next VM session and record the result; fix-round b5edbea untracked evidence/ and added /evidence/ to .gitignore so the merge carries it nowhere — task CLOSED 2026-11-15: merge-tree proven to carry 0 evidence files, review 4/4 confirmed-all-dispositioned, then post-close review FIXED AND CLEAN (1 confirmed: the owed VM check was memory-only) fixed by 255e757 — the open condition is now an open `- [ ]` row at .zcode/TODO.md:18 (./build/test/lumen-ui-gallery --check on the dev VM, check off after recording); final tip **255e757** (8 commits over fa11b38, unmerged as of 2026-11-15), gate READY re-verified there 133/133 (76.85 s), ui-gallery cpu 0/255 both themes / gl 1/255, merge-tree main→44479c2 clean"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-14 on the **notebook** (nouveau card0 + i915 card1 re-read from
`/sys/class/drm/card*/device/driver` this session). Worktree
`/home/dimmus/lumen-wt/unit-pair-green`, branch `zcode/task/unit-pair-green` off main
fa11b38; tip **6b7c81e** (2026-11-15 00:33; later moved to **b5edbea** — see the close
section below; final tip **255e757** — see the post-close section). The studio rewrote the branch locally before
merge (never pushed): 42c2aa3→**e29ba04**, 75bf41d→**3917292**, f10183a→**4bcf390**,
de92ce6→**5efebfc** — trees identical, only e29ba04's message changed (see the
2026-11-15 section); 1ca2bf9 unchanged. Gate **READY**. Branch diff contains **zero
production code** (the test file, two golden PNGs, two `.zcode` doc lines, `evidence/`
logs). Every headline number below re-run by the memory writer (2026-11-14 and again at
final HEAD on 2026-11-15) unless marked implementer/reviewer-sourced.

- **The tolerance ends here.** The pair {ui_gallery, test_task_model} was owner-tolerated
  since Nov 12 ([[lumen-toplevel-drag-protocol-registration]], [[lumen-toplevel-drag-compositor]],
  [[lumen-toplevel-drag-drag-smoke]]); every record since repeated "131/133 known-red".
  This task fixed both, and f10183a rewrote `.zcode/skills/lumen-build-run/SKILL.md:59-62`
  to "the dev tree carries none … both fixed on 2026-11-14" plus the same stale example in
  `.zcode/workflows/lumen-development-studio.dwf.ts:32` (description string only). Grep
  over `.zcode/` excluding `memories/`: zero remaining ui_gallery mentions. The studio's
  `known_red` arg defaults to `[]` and stays that way.
- **test_task_model root cause: a production change updated test inputs but not the
  expectations derived from them.** a65d06b ("Replace panel logout label with popup",
  2026-11-11) rewrote ~83 lines of panel.cppm but only 8 in the test (re-counted from the
  diff): `k_logout_width` 84→28 (`modules/lx.shell/panel.cppm:51`) and panel_logout lost
  its third field `armed` → `{shown, pressed}` (`panel.cppm:170-173`). The stale checks:
  `count == 5 && hidden == 5` (the 28 px icon leaves entries 360 px: `(360−44)/52 = 6`
  shown, 4 hidden) and `panel_diff == 1` for an "arming" change that compared
  `panel_logout{true}` with `{true, false}` — identical inputs since the field was gone,
  so the diff was always 0 and the test verified nothing. 1ca2bf9 fixed the expectations
  and renamed the case to `pressing_the_logout_button_damages_only_the_button`, driving
  `pressed` — `panel_diff` (`panel.cppm:1413-1415`) returns exactly 1 rect at the button
  for a flags-only change, so the new expectation is strictly stronger.
- **Review round 75bf41d:** with count now 6, the adjacent `cells[4]` bound no longer
  covered the rightmost visible entry → `cells[l.count − 1]` (`tests/test_task_model.cpp:274`).
  Safe: `LUMEN_CHECK` throws on failure (`tests/lumen_test.hpp:65-72`, caught per-case
  :40-48), so the index is evaluated only after `count == 6` holds one line above.
- **ui_gallery root cause: golden staleness, not host drift** (the card's suspicion —
  disproven). The goldens were last written at 2c443b0 (2026-11-05, settings restyle);
  the select trigger's chevron is only rendered since 1a44c01 (2026-11-08), whose diff
  shows the old code building `chevron` and returning `control(c, kids, key)` without it.
  The failing diff localized to three 8×6 px blobs (x 591-598; y 1062-1067 / 1138-1143 /
  1176-1181 — pane 9's open/disabled/closed select rows): dark 72 px max 111/255, light
  76 px max 130/255, while GL agrees with CPU to ≤1/255 on both themes — structural
  absence vs presence of the glyph, not antialiasing or font drift. 42c2aa3 regenerated
  both goldens (1040×1270 each).
- **Golden-agreement evidence (its cross-host reading retracted 2026-11-15)** (implementer's
  libpng measurement, confirmed by the gate's independent decode): the pre-fix goldens
  match this host's render bit-for-bit on **1,320,728 of 1,320,800**
  pixels except the three chevron blobs; inputs are pinned (bundled IBM Plex Sans +
  tests/fonts via `LUMEN_TEST_FONT_DIR`, tests/ui_golden.hpp:73, bundled Lucide icons);
  `test_ui_golden` is bit-exact incl. chevron_down at 14 px and the text
  raster/shaping tests pass here. Regeneration-with-evidence was the card's allowed
  alternative since the inputs were already pinned and proven stable. What this does
  **not** prove: cross-host stability — which host wrote 2c443b0's PNGs is recorded
  nowhere, and the rewritten e29ba04 message states exactly that.
- **Suite (re-run by me):** `cmake --build build/test -j2` no-op →
  `./build/test/test_task_model` "24 tests, 0 failed"; `./build/test/lumen-ui-gallery
  --check` max channel error **0/255** vs both new goldens, GL 1/255 vs CPU, exit 0;
  `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed out
  of 133"**, 75.78 s, exit 0 (was 131/133 with the pair red).
- **Gate-time seat smokes (de92ce6, audited from committed logs):** prechecks stamped
  2026-11-14T20:26:59+0500 with no compositor running; drag-smoke **VERDICT: PASS**;
  app-smoke exit 1 = **142 ^PASS / 34 ^FAIL / 1 ^WARN** (re-counted) — the documented
  pre-existing base-tree state, not a regression: this worktree carries the pre-fix
  harness (the notebook fixes sit on the still-unmerged [[lumen-smoke-bench-harness-green]]
  branch) and the R4/W1/W2/W4 rows match that record's pre-REL uinput-holder trap; the
  remaining FAILs include the owner-owned Z/PSI/vulkan-capture families. ui-bench exit 1
  (`tests/bench/results/iritur-08fd3c-none/20261114-de92ce6.json`) shows all G1-G5 gates
  passing with the failure confined to comparing against the 2026-11-04 baselines that
  record proved unreproducible by their own binary (both-direction swings, S1 p50 −89% /
  S2 tick_idle +327%, impossible for a code-free diff). Neither red is this task's, and
  neither is an A/B regression — no no-degradation decision owed.

**Disproven (closed; do not re-chase):** "font-rasterization drift between hosts" as the
ui_gallery cause — the render is deterministic on this notebook and the old goldens agree
bit-for-bit outside the chevron blobs; the goldens were simply older than a reviewed fix.
Also closed: "GL-vs-CPU disagreement means an engine bug" — GL matches CPU to ≤1/255 both
themes (engine self-consistent, G10 tolerance ≤3). **Retracted 2026-11-15:** "the old
goldens are VM-era, so the bit-agreement proves cross-host stability" — the host that
wrote 2c443b0 is in no git metadata or contemporaneous memory record; agreement across
time on one host proves determinism, not cross-host stability.

## 2026-11-15 refresh — branch rewrite, provenance retraction, final gate (all verified this session)

- **The studio rewrote the branch in place before merge; it was never pushed**
  (`git ls-remote origin` for `refs/heads/zcode/task/unit-pair-green` and `main` returns
  only `refs/heads/main` 0394e7c). Hash map — each pair verified with `git diff <old>
  <new>` = empty plus a message compare: 42c2aa3→**e29ba04**, 75bf41d→**3917292**,
  f10183a→**4bcf390**, de92ce6→**5efebfc** (tree- and message-identical), plus new
  **6b7c81e** (00:33, re-captured gate smokes, evidence-only over 5efebfc); 1ca2bf9
  unchanged. The goldens PNGs are the same git objects in both commits (`ls-tree`:
  c5877bd one-dark / 02abc84 one-light), so every pixel measurement above carries over.
  The rewrite's only content change is e29ba04's message: "the VM-era goldens … identical
  code renders identically across the two hosts" → "the committed goldens match this
  host's render bit for bit" plus an explicit provenance caveat ("which host wrote
  2c443b0's PNGs is not recorded … this message does not claim it") and "bit-stable
  across hosts" → "bit-stable on this host; cross-host stability is not established".
  This was review finding 1 (low); the retraction is now applied in this record too.
- **Gate verdict: READY at 6b7c81e.** Re-run by me this session (notebook, nouveau card0
  + i915 card1 re-read from `/sys/class/drm/card*/device/driver`): `cmake --build
  build/test -j8` → "ninja: no work to do" (LUMEN_BUILD_TESTS=ON);
  `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed out
  of 133"**, 75.49 s; `test_task_model` → 24 tests, 0 failed; `lumen-ui-gallery --check`
  → cpu max **0/255** both themes, gl **1/255** vs CPU (G10 ≤ 3), exit 0; and
  `--check --dir /tmp/oldgoldens-unitpair` (merge-base fa11b38 goldens extracted via
  `git show`) reproduces the failure signature exactly — "111/255 … 72 pixels differ" /
  "130/255 … 76 pixels differ" — so the golden-vs-history story is reproducible from the
  tree. The gate's independent PNG decode: old vs new goldens differ on exactly 72/76 px
  (max 111/130), three 8×6 px blobs at x[591..598] on rows 1062/1138/1176, all other
  1,320,728/1,320,724 of 1,320,800 px bit-identical (gatekeeper-run, matches Nov-14).
- **Owner decision (2026-11-15 fix round): the golden-regen substitution is ACCEPTED**
  as meeting the ui_gallery acceptance — proven stale-golden root cause, every pinnable
  input pinned in-repo (bundled font/icons, in-code hinting, bit-exact CPU compare), and
  bit-exact this-host determinism (0/255 both themes, re-run by the fixer); the old
  goldens never controlled the FreeType/HarfBuzz build either, so a cross-host proof is
  not held against the fix (this notebook session cannot produce a VM render anyway).
  **Cross-host check owed:** run `./build/test/lumen-ui-gallery --check` on the dev VM at
  the next VM session and record the result — a failure there is a new finding about
  FreeType-version sensitivity, to be handled then.
- **Branch delta outside evidence/ is 5 files, zero compiled code** (`git diff
  fa11b38..HEAD --stat -- . ':!evidence'`: SKILL.md 8 lines, lumen-development-studio
  .dwf.ts 1 line, the two PNGs, tests/test_task_model.cpp 10 lines) — no seat/bench
  outcome can be a regression of this change.
- **The 6b7c81e smoke re-capture overwrote better evidence (flagged to the owner).** The
  Nov-14 evidence tree (5efebfc ≡ de92ce6) holds a genuine drag-smoke **PASS** — all 10
  scenario rows green with real input (e delta −110,−74 exact; g latency n=150/150,
  median 10.3/15.0 ms, p95 17.2/25.1 ms; h 90 configures for 160 motions; j
  xdg-toplevel-drag +240,+160 with 36 updates) — and a complete app-smoke run (142 PASS /
  34 FAIL rows / 1 WARN, VERDICT: FAIL, the documented pre-REL-harness family of the
  unmerged [[lumen-smoke-bench-harness-green]] fixes; counts re-run from the committed
  run.log). 6b7c81e then modified the evidence in place (168 M / 43 D files under
  evidence/): the drag re-run bound `/dev/input/event2` — "no absolute axis 0", 13
  inject-input errors — so **zero input reached the scenarios** (all deltas +0,+0,
  n=0/0), every scenario row FAILs and only the compositor-health row passes
  ("errors+warnings=0 budget-summaries=0 compositor exit=0 console=text"), VERDICT: FAIL;
  the app-smoke re-run committed **no verdict rows** (run.log 243 lines, zero PASS/FAIL
  tokens; compositor.log has 0 `[ERROR]` lines; clean shutdown per the gate). This
  repeats the overwrite anti-pattern of [[lumen-smoke-bench-harness-green]] round 3 —
  the good runs survive only at 5efebfc in git history while the gate reads the tree
  tip. All prechecks in both snapshots say "ok: no lumen-compositor" + "WARN: a smoke is
  already running" (the owner's live session; re-running without evicting it was not
  available).
- **Merge cautions (non-blocking):** evidence/ is **74 MB** (`du -sh`) and main has never
  carried an evidence/ commit (`git log main -- evidence/` is empty) — the merge must
  not carry 5efebfc/6b7c81e. **Enforced 2026-11-15 (b5edbea):** the fix round untracked
  evidence/ on the branch (`git rm -r --cached`) and added `/evidence/` to .gitignore —
  `git ls-files evidence/` empty, `git ls-tree -r main --name-only -- evidence/` empty,
  so a plain merge carries nothing; the valid Nov-14 captures stay in history at 5efebfc.
  The pre-refresh version of this record cited the orphaned
  hashes and the retracted claim (fixed by this section).

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
