# Lumen open work

Extracted 2026-11-02 from the archived agent memories (`.zcode/memories/`), the ADRs' own
status sections, and the working tree. The ADRs are authoritative for detail; keep this list
current when an item closes.

## Now

- [x] **Commit/PR the ADR-013 settings-app tree** — closed 2026-11-17: everything merged to
  main (`ebe7046` portal Settings, `eaaf7ba` LUMEN_THEME export, `9022e74` settings-startup
  apply); 136/136 ctest at merged main. Push/PR remains the owner's call (never pushed
  without asking).
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
- [x] **portal-appearance-settings seat gate red is unattributed** — closed 2026-11-17
  (quiet-host serialized redo on merged main 9022e74, notebook): drag-smoke **10/10 PASS**,
  app-smoke **177 PASS / 0 WARN / 0 FAIL** with Z2 Screenshot `colours [(255,0,0)]` and Z4
  PickColor `255,0,0` as real-pixel PASSes — the Nov-16 "portal answering but capturing
  black" reading is not supported; the 101-FAIL signature matches the concurrent-session
  WARN family. All asserted probe artifacts preserved under
  `evidence/2026-11-17-seat-redo/` (busctl ReadOne u2/u1, SettingChanged dbus-monitor +
  journal, NotFound error-name/message, ReadAll glob, idle-strace: backend parked in
  poll(-1), zero syscalls/zero CPU over 30 s windows). Independent analyst recount
  confirmed every number. Note: this row's "no BTN codes" node reading was a
  `/proc/bus/input/devices` bitmap misreading — see
  `.zcode/memories/lumen-uinput-key-bitmap-format.md`.
- [x] **compositor-theme-env-export seat gate BLOCKED** — closed 2026-11-17 (redo on merged
  main 9022e74): stale holders killed (pids 3604798/3604800 from Nov 16; the original
  2692863 died earlier, boot_id unchanged proves no reboot), node regenerated from this
  tree with button caps EVIOCGBIT-verified, acceptance 2-4 run as written incl. the
  letter — appearance flipped **through the lumen-settings UI** (app wrote exactly the
  `appearance` line; compositor re-exported `LUMEN_THEME=one-dark`/`one-light`; app re-themed
  live 0.179/0.968), startup export with empty ui.theme → one-light, verbatim `ui.theme`
  name export, lumen-hello relaunch light 0.9189 / dark 0.2061, lumen-settings fresh start,
  teardown unset, config byte-restored (sha256 082ce0e5…). Smokes green (see the portal row).
  The lumen-shell SIGSEGV did **not** reproduce — split to its own row below (non-reproduction
  is not attribution). The row's "no BTN codes" mechanism was a bitmap misreading — see
  `.zcode/memories/lumen-uinput-key-bitmap-format.md`; the Nov-15/16 click-loss is best
  attributed to the concurrent-session WARN family. See
  `.zcode/memories/lumen-compositor-theme-env-export.md` (2026-11-17 section).
- [ ] **lumen-shell notification_view intermittent SIGSEGV** (split 2026-11-17 from the row
  above): ×2 on 2026-11-16 (coredumps 3499403 @21:41:39, 3521729 @21:53:11, identical
  stacks notification_view::build_ui → lx::ui::image → lx::runtime::memory_arena::allocate,
  SI_KERNEL, EXE = the theme-env worktree build), unreproduced 2026-11-17 on merged main
  9022e74 across both smokes, the N1–N8 notification rows and two interactive sessions —
  intermittent, mechanism unattributed. Next: ASan-built lumen-shell on the seat with a
  notify-send + shell-relaunch stress (targeted repro beats statistical smoke), or an
  app-smoke A/B on pre-merge main. Original stacks preserved at
  `evidence/2026-11-17-seat-redo/a-smokes/coredump-originals-Nov16.txt`.
- [ ] **lumen-settings transient "no writable config.toml" banner** (2026-11-17 seat redo,
  supplement): a "Changes are not being saved / There is no writable config.toml" banner
  showed ~10 s after a fresh lumen-settings launch and vanished after the first successful
  write — while locate_config resolved the real user file and the writes demonstrably
  worked (`evidence/2026-11-17-seat-redo/d-supplements/shot-pre-flip-banner-zoom.png`);
  no startup path setting the banner's memory_only state was found by source reading.
  Reproduce, then fix the stale banner state.

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
