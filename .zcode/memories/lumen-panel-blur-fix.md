---
name: lumen-panel-blur-fix
description: "2026-11-13 notebook seat fix (worktree panel-blur-fix, b8d9277..73564e6): the [panel] blur crash loop fixed at both roots — ext_background_effect_manager_v1 now binds on the views' (ui_host) connection via a global_handler installed before client_.connect(), and a live blur change rebuilds the panels; first-ever hasvk blur pixels proven (stripes std 127.5→0.00 inside the topbar vs ~35.7 for plain alpha, live toggle 180 208 px bounded exactly to the strip, 0 restarts); drag-smoke PASS, app-smoke 144/145 vs baseline 138 (31 rows fail in both runs, 28 in all three = host noise), ctest 131/133; gate BLOCKED — the 23:57 gate-time smokes exit 2 on the stale-binary sweep (lumen-compositor 22:33 vs cd6a576's 23:49 source) and never ran against final HEAD"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-13/14 on the **notebook** (card0 nouveau, card1 i915, hasvk Vulkan,
eDP 3200×1800 @2x logical 1600×900 — the seat that diagnosed the crash loop,
[[lumen-raise-the-bar-repro-diagnose]]). Worktree
`/home/dimmus/lumen-wt/panel-blur-fix`, branch `zcode/task/panel-blur-fix` on main
**b8d9277**; commits **e77499a** (the fix) + **25d9bb4** (seat evidence) + review fixes
**cd6a576**, **766bd4a**, **73564e6**; diff confined to modules/lx.shell (lx.shell.cppm
+123/−, panel.cppm 19 lines), tests/test_shell_bridge.cpp (+117) and evidence/ (97
files, +7046/−45 — confinement re-checked with
`git diff --name-only b8d9277..HEAD | grep -v '^evidence/\|^tests/\|^modules/lx.shell/'`
→ empty). Gate verdict **BLOCKED** (below). Anchors re-read and numbers re-run by the
memory writer the day after unless marked packet-sourced.

**Fix 1 — the cross-connection marshal (the crash loop):**
- `ext_background_effect_manager_v1` is now bound on the **views' connection** (the
  lx.wayland.client/ui_host connection the panel surfaces live on), through that
  connection's `global_handler` extension point, installed **before**
  `client_.connect()` so the bind rides the connect's registry roundtrips
  (lx.shell.cppm:2474-2475 `client_.set_global_handler(&client_globals_)` then
  `client_.connect(...)`; bind + capabilities listener at :1178-1198 — the listener
  calls `rebuild_panels()` because capabilities may arrive after the configuration).
  `zlm_shell_v1` stays on the raw privileged connection, untouched.
- The old code bound the manager on the raw connection's registry while
  `get_background_effect` carried the ui_host surface id — an object id resolves only
  on the connection that owns it, so libwayland-server killed the raw connection
  ('unknown object (34)') and lumen-shell exit(1)-looped 6 times.

**Fix 2 — the live-toggle no-op:**
- `shell_app::apply_config` now calls **`rebuild_panels()`**
  (lx.shell.cppm:1546-1563, wired at :3061 and from the capabilities listener :1194):
  closes panel-parented popups (calendar/menu/tooltip), drops `pointer_panel_` /
  pending-menu state exactly like the closed-panel path, then destroys and re-creates
  each panel whose `blurred()` differs — the opaque/translucent buffer choice and the
  blur effect are made at `panel::create`, so re-creating is the only live path
  (`rebuild_backgrounds()` already did this for backgrounds).
- `panel::set_blur` — the "applies to the next panel" logged no-op (panel.cppm:1955-1963
  in the old tree) — is removed; `panel::blurred()` added. Called once per output, not
  per frame.

**Seat evidence (evidence/ in the worktree; README.md is the report):**
- Owner's combined config (animations on + blur on) at session start: **zero
  lumen-shell restarts**, no `invalid arguments` in shell.log, no `unknown object` in
  compositor.log, and `surface blur region … rects=1 area=51200` for every blur-on
  panel (re-grepped: 4 lines in run-blur-session/compositor.log). Startup probes 1-2
  black (the known compositor-first-frame → shell-first-paint gap), fully lit by probe
  4 — vs the broken run5's all-8-black.
- **Geometry finding:** the acceptance's "bright window under the topbar" is
  impossible in Lumen — a fullscreen window covers the panel and a moved window is
  constrained to the usable area (`apply_grab` →
  `constrain_on_output(usable_area(...))`), so the **background layer is the only
  content that can ever sit behind the topbar**; the probes put the bright content
  there. Documented in evidence/README.md.
- Live toggle both ways (magenta gradient behind): on = [243.0, 21.8, 243.0]
  (content through), off = [214.2, 214.2, 214.6] opaque (matches diagnosis run3's
  214.6), on-again **0 px differ**; on vs off = 180 208 px, bbox exactly x0-3199 y0-63
  — the topbar strip and nothing else. Panels destroyed/re-created per toggle
  (surfaces 2 → 8 → 9; blur regions on 2 and 9, none on 8).
- **The hasvk blur pipeline works — first frame it was ever reached.** 8-px black/white
  striped background behind the topbar: stripe std **127.5** below the strip vs
  **0.00** inside it with blur on; plain 0.72-alpha translucency would keep ~35.7 —
  the blur convolves. No `backdrop blur pipeline creation failed` anywhere.
- Smokes: drag-smoke **VERDICT: PASS** (g-latency warning = the known host family);
  app-smoke **144/145 PASS** (≥140 envelope cleared twice) vs a same-seat main-b8d9277
  baseline of **138**. Row attribution (corrected by 766bd4a; re-derived 2026-11-14):
  31 rows fail in both change runs, **28 in all three** — the documented VM-geometry +
  "no screenshot" noise families ([[lumen-app-smoke-known-noise]]); gap Z1/Z6/Z7 never
  ran in the baseline (its Z group aborted); N8 measured 3 once and passes on rerun
  (one-rerun flake rule); E/R5 failed only in the baseline, which ran **hot** — its own
  precheck records loadavg **4.12**/2.30/1.06 on this 2-core host (disclosed by
  73564e6; change runs 0.66/0.27/0.25 and 0.19/0.35/0.58). No degradation rests on the
  envelope + the 28 identical rows, not the +6/+7 margin.
- Suite: tests-enabled `ctest -LE bench` → **131/133**, failures exactly the
  owner-tolerated {ui_gallery, test_task_model}. `build/test/test_shell_bridge` →
  **105 checks, 0 failed** (re-run 2026-11-14), 12 new blur checks added in the fix
  commit itself, including the regression negative: `get_background_effect` through a
  manager bound on the *wrong* connection kills that connection — the crash loop, kept
  as a test — plus the bind-on-views-connection check and the live-toggle
  destroy/create mechanics.
- Host discipline: `~/.config/lumen/config.toml` sha256 `3c70e17e…febc6` restored
  byte-identical (re-hashed 2026-11-14), compositor down, `seat-precheck.sh` output
  captured per session (the diagnosis-gate lesson, applied), 17/17 probes committed as
  byte-verified lossless PNGs.

**Review round (3 confirmed / 0 refuted, all fixed):** cd6a576 guards the teardown
destroy — `if (background_effect_ && client_.connected())`
(lx.shell.cppm:2886-2888) — because on a `client_.connect()` failure after the manager
global was dispatched, `display::fail` already disconnected the display and the proxy
was dangling (UB in an exiting process; guard verified sound by construction, path
unreachable in tests — honest about it). 766bd4a fixes the evidence row-attribution
(the reviewer's own first-token parse had collapsed duplicate row prefixes). 73564e6
discloses the hot baseline verbatim from the captured prechecks.

**Gate BLOCKED (the one open item; source is done):** the gate-time seat runs (23:57)
never started a compositor — both `drag-smoke-logs/run.log` and `app-smoke-logs/run.log`
(121 bytes, re-read 2026-11-14) say `FAIL: build/test/lumen-compositor is older than
modules/lx.shell/lx.shell.cppm` and exit 2 = "could not run" per the script's own
contract (scripts/drag-smoke.sh:31). cd6a576 touched lx.shell.cppm (mtime 23:49) after
the last build; the smokes' conservative staleness sweep sweeps **all of modules/**,
even though lumen-compositor (22:33) does not link lx.shell. So no seat smoke has ever
run against final HEAD 73564e6 — the committed PASS evidence predates the last
source commit. Unblock without touching source: `cmake --build build/test -j2` in the
worktree, re-run both `--join-seat-session` smokes with the same precheck discipline,
commit the logs under evidence/, re-gate.

Disproven theories (closed; do not re-chase): capability ordering (blur_offered_ true
at create) and hasvk lazy-pipeline failure — the card refuted both before the fix, and
the pipeline has now actually rendered blurred pixels. "N8 +1 = regression from the
bind" — one-shot at connect, and the smoke's config has blur=false; flake per the
rerun rule.

**Why:** a Wayland object id is only meaningful on the connection that owns it — any
manager a view needs must be bound on the view's connection (a per-connection
`global_handler` installed before `connect()` is the pattern). And a smoke gate that
refuses to run stale is right to refuse, but it means **any commit that touches a
header's mtime invalidates the seat evidence** — the gate-time runs are the ones that
count, not the ones that went PASS earlier the same evening.

**How to apply:** for the next view-side protocol global, copy the
`client_globals_`/`set_global_handler` pattern (lx.shell.cppm:2474) — never bind a
view's manager on the raw zlm_shell_v1 connection; when a rendered choice is made once
at create (opaque vs blurred), a config change means destroy+re-create, as
`rebuild_panels()` does — mirror `rebuild_backgrounds()`. Before any gate-time smoke,
`cmake --build build/test -j2` even if you believe your change cannot affect the
binary under test — the sweep is mtime-based over modules/; expect exit 2 with the
"older than" line as the tell. Judge app-smoke deltas only after reading the baseline's
own captured precheck for load, and attribute rows with a full-scenario-row parse
(`^(PASS|FAIL)\s+(\S+)`), not a first-token grep. Recognize the crash loop on sight:
shell `invalid arguments for <manager>.<request>` + compositor `unknown object (N)` for
an id bound on the client's other connection.

Related: [[lumen-raise-the-bar-repro-diagnose]] (the diagnosis and root causes),
[[lumen-raise-the-bar-run]] (shipped F3 blur), [[lumen-app-smoke-known-noise]],
[[lumen-notebook-hw]], [[lumen-toplevel-drag-compositor]] (same exit-2 gate trap,
different costume: wrong uinput node then, stale binary now),
[[lumen-no-degradation-rule]].
