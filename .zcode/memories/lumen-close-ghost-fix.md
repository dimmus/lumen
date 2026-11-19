---
name: lumen-close-ghost-fix
description: "2026-11-14 notebook seat fix (worktree close-ghost-fix, f1391e2..e4fb181, gate READY): the floating close-ghost freeze was NOT a frame-clock stall — the client's null-buffer commit collapsed rec.geometry to 0x0 before on_unmap sized the ghost (rest 2x38, fade damaging a ~450 px sliver while drawing the full-size window), so the window's area was never repainted (B1 frozen: base vs settled 21,836 px; fixed V2: 1,506,809 px, fade = log frames 79-96 at 1.21-1.52 M px, settled byte-clean); fix = null-buffer unmap branch first in on_toplevel_commit (window_manager.cppm:2804-2816) + make_ghost widens rest.bounds with copied node rects (:6429-6438); 'null close' + 'shadow close' unit cases (35/35, fails 3 checks unfixed); drag PASS, app-smoke 145 PASS (FAIL set 32→31, only N8); ctest 131/133"
metadata:
  node_type: memory
  type: project
summary: "Ghost freeze root-caused to the null-buffer commit collapsing geometry before on_unmap sized the ghost — fix = unmap branch first + make_ghost widens bounds; unit cases 35/35, app-smoke FAIL set 32→31"
modules: lx.compositor
topics: damage, unmap, ghost

---

Verified 2026-11-14 on the **notebook** (nouveau card0 + i915 card1 per each session's
`precheck.txt`, hasvk, 3200×1800@2x — the seat of [[lumen-animations-close-ghost]] whose
B3 finding this fixes). Worktree `/home/dimmus/lumen-wt/close-ghost-fix`, branch
`zcode/task/close-ghost-fix` off main HEAD; commits **f1391e2** (the fix, 09:29) +
**e160ebe** (B1 repro + V2 seat evidence + smokes, 09:52) + **e31d57c** (README host-line
order) + review fixes **e5f1a6b**/**6601fee** (test-only) + **e4fb181** (10:58, see the
gate wrinkle below). Gate **READY**. Diff confined to
`modules/lx.compositor/window_manager.cppm` + `tests/test_window_animations.cpp` +
`evidence/` (re-checked with `git diff --name-only main...HEAD`). Every number below
re-run by the memory writer this session from the committed evidence (PNGs decoded back
to PPM with a stdlib decoder, then `ppmdiff.py`) unless marked reviewer-sourced.

- **Root cause (not the suspected one):** a closing client's unmapping commit is
  `attach(null)+commit`; it empties the tree, so `on_toplevel_commit` measured the
  window geometry against the empty commit — 0x0 at the old origin (frame 2×38 with
  server decorations) — **before** `on_unmap` built the closing ghost. B1's instrumented
  log (evidence/run-ghost-b1/compositor.log): `:224 surface geometry surface=6 …
  size=0x0`, `:227 GHOSTDBG close id=1 frame=451,200,2x38` against the mapped
  452,237 696×494 (`:120`). The ghost then ticked its full fade (39 `GHOSTDBG tick`
  lines, alpha −1.0→0) while **damaging only the collapsed ~450 px sliver per frame**
  and drawing the copied full-size nodes at an identity pose: the window's area was
  composited once more by the unmap's own node-drop damage and never again — the last
  presented frame (window at ~full opacity) stayed on screen. Tiled closes stayed clean
  only because the survivor's re-tile animation repainted the area (B3 vs B4 in
  [[lumen-animations-close-ghost]]).
- **The fix (window_manager.cppm, two hunks):** the null-buffer unmap branch moves to
  the top of `on_toplevel_commit` (:2804-2816) — geometry stays what the window was
  last shown at, which is what the ghost and unmap damage are sized against; and
  `make_ghost` widens `rest.bounds` with the rects of the nodes it copies (:6429-6438) —
  a CSD shadow reaches past the xdg geometry and the tree's own bbox is already empty
  on this path. Design: size the ghost from the WM's last-known geometry; no extra
  damage beyond the fade's own bounds.
- **Seat verification (run-ghost-v2, fixed tree, probes 09:31-09:33 per
  probe-times.txt):** floating close settled vs late (10 s apart) **changed=0**;
  base-1 vs settled **1,506,809 px (26.2%)** — the whole window area repainted
  (B1 unfixed, re-run: settled vs late **0**, base vs settled **21,836 px** = frozen);
  tiled close (B4 control) **0**; animations-off control **134 px, bbox x3047-3060
  y21-41 = panel clock only**. The fade itself ran on the seat: compositor.log frames
  **79-96 = 18 consecutive composites 1,524,092 → 1,214,044 px** (the window's physical
  area at 2× scale), settle frame 97, then quiet; `:198 unmap id=1 reason=null-buffer`.
- **Unit regression (the acceptance's only-animation case):** `null close` in
  tests/test_window_animations.cpp drives the real wire path (`attach(null)+commit` on
  a mapped toplevel) with the close fade as the ONLY animation — precondition at :986,
  ghost animates :993, fade at the last-shown rect :1025, per-frame published damage
  first ≥ window area / least ≥ 3/4 :1029, then quiet with nothing drawn :1033.
  Fails exactly 3 checks on the unfixed tree (reviewer's scratch build; corroborated by
  B1's log). **35/35 cases pass** (re-run `build/test/test_window_animations` this
  session). e5f1a6b adds the `shadow close` CSD-subsurface case (:1069-1115) so the
  bounds widening is genuinely exercised — the reviewer's widening-deleted scratch build
  fails exactly its shadow-damage check. 6601fee replaced the non-discriminating
  "wl_buffer released once the ghost retires" claim with an honest release-at-commit
  assertion (:983); the ghost dma-buf hold stays covered by
  `lifecycle_keeps_a_ghosts_dmabuf_until_it_lets_go` (:449).
- **Smokes + suite:** drag-smoke **VERDICT: PASS** (09:34) and app-smoke **145 `^PASS` /
  31 `^FAIL`** (09:45, ≥140 envelope) — both committed by e160ebe, against a compositor
  source-identical to final HEAD (the two later commits touch only the test file). FAIL
  set diffed by me against the prior same-host main-identical gate log
  (animations-post-blur-verify): **32 → 31, the only change N8 now PASS** — no new
  failures ([[lumen-app-smoke-known-noise]] families). Suite 131/133, failures exactly
  the tolerated {ui_gallery, test_task_model} (reviewer's run on HEAD; `ctest -N -LE
  bench` = 133 re-run by me, full suite not re-run).
- **Gate wrinkle (read e4fb181 before trusting its title):** it says "gate-time seat
  smokes against final HEAD" but `evidence/smokes/gate/*/run.log` each contain only
  `FAIL: /dev/input/event16 is not an input device node` — the 10:58 re-launch died at
  preflight (the uinput mouse enumerated as **event18** this boot; event16 is a stale
  constant in the studio's gate invocation), and the 66 per-scenario artifacts committed
  alongside are stale carryover from the 09:45 run. The operative PASS evidence is
  e160ebe's; the packet's `gate_runs.seat` exit-2 entries are this re-launch.
- **Host discipline:** `~/.config/lumen/config.toml` sha256 `3c70e17e…febc6` restored
  byte-identical (re-hashed this session); precheck/postcheck per session in both run
  dirs; compositor confirmed down. Disclosure: stale `uinput-mouse.py` leftovers from
  the earlier animations session (pids 275189/275192, predating this task) were killed
  during cleanup.
- **Prose errors in the evidence README** (fixed or disclosed, not blocking):
  "the 12 FAIL rows" — the app-smoke log has 31 `^FAIL` rows (the row-by-row claim is
  what matters and is true); e31d57c fixed a transposed card order in the host line.

**Disproven (closed; do not re-chase):** the card's suspected mechanism — "the close
fade stalls when it is the frame clock's only client" / `window_animation::active()`
/ ghost hold / `defer_retire` / `wire_buffer` retire — GHOSTDBG instrumentation showed
the clock ticking and the fade running its full course; **the stall was damage
starvation, not clock starvation**, and none of those paths were touched. Also closed
earlier: "the ghost is a blur-fix regression" (B5 byte-identical on main pre-fix) and
"opacity stuck near 0 while active() keeps the clock running" (open animations).

**Why:** a frozen animation on the seat can be *damage starvation* — the fade ticks
perfectly while damaging a rect that no longer covers what it draws — so "is the clock
advancing" is the wrong first question; ask what each frame damages against what it
draws. And a commit's effect must be measured against the window as last shown: the
wire's legitimate `attach(null)+commit` empties the tree mid-flow, so anything sized
from that commit's geometry collapses.

**How to apply:** keep the null-buffer unmap branch at the top of `on_toplevel_commit`
and size ghosts from last-known geometry; if ghost content can reach past the window
geometry (CSD shadows), widen `rest.bounds` with the copied node rects as make_ghost
now does. Reproduce with the `null close` case (3 failing checks on unfixed trees) —
it runs in-process, no seat needed. On the seat, never reuse a uinput node constant
across boots/sessions: read the live node (this boot: mouse event18, keyboard event3),
and a preflight `FAIL: … is not an input device node` (exit 2) means the smoke *never
ran* — its logs are not evidence, and don't commit them under a "gate-time smokes"
title. Judge app-smoke deltas by diffing FAIL row IDs against a source-identical prior
log on the same host.

Related: [[lumen-animations-close-ghost]] (the bug record and its B3/B4/B5 matrix),
[[lumen-panel-blur-fix]] (same diagnosis→fix pattern, same exit-2-at-gate trap),
[[lumen-toplevel-drag-compositor]] (uinput-node preflight trap), [[lumen-notebook-hw]],
[[lumen-app-smoke-known-noise]], [[lumen-no-degradation-rule]].
