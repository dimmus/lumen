---
name: lumen-studio
description: Run Lumen's development studio — pick the right orchestration (lumen-development-studio workflow for non-trivial/multi-task changes, lumen-gated-change for one task, lumen-compositor-bench for cross-compositor measurements, or manual role agents from .zcode/agents/), launch/resume/amend the workflows, and honor the binding rules (separate worktree per task, logical commits with PR text, ask before push, seat-based verification, memory records). Use when coordinating a Lumen change end-to-end or when asked to run the studio.
---

# Lumen development studio (ZCode-native)

The authoritative harness lives under `.zcode/`; the older DSH skills in `.agents/skills/`
stay as reference only — where the two disagree, `.zcode/` wins (notably: no Docker
anywhere, and no headless compositor verification).

## Map

| Layer | Location | Contents |
| --- | --- | --- |
| Role agents (Agent tool / workflow personas) — the `.md` files are the single source of truth; workflow personas point at them | `.zcode/agents/*.md` | planner, implementer, reviewer, tester, test-analyst, gatekeeper, researcher, architect, doc-writer, screenshoter (`agent-screenshoter`) + the workflow-stage roles fixer, confirmer, memory-writer |
| Knowledge skills | `.zcode/skills/` | `lumen-build-run` (hosts/commands/measurement), `lumen-testing` (matrices/evidence/statuses), `lumen-decisions` (settled; generated INDEX.md beside it), `lumen-adr` (when a decision needs `docs/adr/NNN-*.md` and how to write it), `lumen-memory-writer` (archive format), `lumen-seat` (the seat broker — every seat job holds a ticket), `agent-screenshoter` (seat screenshots), this skill |
| Workflows | `.zcode/workflows/*.dwf.ts` | `lumen-development-studio`, `lumen-gated-change`, `lumen-compositor-bench` |

## Choosing the orchestration

| Request | Use |
| --- | --- |
| Non-trivial change, possibly several tasks (feature, protocol, optimization) | `lumen-development-studio` workflow |
| Exactly one bounded task, want the light path | `lumen-gated-change` workflow |
| "How fast/heavy is Lumen vs sway/weston/hyprland" | `lumen-compositor-bench` workflow |
| Research, architecture assessment, a doc update, a small fix | direct Agent dispatch with the matching `.zcode/agents/` type (no workflow needed) |

A lead needing pixels from the live seat dispatches `agent-screenshoter` (its skill of the same name carries the verified commands) instead of improvising shell commands.

Launch a workflow with `CreateWorkflow` on its saved name plus args, e.g.
`saved: { name: "lumen-development-studio", args: { task: "..." } }`. A run this session
can be repaired mid-flight with `AmendWorkflow` (edit its script file, submit `path`) and
resumed with `ResumeWorkflowRun` when stopped. Subagent escalations (`dwfq-…`) and holes
park on you — answer them, relaying user decisions when needed; never leave them hanging.

## What the studio workflow does (per task)

run lock (`scripts/run-lock.sh` — one studio run per worktree root, heartbeat per
task) → plan → worktree (`../lumen-wt/<id>` — or `$LUMEN_WT_ROOT` — branch from the
agreed base; ALL predecessor branches integrated for diamond deps) → tests-enabled
baseline build+ctest (computed `ctest -N` counts) → implement (one implementer; commits
each logically complete unit with a detailed message) → required evidence derived from
the diff (`scripts/classify-change.py` — knobs add, never waive) → build+test fix loop
(≤3; two design-level escalations park the task NEEDS_REDESIGN) → independent read-only
review → per-finding independent confirmation (confirmed pre-existing defects are
recorded to `.zcode/TODO.md`, not fixed) → confirmed fixes + re-gate (≤2) → **seat
verification** (brokered drag/app smokes `--join-seat-session`, tolerance by exact
failing-row set; `ui-bench` when the diff is render-path) → read-only gate verdict
(READY/BLOCKED/NOT READY/NEEDS_REDESIGN; BLOCKED-ON-HOST for render-path on the VM) →
memory record in `.zcode/memories/` (on main or spooled to `zcode/memory-spool`) →
PR description artifact. Worktrees and branches survive the run for inspection.

## Binding rules (from the project owner)

1. **No containers.** Development runs in this VM natively or on real hardware (notebook
   sessions hosted by the user over SSH).
2. **No headless verification.** Compositor/display/input evidence comes from the seat;
   see `lumen-build-run`/`lumen-testing` for the exact policy.
3. **Every change is reviewed and tested; new functionality gets new tests.**
4. **Separate git workspace per task** (worktree + branch), logically complete changes
   committed separately with detailed messages; the workflow assembles PR text per task.
5. **Ask before push** — branches and PR text are delivered; pushing/PR creation happens
   only on the user's explicit go.
6. **Ask when deviating** — from a skill, a settled decision, or any requirement here;
   surface the question (escalation, `AskUserQuestion`) instead of improvising.
7. **No degradation** (AGENTS.md §1.2): any measured regression stops the task and the
   A/B evidence goes to the user.

## Token discipline

Prefer the saved workflows (typed results, cached/resumable, tail-capped outputs) over
ad-hoc multi-agent chats. Pass paths, not file contents, across agent boundaries. Repair
runs with `AmendWorkflow` + `path` edits, never full re-submissions. Reuse the
`lumen-decisions`/`lumen-build-run` skills instead of re-deriving their facts. Don't spawn
a confirmer on a finding a command already decided.

**Bounded auditors** (learned from the xdg-toplevel-drag run, where one bare-ask
confirmation of a heavy finding cost 3.9M tokens — the subagent bootstrapped its whole
context from zero, overflowed, and re-read after compaction): confirmers and gatekeepers
verify *cited evidence*, they never re-derive the change. The workflow guarantees this by
construction — the reviewer must emit a reading list (path:line + the one command) per
finding, the confirmer's ask carries the changed-file list and the exact diff command,
follow-ups are capped at one, an honest `inconclusive` return beats a grind, and repo-only
stages (confirmer/reviewer/fixer/gatekeeper/memory) run with zero web-search budget;
low-severity findings confirm on GLM-5.3-Flash. Keep these properties when editing the
workflows.
