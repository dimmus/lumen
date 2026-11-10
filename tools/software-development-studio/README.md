# DSH Software Development Studio

A reusable set of DSH skills for evidence-driven software delivery, with Lumen as its first project adapter. It uses the existing Agent Teams, subagent, task, memory and command tools. No plugin installation or preset switch is required. It does not install hooks or enforce runtime permissions.

## Use now in Lumen

The repository's `.agents/skills/` is a standard DSH project skill root. DSH discovers the studio and testing bundles directly; project bundles take priority over user copies. Ask in ordinary language or select the corresponding skill command:

```text
Use software-development-studio to plan <change>; do not implement yet.
Use software-development-studio to implement <change> in Lumen with review and verification.
Use software-studio-review to review <exact diff/base>; do not edit files.
```

The coordinator loads `lumen-development-studio` in Lumen. It routes each non-trivial deliverable through project-scoped `lumen-gated-change`; multi-task/parallel delivery also loads `lumen-workflow-orchestrator`. Project role skills cover planning, architecture, implementation, review, testing, QA, evidence analysis, documentation, CI and gating. The host-specific `lumen-build-run` and `lumen-decisions` user adaptations remain prerequisites. Project memory remains optional for the reusable core and part of Lumen's declared process.

## Install the reusable core for other projects

Run from this checkout in PowerShell:

```powershell
./tools/software-development-studio/install.ps1 -WhatIf
./tools/software-development-studio/install.ps1
./tools/software-development-studio/install.ps1 -Check
```

The default destination is `$DSH_AGENTS_HOME/skills`, or `~/.agents/skills` when that variable is unset. Use `-Destination <skills-root>` for a custom DSH-scanned root. Without optional Lumen switches the installer copies only the two generic bundles, including their reference resources. It preflights collisions, leaves identical files alone, and refuses to replace differing files without `-Update`. `-WhatIf` previews writes. `-Check` compares installed files without writing. No files are deleted.

To also install the Lumen adapter globally, add `-IncludeLumen`; the installer requires the four existing Lumen adaptations in that destination first. The adapter is normally project-scoped, so unrelated projects do not need it. Other machines need their own DSH runtime, required capabilities and project-specific skills; copying the adapter alone does not establish a working Lumen build environment.

Project source files are authoritative. After editing them, inspect differences and rerun with `-Update`, then `-Check`. Live DSH discovery should see changes without restarting; verify by loading each skill. This is a skill collection, not a named preset in the preset picker.

## Lumen testing specialists

Three complementary project skills cover execution and independent interpretation:

| Skill | Responsibility |
|---|---|
| `lumen-test-agent` | CTest/presets, targeted tests, sanitizers, seeded fuzzing, benchmarks, Perfetto/perf/heaptrack and a broad Linux diagnostics catalog; task-needed package setup |
| `lumen-script-runner` | All 20 repository scripts, source-verified options, prerequisites, side effects, seat ownership, unique logs and reproducible evidence |
| `lumen-test-analyst` | Read-only independent analysis of provenance, coverage, raw failures, memory/timing evidence and comparable A/B results; actionable findings and next experiments |

Example requests:

```text
Use lumen-test-agent to select and run tests for <change>, recording uncovered hardware gates.
Use lumen-script-runner to run <script/groups> and hand the evidence to a separate lumen-test-analyst agent.
Use lumen-test-analyst to analyze <evidence-directory> against <baseline>; do not edit code.
```

The testing skill's references contain the test matrix, diagnostics catalog, setup instructions and evidence contract. Its Python helpers check/plan/install selected development tools and capture bounded non-seat command argv, logs, exit status, timestamps and hashes. They do not infer PASS from exit zero or supervise a graphical session. Seat tests still require the coordinator's assigned Linux display/input; Docker software-renderer checks do not establish hardware Vulkan or KMS coverage. The script runner explicitly handles sourced libraries, templates, generators and boot-affecting installer options.

Project discovery needs no installation. To copy the testing suite into a user skill root (including its references/helpers), use the existing installer with `-IncludeTesting`:

```powershell
./tools/software-development-studio/install.ps1 -IncludeTesting -WhatIf
./tools/software-development-studio/install.ps1 -IncludeTesting
./tools/software-development-studio/install.ps1 -IncludeTesting -Check
```

This option also installs the generic studio core and requires the existing `lumen-build-run` adaptation in the destination. It does not overwrite other Lumen skills. Use `-Update` only after reviewing differing installed files. `-IncludeLumen` remains a separate option for the studio adapter. An independent analyst must be a different agent; using two skills in one agent does not create independent review. Skills guide execution and reporting, and do not install automatic hooks or tool permissions.

## Lumen workflow harness

Invoke `lumen-development-studio` normally, or name `lumen-workflow-orchestrator` for parallel delivery. Each task advances independently through baseline → implement → independent review → verify → confirmed fixes → gate. Task A can finish while task B is still implementing. Real dependency edges, candidate identity and shared resources control readiness, not a global phase barrier. A substantial new defect becomes a linked task with its own baseline/worktree; repair loops remain bounded.

The orchestrator's role map selects `lumen-planner`, `lumen-architect`, `lumen-researcher`, `lumen-implementer` (coder/builder), `lumen-reviewer`, `lumen-tester`, `lumen-qa`, `lumen-gatekeeper`, `lumen-doc-writer` and `lumen-ci-manager`, together with the existing test/script/analysis specialists. A role is a focused instruction bundle, not a permanent worker or permission guarantee. One person/agent invoking multiple skills does not establish independent review.

Use the current physical core count, RAM, container quotas and provider budget to bound workers. This host has two physical cores/four logical processors; default to one CPU-heavy build/test worker plus coordination, with useful read-only review/research overlap. Separate worktrees isolate source edits but not CPU/GPU/seats/build directories/package managers. Lead creates one worktree per implementation owner and reserves shared resources. All DSH task IDs, claims/revisions, candidate hashes and artifacts belong in per-task cards. The orchestrator references document exact tool mapping, task receipts, quota/context checkpoints and safe resume.

Models are discovered with `list_subagent_models` at execution time. Pi/Codex/Claude Code are runtime preferences, not model IDs or guaranteed DSH routes. Use an available authenticated adapter when present and report effective route if observable; otherwise use an advertised DSH subagent model with disclosure. Never hide a missing provider, infer quality solely from brand or repeatedly retry a broken adapter.

```text
Use lumen-development-studio to implement <outcome> with independent task lanes and Lumen gates.
Use lumen-workflow-orchestrator to plan <tasks>; do not implement yet.
Resume the Lumen workflow from <task/evidence IDs> and revalidate candidate state before continuing.
```

For user-level installation of the complete workflow/role/testing suite:

```powershell
./tools/software-development-studio/install.ps1 -IncludeWorkflow -WhatIf
./tools/software-development-studio/install.ps1 -IncludeWorkflow -Update
./tools/software-development-studio/install.ps1 -IncludeWorkflow -Check
```

Review differences before `-Update`: the workflow option intentionally copies the new project `lumen-gated-change` and `lumen-reviewer` adaptations, plus adapter/roles/testing/core. It requires `lumen-build-run` and `lumen-decisions` already installed; it does not copy credentials, install OpenRig, modify runtime permissions, create GUI plugins, enforce CI, or auto-deploy. Project discovery needs no global installation. `-IncludeLumen` without `-IncludeWorkflow` retains the existing prerequisite check path.

OpenRig-informed design and a concrete Claude Code harness parity/gaps proposal are in `.agents/skills/lumen-workflow-orchestrator/references/claude-code-parity.md`. Skills implement the coordination procedure; automatic hooks, quota monitoring, crash-safe dispatch and hard permission enforcement would require separate DSH runtime/plugin/CI work. These are proposals, not features claimed by these files.

## Adopt another repository

Install the reusable core, then invoke it in that repository. It reads that project's AGENTS.md, architecture and decisions. For special rules, create a uniquely named project skill under `.agents/skills/<project>-development-studio/SKILL.md`, name it as the studio adapter in that project's AGENTS.md, and define its sources, build environment, reviewer and required gates. Never copy Lumen's Linux/Vulkan constraints into an unrelated project.

## Delivery and independence

The coordinator selects the shortest process consistent with project policy: read-only orientation/review, a targeted small-change path, or a gated non-trivial implementation. The role roster, delivery procedure and task/evidence contracts live in the generic coordinator's `references/` directory and load only when needed.

For significant Lumen work:

```text
Identify scope and baseline → implement → build/test → independent review
→ independent finding confirmation → minimal fixes → final verification → evidence report
```

Author, reviewer and finding verifier are independent workers. The Lead coordinates full suites and shared runtime resources. Prompt instructions are not a read-only sandbox. If independent workers or required verification are unavailable, report the gap instead of calling the change ready.

All checks belong to an identified candidate state. A pass requires actual successful execution and expected evidence; skipped, unsupported and invalid checks remain visible. Lumen measurements follow same-host/backend interleaved A/B and its no-degradation escalation rule. Container checks do not establish a physical display or hardware Vulkan result. Routine authorized edits proceed without repeated approval; scope changes and project-required regression decisions remain with the user.

## Validation of this package

Load all three skills in DSH to verify real catalog discovery and current instruction loading. Run the installer and its `-Check` comparison to verify the user copy. Review these decision cases after changing instructions: plan-only stays read-only; a wording edit avoids full builds; dirty work is preserved; a failed final command never reports pass; unavailable hardware remains unverified; a finding without deciding evidence remains unresolved; an unverified final repair blocks readiness. These are instruction acceptance cases, not claims of automated behavioral enforcement.
