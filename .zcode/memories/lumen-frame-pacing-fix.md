---
name: lumen-frame-pacing-fix
description: "The callback-driven client pacing bug (46–53 fps, a quarter of frames a period late) found on the notebook 2026-11-03 and fixed 2026-11-04: the aim pairing was blind to exactly the late frames and the repaint lead had no whole-path feedback; the fix, the disproven theories, and the measured after"
metadata:
  type: project
---

A `wl_surface.frame`-driven wl_shm client answering in ~60 µs (pres-probe in
~/lumen-compare; GTK-style frame clocks are this class) got 46–53 fps with 25–30 % of its
frames a refresh period late on the HD 5500, while every tick cost 0.4 ms and the health
counters reported **0 missed** ([[lumen-notebook-hw]]). The same run under
`LUMEN_LOG=debug` was 59.8 fps — a red herring: the log time keeps the core busy enough
that the whole path fits the thin lead, it does not reveal the mechanism.

Root causes, found by tracing flips against tick serves (a dumped event ring: F/H/R/T):

1. **The aim pairing kept one slot.** `output_ui::aim_frame` was overwritten by the next
   tick's aim before a slow frame's flip landed (a slow frame's flip lands after the
   following tick has run), so `fb.tag == aim_frame` never matched exactly for late frames
   — health and any feedback were blind to precisely the misses. Fixed with a four-entry
   aim ring; a flip is judged against the vblank its own repaint aimed at.
2. **The repaint lead covered only the measured thread CPU.** The render thread's wake
   after the publish, the commit ioctl's submission, and the kernel's slack to latch a
   commit onto its vblank cost milliseconds on an otherwise idle (down-clocked) core.
   Fixed with `repaint_scheduler::observe_commit_late`: a flip that landed a period late
   feeds |commit − aim| + 250 µs into the lead (windowed peak, clamped to half a period;
   converges in one miss — a slack-plus-step formula that floors at zero when the commit
   issues past the aim does NOT converge). `publish_commit_timing` stamps the submit time
   unconditionally for it (was debug-gated). The tick also opens with
   `release_due_callbacks()` — the docs always described this backstop; the code lacked it.

Theories disproven on the way (do not re-litigate): the client-answer filter in
`note_client_response` (answers ≥ one period dropped) was NOT the cause; callback-timer
lateness (~163 µs) was NOT it; the held-release overwrite by a second flip never happened
(counter read 0); GPU-timing on/off is irrelevant; a bigger fixed margin made it worse
(uniform every-other-frame).

Measured after (eDP 3200×1800@59.982, default logging): six pres-probe runs 59.81–59.89
fps, 1 late frame in 200, interval sd 1.2 ms, commit→present p50 8–12 ms (weston 22.4,
sway 33.3 on the same display). vkmark fifo unchanged (60.00, sd 0.00 — the WSI client
never had the bug); drag-smoke window-move median 16.8 ms against the 18.7 gate (the
honest lead costs ~4 ms against the broken state's 12.7). 134/134 tests.

**Why:** every counter was reporting green while a quarter of the frames were late —
health can only judge what it can pair, and the lead can only cover what it is told.
The observer effect (debug logging "fixing" a race by slowing the racer) cost a day.

**How to apply:** when a pacing or latency bug shows green counters, instrument the
pairing first (does the judge ever see the events it claims to count?) before tuning the
controller. Any new cost between the tick and the flip belongs in
`observe_commit_late`'s diet automatically — never widen `k_client_margin_ns` for it.
Related: [[lumen-notebook-hw]], [[lumen-best-in-class-bar]].
