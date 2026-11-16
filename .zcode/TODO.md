# Lumen open work

Extracted 2026-11-02 from the archived agent memories (`.zcode/memories/`), the ADRs' own
status sections, and the working tree. The ADRs are authoritative for detail; keep this list
current when an item closes.

## Now

- [ ] **Commit/PR the ADR-013 settings-app tree** — Phases A–G implemented, 134/134 tests
  green (incl. `test_ui_inputs`, `test_settings_ui`, `test_config_edit`); the app presents on
  the automatic tier against a headless compositor; `lumen-ctl devices/outputs/version` answer
  live. Working tree is uncommitted.
- [ ] **ADR-013 open item 1: in-window key capture** for the bindings editor (wl_keyboard
  keysym recording; the text grammar is the complete fallback today).
- [ ] **ADR-013 open item 2: per-device override editor + window-rules UI** — both are
  schema-side rows in `lumen-settings` waiting on their own compositor feature (rule store
  behind the inert `zlm_window_rules_v1`; `[[input.device]]` override plumbing), not on the app.
- [ ] **Owed VM golden check (unit-pair-green)** — run `./build/test/lumen-ui-gallery --check`
  on the dev VM (tests-enabled build of the unit-pair-green tree, or main once it merges) at
  the next VM session and record the result; then check this row off. It is the open condition
  of the owner's 2026-11-15 acceptance of the golden substitution
  (`.zcode/memories/lumen-unit-pair-green.md`): cpu expected 0/255 in both themes, gl ≤ 3 vs
  CPU (G10); a VM failure there is a new finding about FreeType-version sensitivity, to be
  handled then — not a regression of the substitution.
- [ ] **portal-appearance-settings seat gate red is unattributed** (2026-11-16, gate
  NOT_READY at worktree HEAD fedbba1, branch unmerged): both seat smokes failed on the
  final binaries — drag-smoke 9/10 rows FAIL at +0,+0 (after a "another session is running"
  WARN) and app-smoke 101 FAIL rows incl. Z2/Z4 (the xdg-desktop-portal-lumen backend this
  diff touches answering but capturing black); no baseline smoke exists for the task, while
  notebook-full-green-audit was 177/0/0 on this host the day before. Re-run both smokes on
  a quiet serialized host and record green, or produce the baseline A/B naming a mechanism
  per FAIL row; also preserve the busctl ReadOne/SettingChanged and idle-strace probe
  artifacts the review asserted (none survived). See
  `.zcode/memories/lumen-portal-appearance-settings.md`.
- [ ] **compositor-theme-env-export seat gate BLOCKED — acceptance 2-4 never run; smokes
  invalid; lumen-shell SIGSEGV unattributed** (2026-11-16, worktree HEAD e5a1175, branch
  unmerged): the only seat runs were drag-smoke + app-smoke exit 1 under a broken injection
  environment — a stale root uinput holder open since 2026-11-15 12:22 (pid 2692863,
  `scripts/uinput-mouse.py --output 3200x1800`, started from the smoke-bench-harness-green
  worktree, violating the start-from-this-tree rule) whose node event16 "Lumen Inject
  Mouse" reads key caps `70000 0 0 0 0` (no BTN codes — clicks physically cannot reach the
  compositor), plus an "another session is running" WARN; every keyboard row passed, every
  click row failed, so the reds are environmental, but the four acceptance checks
  (appearance flip → `systemctl --user show-environment` LUMEN_THEME=one-light, lumen-hello
  relaunch light→dark, startup export with empty ui.theme, lumen-settings fresh start) were
  never executed. ALSO: lumen-shell from the worktree build segfaulted twice during
  app-smoke (coredumpctl 3499403 @21:41:39, 3521729 @21:53:11, identical stacks
  notification_view::build_ui → lx::ui::image → lx::runtime::memory_arena::allocate); the
  diff touches no lx.ui/lx.shell code and no baseline A/B exists (last green app-smoke
  177/0/0 on Nov 15, predates the user's appearance="light" config). Redo: kill the stale
  holder, regenerate the node from THIS tree, verify button caps in the key bitmap, run
  acceptance 2-4 as written, and attribute the crash against a main baseline. See
  `.zcode/memories/lumen-compositor-theme-env-export.md`.

## Host-limited gates (ADR-010; VM cannot close these)

- [ ] **Vulkan capture green** (replaces the portal task, owner decision 2026-11-14): make
  the Vulkan composite path serve what its GL/CPU siblings do, so the app-smoke rows that
  now WARN naming the gap turn into real PASSes — the SIGUSR1 debug read-back (GL path
  only, `compositor_impl.cpp` `write_screenshot`): S2 T4 T7 L2 L3 Q4 O2 O6 M1 N6 Y1 and
  Z1; window (toplevel) capture (refused in `wire_capture.cpp` `create_session`): Y2 Y5
  Q6, Z6, Z7; capture dma-buf buffers (GL-only, same file): Y4 `into dmabuf`. The harness
  half closed 2026-11-14 in `smoke-bench-harness-green` (host-aware geometry, injector
  pixel-exactness, REL-capable virtual mouse, the backend-limited WARNs and the A/B
  evidence under `evidence/2026-11-14-ui-bench-ab/`).
- [x] **Frame-callback / repaint-lead misses for fast-answering clients** (2026-11-03
  found, 2026-11-04 fixed): a wl_surface.frame-driven client answering in ~0.1 ms got
  46–53 fps with ~25 % of frames a period late, while ticks cost 0.4 ms and health said
  0 missed. Root cause was NOT the client-answer adaptation: (1) the aim/health pairing
  kept one slot, overwritten by the next tick before a slow frame's flip landed — the
  late frames were invisible to health and to any feedback; (2) the repaint lead had no
  whole-path feedback (render-thread wake + commit submission + kernel latch slack).
  Fixed by the four-entry aim ring in `output_ui` (health sees the misses again) and
  `repaint_scheduler::observe_commit_late` (a late flip feeds commit-vs-aim distance +
  step into the lead, windowed peak, converging in one miss). Verified: 6× pres-probe
  runs 59.81–59.89 fps, 1 late/200, sd 1.2 ms; vkmark fifo unchanged (60.00, sd 0.00);
  134/134 ctest; drag-smoke PASS (window-move median 16.8 ms, gate 18.7 — the honest
  lead costs ~4 ms against the broken-state 12.7, still ahead of weston/sway's 22/33).
- [x] **"Console fbcon not re-armed after a session exit on i915"** (2026-11-03 raised,
  2026-11-04 resolved — not a compositor bug): the restore was correct all along
  (`kms_pipeline::~kms_pipeline` SetCrtc of the saved console fb returns 0; the plane is
  back at fb 94 within ~2 s of logout, verified by parallel polling and a manual
  run-desktop session). The failing check was session-smoke's driver detection: it ran
  `modetest -M vmwgfx`, and the first two host-generic rewrites read the wrong sysfs
  node (the connector's `device` names the card, a drm_minor whose uevent has no DRIVER
  line; the PCI device is the card's `device`). Fixed with `drm_driver()` resolving
  `readlink -f $connector/device` + `/device/uevent`; session-smoke 24/24 PASS on
  hardware since.
- [ ] **G2 p95**: S2 301-node panel clock frame — p50 passes (95 µs), p95 490–540 µs vs 150 µs
  budget; biggest lever is reshaping the clock label. (ADR-010 §8 says host wake jitter.)
- [ ] **G7 cold on GL**: p95 135–156 ms vs 150 ms budget — driver cost (EGL + Mesa compiles +
  fontconfig ≈103 ms), Lumen ≈5 ms; no GL program binaries on SVGA3D; build-ahead measured
  and rejected.
- [ ] **G11 frame rule** (no frame > 2× median) — VM jitter; passed on a quiet host.
- [ ] **Explicit sync on the wire** untested: no syncobj manager headless, no timeline
  syncobjs on vmwgfx. Needs hardware.

## Notebook (`iritur`) checks

- [x] Notebook `ui-bench` baselines committed (2026-11-03): `iritur-08fd3c-{none,cpu,gl,
  vulkan,client-cpu}.json` — hasvk Vulkan is a hardware backend there now, no longer
  "vulkan-software". The GPU tests take the connected card's render node
  (tests/render_node.hpp; `LUMEN_TEST_RENDER_NODE` overrides).
- [x] Hardware Vulkan numbers landed (Broadwell hasvk, Mesa 26.2 reports 1.3.354 — the
  floor passes; `Vulkan composite path active` verified on the eDP): vkmark immediate
  686–937 FPS / fifo 60.00 sd 0.00, drag-smoke G6 move median 12.7 ms with a measured
  0.8 ms gpu fence leg (rendering-performance.md). The eDP panel is not host_image_copy
  capable, so that upload path stays unmeasured on this host.
- [ ] PRIME offload (PR #71): an app launched "using discrete GPU" should report the
  GTX 950M (look for `shell.apps` "GPUs:" lines). switcheroo-control is not installed.
- [ ] Thread-scheduling tiers re-measure (reverted on GL) and fence-deadline effect —
  now measurable on hasvk/i915.
- [ ] Hyprland in the startup comparison: 0.57 wedges before its IPC socket exists when
  launched from SSH (`start-hyprland` wants a real tty); weston/sway compare fine.
  Needs a console-launched run by the user (`~/lumen-compare`, pres-probe).

## Parked (one fix round each, or waiting for a port)

- [ ] **GL reset recovery** (raise-the-bar S2, reverted): capture cursor lost after reset.
- [ ] **Rounded corners / window shadows** (raise-the-bar F1, reverted): radius 30–32 masked
  client top corners; GL zeroed arrays per draw.
- [ ] **Shell background render cache + loop order**: still only in unpushed WIP c8d4499 on
  `claude/shell-first-paint` (old base) — port onto current main, then bench/tests/docs.
- [ ] **Tiling unbuilt** (ADR-012 trade-offs): pointer-drag split resize, orientations,
  monocle/dwindle.
- [ ] Non-text drag-and-drop with a real app on the real display (uri-list/icon/ask shipped
  and smoke-tested only).

## Do not re-open without new evidence

highp shaders, frozen test clock, malloc replacement, SDR-through-sRGB-view, GL build-ahead,
shell-side tiling, area-filter cost at non-integer render scales — see
`.zcode/skills/lumen-decisions/`.
