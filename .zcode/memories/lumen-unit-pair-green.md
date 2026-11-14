---
name: lumen-unit-pair-green
description: "2026-11-14 notebook task (worktree unit-pair-green, 1ca2bf9..de92ce6, gate READY): the owner-tolerated known-red pair retired, suite 131/133 → 133/133 exit 0 — test_task_model's two CHECKs were stale expectations left by a65d06b (logout popup shrank k_logout_width 84→28 and dropped panel_logout.armed, touching only 8 of ~95 diff lines; count 5/5 → 6/4 via (360−44)/52; the arming check compared identical inputs so panel_diff was 0 → now drives `pressed`, exactly 1 damage rect) plus review fix 75bf41d (cells[l.count−1] rightmost bound); ui_gallery was NOT font-rasterization host drift — the goldens (2c443b0, Nov 5) predate the select-chevron attach fix 1a44c01 (Nov 8); diff = three 8×6 px chevron blobs (72/76 px, max 111/130 per 255), GL-vs-CPU ≤1/255; goldens regenerated (42c2aa3) with cross-host bit-agreement 1,320,728/1,320,800 px vs the VM-era goldens; f10183a cleared the SKILL.md known-red note — known_red is now empty"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-14 on the **notebook** (nouveau card0 + i915 card1 re-read from
`/sys/class/drm/card*/device/driver` this session). Worktree
`/home/dimmus/lumen-wt/unit-pair-green`, branch `zcode/task/unit-pair-green` off main
fa11b38; commits **1ca2bf9** (19:46, test_task_model) → **42c2aa3** (19:57, goldens) →
review fixes **75bf41d**/**f10183a** (20:19/20:21) → **de92ce6** (20:48, evidence-only).
Gate **READY**. Branch diff contains **zero production code** (the test file, two golden
PNGs, two `.zcode` doc lines, `evidence/` logs). Every headline number below re-run by
the memory writer this session unless marked implementer/reviewer-sourced.

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
- **Host-independence evidence for the new goldens** (implementer's libpng measurement):
  the VM-era goldens match this host's render bit-for-bit on **1,320,728 of 1,320,800**
  pixels except the three chevron blobs, whose sorted diff-coordinate sets are identical
  cross-host; inputs are pinned (bundled IBM Plex Sans + tests/fonts, bundled Lucide
  icons); `test_ui_golden` is bit-exact incl. chevron_down at 14 px and the text
  raster/shaping tests pass here. Regeneration-with-evidence was the card's allowed
  alternative since the inputs were already pinned and proven stable.
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
ui_gallery cause — the render is host-stable and bit-agrees cross-host; the goldens were
simply older than a reviewed fix. Also closed: "GL-vs-CPU disagreement means an engine
bug" — GL matches CPU to ≤1/255 both themes (engine self-consistent, G10 tolerance ≤3).

**Why:** a tolerated-red set outlives its cause — a65d06b landed Nov 11 and the stale
checks sat red-tolerated for three days while verifying nothing, and "golden drift" got
blamed on hosts when the goldens were simply older than a reviewed fix. Tolerance without
a retirement task turns every future suite run into archaeology, and the first reflex on
golden failure should be `git log` on the golden, not font config.

**How to apply:** `known_red` is empty now — any suite red is reportable immediately; do
not re-tolerate without an owner decision plus a retirement task. When a golden test goes
red: localize the pixel diff to blobs first, then `git log --follow
docs/images/<golden>.png` and compare the last-regeneration commit against commits
touching the rendered widget code before suspecting fonts/hinting/hosts. When a change
removes a struct field or changes a geometry constant, grep the test file for expectations
*derived* from it, not just its name (`armed` vanished; the count math and the diff inputs
derived from it silently). Logout semantics today: `panel_logout` is `{shown, pressed}` —
no armed two-click field; damage tests press via `pressed` and expect exactly 1 rect.
Cross-host golden claims need full-image bit-agreement evidence, not max-error stats.

Related: [[lumen-toplevel-drag-protocol-registration]] (origin of the tolerance),
[[lumen-toplevel-drag-compositor]], [[lumen-toplevel-drag-drag-smoke]],
[[lumen-smoke-bench-harness-green]] (unmerged harness fixes; ui-bench baseline story),
[[lumen-close-ghost-fix]], [[lumen-panel-blur-fix]] (both recorded the same pair as
tolerated), [[lumen-notebook-hw]], [[lumen-app-smoke-known-noise]],
[[lumen-ui-bench-noise-and-g7]], [[lumen-no-degradation-rule]].
