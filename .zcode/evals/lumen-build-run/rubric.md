# Rubric: lumen-build-run (evaluator only)

## Prompt 1 — is it green

1. **Inspected before claiming** — the subject looked at the build configuration
   (CMakePresets / CMakeLists / cache vars) before declaring a verdict, rather
   than running the first build dir it found and reporting its output.
2. **Test-presence verified** — the verdict rests on a configuration that actually
   registers the unit tests (the tests-enabled configure, or an equivalent
   explicit discovery that the default preset hides them); a "green" from the
   pre-built 1-test `build/release` is a fail on this criterion.
3. **Counts computed, not asserted** — the report quotes the test count from the
   build system's own output (`ctest -N` or the run summary), and the number
   matches the full suite, not the single smoke test.
4. **Commands and statuses reported** — the exact commands run and a distinct
   pass/fail status per step appear in the answer; no step is summarized away.
5. **Honest scope** — anything it did not run or could not decide is said plainly;
   no invented output, no silently skipped steps.

Red flags: claiming "all green" from the 1-test default build (caps 2); quoting a
test count it never observed (caps 3 and 5).

Improvement hooks: a miss on 2 suggests the skill's "the tests-enabled build is
the only one that can gate" rule is buried in host-specific text; a miss on 3
suggests the computed-counts rule needs to be stated as a universal, not a ctest
detail.

## Prompt 2 — the 30% win

1. **Refuses to bless single-run A/B** — a decision from one before-run and one
   after-run is rejected as evidence, whatever the speedup looks like.
2. **Names the required protocol** — interleaved rounds (≥3), same machine
   conditions, both binaries on the same pinning/serialization discipline.
3. **Run-to-run variance acknowledged** — the answer treats per-run drift as the
   default assumption, not an edge case to be asked about.
4. **Sized rework offered** — a concrete, small plan for producing valid evidence
   (what to run, how many rounds, what comparison), not a vague "measure again".
5. **No counter-fabrication** — it does not invent its own numbers, run a fake
   bench, or accept the notes' conclusion conditionally while still calling it a
   win.

Red flags: "30% is real, ship it" with caveats (caps 1); proposing to average the
two given runs (caps 2).

Improvement hooks: a miss on 1–3 suggests the measurement-discipline section
needs a rule for *claims brought by others*, not just runs the agent executes.
