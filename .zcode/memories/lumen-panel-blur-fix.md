---
name: lumen-panel-blur-fix
description: "2026-11-13/14 notebook seat fix (worktree panel-blur-fix, b8d9277..0fee8ab): the [panel] blur crash loop fixed at both roots — ext_background_effect_manager_v1 binds on the views' (ui_host) connection via a pre-connect global_handler, and a live blur change rebuilds the panels; first-ever hasvk blur pixels proven (stripes std 127.5→0.00 inside the topbar, live toggle 180 208 px bounded exactly to the strip, 0 restarts); drag-smoke PASS, app-smoke 144/145 vs baseline 138 (28 rows fail in all three = host noise), ctest 131/133; gate BLOCKED ×2 (stale-binary sweep exit 2; final-HEAD re-runs left uncommitted in gitignored logs without prechecks) then READY round 3 — 0fee8ab committed the gate-time smokes under evidence/smokes/gate/ with per-session prechecks (drag PASS 01:12, app-smoke 141 PASS ≥140, Z portal family flipped), 2026-11-14"
metadata:
  node_type: memory
  type: project
summary: "Blur manager rebinds per-connection via pre-connect global_handler; live blur rebuilds panels; first hasvk blur pixels; gates BLOCKED ×2 then closed by committed gate-time smokes"
modules: lx.shell
topics: blur, panel

---

Verified 2026-11-13/14 on the **notebook** (card0 nouveau, card1 i915, hasvk Vulkan,
eDP 3200×1800 @2x logical 1600×900 — the seat that diagnosed the crash loop,
[[lumen-raise-the-bar-repro-diagnose]]). Worktree
`/home/dimmus/lumen-wt/panel-blur-fix`, branch `zcode/task/panel-blur-fix` on main
**b8d9277**; commits **e77499a** (the fix) + **25d9bb4** (seat evidence) + review fixes
**cd6a576**, **766bd4a**, **73564e6** + gate evidence **0fee8ab**; diff confined to
modules/lx.shell (lx.shell.cppm +123/−, panel.cppm 19 lines), tests/test_shell_bridge.cpp
(+117) and evidence/ (375 files at 0fee8ab) — confinement re-checked with
`git diff --name-only b8d9277..HEAD | grep -v '^evidence/\|^tests/\|^modules/lx.shell/'`
→ empty. Gate verdict **BLOCKED** twice (below) then **READY** (third round, 2026-11-14).
Anchors re-read and numbers re-run by the memory writer the day after unless marked
packet-sourced.

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

**Update 2026-11-14 (second gate round — BLOCKED again; every number below re-run this
date):** the remediation was half-done. The worktree *was* rebuilt (lumen-shell binary
23:51:59, lumen-compositor relinked 00:26:48 — `find modules -type f -newer
build/test/lumen-compositor` empty, so the mtime sweep passes now) and both smokes
**did** re-run against final HEAD 73564e6 — the first seat runs against final HEAD —
but the results stayed in the gitignored log dirs (`/drag-smoke-logs/` `.gitignore:16`,
`/app-smoke-logs/` `:18`; proven with `git check-ignore -v`), were never committed under
evidence/, and neither session captured a precheck (`find … -iname '*precheck*'` →
empty; the committed `evidence/final-precheck.txt` is 23:15:01 — it belongs to the
22:33-era sessions, not these).
- drag-smoke `drag-smoke-logs/run.log` (mtime 00:27:08): rows a–j all PASS,
  `VERDICT: PASS (warnings: g latency)` — exactly the final-HEAD PASS the gate needed,
  uncaptured (log header also carries a `WARN: another session is running
  (WAYLAND_DISPLAY='lumen-0')` preflight note; it ran anyway and passed).
- app-smoke `app-smoke-logs/run.log` (00:48:26): **141 `^PASS` / 35 `^FAIL`** rows
  (≥140 envelope met), `VERDICT: FAIL`, run-row first error the `critical pressure`
  WARN — found exactly once in **all four** app-smoke logs including the committed
  baseline. The delta vs the committed 144-PASS change run closes exactly: Z2–Z5 newly
  FAIL (the interactive portal rows returned `response None` — no chooser, no stream;
  Z1/Z6/Z7 fail in both runs) and N8 PASS (the documented flake): 144−4+1 = 141.
  Z1–Z7 = the portal-frontend environmental family
  (.zcode/skills/lumen-decisions/SKILL.md:91).
- Gate verdict #2 **BLOCKED** on exactly this: the packet's `gate_runs.seat` still
  cites the 22:33–23:13 pre-cd6a576 runs and cites nothing from 00:27/00:48. The
  unblock is unchanged — commit the gate-time logs under evidence/ **with per-session
  prechecks** (or re-run under the precheck discipline), point `gate_runs.seat` at the
  final-HEAD runs, re-gate. Nothing else needs redoing.
- Clean-state re-verified 2026-11-14: config.toml sha256
  `3c70e17e0c43ad517b91fb120fb6dae11d2cc862a63104ae8017b055470febc6`, no lumen
  processes running, worktree clean at 73564e6.

**Update 2026-11-14 (third gate round — READY; task closed):** the round-2 unblock was
executed exactly as named. **0fee8ab** (2026-11-14 01:33:50 +0500, "evidence: gate-time
seat smokes against final HEAD") committed the runs under `evidence/smokes/gate/` —
`drag-smoke-logs/` + `app-smoke-logs/` (run.log, compositor.log, inject.jsonl,
per-scenario artifacts) and per-session prechecks (`precheck-drag-smoke.txt`,
`precheck-app-smoke.txt`); worktree clean at 0fee8ab. Numbers below re-run by the
memory writer from the committed files unless marked gate-sourced:
- drag-smoke at 01:12:11 (precheck loadavg 0.90/0.33/0.24, "ok: no
  lumen-compositor"): **VERDICT: PASS** — the first PASS seat run against final HEAD
  that is also committed evidence.
- app-smoke: **141 `^PASS` / 35 `^FAIL`** (≥140 envelope met; VERDICT: FAIL expected —
  the smoke config runs blur=false, so the changed path is inert there). N8 **PASS**;
  the FAIL set = the 25 known-noise rows + **Z1–Z7**, of which Z2–Z5 returned
  `response None` — the portal-frontend environmental family
  ([[lumen-app-smoke-known-noise]]) that flips between runs of the same binaries.
- Precheck-wrapper discrepancy, disclosed and disproven: the gate-time prechecks came
  from a wrapper that prints `WARN: a smoke is already running` — a line the committed
  `evidence/seat-precheck.sh` does not contain (grepped; zero matches). Contamination
  is excluded: the 01:23:10 precheck reads loadavg **0.25/0.25/0.25**, and the inject
  `t_us` spans are strictly serialized — drag 115054.65–115068.61 s vs app
  115713.12–116343.80 s, **644 s apart** (re-derived this session from the committed
  inject.jsonl).
- Gate verdict **READY**: all six acceptance items independently verified (gate
  decoded the PNGs itself — 180,253 toggle px / 912-px startup flash vs the README's
  threshold-counted 180,208 / 582, same substance; re-ran `test_shell_bridge` 105/0;
  parsed all four app-smoke logs row-level; re-hashed config.toml; checked no
  compositor running). Host discipline re-verified by the memory writer on the main
  host: config.toml sha256 `3c70e17e…febc6` (byte-identical restore), no lumen
  processes running.

Disproven theories (closed; do not re-chase): capability ordering (blur_offered_ true
at create) and hasvk lazy-pipeline failure — the card refuted both before the fix, and
the pipeline has now actually rendered blurred pixels. "N8 +1 = regression from the
bind" — one-shot at connect, and the smoke's config has blur=false; flake per the
rerun rule. "The gate-time smokes were contaminated by a concurrent session" — the
WARN-wrapper line aside, loadavg 0.25 and the 644 s inject-span gap disprove overlap.

**Why:** a Wayland object id is only meaningful on the connection that owns it — any
manager a view needs must be bound on the view's connection (a per-connection
`global_handler` installed before `connect()` is the pattern). And a smoke gate that
refuses to run stale is right to refuse, but it means **any commit that touches a
header's mtime invalidates the seat evidence** — the gate-time runs are the ones that
count, not the ones that went PASS earlier the same evening, and they only count once
they are committed with their prechecks: an uncommitted rerun rotting in a gitignored
`*-logs/` dir is invisible to `git status` and is not evidence.

**How to apply:** for the next view-side protocol global, copy the
`client_globals_`/`set_global_handler` pattern (lx.shell.cppm:2474) — never bind a
view's manager on the raw zlm_shell_v1 connection; when a rendered choice is made once
at create (opaque vs blurred), a config change means destroy+re-create, as
`rebuild_panels()` does — mirror `rebuild_backgrounds()`. Before any gate-time smoke,
`cmake --build build/test -j2` even if you believe your change cannot affect the
binary under test — the sweep is mtime-based over modules/; expect exit 2 with the
"older than" line as the tell. Finish the rerun in the same breath: tee
`seat-precheck.sh` per session, copy `drag-smoke-logs/` + `app-smoke-logs/run.log` into
evidence/ and commit (the dirs are gitignored — check with `git status --ignored`), and
make the packet's `gate_runs.seat` name those runs; a gate must hold on any seat claim
older than the last source commit. Judge app-smoke deltas only after reading the baseline's
own captured precheck for load, and attribute rows with a full-scenario-row parse
(`^(PASS|FAIL)\s+(\S+)`), not a first-token grep. Recognize the crash loop on sight:
shell `invalid arguments for <manager>.<request>` + compositor `unknown object (N)` for
an id bound on the client's other connection.

Related: [[lumen-raise-the-bar-repro-diagnose]] (the diagnosis and root causes),
[[lumen-raise-the-bar-run]] (shipped F3 blur), [[lumen-app-smoke-known-noise]],
[[lumen-notebook-hw]], [[lumen-toplevel-drag-compositor]] (same exit-2 gate trap,
different costume: wrong uinput node then, stale binary now),
[[lumen-no-degradation-rule]].
