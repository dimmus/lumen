---
name: lumen-uinput-key-bitmap-format
description: "2026-11-17 notebook: /proc/bus/input/devices B: KEY bitmaps print 64-bit words HIGHEST-FIRST, so `70000 0 0 0 0` is the HEALTHY BTN_MOUSE/RIGHT/MIDDLE signature (bits 272/273/274 in word[4]) — the 'no BTN codes registered' poisoning claim in the Nov-15/16 seat-gate reads was a misreading; verify button caps with EVIOCGBIT, never from /proc text; Nov-15/16 click-loss is best attributed to the concurrent-session WARN family"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-17 on the **notebook** (iritur, i915+nouveau), during the seat-gate redo
(`evidence/2026-11-17-seat-redo/`, independent analyst confirmed all four legs).

- **The kernel prints `B: KEY=` bitmaps as 64-bit `unsigned long` words, most-significant
  word first.** Proven on this host by the AT keyboard entry (`B: KEY=20000
  6000000000000020 … fffffffffffffffe`): a 16-hex-digit word is impossible with 32-bit
  words, and the trailing `fffffffffffffffe` = bits 1–63 (ESC..F-keys) confirms the LOW
  word prints last. `70000 0 0 0 0` is therefore word[4] (bits 256–319) = 0x70000 =
  bits 272/273/274 = BTN_MOUSE/BTN_RIGHT/BTN_MIDDLE — the healthy injected-mouse caps line.
- **Ground truth is EVIOCGBIT, not /proc text** (root): `a-smokes/uinput-node-eviocgbit.txt`
  reads BTN_MOUSE(272)=1 BTN_RIGHT(273)=1 BTN_MIDDLE(274)=1, REL_X/REL_Y/REL_WHEEL,
  ABS_X/ABS_Y — format-independent; the same node then passed every click row functionally.
- **Disproven (closed):** "the Nov-15/16 stale holder's node had no BTN codes, so clicks
  physically could not reach the compositor" — that was a misreading of this format, quoted
  verbatim in the old TODO rows and in
  [[lumen-compositor-theme-env-export]]'s gate section (historical record, stays as written).
  The same decode was already noted 2026-11-16 in [[lumen-settings-startup-apply]]'s gate
  section (`70000 0 0 0 0` = BTN_LEFT|RIGHT|MIDDLE, word-4 bits 16-18, touchpad
  cross-checked, watch.jsonl saw BTN_LEFT while the compositor logged zero button lines) —
  this record makes the format fact findable under its own name. Also excluded for Nov-16:
  the older-script-no-REL_X/Y mechanism — the record itself proves pointer motion arrived
  (357 cursor presents, hover n=150/150) while zero button lines appeared. Best-supported
  attribution for the Nov-15/16 click-loss wall: the "another session is running"
  concurrent-session all-FAIL family ([[lumen-app-smoke-known-noise]]). The exact per-run
  mechanism stays unadjudicated.
- The Nov-16 stale holder (pids 3604798/3604800, event16) was killed 2026-11-17; a
  Nov-16-boot_id-unchanged check proves the original Nov-15 holder (2692863) died by kill,
  not reboot.

**Why:** a caps misreading manufactured a confident wrong mechanism ("buttons physically
cannot arrive") that two gate records and two TODO rows then carried as fact — and the true
signature was sitting in the same line, readable once the word order is known.

**How to apply:** before claiming an injected node lacks buttons (or any capability), read
caps with EVIOCGBIT (or functionally exercise one click row) — never parse
`/proc/bus/input/devices` bitmap text by word arithmetic; if you must read it, remember
64-bit words, MSW first. Holders must always be started from the current tree's
`scripts/uinput-mouse.py` and killed by exact pid; a concurrent-session WARN invalidates a
smoke run regardless of injection health.

Related: [[lumen-smoke-bench-harness-green]] (start-from-this-tree rule),
[[lumen-app-smoke-known-noise]] (concurrent-session family),
[[lumen-settings-startup-apply]] (first decode), [[lumen-compositor-theme-env-export]],
[[lumen-portal-appearance-settings]], [[lumen-notebook-hw]].
