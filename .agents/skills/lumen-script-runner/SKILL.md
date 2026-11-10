---
name: lumen-script-runner
description: Run and validate Lumen repository scripts professionally: dependency/protocol setup, generated fixtures, headless benchmarks, assigned-seat smoke tests, input and session tooling. Select correct options, isolate side effects, capture reproducible evidence, and hand results to lumen-test-analyst.
---

# Lumen script execution specialist

Use this skill for execution of `scripts/` tools, not just their names. Load `lumen-test-agent` for the shared testing/environment/installation contract and `lumen-build-run` before builds/runs. Read current AGENTS.md and TEST.md. This skill can guide a dedicated DSH subagent; it neither creates a persistent agent nor grants a display seat automatically.

## Select and prepare

1. Read `references/script-catalog.md` (every current script, purpose, options, side effects and evidence) and `../lumen-test-agent/references/test-matrix.md` for test selection. Inspect the **selected script's current parser, prerequisites, cleanup and verdict logic**, not only header help. Discover new scripts with glob and update your plan rather than running unknown files wholesale.
2. Record task, exact checkout/HEAD/dirty state, build/cache/toolchain identity, requested groups, expected assertions, backend/device, duration and resource reservation. On Windows use the discovered Docker mount for this worktree. Helpers and syntax validation need no graphical seat. Full console smokes require assignment by the coordinating/user session; unavailable hardware is NOT RUN/UNSUPPORTED.
3. Read paths before deletion/overwrite. Several scripts truncate logs or remove matching files in their log directory. Allocate a **new unique log directory for each attempt**, use supported `--logs` or `LUMEN_LOG_DIR`, and never point it at sources, old evidence or a shared directory. Resolve scratch prefix/output paths before generator or install commands. Fixed default sockets, benchmark result filenames and systemd unit names also conflict: serialize them.
4. Install task-needed dependencies using the existing installer and `lumen-test-agent/references/tool-setup.md`; preview the plan, honor prior authorization and runtime policy. Do not run driver/boot changes as routine package setup. Never bulk source shell scripts: `portal-session.sh` and `nouveau-atomic.sh` are libraries, `lumen-session.in` is a configured template, and other scripts execute at top level.

## Execute and preserve evidence

Use `../lumen-test-agent/scripts/capture_run.py` to run bounded **non-seat** commands with a result manifest, or produce the same evidence contract manually for assigned seat/session runs. See `../lumen-test-agent/references/evidence-contract.md`. The helper accepts an argv after `--`, separate stdout/stderr, a timeout and explicit metadata; it never interprets smoke output as PASS. It is not a privileged cleanup supervisor. Do not wrap seat-owning scripts with an external timeout that could prevent their VT/service restoration: use their own bounded operation and coordinator-managed graceful teardown.

Before execute: validate argv against source/help; make required configure/build steps sequential; confirm no competing compositor/build/measurement; confirm private socket and owned output paths; check exit semantics. `--only` generally selects groups, not arbitrary row IDs. Do not claim an omitted group ran. Keep `--skip-gtk`, missing packages, unavailable validation layers and soft skips visible in coverage.

During execute: keep logs complete and chat output bounded; retain every background job ID; handle SIGTERM/normal shutdown through script cleanup; collect owned children and record restoration. Record raw command exit independently from the script's printed verdict. Contradictory verdict/exit, timeout, interrupted cleanup or missing expected files is unresolved/failed execution. Do not infer success from the last line. Warnings need classification and evidence, not automatic acceptance. For comparative performance runs serialize and follow the test skill's interleaved A/B rules.

After execute: retain raw stdout/stderr, exact script hash, timestamps/clock, env overrides (allowlist; no secret dump), artifact index/hashes, per-row output, CTest JUnit when relevant, trace/profile data, cleanup result, capability gaps and rerun provenance. For source generators write to scratch and compare with tracked output; report drift without replacing the expected output. Capture read-only before/after service/output state when the script changes it. Partial runs retain partial evidence.

## Handoff

Hand the completed evidence directory and a brief execution note to a **different agent** loaded with `lumen-test-analyst`. Include the original question, acceptance criteria, baseline/candidate mapping, reservations, known missing evidence and any execution anomalies. Execution observations are separate from causal claims. Do not label an environment explanation or a product regression confirmed without analysis. When no independent analyst is available, disclose that and report execution facts only.

Stop on a measured regression and return A/B evidence to the user before workaround/acceptance, per AGENTS.md. Do not commit, change thresholds, update goldens or suppressions, or alter product code unless separately authorized.
