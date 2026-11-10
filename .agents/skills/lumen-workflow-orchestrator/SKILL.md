---
name: lumen-workflow-orchestrator
description: Coordinate production Lumen changes with independently advancing task lanes: baseline, implement, review, verify, confirm/fix, and gate. Uses DSH tasks, teams/subagents, isolated worktrees, evidence contracts, bounded retries and resumable status. Use when a user requests a Lumen implementation workflow, parallel agents, staged delivery, or gated change.
---

# Lumen workflow orchestrator

You are the accountable Lead. Convert one or more user outcomes into independent Lumen task lanes with explicit owners, candidates, evidence and per-task gates. This workflow is an instruction set for DSH tools, not an automatic scheduler, permission boundary or guarantee that a worker/model exists. Load `references/dsh-execution.md` for exact DSH primitive mapping and concurrency, `references/task-card.md` for task/evidence receipts, `references/role-map.md` for role selection, and `references/claude-code-parity.md` for current harness gaps and proposals.

## Scope and entry

- Load `software-development-studio`, `lumen-development-studio`, and for software implementation `lumen-gated-change`; read AGENTS.md, docs/architecture.md, relevant subsystem docs and current Git state. Query project memory and read cited sources when available; archive claims do not override live code/ADRs.
- For testing, load `lumen-test-agent`; load `lumen-script-runner` for repository scripts; require a separate `lumen-test-analyst` for substantial result analysis. This skill routes into role skills listed in `references/role-map.md`.
- Keep the user's exact objective and authorization. Planning-only stays read-only. No commit/push unless explicitly requested. A request to create workflow skills/docs is not authority to implement an unrelated product feature.
- Establish a session/workflow ID, task IDs, agreed base commit, dirty-state dependencies, host/container facts, acceptance criteria, resource limits and output directory. Record unknowns as unknown, not assumptions.

## Task lanes, not a global phase queue

Each task owns its own state machine:

`proposed → baseline → implement → review → verify → findings/fix → gate → settled`

Allowed sub-states: `blocked`, `paused`, `failed-needs-decision`, `abandoned-by-user`; preserve the last completed state and evidence. Review/verification/fix decisions apply to the exact candidate hash and invalidate downstream evidence if that candidate changes.

- Create a durable DSH shared task for each independently deliverable outcome. Write acceptance criteria, dependencies, owner, exact write scope, read inputs, base and expected artifacts into task description. Use `team_task_create/list/get/claim/update`; never infer a task started because it is ready. Claim with current revision. Use task dependencies only when semantically required.
- A task can advance as soon as **its own** prior state and prerequisites are satisfied. Do not wait for unrelated tasks in the same workflow. A broad external prerequisite such as one baseline build may be shared by same-base tasks, but attach its evidence to each consumer and verify that all run the same base/environment. A failure or design correction in task A does not automatically stall task B unless dependency evidence requires it.
- The workflow tool's `pipeline()` has no cross-item barrier but does run a serial stage chain inside each item; use it for independent mini-workflows only. Freeze/hash each candidate before review and verification; any later edit creates a new candidate and invalidates those receipts. For arbitrary partial dependencies use shared DSH tasks with explicit `blocked_by`. `parallel()` has a barrier: use it only when every result is required for one decision. Never assume workflow hooks supply filesystem, network, timers or arbitrary subprocess execution.
- For ordinary bounded work, use shared tasks and named teammates/subagents rather than invoking a giant workflow. DSH workflow orchestration is an available option, not mandatory overhead. See `references/dsh-execution.md`.

## Parallelism and models

- Discover the current host's **physical core** count (`Get-CimInstance Win32_Processor` on Windows, `lscpu` on Linux) and task-level limits. Set default concurrently executing CPU-heavy workers to `max(1, physical cores - coordinator reserve)`; only exceed physical cores for demonstrably I/O-bound work after checking memory, container CPU quota and provider/context limits. On this 2-core host the default is one concurrent CPU-heavy build/test worker plus coordination, not a swarm of ten. For read-only reviews/research, up to physical cores only when useful and platform limits allow. The coordinator can prepare independent tasks while a worker runs, but never start duplicate builds/smokes/benchmarks.
- Partition by **write scope** first, then resource locks. Every concurrent implementer must have disjoint files/modules. Test/build, GPU/DRM seat, output, network/package install, and baseline paths are shared resources; reserve with task notes/coordinator, serialize conflicts. Worktree isolation does not isolate CPU, GPU, KMS, package manager, shared build dirs or user session.
- Inspect `list_subagent_models` and use only routes/model IDs it returns. Do not invent Pi, Codex CLI, Claude Code, or model aliases from OpenRig when absent in DSH. If no role-specific model is exposed, use configured defaults. Where available, a strong general reasoning model suits lead/architecture/review, fast/reliable coding model suits implementer and low-cost model suits deterministic test triage; validate quality with acceptance/evidence, not branding. Record requested and effective route if the tool provides it; otherwise say not observable.
- The tested provider snapshot exposed `teamo-gateway/gpt-6.1-sol`, `teamo-gateway/deepseek-v4-pro`, and `teamo-gateway/claude-sonnet-5-5`; these are examples only, not hardcoded requirements. The route may vary in another session. A harness runtime (Pi, Codex CLI, Claude Code) and a model are distinct: if the corresponding DSH adapter/tool is available and authenticated, an assigned tester may use Pi, reviewer/Lead Codex, and builder Claude Code as user preferences; availability and effective model must be verified. Do not install or switch providers, copy credentials, or hide an unavailable preferred runtime. Fall back to an advertised DSH subagent route with disclosure rather than repeatedly launching a failed adapter.

## Isolation and baseline

- Inspect `git status`, current branch/HEAD, linked worktrees and remotes **before any write**. Preserve all dirty/user work. When implementation must not interfere with parallel development, Lead creates one distinct worktree/branch per implementation task using `create_worktree` from the agreed base. Never point two authors at one worktree even with prompts saying “separate files.” Record the absolute worktree path and commit before dispatch. Existing intentional dirty dependencies must be carried into a safe candidate or surfaced as a blocker; don't silently lose them by branching from HEAD.
- Baseline in each relevant build environment: discover Docker daemon/container/image/mount/compiler/build tree; ensure the source mount is the task worktree. Run required configure/build/tests on the agreed base before implementation. Store command, candidate/base SHA, environment, exact test count, skips and artifact hashes. Stop that task's implementation if baseline cannot establish a defensible result. A different task lane can continue if it does not rely on failed baseline evidence.
- Cache builds only when source, build configuration and toolchain identity match. Baseline/candidate artifacts do not overwrite one another. Hardware/seat checks are a separately reserved lane; a container/software renderer is not physical display proof.
- Worktree creation from HEAD does not inherit uncommitted changes. If a task depends on local changes, record exactly which and use a deliberate snapshot/commit/copy in its own branch only with user authorization; otherwise block and ask.

## Stage contracts

For the detailed handoff and status record fields, load `references/task-card.md`.

1. **Baseline:** planner + architect bound scope, identify contracts and hazards. Tester/runner execute only applicable baseline gates. Performance claims require matched before evidence. Record PASS/FAIL/NOT RUN/etc. Baseline red blocks only dependent tasks.
2. **Implement:** assign one coder/implementer per task worktree with exact allowed files, invariants, acceptance tests and output contract. Builder may run narrow checks, never claim full verification. Researcher/doc-writer/CI roles are separate tasks or read-only preparation unless scoped edits are assigned. No shared source edits by two workers.
3. **Review:** reviewer did not author the candidate and is read-only. Send exact base/candidate diff including untracked task files and task criteria. Reviewer returns severity findings with path/line, quoted evidence, behavioral risk, expected test and verdict. Review candidate only after an implementation snapshot exists.
4. **Verify:** test/QA verify exact candidate and recorded baseline according to task impact. Use repo-defined CTest/scripts/sanitizer/fuzz/profile gates; record full logs, exit, test counts and capability limits. Do not let review wait for an unrelated task. A review can run in parallel with other tasks' implementation; for the same task, an initial targeted verify may run in parallel only when it doesn't modify the candidate or compete for resources.
5. **Findings/fix:** gatekeeper triages, then an independent confirmer checks each material review finding. Confirmed findings return to the assigned implementer or a fresh fixer with exact evidence/scope; refuted remain documented, unresolved block readiness as policy requires. Every fix changes candidate identity, requires review of changed logic and a new verification. Large/architectural defects outside the bounded fix scope become a **new task ID and worktree** starting at baseline/implementation, linked to the parent; don't mutate task scope invisibly.
6. **Gate:** gatekeeper reads evidence, not just summaries. Confirm candidate hash matches review + tests, all required checks covered, no open critical/major finding, regressions shown to user, docs/contracts updated, no untested last fix, and exact required `lumen-gated-change` rounds/final gate done. Gatekeeper cannot edit or waive a user/project gate. Verdict: READY, BLOCKED, or NOT READY, with missing evidence and owner.

## Resumption and usage limits

- Maintain a compact workflow status in the parent response/shared task board and per-task card. Before compaction/pause/usage limit, checkpoint IDs, exact task states, owners, worktree paths, SHAs, active jobs, locks/reservations, last commands/results, pending decisions and next action. Persist only facts/evidence, not generated assumptions. Don't stop live workers or release task ownership just because a turn is ending.
- On resume, read the latest user request, goal state and cards, re-list tasks/agents/jobs, verify worker liveness and candidate hashes, collect only completed evidence, and continue at the earliest **per-task** unresolved transition. Don't recreate tasks or rerun expensive checks unless current candidate/environment changed or gate requires them.
- Tool failure, missing worker, provider quota or unavailable agent means UNKNOWN/BLOCKED; don't fabricate a report. Use `update_goal` only with direct user request for edit/pause/resume; complete only when the objective is truly achieved.
- Do not promise automatic pause on account usage limits. If a provider reports quota/limit or context is near budget, stop dispatching new expensive work, checkpoint, finish safe in-flight minimal work if possible and explain resume state. Re-arm a disarmed persisted goal when the user asks to continue.

## Recovery

- If a stage hits a large issue, capture its evidence, mark task `blocked` or `failed-needs-decision`, and state whether it is environment, scope, candidate defect, or uncertain. Try only bounded in-scope diagnosis. If resolution implies redesign/new work, create a child task with clean base and full acceptance criteria; do not endlessly patch or label the old task green.
- Never automatically revert, reset, stash, force-push, change baseline/golden/threshold/suppressions, kill unknown processes, alter system services/boot settings or accept regression. Preserve and report.
- On any measured regression under Lumen policy, stop and show the user comparable A/B evidence before retaining, working around or accepting it.

## OpenRig concepts and non-equivalences

Portable practices adopted from OpenRig: stable task/run/attempt identity, named role seats, explicit DAG dependencies and ready waves, per-task state, observable handoffs, resumable checkpointing, evidence receipts, bounded recovery and explicit completion ownership. OpenRig documents `RigSpec`, pods, edges, continuity and task dispatch at [OpenRig README](https://github.com/mvschwarz/openrig/tree/c38d1183cb192ce8ab9d27dc302afaefda860ab2), [RigSpec](https://github.com/mvschwarz/openrig/blob/c38d1183cb192ce8ab9d27dc302afaefda860ab2/docs/reference/rig-spec.md), and its [coordinator loop](https://github.com/mvschwarz/openrig/blob/c38d1183cb192ce8ab9d27dc302afaefda860ab2/skills/rigs/SKILL.md). Adapt concepts, not daemon/CLI/topology assumptions: DSH tasks, teams, subagents, workflow tool, goals and jobs are the actual primitives. Do not install OpenRig or introduce tmux/hooks/config for this skill workflow.

## Completion report

Summarize per task: owner, base→candidate SHA, worktree, current stage/outcome, implementation, independent review/findings, verification commands/results, gate decision, residual risks and next owner. Also list environment/parallelism constraints and tasks not ready. Report only what was observed. Do not claim “all green” based on a sibling task or task-board completed status without evidence.
