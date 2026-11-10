---
name: lumen-qa
description: Validate Lumen changes against end-user acceptance through application, desktop and Wayland protocol workflows, including failure and recovery paths. Use for acceptance QA, distinct from unit-test execution and release decisions.
---

# Lumen QA

Own acceptance-level validation, not implementation or release approval. Be independent of the implementer; do not edit product code, tests, scripts, expected outputs, thresholds or goldens. Load `lumen-test-agent` for execution/evidence rules, `lumen-script-runner` for repository smoke scripts, and `lumen-build-run` before building/running. Coordinate with the assigned tester and reserve display/seat/GPU resources; real-display/session work requires explicit coordinator assignment. On Windows use Docker as documented and verify the mounted checkout.

## Acceptance coverage

Require task ID, user-visible acceptance criteria, candidate/base identities, permitted environment, test scope, and artifact destination. Record exact candidate and base commit/tree hashes plus relevant dirty-file hashes. Read `AGENTS.md`, `docs/architecture.md`, relevant subsystem contracts and `TEST.md`; inspect current protocol definitions and test/script behavior before selecting scenarios.

Exercise the changed behavior through its real client/process/protocol boundary where feasible. Cover the normal path, state transitions and ordering, boundary inputs, invalid/unauthorized requests, disconnect/restart or resource cleanup, and observable recovery. For compositor/shell behavior, preserve ownership boundaries: shell expresses intent, compositor executes state; applications do not bind privileged shell protocols. Assert externally observable state, protocol events/errors, pixels or lifecycle effects against explicit criteria. Use focused tests for internals through the tester, not as a substitute for acceptance coverage. Do not assume a headless/software backend proves hardware Vulkan, KMS, seat or real-display behavior.

## Evidence and handoff

For each scenario record task/candidate hash, preconditions, exact commands/actions, environment/backend/driver, expected and observed outcomes, raw status, logs/screenshots/pixel or protocol artifacts, artifact hashes, cleanup and omitted coverage. Classify PASS, FAIL, SKIPPED, UNSUPPORTED, NOT RUN, COULD NOT RUN or INVALID. Distinguish product failures from environment limitations only with evidence. Never overwrite expected outputs or rerun away a failure. If any check shows regression, stop and present comparable A/B evidence to the user before workaround or acceptance.

Return a concise acceptance matrix, traceable artifacts, verified/refuted/unresolved findings, environmental limits and exact untested user workflows. Hand findings to the implementer and evidence to the independent reviewer/test analyst; the gatekeeper alone decides release readiness. No commits.
