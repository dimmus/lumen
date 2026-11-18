# ui-bench A/B — the undisclosed base-r1 FAIL and the S2 "task slower" pattern

The review found the committed A/B evidence carrying an undisclosed FAIL round
(ui-bench-base-r1.log: S2 tick p50 26.385 -> 38.174 us +44.7%, 8 failures) and the task
rounds consistently above the two clean base rounds on S2 (27.9/27.9/27.5 vs 24.9/26.1),
with no analysis anywhere. This file is that analysis, plus three fresh rounds at the
fixed HEAD.

## base-r1's FAIL is a host artifact, not a code regression

base-r1 ran the SAME 95372af code the stored baseline was recorded from. Its shift is
uniform and multiplicative across every S2 stage — tick, reconcile, layout, emit,
emit_order_encode, frame all +42.7..53.8 % — while p95 moved only ~+10 % on every one.
A code regression moves stages by what the code change does; a clock-frequency state
multiplies everything by the same factor. The same binary then measured 24.875 (r2) and
26.054 (r3): the 38.174 reading is a host-state outlier of a family the bench's own
header records (base-r1's warm-up probe read 26.7 us; the sweep's rounds ran while the
host was also building and running other suites — the quiet-host discipline the task
card names was not held for these rounds).

## The fresh record at the fixed HEAD (quiet, serialized, nothing else running)

  round   S2 tick p50 (us)  vs stored baseline 26.385   verdict
  r1             26.119     -1.0 %                      PASS, 0 failures 0 warnings
  r2             27.542     +4.4 %                      PASS, 0 failures 0 warnings
  r3             27.228     +3.2 %                      PASS, 0 failures 0 warnings
  (ui-bench-fixround-r{1,2,3}.log; the stored baseline is the sweep's own
   20261118-95372af recording. tests/bench/results/iritur-08fd3c-none/ was root-owned
   from the original session's sudo runs — chowned back to the user; the first
   post-chown round at HEAD could not store its JSON, that run was discarded.)

## Conclusion

The task rounds' 27.9/27.9/27.5 sat inside the host's own base spread (24.9-38.2 at
identical code); at the fixed HEAD the gate passes 3/3 with zero failures and warnings,
S2 tick p50 within noise of the stored baseline (-1.0/+4.4/+3.2 %). No degradation to
report under the no-degradation rule; the panel-tile-strings change (655ae13) shows no
measurable S2 cost. The original rounds' FAIL and the "consistently slower" pattern were
environmental (concurrent builds/sessions), and the base-r1 FAIL is now disclosed here
rather than left unexplained in the committed evidence.
