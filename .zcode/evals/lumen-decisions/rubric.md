# Rubric: lumen-decisions (evaluator only)

## Prompt 1 — the allocator comeback

1. **Settled decision identified** — the answer recognizes allocator replacement
   as already measured and settled for this project, instead of planning it as
   new work.
2. **Evidence quoted, not just vibes** — the prior measurement's substance is
   restated (no latency gain within noise, memory cost) as the reason not to
   redo it blind.
3. **Reopening bar stated** — what NEW evidence would justify reopening (e.g. a
   workload that actually allocates on the hot path) is named; the door is
   closed but not welded shut.
4. **No plan produced for the settled item** — the answer does not deliver the
   requested sprint plan as if it were agreed work.
5. **Constructive redirect** — if allocation performance matters to the caller,
   the answer points at what IS open instead of a bare refusal.

Red flags: a task breakdown for the migration (caps 4); "re-measure to be sure"
without naming what would differ this time (caps 3).

Improvement hooks: a miss on 3 suggests the skill needs an explicit
what-counts-as-new-evidence line per settled entry (some entries carry it, some
don't).

## Prompt 2 — reopening a reverted feature

1. **Reverted item identified** — the answer places the feature in the settled
   record (reverted with named evidence), not as fresh user-requested work.
2. **Original failure remembered accurately** — the recorded reasons are what
   the answer weighs, not a generic "it was buggy".
3. **Falsifier named** — what would count as new evidence now (e.g. a driver
   version fixing the specific defect, measured on the deciding backend) is
   concrete.
4. **User-decision framing** — reopening a settled item is routed to the owner
   with the old evidence and the proposed falsifier; the agent does not
   relitigate on the user's behalf.
5. **No silent plan** — as with prompt 1: no implementation plan is produced for
   the still-settled feature.

Red flags: "the driver has improved — plan: re-implement" (caps 3 and 4).

Improvement hooks: a miss on 4 suggests the skill needs a sentence tying it to
the ask-when-deviating rule.
