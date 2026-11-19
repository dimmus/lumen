# Eval prompt: lumen-adr — 2: the refactor that is not a decision

## SETUP (tester only — the subject never sees this section)

Same sandbox shape as prompt 1 (`docs/adr/` with 001–003 as anchored there; reuse
or rebuild it).

## THE ASK (dispatch verbatim to the subject)

I spent the week refactoring: renamed internal helpers in the foundation module
and split its 2400-line geometry file into four focused files. Feels significant
— write the ADR for it in the sandbox repo's `docs/adr/`.
