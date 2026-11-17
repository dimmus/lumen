---
name: lumen-compositor-theme-env-export
description: "2026-11-16 notebook task (worktree compositor-theme-env-export, 5df83bc + fixes 6ba8fdf/655a586/e5a1175, HEAD e5a1175, unmerged, gate BLOCKED on seat): compositor always exports a resolved LUMEN_THEME (resolved_ui_theme — [ui] theme name/.lxtheme path as written, else appearance→one-light/one-dark) into the systemd user-manager environment and re-exports it when a reload changes the resolved value (env_reexports owe/drain/drop in lx.compositor:config_apply — one-at-a-time user bus, cursor-first drain, refused export no longer counts as carried); test_theme_env 8/8 and suite 135/135 re-run this session (76.96 s); seat gate BLOCKED — acceptance 2-4 never run, both smokes invalid not regressed (stale buttonless uinput holder event16 `70000 0 0 0 0`, open since Nov 15 12:22); UNATTRIBUTED lumen-shell notification_view::build_ui SIGSEGV ×2 (coredumps 3499403/3521729) — TODO-tracked; review noise 4/4/0/0"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-16 on the **notebook** (iritur: `readlink` card0→nouveau, card1→i915; the
memory-writer session re-ran the executable checks below, each marked *ran*). Worktree
`/home/dimmus/lumen-wt/compositor-theme-env-export`, branch `zcode/task/compositor-theme-env-export`
off main 7d5806a, HEAD **e5a1175**, `git status --short` clean, **unmerged** (merge-base
HEAD↔main = 7d5806a = main's own tip).

**What shipped** — 5df83bc 20:54 (6 files +177/−10: compositor_impl.cpp, config_apply
.cppm/.cpp, tests/test_theme_env.cpp, CMakeLists.txt +8, deploy/lumen/config.toml comment),
review fixes 6ba8fdf 21:19 (+153/−23, bookkeeping → env_reexports + 5 tests), 655a586 21:21
(+34/−1, theme_forget), e5a1175 21:23 (docs-only, ADR-013):

- `resolved_ui_theme` (modules/lx.compositor/config_apply.cpp:103-109, *ran*: read): the
  [ui] theme name/`.lxtheme` path **as written** when one is configured — byte-identical to
  the pre-change export — else the built-in the appearance selects ("one-light"/"one-dark").
  Reads only theme+appearance, so density/ui-font-size edits resolve to the same value.
- Startup: start_session **always** appends LUMEN_THEME from `resolved_ui_theme`
  (compositor_impl.cpp:3896-3911) and records the value via `theme_sent`; `finish_config_generation`
  calls `export_theme_environment()` (compositor_impl.cpp:4886), which sends **only when the
  resolved value differs** from the last one exported (`theme_changed` gate at :4749) —
  density/font-size churn nothing. Running apps intentionally keep their theme; reaching
  *later-started* apps is the point: the shell launches them in systemd StartTransientUnit
  scopes (modules/lx.shell/app_launch.cppm:44,1548) that inherit the user-manager
  environment SetEnvironment updates, and lx.ui.host's fallback (context.cpp:256-261) is
  one-dark **ignoring appearance** — the export is the only channel that carries it.
- One-at-a-time user bus (`lx.session user_environment`, one pending export): the
  `env_reexports` machine (config_apply.cppm:118-163, *ran*: read) marks a re-export that
  finds the bus busy as **owed** and leaves from that export's reply (`drain`, cursor first —
  a reload exports [cursor] before [ui]; a drain send that finds the bus busy again owes
  itself back). The runtime done never touches READY: `on_environment_exported` alone drives
  send_ready, then `retry_owed_env_exports` (compositor_impl.cpp:3937, 4778-4785) drains.
  `end_session` calls `drop()` (:3970) so a reply landing inside `unset_now`'s flush cannot
  re-set what teardown is unsetting.
- Intentional behavior change (documented, not accidental): with [ui].theme empty the export
  always happens, so a hand-set LUMEN_THEME in the user manager is overwritten while the
  compositor runs and **deleted at session end** (teardown unsets every recorded name,
  user_session.cpp:281-282) — the config layer is the source of truth per ADR-013; stated
  plainly in the new ADR text (docs/adr/013-settings-app.md:600-609, *ran*: read; §4.7 at
  :272-279 likewise).
- Deviations beyond the card's one-file scope (each a direct consequence): the pure helpers
  live in `:config_apply` (whose documented job is config-term conversion — the only
  unit-testable home), the test + CMake registration, the deploy config comment (old text
  contradicted the mandated always-export), and `export_cursor_environment` gained the same
  owed-deferral — without it a reload changing [ui] and [cursor] together dropped the cursor
  export on the one-at-a-time bus (it also fixes the pre-existing cursor-reload-racing-startup
  drop). No-degradation, not scope creep.

**Evidence** (*ran* = this session, at head e5a1175):

- `./build/test/test_theme_env -v` → **8 tests, 0 failed** (resolution table; configured
  name/path passthrough; cfg_ui bits; owed-reexport cursor-first drain; defer-again
  re-owe not lost; teardown drop; changed-value gate; refused-export-not-carried).
  `LUMEN_BUILD_TESTS:BOOL=ON` in build/test/CMakeCache.txt.
- `ctest --test-dir build/test --output-on-failure -LE bench` → **"100% tests passed out of
  135", 76.96 s** (134 before + this test).
- *Not run*: acceptance criteria 2–4 (the seat checks) — never executed by the task either;
  this is the gate's blocker, not a failure.
- *Packet/gate-reported, not re-run here*: drag-smoke exit 1 and app-smoke exit 1 with the
  line-2 WARN "another session is running (WAYLAND_DISPLAY=lumen-0)" (the documented
  all-FAIL condition, [[lumen-app-smoke-known-noise]]); every keyboard-driven row passed
  (V1, O3-O5, O7-O10, N7, N8, Q6, Q7) while every click-dependent row failed and the
  LUMEN_LOG=debug compositor log has **zero button/grab lines**; pointer motion demonstrably
  arrived (357 cursor presents, hover latency n=150/150 at 13.9 ms median); and
  `systemctl --user show-environment` currently carries no LUMEN_THEME — consistent with the
  smoke compositors' clean teardown-unset.

**Fuckups and their fixes** (all four review findings confirmed, 0 refuted — noise line
raised 4 / confirmed 4 / refuted 0 / preexisting 0):

- **Refused export counted as carried (655a586):** the startup path recorded the exported
  value *before* the export's outcome was known, so after a refused/timed-out startup export
  a later reload resolving to that same value was skipped as unchanged — the user manager
  never received LUMEN_THEME at all. Fix: `theme_forget()` at all three failure points —
  outright startup refusal (compositor_impl.cpp:3911), startup reply !ok (:3934, before
  send_ready and retry), runtime reply !ok (:4765). General lesson: **track an environment
  export by outcome, not intent** — a value remembered before its reply turns one refusal
  into a permanently missed export.
- **One bus, two exports (owed-deferral):** user_environment carries one export at a time;
  the naive re-export would drop the cursor export whenever a reload changed [ui]+[cursor]
  together (error-logged, lost). Fix: both re-exports share the owe/drain deferral; the
  runtime done never re-enters READY.
- **Untestable choreography (6ba8fdf):** the owe/drain/gate bookkeeping lived as raw flags
  in compositor_impl with no test able to drive it (grep: no test touches user_environment).
  Fix: behavior-preserving move into pure `env_reexports` (testable, 5 new cases).
- **Stale ADR (e5a1175):** ADR-013 still said the compositor exports "a set [ui].theme" —
  rewritten to the always-export/re-export/teardown-deletes contract (AGENTS.md §9).

**Gate verdict BLOCKED (seat stage invalid, must be redone — not a measured regression):**
acceptance criteria 2–4 (run-desktop appearance flip → `systemctl --user show-environment`
shows LUMEN_THEME=one-light; lumen-hello relaunch light then dark; startup export with empty
ui.theme; lumen-settings fresh start after the flip) were never run, and the two smokes that
did run are incomparable evidence, not a red to A/B: injection was broken. **Host finding
(ran*, still true at write time):** the uinput mouse has been held open since **Nov 15
12:22** — root process pid 2692863 `python3 scripts/uinput-mouse.py --output 3200x1800`
(under sudo setsid 2692860; /proc cwd unreadable, root; the gate attributed it via its own
pgrep to the smoke-bench-harness-green worktree) — violating lumen-build-run's
start-from-this-tree rule — and its node event16 "Lumen Inject Mouse" reads key caps
`70000 0 0 0 0`: **no BTN codes registered**, so button events physically cannot reach any
smoke. pgrep trap: `pgrep -af "uinput\|inject"` matches nothing (pgrep is ERE; `\|` is
literal) — use `pgrep -f "uinput-mouse|inject"`.

**Unrecorded seat finding surfaced (unattributed, TODO-tracked):** lumen-shell from this
worktree's build/test **segfaulted twice** during app-smoke — coredumpctl (*ran*) pids
3499403 @21:41:39 and 3521729 @21:53:11, SIGSEGV, EXE = this worktree's lumen-shell;
gate-reported identical stacks `lx::shell::notification_view::build_ui → lx::ui::image →
lx::runtime::memory_arena::allocate`. The diff touches no lx.ui/lx.shell code, so not
confirmed introduced; the last known-green notebook app-smoke (177/0/0, Nov 15, commit
fd348e7) predates the user's current `appearance = "light"` config, so not confirmed
pre-existing either. Per §1.2 the A/B attribution goes to the user before this can gate.

**Disproven / do not re-chase:** "the seat reds are a regression of this change" — every
click-row failure traces to the buttonless stale holder (keyboard rows all passed; motion
arrived; zero button events in the debug log). "The crash is introduced by the diff" —
untouched code, but unattributed is not the same as pre-existing. "A refused export counts
as carried" — disproven as a remaining defect by 655a586. "With [ui].theme set the export
changes" — it stays byte-identical (the name/path as written); the card's startup-correctness
criterion holds by construction.

**Why:** session-environment exports are one-shot on a one-at-a-time bus, so "did the value
actually reach the manager" must be tracked by *outcome*, not intent — and a compositor that
resolves config terms must export the resolved value, not the raw field, or half the
configuration (here: appearance) silently never reaches the apps that read only the env.
The gate failure is the other half: a seat smoke is evidence only if the injection
environment is part of the run's provenance — a stale buttonless holder manufactures a wall
of click-FAILs that no diff can own, and an unattributed crash cannot gate.

**How to apply:** redo the seat stage first: kill the stale holder (`sudo pkill -f
uinput-mouse` — pids 2692860/2692863), regenerate the node **from this tree**
(`scripts/uinput-mouse.py --output 3200x1800`, note the new event node, `LUMEN_SMOKE_SCALE=1`),
confirm the new node's key caps include button bits (a word ≥ 0x110/272), and confirm an
unoccupied seat (`pgrep -f "uinput-mouse|smoke"`). Then run acceptance 2–4 as written:
run-desktop session → set `[ui] appearance = "light"` via lumen-settings (writes
~/.config/lumen/config.toml) → `systemctl --user show-environment` shows
`LUMEN_THEME=one-light` → launch lumen-hello (comes up light) → flip to dark → relaunch
(dark); with `ui.theme` explicitly set the export is that name/path verbatim; lumen-settings
starts fresh after the flip. Attribute the crash: run app-smoke against a **main baseline
binary** in the same serialized session and compare coredump stacks (`coredumpctl info
3499403 3521729`). When adding exports beside the cursor variables, reuse `env_reexports` —
never add a second one-at-a-time user without owing/drain, and never count a value as
carried before its reply said ok.

## 2026-11-17 seat redo — gate debt closed on merged main (branch merged as eaaf7ba; every number below tester-run, analyst-recounted)

- **Seat gate closed** (evidence `evidence/2026-11-17-seat-redo/`, serialized quiet host,
  notebook): drag-smoke **10/10 PASS** (g: cursor median 9.0 ms p95 18.8, window-move
  median 11.3 p95 18.1); app-smoke **177/0/0**; ctest at merged main 9022e74 **136/136**
  (77.3 s). Acceptance 2-4 run as written incl. the letter — appearance flipped **through
  the lumen-settings UI** (injected clicks: app wrote exactly the `appearance` line,
  compositor re-exported `LUMEN_THEME=one-dark` then `one-light`, app re-themed live
  0.179/0.968); startup export with empty ui.theme → one-light; verbatim `ui.theme="one-dark"`
  export; lumen-hello relaunch light 0.9189 / dark 0.2061; lumen-settings fresh start;
  teardown unset; config byte-restored (sha256 082ce0e5…). A supplementary write-and-rename
  pair had already re-verified the same export machine both directions — the export fires
  identically regardless of who wrote the file.
- **The SIGSEGV did NOT reproduce** — zero coredumps across both smokes (incl. N1–N8
  notification-card draws) and two interactive sessions on merged main. Non-reproduction is
  not attribution: split to its own TODO row (intermittent, ×2 within 12 min on Nov 16,
  never before/since; next = ASan lumen-shell + notify-send/shell-relaunch stress). Original
  stacks preserved at `a-smokes/coredump-originals-Nov16.txt`.
- **The "no BTN codes" poisoning mechanism in the gate section above was a bitmap
  misreading** — `/proc/bus/input/devices` prints 64-bit words MSW-first, so `70000 0 0 0 0`
  IS BTN_MOUSE/RIGHT/MIDDLE; see [[lumen-uinput-key-bitmap-format]]. The Nov-15/16
  click-loss wall is best attributed to the concurrent-session WARN family. The stale-holder
  kill and start-from-this-tree discipline were still correct and necessary.
- Env notes: the export is gated on the compositor's `--session` flag — direct-mode
  run-desktop exports nothing by design (run1 evidence); a pre-existing failed
  xdg-desktop-portal-wlr.service preceded the redo session (cleared by its portal restart,
  not this change).

Related: [[lumen-portal-appearance-settings]] (the D-Bus side of the same ADR-013 appearance
surface; its plan's part B is this card), [[lumen-app-smoke-known-noise]] (concurrent-session
WARN = every scenario FAIL), [[lumen-smoke-bench-harness-green]] (holder autodetect +
start-from-this-tree rule), [[lumen-notebook-full-green-audit]] (the 177/0/0 green the crash
contradicts), [[lumen-notebook-hw]] (this host), [[lumen-no-degradation-rule]] (the
cursor-export drop that widened the diff).
