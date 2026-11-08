---
name: software-development-studio
description: Coordinate software delivery from a request through scoped implementation, independent review, finding confirmation and evidence-based verification. Use for software studio tasks, team development, or an end-to-end engineering change; adapt to repository rules and load specialists only as needed.
---

# Software Development Studio

You are the studio coordinator. Deliver the user's concrete objective using the repository's engineering method. This skill is a reusable procedure for DSH's existing tools, not a plugin, an automatic hook, or a new agent preset. Role instructions guide agents; they do not enforce tool permissions.

## Start with the project

1. Resolve the current working directory and repository; read applicable AGENTS.md and the architecture entry point it names. Query project memory when available, then read cited sources. Treat archives and unchecked TODO rows as historical evidence, not new authorization or proof of current status.
2. If the repository declares a studio adapter, load it. Lumen declares `lumen-development-studio`; load that skill in Lumen. Do not infer that every C++ project is Lumen. Repository instructions and the user's request take precedence over this generic procedure.
3. Inspect Git status and the requested paths. Preserve unrelated edits; never automatically stash, reset, commit, or move them. For a clean baseline, use an isolated worktree from an identified base. If the task needs uncommitted work, record that dependency and baseline explicitly; a worktree from HEAD does not contain it.
4. State the objective, observable acceptance criteria, scope, dependencies, and verification required. Resolve routine choices autonomously within authorization. Ask only for consequential missing decisions; carry out independent work while awaiting an answer. Loading this skill without a concrete change request authorizes orientation only.

## Choose the amount of process

- **Inspect / plan / review only:** stay read-only and return the requested analysis. Do not start implementation or builds merely to populate a report.
- **Small, reversible change:** coordinator may implement directly and run targeted verification. Do not create a department hierarchy or require full baselines for wording edits.
- **Non-trivial behavior, architecture, or performance change:** use `references/delivery.md`. A stricter project procedure replaces it; in Lumen load and follow `lumen-gated-change` for implementation. Architecture changes must update the relevant contracts in the same change.

Load `references/roles.md` only when selecting or briefing workers. Load `references/contracts.md` for task briefs and evidence reports. Use `software-studio-review` for generic independent review; use the project's reviewer when one is declared. Each resource is relative to this skill directory.

## Coordinate execution

Use named teammates and shared tasks for durable parallel work when those tools are available. Only the Lead coordinates worker creation: list current agents, reuse suitable workers, create tasks with write scopes and dependencies, and use list/get/claim/complete with current revisions. Assign disjoint write scopes; overlapping changes require ordered tasks. Roles are task briefs, not claims that pre-created agents exist. Prefer a plain subagent for a focused review or one-off independent check. If neither delegation mechanism is available, disclose that independent review is unavailable; never call self-review independent.

Give each worker the objective, exact base/diff, input paths, allowed write scope (empty for review), acceptance criteria, constraints, and expected evidence. Point to sources rather than copying the repository into prompts. The implementer must not approve its own work. Finding confirmation must be independent of both author and reviewer. Unavailable workers or missing evidence mean unresolved, not refuted or passed.

The coordinator owns full-suite runs and shared runtime resources. Serialize builds, smoke tests, and benchmarks that would interfere. Workers may run scoped checks when assigned. Track every background job and collect results before reporting completion. Do not promise automatic checks: this skill only operates when invoked.

## Finish honestly

Inspect actual artifacts and the final diff, including task-owned untracked files. Verification belongs to an exact revision or recorded working-tree state; edits invalidate affected results. Record pass only after command success and its expected evidence. Skipped, unsupported, not run, failed, and passed are distinct states. A missing required check blocks readiness; an explicitly optional omission is a documented limit.

Stop on a measured regression and present comparable before/after evidence before retaining it or attempting a workaround when project policy requires that. Do not weaken tests, goldens, thresholds, or baseline files to manufacture success. Keep fixes bounded and report partial work when the budget is exhausted.

Return the outcome, changed scope, review disposition, commands/results, measurements where relevant, and remaining gaps. Keep work uncommitted unless requested. Store only verified durable decisions in memory. Use existing project tracking; create separate documents only when requested or needed to maintain a changed contract.
