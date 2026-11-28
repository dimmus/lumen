---
name: lumen-wlr-foreign-toplevel-management-closeout
description: "Close-out detail for the zwlr_foreign_toplevel_management_v1 v3 task (2026-11-25, notebook): full semantics, evidence artifacts and paths, the heaptrack-wrapper fuckup, and the 2026-11-26 libinput-upgrade suite drift."
metadata:
  node_type: memory
  type: project
summary: "wlr-foreign-toplevel close-out: full semantics + artifact paths (evidence/2026-11-25-wlr-foreign-toplevel/), heaptrack-wrapper fuckup, 2026-11-26 suite drift"
modules: lx.compositor, protocols, scripts
topics: protocol, observer, taskbar, waybar
---

Detail split from [[lumen-wlr-foreign-toplevel-management]] (2026-11-28, the ≤120-line
rule). Facts verified 2026-11-25 on the notebook unless dated otherwise; evidence under
`/home/dimmus/lumen-wt/wlr-foreign-toplevel-management/evidence/2026-11-25-wlr-foreign-toplevel/`.

**Full semantics as built:**
- Bind: one toplevel event per mapped window, bottom to top; each handle's batch carries
  title, app_id, state, one output_enter per output the window is shown on, parent (null
  without one), one done. A window mapped later gets the same batch; a child listed before
  its parent still gets parent(handle) before that batch's done.
- Changes send only what changed, closed by one done; no event left open. activated on
  exactly the handle of the window window_manager's focus decision names; none when no
  window has the keyboard. closed at unmap; afterwards the handle is inert (every request
  but destroy ignored, no protocol error, connection stays up). stop → finished, no
  further toplevel event, manager resource destroyed immediately
  (wire_wlr_foreign_toplevel.cpp:524-531 — no direct server-side "object gone" assertion).
- Version gating: a client bound at v1/v2 receives no event its version lacks (fullscreen
  state since 2, parent since 3); a wl_output bound later gets its pending output_enter +
  a done; no event names an object the client destroyed (destructor hooks on manager and
  handle).
- set_rectangle: positive rectangle kept as a hint (last one wins) — nothing consumes it,
  code-reading only; 0×0 accepted and clears; negative width/height or exactly one zero →
  invalid_rectangle.
- Locked session: every request changes nothing (ignored, debug-logged); events keep
  reporting what the compositor itself does.

**Evidence, with artifact names:**
- ctest: baseline tree e80fc1a 148 registered / 143 green −5 bench (ctest-baseline.log);
  HEAD 149 / 144 green (ctest-final.log, 21:29 Nov 25); new test_foreign_toplevel_management
  (CMake TIMEOUT 60) 77 cases, 0 failed; scanner_golden + layer_graph green. No saved
  full-suite artifact postdates the three fix commits — at HEAD the targeted runs passed
  (`ctest -R test_foreign_toplevel_management test_window_protocols` 2/2) and each fix
  commit carries scratch-case red/green evidence in its message.
- Build without the protocol (macro undefined): full library build 1043 steps exit 0
  (noprotocol-build.log); the unit exports exactly install_wlr_toplevel_observer,
  wlr_toplevels_sync_all, wlr_toplevel_output_bound; no zwlr symbol in that library.
- Seat (each run under its own seat-lock ticket, notebook i915+nouveau, LUMEN_SMOKE_SCALE=1):
  drag-smoke 11 PASS / 0 WARN / 0 FAIL twice (drag-smoke-1/2.log); app-smoke 174 PASS /
  4 WARN / 0 FAIL twice (app-smoke-full-1/2.log, second on the final binaries) +
  `--only Q` 9/9 (q8-4). The 4 WARNs are pre-existing GTK rows (B6 ×3, G11) reproduced on
  a brokered clean-main e80fc1a run the same day (app-smoke-main-baseline-1.log) —
  off-change; the skill's documented 177/0/0 no longer reproduces (TODO row).
  Superseded: app-smoke-q8-1/2/3 are the dev runs that iterated to green (probe
  dispatch-loop fix, then a harness focus-window fix).
- Idle A/B (no client bound, serialized seat): 6 interleaved brokered rounds (3× main's
  build vs 3× branch build, ~20 s windows) — 2.10 vs 2.19 ms median CPU, 20/0
  voluntary/nonvoluntary context switches, 9→9 threads, both sides; the unit adds no
  event source/timer/fd and its sync entry returns before iterating
  (measure-idle.sh + logs).
- heaptrack (heaptrack-ftl.sh, heaptrack-analysis.txt): the unit's own allocations = 2
  idle (manager + first table block at bind/first map) and still 2 under traffic (293
  events received, 150 requests); the rest is libwayland's own event-closure allocation
  as for every protocol unit.
- Protocol provenance: XML only via ./scripts/fetch-protocols.sh (wlr-protocols, unpinned
  depth-1 clone; fetch-protocols.log), sha256
  a5a030786a9281e90542eb7c4084f0dbd6a50a7a2975cafa01fa848eebe6de15,
  upstream-before/after.sha256 saved; protocols/manifest.toml:196-199 exactly one
  [[wlr.protocol]] entry.

**Fuckups (2026-11-25):**
- The first heaptrack script signalled the wrapper pid instead of the compositor's — the
  compositor died mid-job and no evidence was produced in that state; the next seat
  ticket's preflight correctly refused the seat as contaminated ("an unbrokered job").
  Recovery: signal the child compositor pid, take a fresh ticket, rerun. When profiling
  the compositor under a wrapper, signal the child pid, never the wrapper; a dead
  mid-job compositor leaves the brokered seat dirty until cleaned.
- A first nanosecond-CPU idle pass was discarded for a helper bug and rerun clean —
  discard and rerun, never patch numbers.
- The packet's gate_runs seat lines said "exit 2" for both smokes — stale dev-run states;
  the final artifacts (drag-smoke-2, app-smoke-full-2) are PASS/exit 0. Cite the
  artifact, not the handoff line.

**Host/environment (2026-11-26, post-task analysis re-run):** the system upgrade incl.
libinput (Nov 26 21:11) drifts the suite: 6 session-lock cases of test_protocol_conformance
fail deterministically (3/3 re-runs) AND reproduce identically on main's own Nov-26
binary — environmental, attributed by clean reproduction (TODO row extended);
ui_bench_e2e_dry_run fails its seat preflight vs the user's live compositor (pid 978).
A gatekeeper seat re-run at HEAD on 2026-11-28 was likewise refused by the preflight —
the user's live session holds the seat.

**Residual limits (stated, not hidden):** seat evidence predates the three fix commits —
mitigated because the fixes' paths are not exercised by smoke row Q8 (it binds before the
windows map) and the two task unit tests pass 2/2 at HEAD; no committed test pins the
late-bind-activated scenario (scratch + review-reproduction evidence only); `set_rectangle`
as a kept hint is code-reading-only; no A/B bench was required (render_path false).
