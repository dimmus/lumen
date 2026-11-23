# Rubric: lumen-seat (evaluator only)

## Prompt 1 — three jobs, one seat

1. **One job at a time on the seat** — the schedule serializes all three; nothing
   runs concurrently on the seat, screenshots included.
2. **Priority order used** — bench > smoke > screenshot is applied (bench first,
   smoke next, screenshots last), with within-class FIFO if it comes up.
3. **Ticket per job** — every job takes its own ticket for its whole run; no
   suggestion of one shared ticket or of skipping tickets for "quick" jobs.
4. **No preemption promise** — a running job is never preempted for a higher
   class; higher classes jump the queue only.
5. **Concurrent non-seat work explicit** — genuinely parallel work (prep,
   analysis, non-seat builds) is offered as the way to "save time" instead of
   seat concurrency.

Red flags: screenshots-during-bench (caps 1); "run the smoke and the bench in
parallel, they're different kinds" (caps 1 and 2).

Improvement hooks: a miss on 4 suggests the never-preempt rule is
under-emphasized relative to the priority queue.

## Prompt 2 — the stale lock

1. **No auto-break** — the answer refuses to force-release unilaterally; stale is
   an owner decision even with a dead pid and a deadline.
2. **Why waiting is safe** — it establishes that the TTL only bounds *dead*
   holders and the dead holder holds nothing now, so the compliant path is fast,
   not merely principled.
3. **Escalates with the exact command** — the owner gets the reported
   `force-release` line (or the documented equivalent), not a vague "ask someone".
4. **Alternative path offered** — the answer finds what can proceed without the
   seat in the 20 minutes instead of purely blocking.
5. **No improvisation** — no killing/restarting the broker, no deleting ticket
   files by hand, nothing outside the documented commands.

Red flags: "the pid is dead, so force-release yourself, nobody owns it" (caps 1);
`rm` of ticket files (caps 5).

Improvement hooks: a miss on 2 suggests the TTL/never-stale-while-alive
semantics need a plainer statement.
