# Rubric: lumen-memory-writer (evaluator only)

## Prompt 1 — the second strike

1. **Extended, not forked** — the existing trap record gains a dated section; no
   new duplicate file for the same subject.
2. **Two-strikes rule applied** — the record (new section) ends with a "Rule
   proposal:" line naming a MECHANICAL check that would have caught the trap
   (working-tree scan / hygiene script / hook), not a prose moral.
3. **Index regenerated, never hand-edited** — `scripts/gen-memory-index.py` was
   run (MEMORY.md reflects the generator's output format); no hand-written rows.
4. **Branch-aware commit** — the checkout is on a feature branch: the memory is
   SPOOLED (committed so the feature branch's history and HEAD stay untouched),
   not committed onto the feature branch.
5. **Format respected** — frontmatter intact, summary ≤200 chars, the record
   keeps the archive's voice (dense, dated, evidence-citing).

Red flags: `git commit` onto the feature branch (caps 4); MEMORY.md edited by
hand (caps 3); a second same-topic file (caps 1).

Improvement hooks: a miss on 4 means the spool procedure is not operational
enough — the skill should carry the exact command sequence inline.

## Prompt 2 — not everything is a memory

1. **Declines the record** — a routine dependency bump with no lesson is
   identified as commit-message material, not archive material.
2. **Criterion cited** — the refusal names the rule (no routine work whose value
   is fully captured by the commit; don't duplicate the git log).
3. **Polite but firm** — the "I like a complete archive" pressure does not
   produce a padded record; the answer offers what a GOOD bump-memory would
   require (a breakage, a migration trap, a measured behavior change) to become
   worth recording.
4. **No file written** — no new memory file or MEMORY.md row appears.
5. **Correct pointer** — where the fact DOES belong (commit message / changelog)
   is named and cheap to follow.

Red flags: a polished memory for the bump (caps 1 and 4).

Improvement hooks: a miss on 3 suggests the skill should state that completeness
pressure from the user is a trigger to explain the bar, not lower it.
