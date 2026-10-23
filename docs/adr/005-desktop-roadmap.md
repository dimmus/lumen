> **Status:** Proposed — where Lumen stands after ADR-004, and what it takes to become a
> desktop shell people would choose

# ADR-005: From render pipeline to desktop

**Date:** 2026-08-04
**Scope:** whole tree, forward-looking
**Deciders:** project owner
**Follows:** [ADR-004](004-architecture-evaluation.md)

---

## 1. Where the work went, and where it did not

ADR-004 rebuilt the frame pipeline, and it worked. Compositing is in linear light with a
float intermediate and an explicit encode pass, 10-bit scanout is a parameter, repaints are
scheduled against measured cost, present no longer blocks the render thread, the draw
contract carries the full payload, and the protocol boundary is fuzzed. That part of Lumen
is now genuinely modern — ahead of several shipping compositors.

The investment is also visible in the line counts:

| Area | Lines | State |
|---|---|---|
| `lx.gfx` | 5876 | Strong. Three backends, linear/HDR-capable, zero-copy import |
| `lx.compositor` | 4779 | Strong pipeline; thin protocol surface |
| `lx.ui` | 2421 | Two competing paradigms mid-migration |
| `lx.text` | 298 | One font, no line breaking, no fallback |
| `lx.shell` | 176 | Declarations only |
| `lx.shell.policy` | 112 | Declarations only |
| `lx.input` | **86** | **Empty — every body is `{}`** |
| `lx.a11y` | 74 | Stub |
| `lx.app` | 68 | Declarations only |

The compositor and graphics layers are 10,655 lines. Everything that makes the thing a
*desktop* — input, shell, toolkit, text, accessibility — is 3,235, and most of that is one
half-migrated widget tree.

**The binding constraint has moved.** It is no longer how well Lumen draws. It is that
almost nothing else exists yet.

## 2. Three findings that decide the plan

### F5 — Lumen cannot accept input at all

[modules/lx.input/lx.input.cppm](../../modules/lx.input/lx.input.cppm) is 86 lines and every
implementation is empty:

```cpp
void lx::input::seat::notify(pointer_event) {}
void lx::input::seat::notify(keyboard_event) {}
void lx::input::seat::process_events() {}
void lx::input::keyboard::set_focus(lx::surface_id) {}
lx::surface_id lx::input::pointer::focus() const { return {}; }
```

There is no libinput (CMake probes for it with `QUIET` and nothing links it), no
xkbcommon anywhere, no key repeat, no modifier state, no focus tracking, no serial
management, no touch, no tablet, no gestures. `wl_seat` is advertised, and there is nothing
behind it. The only xkb in the tree is a hardcoded US keymap *string* sent to clients so
their own libxkbcommon has something to parse.

A compositor that composites in linear light at ten bits with deadline-driven scheduling,
and into which you cannot type a character, is not yet a desktop. This is the single most
important gap and it blocks everything downstream: no input means no focus policy, no
keyboard shortcuts, no window management by the user, no text entry, no IME, no
accessibility.

### F6 — The shell protocol exists but is never advertised

`docs/architecture.md` describes the centrepiece of the design: the shell is a privileged
Wayland client, it binds `zlm_shell_v1`, and window-management intent flows over
`zlm_policy_bridge_v1` while the compositor enforces state.

Both sides are scaffolded — `lx.compositor/shell_bridge.cppm`, `lx.shell/bridge.cppm`,
`lx.session/privilege.cppm`. But `add_global` is called exactly seven times, and
`zlm_shell_v1` is not one of them:

```
wl_compositor  wl_subcompositor  wl_output  wl_seat
wl_data_device_manager  xdg_wm_base  zwp_linux_dmabuf_v1
```

So `lumen-shell` starts, connects, and finds nothing to bind. The architecture's defining
seam is not connected at runtime.

Seven of the ~38 protocols in `protocols/manifest.toml` are served. Missing and load-bearing
for a desktop: **layer-shell** (there is no way to place a panel, dock, wallpaper or
notification without it), **ext-session-lock** (no lock screen), xdg-decoration,
xdg-activation, xdg-output, presentation-time, viewporter, fractional-scale, text-input,
primary-selection, pointer-constraints, relative-pointer.

### F7 — `lx.ui` is two toolkits at once

There is a retained inheritance tree (`widget`, `label`, `button`, `entry`, `checkbox`,
`panel`, `image`, `scroll_area`, `window`) and a newer declarative node system
(`box_node`, `text_node`, `button_node` with `describe<>`, reconciliation and `operator|`
decorators). `docs/subsystems/ui-architecture.md` §9 calls the builder API superseded.

Carrying both is the largest piece of debt in the tree: every widget added has to be added
twice or picked a side, and neither is complete. The declarative side is the right one — it
is the model React, SwiftUI, Flutter and Slint converged on, and it is what makes state
handling and diffing tractable. The retained tree should go.

Under both, the text stack is one font with no line breaking, no bidi, no font fallback and
no colour emoji, which caps the toolkit well below usable regardless of which API wins.

---

## 3. Plan

Sequenced by dependency, not by appeal. Each phase is independently shippable and leaves the
tree in a coherent state.

### Phase 0 — Make it accept input (blocks everything) — **DONE**

> **Landed.** `lx.input` went from 86 lines of empty bodies to a real xkb keyboard state
> plus a libinput device layer, and events now reach clients. Details under each step.

1. [x] **libinput + logind device handling.** Devices open through a `device_provider`, so
   `lx.input` does not depend on `lx.session` and the seat stays testable against fixtures;
   the compositor supplies logind `TakeDevice` with a direct-open fallback. libinput's fd is
   an `fd_source` on the event loop. `suspend`/`resume` are exposed for VT switching — the
   fds must be released or the next session cannot claim them. Device add/remove is counted.
   **VT switching (done, ADR-006):** Ctrl+Alt+F*n* goes through logind `Seat.SwitchTo`; the
   compositor pauses on PauseDevice / `Active`=false (render gated, libinput suspended with
   every device handed back by ReleaseDevice, held keys and buttons released, windows
   `suspended`, frame callbacks at 1 Hz) and resumes on ResumeDevice with a full modeset,
   the cursor plane restated and a full repaint. `chvt` and another session ending resume
   the same way.
2. [x] **Server-side xkbcommon.** Real keymap compiled from configuration, with depressed,
   latched and locked modifiers tracked separately, named-modifier lookup for compositor
   keybindings, and per-key repeat flags. Clients receive the *compiled* keymap over a
   sealed memfd instead of the hardcoded US string — sealed because every client maps it
   shared, and without `F_SEAL_SHRINK` one of them could resize it under the others. Tested
   by checking a German layout produces `z` where US produces `y`, which the old string made
   impossible. The evdev→xkb keycode offset of 8 is applied in exactly one place; wrong by
   eight produces plausible letters, so it hides.
3. [x] **`wl_seat` for real.** `compositor::input_router` tracks every seat-derived
   resource per client and delivers keyboard enter/leave/key/modifiers and pointer
   enter/leave/motion/button/axis, with `axis_source` and v120 discrete values so a wheel
   notch and a touchpad drag are distinguishable. `enter` carries the held-key array and is
   followed immediately by modifiers, or a client inherits stuck modifiers across a focus
   change. Resources unregister on destroy, so the router cannot send into freed memory.
   Serials come from the seat's single monotonic counter.
4. [~] **Focus: hit-testing landed, policy still a placeholder.** `seat_manager` is bound
   to the compositor's real seat, not to a function-local one that nothing else could see.
   Pointer focus and keyboard focus are tracked separately, which is what lets
   click-to-focus and focus-follows-mouse differ.

   **Pointer focus.** It comes from per-surface hit-testing in `lx.compositor.wm`: one pass
   from top to bottom over the stacking order, against the surface extents intersected
   with the `wl_surface` input region (`set_input_region` is implemented). It allocates
   nothing. `enter`/`leave` are paired, with surface-local coordinates. While a button is
   held, the pressed surface keeps focus (the implicit grab): motion outside it still goes
   to it, and the window under the pointer gets `enter` only after release. When the scene
   changes under a still pointer (map, unmap, restack, a window that moved), focus is
   re-evaluated without waiting for motion.

   **Keyboard focus and stacking (placeholder policy).** A newly mapped toplevel takes
   keyboard focus. A press on any button focuses and raises the window under the pointer
   (click-to-focus, raise-on-press). The shell can already override this with
   `request_activate`, `request_raise`, `request_lower`, `set_toplevel_stacking_index` and
   `request_close` over the policy bridge.

   **Interactive move and resize.** One executor in `lx.compositor.wm`
   (`window_manager::begin_grab`) with three entry points:
   - `xdg_toplevel.move` / `.resize`. The serial must be the press serial delivered to
     that client, and the button must still be held (the `wlr_seat_validate_pointer_grab_serial`
     rule). An invalid edge is the `invalid_resize_edge` protocol error.
   - `zlm_policy_bridge_v1.request_move` / `request_resize`. The shell names the toplevel,
     but the serial must still be the live press serial, from any client.
   - **Placeholder binding: Super+left-drag moves, Super+right-drag resizes** from the
     edges of the third (corner by quadrant at the centre) the window was grabbed in. Neither
     press nor release reaches the client. It is temporary and moves to shell policy over
     the bridge.

   During a grab the client gets `leave` and no pointer events; the cursor shows the
   move or resize shape. Pointer motion is coalesced and applied once per tick. A move
   changes only the scene node's origin (no texture work), with the old and new rects as
   separate damage clips. A resize sends `xdg_toplevel.configure` with `resizing`, one in
   flight at a time: the next size goes out only after the previous serial was acked and
   its buffer committed, so every pointer position in between is coalesced. Geometry
   changes only when the client commits, and left/top edges shift the origin in the same
   snapshot as the new buffer, so no frame is stretched, cropped or mismatched. Release
   sends a final configure without `resizing`. Escape cancels and restores the start
   geometry; a client that disconnects or unmaps mid-grab ends it. Pointer-to-present
   latency is logged per present (`compositor.latency`, debug).

   **Subsurfaces (done).** `wl_subcompositor`/`wl_subsurface` are real
   (`lx.compositor:surface_tree`): position and `place_above`/`place_below` are
   double-buffered on the parent, synchronized children's content is cached and applied
   atomically with the parent's (same UI call, same snapshot, texture updates held back to
   that snapshot), `set_desync` and a parent going away apply what was cached, every
   protocol error (`bad_surface`, `bad_parent`, `wl_subsurface.bad_surface`,
   `wl_surface.defunct_role_object`) is posted, and every destruction order is safe. Each
   window's tree is one scene container flattened in paint order; the hit test walks it
   top-down, each surface clipped to its own extents and input region, and pointer focus
   follows the actual surface (a CSD title bar receives the press and its motion). Window
   geometry is clamped to the whole tree. foot's and alacritty's client-side frames, and
   GTK 4, now render (`scripts/app-smoke.sh` group A).

   **Popups (done).** `xdg_positioner` and `xdg_popup` are real
   (`lx.compositor:positioner`, `lx.compositor:popup`): the positioner rules are validated
   (`invalid_input`) and placed with the spec's precedence flip → slide → resize against
   the usable area (the whole output for a layer surface's popups), parent-relative, honouring
   `set_parent_size`/`set_parent_configure` and `reactive`; configures and `reposition`
   (v3) apply at the commit after the ack, so a popup moves in the same snapshot as its new
   content; every popup is the root of its own surface tree in a popup layer above every
   window and below the cursor, hit before the windows. The grab is a stack of one client's
   grabbing popups: the serial must be that client's last button or key press (stricter
   than wlroots, as Mutter and Smithay), pointer events are owner-events, a press outside
   the client dismisses the chain top-down and is consumed, the topmost mapped grabbing
   popup has the keyboard while its window stays activated, and an interactive move/resize
   or activating another client's window ends the grab. Every xdg-shell error on the popup
   path is posted (`invalid_positioner`, `invalid_popup_parent`, `not_the_topmost_popup`,
   `defunct_surfaces`, `invalid_grab`, `unconfigured_buffer`, `invalid_serial`,
   `defunct_role_object`). GTK 4 menus, context menus and popovers work
   (`scripts/app-smoke.sh` group B).

   **Not yet:**
   - Focus as the shell's decision, in place of the placeholder defaults.
   - Hit-testing across several outputs. It uses output 0's coordinate space until D5's
     per-output loop.
   - Touch: a popup grab can only be taken with a pointer or keyboard serial.

*Done when:* a real client receives a keystroke and a click, and `wev`/`weston-info` show a
correct seat. This is the milestone that turns Lumen from a renderer into a compositor.

**Verification status.** Everything above is unit-tested without hardware — 15 tests over
keymap compilation, modifier tracking, locked-vs-held, serial monotonicity and the held-key
set. The end-to-end path *cannot* be verified in this environment: it needs a seat with real
devices and permission to open them. The honest test is `wev` under a real session, and it
has not been run. Treat Phase 0 as complete in construction and unproven in the field until
someone types into it.

### Phase 1 — Make it a desktop — **in progress**

5. [x] **`zlm_shell_v1` is advertised**, and the registry now lists nine globals rather
   than seven. The protocol was already generated and both sides scaffolded; only the
   `add_global` call was missing, which is why `lumen-shell` connected and found nothing.
   `zlm_shell_v1`, `zlm_policy_bridge_v1`, `zlm_workspace_manager_v1` and
   `zlm_window_rules_v1` all bind, and `request_activate` /
   `set_toplevel_stacking_index` route to `shell_bridge`; binding the bridge emits the
   startup snapshot. Advertised to every client but bound only by one that passes the
   privilege check, so a rejected bind is a protocol error naming the reason rather than a
   global that mysteriously does not exist. Requests whose compositor-side action does not
   exist yet (maximize/fullscreen/minimize, workspace objects, rule persistence) accept and
   do nothing, so the shell can bind and drive what does work instead of failing at bind.
   - [x] **A real `lumen-shell`** consumes it. A one-thread libwayland-client process
     (`ppoll` on the display fd and a signalfd; no timers) binds `zlm_shell_v1`, keeps a task
     model from the snapshot and deltas (`lx.shell:task_model`), and draws a 32 px top
     panel through layer-shell (`lx.shell:panel`): one entry per window with its title, the
     focused one highlighted; a click sends `request_raise` + `request_activate`; entries
     follow create/destroy/title/focus live, repainting and damaging only the entries that
     changed. The compositor now sends `capabilities` on bind and `focus_changed` right
     after `snapshot_done` (no XML change), logs every bridge request with its result, and
     **enforces the binary half of the privilege model**: the peer's `/proc/<pid>/exe` must
     be the configured shell (`--shell PATH`, default `lumen-shell` next to the
     compositor), so another same-UID client can no longer bind the global and replace the
     shell's bridge (`scripts/app-smoke.sh` group E, `test_shell_bridge`,
     `test_task_model`). A restarted shell gets a fresh snapshot. Found on the way: the
     Wayland server's per-client destroy listener was indexed by a slot that compaction
     moves, so out-of-order disconnects corrupted a client's destroy list and the
     compositor could spin forever in it (shell restarts hit it); fixed, with a regression
     case in `test_protocol_conformance`. Since: workspaces and minimize (ADR 011 Part 3:
     per-output dynamic workspaces served as ext-workspace-v1, the panel's indicator, zlm v7
     `toplevel_workspace`) and an Alt+Tab window switcher (ADR 011 Part 4: most recently
     used first, the keys held through zlm v7 `held_action` while the application keeps the
     keyboard, a view only after 150 ms), notifications and a tray (ADR 011 Part 5: an
     org.freedesktop.Notifications server — spec 1.2, one layer surface for the popups,
     do-not-disturb, never over the lock, a history behind the panel's bell — and a
     StatusNotifierWatcher and host with com.canonical.dbusmenu menus as xdg_popups of the
     panel). Not yet: a clock, the shell owning focus policy.
   - [x] **A launcher, and apps that outlive their launcher** (ADR-006 Part B). The panel's
     Apps button and the Super tap (a zlm v4 tap grab) toggle an overlay-layer launcher
     listing the XDG desktop entries (Desktop Entry Specification 1.5: ID shadowing,
     Hidden, NoDisplay, OnlyShowIn/NotShowIn, TryExec, localised names, Exec quoting and
     field codes; inotify marks the catalog stale, rescanned on open); Super+Return starts
     a terminal. Each launch is its own systemd user service
     (`app-lumen-<id>@<random>.service`, `StartTransientUnit`, `Type=exec`, `app.slice`;
     a transient scope per launch below systemd 250), double fork + `setsid` without
     systemd --user, with an activation token from the
     triggering input so the new window takes the keyboard. Launched apps survive the shell
     and the terminal that started the session. The compositor gained sway's rule that an
     `on_demand` top/overlay layer surface takes the keyboard on map. Not yet: icons,
     desktop actions, `DBusActivatable` launching, recently-used ordering.
6. [x] **wlr-layer-shell** (v5, `lx.compositor:layer_shell`). Panels, docks, wallpapers,
   notifications and OSDs now have somewhere to go. Every layer surface roots a surface tree
   in one of four layer containers (background and bottom under the windows, top and
   overlay above them, all under the popup layer). Arrangement is wlroots' (two passes,
   exclusive zones first, zone -1 against the whole output) with two deliberate deviations:
   a smaller committed size is centred between opposite anchors (the spec's text) and a zone
   counts only once mapped. The usable area it leaves drives placement, move constraints,
   `configure_bounds`, window popups and the new **maximize** (`xdg_toplevel.set_maximized`,
   the bridge's `request_maximize`, a title-bar double click; position, size, state and
   frame extents apply with the answering commit; maximized windows follow usable-area
   changes; `wm_capabilities` lists maximize). Keyboard interactivity is exact: `exclusive`
   on top/overlay takes every key (only its own popups can take them back), `on_demand` is
   focused by a click, `none` never, and popups of a `none` surface grab without the
   keyboard. Layer popups (`get_popup`) are constrained to the whole output. Every
   layer-shell error is posted with the spec's code (`role`, `invalid_layer`,
   `already_constructed`, `invalid_size`, `invalid_anchor`,
   `invalid_keyboard_interactivity`, `invalid_exclusive_edge`, `invalid_surface_state`)
   (`scripts/app-smoke.sh` group D). Not yet: several outputs (every layer surface is on
   output 0 until D5, and `output_gone` is never sent), fullscreen. Since: drag-to-restore a
   maximized or snapped window, Windows 11's Super+arrow snapping with xdg tiled states, and
   edge snapping with a preview (ADR 011 Part 4, app-smoke group O).
7. **ext-session-lock.** Security-critical and easy to get subtly wrong: the lock surface
   must survive a crashing locker, and input must not leak to normal clients while locked.
   - [x] **ext_session_lock_manager_v1** (ADR 007 §2d): input leaves normal clients at the
     lock request, `locked` only after a locked frame reached the output (or nothing of the
     session can be on screen), a locker that dies leaves the session locked on dark red
     until a new locker takes over or logind's `Unlock`, every protocol error; lock across VT
     switches and display power; logind Lock/Unlock/LockedHint/before-sleep;
     zlm_shell_v1 v5 `lock_state`/`lock_requested`; lumen-shell's `--lock-command` (Super+L,
     lock before blanking, relaunch of a dead locker). Verified with swaylock and with
     lumen-probe's locker (app-smoke group L). Lumen ships no locker of its own.
   - [x] **text-input-v3 (v2), input-method-v2, virtual-keyboard-v1** (ADR 007 §2e): fcitx5
     and ibus run as ordinary clients; keys reach the input method's grab only while a
     focused text input is enabled, preedit and commits reach GTK 4 and foot, the candidate
     window sits at the cursor (app-smoke group M). The toolkit side (item 15) is separate.
8. **xdg-decoration, xdg-activation, xdg-output, primary-selection.** The unglamorous set
   that makes real applications behave.
   - [x] **xdg-decoration** (v2): foot and alacritty negotiate server-side and get a Lumen
     frame — title bar with the client's title (ellipsized when narrow), close button,
     1 px borders with an invisible 8 px resize band — whose presses drive the same grab
     executor as `xdg_toplevel.move/resize`; middle-click on the title lowers. A client that
     insists on `client_side` (GTK, `foot -o csd.preferred=client`) keeps its own frame;
     `--decorations=client` withdraws the global. The mode applies with the commit
     answering its configure (frame-perfect). Text comes from `lx.text`'s new single-line
     rasterizer (HarfBuzz + FreeType into a cached texture); missing glyphs render as the
     face's .notdef until font fallback (step 14). A title double click maximizes (step 6).
     Not yet: a window menu (right-click), theming. Since: drag-to-restore a maximized
     window (ADR 011 K4), keyboard move and resize (Alt+F7, Alt+F8).
   - [x] **primary-selection** (`zwp_primary_selection_device_manager_v1`) and the
     **clipboard** done properly (`wl_data_device_manager` v4): one selection per seat and
     kind, offered — every mime type — to the keyboard-focused client before its `enter` and
     again on every change, inert once that client loses the keyboard; `set_selection` must
     quote an input serial the client was sent (per-client serial log), not older than the
     selection in place, and a refused source is cancelled; `receive` passes the fd to the
     owner only for an offered type; an owner exiting clears the selection. Copy and paste
     work between foot, alacritty and GTK 4 both ways, by keyboard and by middle click
     (app-smoke group F). Not yet: a selection that outlives its owner (a clipboard
     manager on `ext-data-control-v1`).
   - [x] **drag-and-drop** (`wl_data_device.start_drag`): the serial of the held press,
     a drag icon in its own scene layer moved once per tick with the pointer, enter / motion
     / drop / leave to the surface under the pointer (the source client's own surfaces
     included; a drag without a source stays in its client), action negotiation with the
     modifiers' user action (Shift move, Ctrl copy, Alt ask) and `ask`, the pointer showing
     the outcome, cancellation on Escape, a release over nothing, nothing accepted and
     client death. Probe-to-probe drags and GTK 4 → foot work (app-smoke group G). Not yet:
     touch-initiated drags (no touch routing).
   - [x] **xdg-output** (`zxdg_output_manager_v1` v3) and the output contracts: outputs
     named after their connector (`Virtual-1`), a virtual HEADLESS-1 without KMS, logical
     rect, name and description closed by `wl_output.done`; `wl_surface.enter` / `leave` from
     scene-node rects for every surface (toplevels, subsurfaces, popups, layer surfaces, the
     drag icon, the cursor surface), replayed to a `wl_output` bound late; the v6 preferred
     buffer scale and transform (sent only off the defaults; scale capped at 1 until buffer
     scale is honoured). foot and GTK 4 receive enter (app-smoke group H).
   - [x] **ping/pong liveness** (`xdg_wm_base.ping`): pings on activation, presses, the
     configure watchdog and periodically to the focused client; unanswered past
     `--ping-timeout` (5 s) the client is shown not responding — "(Not Responding)" in the
     server-side title, its windows dimmed, `toplevel_flags` to the shell (zlm v2), pointer
     motion withheld — and never killed; libwayland's per-client buffer is 1 MiB, so a
     stopped foot survives hundreds of motions and recovers on SIGCONT (app-smoke group I).
     Not yet: a "wait / force quit" offer in the shell.
   - [x] **xdg-activation** (`xdg_activation_v1`): 128-bit random tokens, single use,
     30 s lifetime; a token can grant focus when its serial is an input serial delivered to
     the issuer and the issuer had the keyboard or it is the issuer's own last click or key
     press (so a layer-shell launcher can issue from a click). Presenting it focuses and
     raises the window unless the user has since pressed a key or button in another window
     (the X11 user-time rule, as in Mutter) — then, and for tokens that are unknown,
     expired, serial-less (foot's bell) or never grantable, the window is marked
     demands-attention instead of stealing focus, and the shell highlights its task
     (`toplevel_flags`, zlm v2). A token presented before the window maps is decided at map:
     a launch token the user moved on from (or that expired) opens the new window below the
     focused one without the keyboard, while a token that proves nothing (GTK's self-issued
     serial-0 token) leaves the new-window policy alone. foot started
     with `XDG_ACTIVATION_TOKEN` is focused, and no longer warns (app-smoke group J). Not
     yet: focus-stealing prevention for new windows that present no token (still the
     placeholder "a new window takes the keyboard"; it belongs in shell policy), a frame
     accent for an urgent window (the shell shows it).

*Done when:* `lumen-shell` draws a panel, switches workspaces, and locks the session.
Status: the panel — yes (task list over the bridge, step 5), with a launcher. Workspaces — yes
(ADR 011 Part 3: per output, dynamic or static, owned by the compositor and served as
ext-workspace-v1; the panel's indicator switches them, Super+1…9 and friends, minimize and
sticky windows, hotplug keeps what is visible visible). Session lock — yes (step 7; the
locker is an external client such as swaylock, started by the shell's `--lock-command`).

### Phase 2 — Cash in the pipeline work

ADR-004 made these implementable; they are now mostly protocol code.

9. **viewporter + wp-fractional-scale-v1** — D1 delivered `src`/`dst`, so this is
   negotiation plus plumbing.
10. **color-management-v1** — D2 delivered linear blending, PQ/HLG and 10-bit output. This
    is the differentiator: almost nothing ships working HDR on Linux.
11. **presentation-time, wp-fifo, wp-commit-timing** — D3 delivered the deadline model.
12. **D5, the per-output frame loop** — required for multi-monitor, per-output scale and
    mixed refresh rates. Plan is in ADR-004's "Needs revisiting"; step 1 (grouping
    per-output state into a struct) pays for itself alone.

### Phase 3 — A toolkit worth using

13. **Finish the declarative migration and delete the retained tree.** One paradigm.
14. **Real text.** Line breaking (UAX #14), bidi (UAX #9), font fallback chains, colour
    emoji, subpixel positioning. This is the difference between "renders text" and "renders
    text people read all day".
15. **Focus and input in the toolkit** — keyboard navigation, focus rings, IME via
    text-input-v3 and input-method-v2. IME is not optional; it is how most of the world
    types.
16. **Accessibility, from the start.** ADR-001 covers the AT-SPI bridge. Every toolkit that
    deferred this paid more later and shipped worse. Doing it while the widget set is small
    is the cheapest it will ever be.
17. **Scrolling, gestures, animation** driven by the frame clock D3 already provides.

### Phase 4 — Differentiate

18. **HDR end to end** — the pipeline is ready; it needs KMS `HDR_OUTPUT_METADATA` and
    `COLOR_ENCODING`, plus the protocol from step 10.
19. **VRR / adaptive sync** — the deadline scheduler is the hard part and it exists.
20. **ext-image-copy-capture + portal** — screencast and screenshot, the thing every
    meeting needs.
21. **wp-security-context and sandboxing** — per-client capability gating. Lumen's
    privilege checker is already the right shape for it.
22. **Xwayland**, rootless, for the long tail.

### Cross-cutting, start now rather than later

- **Input latency measurement.** Instrument end to end (event → present) and track it like
  the frame budgets already are. It is the number users feel and the one Lumen is currently
  best positioned to win.
- **An interaction test harness.** Golden images cannot test focus, drag, grabs or IME. A
  scripted Wayland client that drives real sequences is needed before Phase 0 lands, or none
  of Phases 0–3 will have meaningful coverage.
- **Finish D4's Vulkan 1.3 move** before the renderer grows more passes, not after.

---

## 4. What to do first

**Phase 0, step 1–3.** Nothing else changes what Lumen *is*. Every remaining item either
depends on input or is invisible without it, and the gap between "renders beautifully" and
"is a desktop" is exactly one working seat.

The honest framing for anyone reading the repo today: Lumen has an excellent display
pipeline and the skeleton of a desktop around it. ADR-004 made the first part true. This
plan is about the second.
