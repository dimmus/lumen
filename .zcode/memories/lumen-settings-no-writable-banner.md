---
name: lumen-settings-no-writable-banner
description: "2026-11-24 notebook: the lumen-settings 'There is no writable config.toml' banner REPRODUCED and fixed (023443e, gate READY at ebedef5, branch zcode/task/settings-no-writable-banner) — it is up from launch, not a ~10 s transient: wl_keyboard.keymap arrives 86 ms after spawn, on_keymap (main.cpp:99) → stop_binding_capture() → session::repaint() flipped untouched→memory_only, and ten non-edit call sites rode the same path; fix routes non-edits through refresh(), leaves repaint() to the five write-failure tails, adds settings::k_banner_key; regression test red pre-fix (13/13 callers raise), green after; seat A/B (tickets -10256 pre / -15838 post): banner PRESENT 13/17 shots pre, only the unwritable edit f1/f2 post; ctest 141/141 -LE bench at ebedef5; sibling defect (no banner from `written`) fixed 2026-11-25 — [[lumen-settings-written-banner-escalation]]."
metadata:
  node_type: memory
  type: project
summary: "settings banner up from launch (keymap +86 ms → repaint() memory_only via 10 non-edit callers); fixed 023443e (non-edits refresh()); seat A/B 13/17 pre PRESENT → only unwritable edit; ctest 141/141"
modules: executables/lumen-settings, tests
topics: banner, seat-repro, wayland-debug, injection

---

Verified 2026-11-24 on the **notebook** (iritur i915+nouveau, [[lumen-notebook-hw]]). Worktree
`/home/dimmus/lumen-wt/settings-no-writable-banner`, branch `zcode/task/settings-no-writable-banner`,
base a89cbad, commits **023443e** (fix) + **ebedef5** (TODO row ticked); gate READY; evidence under
`evidence/2026-11-24-settings-no-writable-banner/` in the worktree, mirrored byte-identical
(diff -rq empty, 468 files) to `/home/dimmus/lumen/evidence/2026-11-24-settings-no-writable-banner/`
— start with REPRO.md. Sightings: 2026-11-17 ([[lumen-portal-appearance-settings]] :152, TODO row),
again that session ([[lumen-settings-key-capture-seat]] :43); root cause read from source 2026-11-23.

- **Correction of the row's premise:** the banner is not "transient, ~10 s after launch" — on the
  pre-fix build it is up **by the first capture at +2 s** with zero input events, still at +10 s and
  +30 s; three independent pre-fix jobs agree.
- **Root cause (a89cbad):** `session::repaint()` flipped `untouched → memory_only` on *any* call
  (settings_ui.cppm:1250-1254) and the banner is gated on that state alone (:2419-2420). Ten call
  sites that are not edits went through it: set_current, toggle_expanded, open_group,
  begin_edit_action, cancel_edit_action, begin_binding_capture, stop_binding_capture, set_open_menu,
  capture_binding_key ×2 (:166, :177, :188, :266, :271, :281, :286, :316, :1623, :1645 at base).
  `memory_only` is correct only for the failure tails of set_bool/set_int/set_number/set_enum/
  commit_string.
- **The launch trigger, observed:** `main.cpp:99` calls `stop_binding_capture()` from
  `shortcut_input::on_keymap`; `WAYLAND_DEBUG=1` trace (keymap-trace/, no input): first commit +13 ms,
  **wl_keyboard#26.keymap +86 ms**, keyboard enter +108 ms — on_keymap runs in the roundtrip after
  shortcut_input's construction, so every fresh launch raised the banner ~86 ms in. main.cpp:95-96
  adds focus-out as a second no-edit trigger.
- **Seat A/B, one class-smoke ticket per job** (first line of each seat-job.log): pre
  ticket-20261124041104-366099-10256 (binary df98ef70… from base source ece0bf9d…), post
  ticket-20261124043509-381077-15838 (0e83b4ed… from HEAD source 8f1bd744…). Both jobs: seat quiet
  before/after, config.toml afd5acd7… restored byte-identical, LUMEN_SMOKE_SCALE=1, foreign-input
  CLEAN, compositor and shell binaries identical across the jobs (the A/B isolates lumen-settings);
  the edits wrote byte-identical files on both builds (e1 ca6415f5…, g1 4a0effd8…).
- **Verdicts** (verdict.py, two window-relative patches; PRESENT at mean-diff ≥ 6; banner
  (59,59,59) vs background (40,44,51), d=19): pre PRESENT on 13/17 shots (a1-a4 launch, b1-b2
  dropdown open, c1-c2 dismissed, d1-d2 pane switch, f0-f2 read-only dir), ABSENT only after a
  successful write (e1/e2, g1/g2); post ABSENT 15/17, PRESENT only f1/f2 — the edit that cannot be
  written. Reviewer re-judged 18 shots independently, same numbers.
- **Decision per trigger:** (a) fresh launch no-input RAISED it (the reproduced trigger); (b) (c) (d)
  cannot be judged apart on the seat — the banner is already up when they happen — so they were
  decided in the unit harness: each flips the state on pre-fix logic, none on the fix; (e) one real
  edit CLEARED it on both builds. (f)/(g) ran on both builds: the legitimate case (banner for an
  unwritable edit, gone after a successful write) is intact.
- **The fix (023443e):** the ten non-edit sites now call `refresh()` — the module's "re-describe
  without touching the save state" idiom; `repaint()` keeps the flip and is called only by the five
  failure tails (:1269, :1288, :1334, :1377, :1409 + declaration :326 at HEAD; grep `repaint();`).
  main.cpp needed no change. The banner box gained the element key `settings::k_banner_key`
  (= 0x73'0006, :425, applied :2453) so the test asserts the banner itself; no pixel changes. No
  legitimate banner lost: bindings-editor/reset failures report through error_/reset_note_, untouched.
- **Regression test** `settings_banner_is_raised_only_by_an_edit_that_did_not_reach_the_file`
  (16th LUMEN_TEST in test_settings_ui): 13 non-edit caller scenarios from `untouched` over a
  writable file stay down; the seat's (b)(c)(d) through the real controls stay down; (f)(g) with the
  app's own session over a read-only dir — edit stays in session, banner up while the header still
  names the file, nothing written; writable again, write lands, banner gone. RED on base + k_banner_key
  only: "13 of the 13 non-edit callers raised the banner", CHECK(raised == 0) failed, 16 tests 1
  failed; GREEN after. Suite 141/141 `-LE bench` at base (82.58 s), fix (79.88 s), ebedef5 (80.54 s);
  146 registered, 5 bench. Caveats: the red run is implementer-only, not re-executed; the main.cpp
  launch wiring has **no automated test** (the test drives a bare session) — a regression
  reintroduced at main.cpp:95-99 passes the suite.
- **Host findings:** a fresh compositor starts its pointer at the output centre —
  `wl_pointer.enter(surface, 480, 306)` — so "no input" in a repro means no input *event* while the
  pointer hovers the window (hover calls no session method; first input +97 s pre / +56 s post);
  `WAYLAND_DEBUG=1` on the client is the tool that names a launch-time trigger like this one.
- **Adjacent, NOT fixed here (own TODO row):** `repaint()` escalates only from `untouched`, so after
one successful write (`state == written`) a later edit that cannot reach the file is applied to the
session and shows nothing — the banner's purpose, missed; probe in the unit harness, never
exercised on the seat. Found while probing the fix; it is the mirror image of the false banner.
(Fixed 2026-11-25 — [[lumen-settings-written-banner-escalation]].)
- Deviations of record: six seat jobs for a card that asked for one — the two invalid pre attempts
  (driver sent clicks without the window origin; the Cursor "Theme" text input mistaken for a
  dropdown), post-run1 (repeat of the fixed run) and the keymap trace each hold their own ticket and
  README; `lumen-settings-shot` drew the banner in every one of its shots pre-fix (it opens panes via
  set_current) and none after — no golden encodes the banner.

**Review noise (cross-run false-positive tracking): raised 3 / confirmed 3 / refuted 0 / preexisting
3.** All three confirmed findings are PRE at base a89cbad: the written-state banner gap above
(medium, now its own open TODO row), whole-file clang-format debt on both changed C++ files making
diff-hygiene exit 1 (low, now an open TODO row), and diff-hygiene.sh:92 `local` outside a function +
its /tmp leak (low — already tracked from the invariant-gap-tests round).

**Why:** a save-state flag named for a failure must be set only where the failure happens — a shared
"redraw" helper carried the transition, and thirteen call scenarios raised "no writable config.toml"
over a writable file from 2026-11-17 until this fix. And a fix's own probe found its mirror image
(banner missing when it should show): probe both directions of a defect gate before calling the fix
complete.

**How to apply:** to re-verify: `evidence/2026-11-24-settings-no-writable-banner/seat-job.sh pre|post`
(ticket per job, verdict.py on patches P/Q, REPRO.md §1 is the procedure). When a task's defect is a
state gate, grep **all** callers of the state-setting helper before judging a fix minimal — the lead's
list missed capture_binding_key. Do not cite drag-smoke/app-smoke as settings evidence: neither script
launches lumen-settings, and their binaries were byte-identical across these jobs. When adding UI
behind a state, add the element key in the same change so the test can see the pixels, not just the
flag.

Rule proposal: second recorded occurrence of the injection-targeting family
([[lumen-settings-key-capture-seat]]: the bindings editor at the pane's bottom misread as a failed
click; here: a text input mistaken for a dropdown, and window-relative clicks sent without the
origin). Mechanical check: a seat timeline step may click only a target whose control type and
window-relative geometry were read off the live shot (findwin.py / the coords sync files); an
attempt found mis-targeted is voided whole with a README, and none of its steps may be cited.

Related: [[lumen-settings-key-capture-seat]] (earlier sightings; the injection-targeting family),
[[lumen-portal-appearance-settings]] (the 2026-11-17 sighting and its session),
[[lumen-notebook-hw]] (the host), [[lumen-invariant-gap-tests]] (the round that first tracked
diff-hygiene.sh:92).
