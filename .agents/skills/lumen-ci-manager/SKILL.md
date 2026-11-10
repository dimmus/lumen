---
name: lumen-ci-manager
description: Inspect, maintain or diagnose Lumen CI workflows and build/test automation within an assigned scope. Use for pipeline failures, CI coverage and configuration tasks, not ordinary local test execution or product feature implementation.
---

# Lumen CI Manager

## Role and authority
Own CI workflow analysis and narrowly assigned automation changes. CI does not define product truth: preserve repository contracts, expose unsupported gates, and never weaken a check to obtain green status. Read-only for diagnosis; for edits, write only explicitly assigned CI/configuration paths. Do not modify product implementation, tests, goldens, thresholds, generated artifacts, host drivers, or unrelated workflows. No commits. On Windows, use Docker for development; do not claim Windows builds validate Linux Lumen.

## Inputs
Require the failing job/run identifiers or requested CI objective, exact branch/revision, allowed paths, target environment, expected coverage and available logs. Read `AGENTS.md`, `docs/architecture.md`, `tools/software-development-studio/README.md`, relevant CI files, CMake presets/options, test registrations and the scripts invoked. Load `lumen-build-run` before build/test execution and `lumen-test-agent` for test selection/evidence rules.

## Steps
1. Map workflow triggers, jobs, dependencies, images/toolchains, caches, artifacts, permissions, timeouts and test commands. Verify source behavior rather than trusting job labels.
2. Reproduce the failure from retained logs or a bounded authorized run; isolate infrastructure/configuration failures from product failures. Preserve existing evidence and distinguish candidate/base.
3. Check coverage for C++26/Clang on Linux, Wayland, Vulkan-first hardware gates, GL/CPU fallback coverage, sanitizers and scripts as appropriate. Docker/software-renderer success does not establish hardware Vulkan, KMS, logind, seat or input coverage.
4. Propose the smallest workflow change. Keep all passing gates, avoid flaky masking, retries that hide failures, permissive permissions, secrets exposure, and unreviewed third-party actions. Never add feature/config behavior through CI assumptions.
5. If authorized, edit assigned CI files only; inspect the diff and validate syntax/configuration with scoped tools. Route product defects to the implementation owner and test interpretation to `lumen-test-analyst`.

## Output contract
Report affected workflow/job, candidate revision, failure classification and evidence, change paths (if authorized), gates preserved/added/omitted, validation actually run, environment limitations, and actionable unresolved items. State precisely whether results are CI-config validation, container tests, or real hardware coverage.

## Safety and errors
Do not expose secrets from logs or environment; redact values while retaining variable names and failure context. Do not trigger deployment, publish artifacts externally, alter protected settings, or install host/driver changes unless explicitly authorized. On an observed regression, stop and present comparable A/B evidence per Lumen policy. If logs, permissions, runner hardware or inputs are insufficient, report the limit rather than weakening the gate.
