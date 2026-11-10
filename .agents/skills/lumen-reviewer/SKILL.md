---
name: lumen-reviewer
description: Independently review a scoped Lumen candidate read-only for correctness, architecture, regressions and evidence-backed findings. Use after implementation and before release gating.
---

# Lumen Reviewer

Review only; do not edit, run mutating commands, commit, or make the author's fixes. A separate agent identity must perform the review. This project skill intentionally adapts the user-level `lumen-reviewer` name for Lumen-specific review.

## Establish scope

Require the task ID, acceptance criteria, candidate and base revisions, exact read scope, and a clean mapping of changes to that task. Record candidate/base tree hashes (or exact commit IDs if clean commits); if dirty, record the relevant diff and file hashes. Do not infer candidate contents from a branch name. Read `AGENTS.md`, `docs/architecture.md`, applicable subsystem docs, `.zcode/skills/lumen-decisions/SKILL.md`, and `.zcode/skills/lumen-build-run/SKILL.md` when judging measurements.

Inspect the supplied diff and surrounding code. Check behavior, layer/process/thread boundaries, API/error contracts, resource lifetimes, hot-path constraints, protocol/codegen consistency, scope discipline, and required docs/tests. Verify claims against actual test or measurement artifacts; a command description or green summary is not evidence. Do not duplicate execution when reviewing code; identify missing evidence and use the assigned test specialist.

## Report

Return task ID and candidate/base identity, reviewed paths, and findings ordered by severity. Each finding states severity, file:line, violated criterion, concrete impact, and a bounded correction or deciding evidence. Separate verified, refuted, and unresolved items. Cite artifact paths and hashes for evidence claims. If clean, say no findings and state coverage limits. Never approve a measured regression: require stopping and presenting comparable A/B evidence to the user under `AGENTS.md` §1.2. Give a concise readiness verdict; only the gatekeeper makes the release decision.
