> **Status:** Accepted. Part 1 (the content-update queue and the commit-path protocols:
> linux-drm-syncobj-v1, implicit dma-buf readiness, fifo-v1, commit-timing-v1,
> tearing-control-v1, content-type-v1, single-pixel-buffer-v1, alpha-modifier-v1)
> implemented. Part 2 (window and session protocols: xdg-dialog-v1, xdg-toplevel-icon-v1,
> xdg-system-bell-v1, xdg-foreign v2 and v1, keyboard-shortcuts-inhibit-unstable-v1,
> ext-data-control-v1 and wlr-data-control v2, security-context-v1, zlm_shell_v1 v6)
> implemented. Part 3 (pointer: pointer-constraints-unstable-v1,
> relative-pointer-unstable-v1, pointer-gestures-unstable-v1 v3) implemented. Part 4
> (screen capture: ext-foreign-toplevel-list-v1, ext-image-capture-source-v1,
> ext-image-copy-capture-v1, wlr-screencopy v3, the capture indicator) implemented. Part 5
> (portals: xdg-desktop-portal-wlr for ScreenCast, xdg-desktop-portal-lumen for Screenshot,
> PickColor and the source chooser, the session environment) implemented.

# ADR-009: App compatibility and screen capture

**Date:** 2026-10-21
**Scope:** Part 1: `lx.compositor` (`:content_update` — queue, readiness policy, readiness
poller; `wire_surface.cpp` — the content-update commit, role snapshots, frame-callback
lists; `wire_fifo.cpp`, `wire_tearing.cpp`, `wire_single_pixel.cpp`, `wire_syncobj.cpp`;
`:output_policy` — `allow_async`; `compositor_impl` — poller source, latch/tick flush points,
commit-timing clock arming, release-fence mailbox), `lx.gfx` (`:drm_timeline`; solid draws in
the GL, CPU and Vulkan backends and the headless path), `lx.scene` (solid draws, the
opacity-squared fix), `lx.scheduler` (`frame_clocks::request_at`), `lx.drm`
(`supports_async_flip`), `lumen-probe timing|solid|tearing`, `scripts/app-smoke.sh` group T.
Part 2: `lx.compositor` (`:toplevel_icon`, `:foreign_registry`; `wire_dialog.cpp`,
`wire_toplevel_icon.cpp`, `wire_bell.cpp`, `wire_foreign.cpp`, `wire_shortcuts_inhibit.cpp`,
`wire_data_control.cpp`, `wire_security_context.cpp`; `:wm` — modal policy, centring of
transients; `:keybindings` — inhibition, Super+Escape; `wire_selection.cpp` — per-source
ops), `lx.wayland.server` (security-context listeners, `security_context` on connections),
`zlm_shell_v1.xml` v6, `lx.shell` (`:icon_theme`, `:visual_bell`; icons, parents and the
bell in the bridge, task model and panel), `lumen-probe dialog|icon|bell|foreign-export|
foreign-import|shortcuts-inhibit|data-control|security-context`, `scripts/app-smoke.sh`
groups U and V, `tests/test_window_protocols.cpp`, `tests/test_toplevel_icon.cpp`.
Part 3: `lx.input` (unaccelerated deltas, an absolute device's previous position, gesture
events, a device's gesture capability), `lx.compositor` (`:pointer_constraints` — effective
regions, confinement, the activation predicate; `wire_pointer_constraints.cpp`,
`wire_relative_pointer.cpp`, `wire_gestures.cpp`; `:input_router` — pointer-focus hook,
motion lock, `send_pointer_frame`; `compositor_impl` — the motion path, warps, Super+Escape,
gesture sink), `lumen-probe constraints|gestures`, `scripts/inject-input.py rel`,
`scripts/app-smoke.sh` group W, `tests/test_pointer_constraints.cpp`,
`tests/test_pointer_protocols.cpp`.
Part 4: `lx.compositor` (`:capture` — the policy; `:capture_engine` — jobs, completions,
guarded shm, the render-side copies; `wire_foreign_toplevel.cpp`, `wire_capture.cpp`,
`wire_capture_cursor.cpp`; `compositor_impl` — the copies after `composite_output`, window
views, the worker read-back, lock/pause/power/hotplug hooks, virtual-output CPU capture;
`:shell_bridge` — the toplevel observer; frame-callback pacing of captured windows),
`lx.gfx` (`:gl_capture`; `gl_compositor::composite_into`), `lx.scene` (capture views 16–23,
`output_view_config::root`), `lx.shell` (the panel's capture dot), `lumen-probe toplevels|
capture|cursor-capture|animate`, `scripts/app-smoke.sh` group Y, `scripts/monitor-smoke.sh`
step `capture`, `tests/test_capture.cpp`, `tests/test_capture_protocols.cpp`,
`tests/test_gl_capture.cpp`, `tests/test_vk_capture.cpp`.
Part 5: `lx.portal` (reworked: `:compose`, `:png`, `:files`, `:chooser`, `:desktop`,
`:overlay`, `:service`; the Flatpak-token stub removed), `executables/xdg-desktop-portal-lumen`,
`deploy/portal/` (`lumen.portal`, `lumen-portals.conf`, the D-Bus service),
`deploy/systemd/xdg-desktop-portal-lumen.service.in`, `deploy/xdg/xdg-desktop-portal-wlr/Lumen.in`,
CMake (`LUMEN_BUILD_PORTAL`, install rules, the build-tree mirror under `share/`),
`scripts/portal-session.sh`, `scripts/run-desktop.sh` (`--no-portals`),
`scripts/app-smoke.sh` group Z, `scripts/inject-input.py` (navigation keys),
`tests/clients/lumen-portal-probe.cpp`, `tests/test_portal_compose.cpp`.
**Deciders:** project owner

---

## Context

Mesa's Vulkan WSI paces FIFO swapchains with `wp_fifo_v1` and `wp_commit_timer_v1` and asks
for tearing with `wp_tearing_control_v1`; GPU drivers with explicit sync (NVIDIA, newer Mesa)
want `wp_linux_drm_syncobj_v1`; GTK and Firefox draw backgrounds with single-pixel buffers
and fade with alpha modifiers. Each of these either constrains *when* a committed update may
be applied or adds state that rides it. Lumen applied every commit at once: there was no way
to hold an update back.

wayland 1.24 specifies the model the protocols assume: a commit creates a content update
placed at the end of a per-surface queue, applied atomically with the synchronized
subsurfaces' updates it depends on, once its readiness constraints are met. Mutter
(`meta-wayland-transaction.c`), Smithay (blockers) and wlroots (`wlr_surface_lock_pending`)
all implement it in some form.

## Decision (Part 1)

The full design, with the alternatives and the prior art read, is
`design-app-compat-capture.md` §0, §3 and Part A; this records what was built.

1. **Content-update queue** (`:content_update`, Wayland-free). A commit of a surface that is
   not effectively synchronized makes a DCU. Fast path: empty queue and nothing blocking —
   applied at once, as before, no pool slot. Otherwise the update (surface state, a role
   snapshot, the tree's cached child state, synchronized descendants sealed in) waits in a
   fixed pool (512) and applies in commit order when ready. Blockers: fence waits (counted,
   cleared by the poller), the fifo barrier and the commit-timing target (evaluated when a
   queue is tried, never stored). Hidden surfaces ignore fifo and timing.
2. **Bounds.** 128 queued updates per surface and per client (weston-simple-timing queues
   ~110 frames ahead); beyond, the newest two merge — a frame is dropped, nothing is ever
   shown early. Frame callbacks moved to a shared node pool.
3. **Readiness poller.** One epoll fd registered as one event-loop source; entries are
   `{update, generation}` so a stale wake is ignored. No allocation per wait.
4. **Implicit readiness.** A dma-buf update waits for its writers' fences (poll, then an
   exported sync_file), so a slow GL client never stalls the composite.
5. **Explicit sync** only where it works end to end; acquire waited for *signalled* on the
   UI thread (eventfd from `DRM_IOCTL_SYNCOBJ_EVENTFD`); release signalled at once when never
   sampled, otherwise at buffer retirement with the newest composite fence (in-order GPU
   queue). Only dma-buf buffers.
6. **fifo / commit-timing** tied to the per-output frame clocks: barriers clear when the
   pace output's tick latches the frame that showed them; targets apply in the first frame
   predicted to present no earlier than 0.5 ms before them; a stopped clock is armed for
   that one tick (`request_at`).
7. **Solid draws** for single-pixel buffers in every backend (GL: a scissored clear when
   opaque and untinted); **alpha** as node opacity, with the scene no longer multiplying it
   into the tint too (it was applied twice: alpha 0.5 showed as 0.25).
8. **Tearing**: hint stored and evaluated by a pure policy (`allow_async`); on a card
   without `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` it is ignored and reported once.

## Decision (Part 2)

The design is `design-app-compat-capture.md` §0 (D1, I1, B1, XF1, K1, DC1, SC1, SC2) and
Part B; deviations are recorded in its implementation notes.

9. **Modal dialogs** are the window manager's: effective while the parent is mapped; the
   parent never takes the keyboard (focus, activation and shell requests are redirected to
   the topmost modal descendant), a press on it is consumed and focuses the dialog, the
   dialog stays above it through raise, lower and restack; every child maps centred on its
   parent. `set_modal` acts at once (Mutter).
10. **Icons** are snapshotted at `set_icon` into sealed memfds and reach the shell as fds; the
    shell resolves names (XDG icon theme, PNG/SVG) — the compositor never decodes an icon.
11. **The bell** goes to the shell (visual flash), 4 per second per client, none while locked.
12. **xdg-foreign** v2 and v1 share one registry of unguessable handles; relations are undone
    with the objects.
13. **Shortcuts inhibition** is granted (sway, niri, KWin); VT switching, logout, Escape-in-
    grab and Super+Escape (the toggle) are never inhibited, nor shell grabs flagged
    `not_inhibitable` (zlm v6).
14. **Data control** (ext and wlr v2) on the one selection model, each source with its own
    ops; no serial; hidden from sandboxed clients.
15. **security-context-v1**: the server accepts on the engine's socket and marks those clients
    sandboxed; the global filter then hides session-wide globals exactly. The manager itself is
    hidden from sandboxed clients.

## Decision (Part 3)

The design is `design-app-compat-capture.md` §0 (PC1, RP1, G1) and Part C; deviations are in
its implementation notes.

16. **Pointer constraints** hold only while the constrained surface has pointer focus, its
    window has the keyboard, the pointer is inside region ∩ input region ∩ surface (to
    activate), and no session lock, VT pause, move/resize/drag or popup grab owns the pointer
    — Mutter's rule, stricter than niri's. At most one is active (the pointer focus's). A
    lock keeps the pointer still (no cursor move, no hit-test, no `wl_pointer.motion` —
    enforced at the router, so no re-hit or catch-up can leak one) while relative motion
    flows; a confinement clips the motion segment across the region's rects and slides along
    edges (wlroots' `wlr_region_confine` idea), one wl_fixed short of far edges.
17. **Warps** (a lock's cursor hint on unlock while the surface still has the pointer; a
    confinement brought inside a new region) move the pointer like motion does — clamped to
    the outputs, cursor and focus following, `wl_pointer.motion` — with no relative motion.
    Region and hint changes apply with the surface's content update and take effect at the
    end of the commit request, never in the middle of applying an update.
18. **Super+Escape** releases the active constraint as well as toggling a shortcuts
    inhibitor (one key gives a VM viewer's keyboard and pointer back); a persistent
    constraint re-arms after the pointer leaves the surface or the window loses the keyboard;
    a oneshot is spent at its first deactivation and stops blocking a new constraint.
19. **Relative pointer** carries libinput's accelerated and unaccelerated deltas (an absolute
    device: its position change in layout units, both alike) to the pointer focus's client,
    before the motion and in its frame; **gestures** (v3) are latched at begin to the client
    with pointer focus, through a compositor-policy hook that consumes none today.

## Decision (Part 4)

The design is `design-app-compat-capture.md` §0 (FT1, CC1–CC10, WS1, CI1) and Part D;
deviations are in its implementation notes.

20. **Copies ride the composite.** An output frame is copied on the render thread right
    after the output composited it, from its scanout target — never a composite of its own;
    a frame needed with nothing new composited comes from the newest composited slot. A
    requested frame waits for its output to change, so a capture starts no clock.
21. **Damage** is the composites' own: a per-output ring of the last eight composites'
    damage; a session copies everything since the composite it last copied. A window
    session composites its own capture view (the window's tree projected into the buffer),
    damaged by the scene.
22. **Formats**: shm XRGB/ARGB8888 everywhere; dma-buf XR24/AR24 LINEAR where there is a
    render node (GL blit). On GL a shm frame is blitted into a LINEAR GBM buffer and read on
    a worker thread: render never maps.
23. **Privacy**: a capture during the session lock is a black curtain, then frames are held
    until unlock; a lock epoch voids every copy made before the request, even one that lands
    after it. VT away and display power off hold frames. Capture globals are hidden from
    sandboxed clients. The shell's panel shows a red dot while anyone else captures (zlm v6
    `capture_state`).
24. **wlr-screencopy v3** is a front end of the same core: one one-shot session per frame,
    `copy_with_damage` per (client, output) damage points.

## Decision (Part 5)

The design is `design-app-compat-capture.md` §0 (P1–P3, E1) and Part E; deviations are in
its implementation notes.

25. **ScreenCast is xdg-desktop-portal-wlr's** (0.8.4: ext-image-copy-capture outputs and
    windows, dma-buf and shm, damage, embedded or hidden pointer). It is the maintained
    PipeWire producer of the capture protocols' authors; a Lumen producer would duplicate
    it, and an in-compositor PipeWire producer (Mutter, KWin; niri via Mutter's D-Bus API)
    would put PipeWire in the compositor for no gain — with ext-image-copy-capture a cast
    costs exactly the compositor's one copy per frame. Its config caps casts at 60 frames/s.
26. **Screenshot and PickColor are xdg-desktop-portal-lumen's** (Screenshot version 3,
    targets Screen | Window | Area): captured in-process over ext-image-copy-capture,
    composed in layout space at the largest scale, PNG at zlib level 1. Non-interactive →
    a private runtime file; interactive → `$XDG_PICTURES_DIR/Screenshots` with GNOME's
    naming; Window → the window captured on its own. xdpw's own Screenshot (grim into
    `/tmp/out.png`, no choice, no PickColor) is not used. No Active Window target: nothing
    outside the shell knows the active window.
27. **The cast's source chooser is Lumen's** (`xdg-desktop-portal-lumen --choose`, xdpw's
    dmenu protocol): an overlay list of outputs then windows, with the window's application.
28. **`lumen-portals.conf`**: default gtk, ScreenCast wlr, Screenshot lumen, Inhibit none.
29. **Session environment** (E1): `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP=Lumen`,
    `XDG_SESSION_TYPE=wayland` exported to the systemd user manager (the environment every
    portal backend is activated with) and the frontend restarted; `run-desktop.sh` does it
    reversibly through `scripts/portal-session.sh`, with the build tree's portal files via
    `XDG_DESKTOP_PORTAL_DIR`, and starts WirePlumber when no PipeWire session manager runs.
    Nothing under `~/.config` is written.
30. **Locked session**: the backend refuses (response 2) while logind's `LockedHint` is set;
    the compositor's curtain stays the guarantee.

## Consequences

- Vulkan FIFO clients (vkcube on lavapipe) present one frame per refresh with every frame
  shown; timed commits land within a refresh after their target, never before.
- An idle desktop costs what it did: nothing runs without queued updates, and a timed update
  wakes its output once.
- Not exercised on the development host (vmwgfx): explicit sync end to end (no syncobjs —
  the global is hidden; the timeline logic is unit-tested on fake kernel ops) and async
  flips (no capability). The atomic-async commit path is not wired: no hardware here can
  validate the property set the kernel accepts in an async commit; the policy's
  `pipeline_tested` input stays false until it is.
- Part 2: GTK 4 and Qt 6 dialogs are modal (GTK's message dialog checked live), foot no
  longer warns about icons or the bell and its bell flashes the window, wl-clipboard and
  cliphist work through ext-data-control, a VM viewer gets the desktop's shortcuts while the
  user keeps VT switching and Super+Escape, and Flatpak clients are identified by their
  security context. Not exercised on this host: Flatpak itself (security-context is checked
  at the protocol level, with lumen-probe playing the engine).
- Part 3: games, VM viewers and weston-constraints lock and confine the pointer; relative
  motion keeps flowing while locked (mouse and VM absolute pointer alike). Not exercised on
  this host: gestures from real hardware (no touchpad, no uinput) — delivery is checked
  in-process with injected gestures, and the compositor logs each device's capabilities.
- Part 4: grim (output, window — also covered — and with the pointer), wf-recorder and
  lumen-probe capture work at scale 1 and fractional scale, pixel-identical to the
  compositor's own read-back; screencasts carry only damage; capture costs the render thread
  ~0.16 ms per copied frame for dma-buf and shm alike on SVGA3D. Not exercised on this host:
  Vulkan-backend capture on hardware (lavapipe in-process only; it reads shm frames back
  asynchronously — a copy into the session's persistent mapped buffer, waited for and
  written out on a worker — and captures no windows), multi-planar or tiled dma-buf
  targets (LINEAR only).
- Part 5: a Screenshot through xdg-desktop-portal (non-interactive, with the frontend's
  access dialog the first time; interactive area, output, all; a window), PickColor, and a
  ScreenCast of an output or a window chosen in Lumen's list, consumed over PipeWire, all
  work on a `run-desktop.sh` session, and the user manager is left as it was afterwards. Not
  exercised on this host: browsers and OBS themselves (not installed; the probe is a
  PipeWire consumer like them), dma-buf casts (xdpw keeps only modifiers its GBM confirms,
  and vmwgfx's GBM has no modifier support, so it casts through shm; offering the implicit
  modifier as well trips an xdpw 0.8.4 assertion on such drivers, so the capture
  constraints stay LINEAR), Flatpak callers (the document-portal path of a sandboxed app).
