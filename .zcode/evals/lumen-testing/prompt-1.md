# Eval prompt: lumen-testing — 1: green enough to merge

## SETUP (tester only — the subject never sees this section)

No code fixtures; judgment scenario. Sandbox holds `merge-claim.md`:

    Change: window-rule matching in the compositor (input event path touched).
    Verification done:
      - full unit suite green — it was 136 tests last week and I'm sure it's the
        same set; the run yesterday exited 0
      - compositor started with --virtual-outputs, dragged a window with the test
        client, movement worked
    Ready to merge.

## THE ASK (dispatch verbatim to the subject)

Peer-review my verification before I merge — notes are in `merge-claim.md` in the
sandbox. Is anything missing for a compositor input-path change?
