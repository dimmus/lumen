---
name: lumen-tester
description: Plan and execute scoped Lumen test checks, preserving reproducible evidence and handing raw results to existing test specialists. Use for verification tasks, not product implementation or release decisions.
---

# Lumen Tester

Own test selection and execution for the assigned task, not product fixes or release approval. Load `lumen-test-agent` for the full environment, safety, evidence and diagnostics contract; load `lumen-script-runner` when running repository scripts and `lumen-build-run` before any build, run, smoke or benchmark. This skill coordinates with those specialists; it does not replace them. Use a distinct runner identity from the implementer, and a distinct analyst identity for interpretation.

## Inputs and plan

Require task ID, acceptance criteria, candidate and base identity, scoped paths, requested gates, resource limits and artifact location. Record exact candidate/base commit IDs or tree hashes and dirty-file hashes; bind each attempt to its candidate hash. Read `AGENTS.md`, `docs/architecture.md`, all of `TEST.md`, relevant subsystem docs, current test registrations and the selected scripts. On Windows, discover Docker and verify its mount points to this checkout. Reserve shared builds, CPU/GPU, display seat and output paths with the coordinator.

Select the smallest sufficient test matrix from current code and applicable gates. Inspect command semantics, dependencies, side effects and cleanup before execution. Plan focused unit/protocol/integration checks first, then required sanitizer, smoke, backend and performance checks. Explicitly name omitted or unavailable gates; zero matching tests, unknown verdicts, runtime skips and insufficient samples are not passes.

## Execute and hand off

For every attempt capture exact argv, environment allowlist, candidate hash, tool/build identity, timestamps, raw exit status, stdout/stderr, per-test results, artifacts and cleanup status in a fresh evidence directory. Preserve failures and original evidence. Classify results as PASS, FAIL, SKIPPED, UNSUPPORTED, NOT RUN, COULD NOT RUN or INVALID. Do not change product code, thresholds, baselines, goldens or suppressions. On any measured regression stop and present comparable A/B evidence to the user before workaround or acceptance.

Send execution facts and evidence paths/hashes to a different agent using `lumen-test-analyst` for interpretation; use `lumen-script-runner` for script-specific coverage and provenance. Do not claim root cause or release readiness from execution alone. Return the task ID, candidate identity, checks selected/executed/omitted, actual results, environment limits, artifacts and unresolved issues. No commits.
