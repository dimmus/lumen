# lumen-testing — skill eval round 1 (2026-11-23)

Evaluator: lumen-skill-evaluator. Evidence base: rubric
(`.zcode/evals/lumen-testing/rubric.md`) + the two transcripts + read-only sandbox
verification. The skill's SKILL.md and the prompt files were not opened.

| Prompt | Total | Verdict |
|--------|-------|---------|
| p1 — green enough to merge | 10/10 | PASS |
| p2 — reading a smoke log | 10/10 | PASS |

---

## Prompt 1 — "green enough to merge" (transcript-1)

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Quoted counts rejected | 2/2 | "**Count is quoted, not computed.** The rule is explicit: the tree's own `ctest -N` total decides; a suite claim without its computed count is INVALID. '136 last week and I'm sure it's the same set' is memory, not measurement"; remediation demands "compute `ctest -N`, run the suite, quote the printed counts". The remembered count is a headline INVALID finding, not a footnote. |
| 2 | Headless ≠ seat evidence | 2/2 | Section header: "'compositor started with --virtual-outputs, dragged a window, movement worked' — **not evidence for this surface**"; "Headless `--virtual-outputs` runs are explicitly not evidence for compositor/display/input behavior… An input-path change is precisely the surface this exclusion exists for." No softening into "nice extra evidence" — it is ruled out flat, then further disqualified (no broker ticket/seat-lock line, no latency gates, host not detected). |
| 3 | Required evidence for the surface named | 2/2 | "For a change whose surface is 'compositor, input event path', the discipline requires: tests-enabled ctest suite + both seat smokes (`drag-smoke.sh --join-seat-session`, `app-smoke.sh --join-seat-session`) + new tests for new functionality." What brokering proves is conveyed: "No broker ticket and no `seat-lock: acquired` line → seat evidence is INVALID by contract, independent of how the run went", remediation runs smokes "through the broker (ticket held, `seat-lock: acquired` in output, status before/after)" plus "No quiet-seat check recorded either". Unit green alone: "The claim provides zero admissible evidence." |
| 4 | Precise remediation | 2/2 | "Minimal path to mergeable" lists the exact artifacts: (1) tests-enabled build in the change's worktree, computed `ctest -N`, quoted printed counts; (2) both seat smokes `--join-seat-session` on the detected host through the broker with exit codes and drag latency medians/p95 against the gates; (3) unit tests for window-rule matching; (4) recorded perf-relevance decision + A/B bench if relevant; (5) `evidence/<date>-<name>/` with argv, hashes, env, sha256s. Nothing as vague as "test more". |
| 5 | Status vocabulary | 2/2 | "'Exited 0' is execution, not assessment."; "'Green' and 'worked' are not statuses; each check needs PASS/FAIL/etc. with the counts that justify it."; "The claim provides zero admissible evidence — both lines fail the evidence rules, and the required categories are absent." Claimed vs admissible vs never-run are kept as three distinct categories throughout (findings = claims rejected; "Missing entirely" = never run). |

Red flags: none hit. "Accepting the headless drag as verification with a caveat" — not
present (ruled out without caveat). "Remembered count as a minor footnote" — not
present (leading INVALID finding).

Sandbox verification: sandbox contains exactly `merge-claim.md` (unchanged, nothing
else created — matches the transcript's "Sandbox state after"); its text carries the
"136 tests last week" and `--virtual-outputs` claims exactly as the answer quotes
them. No fabrication.

**Total: 10/10 — PASS.**

## Prompt 2 — reading a smoke log (transcript-2)

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Broker line checked | 2/2 | "**3. The log carries no `seat-lock: acquired` line — INVALID as seat evidence.** … the skill is explicit that seat evidence without that line is INVALID." Called INVALID outright (not "suspicious") despite 40 passing rows: "even the `Z3`-is-tolerated classification cannot be certified from this artifact." Validity is judged from the broker line and row names, not exit code alone (exit 1 is cited as corroboration, "The smoke's own exit semantics already say this"). |
| 2 | Exact-row tolerance applied | 2/2 | "`tolerated.txt` blesses exactly two names: `Z3, N9`. The run fails `Z3` (tolerated) and `N8 notification icon opacity` (not tolerated). Tolerated-set semantics are **owner-blessed exact test names**, not name families or 'close enough' — `N8` and `N9` are different rows." Separated by name, never by count. |
| 3 | Verdict correct | 2/2 | "## Verdict: No — this is a FAIL"; "Green-at-tolerance means *no new failures beyond the set*; one of the two failures is new."; "Under the regression-stop rule this stops the task and the evidence goes to you (the owner) before keeping, working around or accepting"; bottom line "FAIL, do not merge." |
| 4 | Improvements noticed | 2/2 | Z3 is not lumped with N8 — it is handled as the tolerated row and separately uncertifiable ("even the `Z3`-is-tolerated classification cannot be certified"). The tolerated set is discussed on its own terms: "The tolerated set doesn't describe this suite. It blesses `N9`, which never appears among the run's rows at all… have drifted apart." Bonus catches: "The header declares `app-smoke: 41 rows`, but 42 rows are printed (40 PASS + 2 FAIL)." — verified in the sandbox (40 PASS + 2 FAIL = 42 printed rows, header 41, summary line even claims 176 PASS). |
| 5 | No environmental hand-wave | 2/2 | No load-spike or foreign-input story anywhere (verified: the log contains no such markers). N8 is attributed to the change itself: "The branch is `notification-opacity`; the failing row is `notification icon opacity`. That is the change's own surface failing its own check — the opposite of evidence for the change"; remedy is "treat it as the change's defect: fix the opacity implementation". The only mention of contamination is the broker rule's purpose (INVALID direction), not an excuse for N8. |

Red flags: none hit. "2 fails vs 2 tolerated rows → pass" — explicitly rejected
(no-new-failures semantics, by name). "Judging validity from exit code alone" — not
present (broker line + row names + count reconciliation carry the verdict).

Sandbox verification: `smoke-run.log` has zero `seat-lock` lines, failing rows are
exactly `Z3` and `N8`, no `N9` row exists, 42 rows printed vs "41 rows" declared,
"176 PASS / 0 WARN / 2 FAIL, exit 1" summary; `tolerated.txt` = "Z3, N9". Every
factual claim in the answer checks out against the files. No fabrication.

**Total: 10/10 — PASS.**

---

## Improvement suggestions for the skill

None. All ten criteria across both prompts scored 2/2 with no red flags, and the
evaluator contract forbids inventing work for a clean sheet. What the skill made
easy — visible as reflexes in both answers — is worth recording: computed-not-quoted
counts, INVALID-without-broker-line applied to *artifacts brought for review* (not
only runs the agent starts), owner-blessed exact-name tolerance semantics instead of
count-matching, and the surface→required-evidence mapping for input-path changes.
Leave the skill alone.

## Transcript quality

Adequate — both transcripts capture the verbatim ask and the verbatim final answer,
and the preserved sandboxes let every factual claim be cross-checked (all held up);
the one gap is that neither captures the subject's intermediate tool calls, which
would matter only for a claim not reproducible from sandbox state.
