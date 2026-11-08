# Task and evidence contracts

Use concise records in the task board or response; these are content contracts, not a requirement to create files.

## Work brief

- Objective and observable acceptance criteria.
- Repository/worktree, base commit and exact candidate/diff boundaries.
- Existing dirty/untracked work and whether it is an input or excluded.
- Read sources: applicable instructions, design/API contracts, decisions, relevant skills.
- Allowed write paths, non-goals, dependencies and shared resources to serialize.
- Required checks, measurement plan and division of responsibility for running them.
- Return: actual changed paths, implemented behavior, commands/results, risks and blockers.

For review/confirmation, write scope is empty. State read-only explicitly. Prompt-only read-only instructions are not a sandbox; use runtime restrictions if available and disclose the distinction when relevant.

## Finding

- ID, severity (critical/major/minor/note), path and line.
- Specific failure and concrete trigger/impact.
- Quoted evidence and relevant contract or reproduction.
- Confirmation: confirmed / refuted / unresolved, independent verifier and reason.
- Resolution: fixed / deferred / no change, with evidence and any required user decision.

Do not force a minimum number of findings. A suspected defect without deciding evidence stays a hypothesis.

## Verification result

- Check and requirement it establishes; required or optional.
- Tested state: base/candidate commit, or recorded dirty diff plus relevant untracked artifacts; identify subsequent edits that invalidate it.
- Host/container, toolchain, backend/device and meaningful conditions.
- Exact command, exit code, result artifact when any, and assertion/observed behavior.
- Status: PASS / FAIL / NOT RUN / UNSUPPORTED / INVALID (contaminated measurement).
- For measurements: before/after distributions and sample/warm-up counts; same conditions, separate backend baselines, uncertainty and comparison limitations.

## Final verdict

Reserve READY for a delivered change, only when all required checks passed on the final state and no blocking confirmed/unresolved findings remain. For plan-only or review-only requests, report that the requested analysis is complete and state its limits; do not imply implementation readiness. Otherwise use INCOMPLETE, FAILED, or BLOCKED with the concrete missing dependency. Optional checks not run remain visible. State what changed, why, review results, commands and evidence, and what is not established. Compilation is not runtime verification; screenshots are not latency evidence; a software GPU is not hardware Vulkan.
