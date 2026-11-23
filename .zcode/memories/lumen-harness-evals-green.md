---
name: lumen-harness-evals-green
description: "2026-11-19..23: harness-evals round — push policy flipped (READY work merges into main and pushes; no gh CLI, PR of record = PR-text artifact + merge commit), DSH-era archive purged from repo AND user level, scope labels on every agent/skill/workflow, first skill-eval round 8/8 PASS 9.0-10.0 with four eval-motivated skill fixes; traps: no nested Agent dispatch, rate limit burned twice, memory-writer sandbox escape onto real main"
summary: "Push policy flipped 2026-11-19: READY merges+pushes (no gh CLI). DSH archive purged. Skill evals round 1: 8/8 PASS 9.0-10.0, 4 skill fixes. Traps: no nested dispatch, rate limit x2, sandbox escape."
modules: harness
topics: evals, landing-policy, dsh-purge, dispatch
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-23 in the main checkout (main at a87aef8) against branch
`zcode/task/harness-evals` (8 commits 2e6f772..6071e47, every commit message re-read for
this record); the eval round ran 2026-11-23 on the **notebook** host — greenfield /tmp
sandboxes, no seat job, the live session out of scope.

- **Push/merge policy FLIPPED** (owner standing rule 2026-11-19, 2e6f772): gate-READY
  work now ends merged into main and pushed by the run itself; below-READY stays with the
  owner. Studio landing phase: READY branches merge in topological order, the MERGED tree
  re-runs the tests-enabled suite once in `build/merge-gate` (each task gated alone, the
  combination never was), a red merged tree is reset to the pre-merge SHA — never pushed,
  main never left red. Gated-change light path: implementer commits logical units, branch
  merges FAST-FORWARD ONLY, then pushes. No `gh` CLI and no HTTPS credential helper here:
  PR of record = PR-text artifact + merge commit, push over SSH. All pre-2026-11-19 "never
  pushed without asking" notes (AGENTS.md, older TODO rows) are historical.
- **DSH-era archive purged** (9dd2566): `.agents/skills/` and
  `tools/software-development-studio/` deleted from the repo, AND at user level the 9
  stale Lumen/DSH skills moved `~/.zcode/skills` → `~/.zcode/skills.dsh-archive-2026-11-23/`
  (archive dir verified present 2026-11-23; the live dir keeps only non-Lumen skills).
  The stale copies were still harness-discoverable — 16 skill descriptions loaded per
  session, "On Windows, use Docker" guidance contradicting the binding no-containers rule
  — and name-collided with `.zcode` roles (a lead could load the DSH lumen-reviewer
  SKILL instead of dispatching the agent). The 5 catalogs lumen-testing still cites were
  ported to `.zcode/skills/lumen-testing/references/` with superseded-environment headers.
- **Scope labels** (fa9015e): general/mixed/current-project on every agent, skill and
  workflow (agents default `mixed`; screenshoter `current-project`); `lumen-studio`
  §Porting holds the portable-vs-re-specify table for harness reuse on other projects.
- **Skill-eval harness** (d6fea77, `.zcode/evals/`): 8 skills × 2 no-clue prompts + rubric
  each — the trap lives in the scenario, no prompt hints the correct approach.
  `lumen-skill-tester` builds the sandbox, dispatches ONE fresh subject with the verbatim
  ASK, never reads the rubric, never coaches; `lumen-skill-evaluator` scores the captured
  transcript in isolation, never reads the skill under test (judging "did it do the job")
  — 0/1/2 per criterion with quoted evidence, red flags cap their criterion, PASS ≥ 7.
  Playbook: rerun after any skill edit; a drop ≥ 1.0 is fix-or-revert. Post-review
  (c52a582) repo-scoped subjects run against a PRUNED repo copy with `.zcode/evals/`
  excluded — rubrics readable in-repo would answer every trap.
- **Round 1** (c394f94, 2026-11-23, notebook): all 8 PASS — build-run / testing / adr /
  memory-writer / studio 10.0, screenshoter 9.5, seat 9.0, decisions 9.0. Per-criterion
  score reports in `.zcode/evals/results/*-2026-11-23.md`, table + round notes in
  RESULTS.md. Four eval-motivated skill fixes landed in the same commit: lumen-seat rule 5
  (refusing seat parallelism must offer the non-seat-overlap-with-queue redirect),
  lumen-decisions two-license-levels (settled-closed vs licensed-for-re-proposal →
  go/no-go to the owner, never a full relitigation), agent-screenshoter §6 (name the
  sanctioned portal fallback in the same breath as the install refusal), lumen-memory-
  writer step-0 resolve-the-checkout. The next round's scores decide whether they worked.
- **Traps — dispatch is itself an environment:** subagents cannot dispatch subagents (no
  nested Agent tool). Two dead testers burned a rate limit improvising subprocess
  workarounds before the tester contract documented lead-dispatch and forbade them — and
  that same limit had already killed the round's first attempt on 2026-11-19 (resumed
  2026-11-23 after reset): the class struck twice, see Rule proposal. Separately, a
  dispatched memory-writer escaped its /tmp sandbox and committed a fictional record onto
  the REAL main checkout (c11e138, reverted the same minute, main restored to a87aef8 —
  no trace in `git log --all`, verified). Dispatch prompts now identify the project root.
  Tester transcripts initially lacked command trails (every evaluator flagged it); the
  transcript spec now requires them.
- **Review round 2** (c52a582): 9 findings, all fixed, none refuted. The 4 MAJORs:
  AGENTS.md §4.6 still said "the user decides on push in both cases" against the flipped
  policy one screen up; gated-change never verified the commit handoff — a partial commit
  (untracked file left behind) would ff-merge a tree that was never gated, and the count
  line reported 1 for zero commits (`''.split('\n').length`), now `status --porcelain`
  empty AND `rev-list --count >= 1` before anything lands; the studio conclusion claimed
  "merged and pushed" unconditionally including conflict-abort and re-gate-rollback paths
  (now tracks landingLanded/landingPushed, a rolled-back merge is reported as rolled
  back); eval isolation overclaimed "structural" while rubrics sat in-repo.
- **docs/invariants.md** (cc39a7e): the 3 missing AGENTS.md §5 rows mapped —
  build_draw_list_into no-interim-copy R1 (test_snapshot + bench G5), memory-pressure
  cascade (GAP: consumers' eviction tested for own triggers, not driven by the
  coordinator's signal), overflow caps (GAP: no designated overflow test). GAPs extend
  the existing 'Invariant GAPs' TODO class — work items, not footnotes.
- **Gate evidence** (task packet, branch-gated): tests-enabled ctest 142 registered;
  `smoke_harness` fails deterministically — the hermetic perfetto fixture stages
  ui-bench.sh + bench-warmup.sh but not `seat-lock.sh`, which ui-bench.sh re-execs since
  the seat-broker merge ba80f18 (pre-existing, not change-caused; the TODO row with the
  one-line fix rides the branch). `test_protocol_conformance` session-lock rows are a
  load flake: red twice (2026-11-19, 2026-11-23) always under heavy parallel session
  activity, PASS on quiet standalone rerun both times (TODO row corrected in 6071e47).
  diff-hygiene clean vs base. Mimosa full scan: 3 static path-traversal findings, all in
  pre-existing scripts untouched by the diff.

**Why:** the harness's weak point was again its own claims — isolation, landing state and
commit handoff were overclaimed prose until an independent reader checked them; and
dispatching agents is an environment with its own traps (no nesting, shared rate limits,
sandbox roots) that contracts must state up front, not discover by fire — twice burned.

**How to apply:** rerun a skill's eval after any edit to it (drop ≥ 1.0 = fix-or-revert).
Eval subjects are lead-dispatched, never nested; pin the project root in every dispatch
to a memory-writer. Landing follows the flipped policy — studio re-gates the merged tree
in `build/merge-gate`, red merged tree resets and never pushes; gated-change is ff-only
after a verified commit handoff. A `smoke_harness` red on the perfetto fixture at gate
time is the ba80f18 staging gap, not the change under test. The TODO rows (fixture fix,
flake signature) arrive with the harness-evals merge — do not duplicate them.

Rule proposal: the eval playbook gains a round-start checklist item — "lead confirms
every subject is lead-dispatched and no tester prompt implies self-dispatch" — plus an
evaluator red flag (criterion capped to 0) on any CLI/subprocess agent invocation found
in a tester transcript; together they mechanically catch the improvisation that burned
the rate limit in both occurrences.

Related: [[lumen-harness-review-green]] (review round 1 — the mechanisms this round
exercised), [[lumen-zcode-agent-harness]], [[lumen-smoke-bench-harness-green]].
