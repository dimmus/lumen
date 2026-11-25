---
name: lumen-scene-container-warn-and-drop
description: "container_node::add warn_and_drop warn landed 2026-11-25 (merge e8b2500, commit 4191ed0): warn-once per overflow episode re-armed by remove(), pinned in test_overflow_caps, A/B clean — and the second sub-us A/B mover adjudicated away (Rule proposal below)."
summary: "The warn warn_and_drop promises, landed 2026-11-25 (e8b2500): warn-once per episode, remove() re-arms; pinned + red-proven; 143/143 twice; A/B 6x vulkan clean; 2nd sub-us A/B mover."
modules: lx.scene, tests
topics: overflow-caps, logging, bench-ab
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-25 on the notebook (i915+nouveau, hasvk decisive): the pre-existing gap
"container_node::add never warns under warn_and_drop" (TODO row, found 2026-11-24,
since 1a44c01) was confirmed on main, fixed in the module's logging idiom, pinned,
gated and landed.

- Defect held on main first: `test_overflow_caps` on pristine 657da67 (tree identical to
  main) prints `observed, not pinned (1): container_node::add under warn_and_drop
  refuses and logs nothing` — the k_further overflowing adds produce warns==0
  (/tmp/defect-main-overflow-caps.log).
- Fix (commit 4191ed0, modules/lx.scene/lx.scene.cppm): `import lx.trace` +
  file-local `constexpr lx::trace::category log_container{"scene.container"}` beside
  `log_draw_list{"scene.draw_list"}` (snapshot_buffer.cppm:14); static message
  "container child cap reached" (the family's "… cap reached" shape — smoke scripts
  grep log text, keep it stable); warn-once latch (the cap_warned_ idiom,
  snapshot_buffer.cppm:314/:813-817) firing only on the warn_and_drop path of add().
  reject/drop_newest/drop_oldest fall through to the same return false / evict branch.
- **Latch semantics (the explicit decision)**: the latch marks an overflow *episode*,
  not a container's life — `remove()` making room re-arms it (draw_list's clear_lists
  re-arm, snapshot_buffer.cppm:735-739, is the family precedent). A container that
  drained and refilled warns again; one sitting full stays quiet. Stated in the commit
  message, pinned in the test (two warns across drain+refill, not one-for-life).
- CMakeLists DEPENDS for lx.scene left without lx.trace (existing registration
  precedent): the module scanner already resolves the :snapshot partition's lx.trace
  import the same way and the build is green — no DEPENDS edit needed.
- Test (tests/test_overflow_caps.cpp): the observed() marker flipped to the family's
  hard pin (the draw_list pin's shape — exactly one warn under warn_and_drop, never
  otherwise) + an episode-re-arm pin; the file header's departure ledger updated
  (container joins text_input as "fixed, not accommodated"). Red-proven on a scratch
  worktree at 657da67 with the module hunk reversed: the named pin FAILs.
- Gate READY per classifier on the real diff (logic + render-path -> ctest + bench_ab):
  worktree ctest 143/143 -LE bench; merged tree re-gated 143/143 (build/merge-gate);
  bench_ab = 6 interleaved hasvk vulkan runs (3x base 657da67, 3x 4191ed0, pin 3,
  setarch -R by the script, quiet-CPU hold + host warm-up verified per run, six
  distinct bench tickets, seat quiet before/after). Artifact locations (stated
  precisely 2026-11-25, superseding the handoff note's "6 result JSONs in
  /tmp/scene-ab/"): the six run logs (run1..run6 {base,cand}.log) and the three
  base JSONs (20261125-657da67{,-2,-3}.json, git stamp 657da67) live in
  /tmp/scene-ab/; the three candidate JSONs (20261125-4191ed0{,-2,-3}.json, git
  stamp 4191ed0, backend vulkan) live in the task worktree's
  tests/bench/results/iritur-08fd3c-vulkan/, where ui-bench wrote them — all six
  exist, split across the two places. One >=10% mover: S2/update +15.4%
  = +0.010 us on a 0.065 us stage, 100x under the bench's own ABS_FLOOR_US=1.0
  (scripts/ui-bench.sh:113), and lumen-ui-bench (tests/bench_ui.cpp, CMakeLists:1242)
  never calls container_node::add/remove — the stage cannot reach the diff. All
  composite frame p50s flat to -3%.
- Format debt, fourth measured instance for the decision row (.zcode/TODO.md
  "Whole-file clang-format debt" row): lx.scene.cppm carries 36 violation lines (53
  replacement operations) at base and HEAD alike under the repo .clang-format —
  corrected 2026-11-25 from "1890 violation lines", a figure a style-less
  clang-format run on a bare /tmp copy produces, ~35x the real debt; this change's
  19 added lines intersect the violation set emptily; diff-hygiene exits 1 with no
  added line at fault (its own `local` bug row is separate).

**Why:** an enum that promises "Log warn, then drop_newest" is a contract every site
must keep — the overflow-caps lineage exists to hold each fixed-capacity structure to
it, and the marker it carried for this site was the debt's receipt. And a sub-µs A/B
mover >=10% is now twice (task 1's S4/update +26% = 0.091 us; here S2/update +15.4% =
0.010 us) noise the gate must not stop on — but only when the magnitude is under the
bench's own floor AND the stage is off the diff's path; either leg alone is not enough.

**How to apply:** a new overflow_action site gets its warn in the module's idiom
(category + static "… cap reached" message + warn-once latch) and its pin in
test_overflow_caps the same day; an A/B stage moving >=10% is adjudicated on both legs
(delta vs ABS_FLOOR_US=1.0, and reachability of the diff's code from the stage's
scenario source) before it is called noise; extend [[lumen-text-layout-line-cap-reject]]
bench discipline, don't re-derive it.

Rule proposal: the A/B adjudication step should compute mechanically, per stage with
delta >= 10%, the pair (delta_us vs ABS_FLOOR_US, "diff's symbols reachable from the
scenario's measured path") and print the verdict table in the gate log — the two
sub-µs movers so far were adjudicated by hand from the same two facts a script could
check (a ui-bench.sh `--ab-adjudicate` step, or the gatekeeper's bench_ab judgment
block).
