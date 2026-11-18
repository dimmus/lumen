# Seat regression: attribution and resolution (2026-11-18 evening)

The gate-time seat runs of this task failed app-smoke 5/5 (69–71 PASS, lumen-shell
`Segmentation fault (core dumped)` early in every log). Attribution A/B, all on the same
evening, same seat, same uinput holder (`scripts/uinput-mouse.py`, event16):

| tree | commit | app-smoke |
|---|---|---|
| base (snprintf tip) | `95372af` | **176/0/1** — the single FAIL is the `compositor.memory: critical pressure` clean-log row (present in every run that evening, absent in the morning; host state, not code) |
| sweep, pre-fix | `0e0571c` | 71/101, SEGV |
| bisect leg | `573d7f3` (= `655ae13^`) | **166/0/4, no SEGV** |
| sweep tip | `7870383` | 69–71/102–103, SEGV 5/5 |

Everything between `573d7f3` and the red `0e0571c` is tests/memory/evidence except
`655ae13` — but `655ae13` is a *correct* fix (panel tile strings, ASan
stack-use-after-return). It perturbed the shell's stack layout and flipped a pre-existing
intermittent bug (`.zcode/TODO.md` notification_view SIGSEGV, 2026-11-16 coredumps)
deterministic.

## Root cause (ASan shell on the seat, the TODO's own prescription)

`LUMEN_BUILD_DIR=build/asan` app-smoke: `AddressSanitizer: stack-use-after-return in
strlen` ← `text_block_node::apply` (`text_node.cppm:364`) — **retained text blocks keep
the `const char*` and re-read it at every tick's reconcile**; views that handed them
pointers into `build_ui()` frame storage dangled. Origins caught, in order:

1. panel tiles — fixed by this branch's `655ae13` (panel members)
2. switcher letters — `switcher.cppm:700` `char letter[2]` → `letters_[]` member
3. menu row labels — `menu_popup.cpp:788` mnemonic-stripped `std::string`'s SSO →
   `level::label[]` member storage
4. launcher status line — `launcher.cppm:813` `char line[224]` → `status_line_[]` member

notification_view's own `build_ui` has zero stack text buffers — it was the downstream
victim consuming the poisoned stack (matches its presence in the 2026-11-16 coredumps).

## Files

- `base-95372af-tonight-run.log`, `leg-573d7f3-run.log` — the A/B legs
- `asan-first-run-output.log` — first ASan smoke (switcher origin, full report)
- `asan-smoke2.log`, `asan-smoke3.log` — after the switcher fix (menu origin) and the
  menu fix (launcher origin; 172/1/5)
- `asan-final-run.log`, `asan-final-shell-n.log` — final ASan run with all four fixes

## Remaining known issue (recorded, not introduced here)

`heap-use-after-free READ of size 8` at tray-menu close, after the tray rows passed
(`asan-final-shell-n.log:184-186`; report stack lost to abort + truncation). Menu/tray
code is untouched by this branch → pre-existing on main, ASan-only. New TODO row opened.
