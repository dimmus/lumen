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

## Host-limited gates (ADR-010; VM cannot close these)

- [ ] **app-smoke host calibration**: ~36 rows embed the dev VM's 1914×924 output
  geometry and its font metrics (deco title 28 px vs the notebook's 36, B6 flip rows,
  D1/D6 usable-area arithmetic, H1/S1 point probes). On the notebook (3200×1800 eDP,
  LUMEN_SMOKE_SCALE=1): 140/176 PASS, every got-value checks out against the new
  geometry. Parameterize the rows on the parsed output size + measured frame height.
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
