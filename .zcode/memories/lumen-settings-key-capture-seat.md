---
name: lumen-settings-key-capture-seat
description: "ADR-013 key capture proven on the notebook seat 2026-11-17 (Super+Shift+y recorded, Save wrote the [bindings] line, gen 2, Esc/Backspace/reserved all live) and closed verify-and-close the same day — docs-only d6de5a5, gate READY, review noise 1/1/0/1; injected Super-chords can trip the shell's Super tap grab and steal keyboard focus mid-chord."
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

**Close-out 2026-11-17 (verify-and-close → gate READY; written on main from the packet +
gate/review evidence):** the card's premise was stale ("capture only via text grammar" —
it shipped in `0439bc4`), so the committed change is **docs-only** and the contingency fix
never triggered:

- Branch `zcode/task/settings-key-capture-close-out` (base 99e9950, unmerged, not pushed —
  push/merge stays the owner's call): `d6de5a5` rewrites ADR-013's three stale places
  (header Status, Phase E as-built paragraph, Status section with counts refreshed from
  this session) and checks the TODO 'Now' row 2 off with the evidence path; `f799387` is
  this record's shorter copy on the branch. At merge this file is add/add — keep main's
  (extended) copy. Branch diff verified docs-only: 4 files (ADR, TODO, memory, MEMORY.md),
  zero product code.
- Gate READY, gatekeeper re-executed the load-bearing checks: rebuild +
  `ctest --test-dir build/test --output-on-failure -LE bench` = **136/136 in 77.52 s**
  (author 77.19 s, review 77.71 s — three green runs of the same tree);
  `-DLUMEN_SETTINGS_HAS_XKBCOMMON=1` in build/test/build.ninja DEFINES and the binary
  links libxkbcommon with the three keysym symbols undefined; the sha256 chain reproduced
  independently (before-save 082ce0e5… → after 878a08d7… = the RECONSTRUCTED file =
  byte-unchanged across the refused Save → RESTORED back to 082ce0e5…; `diff` before vs
  RECONSTRUCTED reproduces save-diff.txt exactly); gen 1→2 (57→58 bindings) at
  logs2/compositor.log:42/:4194; F1 probe pair `cmp` byte-identical; pixel reads of
  42/46/53/54/55-chord/esc/ctrlbs/superesc/saverefused PNGs matched the inj-s2.jsonl tags.
  Status counts re-verified this session by grep: 15 `LUMEN_TEST` registrations in
  test_settings_ui.cpp, 6 in test_config_edit.cpp, 8 in test_ui_inputs.cpp — the string
  `LUMEN_TEST` also matches `LUMEN_TEST_FONT_DIR` ×2 there, so count registrations, not
  string hits.
- **Deviation (disclosed in TODO/ADR/memory/commit):** the reserved combo exercised live
  is **Super+Escape**, not the card's Ctrl+Alt+F1 — `key_table::internal` consumes VT
  combos before any client by design (`keybindings.cppm:125-131`; live proof: probe shots
  56/57 pixel-identical, no VT-switch line), while Super+Escape is the reserved entry that
  does reach plain clients (escape hatch "consumed only when it acts",
  `keybindings.cppm:119-123`) and is the unit test's case (test_settings_ui.cpp:763-765).
- **Review noise (cross-run false-positive tracking): raised 1 / confirmed 1 / refuted 0 /
  preexisting 1.** The finding is the tap-grab corner above — confirmed, tagged
  pre-existing, now an open TODO row on main; the committed diff never touched
  `keybindings.cppm`, so per the gate rule it does not block.
- **Packet gate seat smokes (both exit 1) = invalid executions, not regressions and not
  passes:** each run's own output shows the mandatory uinput holder was absent —
  app-smoke banner "pointer /dev/input/event2 (default: no 'Lumen Inject Mouse')" plus
  kernel EINVAL "no absolute axis 0" on every absolute write; drag-smoke "only 0 of 0
  motions reached a flip". A docs-only diff touches no compositor/display/input surface,
  and the same-day recorded baseline of byte-identical code (265e347,
  `evidence/2026-11-17-seat-redo/`) is drag 10/10 + app-smoke 177/0/0. Holder discipline
  (start from the task tree, hold open, kill by exact pid) is again the deciding factor —
  same family as the Nov-15/16 invalid runs; the caps side is
  [[lumen-uinput-key-bitmap-format]].
