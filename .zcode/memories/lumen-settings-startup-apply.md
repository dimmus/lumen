---
name: lumen-settings-startup-apply
description: "2026-11-16 notebook task (worktree settings-startup-apply, single commit 06ab1e0 off main f16bf5a, unmerged, gate BLOCKED on seat): lumen-settings applies the config [ui] section once at startup — main.cpp:172 calls apply_ui_live(/*theme_too=*/a.theme == nullptr) after set_active_session, before w.open; defaulted `bool theme_too = true` keeps an explicit --theme authoritative for the theme while the config's density still lands, and live_theme_ records config-derived state either way so the no-op guard stays meaningful; test_settings_ui 15/0 + suite 134/134 re-run this session (76.90 s); review noise 0/0/0/0; seat gate BLOCKED again on the same three-worktree click-FAIL signature — and the gate's root-cause claim is DISPROVEN by sysfs: inject node key caps `70000 0 0 0 0` decode to BTN_LEFT|RIGHT|MIDDLE (word-4 bits 16-18, touchpad cross-checked), watch.jsonl saw BTN_LEFT delivered on event16, compositor logged zero button lines"
metadata:
  node_type: memory
  type: project
summary: "lumen-settings applies [ui] once at startup (apply_ui_live before w.open); explicit --theme stays authoritative for theme; seat reds were the concurrent-session family"
modules: executables/lumen-settings
topics: startup, adr-013

---

Verified 2026-11-16 on the **notebook** (iritur: card0→nouveau, card1→i915; the
memory-writer session re-ran every check marked *ran*). Worktree
`/home/dimmus/lumen-wt/settings-startup-apply`, branch `zcode/task/settings-startup-apply`
off main f16bf5a, HEAD **06ab1e0** (2026-11-16 22:34, 3 files +71/−12), **unmerged**
(merge-base HEAD↔main = f16bf5a = main's tip), `git status --short` clean.

**What shipped** — one commit, 06ab1e0:

- **Startup apply:** `executables/lumen-settings/main.cpp:172` (*ran*: read) makes one
  `the_session.apply_ui_live(/*theme_too=*/a.theme == nullptr);` call after
  `set_active_session` (:164-165) and before `w.open` — the layers the session just read
  are the app's own truth, so the first describe renders the config's theme and density
  regardless of the inherited environment (stale LUMEN_THEME, or none on a direct
  launch). Placement safety: `refresh()` is root-null-safe pre-window
  (settings_ui.cppm:326-330, *ran*: read `if (root && root->tree())`); the theme/density
  setters are lx.ui engine globals.
- **Precedence split as a defaulted parameter** (settings_ui.cppm:222-226 declaration,
  :1405 definition, *ran*: read): `void apply_ui_live(bool theme_too = true)`. When main
  has an explicit `--theme` (which themed the app context at `app.start()`), it passes
  `false` — the flag stays authoritative for the theme while the config's density still
  applies; with no flag the config truth wins over the env. The two existing edit-path
  call sites are unchanged (minimal diff, one code path).
- **Guard semantics:** the `live_theme_` guard (:1412-1414, seed `"\x01"` at :390 so the
  first call always applies) and the state recording (:1415-1417) run *outside* the
  `theme_too` branch — under `--theme` an unchanged-config edit stays a no-op (the
  flag's theme survives) and the first real theme edit applies the file as before. The
  flag's theme now survives strictly longer than pre-change: any first [ui] edit used to
  flip it.
- No in-repo script uses `--theme` (grep over scripts/ and .zcode/skills/ — re-run by
  review; it is documented CLI only), so nothing else changes meaning. No doc update owed
  (no API/architecture change); consistent with ADR-013 §4.7 "settings app = live".

**Evidence** (*ran* = this session, at worktree HEAD 06ab1e0):

- `grep LUMEN_BUILD_TESTS build/test/CMakeCache.txt` → **BOOL=ON**.
- `./build/test/test_settings_ui` → **15 tests, 0 failed**, including the new
  `settings_startup_apply_renders_the_config_ui_section` (tests/test_settings_ui.cpp:773,
  *ran*: read): LUMEN_CONFIG temp file with appearance=light + density=compact flips
  `active_theme().appearance_mode()` to light and `ui_density()` to compact over a
  dark-seeded context; a second apply is a `theme_generation()` no-op (the live_theme_
  guard); `theme_too=false` keeps the flag's dark while the config's density lands.
- `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed out
  of 134", 76.90 s**.
- *Not re-run by me (packet-reported)*: the author's unattended seat check —
  compositor on socket lumen-settings-check reading the same forced config;
  `lumen-settings --exit-after-first-present` exit 0 (presented=1) in all three launch
  modes (plain; stale `LUMEN_THEME=one-dark`; `--theme one-dark`). The interactive
  acceptance items (2: set appearance in Settings, close/reopen, light topbar, dark
  flip, lumen-hello; 3: visual `--theme one-light` over a dark config — proven only at
  unit level over the mirrored case; 4: the exit leg's saved artifacts) were never
  executed by anyone — the gate's missing-evidence list, not a failure.

**Gate verdict BLOCKED — and the gate's root-cause diagnosis is disproven (ran, this
session):** both smokes `VERDICT: FAIL` (drag-smoke-logs/run.log, app-smoke-logs/run.log;
`grep -c FAIL app-smoke-logs/run.log` → 101; drag rows at delta +0,+0 grab=none;
square1.log zero button lines) — the byte-identical signature across three consecutive
worktrees with disjoint diffs ([[lumen-notebook-full-green-audit]] was 177/0/0 on Nov 15).

- **Disproven: "the inject node has no BTN codes, clicks physically cannot reach the
  compositor."** `/sys/class/input/input29/capabilities/key` (*ran*) reads
  **`70000 0 0 0 0`** — sysfs prints bitmap words top-word-first; word 4 × bits 16-18 =
  bits **272-274 = BTN_LEFT|BTN_RIGHT|BTN_MIDDLE**. Cross-checked (*ran*): the Synaptics
  TouchPad reads `e520 10000 0 0 0 0` — its word 4 `0x10000` is exactly bit 272
  (BTN_LEFT) at the same position, and a keyboard's low words hold the KEY bits dense;
  `ev` = `f` (SYN|KEY|REL|MSC) — a complete 3-button REL mouse. The previous task's gate
  and its memory ([[lumen-compositor-theme-env-export]]) carry the same misread.
- **Kernel delivery verified:** drag-smoke-logs/watch.jsonl has
  `{"kind":"seen","dev":"/dev/input/event16","ev":[[1,272,1]]}` press/release pairs
  (*ran*) — injection reached evdev on a node that has button caps.
- **Compositor saw none:** drag compositor.log has zero real button/grab lines (719
  pointer/motion/cursor lines; the `button|grab` grep hits only device-enumeration text,
  *ran*) — the loss sits between kernel and compositor, mechanism unattributed. Neither
  lx.input nor scripts/uinput-mouse.py EVIOCGRABs anything (*ran*: grep, no hits).
- The concurrent-session WARN **is** present at drag run.log line 2 ("another session is
  running (WAYLAND_DISPLAY='lumen-0'"), the documented all-FAIL family
  ([[lumen-app-smoke-known-noise]]); no lumen-compositor runs now and no lumen-0 socket
  exists (*ran*) — the interference existed at smoke time only.
- **Stale holder still alive** (*ran*): pids 2692858/2692860/2692863 since Nov 15
  12:22; pgrep -af shows the originating shell verbatim — `cd
  /home/dimmus/lumen-wt/smoke-bench-harness-green && sudo -n setsid python3
  scripts/uinput-mouse.py --output 3200x1800` — confirming the start-from-this-tree
  violation attribution. Killing it stays right for hygiene, but it is not a proven
  click-loss mechanism.
- **Consequence for the open TODO row** (compositor-theme-env-export seat gate): its
  "regenerate the node and verify BTN codes" step is **already satisfied** — the node
  has button caps. The decisive step is the quiet-host serialized re-run with a main
  baseline A/B.
- lumen-shell segfaulted again in app-smoke (*ran*: run.log `line 3284: 3593680
  Segmentation fault (core dumped)`; gate reports coredumps 3571365 @22:55:05 +
  3593680 @23:06:37, same notification_view::build_ui → lx::ui::image →
  memory_arena::allocate stack) — the TODO-tracked pre-existing/unattributed crash
  family; this diff touches no lx.ui/lx.shell code.

**Review noise (cross-run false-positive tracking):** raised **0** / confirmed **0** /
refuted **0** / preexisting **0** — a zero-finding review; packet `preexistingFindings`
empty, no new TODO rows.

**Deviations:** none within card scope. Two evidence boundaries stated plainly by the
author: the interactive seat acceptance was deferred to the seat-smoke stage, and the
`--theme` precedence has no rendered-pixel comparison (unit-level mirrored case +
flag-path seat exit 0).

**Disproven / do not re-chase:** "the inject node lacks button caps" — it has all three
BTN codes (decode above; re-check any caps claim by computing word_index×64+bit, never
by eyeballing the hex). "The reds are a regression of this change" — identical signature
across three disjoint diffs and the change's code paths are not in the failing rows.
"apply_ui_live at startup is unsafe pre-window" — root-null-safe refresh, engine-global
setters, proven by the unit test and the author's three launch-mode seat runs.

**Why:** a settings app must render its own configuration at startup rather than inherit
whatever the environment carries — and precedence must be an explicit split (a flag wins
for what it set; the config supplies the rest) while the no-op guard keeps tracking
config-derived state, or "unchanged config" silently re-flips the flag's theme. The gate
is the second lesson: a written diagnosis can be wrong in its central mechanism claim
(here a misread evdev bitmap), and executing its prescription would "verify" a check
that was already true while the real cause stayed hidden.

**How to apply:** to close the blocked seat gate: get the host quiet (`pgrep -x
lumen-composito`, no lumen-0 socket), kill the holder tree (pids 2692858/2692860/2692863,
`sudo pkill -f uinput-mouse`) and regenerate from THIS tree per the TODO — but expect
the click reds to need the quiet-host re-run plus a main-baseline A/B, not the node
swap. To read evdev key caps: `/sys/class/input/inputN/capabilities/key` words run
top-first; bit = word_index×64+bit; BTN_LEFT=272 → word 4 bit 16 (`…10000` there is
BTN_LEFT, not a missing-button bitmap). For lumen-settings startup theming: the config's
[ui] applies once before `w.open` via `apply_ui_live`; `--theme` keeps the theme and the
config keeps density; extend `settings_startup_apply_renders_the_config_ui_section`
rather than adding a parallel mechanism. Then execute the deferred acceptance with
artifacts: interactive close/reopen (light topbar, dark flip, lumen-hello builtin),
`--theme one-light` over a dark config visual, and a saved `--exit-after-first-present`
log.

Related: [[lumen-compositor-theme-env-export]] (the export side of the same ADR-013
truth chain — config layer is the source of truth, the env channel serves later-started
apps; its gate record carries the same caps misread), [[lumen-portal-appearance-settings]]
(the D-Bus side; this task is the second half of its ask's repro B),
[[lumen-app-smoke-known-noise]] (concurrent-session → all-FAIL rule),
[[lumen-notebook-full-green-audit]] (the 177/0/0 green the signature contradicts),
[[lumen-smoke-bench-harness-green]] (holder autodetect + start-from-this-tree rule),
[[lumen-notebook-hw]] (this host).
