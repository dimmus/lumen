---
name: lumen-toplevel-drag-protocol-registration
description: "2026-11-12: staging xdg-toplevel-drag-v1 (wayland-protocols 1.49) registered as lx.wayland.protocols.toplevel_drag (worktree commit d95abb1, manifest block only, no global advertised); suite 133 non-bench with only the owner-tolerated known-red {ui_gallery, test_task_model}; seat rerun with LUMEN_BUILD_DIR=build/test on the notebook — drag-smoke PASS, app-smoke exit 1 = documented ~140-PASS notebook baseline; review dedup dropped 578e08c (≡ d4e96d5); gate READY."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-12 on the vmware VM, worktree
`/home/dimmus/lumen-wt/toplevel-drag-protocol-registration` (branch
`zcode/task/toplevel-drag-protocol-registration`, based on e9c28e9; harness commit 1c9e0ff
landed on the main tree in parallel and is NOT an ancestor):

- d95abb1 (19:33) is a 6-line [[upstream.protocol]] block at `protocols/manifest.toml:282-286`
  — name "xdg-toplevel-drag", path `staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml`,
  source wayland-protocols, module `lx.wayland.protocols.toplevel_drag`; shape-identical to
  the xdg-dialog staging entry. `protocols/upstream` is gitignored, so the committed diff is
  the manifest block alone. Build glue only: no global advertised, clients see nothing new
  on the wire.
- Fetched XML (6884 bytes): two interfaces, no events, plain requests only — within
  wl-scanner-cpp's existing coverage, no scanner change. Error surface the codegen carries:
  `xdg_toplevel_drag_manager_v1.error.invalid_source=0`; `xdg_toplevel_drag_v1.error.
  toplevel_attached=0, ongoing_drag=1`.
- Codegen evidence: `build/test/generated/GeneratedProtocols.cmake` registers the module and
  appends `LUMEN_PROTOCOL_XDG_TOPLEVEL_DRAG_V1=1`; `compile_commands.json` shows that flag on
  the `generated/protocols/xdg-toplevel-drag-v1-protocol.c` TU of lumen-protocol-glue.
  `fetch-protocols.sh` touches `upstream/.fetch-stamp`, a configure dependency alongside
  manifest.toml, so the next build reconfigures itself after a fetch.
- Decisions: no CMake DEPENDS change for lx.compositor (glue arrives via lumen-protocol-glue
  PUBLIC definitions + generated include dir, same as xdg-dialog); no tests added —
  registration-only, no behavior exists to exercise (AGENTS §4.6); wire-level unit tests
  belong to the compositor-side tasks that follow.
- Suite evidence: tests-enabled build configure+build clean; `ctest -LE bench` → 2/133
  failed (`ui_gallery`, `test_task_model`) — the owner-tolerated known-red set (1c9e0ff,
  19:13, predates the task); implementer A/B (manifest stashed → same two failures) proved
  pre-existence. `ctest -N` → 138 registered total incl. 5 bench; `-LE bench` → 133.
- Review confirmed the skill's "138 tests" figure was wrong and "suite green" undefined →
  docs-only fix 578e08c (2 files, +8/-3) on the task branch: lumen-build-run SKILL.md:42 now
  reads "133 non-bench (138 registered incl. bench)" and defines suite-green as no new
  failures beyond the owner-tolerated known-red set. Note: as of this record 578e08c is NOT
  merged — the main tree's SKILL.md:42 still says "138 tests".
- Fuckup: the 19:49 seat smokes (drag-smoke.sh, app-smoke.sh `--join-seat-session`) ran in
  the worktree with the scripts' default `LUMEN_BUILD_DIR=build/release`
  (drag-smoke.sh:57, app-smoke.sh:437); the worktree only had build/test, so both exited 2
  at preflight ("build/release/lumen-compositor is missing"), every other log file 0 bytes,
  the compositor never launched. The gate read `drag-smoke-logs/run.log` +
  `app-smoke-logs/run.log`, invalidated the packet's "exit 2" results and BLOCKED — seat
  smokes are required for protocol-surface changes (lumen-testing skill). Fix: rerun with
  `LUMEN_BUILD_DIR=<wt>/build/test` (lumen-compositor + lumen-square verified present,
  19:37) or `cmake --build --preset release` in the worktree first. Still owed as of this
  record.

Update 2026-11-12 evening (gate pass; everything below re-verified this pass): the owed
seat rerun ran with `LUMEN_BUILD_DIR=<wt>/build/test` on the **notebook**, not the VM —
`readlink /sys/class/drm/card*/device/driver` → i915+nouveau and the logs show eDP-1
3200x1800 (Sharp panel), so the 20:00 section's "vmware VM" label was wrong for this
workflow (harness 5642621 added host detection for exactly this). drag-smoke
`--join-seat-session` → `VERDICT: PASS` (10 PASS / 0 FAIL rows, compositor exit=0,
`drag-smoke-logs/run.log`). app-smoke → exit 1 = the documented notebook baseline, not a
regression: 141 PASS / 35 FAIL rows (`grep -c 'FAIL'` says 36 — one hit is the
`VERDICT: FAIL` line itself), families = geometry/want-value rows embedding the VM's
geometry, portal frontend Z2–Z6, screenshot-less capture rows (Y/Q/O: "no screenshot"),
PSI "critical pressure" on the clean-log row — build-run SKILL.md:31-32: "app-smoke exits
1 with ~140 PASS rows". No A/B was required: `grep -ri toplevel_drag modules/ executables/`
→ 0 hits, the change has no runtime path. Gate verdict: **READY**. Review round: the
branch briefly carried 578e08c, patch-identical to d4e96d5 already on zcode/agent-harness;
fixed by resetting the branch back to d95abb1 (`git cherry -v zcode/agent-harness HEAD` →
only `+ d95abb1…`); the docs fix lives on the harness branch (main-tree
lumen-build-run SKILL.md:53 now reads "133 non-bench … (138 registered incl. bench)").
Artifacts re-verified: 307 TUs carry `LUMEN_PROTOCOL_XDG_TOPLEVEL_DRAG_V1` in
`build/test/compile_commands.json`; `build/test/generated/GeneratedProtocols.cmake:741-746`
registers the module; the glue `.o` exists; the fetched XML is cmp-identical to the pinned
1.49-tag clone (`/tmp/wp149-check`).

Disproven theories: "exit 2 from a smoke = compositor/seat failure" — it was the binary
preflight; read run.log before interpreting exit codes. "Suite green = 138 passing" —
138 is the registered total including bench; the gating suite is 133. "This workflow ran
on the vmware VM" — host detection and the eDP-1/3200x1800 log rows prove the notebook.

**Why:** two traps in one small task — a manifest-only change still gates on seat smokes,
and smoke scripts silently pick a build dir the worktree doesn't have, producing
plausible-looking exit codes that mean nothing.

**How to apply:** before any `-smoke.sh` in a worktree, set
`LUMEN_BUILD_DIR=<worktree>/build/test` or build `build/release` first; check
`run.log` for the preflight FAIL line before citing a smoke result. Quote suite results as
"133 non-bench (138 registered)", with failures judged against the owner-tolerated known-red
set. Judge app-smoke exit 1 against the host first (host detection, then the skill's
documented baseline for that host) before calling it a failure. The 578e08c/d4e96d5 dedup
is done — the doc fix lives on zcode/agent-harness as d4e96d5; when merging the task
branch, expect only d95abb1 to be new.
Related: [[lumen-zcode-agent-harness]], [[lumen-app-smoke-known-noise]],
[[lumen-host-build-run-env]], [[lumen-notebook-hw]].
