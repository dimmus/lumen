---
name: lumen-wlr-foreign-toplevel-management
description: "2026-11-25 notebook: zwlr_foreign_toplevel_management_v1 v3 served from lumen-compositor (new wire_wlr_foreign_toplevel.cpp + a third shell_bridge observer slot set_list_observer), commits 28db6b6..cde2e50, gate READY; ctest 148→149, new test 77 cases 0 failed, drag-smoke 11/11, app-smoke 174/4/0 (WARNs pre-existing GTK, reproduced on main), idle A/B 2.10 vs 2.19 ms CPU/20 s, heaptrack 2 allocs idle=traffic; landing owes ./scripts/fetch-protocols.sh on main first (gitignored XML, sha256 a5a03078…)."
metadata:
  node_type: memory
  type: project
summary: "zwlr_foreign_toplevel_management_v1 v3 from lx.compositor, 28db6b6..cde2e50 READY 2026-11-25; ctest 148→149 (77 cases), smokes PASS, idle A/B flat; landing owes fetch-protocols.sh on main"
modules: lx.compositor, protocols, scripts
topics: protocol, observer, taskbar, waybar
---

Verified 2026-11-25 on the notebook (i915+nouveau; /sys/class/drm: card0 nouveau, card1
i915), worktree `/home/dimmus/lumen-wt/wlr-foreign-toplevel-management`, branch
`zcode/task/wlr-foreign-toplevel-management` (base e80fc1a), 21-file diff, gate **READY**
at cde2e50. At record time (2026-11-28) the branch is NOT yet merged into main — landing =
merge + `./scripts/fetch-protocols.sh` on main FIRST + merged-tree re-gate. Evidence:
`evidence/2026-11-25-wlr-foreign-toplevel/` inside that worktree (evidence/ is untracked).
The detailed per-criterion semantics, full evidence artifacts, fuckup chronology and host
findings live in [[lumen-wlr-foreign-toplevel-management-closeout]].

**Built** (commits 28db6b6 feat, d7d0c9c test, 940a5f1 probe + app-smoke row Q8, a5608ca
docs; fixes d3a39a0, b36064b, f37303c; cde2e50 doc):
- New wire unit `modules/lx.compositor/wire_wlr_foreign_toplevel.cpp` serves
  zwlr_foreign_toplevel_manager_v1/_handle_v1 v3 for bars/switchers: per-handle diff
  (string-hash title/app-id, state bits, output, parent) closed by one `done` only when
  something was sent, bind batch bottom-to-top, `closed` at unmap → inert handle (requests
  ignored + debug-logged), `stop` → `finished`, v1/v2 clients get no event their version
  lacks, a late wl_output bind delivers the missed `output_enter` + `done`; requests
  (activate / close / minimize / maximize / fullscreen) call the same window_manager entry
  points the zlm bridge's do (window_manager::activate = unminimize + workspace switch +
  focus + raise, window_manager.cppm:4855-4865); set_rectangle: 0×0 clears it, negative
  width/height or exactly one zero → invalid_rectangle, otherwise stored as an unused
  hint; caps 4 managers/client / 32 total (refused bind → `finished`, failed alloc →
  no_memory); the session lock freezes every request; privileged=false, deny_sandboxed=true.
- Third observer slot — `shell_bridge::set_list_observer` (+ optional `updated` callback):
  a second `set_observer` call would silently have dropped ext-foreign-toplevel-list and
  the capture pickers (the shared-slot trap). Event-driven, no polling: two sync hooks ride
  existing entry points (wire_output.cpp output-bound + outputs-changed,
  wire_workspace.cpp workspaces_changed); compositor_impl.cpp untouched → classify =
  build/harness-scripts/logic/protocol/seat-surface, bench_ab false, render_path false.
- `activated` = the window manager's focus decision (keyboard_focus), not the literal key
  holder under an exclusive layer surface; none when no window has the keyboard.
  output_enter names the output whose active workspace lists the window; minimizing sends
  no output_leave (a bar keeps the restore button) — semantics confirmed by reading
  waybar's src/modules/wlr/taskbar.cpp; waybar itself was never run (not installed here —
  stated in ADR 009 and the user guide). No config key, env var or option; debug-only
  logging on one category (compositor.foreign_toplevel).

**Review noise 7/7/0 + 2 preexisting** (raised/confirmed/refuted; zero refuted): 4
confirmed findings fixed — d3a39a0 a late bind left TWO handles activated (bind now seeds
`g_activated = wm()->keyboard_focus()` at wire_wlr_foreign_toplevel.cpp:629 before the
opening batch; review reproduced red→green live); b36064b inert-handle requests now log
`result=inert` (:390-394); f37303c a refused/unallocatable bind says so (debug line +
no_memory); cde2e50 the user guide now states what `activated` names and that waybar was
not run. The 5th confirmed is the landing prerequisite below. The 2 preexisting went to
TODO rows: whole-file format debt (5th measured instance, 0 of 91 added lines at fault)
and the stale app-smoke baseline in the build-run skill.

**Evidence headline** (full numbers and artifact names in the closeout): ctest baseline
tree e80fc1a 148 registered / 143 green → HEAD 149 / 144 green; new
test_foreign_toplevel_management 77 cases, 0 failed (libwayland-client bar vs the
in-process compositor, two virtual outputs); scanner_golden + layer_graph green; the
library builds with the protocol undefined (unit exports exactly 3 stubs, no zwlr symbol);
drag-smoke 11 PASS twice, app-smoke 174 PASS / 4 WARN / 0 FAIL twice + `--only Q` 9/9
(the 4 WARNs are pre-existing GTK rows reproduced on a brokered clean-main e80fc1a
baseline); idle A/B 6 interleaved brokered rounds — 2.10 vs 2.19 ms median CPU per ~20 s
window, 20/0 context switches, 9→9 threads; heaptrack 2 own allocations idle and under
traffic; the XML came only from fetch-protocols.sh, sha256 a5a03078…

**Landing prerequisite (confirmed finding 5, open at record time):**
`/protocols/upstream/` is gitignored (.gitignore:11), so merged main lacks the XML: the
unit then builds out as the 3 stubs, test_foreign_toplevel_management prints SKIP (the
repo idiom), and test_window_protocols' two unguarded checks + app-smoke row Q8 go RED.
The landing must run `./scripts/fetch-protocols.sh` on main BEFORE the merged-tree
re-gate.

**Why:** a bar-facing protocol is mostly observer plumbing — the traps are the shared
observer slot (a second set_observer silently drops the previous observer) and state that
must be seeded at bind, not only maintained after (a late subscriber otherwise reports
stale `activated`). And a manifest entry's XML is invisible to the next checkout:
registration alone never makes a protocol exist.

**How to apply:** new bar-facing needs → extend toplevel_observer + the per-handle diff
(never poll). Any `protocols/manifest.toml` entry → plan the fetch on the target tree as
part of landing. A late-bound observer of existing state → seed the full snapshot at bind
(the :629 pattern) and pin it in the test. Signals into a profiler-wrapped compositor
target the child pid. Before reading their symptoms as new, check the TODO rows
"Whole-file clang-format debt" and "test_protocol_conformance session-lock rows".

Rule proposal: second archive occurrence of the gitignored-upstream trap
([[lumen-toplevel-drag-protocol-registration]] recorded 2026-11-12 "protocols/upstream is
gitignored, so the committed diff is the manifest block alone"; here it first BIT — stubs
+ red rows on a merged tree). Mechanical check: a diff touching protocols/manifest.toml
makes the landing phase run `./scripts/fetch-protocols.sh` on the target tree before its
re-gate and the re-gate fails if the new module's generated artifact is missing (or the
new test SKIPs) — enforceable as a landing-phase step keyed on `manifest.toml` in the
merge diff.

Related: [[lumen-wlr-foreign-toplevel-management-closeout]],
[[lumen-toplevel-drag-protocol-registration]], [[lumen-host-build-run-env]],
[[lumen-vulkan-capture-green]] (the ext-foreign-toplevel capture path shares the
observer plumbing).
