# Rubric: lumen-adr (evaluator only)

## Prompt 1 — the weighed choice

1. **WHEN applies, correctly** — the answer treats this as ADR-worthy
   (cross-module, weighed alternatives) without hesitation or invention.
2. **Number derived from the corpus** — the new ADR is numbered 004, derived from
   listing the directory, not from a memorized corpus size.
3. **Header complete** — Status / Date / Scope / Deciders fields all present
   (Related optional); Status starts at Accepted without fake implementation
   notes.
4. **Body shape** — Context → Decision → Consequences; all three weighed options
   appear with why the losers lost; consequences state what is explicitly NOT
   built (e.g. no service process), not only the positive.
5. **Scope honest** — the Scope field lists what the decision actually touches
   (text + UI modules), not boilerplate.

Red flags: any number ≠ 004 (caps 2); consequences that omit the rejected
alternatives (caps 4).

Improvement hooks: a miss on 2 is the eval working as designed — the skill's
re-verify-the-number rule needs strengthening if subjects guess.

## Prompt 2 — the refactor that is not a decision

1. **Negative space applied** — the answer declines to write an ADR:
   single-module implementation detail, no weighed choice, no boundary crossed.
2. **Reason given** — the refusal explains WHICH criterion fails (a decision
   record vs a commit-message matter), citing the corpus's own division.
3. **Redirect to the right record** — the commit message / module docs are named
   as the carrier for this work.
4. **No ADR file created** — no new `docs/adr/004-*.md` (or any ADR) appears in
   the sandbox.
5. **Boundary offered** — the answer says what WOULD flip it into ADR territory
   (e.g. the split changing a public contract or crossing modules).

Red flags: an ADR authored for the refactor (caps 1 and 4 — the classic
over-trigger the skill exists to prevent).

Improvement hooks: a miss on 5 suggests the negative-space section should state
the flip conditions once, generically.
