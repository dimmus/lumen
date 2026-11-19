---
name: lumen-memory-writer
description: Write a Lumen project memory record in .zcode/memories/ — important changes, measured evidence, research results, test/bench/smoke outcomes, fuckups and their solutions, and new findings — in the archive's format (frontmatter summary/modules/topics/supersedes, ≤200-char index rows, ≤120-line records), index it via scripts/gen-memory-index.py, and commit it on main or spooled to zcode/memory-spool. Use at the end of any task that produced evidence or lessons worth keeping; also when the user asks to remember a finding.
---

# Writing Lumen memories

> **Scope:** mixed — the archive format and rules are general; paths, the generator script and the spool ref are current-project.

`.zcode/memories/` is the project's long-term evidence archive: what was measured, what
broke and how it was fixed, what was learned about hosts, protocols and upstreams. A
memory's job is to save a future session from re-deriving (or re-breaking) something.
Write one when a task produced any of:

- an important change (design decision, new subsystem behavior, protocol addition) and
  the evidence for it;
- measurement results worth quoting later (bench/smoke/A/B numbers with their artifacts);
- research conclusions (upstream behavior, driver quirks, protocol semantics) that took
  real work to establish;
- a fuckup and its solution — the trap, the symptom, the fix, and how to recognize it
  next time (these are the most valuable records in the archive);
- a new or useful finding about the environment (hosts, seat, injection, ccache…).

Do not write a memory for routine work whose value is fully captured by the commit
message and docs; do not duplicate an existing memory — extend it (append a dated
section) instead of forking a new one.

## Format (match the existing archive)

File: `.zcode/memories/lumen-<kebab-topic>.md`, topic-specific, one subject per file.

```markdown
---
name: lumen-<kebab-topic>
description: "One dense sentence with the numbers/dates a search would key on."
summary: "≤200 chars — this becomes the MEMORY.md row; detail lives in the record."
modules: lx.compositor, scripts          # retrieval keys for INDEX.md
topics: drag, injection                  # optional
supersedes: lumen-old-record             # only when replacing one
metadata:
  node_type: memory
  type: project
---

<First line: what was verified, when, on which host. "Verified 2026-11-12 on the vmwgfx
VM: ...">

<Dated bullet lists of verified facts. Numbers-heavy, past tense, no filler. Cite
evidence inline: PR numbers, merge SHAs, file paths, artifact paths, commands.>

**Why:** <one or two sentences — the general lesson a fresh session would otherwise
re-derive the hard way>

**How to apply:** <imperative: exact commands, what to extend, what never to do>

Cross-reference related records with [[lumen-other-memory]] wiki-links. List disproven
theories explicitly so they are not re-litigated.
```

Rules:

- The `description` is a search snippet — dates, numbers, host names belong in it. The
  `summary` is the index row (≤200 chars, enforced by the generator — a longer one fails
  regeneration; the fix is shortening the summary, never the detail out of the record).
- **Records stay ≤120 lines.** A record that grows past that splits: the durable content
  (summary facts, Why, How) stays, the long close-out chronology moves to
  `lumen-<topic>-rounds.md` / `-closeout.md` with its own thin frontmatter and a
  cross-link both ways. The three original overgrowths (unit-pair-green,
  smoke-bench-harness-green, vulkan-capture-green) were split this way on 2026-11-19.
- Memories are **historical**: never cite one as proof of current behavior; current truth
  is code + git history + ADRs. Every memory should say when it was verified.
- **Two strikes become a rule** (the postmortem loop): when the trap this record
  documents is the second recorded occurrence of its class, the record MUST end with a
  `Rule proposal:` line naming the mechanical check that would have caught it — a
  reviewer checklist item, a hook, or a ctest. A trap seen twice and left as prose is
  the archive failing its job. (Delivered examples: the seat broker after three
  concurrent-session incident families; diff-hygiene.sh after two CRLF incidents.)

## Indexing — generated, never hand-written

After writing or extending a record, regenerate both indexes:

```bash
python3 scripts/gen-memory-index.py <repo-root>
```

`MEMORY.md` (flat, one ≤200-char row per record) and `INDEX.md` (grouped by module with
topics and dates — the retrieval layer for `lumen-decisions`/`lumen-build-run` lookups)
are generated files; do not hand-edit their rows. The generator fails on an over-cap
`summary:` and on malformed frontmatter — fix the record, not the index.

## Committing — decided by the branch, not by habit

Memory files are checked in. Check `git branch --show-current` in the MAIN checkout:

- **main** → merge any existing `zcode/memory-spool` first
  (`git merge --ff-only zcode/memory-spool`; if it does not fast-forward, stop and
  report), then commit the records + regenerated indexes (+ `.zcode/TODO.md` rows) with
  subject `memory: <topic>`. Never push unless asked.
- **any other branch** (a gated run left the checkout on a feature branch) → **spool**:
  write the files, then commit them to `refs/heads/zcode/memory-spool` through a
  temporary index so the feature branch and HEAD stay untouched — the exact command
  sequence is in `.zcode/agents/lumen-memory-writer.md`. A memory committed onto a
  feature branch is lost on switch-back; the spool is what prevents that.

When the task's packet lists pre-existing findings, add one open row each to
`.zcode/TODO.md` and commit it together with the memory.
