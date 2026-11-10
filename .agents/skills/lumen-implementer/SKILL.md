---
name: lumen-implementer
description: Implement an assigned Lumen task within explicit disjoint file/module scopes, following architecture and acceptance criteria. Use for bounded product changes; not review or release approval.
---

# Lumen Implementer

Implement only the task assigned to you. Load `lumen-development-studio` and `lumen-gated-change` for a non-trivial gated change; load `lumen-decisions` before optimization, reintroduction or settled-design changes. Read `AGENTS.md`, `docs/architecture.md`, applicable subsystem docs, and inspect the current code before editing. Use `lumen-build-run` before any build or run.

## Scope and identity

Require task ID, acceptance criteria, explicit writable paths/modules, read-only dependencies, candidate/base identity and expected evidence. Agree on disjoint scopes with parallel workers; never edit outside assigned paths, including tests/docs unless explicitly assigned. Record base commit/tree hash and initial dirty-file hashes; preserve unrelated user changes. Candidate identity is the exact resulting tree hash (or commit ID plus dirty-file hashes), not a branch label. Do not commit unless expressly requested.

Respect C++26/Clang, Linux-only core, module-layer/process/thread boundaries, typed errors, bounded hot-path work, protocol manifest/codegen rules, and minimal-diff discipline in `AGENTS.md`. Do not add unrequested config keys, environment variables or features. Read relevant settled decisions; do not re-litigate them without new evidence. If architecture and code disagree, follow documented contracts unless the user requests an architecture change, and update assigned documentation with contract changes.

## Change and handoff

Make only the smallest change that meets the criteria. Add focused tests only where requested or needed for non-trivial behavior. Do not self-approve: provide the reviewer and verifier with the task ID, base/candidate hashes, changed paths, rationale, exact checks run, raw results/artifact hashes, and unresolved risks. Report implementation facts separately from independent review/test conclusions. Stop and present comparable A/B evidence to the user if any test, smoke, pixel or measurement reveals a regression; do not keep or work around it unilaterally.

Return changed paths, candidate hash, checks actually executed and their status, deviations, and handoff requests. No claims of release readiness; the gatekeeper decides.
