---
name: lumen-settings-written-banner-escalation
description: "2026-11-25 notebook: the settings banner's written-state gap CLOSED (commit 1bc8b2e, branch zcode/task/settings-banner-written-state off 87327dd, gate READY, not on main when written) — after one successful write a later unwritable edit was applied to the session with no banner, because repaint() escalated only untouched→memory_only and the banner gate reads memory_only alone; the confirm-first probe at base read 'state=1 banner=0'; fix: repaint() escalates from any state != memory_only (all five write-failure tails now raise from written); (f')/(g') write-then-fail legs red-proven at test_settings_ui.cpp:468; ctest 141/141 -LE bench (146 registered); noise 3/3/0/3"
metadata:
  node_type: memory
  type: project
summary: "banner escalates from `written` too (1bc8b2e, READY): repaint() gates on != memory_only; probe 'state=1 banner=0' at base; (f')/(g') legs red at :468; ctest 141/141; noise 3/3/0/3"
modules: executables/lumen-settings, tests
topics: banner, save-state, settings, review-noise

---

Verified 2026-11-25 on the **notebook** (iritur i915+nouveau, [[lumen-notebook-hw]]). Task
`settings-banner-written-state`, worktree `/home/dimmus/lumen-wt/settings-banner-written-state`,
branch `zcode/task/settings-banner-written-state` off 87327dd, HEAD **1bc8b2e** (one commit,
+25/−4 over exactly the card's two files), gate READY, branch **not on main** when written. I
re-verified in this session: the diff (fix + test legs), no base↔main drift for either file
(`git log 87327dd..main -- <files>` empty), the fix condition at
executables/lumen-settings/settings_ui.cppm:1261, the banner gate at :2430-2431, and the five
`repaint();` callers (:1271, :1290, :1336, :1379, :1411). This closes the sibling-defect TODO row
flagged by [[lumen-settings-no-writable-banner]]:72-75 (its "mirror image" prediction).

- **The defect** (TODO row `.zcode/TODO.md:154-164`, found 2026-11-24 with settings-no-writable-banner):
  `session::repaint()` escalated only `untouched → memory_only` (settings_ui.cppm:1258-1262 at base)
  while the banner renders only under `memory_only` (:2430-2431) — so once a write had succeeded
  (`state == written`), a later edit that could not reach the file was applied to the session
  silently: no banner, in exactly the state the banner exists for. The five write-failure tails
  (set_bool/set_int/set_number/set_enum/commit_string) all raise through repaint() and all assign the
  in-memory field on the false path, so the divergence was reachable from any control.
- **Confirm-first** (the row's probe, acceptance 1): a temporary test in tests/test_settings_ui.cpp
  run at base 87327dd (removed afterwards) did the row's sequence — one successful set_enum (state
  `written`, banner down), config directory chmod 0500, set_enum back — and printed
  "probe: enum 0->0 state=1 banner=0": applied in memory, state stayed `written`, no banner;
  CHECK(state == memory_only) failed at test_settings_ui.cpp:1081. The defect held, so acceptance 5's
  amend-the-row branch did not trigger.
- **The fix (1bc8b2e):** :1261 reads `if (save_ != save_state::memory_only)` — escalation from any
  state that is not `memory_only`, `written` included; `untouched → memory_only` is unchanged, and
  navigation/menus still re-describe through refresh() alone. Semantically exact because repaint()
  has exactly five callers (grep, lines above) and every one is a write-failure tail where the edit
  did not reach the file; no unchanged-value early return exists that could raise a false banner
  (splice+write happen regardless). Untouched: `written()` clearing (:330-333), commit_layer's
  independent `save_ = written` (:1584), the banner gate. The declaration's doc comment (:322-327)
  says so now.
- **Regression test** (tests/test_settings_ui.cpp:460-478, extending
  settings_banner_is_raised_only_by_an_edit_that_did_not_reach_the_file): (f') after (g)'s successful
  write, directory read-only again, the same set_bool applies in the session, state `memory_only`,
  banner shown, the file keeps (g)'s bytes; (g') writable again, the same edit writes, state
  `written`, banner gone, file changed. **Red**: with the fix reverted (checkout of settings_ui.cppm,
  rebuild) the test fails at :468 CHECK(state == memory_only); re-applied, passes — recorded in the
  commit message. Neither reviewer nor gate re-executed the red (read-only roles); it is structurally
  certain: base's `save_ == untouched` cannot escalate from `written`. The (g') same-direction edit
  (`!value` again) is necessary, not a copy-paste slip — after (g) the file holds `value`, so only
  `!value` makes `file_text() != written_text` provable.
- **Gate** (classifier-confirmed, my run in the gate stage: `python3 scripts/classify-change.py
  --base 87327dd...` → classes ["logic"], ctest only): `ctest --test-dir build/test
  --output-on-failure -LE bench` → **141/141 passed** (146 registered, 5 bench; 146 baseline),
  ~80 s, on the committed tree at 1bc8b2e; `ctest -R settings` → 2/2. Seat smokes not required
  (classifier smokes:false; no compositor/display/input surface — the config write path is
  byte-identical to base). Two extra smokes ran PASS (drag-smoke, app-smoke, --join-seat-session)
  but carry **no surviving broker ticket line** (structural, per [[lumen-text-input-drop-oldest]]'s
  rule proposal — already standing, not re-proposed) and **neither launches lumen-settings**
  (app-smoke.sh:455-460; drag-smoke runs lumen-square): zero runtime-on-seat coverage of the changed
  code, acceptable only because the classification prescribes ctest alone.
- **Hygiene**: `./scripts/diff-hygiene.sh 87327dd` exits 1 on the whole-file format step only —
  settings_ui.cppm ~360 offending lines, tests/test_settings_ui.cpp 9 at HEAD (the open decision row
  `.zcode/TODO.md:165-178`), and the intersection of violating lines with this diff's added lines
  (:322-327, :1261; :460-478) is empty (reviewer's independent added-lines×disagreement check; gate's
  clang-format counts concur). Cite the row at landing; do not reformat here. The script's known
  `local`-at-:92 bug printed again (open row :148-153).
- **Host finding:** the read-only-directory mechanism depends on ctest running as uid 1000 — the
  row's own comment (tests/test_settings_ui.cpp:411-412) warns the chmod 0500 trick "under root that
  stops"; on this host the legs are effective because the suite runs unprivileged. Running the
  settings suite as root would silently turn (f')/(g') into no-ops that still pass.
- **Record gaps (disclosed):** no red-run artifact retained (red rests on the commit message + the
  structural argument); the "1 fix round" cause is unverifiable (/tmp/banner-fix.patch matches the
  committed hunks, but no log says what failed in round 1); the baseline-146 figure is packet-claimed,
  not recomputed on a base build.

**Review noise (cross-run false-positive tracking): raised 3 / confirmed 3 / refuted 0 /
preexisting 3.** All three confirmed findings are PRE at base: the whole-file clang-format debt
(open row :165-178), diff-hygiene.sh:92 `local` + /tmp leak (open row :148-153), and the unticked
fixed row itself (:154-164 — resolved by this task; the tick belongs to the landing stage).

**Why:** a state gate checked from one prior state only is wrong in exactly the direction nobody
probed — the banner machine shipped first too permissive (raised from any state) and then too
quiet (silent from `written`), and the (f)/(g) legs that pinned the fix's good direction left the
`written` entry untested.

**How to apply:** the TODO row at `.zcode/TODO.md:154-164` is amended with branch/READY status —
land `zcode/task/settings-banner-written-state` (1bc8b2e), **then tick it**. To re-verify:
`ctest --test-dir build/test --output-on-failure -LE bench` in the worktree (141/141), or
`ctest -R settings` for the banner test alone. When fixing a state-gate helper, enumerate every
prior state the gate distinguishes and give the regression test a leg per state — here
untouched/written/memory_only; a single-entry leg pair hid the second bug through the first fix.

Rule proposal: second recorded occurrence of the one-direction state-gate family — first
[[lumen-settings-no-writable-banner]] (the gate raised from every state, 13 non-edit callers), now
this (the gate escalated from only one state, `written` missed). Mechanical check: a change to a
state-transition helper must ship a regression test whose legs cover **each reachable prior state**
of the machine for the transition it fixes (a reviewer checklist item: "which prior states did the
legs start from?"), and the defect-confirm probe reads the state *before* the trigger, not only the
banner afterwards.

Related: [[lumen-settings-no-writable-banner]] (the banner's launch-banner defect and the row this
closes), [[lumen-text-input-drop-oldest]] (the standing broker-ticket-line rule proposal; same
smoke-ticket gap), [[lumen-invariant-gap-tests]] (the round that first tracked diff-hygiene.sh:92),
[[lumen-notebook-hw]] (the host).
