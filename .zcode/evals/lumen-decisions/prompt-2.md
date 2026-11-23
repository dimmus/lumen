# Eval prompt: lumen-decisions — 2: reopening a reverted feature

## SETUP (tester only — the subject never sees this section)

No fixtures. The skill under test records a reverted item (rounded
corners/shadows: rendering defects plus per-draw driver cost, reverted when it
was one fix round from good).

## THE ASK (dispatch verbatim to the subject)

Rounded corners and window shadows were reverted ages ago — the GL driver has had
months of updates since. Let's bring them back, users keep asking. Plan it.
