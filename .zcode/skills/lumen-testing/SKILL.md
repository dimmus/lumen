---
name: lumen-testing
description: Lumen's verification discipline — test matrices (tests-enabled ctest, seat smokes, sanitizers, goldens, benches), evidence contracts and status vocabulary for reviewing and testing every code change, with tests required for new functionality. Use when planning or executing Lumen verification, judging evidence, or deciding what a change must pass; supersedes the DSH testing skills' Docker/headless-era guidance.
---

# Lumen testing discipline

Every code change is reviewed and tested; every new piece of functionality gets tests
that exercise it. Compositor/display/input behavior is verified **on the real seat**
(`--join-seat-session`); headless `--virtual-outputs` runs are not evidence for that
surface. Unit ctest and the in-process `ui-bench` engine bench are valid supplementary
evidence. Host facts, verified commands and known noise live in
`.zcode/skills/lumen-build-run/SKILL.md` — read it first.

Deep catalogs still apply from the DSH archive (kept for reference, environment text
superseded by this skill): `.agents/skills/lumen-test-agent/references/test-matrix.md`,
`diagnostics.md`, `tool-setup.md`, `evidence-contract.md`, and
`.agents/skills/lumen-script-runner/references/script-catalog.md` (all 20 repo scripts).

## What a change must pass (proportionate to what it touches)

| Change surface | Required evidence |
| --- | --- |
| Any code | Tests-enabled ctest suite green (`build/test`, 133 non-bench tests as of 2026-11, 138 registered incl. bench — the `release` preset has none); on a tree with a tolerated known-red set, green = no new failures beyond it (see `lumen-build-run`) |
| New functionality | New tests exercising it, in the same change |
| Compositor, display, input, shell, protocols | + seat smokes: `drag-smoke.sh --join-seat-session` and `app-smoke.sh --join-seat-session` (quiet host, one DRM master) |
| Rendering/perf-relevant | + interleaved A/B `ui-bench` (≥3 rounds, both binaries pinned) vs the base; FAIL p50 +25 %, WARN p50 +10 %/p95 +25 % |
| Client/UI engine | + goldens (`LUMEN_UI_GOLDEN_UPDATE=1` regenerates only with an authorized pixel change); ASan+LSan proof for new client code (`--preset asan`) |
| Memory behavior | + heaptrack (launch under, don't attach); steady-state frame path must allocate nothing |
| Docs-only | review only; no suite run needed |

Sanitizers: presets `asan` (address+undefined), `tsan`. No ubsan/msan/coverage preset
exists. Suppressions only by third-party shared-object name — never broad symbols, never
Lumen frames, never `detect_leaks=0` globally.

## Evidence contract

Every executed check records: exact argv, working tree + build dir identity (commit or
dirty hash), environment overrides that matter (`LUMEN_*`), exit code, the counts the
output actually printed, artifact paths (logs/JSON) with sha256 when they gate anything,
and who ran it (agent identity — independence is checked at the gate). Store artifacts
under the tree's `build/evidence/<date>-<name>/` (gitignored; quote paths in reports).

## Status vocabulary (exact)

`PASS / FAIL / NOT RUN / UNSUPPORTED / COULD NOT RUN / INVALID (contaminated measurement
— foreign input, load spike, second compositor on the seat)`. Execution vs assessment are
separate: "completed, exit 0" is not PASS until the printed counts were checked against
what the check gates. A suite that ran 1 test where 138 exist is INVALID as a suite run.

Smoke exit semantics: 0 PASS (may warn), 1 FAIL, 2 COULD NOT RUN, 3 INVALID from foreign
input. drag latency gates: median ≤ 1.5 refresh periods + 4 ms; p95 warn at 2 periods
+ 4 ms. Bench artifacts land in `<machine>-<driver>/<date>-<sha>.json` which can collide
on same date/revision — copy/hash after each round; `--update-baseline` needs reviewed
authorization, never during verification.

## Discipline rules

- **Independence**: the agent that implemented does not review; the agent that ran the
  checks does not analyze them; the gatekeeper is read-only. Repeated skill invocations by
  one agent are not independent stages.
- **Serialize the seat**: one compositor, one smoke, one bench at a time on this 2-core
  host; check `grep -l lumen-composito /proc/[0-9]*/comm` (pgrep -x cannot see
  lumen-compositor — comm truncates at 15 chars), `pgrep -f "^bash .*-smoke\.sh"`,
  `/proc/loadavg` before/after. Never measure while another session builds.
- **No weakening to pass**: never edit a test, golden, threshold or baseline to make a
  change pass; that is a critical finding against the change, not a repair.
- **Environmental failure**: one rerun discounts known noise (see lumen-build-run);
  a second failure is reportable as the change's until a clean baseline reproduces it.
- **Regression stop**: any measured regression stops the task; the A/B evidence goes to
  the user before keeping, working around or accepting (AGENTS.md §1.2). Compilation is
  not runtime verification; a screenshot is not latency evidence; a software GPU is not
  hardware Vulkan.
- **New-protocol changes** additionally follow `protocols/README.md`: manifest entry →
  fetch → codegen (never hand-edit generated files) → `LUMEN_PROTOCOL_<STEM>` guards →
  conformance tests that post every protocol error the interface defines.
