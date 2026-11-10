---
name: lumen-gated-change
description: Coordinate one non-trivial Lumen task from clean baseline through implementation, independent review, evidence verification, bounded fixes and a final gate using Docker and DSH roles.
---

# Lumen Gated Change

This is the **per-task execution contract** used by `lumen-development-studio` and `lumen-workflow-orchestrator`. It applies to one independently deliverable task; it does not serialize unrelated task lanes. For multi-task parallel delivery, load `lumen-workflow-orchestrator` and let this skill define each lane's mandatory sequence. This is DSH guidance, not an automatic scheduler, security boundary, CI job, or permission grant.

## Preconditions and source of truth

1. Require a concrete user-authorized change. Read current `AGENTS.md`, `docs/architecture.md`, applicable subsystem docs and `.zcode` decisions; query project memory and read sources when available. Load `lumen-build-run` for any build/run/measurement and `lumen-decisions` before optimization/reintroduction/settled outcomes. Inspect actual current code and git status; archives/old commands are not host facts.
2. On this Windows host, use Docker for Linux C++26/Clang development. Discover current container, image, toolchain and exact bind mount; validate that source mount resolves to this task's worktree. Never assume container has KMS, logind, seat input, PMU, VKMS, or physical Vulkan.
3. Create/claim one shared task record for this deliverable if durable team work is in use. Name acceptance, exact base, write scope, owner, dependencies, resource reservations, rounds and artifact path. Claim by current task revision. Do not edit shared status without refreshing it.
4. Inspect dirt and linked worktrees before creating anything. Lead creates a task-specific branch/worktree when another task/checkout may be edited concurrently. A new worktree from HEAD excludes dirty inputs: preserve and report concrete dependencies; never auto-stash/reset/copy unknown edits. Use one editor per worktree and disjoint scopes among concurrent authors.

## Per-task stages

### 1. Baseline

Capture base SHA/tree and dirty dependencies. Discover missing protocol stamp and fetch only via approved repository script/network. Configure, build and run the required baseline suite in the appropriate environment. Record exact commands, container/source path, image/toolchain and per-test count/skips/results with immutable artifacts. Stop **this task** before implementation if the needed baseline is red or unverifiable; diagnose only within authorization. Unrelated lanes continue unless they depend on it. For performance-sensitive work, capture matching before-measurements; container results never establish hardware Vulkan/KMS coverage.

### 2. Implement

Assign one `lumen-implementer` (or scoped coding specialist) with task ID, base, worktree, acceptance and exact writable paths. Architect/research/doc/CI tasks use their role skills as needed and remain separately scoped. Implementer returns changed paths, candidate hash, rationale, narrow checks, risks and review focus. No commits/pushes unless user explicitly requested. Do not expand to unrequested features/config/env keys.

### 3. Review

Freeze a candidate identity (commit SHA, or tree + all relevant dirty/untracked hashes). Give original task, acceptance, exact base/candidate, diff and related files to a distinct `lumen-reviewer`, read-only. Require evidence-grounded severity findings and verdict. Candidate cannot be edited during review; if edited, invalidate this receipt and review the new candidate.

### 4. Verify

Select tests with `lumen-test-agent`; use `lumen-script-runner` for repository scripts; keep smoke, build directory, GPU/display and benchmarks serialized. A separate `lumen-tester` can execute a scoped matrix, but it must not overlap Lead-owned full suites/shared hardware. Preserve raw commands, exits, logs, actual test counts, skip/unsupported reasons, artifacts/hashes and cleanup. `lumen-test-analyst` is a distinct read-only role that interprets significant evidence; do not have runner analyze itself and claim independence. Status values remain PASS, FAIL, SKIPPED, UNSUPPORTED, NOT RUN, COULD NOT RUN and INVALID.

A frozen candidate may be reviewed and tested independently in parallel only where resources do not conflict; both receipts must name the same candidate hash. A merge/rebase, any edit, or generated output change creates a new candidate and invalidates relevant evidence. No check passes merely because its command returned zero; expected tests/rows/artifacts must be present.

### 5. Findings and fixes

A separate confirmer, not the author or original reviewer, decides material findings as confirmed, refuted, or unresolved from source/evidence. Record one finding receipt per item. Send confirmed findings to a bounded fixer with exact scope. Fix creates a new candidate; repeat impacted review and verification. Initial verify/fix budget: up to three verification rounds; confirmed-review-finding fix budget: up to two additional rounds, aligned with installed gated workflow. Do not leave a final fix unverified. If large error needs architecture or acceptance change, mark task blocked, explain evidence and create a linked child task with its own base/baseline/worktree rather than silently widening scope. Unresolved material findings block readiness.

### 6. Gate

A separate `lumen-gatekeeper` reads the full task packet; it cannot edit or waive criteria. It checks acceptance, candidate identity across review/tests/fixes, independent actors, all required outcomes, baseline-passing checks/goldens, docs/contracts, performance A/B, skips and residual risks. Verdict is READY, BLOCKED or NOT READY, never implicit green; READY does not authorize merge/publish/release. For any regression, stop and show the user comparable A/B evidence before retaining, working around or accepting it; only user can decide next direction under AGENTS.md policy.

Final full suite must run against the exact finished candidate and return expected evidence. Optional smoke/bench requests are honored only on a verified suitable quiet Linux seat; missing hardware is uncovered, never passed. Do not assume blanket historical smoke test counts. Report exact actual count/status. No commits or push unless requested.

## Bounded recovery and resume

- Tool/agent timeout or missing response is UNKNOWN, not proof of death, failure, or success. Inspect actual job/agent/task state; retain failed output; never duplicate a potentially live writer or runner.
- Infrastructure errors may receive bounded retry with new attempt ID after the cause is checked. Deterministic test failures are not retry-until-green. Rerun only when candidate/environment changed, new evidence decides a diagnosis, or final gate requires it.
- On context/usage pressure checkpoint task ID/revision, base/candidate hashes, state, owners, exact worktree, active jobs/locks, last commands/evidence, findings, pending decisions, rounds and next action. Leave safe active work intact. On resume revalidate candidate hashes and current task state; never reuse stale evidence.
- A failed task does not block unrelated tasks, but every dependent task must name and satisfy its actual dependency. Do not mark the global workflow done while any required lane is unresolved.

## Final handoff

Return task ID; base→candidate; worktree/branch; change summary; reviewer findings and independent disposition; exact test/QA commands/results/counts; artifact hashes; final gate verdict; measures where relevant; hardware/seat/CI not-covered limits; user decisions and next owner. Distinguish confirmed, refuted and unresolved. Do not claim worktree merge, commit, push, CI or deployment unless actually performed and authorized.
