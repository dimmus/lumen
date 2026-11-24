---
name: lumen-tray-menu-uaf
description: "2026-11-24 notebook: the 2026-11-18 tray-menu heap-use-after-free (sanitizer-sweep sighting) captured, root-caused and fixed — close_from() freed the level surface's ui_context inside the Enter's own delivery and ui_root::key's tail read it after (overlay.cppm:767); ASan seat repro 1/1 with a runtime-written report under the job's one class-smoke ticket, post-fix 30/30 clean, in-process test aborts pre-fix with the identical signature; ctest 141/141 both suites; brokered drag/app smokes PASS (N6 tray row); host: button injection dead 10:01–10:52, kernel evdev verified good, fresh-main A/B → environmental; review noise 5/5/0/1"
metadata:
  node_type: memory
  type: project
summary: "Tray-menu UAF fixed (a89cbad..63941df): close_from() freed ui_context inside its own key delivery; ASan seat repro 1/1, post-fix 30/30; injection dead-window 10:01-10:52; noise 5/5/0/1"
modules: lx.shell, lx.ui.host, tests, scripts
topics: asan, use-after-free, menu, tray, popup, seat

---

Verified 2026-11-24 on the **notebook** (iritur; card1 nouveau + card2 i915, read this
session). Task `shell-tray-menu-uaf`, worktree `/home/dimmus/lumen-wt/shell-tray-menu-uaf`,
branch `zcode/task/shell-tray-menu-uaf`, base a89cbad, HEAD **63941df** (13371bf the fix,
7bdd64d the TODO tick, 63941df the review guard), tree clean, diff = exactly the card's
5 files (511+/15−), gate READY. Evidence in the worktree's
`evidence/2026-11-24-shell-tray-menu-uaf/`, mirrored to
`/home/dimmus/lumen/evidence/2026-11-24-shell-tray-menu-uaf/` (git-ignored). Artifact
line references below were re-verified in those files this session.

- **The defect** (the sanitizer-sweep's open TODO row, [[lumen-sanitizer-sweep-green]]):
  `menu_popup::close_from()` destroyed the level's `lx.ui.host::surface` — and its
  `ui_context` — **while the menu's own input router was still delivering the Enter that
  activated the row**; the dispatch tail (`ui_root::key` → `overlays.after_input()`,
  `overlay.cppm:767`; the rule `input_router::delivering()` documents,
  `lx.ui.host/input.cppm:82-87`) then read the freed context → `heap-use-after-free READ
  of size 8`. The pre-fix release shell survives by luck; ASan dies.
- **The capture** (seat, ONE class-smoke ticket `ticket-20261124085605-524470-15104`,
  before/after `seat-lock.sh status` in `seat-ticket.log`, `LUMEN_SMOKE_SCALE=1`,
  compositor `--scale 1`): `repro-loop.sh` cycle 1 — right-click tray → `menu tray: open`
  → Down+Return → activate → closed → shell dead, 1/1.
  `run-1/asan-compositor.524627` (107 lines) is the **complete report written by the
  sanitizer runtime** (`ASAN_OPTIONS log_path= symbolize=1 fast_unwind_on_malloc=0
  abort_on_error=0` — the tool's knobs for the repro only; the committed
  `abort_on_error=1` stays the ctest env): READ at `overlay_layer::after_input`
  (overlay.cppm:767) ← `ui_root::key` ← `input_router::deliver_key`; freed by
  `surface::destroy`'s `ui_.reset()` (surface.cpp:217) ← `menu_popup::close_from`
  (menu_popup.cpp:462, pre-fix line) ← activate ← the Enter's own delivery; allocated by
  `surface::create` ← `open_level` (menu_popup.cpp:400). The filename says "compositor"
  because run-1's script had the two log_path prefixes swapped — the stack
  (`executables/lumen-shell/main.cpp:109`) proves it is the SHELL's report.
- **The fix** (13371bf + 63941df) — calendar_popup's rule applied to the menu:
  `close_from()` defers a level's teardown when that level's router is `delivering()`;
  the xdg_popup done listener defers instead of destroying its role mid-listener; both
  drain at the top of `after_dispatch()`; `close()` skips its tail while a full close is
  pending (the closed callback fires once); `open()` finishes a still-pending close
  before reusing level 0 and refuses with `log_menu.error` (static message) if called
  during delivery — the 63941df guard mirroring calendar_popup.cpp:101-104, unreachable
  for every current caller (panel/tray D-Bus/note-card paths verified by the reviewer).
- **Regression test**: `tests/test_shell_menu.cpp` popup-close lifecycle phase (the
  CMakeLists change is a 2-line link-deps addition to the existing target, no new
  executable): in-process compositor (CPU backend), client forked **before** the
  compositor exists, pumped the way the shell's loop pumps; drives the real wire through
  the menu's own router — Enter activate-close (the defect path), Escape, outside-click
  popup_done, reopen after each, callback accounting. `run-inprocess-prefix/
  asan-test-prefix.log` = the pre-fix tree aborting with the identical signature; green
  on every build post-fix.
- **Post-fix proof**: `run-8-postfix/summary.txt` = `cycles_run=30 clean=30 flakes=0
  report=none`, shell alive — 30× the reproducing count (ask required ≥5×). ctest
  141/141 on both suites (tests-enabled `-LE bench` and `ctest --preset asan`), run on
  the final tree by the gatekeeper (the packet's one caveat — no surviving full-run
  artifact on 63941df — closed by those runs). Brokered smokes green: drag-smoke ticket
  `ticket-20261124105225-613286-15477` VERDICT PASS; app-smoke ticket
  `ticket-20261124105300-614112-6824` VERDICT PASS 177/0/0 — its N6 row exercises the
  fixed path end-to-end (right-click → dbusmenu popup → Down+Return → Event clicked →
  closed; "maps 2" = a reopen through the guarded open()).
- **Harness traps, not product failures (seat runs 2–7)**: a `wait_log` line-mark race
  (the mark taken after the reaction landed) and the seat **dropping the first injected
  event(s) after a quiet spell** (app-smoke's N6 tray row's 1 px double-move is the same
  phenomenon) — the loop warms up before every injection group and retries a missed
  stage, counted as a flake, never as a failure.
- **Fuckup worth the class**: the run-1 log_path prefix swap made `report=none` land in
  the summary while the report existed — the capture succeeded only because the
  shell-death fallback (`shell_pid_alive=no`) was checked too. **A report detector's
  glob must match the writer's log_path prefix**; the stored script now agrees
  (compositor → `asan-compositor` :69, shell → `asan-shell` :114, checker globs
  `asan-shell.*` :182 — verified this session).
- **Host finding**: button injection died **host-wide** in the 10:01–10:52 window
  (motions unaffected; details in [[lumen-notebook-hw]]) — freshly rebuilt zero-diff
  main failed drag-smoke with the identical zero-button signature, so the failures were
  environmental, never attributed to this diff; the same tree+build passed brokered at
  10:52/10:53.
- **Decisions**: (1) ASan runtime knobs on the shell process for the seat repro only —
  not Lumen config/env surface. (2) Scope held in lx.shell despite the read landing in
  lx.ui: the free is lx.shell's close sequencing; the card's "READ of size 8 ≠ the
  sweep's strlen-shaped SUAR" note was right — no scope amendment. (3) The Mimosa hook
  declined the fork+execv in-process test idiom (test_clock_popup.cpp:91-94 precedent)
  as command-injection → the harness forks the client before the compositor exists, no
  exec needed.
- **Review noise (false-positive-rate trend):** raised 5, confirmed 5, refuted 0,
  preexisting 1 — the four confirmed in-round findings were fixed before HEAD (smokes
  re-run brokered green; repro prefix writer/checker agreement; README's impossible
  timestamp + the mirrored A/B logs; the open() guard), and the one preexisting finding
  is the already-tracked whole-file clang-format drift (`.zcode/TODO.md:117-123`).

**Disproven**: "the UAF is the sweep's retained-text-block class" — different signature
(pointer-sized READ from freed heap vs strlen-shaped stack-use-after-return) and
different mechanism (teardown inside delivery vs pointers into dead frames); "the
mid-job smoke failures were this diff's regression" — byte-identical signature on
freshly built main, then PASS brokered on the same tree.

**Why:** a use-after-free at "close" is a lifecycle question, not a memory question —
the menu was destroyed inside the event it was handling. When a handler can close the
thing dispatching to it, teardown must defer to the delivery boundary
(`after_dispatch`); and a sanitizer seat death is only diagnosable if the runtime writes
its own report — a truncated journal line is not evidence.

**How to apply:** repro a sanitizer seat death with `ASAN_OPTIONS log_path=<prefix>
symbolize=1 fast_unwind_on_malloc=0 abort_on_error=0` on the suspect process (tool
knobs, not product surface) and make the report detector glob the same prefix. Any
popup/menu close path: check `surface.input().delivering()` before destroying and defer
to `after_dispatch()` — calendar_popup and now menu_popup are the references. In-process
compositor tests: fork the client before `compositor::start()`.

Rule proposal: second recorded strike of the non-hermetic-broker-ctest class (first:
[[lumen-smoke-harness-broker-fixture]] — `ui_bench_e2e_dry_run`'s TIMEOUT 30 expires
whenever a seat job holds the lock; here the job's OWN held ticket queued ui-bench.sh's
self-wrapped seat-lock and it timed out in BOTH suites, passing in 0.16 s after ticket
release; TODO row `.zcode/TODO.md:145`). Mechanical check: a reviewer/gate checklist
grep for `ui-bench.sh|seat-lock` inside `add_test` blocks — any hit must set TIMEOUT ≥
the broker's DEFAULT_WAIT (900 s), or document "red while a seat job holds", or move to
a seat-less dry path.

Related: [[lumen-sanitizer-sweep-green]] (the sighting and the SUAR class this is NOT),
[[lumen-smoke-harness-broker-fixture]] (first strike of the broker-ctest class),
[[lumen-notebook-hw]] (the injection dead window, quiet-spell drops),
[[lumen-app-smoke-known-noise]].
