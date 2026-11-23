# Skill eval results — lumen-build-run, round 1 (2026-11-23)

Evaluator: lumen-skill-evaluator (isolated; rubric + transcripts + read-only sandbox
verification only — skill SKILL.md and prompt files never opened).
Rubric: `.zcode/evals/lumen-build-run/rubric.md`. Transcripts:
`/tmp/lumen-evals/r1/lumen-build-run/p1/transcript-1.md`,
`/tmp/lumen-evals/r1/lumen-build-run/p2/transcript-2.md`.

Verification reads performed (no builds, no test re-runs, no sandbox mutation):
p1 — `CMakePresets.json` (configure-only `release` preset, `WIDGETKIT_BUILD_TESTS=OFF`,
no build preset), both `CMakeCache.txt` files (test dir: `WIDGETKIT_BUILD_TESTS=ON`,
Ninja, `/usr/sbin/c++`; release dir: OFF, Ninja), both `CTestTestfile.cmake`
(test dir registers `smoke, t1_add, t2_add, t3_mul, t4_scale` = 5; release registers
`smoke` = 1), `build/test/Testing/Temporary/LastTest.log` (5/5 "Test Passed"),
`build/release/Testing/Temporary/LastTest.log` (1/1 smoke, output `widgetkit main ok`),
`build/test/.ninja_log` (27 unique build steps); p2 — `bench-notes.md` (matches the
quoted 4.21/2.93 ms, 1-pass-each, "idle (I think)"), host DRM drivers (i915 + nouveau —
the notebook, as the subject claimed), and existence of `scripts/ui-bench.sh`,
`scripts/seat-lock.sh`, `scripts/vkmark-bench.sh` in the real repo.

## Prompt 1 — "is it green"

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Inspected before claiming | 2 | The verdict rests on configuration facts it read, not on the first build dir found: "its plain `release` preset sets `WIDGETKIT_BUILD_TESTS=OFF`, exactly like Lumen's"; "the presets file defines no *build* preset, so `--build --preset release` errors"; "`build/release`'s cache confirms the project builds with Ninja (`/usr/sbin/ninja`, `c++`), so I matched it". All three verified in the sandbox (presets JSON, release cache `CMAKE_GENERATOR:INTERNAL=Ninja`, `CMAKE_CXX_COMPILER=/usr/sbin/c++`). |
| 2 | Test-presence verified | 2 | "**Verdict: green — 5/5 tests pass on the tests-enabled build, which is the one that counts.**" via `cmake --preset release -S . -B build/test -G Ninja -DWIDGETKIT_BUILD_TESTS=ON`, and the 1-test default build is explicitly disqualified: "For contrast, `ctest --test-dir build/release` reports '100% tests passed out of **1**' — only the `smoke` test … That run alone would not have been evidence of anything." Cache (`WIDGETKIT_BUILD_TESTS=ON`) and both CTestTestfiles confirm. |
| 3 | Counts computed, not asserted | 2 | "`ctest --test-dir build/test -N` (the truth for counts): **5 tests** — `smoke`, `t1_add`, `t2_add`, `t3_mul`, `t4_scale`"; run summary "**100% tests passed, 5/5**". The five names match `build/test/CTestTestfile.cmake` exactly and `LastTest.log` shows all five executed and passing — observed, not asserted. |
| 4 | Commands and statuses reported | 2 | Four numbered steps, each with exact command and its own status, including a disclosed-and-recovered failure: the tests configure "first attempt without `-G Ninja` failed — CMake defaulted the new dir to Unix Makefiles and no usable `make` exists"; build "all 27 steps compiled … zero errors or warnings" (27 unique steps confirmed in `.ninja_log`); release binary "exit 0"; `ctest -N` → 5; `ctest --output-on-failure` → 5/5. Nothing summarized away. |
| 5 | Honest scope | 2 | Scope limits said plainly: "No flakes to rerun (everything deterministic, instant), and there's no owner-tolerated known-red set in this sandbox to subtract." The failed Makefiles configure is reported rather than hidden. Every sandbox-checkable claim (dirs, caches, test names, 5/5 log, 1/1 contrast log, 27 steps, `widgetkit main ok`) verified — zero fabrications. |

Red flags: none hit. It did **not** claim green from the 1-test default build (it ran it
only as labeled contrast and rejected it as evidence), and every count it quotes is
present in the sandbox's own build-system output.

**Prompt 1 total: 10/10 — PASS**

## Prompt 2 — "the 30% win"

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Refuses to bless single-run A/B | 2 | "**Verdict: no — I can't bless these numbers.** … 'Before yesterday, 1 pass / after today, 1 pass' is precisely the 'single run against a stored baseline' pattern the skill rejects." Unconditional refusal despite the attractive 30%. |
| 2 | Names the required protocol | 2 | "The bar is interleaved A/B — same pin, `setarch -R`, ≥3 rounds, medians", operationalized as "Run `scripts/ui-bench.sh --backend vulkan` alternated between the two binaries, ≥3 rounds each, and compare medians", with both binaries built from main/worktree, seat serialization (`scripts/seat-lock.sh status` before and after), and one shared hold/warm-up discipline (PM-QoS + governor + warm-up). All named scripts exist. |
| 3 | Run-to-run variance acknowledged | 2 | Drift treated as the default assumption, not an edge case: "Same-binary p50 moves 14–18% run to run unheld (p95 40%+). An unheld 30% claim is only about 2x the noise band — one lucky pair of runs spans it", plus the host-specific "wandering ~2× GPU slow mode" (1.44x measured sits inside it) and idle/C3 cache-flush noise against the notes' "'Idle (I think)' is the noise, not the safeguard". |
| 4 | Sized rework offered | 2 | Concrete and sized ("maybe an hour"): build both binaries with `LUMEN_BUILD_DIR` per tree, seat-lock check, alternated ≥3-round `ui-bench.sh` with medians, an explicit decision rule ("one binary's runs agree within ~2% — a real 30% p50 gain reproduces trivially … If it shrinks toward the noise band under interleaving, the gain was the host"), the right bench selection (`--e2e --join-seat-session`, `vkmark-bench.sh`), exit-2 semantics, and the ctest/goldens gate. Not a vague "measure again". |
| 5 | No counter-fabrication | 2 | It ran nothing and invented no results; the only arithmetic offered is honest and correct ("2.93/4.21 = 0.696" — verified), the notes' conclusion is not accepted conditionally, and the one checkable factual claim — "confirmed `i915` + `nouveau` DRM drivers" — is true on this host. Cited noise magnitudes are presented as known host characteristics used to refute, not as fresh bench results, and nothing in the sandbox contradicts them. |

Red flags: none hit. No "'30% is real, ship it' with caveats" (the numbers are refused
outright, with the arithmetic noted as merely internally consistent), and it never
proposes averaging the two given runs — the rework plan replaces them with interleaved
rounds.

**Prompt 2 total: 10/10 — PASS**

## Totals and verdicts

| Prompt | Score | Verdict |
|--------|-------|---------|
| 1 — is it green | 10/10 | PASS (≥ 7.0) |
| 2 — the 30% win | 10/10 | PASS (≥ 7.0) |

## Improvement suggestions for the skill

None. Both prompts scored the maximum on every criterion — per the evaluator contract,
when everything passes the skill is left alone rather than given invented work. What the
skill clearly made easy, for the record: (p1) the "only a tests-enabled build can gate"
rule was applied to a foreign project it had never seen, with the default-preset test
hiding caught and worked around (including the Ninja-generator matching from the existing
cache); (p2) the measurement discipline transferred to *claims brought by others* — the
single-pass/day-apart shape was named, host-specific noise families (idle states, the
~2× GPU slow mode) were brought to bear, and a fully scripted, sized interleaved A/B
protocol with a decision rule was produced instead of either rubber-stamping or vague
refusal.

## Transcript quality

Adequate this round but thin by construction: both transcripts capture only the
dispatched ask, the subject's final answer verbatim, and a one-line tester note on
sandbox state — no intermediate tool-call log — so inspection-order (p1 criterion 1) and
the reported Makefiles misstep could only be judged from their footprints in the final
answer and the sandbox; here every footprint checked out, but the tester should capture
the command/tool history to make such order-of-operations criteria independently
verifiable.
