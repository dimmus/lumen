# Task Card and Candidate Evidence Contract

Use one shared task ID per independently deliverable result. The task board owns dependency, owner, state and revision. The card records context/evidence; avoid creating a second scheduler in Git or an invented status database.

## Required Task Specification

```text
Task ID: <DSH shared task id>
Objective: <one independently testable outcome>
Acceptance criteria: <observable, finite checks>
Base: <commit SHA + clean/dirty tree details>
Worktree: <absolute path + branch; one implementation owner per worktree>
Read scope: <files/contracts>
Write scope: <exact paths or modules>
Dependencies: <shared task IDs, only real prerequisites>
Lane state: proposed | baseline | implement | review | verify | findings/fix | gate | settled | blocked | paused
Owner: <DSH target or unowned>
Parallel resources: <CPU/build/container/GPU/seat/network/package locks>
Candidate: <commit/tree hash + dirty file hashes, initially none>
Required checks: <acceptance mapped to tests/evidence>
Artifacts: <fresh unique directory, append-only attempt paths>
Outstanding decisions: <user decision or none>
Last checkpoint: <UTC time, state revision and next action>
```

## Attempt and Evidence Identity

For every stage attempt record: task ID, attempt ID, stage, agent identity/role, start/end time, exact candidate/base IDs, exact command/argv and allowlisted overrides, host/container/image/compiler/build/backend/device, raw exit/terminal status, expected/actual checks and counts, result classification, artifact relative path + SHA-256, cleanup/restoration status, and limitations. Use immutable per-attempt evidence; never replace a failed attempt. Environment secrets must not be recorded.

Candidate identity is a clean commit SHA or a tree hash plus enumerated dirty-file hashes and untracked task-file hashes. A branch name, worker summary, commit message, task-board status, successful command exit, or lockfile is not candidate proof.

Any edit, merge, cherry-pick, rebase, generated file change or changed test result produces a **new candidate ID**. Review/test evidence for the old candidate stays historical and cannot satisfy the new candidate's stage. Merge between task lanes creates an integration candidate and reopens its impacted review/tests; do not call the union green because both parent branches were green.

## State Transition Receipt

Each transition is a small append-only receipt in the task output/card:

```text
from → to
actor + role:
UTC time:
base/candidate:
acceptance criteria affected:
inputs and dependencies consumed:
evidence refs + hashes:
findings/results:
resource released or held:
next owner + action:
```

A stage is complete only when its acceptance evidence reference and content hash are attached, and a distinct owner accepts its output. Missing worker response, tool error, timeout, or absent evidence remains blocked/unknown; it is not a false, green, or implicitly retried result. Retries get new attempt IDs. Reuse DSH shared task claims and revisions to prevent duplicate ownership.

## Findings Ledger

For each finding: finding ID; originating review/test/QA evidence; candidate hash; severity; source path/line or artifact+row; expected vs observed; status `open | confirmed | refuted | unresolved | fixed`; confirmer identity distinct from author/reviewer; reason and evidence; assigned fix task; resulting candidate; re-review and verification receipts. A large architectural/scope change becomes a child task with explicit parent link and its own base/baseline rather than endless fixes on the same task.

## Final Gate Receipt

Gatekeeper records exactly `READY | BLOCKED | NOT READY`, task/candidate hash, acceptance criteria table, baseline result, implementation receipt, reviewer identity/verdict, independent material-finding dispositions, verifier/QA commands and per-check statuses, final rerun after last edit, required document/CI deltas, regressions and user disposition, hardware/backend limits, and any residual risk/next owner. Gatekeeper is read-only. Only explicit user authority may waive an optional criterion; required Lumen policy (including no-degradation/regression escalation) is not waivable by a worker.