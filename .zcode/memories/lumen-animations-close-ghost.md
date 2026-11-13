---
name: lumen-animations-close-ghost
description: "2026-11-14 notebook seat (worktree animations-post-blur-verify, evidence-only cd6a576..7f886d5 on panel-blur-fix, gate READY): symptom-1 toggle damage non-reproducible in 7 probed states (0 px / clock-only 212 px); combined-config startup clean (2 black frames = known gap); NEW pre-existing bug — closing a FLOATING window with animations on freezes a full-opacity ghost (B3 248 139 px byte-identical on main pre-fix; tiled closes 0 px — the concurrent re-tile carries the fade); drag-smoke PASS, ctest 131/133"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-14 02:20–03:37 on the **notebook** (nouveau card0 + i915 card1, hasvk,
3200×1800@2x — the seat of [[lumen-raise-the-bar-repro-diagnose]] and
[[lumen-panel-blur-fix]]). Worktree `/home/dimmus/lumen-wt/animations-post-blur-verify`,
branch `zcode/task/animations-post-blur-verify`, **source-identical to panel-blur-fix HEAD
cd6a576** (0 non-evidence files in `git diff --name-only zcode/task/panel-blur-fix..HEAD`,
re-run); evidence commits **0618aec** (session A) + **91ec1f7** (session B) + **36cb4ae**
(B2–B5) + **b172a8b** (report + drag-smoke) + review fixes **1cd104b**/**ba5d66f** +
gate smokes **7f886d5**; gate **READY**. Report: `evidence/ANIMATIONS-REVERIFY.md`; six
seat sessions: A `run-anim-live2`, B `run-anim-start2`, B2 `run-anim-b2`,
B3 `run-anim-b3-bluroff`, B4 `run-anim-b4`, B5 `run-anim-b5-prefix` (main's pre-fix
build/release at fc92d14). Config restored byte-identical per session (sha256
`3c70e17e…febc6`, re-read from `config-sha256-RESTORED.txt`).

- **Symptom 1 closed (the task's verdict):** live-enable of `[animations] enabled=true` is
  clean in the four states the diagnosis did not cover — fullscreened window (gen 4,
  **0 px**), 5 windows across 2 workspaces (gen 6, **0 px** on the active ws), mid-drag
  (gen 8, settled byte-identical to an anim-off control drag, 212 px = panel clock bbox
  x3047-3061 y21-41), combined blur+animations desktop (gens 11/13, **0 px**) — 7 probed
  states total with the diagnosis's three. `toggles.txt` = 12 lines, gens 2–13 each once,
  against 13 `config: generation` lines (gen 1 = start; gen 2 = the unprobed aborted
  attempt, below).
- **Startup with the owner's combined config on the fixed tree is healthy:** 2 black
  frames only (the compositor-first-frame → shell-first-paint gap, A/B'd in the diagnosis
  run2/run2b), fully lit from probe 3 (5 760 000 px), **zero** shell restarts — vs the
  broken run5's 8/8 black + 5 restarts. Workspace slide round-trip **0 px** (B2); tile
  reflow round-trip 190 px clock-only.
- **THE FINDING (follow-up task, not fixed): closing a window on a FLOATING workspace
  with animations enabled leaves the closed window drawn on screen indefinitely** — a
  frozen close ghost at full opacity (titlebar text "dimmus@iritur: ~", focused border,
  exact geometry), byte-identical between probes 1 s apart, stable 10+ s, and it
  **survives a workspace round-trip while mutating** (B2: 97 304 px of the phantom
  changed). B3: 248 139 px, bbox x966-2361 y464-1527. Log shows correct WM teardown
  (`unmap reason=null-buffer`, decoration destroy, refocus) then 3–4 ~2 M-px composites,
  one ~410 k-px frame, then the frame clock idles — **the fade never ramps to invisible**.
- **Matrix:** stale in every animations-on floating close (blur on B/off B2/never-on B3/B4;
  109 402–248 281 px); **tiled close is clean** (B4, 0 px — the surviving window's
  concurrent re-tile animation carries the frame clock, which is why the diagnosis run1b's
  close looked clean: it was tiled); **B5 on main's pre-fix build: 248 139 px,
  byte-identical count/bbox/meandiff to B3 → pre-existing on main, NOT a regression from
  the blur-fix chain** (the no-degradation gate concern, closed); animations-off floating
  close clean (B5 animoff even cleared an earlier phantom).
- Follow-up entry point (named in the report, not investigated): close-ghost
  retire/`active()` accounting when no other animation shares the frame clock —
  `window_animation.cppm` (`active()`, `launch(-1.0, 0.0)`), `compositor_impl.cpp` ghost
  hold/defer_retire, `wire_buffer` retire — the diagnosis's seeded hypotheses already
  point there.
- Gate: drag-smoke **PASS** twice (task-time `smokes/drag-smoke-anim.log`, and gate-time
  against final HEAD committed by 7f886d5 — VERDICT lines re-read); app-smoke exit 1 with
  FAIL set identical to the baseline's own gate-time run (Z2–Z5 = the portal environmental
  family, [[lumen-app-smoke-known-noise]]; row-by-row A/B in evidence/README.md) — no
  degradation with zero source change. Suite 131/133, failures exactly the tolerated
  {ui_gallery, test_task_model}.
- Disproven: "the ghost is a blur-fix regression" (B5 byte-identical refutes); "session A's
  bell/DND pixel flips are toggle damage" — every `bell x=1516` layout follows a
  `do not disturb on (screen captured)` line; rapid `grim` capture itself flips the panel
  bell's DND layout (~3.4 k px at x2775-3121 y17-46), probe-induced, settled shots
  byte-identical.
- Fuckups: (1) session A's first state-1 toggle fired unprobed — a shell backgrounding
  mistake flipped the config (gen 2) before any probe; re-run cleanly as gen 4 and
  disclosed in the report's generation accounting. (2) Session B hit its `-t 900` limit
  mid-suite; the missing motion round-trips ran as fresh session B2. (3) B2's teardown
  wrote into `run-anim-start2/` because the shared finish script hardcodes that dir —
  artifacts moved to `run-anim-b2/` (`teardown-note.txt`) and start2's own files restored
  to committed bytes. (4) `KEY_EQUAL` is not in `inject-input.py`'s key-name list
  (packet-sourced); the equal key was injected by evdev code 13. Review: 2 low findings,
  both report-accuracy (1cd104b: `toggles.txt` misdescribed as listing gen 2 twice;
  ba5d66f: PNG-conversion audit-trail overclaim — only 5 of ~144 PNGs have a conversion
  log), both fixed touching only ANIMATIONS-REVERIFY.md.

**Why:** a fade animation that is the *only* client of the frame clock can freeze mid-fade
and leave permanent seat-visible damage that every prior probe missed — because the probe
happened to run on a tiled workspace where another animation was driving the clock. A
"settled byte-clean" motion check is only as good as the layout state it was taken in;
the default floating layout must be probed too. And byte-identical A/B against a pre-fix
main build is the fastest way to discharge the no-degradation question for an evidence-only
verification task.

**How to apply:** fixing the ghost: start from the floating close with animations on
(session B3's protocol — Super+Return, Alt+F4, probe 1 s apart), read
`window_animation.cppm`/`compositor_impl.cpp` ghost retire paths; the tiled-vs-floating
split (B4) is your A/B lever. When probing animations on the seat, include a floating
workspace close, and remember grim capture flips the panel bell into DND (ignore ~3.4 k px
at the bell bbox; use settled shots). For evdev keys missing from `inject-input.py`,
pass the numeric code (equal = 13). Never reuse a run dir across sessions — the finish
script hardcodes its dir and will trample an earlier session's teardown.

Related: [[lumen-raise-the-bar-repro-diagnose]] (symptom 1's diagnosis, the states it
covered, run2/run2b), [[lumen-panel-blur-fix]] (the fix this re-verifies on top of),
[[lumen-notebook-hw]], [[lumen-app-smoke-known-noise]], [[lumen-no-degradation-rule]].
