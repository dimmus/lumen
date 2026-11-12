---
name: lumen-toplevel-drag-drag-smoke
description: "2026-11-13: seat-visible end of xdg-toplevel-drag-v1 (worktree b0a6244+3ff721b, fixes fc31dbc/8c0ca0e) — lumen-square --toplevel-drag client (scanner glue, LUMEN_SQUARE_HAS_TOPLEVEL_DRAG), drag-smoke row j (+240,+160 exact, 36 monotone updates, judged WM+client); final run PASS zero-warnings with g medians improved vs same-day baseline; ctest 131/133 known-red; app-smoke exit 1 baseline, FAIL set strict subset (N8 flipped PASS); traps = gitignored protocol XML silently compiling the feature out, VM-default injection nodes failing on the notebook; gate READY"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-13 in worktree `/home/dimmus/lumen-wt/toplevel-drag-drag-smoke` (branch
`zcode/task/toplevel-drag-drag-smoke`, based on the parent task's f470a11; commits
b0a6244 + 3ff721b + review fixes fc31dbc, 8c0ca0e — clean at 8c0ca0e). Ran on the
**notebook** (i915, eDP-1 3200x1800@59, hardware cursor planes), uinput mouse event16 +
`LUMEN_SMOKE_SCALE=1`. Gate verdict **READY** (merge remains the user's call).

- b0a6244 gives lumen-square a `--toplevel-drag` mode: binds wl_data_device_manager v3 +
  xdg_toplevel_drag_manager_v1 behind `LUMEN_SQUARE_HAS_TOPLEVEL_DRAG`; client glue is
  generated with real wayland-scanner (client-header + private-code) in the CMake block
  beside the existing cursor-shape/tablet one (CMakeLists.txt:430-455), degrading to a
  build without the mode plus a STATUS line when the XML is absent. Interior left press →
  wl_data_source (offer text/plain) → `get_xdg_toplevel_drag` → attach own toplevel with
  the pointer's surface-local offset → `wl_data_device.start_drag` with the press's own
  serial; the source's cancelled/finished destroy the single-use pair.
- 3ff721b adds drag-smoke **row j** beside a–i (scripts/drag-smoke.sh): injected press
  + drag (+240,+160) inside the mode square must move the window to start+delta with size
  kept, judged from **both sides** — WM: exactly one `dnd start` (0 rejected), one attach,
  ≥3 monotone `cause=toplevel-drag` geometry updates (±2 px), one no_target cancel +
  detach, nothing after detach, zero wm grabs; client: `press -> toplevel-drag`,
  `dnd cancelled`, no release line reaching the client.
- Review round (3 confirmed / 0 refuted), both fixes re-proven by the reviewer's own
  execution: fc31dbc — the map-wait's `[[ -n "$JID" ]]` guard was unreachable (on timeout
  `tail -1` of ALL map lines returns the scenario-f square's id, never empty), so the loop
  now fails on the count itself, exit 2 with the square's log dumped (drag-smoke.sh:436-441);
  8c0ca0e — attach moved **before** start_drag per the XML's stated workflow; proven on the
  wire in the final run: compositor.log `toplevel-drag attach … dragging=0` immediately
  before `dnd start client=3` (the attach line fires from started(), so the old order
  cannot produce it).
- Final seat run (drag-smoke-logs/run.log, 00:54): **VERDICT PASS, zero warnings**, exit 0,
  console=text, errors+warnings=0. Row j: origin (1400,750)→(1640,910), delta +240,+160
  exact, size 400x300 kept, **36** monotone geometry updates, start=1 rejected=0 attach=1
  detach=1 cancel(no_target)=1 wm-grabs=0 leaked releases=0; square2.log shows the client
  side exactly as judged. Rows a–f/h/i byte-identical to the same-day baseline
  (/tmp/drag-smoke-baseline/run.log, 00:08, identical binaries, no row j).
- No-degradation A/B: baseline g window-move median 14.8/p95 33.1 ms → final 13.0/26.2 ms,
  cursor median 12.4→8.5 ms — the final run *improved* and carries no warnings; the two
  earlier g-WARN runs of this session (p95 39.1/48.5 ms vs the 37.3 ms budget) were the
  documented host display-leg noise ([[lumen-toplevel-drag-compositor]]), and j's square
  launches only after g's measurement window, so no mechanism exists for j to touch g.
- Suite: tests-enabled `ctest --test-dir build/test --output-on-failure -LE bench`
  (/tmp/ctest-full.log, 00:19, after all CMake changes) → 131/133, failures exactly
  {17 ui_gallery, 81 test_task_model} — the owner-tolerated known-red set. No new unit
  tests: the wire-level behavior (both attach orders, per-tick following, protocol errors)
  is covered by the parent task's 12 conformance cases; a seat-visible injected-drag row
  cannot exist in ctest. Fix commits re-ran no suite (verified: none of the 138 registered
  tests exercise lumen-square or drag-smoke.sh, and the final source is seat-proven).
- app-smoke `--join-seat-session` → exit 1, honest in the packet (`seat_failed: true`) and
  **not a regression**: final run (app-smoke-logs/run.log, 01:16, 141 PASS) FAIL-row set is
  a strict **subset** of the pre-change baseline (/tmp/app-smoke-gate.log, 23:30) — the
  only delta is N8 (the documented alternating context-switch row) flipping *to PASS*
  (shell 0/compositor 11 vs baseline 3/16); ~35 failures are the documented notebook
  environmental families ([[lumen-app-smoke-known-noise]]).

Fuckups: (1) the worktree lacked
`protocols/upstream/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml` (gitignored; the
parent's copy step missed it), so the compositor had been built with the protocol compiled
out — re-copied from the parent worktree and reconfigured; `fetch-protocols.sh` after any
fresh worktree configure is the general fix. (2) The first baseline attempt used the VM's
default injection nodes (event2/event3) and failed environmentally — on the notebook
event2 is not the absolute uinput mouse; correct env is the regenerated uinput node
(event16) + `LUMEN_SMOKE_SCALE=1` (see [[lumen-notebook-hw]]). Residual, non-blocking:
the file-header comment at executables/lumen-square/main.cpp:36-39 still narrates the
pre-fix start_drag-then-attach order — one line of doc drift in a demo client.

Disproven theories: "row g's WARN = regression from row j" — same WARN appeared on the
parent binary in interleaved rounds, and the final zero-warning run improved every g
median vs baseline. "app-smoke exit 1 = breakage" — FAIL set is a subset of baseline.
"A missing upstream XML fails the build loudly" — it silently disables the feature (that
is why the CMake STATUS line exists).

**Why:** a smoke row is only as trustworthy as the environment that produced it —
half-copied gitignored inputs compile features out silently, wrong-host injection defaults
produce INVALID-looking failures, and one WARN latency row proves nothing without a
same-binary baseline.

**How to apply:** for the next demo-client protocol mode: scanner glue in the
lumen-square CMake block gated on `EXISTS` (never a hard require), destroy single-use
protocol pairs on their end events, and judge new smoke rows from both compositor and
client logs. Before any worktree smoke: `ls protocols/upstream/staging/<proto>/` and
regenerate the uinput node on the detected host (`scripts/uinput-mouse.py --output
3200x1800` → note the event node, `LUMEN_BUILD_DIR=build/test`, `LUMEN_SMOKE_SCALE=1`);
compare smoke/latency claims only against a same-day same-binary baseline, and app-smoke
exit 1 against FAIL-row-set diffs, not exit codes.
Related: [[lumen-toplevel-drag-compositor]],
[[lumen-toplevel-drag-protocol-registration]], [[lumen-notebook-hw]],
[[lumen-app-smoke-known-noise]], [[lumen-no-degradation-rule]].
