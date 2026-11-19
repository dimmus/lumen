---
name: lumen-studio
description: Run Lumen's development studio — pick the right orchestration (lumen-development-studio workflow for non-trivial/multi-task changes, lumen-gated-change for one task, lumen-compositor-bench for cross-compositor measurements, or manual role agents from .zcode/agents/), launch/resume/amend the workflows, actively lead long runs (monitor, re-dispatch, answer holes, decide re-planning), and honor the binding rules (separate worktree per task, logical commits with PR text, positive work lands merged+pushed when gate-READY, seat-based verification, memory records). Use when coordinating a Lumen change end-to-end or when asked to run the studio.
---

> **Scope:** mixed — the orchestration pattern is general; the named workflows, role
> files, scripts and hosts are current-project. See §Porting.

# Lumen development studio (ZCode-native)

The authoritative harness lives under `.zcode/`. The DSH-era skills
(`.agents/skills/`, `tools/software-development-studio/`) were removed 2026-11-19 —
stale Docker-era duplicates of the role names; git history keeps them, and the live
deep catalogs were ported to `.zcode/skills/lumen-testing/references/`.

## Map

| Layer | Location | Contents |
| --- | --- | --- |
| Role agents (Agent tool / workflow personas) — the `.md` files are the single source of truth; workflow personas point at them | `.zcode/agents/*.md` | planner, implementer, reviewer, tester, test-analyst, gatekeeper, researcher, architect, doc-writer, screenshoter (`agent-screenshoter`) + the workflow-stage roles fixer, confirmer, memory-writer, and the eval pair skill-tester / skill-evaluator |
| Knowledge skills | `.zcode/skills/` | `lumen-build-run` (hosts/commands/measurement), `lumen-testing` (matrices/evidence/statuses, deep catalogs in its `references/`), `lumen-decisions` (settled; retrieval via the generated `.zcode/memories/INDEX.md`), `lumen-adr` (when a decision needs `docs/adr/NNN-*.md` and how to write it), `lumen-memory-writer` (archive format), `lumen-seat` (the seat broker — every seat job holds a ticket), `agent-screenshoter` (seat screenshots), this skill |
| Workflows | `.zcode/workflows/*.dwf.ts` | `lumen-development-studio`, `lumen-gated-change`, `lumen-compositor-bench` |
| Skill evals | `.zcode/evals/` | per-skill greenfield prompts (no clues) + rubrics; scores in `RESULTS.md` (see `.zcode/evals/README.md`) |

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
PR description artifact → **landing phase**: every gate-READY branch merges into main in
topological order, the merged tree re-runs the tests-enabled suite once, and only a
green merged main is pushed (with the task branches). Worktrees and branches survive
the run for inspection; a red merged tree is reset, never pushed.

## Lead duties (the lead is active, never fire-and-forget)

The lead (the session that owns the delivery) directs the run; workflows execute stages
but do not steer themselves. A passive lead parks blocked work silently — do not be one.

1. **Dispatch by shape**: read the request, pick the lane (table above), and for studio
   runs sanity-check the plan's task count/scopes against the request before letting it
   run — a plan that misses half the request is cheaper to fix at phase 1.
2. **Answer holes fast**: workflow holes (`proceed on red baseline`, `seat busy`, …)
   park the whole run on you. Decide or relay the user's decision immediately; never
   leave them hanging.
3. **Watch long runs**: check the board (`tasks` artifact) between phases; when a task
   parks (BLOCKED / NEEDS_REDESIGN), decide now whether the remaining plan still holds —
   re-plan the remainder or let dependencies skip with recorded findings. Two tasks
   parked on the same cause = re-plan, do not burn a third.
4. **Parallelize independent agents**: dispatch independent role agents in ONE message
   (multiple Agent/tool calls) whenever no data dependency forces ordering — parallel
   confirmations, parallel research + architecture assessment, reviewer of task N while
   task N+1's baseline builds. Do NOT parallelize builds on this 2-core host, and seat
   work always serializes through the broker.
5. **Land or report**: when the run ends, verify the landing phase actually pushed (or
   say why it did not), surface every finding with its disposition, and make sure the
   memory record landed. The run's report is the floor of your summary, not its ceiling.

## Binding rules (from the project owner)

1. **No containers.** Development runs in this VM natively or on real hardware (notebook
   sessions hosted by the user over SSH).
2. **No headless verification.** Compositor/display/input evidence comes from the seat;
   see `lumen-build-run`/`lumen-testing` for the exact policy.
3. **Every change is reviewed and tested; new functionality gets new tests.**
4. **Separate git workspace per task** (worktree + branch), logically complete changes
   committed separately with detailed messages; the workflow assembles PR text per task.
5. **Positive work lands** (owner standing rule, 2026-11-19) — gate-READY tasks are
   merged into main by the run (topological order; the merged tree re-gated; ff-only
   on the light path) and pushed over SSH. Anything below READY — BLOCKED (incl.
   BLOCKED-ON-HOST), NOT_READY, NEEDS_REDESIGN, regressions — is delivered as branch +
   evidence for the owner, never merged or pushed. No `gh` CLI on this host: the
   PR-text artifact plus the merge commit is the PR of record.
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

**Reading large files** (the repo's `.cppm`s run past 5,000 lines): grep for the symbol
first, read ±40 lines around the hits, `git log -L <range>:<file>` when history decides
the question; never `cat` a file over ~400 lines; never re-read a file after an edit —
the edit tool already verified the change. Memory retrieval goes through the generated
`.zcode/memories/INDEX.md` (grep the module slice) — never load the whole archive.

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

## Porting this harness to another CMake/C++ project

Every agent, skill and workflow carries a **Scope** line: `general` (port as-is),
`mixed` (port the skeleton, re-specify the marked parts), `current-project` (do not
port — re-specify from scratch). What must be re-specified when copying:

| Portable as-is | Re-specify per project |
| --- | --- |
| Role contracts' structure (scope bounds, independence rules, evidence vocabulary, bounded-auditor discipline, stop-on-regression) | each agent's *Read first* block (AGENTS.md, docs, decisions file) |
| The gated pipeline shape: baseline → implement → review → confirm → fix → verify → gate → memory → land | workflow internals: build commands, ctest labels, classifier (`scripts/classify-change.py`), worktree root, branch prefixes, broker/run-lock scripts |
| The broker *model* (one seat, ticket classes, priority queue, stale = owner decision, `acquired` line as proof) | `scripts/seat-lock.sh` preflight (compositor/smoke process names, loadavg), which jobs self-ticket |
| Memory archive format (frontmatter, ≤200-char rows, ≤120-line records, two-strikes rule, spool) | the archive contents themselves, `gen-memory-index.py` location, TODO rows |
| Eval harness design (no-clue prompts, isolated evaluator, 0/1/2 scoring) | every prompt and rubric — they test THIS project's skills |

Scope of the current pieces: role agents and `lumen-testing`/`lumen-seat`/`lumen-adr`/
`lumen-memory-writer`/this skill are `mixed`; `lumen-build-run`, `lumen-decisions`,
`agent-screenshoter`, the memories archive and all three workflows are `current-project`.
