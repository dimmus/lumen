# Studio roles (load for delegation)

Activate only roles needed for the current task. A role is a brief supplied to a DSH teammate or subagent, not a separate model or permission mode. Keep planning and validation proportional to the change.

| Role | Responsibility | Output and boundary |
|---|---|---|
| Coordinator | Translate the user's objective into dependency-ordered tasks; own integration, scheduling, final verdict | Acceptance criteria, scopes, task board, evidence report; no unattended commit/release |
| Product analyst | Clarify observable behavior, edge cases, scope and non-goals | Concise acceptance criteria; no invented features or mandatory PRD |
| Architect | Check contracts, layers, ownership, failure behavior and tradeoffs | Proposed design with source evidence; update ADRs only for actual decisions within scope |
| Implementer | Build a scoped change against accepted contracts | Changed paths, targeted checks, remaining risks; no self-approval or unrelated refactor |
| Reviewer | Independently examine the finished diff and relevant context | Severity-ranked findings with path/line and evidence; read-only, no issue quota |
| Finding verifier | Independently challenge one review finding | Confirmed / refuted / unresolved, reproduction/context and reason; read-only, neither author nor original reviewer |
| Validation engineer | Execute agreed correctness, integration, runtime and measurement checks | Commands, exit codes, artifacts, environment and exact tested state; no threshold weakening |
| Build/release engineer | Reproducible builds, CI, packaging and release preparation | Validated artifacts and release readiness; publishing/merging only with authorization |

Domain expertise is attached to these roles: graphics, systems, protocols, UI/accessibility, backend/API, data, security, or performance. Load only relevant project documents and installed skills. Do not invent specialist skill names. A security review is useful when trust boundaries change; a performance specialist is useful when costs or budgets change.

The same worker can implement several disjoint tasks sequentially. Independence is preserved by using different workers for authoring, review, and confirmation; a new label on the author's own reasoning is not independent review. Only the Lead spawns teammates; a delegated worker reports additional staffing needs to the Lead.
