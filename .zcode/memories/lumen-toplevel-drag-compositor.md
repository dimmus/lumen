---
name: lumen-toplevel-drag-compositor
description: "2026-11-12: compositor side of xdg-toplevel-drag-v1 (worktree 0600289+a0de5cf, review fixes 82634bf/f470a11) — wm_dragged_window as the third pointer mode (no wm_grab), xfer_source mark → invalid_source, toplevel_record.unmap_epoch for re-attach; ctest 131/133 known-red only, conformance 242 cases, dnd_grab alloc-counter motion=0/tick=0, drag-smoke PASS ×2 on the notebook; g-latency WARN proven host noise by interleaved A/B; seat trap = wrong uinput event node; gate READY"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-12 in worktree `/home/dimmus/lumen-wt/toplevel-drag-compositor` (branch
`zcode/task/toplevel-drag-compositor`, based on the parent task's d95abb1; commits
0600289 + a0de5cf + review fixes 82634bf, f470a11 — full diff d95abb1..f470a11, 12 files,
+1044/−10). Gate verdict **READY** (merge remains the user's call).

- 0600289 adds `modules/lx.compositor/wire_toplevel_drag.cpp` (270 lines) from the
  wire_dialog.cpp template, gated `#if defined(LUMEN_HAS_PROTOCOL_GLUE) &&
  defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_DRAG_V1)` (wire_toplevel_drag.cpp:37);
  `xdg_toplevel_drag_manager_v1` registered as an unprivileged global immediately beside
  xdg_wm_dialog_v1 (wire_install.cpp:612-616) — per-window feature, sandboxed clients
  included. a0de5cf adds the protocols/README.md narrative row.
- Design: the dragged window is **window_manager state (`wm_dragged_window`), not a
  wm_grab** — the DnD drag stays the third pointer mode (grab_mode move/resize remain the
  interactive move's). Anchored at attach/map, applied once per tick through move_window
  with the same round_delta whole-delta primitive as apply_grab; plain move, no snap
  zones/drag-to-restore. The attached window is excluded from drag_hit; unmap
  auto-detaches and re-attach after remap works via new `toplevel_record.unmap_epoch`
  (toplevel_manager.cppm:151), which distinguishes an auto-detaching unmap from a live
  attachment (else `toplevel_attached`).
- Error surface: `xfer_source::toplevel_drag` back-pointer (wire.cppm:497) marks a source
  — `get_xdg_toplevel_drag` twice → manager/invalid_source; a marked source given to
  `set_selection` → **wl_data_source**/invalid_source (wire_selection.cpp:482-488,
  mirroring the existing actions_set error), because protocol errors resolve against the
  receiving resource's interface — the drag manager's own enum cannot be raised on a
  wl_data_device-carried request. attach works before or after start_drag; a toplevel
  mapping mid-drag appears at hotspot minus offset; destroy before the drag ends →
  ongoing_drag (a destructor request surfaces as interface "[destroyed]" client-side);
  wire_dnd calls toplevel_drag_started/ended on every end path (drop, cancels, source
  destroyed, client gone).
- Tests: 12 wire-level cases in test_protocol_conformance.cpp (real press serials,
  injected deltas +34,+21 then +46,+16 asserted per tick, new `pump_tick` helper);
  `./build/test/test_protocol_conformance` → 242 cases, 0 failed. Review confirmed the
  acceptance's conditional was met (window_manager state extended) but test_dnd_grab was
  not extended → 82634bf adds a real-window_manager `wm_fixture` and
  `dnd_dragged_toplevel_motion_and_tick_do_not_allocate` → "allocations: motion=0 tick=0"
  (9 tests, 0 failed) — the no-alloc/O(1) contract is now test-enforced, and the fixture
  clears the WM's tree unmap hook (installed window_manager.cppm:1680) on teardown.
- Review also confirmed a test_ui_host flake as a **probe-point bug, not routing**: with
  B mapped first the fixed probe point landed inside overlapping A; f470a11 replaces
  fixed points with `only_in()` exclusive corners (no assertion weakened). Suite green
  after; the flake's exact interleaving is not forceable, so "cannot flake again" is not
  provable — only the placement-order dependence is gone.
- Suite: `ctest -LE bench` → 131/133, failures exactly {17 ui_gallery, 81 test_task_model}
  — the owner-tolerated known-red set, reproduced identically on the parent commit (see
  [[lumen-toplevel-drag-protocol-registration]]). Gatekeeper independently re-ran the
  suite and both new test binaries.
- Seat (notebook, i915+nouveau, eDP-1 3200x1800): the packet's `seat_failed: true` was
  **preflight-only** — both 23:10 runs exited 2 at drag-smoke.sh:137 ("FAIL:
  /dev/input/event17 is not an input device node"), all logs 0 bytes, compositor never
  launched: the uinput ABS mouse was event16, not event17. Gatekeeper regenerated the
  evidence: `sudo scripts/uinput-mouse.py --output 3200x1800` → /dev/input/event16,
  keyboard event3, `LUMEN_BUILD_DIR=build/test`, `LUMEN_SMOKE_SCALE=1` → drag-smoke
  `VERDICT: PASS` twice (rows a–i); app-smoke exit 1 = the documented notebook baseline
  (140 PASS; FAIL set = the 35 environmental rows + exactly one alternating timing row —
  N8 flipped with baseline-identical counters 0/11, T3 a documented flake).
- No-degradation A/B (gatekeeper, back-to-back interleaved): new binary window-move
  {median 18.5 ms / p95 47.6 WARN; then 13.1/35.7 clean} vs parent d95abb1 binary
  {17.1/47.0 WARN; 17.4/46.9 WARN} and its 20:43 baseline {13.5/30.4} — distributions
  overlap, the g-latency WARN alternates on both binaries, slow samples attribute to the
  display (page-flip) leg. The implementer's own runs: base 13.0–16.0 ms vs new
  10.2–17.2 ms medians, within each binary's run-to-run spread. No regression signal; no
  A/B evidence warranted stopping (AGENTS §1.2 satisfied by measurement, not assertion).

Disproven theories: "g-latency WARN on the new binary = regression" — the same WARN
appears on the parent binary in interleaved rounds; it is host display-leg noise.
"app-smoke failing one more row than baseline = new breakage" — the extra rows flip
between runs of the same binary. "a named protocol error can be posted from any request"
— errors resolve per receiving interface. Fuckups: (1) the smoke gate reran with a stale
injection node (event17) that preflight rejects — same exit-2 trap as the parent task's
LUMEN_BUILD_DIR miss, new costume; (2) that failed rerun **overwrote the implementer's
PASS logs** in drag-smoke-logs/ and app-smoke-logs/, making the original seat evidence
unverifiable — the gatekeeper's regenerated runs are the evidence of record.

**Why:** serving a per-window protocol is mostly WM-state placement (a third pointer
mode beside wm_grab) plus error-surface bookkeeping, and on this host a single smoke
latency WARN or one flipped smoke row is not degradation evidence — only interleaved A/B
against the parent binary is.

**How to apply:** for the next staging protocol: wire unit from the wire_dialog.cpp
template, register beside xdg-dialog as unprivileged, put behavior in window_manager as
parallel state, and whenever window_manager gains a motion path extend test_dnd_grab's
wm_fixture + alloc counter. Before any seat smoke verify the injected nodes exist
(`ls /dev/input/event*`) and set `LUMEN_BUILD_DIR`; copy a prior smoke's logs aside
before rerunning — gate reruns overwrite them; judge latency only via interleaved A/B.
Related: [[lumen-toplevel-drag-protocol-registration]], [[lumen-notebook-hw]],
[[lumen-app-smoke-known-noise]], [[lumen-no-degradation-rule]],
[[lumen-zcode-agent-harness]].
