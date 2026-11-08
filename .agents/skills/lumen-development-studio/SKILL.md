---
name: lumen-development-studio
description: Apply the Software Development Studio to Lumen using its architecture, settled decisions, Docker development, independent finding confirmation and no-degradation gates. Use for Lumen studio setup, planning or coordinated delivery.
---

# Lumen studio adapter

Resolve this checkout with working_directory. Load `software-development-studio` if it has not already been loaded; its discovery step can load this adapter once without recursive reloads. This adapter selects Lumen's existing method; it does not replace or weaken it. A setup/plan request does not authorize an unrelated Lumen implementation.

## Sources and precedence

Read repository AGENTS.md and docs/architecture.md. Query `project-memory` and read cited sources; archives and .zcode/TODO.md may be stale. Current code and Git history establish implementation status; accepted ADRs and current instructions establish intended contracts. Report contradictions rather than silently choosing a convenient historical statement.

Load only applicable subsystem documents from AGENTS.md's required-reading map. The methodological sources are:

- docs/adr/004-architecture-evaluation.md: distinguish declared, scaffolded and incorrect behavior; repair structural contracts before feature expansion. Its original findings are historical, not a fresh backlog.
- docs/adr/005-desktop-roadmap.md: dependency-first phases that leave a coherent, independently shippable tree; consult current per-phase status.
- docs/adr/010-gpu-ui-engine.md §3 and §8: explicit acceptance budgets, stage-level measurements, controlled comparisons, backend parity and evidence-linked reporting. §8.5 amends the earlier p95 gate description in §8.1; use current scripts and the build/run skill to resolve executable checks.
- docs/subsystems/threading.md and memory-management.md: process/thread ownership, immutable snapshots, bounded lifetimes and capacities.
- docs/subsystems/errors-and-logging.md: typed failures, no placeholder success and no hot-path logging/I/O churn.

Paths above are relative to the repository root, not this installed skill directory.

## Delivery and team

For a non-trivial implementation, load and follow `lumen-gated-change`; use `lumen-reviewer` in an independent read-only worker. Confirm each finding with a worker independent of author and reviewer; `software-studio-review` supplies confirmation instructions. The archived .zcode/workflows/lumen-gated-change.dwf.ts is a reference, not runnable DSH code. The installed adaptation corrects historical path, status-reporting and verification-loop problems; do not execute or copy its old DSL.

Before optimizations, reintroductions or settled-design changes, load `lumen-decisions`. Before any Lumen build, run, smoke, benchmark or measurement, load `lumen-build-run`. Use Docker for development on this Windows host. Discover the actual container, source mount, compiler and build directory before running commands; archived VM/notebook facts are not facts about this Windows host. Do not hardcode a container name or claim a hardware capability from an archive.

Specialists, when needed: foundation/runtime ownership; Wayland/protocol integration; graphics/Vulkan/dmabuf; compositor/input/KMS; shell/UI/text; build/testing/measurement. Each receives disjoint paths and relevant sources. Keep compositor display state/execution, shell intent/UI, and application surfaces separate. UI mutates; render reads immutable snapshots; workers hand off via executors. C++26/Clang, Linux-only core, Vulkan-first and downward dependencies remain binding.

## Gates

- Preserve unrelated dirty work; identify the tested baseline. Never automatically stash, reset or commit. Worktrees from HEAD omit local changes; surface dependencies on those changes.
- All baseline-passing checks and goldens remain green. Performance/behavior claims need before/after evidence; optimizations with marginal or unproven gains are not accepted.
- Compare with relevant state-of-the-art systems deliberately, and support efficiency claims with measurements. Do not import another project's design by reputation alone.
- The Vulkan backend is the decisive backend per current AGENTS.md §1.2. A software Vulkan or container pass does not establish hardware Vulkan correctness/performance. Keep GL/CPU evidence separate and report unavailable hardware gates.
- Any observed regression requires stopping and showing the user comparable A/B evidence before retaining, working around or accepting it. Benchmark thresholds tolerate noise; they do not authorize smaller regressions.
- Serialize measurements against builds and other smoke/benchmark runs. Use same-host, same-backend interleaved A/B for fine comparisons; report distributions and actual metadata. Record unavailable seat/GPU checks as not covered, never pass.
- Derive test counts, supported backends and commands from the current tree/environment. Record successful exit and evidence before printing green. Keep verified/refuted/unresolved findings distinct; re-review fixes and verify the final state.

Use the existing gated skill's three initial verification rounds and two repair rounds. Report unfinished work at the budget. No new feature, config key or environment variable outside the user's request; minimal diffs and no commits unless requested. Update architecture/API/threading/UML documentation in the same change when its contract changes.
