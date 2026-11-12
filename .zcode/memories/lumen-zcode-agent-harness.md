---
name: lumen-zcode-agent-harness
description: "2026-11-12: the DSH harness was transformed into ZCode-native agents/skills/workflows under .zcode/ (branch zcode/agent-harness); owner rules now bind — no Docker, no headless compositor verification, worktree per task with logical commits, ask before push; the plain release preset registers ONE ctest (LUMEN_BUILD_TESTS=OFF) so any suite claim must use build/test."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-12 on the vmware VM (branch `zcode/agent-harness`, commits d90ade5…de2f5ec):
the project's agent harness now lives under `.zcode/` — role agents (`.zcode/agents/*.md`,
9 roles with `disallowedTools`/`maxTurns` enforcement), skills (`lumen-build-run` rewritten,
`lumen-testing`, `lumen-memory-writer`, `lumen-studio`), and saved dynamic workflows
(`lumen-development-studio`, fixed `lumen-gated-change`, `lumen-compositor-bench`).
`.agents/` (DSH) is reference-only by the owner's decision; `.zcode/` wins on disagreement.

Owner rules recorded in AGENTS.md, binding for all agent work:

- No containers anywhere: vmware Arch VM native (this host) or the iritur notebook for
  hardware Vulkan in user-hosted SSH sessions. Never Docker-on-Windows.
- No headless compositor verification: compositor/display/input evidence only from the
  real seat (`--join-seat-session` smokes); unit ctest and in-process ui-bench are
  supplementary. Every change reviewed+tested; new functionality gets tests.
- One git worktree per task (`../lumen-wt/<id>`), logical commits with detailed messages,
  PR text assembled per task, the user decides on push. Ask when deviating.

Measured/verified facts that motivated the workflow shapes:

- `cmake --preset release` sets `LUMEN_BUILD_TESTS=OFF` → `ctest` on `build/release`
  sees exactly 1 test (`scanner_golden`). The old `lumen-gated-change` "full suite" gate
  was therefore vacuous. Tests-enabled: `cmake --preset release -B build/test
  -DLUMEN_BUILD_TESTS=ON` → 138 tests (`ctest --test-dir build/test -LE bench`).
- `cmake --preset release -S <worktree> -B <worktree>/build/test …` works from any cwd
  (verified via EvalWorkflowSnippet, configure 1.5 s warm); worktrees need
  `protocols/upstream` copied in before configure (gitignored).
- ZCode 3.18.x: `.zcode/agents/*.md` hot-load as Agent-tool types mid-session;
  frontmatter supports `disallowedTools`, `maxTurns`, `model`+`thoughtLevel`;
  workflows support per-subagent `model` in personas (studio's memory-writer runs on
  GLM-5.3-Flash), AmendWorkflow retunes a live run's concurrency, and runs
  resume/replay. No public 3.18.x changelog exists yet (checked zcode.z.ai/en/changelog,
  GitHub releases — newest published 3.17.2); the bundled dynamic-workflows skill is
  the authoritative surface and is byte-identical between 3.18.1/3.18.2.

**Why:** a future session would otherwise re-derive the whole harness split, re-discover
the release-preset test trap, or follow the stale Docker/headless guidance in the DSH
skills — each of which silently invalidates verification evidence.

**How to apply:** route work per `.zcode/skills/lumen-studio/SKILL.md` (studio workflow
for non-trivial/multi-task, gated-change for one task, compositor-bench for comparisons,
direct role agents for research/small fixes). Always gate on `build/test`, never
`build/release`. Keep memories flowing via `lumen-memory-writer` at each task's end.
Related: [[lumen-host-build-run-env]], [[lumen-no-degradation-rule]],
[[lumen-best-in-class-bar]].
