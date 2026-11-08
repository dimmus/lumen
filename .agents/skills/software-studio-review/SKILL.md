---
name: software-studio-review
description: Independently review a software change, or confirm a review finding, against its task, architecture and evidence. Use for read-only studio reviews and finding verification; use a declared project reviewer for project-specific review.
---

# Independent studio review

Read-only: do not edit files, install dependencies, update baselines, commit or push. Run only non-mutating checks; ask the coordinator to run checks that create artifacts. These instructions are not a runtime permission sandbox.

Require the original task, repository/base and exact candidate diff, task-owned untracked files, pre-existing edits, and relevant acceptance criteria. Discover applicable AGENTS.md and architecture/decision sources and read the code around the diff. If the review boundary is unclear, report the missing input; continue useful read-only inspection without pretending it is a complete review. In Lumen, load `lumen-reviewer` for review, while retaining the finding-confirmation mode below when assigned that mode.

## Review mode

Check correctness and concrete edge cases first, then ownership/lifetimes/concurrency, layering and interface contracts, failure handling, trust boundaries, performance budgets, regression evidence and scope. Confirm actual artifacts and tests rather than trusting the author's report. Separate pre-existing issues from introduced regressions. Read relevant untracked files explicitly; a Git diff alone omits them. Where the behavior or public contract changed, check associated documentation.

Return only actionable findings supported by the inspected code or a reproducible check. Each includes severity, path:line, trigger, impact, quoted evidence and the relevant contract. Distinguish an unverified suspicion from a defect. Do not impose generic style preferences against repository conventions or invent findings to meet a quota. End with a verdict, inspected scope, checks performed and material coverage gaps. No findings is valid and does not establish checks you did not run.

## Finding-confirmation mode

When assigned one finding, verify it independently of both author and original reviewer. Read the cited code and adjacent callers/contracts yourself. Look for context that refutes it and run an available non-mutating narrow check if it decides the issue. Return exactly one disposition with a reason:

- **CONFIRMED:** evidence establishes the stated failure; cite the trigger and code/check.
- **REFUTED:** evidence establishes why the finding does not hold; cite the controlling context.
- **UNRESOLVED:** evidence or environment cannot decide; name the missing check or input.

Do not fix the finding, turn lack of reproduction into proof of absence, or broaden the assignment into unrelated changes. The coordinator assigns repairs and final verification.
