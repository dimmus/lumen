# Skill evals — greenfield tests for the `.zcode` knowledge skills

A skill is a prompt-engineering artifact; like code it regresses when edited. This
harness gives every knowledge skill in `.zcode/skills/` a small, fast, repeatable test
round, so a skill edit is judged by measured agent behavior — not by how good the edit
looked.

## Isolation model (the whole design rests on it)

Three parties, three information partitions — enforced structurally, not by politeness:

| Party | Reads | Never sees |
| --- | --- | --- |
| **Subject** (fresh general-purpose subagent) | the verbatim ASK + the skill's SKILL.md | the rubric, the SETUP rationale, that this is a test |
| **Tester** (`lumen-skill-tester`) | `prompt-<N>.md` (SETUP + ASK) | `rubric.md` |
| **Evaluator** (`lumen-skill-evaluator`) | `rubric.md` + the transcript (+ sandbox verification reads) | the SKILL.md under test, the run itself |

Structurally: the subject works in a sandbox — synthetic fixtures, or for repo-scoped
evals a **pruned copy** of the repo with `.zcode/evals/` excluded (the prompts and
rubrics spell out every trap's answer; a subject that can read them makes the eval
worthless). No eval ever points a subject at the real repo.

The ASK is written like a real user message: the trap lives in the scenario, never in
a hint. No prompt says what the correct approach is — if the skill does not teach it,
the eval must be able to fail.

## Layout

```
.zcode/evals/
  README.md                  this file
  RESULTS.md                 the score table (one row per skill, dated rounds)
  results/                   per-run score reports (evaluator-written)
  <skill>/prompt-<N>.md      SETUP (tester-only) + THE ASK (verbatim to the subject)
  <skill>/rubric.md          criteria + red flags + improvement hooks (evaluator-only)
```

Two prompts per skill (the standard round); **prompt-1 is the fast lane** for every
skill (each rubric's first prompt is the core-discipline test — run it alone when
time is short, both for a full round). Sandboxes are built fresh under
`/tmp/lumen-evals/<skill>-<N>-<ts>/` and never touch the real repo: synthetic
fixtures, or a pruned repo copy for repo-scoped evals.

## Scoring

Each prompt carries 5 criteria, scored 0 / 1 / 2 → total normalized to **/10**;
**PASS ≥ 7.0**. Red flags cap their criterion at 0. The skill's score for a round is
the mean over its prompts, recorded in `RESULTS.md` with the date and the skill-file
hash (`git log -1 --format=%h -- .zcode/skills/<skill>/`).

## Playbook

1. **When to run**: after ANY edit to a skill's SKILL.md, run at least its fast lane;
   after harness-wide changes, run the full round (both prompts, all skills). New
   skills arrive with their evals in the same change — an unevaluable skill is a
   finding, not an exception.
2. **Dispatch**: run `lumen-skill-tester` per prompt; independent prompts dispatch in
   parallel (one message, several tester agents). Score with `lumen-skill-evaluator`
   only after the transcript is complete.
3. **Judge the delta**: a score drop of **≥ 1.0 point** against the last recorded
   round is a regression — fix the skill or revert the edit (the no-degradation rule
   applies to the harness itself). A flat score after an edit meant to improve a skill
   means the edit is not carrying its weight; consider reverting it.
4. **Feed the rubric back**: evaluator improvement suggestions land as TODO rows or
   skill edits; the next round's scores decide whether they worked.
5. **Honesty over scores**: a round where every subject passes trivially means the
   prompts are too easy — harden the scenario, do not celebrate.

## What is deliberately NOT tested here

Role agents are exercised by every real workflow run (their evals are the studio's own
gates); the workflows are validated by live runs, not scripted prompts. This harness
covers the knowledge skills — the pieces agents load mid-run that no gate otherwise
exercises directly.
