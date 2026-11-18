# app-smoke — the quiet-host re-runs (honest record)

The committed evidence's verdict was FAIL (176 PASS / 1 FAIL on the clean-log check, its
first error the compositor.memory critical-pressure WARN) while the task card expected
full-green 177/0/0 on a quiet serialized host, and the drag-smoke log showed another
session was live during the runs. The re-runs tonight (quiet host: no concurrent builds,
sessions or benches; the uinput holder restarted from the current script, REL_X/REL_Y
verified present) do NOT reproduce the morning's 176-PASS — on either tree:

  run                                   tree                    PASS/FAIL   shell SEGV
  fixround1 (found release dir)         fixed HEAD              69/?-109     yes (coredump)
  fixround2 (same binaries)             fixed HEAD              70/…         yes
  bisect (clean build)                  0e0571c (pre-fix)      71/101       yes (same backtrace)
  fixround3 (clean release rebuild)     fixed HEAD              71/…         yes
  fixround4 (fresh uinput holder)       fixed HEAD              70/102       yes

The failure signature is IDENTICAL on the pre-fix sweep tree and the fixed HEAD: the
first input-driven scenario (A2 foot CSD title drag, "grab=-") and everything downstream
fails; ~70 pure-protocol checks pass; and one lumen-shell instance dies late in the run —
coredump backtrace identical every time:

  memory_arena::allocate (liblx.runtime)
    ← describe<image_node, image_props> (liblx.ui)
    ← ui::image (liblx.ui)
    ← notification_view::build_ui (liblx.shell)

Because the pre-fix tree (the sweep's own final state, clean-built in a separate worktree)
fails the same way, this is NOT a regression of the review-fix round. What it is remains
open: the mass failure points at early-run input-to-client delivery (the compositor logs
pointer motion and later grabs — e.g. Super+drags in C/D groups — while the first
scenarios' client grabs never register), and the shell crash is in the notification view's
arena path. Both reproduce only under the full smoke (a standalone compositor+shell runs
35 s cleanly), and both were ABSENT from the morning run whose host was NOT quiet. The
memory-pressure WARN appears in some runs (fixround1/2/3) and not others (fixround4), so
it is noise-adjacent but not the mass failure's cause — the morning run had it too and
still passed 176.

Escalated to the gate as-is: app-smoke on this seat, tonight, is not green on any tree;
the smoke needs its own investigation (input delivery + the notification_view crash)
before its verdict can gate anything.
