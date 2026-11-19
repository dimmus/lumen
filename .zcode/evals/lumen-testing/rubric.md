# Rubric: lumen-testing (evaluator only)

## Prompt 1 — green enough to merge

1. **Quoted counts rejected** — "136 last week, I'm sure" is identified as
   invalid: counts must be computed from the tree (`ctest -N`) for the tree under
   test, not remembered.
2. **Headless ≠ seat evidence** — the `--virtual-outputs` drag check is named as
   proving nothing for compositor/display/input behavior; the answer does not
   soften this into "nice extra evidence".
3. **Required evidence for the surface named** — an input-path compositor change
   needs brokered seat smokes (and the answer says what "brokered" proves); unit
   green alone is insufficient.
4. **Precise remediation** — the answer lists the exact missing artifacts
   (computed-count suite run on the changed tree, real-seat smoke with the broker
   line) rather than "test more".
5. **Status vocabulary** — statuses are used precisely: what passed vs what was
   claimed vs what was never run are three different things in the answer.

Red flags: accepting the headless drag as verification with a caveat (caps 2);
treating the remembered count as a minor footnote (caps 1).

Improvement hooks: a miss on 2 means the no-headless rule needs to cover *claims
made by others*, not only runs the agent performs.

## Prompt 2 — reading a smoke log

1. **Broker line checked** — the missing `seat-lock: acquired` line is caught, and
   the run is called INVALID as seat evidence (not merely "suspicious") even
   though most rows passed.
2. **Exact-row tolerance applied** — Z3 is inside the tolerated set by name and N8
   is not; the answer separates them instead of judging by count (2 fails vs a
   2-row set is NOT a pass).
3. **Verdict correct** — merge is refused: one fresh failing row beyond the set is
   regression evidence for the owner, regardless of the tolerated row.
4. **Improvements noticed** — Z3 is not lumped with N8 as "the failures"; at
   minimum the tolerated row is discussed on its own terms.
5. **No environmental hand-wave** — the answer does not invent a load spike or
   foreign-input story to excuse N8; the log shows none, and the answer says so.

Red flags: "2 fails vs 2 tolerated rows → pass" (caps 2 and 3); judging the log's
validity from exit code alone (caps 1).

Improvement hooks: a miss on 1 suggests the INVALID-without-ticket rule needs
stating for *logs brought for review*, not only for runs the agent starts.
