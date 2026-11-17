---
name: lumen-settings-key-capture-seat
description: "ADR-013 key capture proven on the notebook seat 2026-11-17 (Super+Shift+y recorded, Save wrote the [bindings] line, gen 2, Esc/Backspace/reserved all live); injected Super-chords can trip the shell's Super tap grab and steal keyboard focus mid-chord."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-17 on the notebook `iritur` (i915+nouveau), quiet serialized seat session
(worktree `zcode/task/settings-key-capture-close-out` @ 99e9950, tests-enabled build):
ADR-013's in-window shortcut capture — shipped in `0439bc4` all along, the ADR/TODO text
predating that commit — works end-to-end through the real wl_keyboard path.

- Live evidence (all artifacts + injection JSONL under
  `evidence/2026-11-17-shortcut-capture/`, ASSERTIONS.md maps each check): Record →
  injected Super+Shift+y lands as `Super+Shift+y` (xkb level-0 + `xkb_keysym_to_lower`,
  `main.cpp:105-111`); Save splices `# set by lumen-settings` +
  `"Super+Shift+y" = "shell.launcher"` into `[bindings]` and the compositor's watch logs
  `generation 2 … 58 bindings`; bare Esc cancels the editor; bare Backspace empties the
  field while Ctrl+BackSpace records `Ctrl+BackSpace`; Super+Escape fills the field with
  the inline "reserved by Lumen" error and Save refuses with the config byte-unchanged
  (sha 878a08d7… before and after). 136/136 ctest on the same tree.
- `Ctrl+Alt+F1` can never reach a plain client: `key_table::internal` consumes VT combos
  before anything else ("consumed even when … our own VT, as sway does",
  `keybindings.cppm:125-131`) — probe shots 56/57 pixel-identical, no VT switch.
  `Super+Esc` is the reserved combo that does reach clients (escape hatch "consumed only
  when it acts", `keybindings.cppm:120-123`) — use it to exercise the reserved path live.
- **Trap:** injected `KEY_LEFTMETA+…` chords tripped the shell's Super tap grab twice
  (launcher layer opened, keyboard focus left the app mid-chord): the client's modifier
  state then held a stale "Super" (next bare key recorded as `Super+…`) and later keys
  typed into the launcher. `observe_tap` disarms on any other press
  (`keybindings.cppm:420-459`), so a human chord should not fire it — the observed fire
  points at an event-coalescing corner worth a look if it reproduces under real typing.
  Recovery was automatic: the compositor re-sends the keymap on refocus, which resets
  client state (and by design stops capture — re-click Record).
- **UI fact:** the bindings editor renders at the pane's content *bottom* (appended after
  the action sections, `settings_ui.cppm:2135-2142`) while the code comment says "top" —
  when driving it by injection, scroll fully down before concluding a click failed.
- The "no writable config.toml" banner (open TODO row) shows on fresh start while the
  write path demonstrably works — again on this session.

**Why:** the seat is the only evidence for the live wl_keyboard→xkbcommon→state-machine
wiring (units drive `capture_binding_key` directly), and the tap-grab/focus-theft
interaction cost an hour of misattribution — it looks exactly like "capture silently
records nothing".

**How to apply:** to re-verify: quiet host, `run-desktop.sh --join-seat-session --scale 1
-- lumen-settings`, uinput holder from the tree, keyboard on the internal AT node; avoid
lone-meta taps during capture tests (they open the launcher by design); read the field by
pixels, not by vision-model coordinates — the modal's buttons sit at the content bottom
(Record ≈x1839-1937, Save ≈1956-1983, Cancel ≈2002-2042 at 3200x1800/scale 1 in this
build). Related: [[lumen-compositor-theme-env-export]] (same session tooling),
[[lumen-portal-appearance-settings]] (the banner row), [[lumen-host-build-run-env]].
