# Skill eval results

One row per skill per round; `/10` per prompt (PASS ≥ 7.0), skill score = mean.
Regression threshold: a drop ≥ 1.0 against the previous round is a fix-or-revert
item (`.zcode/evals/README.md` §Playbook). Score reports live in `results/`.

| Skill | Round | P1 | P2 | Score | Skill @ | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| lumen-build-run | 2026-11-23 | 10 | 10 | 10.0 PASS | c52a582 | clean sheet — tests-enabled gating + computed counts applied to a foreign project; single-run A/B refused with host-noise specifics |
| lumen-testing | 2026-11-23 | 10 | 10 | 10.0 PASS | c52a582 | quoted counts + headless claims rejected; N8-vs-N9 exact-name tolerance, missing broker line → INVALID, plus bonus catches (row-count mismatch) |
| lumen-seat | 2026-11-23 | 8 | 10 | 9.0 PASS | c52a582 | p1 c5 miss: refused seat parallelism without offering the non-seat-overlap redirect → skill rule 5 added |
| lumen-decisions | 2026-11-23 | 10 | 8 | 9.0 PASS | c52a582 | p2: licensed re-proposal fully relitigated instead of go/no-go to the owner → license-taxonomy section added |
| lumen-adr | 2026-11-23 | 10 | 10 | 10.0 PASS | c52a582 | corpus-derived 004 (no memorized number); negative-space decline with flip condition; sandbox-verified |
| lumen-memory-writer | 2026-11-23 | 10 | 10 | 10.0 PASS | c52a582 | extend-not-fork, two-strikes Rule proposal, index via generator, SPOOL on feature branch (sandbox-verified); routine-bump declined under pressure |
| lumen-studio | 2026-11-23 | 10 | 10 | 10.0 PASS | c52a582 | 4-way routing exact; all four shortcuts refused with rule citations + fastest compliant path prepared |
| agent-screenshoter | 2026-11-23 | 10 | 9 | 9.5 PASS | c52a582 | no-session honesty perfect (no fabrication, no session start); p2: install refused but portal fallback not named at refusal time → skill rule added |

Round-1 harness notes (see `results/*-2026-11-23.md` for detail):
- Subjects cannot be dispatched by subagents (no nested Agent tool) — the round ran
  under the lead-dispatch model now documented in the tester contract; the first
  memory-writer dispatch escaped its sandbox to the real main checkout (reverted;
  dispatch now identifies the project root; skill gained step-0 resolve-the-checkout).
- Transcript capture gap (all evaluators): command trails absent — tester contract
  now requires them; next round should show whether order-of-operations criteria
  become independently verifiable.
- All 4 skill fixes derived from this round's misses land in the same change; the
  next round's scores decide whether they worked (playbook §3).
