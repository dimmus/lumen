---
name: lumen-adr
description: Decide whether a Lumen change needs an architecture decision record in docs/adr/ and author it — when an ADR is required (cross-module/layer or protocol-surface decisions, settled choices where alternatives were weighed, superseding or amending a settled decision) and when it is not (single-module implementation details, TODO-row work), plus the corpus conventions — next number re-checked against ls docs/adr/ at authoring time, NNN-kebab-case.md filename, the Status/Date/Scope/Deciders/Related header block, Context → Decision → Consequences bodies with their per-part variants, the status vocabulary's per-part implementation notes, the ADR/lumen-decisions/TODO division, and the user-acceptance gate. Load when authoring an ADR or amending/flipping the status of one.
---

# Authoring Lumen ADRs (lumen-adr)

ADRs are authored by the **lumen-architect** agent — the design and its decision record are
that agent's output (`.zcode/agents/lumen-architect.md`). The architect is read-only, so in
a task the file itself is written by the implementer/doc-writer and committed **in the same
change as the code it decides** (AGENTS.md §9). Nothing lands without the user's acceptance
of the architecture change first: a new ADR, and any status or contract change to an
existing one, is a proposal requiring sign-off (`lumen-architect.md:44-46`,
`lumen-doc-writer.md:30-31`).

## WHEN a decision needs an ADR

Write a new `docs/adr/NNN-*.md` when the change:

- **crosses module or layer boundaries, or changes a protocol surface** (wire format,
  globals, versions) — the class of change AGENTS.md §2 routes through
  `docs/architecture.md` and the subsystem docs ("any architectural change");
- **settles a design choice where alternatives were weighed** — the record must show the
  alternatives and why they lost (ADR-001 native sd-bus vs AccessKit; ADR-012
  compositor-side vs plugin vs shell-side tiling);
- **supersedes or amends a settled decision** — the new ADR says so explicitly, and the
  superseded ADR's Status points forward (ADR-005: "Phase 3 is superseded by ADR-010",
  `005-desktop-roadmap.md:2` and `:416`).

Negative space — these do **not** get an ADR:

- single-module implementation details (a refactor inside one module, internal data
  structures) — the commit message and the module's docs carry them;
- TODO-row work: open or unsettled items tracked in `.zcode/TODO.md` — an ADR records a
  decision **made**, not work wanted;
- measured outcomes with no open architectural choice — `lumen-decisions` /
  `.zcode/memories/` material, not a new ADR.

Division of records (all three exist; keep them in their lanes):

| Record | Holds |
| --- | --- |
| `docs/adr/NNN-*.md` | the decision and its reasoning: context, alternatives, consequences |
| `.zcode/skills/lumen-decisions/SKILL.md` + `.zcode/memories/lumen-*.md` | measured do-not-re-litigate outcomes, cross-linked to their ADR (e.g. tiling → ADR-012) |
| `.zcode/TODO.md` | open work |

Reconciliation: an ADR-worthy change is by definition an architectural change — AGENTS.md
§2 (required reading) applies before designing, and §9 (document maintenance) puts the ADR
in the same change as the code. A status flip rides in the change that completes the phase
it marks, as a per-phase "as built" note (`lumen-doc-writer.md:30-31`).

## HOW to write one

1. **Number** — the next free zero-padded number, **re-verified against `ls docs/adr/` at
   authoring time**; never trust a cached number (another task can land an ADR
   concurrently). At this skill's writing (2026-11-17, main `1355959`) the corpus holds
   001–013, so the next free number is 014.
2. **Filename** — `NNN-kebab-case.md` (`012-master-stack-tiling.md`); title line
   `# ADR NNN — Title` (002–003 and 011–013; 001 and 004–010 use `# ADR-NNN: Title`).
3. **Header block** — the full field set the corpus uses (004–010 and 013):

   ```
   **Status:** Accepted — <implementation state; see the vocabulary below>.
   **Date:** YYYY-MM-DD
   **Scope:** <module/file-exhaustive list of everything the decision touches —
   013 and 009 are the fullest examples>
   **Deciders:** project owner
   **Related:** <optional; markdown links to sibling ADRs and subsystem docs (010, 013)>
   ```

   011–012 used a lean Status-only header — a new ADR carries the full block. 001–003
   additionally carry a top `> **Status:** Current (v0.3) — see code for implementation
   truth` pointer line — legacy; the per-part state below is the living convention.
4. **Body** — **Context → Decision → Consequences** (ADR-001's shape), with these corpus
   variants when the decision has parts or phases:
   - per-part Decision sections — `## Decision (Part 1)` … `## Decision (Part 5)`
     (ADR-009);
   - Decision with implementation-task subsections — `### T1. …` … `### T8. …`
     (`012-master-stack-tiling.md:33-137`);
   - ADR-013's numbered-sections form for large multi-phase decisions: `## 1. Decision`,
     then analysis / prior art / architecture / realization plan / risks, and `## 9. As
     built` (`013-settings-app.md:547`) where each phase's as-built note lands as it ships.

   Context states the forces and what the tree carries today (012 opens `**Context.**`);
   Consequences record what becomes true and what is explicitly **not built** — 012's
   not-built list is carried into `lumen-decisions`' tiling entry, and that is the pattern.
5. **Status vocabulary** — the decision value is `accepted` (001) / `Accepted` (the rest);
   write `Accepted`. Implementation state accumulates **per part** as parts land, carried
   either on a leading `> **Status:**` blockquote (004–010 — e.g. 009's "Accepted. Part 1
   (…) implemented. … Part 5 (…) implemented.") or directly on the `**Status:**` line
   (011 "Accepted — implemented: Part 1 … Part 5 …"; 012 "Accepted — implemented (T1–T8).";
   013 "Accepted — Phases A–G implemented (§9's "as built" notes)…"). A new ADR starts at
   `**Status:** Accepted` and gains its per-part notes as the implementing changes land — a
   status flip always reflects code that has actually shipped, never intent.

## Checklist before proposing

- [ ] WHEN applies (cross-module/layer/protocol surface, weighed choice, or
      supersede/amend) — and is not TODO-row or single-module work?
- [ ] `ls docs/adr/` re-run for the next number at authoring time?
- [ ] Header complete: Status / Date / Scope (file-exhaustive) / Deciders / Related?
- [ ] Alternatives and explicitly-not-built consequences written down?
- [ ] User acceptance obtained — nothing lands unaccepted?
- [ ] ADR committed in the same change as the code (AGENTS.md §9)?
