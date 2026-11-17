---
name: lumen-adr-skill
description: "2026-11-17 notebook: harness gained the lumen-adr knowledge skill — when a change needs docs/adr/NNN-*.md (cross-module/layer/protocol, weighed alternatives, supersede/amend; NOT single-module or TODO work) and how to write one (next number re-checked via ls docs/adr/, 001–013 → next 014 at main 1355959; Status/Date/Scope/Deciders/Related header; Context→Decision→Consequences with 009/012/013 variants); per-part implementation state rides a > **Status:** blockquote in 004–010 but a plain **Status:** line in 011–013."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-17 on the iritur notebook (i915+nouveau per
`readlink -f /sys/class/drm/card*/device/driver`): harness task `lumen-adr-skill`
(branch `zcode/task/lumen-adr-skill`, on top of TASK A `agent-screenshoter`'s result
ac0330d, main base 1355959) added the ADR-authoring knowledge skill — docs-only, zero C++.

- New: `.zcode/skills/lumen-adr/SKILL.md` (WHEN + HOW, per the task card); wired with one
  sentence in `lumen-architect`'s Read-first block, one Map row entry in `lumen-studio`,
  one name in the AGENTS.md harness bullet — the latter two on top of TASK A's edits to
  the same lines (the only reason for the dependsOn).
- ADR corpus facts re-verified in the worktree this session: `ls docs/adr/` = 001–013 →
  next free 014; filenames `NNN-kebab-case.md`; full header (Date/Scope/Deciders/Related)
  in 004–010 + 013, lean Status-only in 011–012, plus 001–003's legacy top
  `> **Status:** Current (v0.3)` pointer; bodies 001 Context→Decision→Consequences, 009
  `## Decision (Part N)`, 012 `### T1–T8` (Decision :31, Consequences :153), 013 numbered
  §1 Decision … §9 As built (:547).
- Correction to the card's plan note (observed over its shorthand): per-part
  implementation state rides a leading `> **Status:**` blockquote in 004–010 (e.g. 009
  Parts 1–5, 006 Parts A–B) but a plain `**Status:**` line in 011–013 (012 "Accepted —
  implemented (T1–T8)." is NOT a blockquote). The skill documents both carriers as
  verified. Supersession pattern: 005's Status links ADR-010 (:2, :416).
- Skill states ADR authoring is the lumen-architect agent's job (read-only agent — the
  file is written by the task's implementer/doc-writer); user acceptance precedes any new
  ADR or status/contract change (`lumen-architect.md:44-46`, `lumen-doc-writer.md:30-31`);
  division: ADR = decision+reasoning, `lumen-decisions` = measured outcomes, TODO = open
  work; next number always re-checked at authoring time (race: concurrent tasks can land
  an ADR).
- No ctest run: diff ⊆ `.zcode/**` + AGENTS.md ⇒ no compiled input changed ⇒ the
  tests-enabled baseline cannot be affected (no rebuild made or claimed). No
  `docs/adr/014-*.md` authored — the skill defines the process only.

**Why:** ADR conventions were implicit in the corpus — each authoring session re-derived
numbering, header fields and status vocabulary, and the user-acceptance gate lived only
in two agent files the author might never load.

**How to apply:** load `lumen-adr` before authoring or flipping any ADR; re-run
`ls docs/adr/` for the next number rather than quoting 014 from here (it was next free at
main 1355959); route authoring through `lumen-architect`, get user acceptance, and commit
the ADR with the code (AGENTS.md §9). Extend the skill (not a new memory) when the corpus
grows a new header/body variant.
