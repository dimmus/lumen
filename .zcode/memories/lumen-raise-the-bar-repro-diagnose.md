---
name: lumen-raise-the-bar-repro-diagnose
description: "2026-11-13 notebook seat diagnosis (main 0394e7c, evidence-only branch 3b3a73e..16f5d4d, 7 seat runs): the owner's black-screen/flickering-buttons startup is [panel] blur=true crash-looping lumen-shell — get_background_effect sent on the raw connection carrying a ui_host wl_surface id ('unknown object (34)' ×6, 5 restarts); animations alone start and animate byte-clean (3 seeded causes refuted); live blur toggle = logged no-op; live-enable 'screen damage' NOT reproduced; gate NOT_READY (acceptance 1 = 2 of 3)"
metadata:
  node_type: memory
  type: project
summary: "[panel] blur=true at start crash-loops lumen-shell (cross-connection get_background_effect) and IS the black-screen symptom; animations alone byte-clean; 3 seeded causes refuted"
modules: lx.shell
topics: blur, crash, diagnosis

---

Verified 2026-11-13 on the **notebook** (card0 nouveau, card1 i915 — drivers re-checked
this session; hasvk Vulkan, eDP 3200×1800@2x) against main **0394e7c**, release build
rebuilt to HEAD in the main checkout (DIAGNOSIS.md:5-7). Read-only diagnosis: no source
was written; branch
`zcode/task/raise-the-bar-repro-diagnose` (worktree
`/home/dimmus/lumen-wt/raise-the-bar-repro-diagnose`) holds only `evidence/` — 194 files in
seven commits (3b3a73e probe helpers + config-integrity records, db23b8e per-run debug
logs, bfb9ae9 PNG probes + `DIAGNOSIS.md`; review fixes 95fafdb, d2c0066, 2a54bc2,
16f5d4d). Gate verdict **NOT_READY** (below). Every log and source anchor in this record
was re-read the same day by the memory writer unless marked packet-sourced.

**Symptom 3 — `[panel] blur=true` never blurs; at session start it kills the shell (root-caused):**
- run4-blur-start: lumen-shell exits 1 on a protocol error every start — shell.log
  `wl_display#1: error 1: invalid arguments for ext_background_effect_manager_v1#20.get_background_effect`
  ×6, `[run-desktop] lumen-shell exited (1); restarting it` ×5, then
  `keeps failing; not restarted again` (shell.log:145); compositor.log
  `unknown object (34), message get_background_effect(no)` ×6 (first at :74), each followed
  by `error in client communication`. The desktop ends with no panel and no background
  (settled probe: topbar mean 0.0, 25.8 % non-black = the foot window on black;
  DIAGNOSIS.md:64-70 — figures recomputed from the committed PNGs by the review).
- Cause = a **cross-connection object argument**. `shell_app::run` (lx.shell.cppm:2329)
  opens two connections:
  the raw `wl_display_connect` (`d`, whose registry binds both zlm_shell_v1
  (lx.shell.cppm:1114) and the blur manager (:1145-1148) — :2404, :2433) and the
  lx.wayland.client connection `client_`/`ui_host_` run on (:2415, :2423). `panel::create`
  then sends `ext_background_effect_manager_v1_get_background_effect(manager,
  surface_.wl().native())` (panel.cppm:2011-2013) with the **ui_host** connection's
  wl_surface — the request leaves on `d` carrying an id that exists only on the other
  connection, and libwayland-server kills the client. Same on every shell restart and (by
  reading) on any later per-output panel creation with blur enabled.
- Code reading only (not seat-observed — the client dies at the first request): the blur
  region is created on the ui connection too (panel.cppm:2014-2017), so a fix limited to
  the surface argument would hit the same error one request later at `set_blur_region`.
- Live toggle (run3-blur-live) is a logged no-op: shell.log:52
  `[WARN] shell.panel: panel blur changed after create: it applies to the next panel`
  (panel.cppm:1962); probes differ by 171 of 5 760 000 px (the clock bbox), topbar mean
  214.6 → 214.6. `apply_config` only flips `panel_blur_` and calls `update_panel_blur()`
  (lx.shell.cppm:2996-2998 → :527-530); the once-per-create `so.opaque = !blur`
  (panel.cppm:1980) is never re-made, and nothing re-creates a panel (created per output,
  :1499).
- The hasvk blur pipeline has never run on this seat: no `surface blur region` /
  `backdrop blur pipeline creation failed` line in any run; the compositor advertises blur
  at bind on `supports_blur() = ready() && !blur_failed_` (vk_renderer.cppm:3815-3817),
  before the lazy pipeline exists.

**Symptom 2 — black screen + flickering buttons at session start = symptom 3b:**
- Reproduced only with animations AND blur on (run5-both-start, the owner's combined
  config): startup probes 1-8 black (mean 0.00), settled = lone window on black, the same
  six-cycle crash loop (shell.log 6 `invalid arguments`, compositor.log 6
  `unknown object (34)`). The flicker is the panel appearing and dying with each of the 5
  restarts; animations are bystanders.
- Animations alone (run2-anim-start) A/B'd against off (run2b-anim-off-start): both show
  the same two black startup frames and full content by probe 3-4 (mean 68.4/68.45) — the
  ~0.3 s transient is the compositor-first-frame → shell-first-paint gap; the foot-window
  fade ran at output rate and settled (DIAGNOSIS.md:116-125).

**Symptom 1 — "visible screen damage" on live-enable of `[animations] enabled=true`: NOT
reproduced** (acceptance 1 delivered 2 of 3; stated up front by 95fafdb):
- Three log-verified enable windows (run1 generations 2 and 4, run1b generation 2; the
  `config: generation N` line is compositor_impl.cpp:4696) across three window states
  (at tiles, resized off-tile, dragged and re-tiled): 0 px beyond the clock (153-171 px);
  renderer damage stays `area=204800` (the panel strip). Post-enable open/close/
  workspace-slide/reflow all settled byte-clean (`ws-roundtrip` vs `ws-base` 0 px;
  `close-settled` vs `base-1` 171 px) (DIAGNOSIS.md:145-210).
- Not probed: a live enable on a blur=true session (run4/run5 logs hold one
  `config: generation` line each) — the state the owner was most plausibly in; DIAGNOSIS.md
  :205-210 offers that reading without claiming it.

Gate NOT_READY, reasons as recorded in the packet (re-checked where marked): symptom 1
unreproduced and its root cause unpinned; run1's first driver session lost its logs
(fuckup 1); the quiet-host precheck ran but its output was never captured
(`grep -rl loadavg evidence/` matches only `seat-precheck.sh` and DIAGNOSIS.md's own
mention of it; 16f5d4d says this in place); two `[WARN] compositor.memory: critical pressure — evicting
caches` lines (run1-anim-live/compositor.log:452, run1b-anim-motion/compositor.log:1729)
go unmentioned in DIAGNOSIS.md (designed eviction under host PSI, per
[[lumen-app-smoke-known-noise]]:16); app-smoke exit 1 was not certified row-by-row —
my recount `grep -c '^PASS'` = 140, `'^FAIL'` = 36 (packet said 141/35), VERDICT: FAIL at
app-smoke-logs/run.log:191, the last FAIL row itself citing the pressure WARN — the
documented notebook envelope; drag-smoke VERDICT: PASS (drag-smoke-logs/run.log:26);
worktree suite 133 tests, failures exactly {17 ui_gallery, 81 test_task_model}
(build/test LastTestsFailed.log + 133 CTestCostData rows) — the tolerated known-red; and
nothing in the tree could regress: `git diff 0394e7c..HEAD --name-only | grep -v
'^evidence/'` is empty (194 files, all evidence). Clean-state invariants re-verified:
config.toml sha256 `3c70e17e…febc6` before = final = now (`cmp` clean vs
`/tmp/lumen-config-ORIGINAL-backup.toml`); compositor down (`grep -l lumen-composito
/proc/[0-9]*/comm` exit 1); main checkout clean at 0394e7c.

Fuckups and fixes (review round: 4 confirmed / 0 refuted; fixes touch evidence only):
(1) `scripts/run-desktop.sh:184` truncates compositor/shell/client/session logs at every
session start (`: >"$comp_log"; …`) — run1-anim-live hosted two driver sessions
(driver.log 19:34, driver2.log 19:46, by mtime) in one log dir, so the first session's logs were
destroyed; only its `probe-before-*` shots survive and "four live-enables" had to be
corrected to three with the truncation explained (2a54bc2). (2) The report's index named
`.ppm` probes while only byte-verified lossless PNGs are committed (verify-png.sh decodes
each PNG and compares it byte-for-byte to its PPM payload → png-verify-report.txt;
evidence/ holds 0 ppm / 118 png now) and five ephemeral `launcher.pid` files had been
committed (d2c0066). (3) The unmet acceptance item was not stated up front (95fafdb).
(4) The seat-discipline claim read as artifact-backed when it was not (16f5d4d). Packet
field error: `implementation.commits` said `[]` although the seven evidence commits exist.

Host findings (re-checked this session): `scripts/inject-input.py` defaults are the VM's —
`--pointer /dev/input/event2`, `--keyboard /dev/input/event1` (inject-input.py:527-530);
on the notebook event1 is the **Power Button**, event3 the AT Translated Set 2 keyboard,
event16 the "Lumen Inject Mouse" uinput node (`/proc/bus/input/devices`, read live) — pass
`--keyboard /dev/input/event3 --pointer /dev/input/event16 --output 3200x1800`
explicitly. (The build-run skill's "event1 = AT keyboard" line, SKILL.md:24, is the VM
mapping.) Probes were `grim -t ppm` over ext-image-copy-capture, diffed per pixel at
threshold 8 (ppmdiff.py/ppmstats.py); full-screen diff counts are out of 5 760 000 px.

Disproven theories (this seat, states probed — do not re-derive):
- "Startup black/flicker is an animation bug": all three seeded mechanisms were clean —
  present-clock sampling of the predicted vblank advanced every tick and motions ran at
  output rate (compositor_impl.cpp:10460-10462), opacity never stuck near 0 while
  `active()` held the frame clock (window_animation.cppm:118,
  modules/lx.compositor/window_animation.cpp:188 `alpha.launch(-1.0, 0.0)`), ghost
  hold/defer_retire retired cleanly (compositor_impl.cpp:4666-4673, wire_buffer.cpp:66-95,
  per DIAGNOSIS.md:135-137). The A/B without animations is visually identical.
- "Blur fails on capabilities-vs-create ordering" (seeded b) — `blur_offered_` was true
  and the manager non-null; the create reached the get_background_effect call.
- "Blur fails in the hasvk lazy pipeline" (seeded c) — refuted **for this symptom only**;
  the pipeline itself remains untested on this host.
- "`set_tiling_config`'s unconditional `tiling_dirty_` flush damages at toggle time" —
  true as code (window_manager.cppm:5939 `set_tiling_config`; animations ride cfg_windows,
  config_apply.cpp:74-76) but `aim_tile` skips when geometry already matches
  (:6016, :6041); damage after every probed generation stayed ~200k px.
- "First-pose damage ordering in `window_animator::tick`" (window_animation.cpp:398-411) —
  no artifact in any captured motion.

**Why:** a client holding two Wayland connections can hand one connection's object to a
proxy on the other and nothing in the C API stops it — the server kills the client, and a
crash-looping shell reads on the seat as a black, flickering desktop that looks like a
compositor or animation bug. Reproduce the user's *combined* config before chasing
per-feature hypotheses; and evidence survives only if every session gets its own log
directory.

**How to apply:** the fix task (not done here): bind
`ext_background_effect_manager_v1` on the connection that owns the panel's wl_surface (or
create surface, region and effect on one display — check `set_blur_region`'s region
argument in the same change); make a live `[panel] blur` change re-create the panel (the
key is live in the schema per the task card, lx.config/schema.cppm:178-181); then prove
blur on the seat — it will be the hasvk pipeline's first real frame, so watch for
`backdrop blur pipeline creation failed` and the discarded `ensure_blur_levels` result
(vk_renderer.cppm:3837, per the card). Re-test symptom 1 only after that fix, with a live
enable on a blur=true session. For any future seat evidence run: one LOG_DIR per session
(never relaunch `run-desktop.sh` into a dir whose logs you still need), tee
`seat-precheck.sh` into the run dir, grep the run's logs for `critical pressure` and
report it, and commit exactly the artifacts the report names. Recognize this crash on
sight: shell `invalid arguments for <manager>.<request>` + compositor
`unknown object (N), message <request>` for an id the client never bound on that
connection.

Related: [[lumen-raise-the-bar-run]] (the run that shipped F2 spring animations + F3
blur), [[lumen-notebook-hw]], [[lumen-app-smoke-known-noise]],
[[lumen-host-build-run-env]], [[lumen-zcode-agent-harness]],
[[lumen-no-degradation-rule]].
