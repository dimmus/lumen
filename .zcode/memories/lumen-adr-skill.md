---
name: lumen-adr-skill
description: "2026-11-17 notebook: harness gained the lumen-adr knowledge skill (branch zcode/task/lumen-adr-skill 562680f→18653e9 on TASK A ac0330d, gate READY, unmerged) — WHEN a change needs docs/adr/NNN-*.md (cross-module/layer/protocol, weighed alternatives, supersede/amend; NOT single-module or TODO work) and HOW (next number re-checked via ls docs/adr/ = 001–013 → 014 at main 1355959; NNN-kebab-case.md; Status/Date/Scope/Deciders header; Context→Decision→Consequences with 009/012/013 variants); per-part status state rides a > **Status:** blockquote in 004–010 but a plain **Status:** line in 011–013; review noise 2/2/0/0; docs-only ⇒ no ctest run."
metadata:
  node_type: memory
  type: project
summary: "When an ADR is required (cross-module/protocol, weighed alternatives, supersede) and the corpus conventions; ADR=decision, decisions=measured, TODO=open"
modules: harness, docs/adr
topics: adr, authoring

---

Verified 2026-11-17 on the iritur notebook (nouveau card0 + i915 card1 per
`readlink -f /sys/class/drm/card*/device/driver`): harness task `lumen-adr-skill`
(branch `zcode/task/lumen-adr-skill`, cut on TASK A `agent-screenshoter`'s result
ac0330d, main base 1355959, unmerged — nothing pushed) added the ADR-authoring
knowledge skill — docs-only, zero C++, no `docs/adr/014-*.md` authored (the skill
defines the process only).

- Commits 562680f (skill + wiring), 35a35c7 (branch's pre-review copy of this record),
  416fd6c + 18653e9 (review fixes, below). Diff = 9 paths, all `.zcode/**` + AGENTS.md
  ⇒ no compiled input changed ⇒ the tests-enabled ctest baseline cannot be affected
  (no ctest run made or claimed); no seat smokes/bench (off — nothing display/input).
- New `.zcode/skills/lumen-adr/SKILL.md` — WHEN: cross-module/layer or protocol-surface
  decisions, settled choices where alternatives were weighed, supersede/amend; negative
  space: single-module implementation details and TODO-row work get no ADR; division:
  ADR = decision + reasoning, `lumen-decisions` = measured do-not-re-litigate outcomes,
  `.zcode/TODO.md` = open work; reconciled with AGENTS.md §2 required reading and §9
  same-change rule.
- HOW: next free number re-verified via `ls docs/adr/` at authoring time — never a
  cached number (race: a concurrent task can land an ADR); `NNN-kebab-case.md`; header
  Status/Date/Scope/Deciders: project owner + optional Related; body Context →
  Decision → Consequences (009 numbered per-Part, 012 `### T1`–`T8` at :33/:137, 013
  numbered §1 Decision … `## 9. As built` at :547); title line `# ADR NNN — Title`
  (002–003 and 011–013; 001 and 004–010 use `# ADR-NNN: Title`).
- ADR corpus facts, re-verified this session in the worktree (`ls docs/adr/`,
  `grep -Hm1 "^# " docs/adr/*.md`, Deciders grep): 001–013 exist → next free 014;
  per-part implementation state rides a leading `> **Status:**` blockquote in 004–010
  (009 Parts 1–5) but a plain `**Status:**` line in 011–013 (012 "Accepted —
  implemented (T1–T8)." is NOT a blockquote); 001–003 blockquote is the legacy
  `> **Status:** Current (v0.3)` pointer; full header (Date/Scope/Deciders) in 004–010
  + 013, lean Status-only in 001–003/011/012; supersession links in 005:2/:416 →
  ADR-010.
- Correction to the card's plan note (deliberate deviation, disclosed in the packet):
  the card described 012's status as a blockquote — direct verification shows the
  blockquote carrier is 001–010 and 011–013 are plain; the skill documents the corpus
  as verified, covering both carriers.
- Authoring rules the skill states: ADR authoring is the lumen-architect agent's job
  (the agent is read-only — the task's implementer/doc-writer writes the file); user
  acceptance precedes any new ADR or status/contract change (`lumen-architect.md:44-46`,
  `lumen-doc-writer.md:30-31` — both paragraphs re-read this session). Wiring: one
  sentence in lumen-architect's Read-first block (:19), one lumen-studio knowledge Map
  row (:17), one name in the AGENTS.md harness bullet (:14) — the latter two stacked on
  TASK A's edits to the same lines (the only reason for the dependsOn).

**Close-out 2026-11-17 (gate READY, written on main from the packet + gate/review
evidence):**

- **Review noise (cross-run false-positive tracking): raised 2 / confirmed 2 / refuted
  0 / preexisting 0.** Both findings low, both fixed. (1) The skill cited the
  user-acceptance gate as `lumen-architect.md:40-42` — this task's own +4-line insertion
  into that file had shifted the Safety sign-off rule to :44-46, so the skill landed
  citing the wrong paragraph (lines 40–42 = Output-contract tail); fixed 416fd6c
  (SKILL.md:13 + the memory record's cite), `sed -n '44,46p'` re-verified at tip.
  (2) The title-line parenthetical said the em-dash form was "the 011–013 form" —
  `grep -Hm1 "^# " docs/adr/*.md` shows 002/003 use it too; fixed 18653e9 to the exact
  split quoted above. No bypass: `git diff --name-only 35a35c7..HEAD` = exactly the two
  named files; lumen-architect.md and docs/adr were never edited to make the old claims
  true — the claims were corrected to match the unchanged docs.
- preexistingFindings = [] ⇒ no TODO rows opened by this close-out. Branch commit
  35a35c7 carries this record's pre-review copy — at merge this file is add/add; keep
  main's (extended) copy, same as [[lumen-agent-screenshoter]].

**Why:** ADR conventions were implicit in the corpus — every authoring session
re-derived numbering, header fields and status vocabulary, and the user-acceptance gate
lived only in two agent files the author might never load; equally, a line-number cite
written before the same change edits the cited file is stale on arrival.

**How to apply:** load `lumen-adr` before authoring or flipping any ADR; re-run
`ls docs/adr/` for the next number rather than quoting 014 from here (it was next free
at main 1355959); route authoring through `lumen-architect`, get user acceptance first,
and commit the ADR in the same change as the code (AGENTS.md §9). When a change inserts
lines into a file it also cites, write the cite against the post-edit tree. Extend the
skill (not a new memory) when the corpus grows a new header/body variant. Related:
[[lumen-zcode-agent-harness]], [[lumen-agent-screenshoter]], [[lumen-tiling-adr012]].
