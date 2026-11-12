---
name: lumen-toplevel-drag-protocol-registration
description: "2026-11-12: staging xdg-toplevel-drag-v1 (wayland-protocols 1.49) registered as lx.wayland.protocols.toplevel_drag (worktree commit d95abb1, manifest block only, no global advertised); suite 133 non-bench with only the owner-tolerated known-red {ui_gallery, test_task_model}; gate BLOCKED solely because seat smokes ran with the scripts' default LUMEN_BUILD_DIR=build/release (absent in the worktree) and died at binary preflight — rerun with LUMEN_BUILD_DIR=build/test."
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

Disproven theories: "exit 2 from a smoke = compositor/seat failure" — it was the binary
preflight; read run.log before interpreting exit codes. "Suite green = 138 passing" —
138 is the registered total including bench; the gating suite is 133.

**Why:** two traps in one small task — a manifest-only change still gates on seat smokes,
and smoke scripts silently pick a build dir the worktree doesn't have, producing
plausible-looking exit codes that mean nothing.

**How to apply:** before any `-smoke.sh` in a worktree, set
`LUMEN_BUILD_DIR=<worktree>/build/test` or build `build/release` first; check
`run.log` for the preflight FAIL line before citing a smoke result. Quote suite results as
"133 non-bench (138 registered)", with failures judged against the owner-tolerated known-red
set. Merge 578e08c (or re-apply its two-file doc fix) so the main tree's skill matches.
Related: [[lumen-zcode-agent-harness]], [[lumen-app-smoke-known-noise]],
[[lumen-host-build-run-env]].
