---
name: lumen-memory-writer
description: Write a Lumen project memory record in .zcode/memories/ — important changes, measured evidence, research results, test/bench/smoke outcomes, fuckups and their solutions, and new findings — in the archive's established format, and index it in MEMORY.md. Use at the end of any task that produced evidence or lessons worth keeping; also when the user asks to remember a finding.
---

# Writing Lumen memories

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
- a new or useful finding about the environment (hosts, seat, injection, ccache, CRLF…).

Do not write a memory for routine work whose value is fully captured by the commit
message and docs; do not duplicate an existing memory — extend it (append a dated section)
instead of forking a new one.

## Format (match the existing archive)

File: `.zcode/memories/lumen-<kebab-topic>.md`, topic-specific, one subject per file.

```markdown
---
name: lumen-<kebab-topic>
description: "One dense sentence with the numbers/dates a search would key on."
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

- The `description` is a search snippet — dates, numbers, host names belong in it.
- Disproven theories get written down (that is most of the archive's value).
- Memories are **historical**: never cite one as proof of current behavior; current truth
  is code + git history + ADRs. Every memory should say when it was verified.
- 10–40 lines is the archive's voice; a memory that needs more is two memories.

## Indexing

Add one row to `.zcode/memories/MEMORY.md`:

`- [Title](lumen-<topic>.md) — one-line summary`

## Committing

Memory files are checked in. Commit the new/updated memory and the MEMORY.md row on the
current branch (usually the harness/main tree, not a task worktree) with
`memory: <topic>` as the subject. Never push unless asked.
