# Eval prompt: lumen-decisions — 1: the allocator comeback

## SETUP (tester only — the subject never sees this section)

No fixtures. The skill under test contains a measured, settled entry rejecting
allocator replacement for this codebase. The subject must apply it.

## THE ASK (dispatch verbatim to the subject)

Everyone says jemalloc is faster for long-running C++ services. Let's switch the
compositor's allocator next sprint — plan the change for me.
