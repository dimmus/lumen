# DSH Software Development Studio

A reusable set of DSH skills for evidence-driven software delivery, with Lumen as its first project adapter. It uses the existing Agent Teams, subagent, task, memory and command tools. No plugin installation or preset switch is required. It does not install hooks or enforce runtime permissions.

## Use now in Lumen

The repository's `.agents/skills/` is a standard DSH project skill root. DSH discovers the studio and testing bundles directly; project bundles take priority over user copies. Ask in ordinary language or select the corresponding skill command:

```text
Use software-development-studio to plan <change>; do not implement yet.
Use software-development-studio to implement <change> in Lumen with review and verification.
Use software-studio-review to review <exact diff/base>; do not edit files.
```

The coordinator loads `lumen-development-studio` in Lumen. For non-trivial implementation that adapter selects the existing `lumen-gated-change`, `lumen-reviewer`, `lumen-build-run` and `lumen-decisions` skills. Those existing user-level adaptations remain prerequisites; this package does not redistribute or overwrite them. Project memory remains optional for the reusable core and part of Lumen's declared process.

## Install the reusable core for other projects

Run from this checkout in PowerShell:

```powershell
./tools/software-development-studio/install.ps1 -WhatIf
./tools/software-development-studio/install.ps1
./tools/software-development-studio/install.ps1 -Check
```

The default destination is `$DSH_AGENTS_HOME/skills`, or `~/.agents/skills` when that variable is unset. Use `-Destination <skills-root>` for a custom DSH-scanned root. The installer copies only the two generic bundles, including their reference resources. It preflights collisions, leaves identical files alone, and refuses to replace differing files without `-Update`. `-WhatIf` previews writes. `-Check` compares installed files without writing. No files are deleted.

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
