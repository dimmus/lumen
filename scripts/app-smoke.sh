#!/usr/bin/env bash
# Unattended real-application check on the real console.
#
# Runs the compositor at LUMEN_LOG=debug with real clients (foot, gtk4-widget-factory) and
# lumen-square, drives them with injected evdev input (scripts/inject-input.py, as root),
# and judges every scenario from the compositor's log — the window manager's lines and the
# surface tree's `tree root=… nodes=…` dumps, which list every drawn surface of a window in
# paint order with its global rect at that moment — and from the clients' side: the probes'
# and lumen-square's stdout, WAYLAND_DEBUG=client traces of foot and GTK, lumen-shell's log.
# The same machinery as drag-smoke.sh; that script stays the separate regression gate for
# move/resize and latency.
#
#   ./scripts/app-smoke.sh                       # from the TTY you are logged into
#   ./scripts/app-smoke.sh --join-seat-session   # from SSH (see drag-smoke.sh)
#
# Options:
#   --join-seat-session   run the compositor inside the active seat0 logind session
#   --countdown S         seconds of warning before input starts (default 3)
#   --socket NAME         Wayland socket name (default lumen-app-smoke)
#   --logs DIR            log directory (default <repo>/app-smoke-logs)
#   --only LIST           comma list of scenario groups to run (default: all available)
#   --skip-gtk            skip the GTK scenarios
#   --scale S             the output's scale (fractional allowed); only with --only O — the
#                         other groups assume scale 1
#
# Groups (more arrive with the steps that implement them; see the design in TEST.md):
#   A  subsurfaces
#     A1 foot with client-side decorations (`-o csd.preferred=client`: it insists, and the
#        compositor respects that): its title bar and borders are subsurfaces —
#        mapped, part of the window geometry, the title bar above the main surface
#     A2 drag foot's title bar (a subsurface): xdg_toplevel.move; every node moves by the
#        window's delta. A2s: the same with Super+drag (the compositor's grab), exact
#     A3 resize from foot's CSD border subsurface: in every tree dump during the grab the
#        title bar is exactly as wide as the window geometry (frame-perfect); one configure
#        in flight, every geometry change answers an acked configure
#        (foot's own trace: >=4 get_subsurface, one xdg_toplevel.move, one resize edges=10)
#     A4 lumen-square --sub: a synchronized 32 px strip; in every tree dump during a resize
#        its width equals the window's (never a frame with one size of each). Then
#        --sub-desync: strip commits apply on their own.
#     A5 gtk4-widget-factory maps and stays up (GSK_RENDERER=cairo; see TEST.md)
#   B  popups (lumen-probe for exact geometry, GTK for real-world menus)
#     B1 positioner base: popup 0 is configured at the rect the anchor/gravity give and
#        mapped at that offset from the window geometry
#     B2 at the output's right edge: flip_x puts it left of the anchor rect, slide_x
#        against the edge, resize_x clips it to the space left
#     B3 a grabbing chain of three: keyboard on the topmost; an outside click dismisses
#        all three top-down (popup_done 2, 1, 0) and reaches no client
#     B4 a grab quoting a stale serial is denied: popup_done at once
#     B5 an interactive move (Super+drag) ends the popup grab; a non-grabbing popup
#        follows its window exactly
#     B6 gtk4-widget-factory context menu: grabbed, mapped where its positioner says,
#        keyboard on it, Escape closes it, an outside click dismisses it, flipped near the
#        output's bottom edge, slid at its right edge (WARN if GTK's layout differs)
#     B7 a real GTK nested chain: widget-factory page 2's menubar, Edit (popup of the window)
#        then its Checks submenu (popup of that popup, opened by the H mnemonic); an outside
#        click dismisses the submenu, then the menu. From GTK's trace (B6+B7): every
#        xdg_popup.configure equals the compositor's computed rect, popup_done reached
#        exactly the dismissed popups in order, and no outside click reached GTK
#   C  server-side decorations (xdg-decoration)
#     C1 negotiation: foot asks for server-side and gets it (the frame applies with the
#        commit answering that configure); foot -o csd.preferred=client gets client-side and
#        draws its own subsurface frame; gtk4-demo is never told server-side
#     C2 drag the compositor's title bar: a decoration grab moves the window by exactly the
#        pointer delta and the press reaches no client
#     C3 drag 4 px outside the right edge (the invisible resize band): a resize grab with
#        edges=8, the width follows to within foot's cell rounding
#     C4 click the close button: xdg_toplevel.close, foot exits, the window unmaps
#     C5 a title set by the client is rasterized (and ellipsized when it does not fit)
#     C6 the frame of the window that takes the keyboard turns active (the other's
#        inactive); a middle click on its title bar lowers it
#     (foot's own trace for C1–C4: set_mode(server_side), configured server_side, no
#     wl_pointer.button at all — every press was on the frame — and xdg_toplevel.close)
#   D  wlr-layer-shell (lumen-probe layer mode; a top panel from D1 stays up through D8)
#     D1 a top panel (zone 32): configured full-width, mapped at 0,0, the usable area starts
#        at y=32; lumen-square maps below it and a Super+drag upwards stops at y=32
#     D2 a background surface with zone -1 is configured to the whole output despite the
#        panel
#     D3 an overlay anchored bottom-right with margins sits exactly at the corner minus them
#     D4 an exclusive overlay takes every key from the focused window, a click on the window
#        does not take them back, and when it unmaps the keys return
#     D5 an on_demand surface on the top layer takes the keys when it maps (sway's rule, which
#        the launcher relies on), keeps them through a click on itself, and loses them to a
#        click on the window
#     D6 maximize fills the usable area exactly and restore returns to the previous rect:
#        foot (server-side frame, title-bar double click; the frame's extents come off) and
#        gtk4-demo (client-side, header-bar double click → xdg_toplevel.set_maximized)
#     D7 an invalid layer state (width 0 without left+right) is the client's protocol error
#        (zwlr_layer_surface_v1 code 1); the compositor carries on
#     D8 a popup of a panel (layer get_popup) flips into the output (constraint: the whole
#        output), grabs without taking the keyboard (interactivity none), and is dismissed
#        by an outside click
#   E  lumen-shell (the real shell over zlm_policy_bridge_v1; lumen-square and foot as windows)
#     E1 the shell binds, gets the snapshot (two windows) and the focus right after it, maps
#        its 32 px top panel through layer-shell (the usable area starts at y=32) and draws
#        two entries with the focused window's highlighted
#     E2 a click on the other entry: bridge request_raise + request_activate, the compositor
#        raises and focuses that window (which receives the activating configure), and the
#        panel's highlight moves to the clicked entry
#     E3 a new foot that retitles itself: it maps below the panel, the entry is added,
#        renamed live, and the redraw damages that entry only
#     E4 a window's client exits: its entry is removed
#     E5 the shell is stopped and started again: the bridge goes, the compositor holds the
#        panel's strip (the zone hold, ADR 011: the usable area never returns to the whole
#        output) until the new panel maps, a new snapshot lists the current windows (in the
#        order the old panel showed them), the panel comes back
#     E6 a copy of lumen-shell at another path is refused at bind (exit 1; the compositor's
#        warning names both paths) and the running shell is unaffected
#   F  clipboard and primary selection (foot and alacritty, real copy and paste; files
#      compared byte for byte)
#     F1 foot → alacritty by keyboard: double-click a word in foot, Ctrl+Shift+C; a new
#        alacritty running `cat > F1.out` is offered the selection as it takes the keyboard,
#        Ctrl+Shift+V, Enter, Ctrl+D: the file holds the word
#     F2 alacritty → foot, the mirror of F1
#     F3 primary by middle click: a word double-clicked in foot (no copy key) is pasted by a
#        middle click on an unfocused alacritty; the primary offer reaches alacritty before
#        the press does. F3r the reverse (alacritty → foot)
#     F4 GTK both ways: text typed in widget-factory and copied with Ctrl+C pastes into
#        foot; text copied in foot pastes into GTK (Ctrl+V) and back out again (WARN when
#        widget-factory's layout puts no text widget at the point)
#     F5 ownership: a second owner cancels the first source (foot's trace), and when the
#        owner exits the focused client is told there is no selection
#     F6 delivery order (foot/alacritty traces): every keyboard enter is immediately
#        preceded by the selection events; an unfocused probe is offered nothing until it
#        is clicked, then reads the selection
#     F7 serials: a set_selection quoting a serial never sent is refused and cancelled; a
#        source set twice is the probe's protocol error (wl_data_device code 1)
#   G  drag-and-drop (lumen-probe dnd-source / dnd-target side by side, deterministic)
#     G1 copy|move, the target prefers move: one drag focus on the target, >=10 motions, drop,
#        the data read, finish; the source sees drop_performed, finished, last action move;
#        no button release reached either client
#     G2 modifiers: Ctrl held → copy; Ctrl then Shift mid-drag → copy, then move
#     G3 a release over nothing cancels (no drop_performed; reason=no_target)
#     G4 Escape over the target cancels (leave, no drop); Escape reaches no client
#     G5 a target that accepts nothing: leave, no drop, cancelled (reason=not_accepted)
#     G6 a drop on the source client's own second window: enter, drop, data, finished
#     G7 a drag with no source: other clients see nothing; the client's own second window
#        gets enter with no offer, then the drop
#     G8 the source's client exits mid-drag: the target is left, the compositor carries on
#        (reason=source_destroyed) and the pointer returns after the release
#     G9 the icon (G1's, 32x32 at offset -16,-16) follows the pointer exactly: every move of
#        its node is an injected position plus the offset (±1 px); it is gone after the drop;
#        no full-output damage during the drag
#     G10 start_drag quoting the enter serial (not the press) is refused and cancelled; the
#        source keeps the pointer and gets its release
#     G11 GTK → foot (WARN-level): text selected in widget-factory dragged onto foot running
#        `cat`; the file holds it
#   H  outputs (wl_output, xdg-output, wl_surface.enter/leave)
#     H1 lumen-probe outputs: wl_output and zxdg_output_v1 report the output's name (the
#        compositor's `output 0 name=` line), mode and logical rect; its toplevel gets enter
#     H2 real clients (foot's traces from A1 and C1): the main surface gets
#        wl_surface.enter, and so does every CSD subsurface that was mapped
#     H3 a wl_output bound a second after mapping replays the enter (the compositor's
#        `output bind … replayed=1`, then the probe's enter)
#     H4 a popup gets enter when it maps and leave when an outside click dismisses it; the
#        toplevel gets leave when it unmaps (null attach)
#     H5 real clients from their own side: a traced foot and a traced gtk4-widget-factory get
#        wl_output (mode, name) and zxdg_output_v1 (logical position 0,0, logical size = the
#        mode, name) equal to the compositor's output, and wl_surface.enter on the toplevel
#     H6 every traced real-client log of the run (foot, alacritty, GTK — A, B6, C, F, G11, H5,
#        I3, J6): the same values, and enter/leave alternate per surface and output starting
#        with enter (no double enter, no leave without enter), naming a bound wl_output
#   I  liveness (xdg_wm_base.ping/pong; the compositor runs with --ping-timeout 1500)
#     I1 a probe with a server-side frame leaves the activation ping unanswered for 4 s:
#        `not-responding`, the title reads "<title> (Not Responding)", its tree is dimmed,
#        lumen-shell's task model gets flags=1; when it answers: `responsive`, the plain title,
#        undimmed, flags=0
#     I2 a probe that never answers: still connected and still receiving clicks after three
#        timeouts; no protocol error, marked not responding exactly once
#     I3 a real hang: foot (server-side frame) stopped with SIGSTOP, clicked (ping
#        reason=press): not responding; 600 pointer motions over it are withheld (it is not
#        disconnected); after SIGCONT it is responsive and its keyboard works
#   J  xdg-activation (tokens, focus-stealing prevention; lumen-probe activation-issuer /
#      activation-target)
#     J1 a token from a click in the focused issuer: `token create … grantable=1`, the target
#        presents it: `result=granted reason=ok`, it gets the keyboard
#     J2 as J1, but the user clicks foot after the token was issued: `result=urgent
#        reason=user_moved_on`, focus stays on foot, lumen-shell gets flags=2 for the target;
#        clicking its panel entry focuses it and the flags go back to 0
#     J3 a token without a serial (--no-serial): `result=urgent reason=no_serial`
#     J4 an unknown token: `result=urgent reason=unknown_token`, no protocol error
#     J5 presented before the window's first commit: `result=pending`, then at map
#        `result=granted … cause=map` and the new window has the keyboard; J5b a launch token
#        the user moved on from (a click elsewhere after it was issued): at map
#        `result=urgent reason=user_moved_on`, the keyboard stays where it was, the window is
#        marked; J5c an unknown token proves nothing: at map `result=granted reason=new_window`
#     J6 real foot started with XDG_ACTIVATION_TOKEN from the issuer: granted, and foot logs
#        nothing about XDG activation (it warned while the global was missing)
#   S  wp_viewporter and wp_fractional_scale_v1 at the run's scale (1; the fractional
#      scales are scripts/scale-smoke.sh's, which restarts the compositor per scale)
#     S1 lumen-probe scale: preferred_scale 120 arrives before the first configure, the
#        client renders a buffer exactly its viewport destination (1:1), the compositor's
#        `surface geometry` line agrees, and hit-testing ends at the surface's last pixel
#        (the probe sees its last column, then leave one pixel further)
#     S2 a viewport crop scaled up: --buffer 200x100 cropped to its top-right quadrant
#        (100,0 100x50) and scaled to 300x150 maps at 300x150; a screenshot of the frame
#        (SIGUSR1) shows the quadrant's green, its white frame at the edges, nothing of the
#        neighbouring quadrants
#     S3 real clients: no foot log of the run warns that fractional scaling is missing;
#        GTK's trace (H5) binds wp_fractional_scale_manager_v1 and wp_viewporter and is told
#        preferred_scale 120
#   P  wp_presentation (lumen-probe presentation: one wp_presentation_feedback per commit)
#     P1 a client drawing continuously, paced by frame callbacks (180 frames): clock_id is
#        CLOCK_MONOTONIC; every frame is presented (none discarded, none left pending), each
#        preceded by sync_output for its wl_output; refresh is the output mode's period;
#        flags always carry vsync and hw_completion, and hw_clock exactly when the driver
#        gives a vblank counter (seq > 0, then strictly increasing; vmwgfx has none: seq 0);
#        presentation times strictly increase, no faster than the refresh, after the commit
#     P2 superseded commits: three commits per frame (--burst 3, 90 commits): the first two
#        of every frame are discarded, the last presented
#     P3 occlusion: a maximized opaque probe covers an animating one; while covered the
#        lower one presents nothing (its updates are not on screen), its frame callbacks are
#        throttled to about one a second and each throttled commit discards the previous;
#        uncovered, it presents again at the display rate
#   K  idle, inhibition and display power (lumen-probe idle / inhibit / power)
#     K1 ext_idle_notification_v1 with a 1.5 s timeout: idled 1.5 s after it was created
#        (no input meanwhile), resumed by an injected motion, idled again 1.5 s later
#     K2 a visible zwp_idle_inhibitor_v1 holds an inhibitor-obeying notification back (an
#        input-only one, v2, idles regardless); covered by a maximized opaque window the
#        inhibitor is not honoured and the notification idles 1.5 s after the cover mapped
#     K3 zwlr_output_power_v1 off: the CRTC goes off (ACTIVE=0), nothing is composited or
#        committed while dark; an injected key wakes it — the key reaches no client (nor its
#        release), the next key does — with one full repaint, and every power object hears
#        `mode off` then `mode on`
#   L  session lock (lumen-probe lock: a minimal locker; ext-session-lock-v1)
#     L1 a locker with a lock surface: input leaves the focused window at the lock request,
#        `locked` follows the first locked frame (awaiting → locking → locked, within 0.5 s),
#        the lock surface takes the keyboard; keys and a click reach the lock surface only (the
#        window sees neither); a wrong test password is refused, the right one unlocks, and
#        the window has the keyboard again (its next key reaches it)
#     L2 the locker dies while locked: the session stays locked (abandoned, dark red over the
#        whole output in a screenshot), keys reach nobody; a new locker takes over and is
#        `locked` at once, then unlocks
#     L3 a locker without lock surfaces: `locked` after the compositor's 1 s wait, the output
#        a solid colour (screenshot black)
#     L4 logind's Unlock (loginctl unlock-session): the live locker is sent `finished`,
#        answers with unlock_and_destroy, the session is unlocked
#   M  text input and input methods (lumen-probe text-input / im: text-input-v3 v2,
#      input-method-v2, virtual-keyboard-v1; foot and GTK 4 as real text inputs)
#     M1 the probe window's text input is entered once the input method binds and enables;
#        the input method is activated with the window's state; two KEY_A presses go to the
#        input method's grab (never to the window) and come back as preedit "α", "αα"; space
#        commits "αα" (done carries the window's commit count); KEY_B is forwarded through the
#        virtual keyboard and reaches the window. The popup is shown exactly below the cursor
#        rectangle (window origin + 20,40+18; v2: once the window's surface commit applied
#        it), told text_input_rectangle 0,-18 2x18, and is on screen (screenshot)
#     M2 focus moves to another window: the text input is left, the input method deactivated,
#        the popup hidden, and KEY_C reaches the other window directly (the grab never sees
#        it); the other window goes: entered, activated, the popup back, KEY_D forwarded
#     M3 the session locks while the text input is active: left, deactivated, the popup
#        hidden, KEY_E reaches the lock surface only; unlocked: entered and active again
#     M4 foot: no "text input" warning; the probe input method composes "αα" into foot
#        running `cat` (foot is sent preedit_string "α"/"αα" and commit_string "αα"), keys
#        typed after it arrive through the virtual keyboard — the file holds "ααxyz"
#     M5 GTK 4 (gtk4-demo entry_undo, WARN-level): binds text input v2, gets preedit "αα"
#        and commit_string "αα" from the probe input method
#   T  commit-path protocols (design-app-compat-capture Part A)
#     T1 vkcube --present_mode 2 (Mesa's Vulkan WSI FIFO on lavapipe): binds wp_fifo and
#        sends set_barrier + wait_barrier per present; it never commits faster than the
#        refresh (WARN when the compositor never had to hold an update: lavapipe slower
#        than the display)
#     T2 weston-simple-timing: commit-timing targets queued by the compositor, no error
#     T3 lumen-probe timing: 120 FIFO updates committed back to back are all presented, one
#        per refresh (none discarded, no interval under half a refresh); 20 timed updates
#        are presented no earlier than 0.5 ms before their target and at most a refresh late
#     T4 lumen-probe solid: a single-pixel green buffer sized 320x240 by a viewport, alpha
#        one half, type video — the compositor applies it as a solid (no texture), the
#        alpha and the content type reach the scene, and a screenshot shows half green
#     T5 lumen-probe tearing: a fullscreen window with the async hint — the hint is applied,
#        and on a card without async flips the compositor says so and presents with vsync
#     T6 weston-simple-egl (GL client, dma-buf with implicit fences): frames keep coming;
#        reports how often an update had to wait for its buffer's fences
#   U  window protocols (design-app-compat-capture Part B; lumen-shell runs for U and V)
#     U1 lumen-probe dialog: a modal child (xdg-dialog-v1) maps centred on its parent; a click
#        on the parent is consumed (the probe sees no button) and the dialog keeps the keyboard
#     U2 GTK 4's modal message dialog (gtk4-demo Dialogs, Alt+M; WARN-level): GTK sends
#        get_xdg_dialog + set_modal, a click on the parent is consumed
#     U3 xdg-toplevel-icon: foot's icon name and the probe's name with 16+32 px buffers reach
#        the shell (its bridge's `icon handle=` lines); foot does not warn about the protocol
#     U4 xdg-system-bell: foot's \a (bell.system=yes) and six probe rings 50 ms apart reach
#        the shell (4 per second per client pass), which flashes the window (an overlay layer
#        surface `lumen-bell`); foot does not warn about the protocol
#     U5 xdg-foreign: a v2 and a v1 export imported by another client (set_parent_of; the v2
#        child modal), each import told `destroyed` when its exporter exits
#   V  keyboard, data, sandbox
#     V1 keyboard-shortcuts-inhibit (lumen-probe shortcuts-inhibit): Super and Super+Return
#        reach the probe and neither the shell's tap nor a grab fires; Ctrl+Alt+F2 still
#        switches VT (paused, chvt back, resumed); Super+Escape turns the inhibitor off
#        (`inactive`), after which the Super tap opens the launcher (Escape closes it)
#     V2 ext-data-control with wl-clipboard: wl-copy → wl-paste (clipboard and primary),
#        cliphist stores every selection through `wl-paste --watch`, foot pastes a wl-copy
#        selection into a file (Ctrl+Shift+V), a second wl-copy cancels the first
#     V3 security-context (lumen-probe security-context): a client through the engine's
#        listener is sandboxed and sees none of the session-wide globals (data control, layer
#        shell, lock, output management and power, idle notifier, input method, virtual
#        keyboard, the security-context manager itself); the socket stops when its close fd
#        hangs up
#   W  pointer protocols (design-app-compat-capture Part C; relative motion injected on the
#      VMware mouse's REL_X/REL_Y node, LUMEN_INJECT_RELATIVE, default /dev/input/event3)
#     W1 lumen-probe constraints (persistent lock): locked once the pointer is on the focused
#        window; 20 relative motions arrive with their unaccelerated deltas (10,-5 device
#        units) and an absolute-device move arrives as a relative delta of the same size —
#        and no wl_pointer.motion, the pointer staying where it was (every re-entry and the
#        first motion after the release are at the lock position). Deactivated by focus loss
#        (lumen-square takes the keyboard), a VT switch (Ctrl+Alt+F2) and the session lock
#        (lumen-probe lock), and holding again after each; Super+Escape releases it
#     W2 lumen-probe constraints --confine --region 40,40,200,120: confined inside the region;
#        absolute moves out of it and relative motion stop at its edges (one wl_fixed short
#        of the far ones); Super+Escape unconfines
#     W3 lumen-probe constraints --oneshot --hint 60,50: released by Super+Escape the pointer
#        is warped to the hint (wl_pointer.motion 60,50); a spent oneshot never locks again
#     W4 weston-constraints (WAYLAND_DEBUG=client): a click confines (motion clamped inside the
#        surface), the next locks (relative_motion, no wl_pointer.motion), the next unlocks
#     W5 pointer gestures: lumen-probe gestures binds v3; the compositor's device list says
#        whether any device can make gestures (a VM has no touchpad: bind only)
#   Y  screen capture (design-app-compat-capture Part D; grim, wf-recorder, lumen-probe)
#     Y1 grim -o: the PNG is the output's size and, pixel for pixel, the frame the compositor
#        reads back itself (SIGUSR1); the probe window's colour at its place
#     Y2 grim -T of a window covered by another: its own pixels, at its own size
#     Y3 grim -c paints the pointer, grim without it does not (the pointer is a plane)
#     Y4 lumen-probe capture of an animating client (dma-buf and shm): the first frame full,
#        later ones only the moving square's damage; the window's colour where nothing moves
#     Y5 a covered window's screencast keeps its frames coming (paced like a shown window,
#        not the 1 s throttle of covered windows)
#     Y6 the session lock: a capture from the request on is the black curtain, no frame while
#        locked, the desktop again after unlock
#     Y7 wf-recorder (wlr-screencopy): a 3 s recording of the output has frames
#     Y8 the toplevel list (ext-foreign-toplevel-list) and a pointer cursor session
#   Z  portals (design-app-compat-capture Part E; scripts/portal-session.sh sets up
#      xdg-desktop-portal for the run — the user manager's environment and the frontend are
#      restored afterwards; lumen-portal-probe is the application)
#     Z1 Screenshot (xdg-desktop-portal-lumen, non-interactive): a private PNG of the
#        output's size, pixel for pixel the compositor's own read-back
#     Z2 Screenshot through xdg-desktop-portal, interactive: a dragged area over a red
#        window is a PNG of exactly the dragged size, all red, in the pictures directory
#     Z3 interactive, the other ways out: Enter → every output, a click → that output,
#        Escape → response 1 (cancelled)
#     Z4 PickColor through the frontend: a click on the red window → (1, 0, 0)
#     Z5 ScreenCast through the frontend (xdg-desktop-portal-wlr): Lumen's source chooser
#        lists the output then the windows, Enter shares the output; the PipeWire stream (the
#        probe offers dma-buf, then shm) has the output's size, frames at a moving client's
#        pace, the red window's pixels
#     Z6 ScreenCast of a window chosen in the chooser: the window's size
#     Z7 Screenshot of a window (target Window: the list, Enter): the window's own size
#     Z8 the session locked: a screenshot is refused (response 2), never the lock screen;
#        after unlock it works again
#   R  the configuration file (design-session-and-wm Part C; the compositor runs with
#      LUMEN_CONFIG=<logs>/R-config/config.toml, absent until R writes it; lumen-shell and
#      lumen-probe keyboard as clients)
#     R1 the file appears (by rename): repeat 10/600 and a us,de keymap reach a new client, the
#        mouse is set flat 0.5 with natural scrolling, the cursor 48 px; Super+Y (configured
#        as window.close) closes the focused window
#     R2 grp:alt_shift_toggle: y, Alt+Shift, y types y then z, as the client's own xkb sees it
#     R3 a live edit: repeat 30 reaches the client (debounce 100 ms), one generation, no notice
#     R4 the mouse: a wheel notch up scrolls positive (natural), doubled (scroll-factor 2),
#        relative motion x1.5 (flat 0.5)
#     R5 a syntax error: rejected, nothing resent, `lumen-ctl reload` exits 1 naming the line,
#        the shell's panel shows one notice
#     R6 a bad value keeps that key's value, an unknown action suggests the right one; the file
#        removed: the defaults return (repeat 25/600)
#   Q  workspaces (design-session-and-wm Part W; lumen-shell runs, lumen-probe window reports
#      every configure's states)
#     Q1 three windows on workspace 1, Super+2: all hidden and told `suspended`, nothing has
#        the keyboard, lumen-probe workspaces sees 2 active; Super+1: shown again, the window
#        focused there has the keyboard back (focus memory)
#     Q2 Super+Shift+2 moves the focused window to 2 and follows (move-follows): it is shown,
#        the others hidden; lumen-shell's indicator logs `1+ 2*+ 3`
#     Q3 lumen-probe workspaces --activate 1 (ext-workspace activate + commit): 1 active
#     Q4 a fullscreen window: on its workspace a screenshot is its colour; Super+2: neither
#        it nor its black backdrop shows (the background does); Super+1: it again
#     Q5 Super+H minimizes the focused window (hidden, `suspended`); a click on its panel
#        entry brings it back with the keyboard; xdg_toplevel.set_minimized (lumen-probe
#        window --minimize-after) minimizes too
#     Q6 lumen-probe capture of a window on a hidden workspace: frames keep coming, the
#        window's own pixels
#     Q7 the session lock: Super+2 and an ext-workspace activate while locked change nothing
#   O  switching and snapping (design-session-and-wm Part K; lumen-shell runs, three
#      lumen-probe windows report their configures and what their keyboards are told)
#     O1 a quick Alt+Tab (released within the 150 ms open delay): the previous window gets the
#        keyboard and the switcher is never drawn
#     O2 Alt held, Tab, wait, Tab, Alt up: the switcher is drawn (centred, at the output's
#        scale: its colours in a screenshot), the third MRU window gets the keyboard
#     O3 Alt+Tab, Escape: nothing changes; the focused window saw only Alt (no leave)
#     O4 Alt+Shift+Tab: backwards, the least recently used window
#     O5 Super+Left/Up/Down/Right/Up/Down: half, quarter, half (tiled states), restored;
#        maximized, restored to the same place — each as the window's configures say
#     O6 Super+drag to the left edge: the preview (its border and fill as composited, from a
#        screenshot), the release snaps to the left half; dragged out: restored to its size
#     O7 Alt+F7, arrows, Return: the window moves by the arrows' steps (10 px)
#     O8 Alt+F8: the first arrow of an axis picks that edge; Return applies
#     O9 the session locks during a hold: cancelled, the switcher closes, nothing activated
#     O10 no shell: Alt+Tab is the compositor's quick switch
#     O11 a click on a window during a hold cancels it (that window keeps the keyboard)
#   N  notifications and the tray (design-session-and-wm Part N; lumen-shell runs with its
#      state in this run's directory; notify-send, lumen-probe sni, dbus-monitor, gdbus,
#      busctl)
#     N1 notify-send -t 1500 expires (NotificationClosed reason 1); a critical one stays and
#        lifts the popups to the overlay layer; CloseNotification closes it (3), an unknown
#        id is an error
#     N2 replaces_id (notify-send -r): the same id, the card replaced in place
#     N3 a click on an action button: ActivationToken, ActionInvoked, NotificationClosed(2) in
#        that order (dbus-monitor), notify-send prints the action; a click on a card without
#        a default action dismisses it
#     N4 locked (lumen-probe lock): a notification waits, pops up after the unlock, expires
#     N5 do not disturb while the screen is captured (lumen-probe capture): normal ones to
#        the history, critical ones still shown; a middle click on the panel's bell sets and
#        clears it
#     N6 lumen-probe sni in the panel (its pixmap in a screenshot), the tooltip, a left click
#        (ProvideXdgActivationToken then Activate with that token), busctl on the watcher, a
#        right click opens its com.canonical.dbusmenu menu (a grabbing popup), Down+Return
#        clicks Open, the wheel scrolls it
#     N7 the shell killed and started again: the item registers with the new watcher within
#        a second; notification ids continue after the persisted block
#     N8 idle with a history and a tray item: lumen-shell has no wakeups
#   X  robustness: lumen-square --sub --destroy-parent-first tears the parent wl_surface
#      down before the strip's wl_subsurface; no protocol error, compositor alive
#
# Global criteria: no ERROR line and no WARN except the 600-frame budget summaries and host
# present stalls (the 250 ms present watchdog with a flip already submitted, reported as a
# WARN row: SVGA3D flip/fence latency, as drag-smoke's g; without a submitted flip it
# fails); no
# "unregistered texture", "shm staging returned no buffer" or "texture update queue full";
# every node in every tree dump has a positive size; every mapped subsurface touches the
# output; the compositor exits 0 on SIGTERM and the console is back in text mode. Protocol
# errors on either side: exactly the provoked ones (D7, E6, F7r) — libwayland-server's "error
# in client communication (pid N)" for those PIDs only, and no client log with a
# libwayland-client error line, GTK's "Protocol error"/"Lost connection" or foot's "err:".
#
# Exit status: 0 PASS (possibly with warnings), 1 FAIL, 2 could not run, 3 INVALID (input
# the injector did not send was seen — re-run hands off).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SQUARE="${BUILD}/lumen-square"
PROBE="${BUILD}/lumen-probe"
PORTAL_PROBE="${BUILD}/lumen-portal-probe"
SHELL_BIN="${BUILD}/lumen-shell"
INJ="${ROOT}/scripts/inject-input.py"
POINTER_DEV="${LUMEN_INJECT_POINTER:-/dev/input/event2}"
KEYBOARD_DEV="${LUMEN_INJECT_KEYBOARD:-/dev/input/event1}"
RELATIVE_DEV="${LUMEN_INJECT_RELATIVE:-/dev/input/event3}"

SOCKET="lumen-app-smoke"
LOG_DIR="${ROOT}/app-smoke-logs"
JOIN_SEAT=0
COUNTDOWN=3
ONLY="A,B,C,D,E,F,G,H,I,J,S,P,K,L,M,T,U,V,W,X,Y,Z,R,Q,O,N"
SKIP_GTK=0
SCALE=1
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --countdown) COUNTDOWN="${2:?--countdown needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --only) ONLY="${2:?--only needs a list}"; shift ;;
    --skip-gtk) SKIP_GTK=1 ;;
    --scale) SCALE="${2:?--scale needs a value}"; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
want() { [[ ",${ONLY}," == *",$1,"* ]]; }
for g in ${ONLY//,/ }; do
  case "$g" in
    A|B|C|D|E|F|G|H|I|J|S|P|K|L|M|T|U|V|W|X|Y|Z|R|Q|O|N) ;;
    *) echo "unknown group '$g' (see --help)"; exit 2 ;;
  esac
done
if [[ "$SCALE" != 1 && "$ONLY" != O ]]; then
  echo "--scale is for group O only (--only O)"; exit 2
fi
SCALE_ARG=""
[[ "$SCALE" != 1 ]] && SCALE_ARG="$SCALE"

# Every lumen-shell of this run keeps its state (the next notification id, do-not-disturb)
# here, never in the user's $XDG_RUNTIME_DIR/lumen/shell-state.
export LUMEN_SHELL_STATE="${LOG_DIR}/shell-state"
COMP_LOG="${LOG_DIR}/compositor.log"
INJ_LOG="${LOG_DIR}/inject.jsonl"
SCEN_LOG="${LOG_DIR}/scenarios.jsonl"
WATCH_LOG="${LOG_DIR}/watch.jsonl"
RUN_LOG="${LOG_DIR}/run.log"

mkdir -p "$LOG_DIR"
for f in "$COMP_LOG" "$INJ_LOG" "$SCEN_LOG" "$WATCH_LOG"; do : > "$f"; done
rm -f "$LOG_DIR"/foot*.log "$LOG_DIR"/gtk*.log "$LOG_DIR"/square*.log "$LOG_DIR"/probe*.log \
  "$LOG_DIR"/shell*.log "$LOG_DIR"/fake-shell* "$LOG_DIR"/alacritty*.log "$LOG_DIR"/F*.out \
  "$LOG_DIR"/J*.token "$LOG_DIR"/J*.go "$LOG_DIR"/lumen-screenshot-*.ppm

exec > >(tee "$RUN_LOG") 2>&1
TEE_PID=$!
close_log() {
  if [[ -n "${TEE_PID:-}" ]]; then
    exec >&- 2>&-
    wait "$TEE_PID" 2>/dev/null
    TEE_PID=""
  fi
}
trap close_log EXIT

# ── Preconditions ────────────────────────────────────────────────────────────
for bin in "$COMP" "$SQUARE" "$PROBE" "$SHELL_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: $bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done
command -v foot >/dev/null || { echo "FAIL: foot is not installed (group A needs it)"; exit 2; }
if want F && ! command -v alacritty >/dev/null; then
  echo "FAIL: alacritty is not installed (group F needs it)"; exit 2
fi
if want N; then
  for tool in notify-send dbus-monitor gdbus busctl; do
    command -v "$tool" >/dev/null ||
      { echo "FAIL: $tool is not installed (group N needs it; notify-send is libnotify's)"; exit 2; }
  done
fi
if (( ! SKIP_GTK )) && ! command -v gtk4-widget-factory >/dev/null; then
  echo "NOTE: gtk4-widget-factory not installed; GTK scenarios skipped"; SKIP_GTK=1
fi

newest_of() {
  { git -C "$ROOT" ls-files -z -- "$@"
    git -C "$ROOT" ls-files -z --others --exclude-standard -- "$@"; } |
    (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1
}
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  for pair in "$COMP:modules executables/lumen-compositor" "$SQUARE:executables/lumen-square" \
              "$PROBE:tests/clients" "$SHELL_BIN:modules/lx.shell executables/lumen-shell" \
              "$BUILD/xdg-desktop-portal-lumen:modules/lx.portal executables/xdg-desktop-portal-lumen"; do
    [[ -e "${pair%%:*}" ]] || continue
    bin=${pair%%:*}
    # shellcheck disable=SC2086 # the path list is meant to split
    src=$(newest_of ${pair#*:})
    if [[ -n "$src" && "$ROOT/$src" -nt "$bin" ]]; then
      echo "FAIL: $bin is older than $src. Rebuild:  cmake --build --preset release"
      exit 2
    fi
  done
fi

if ! sudo -n true 2>/dev/null; then
  echo "FAIL: the injector writes to evdev nodes as root and needs passwordless sudo"
  exit 2
fi
for dev in "$POINTER_DEV" "$KEYBOARD_DEV"; do
  [[ -c "$dev" ]] || { echo "FAIL: $dev is not an input device node"; exit 2; }
done
if want W && [[ ! -c "$RELATIVE_DEV" ]]; then
  echo "FAIL: $RELATIVE_DEV is not an input device node (group W injects relative motion there;"
  echo "      set LUMEN_INJECT_RELATIVE to a node with REL_X/REL_Y)"
  exit 2
fi

SEAT_SCOPE=""
sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
if [[ -n "$sid" && "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" == "$(id -u)" ]]; then
  scope=$(loginctl show-session "$sid" -p Scope --value 2>/dev/null || true)
  [[ -n "$scope" ]] && SEAT_SCOPE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/${scope}"
fi
MY_CGROUP="/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)"
IN_SEAT=0
[[ -n "$SEAT_SCOPE" && "$MY_CGROUP" == "$SEAT_SCOPE" ]] && IN_SEAT=1
if (( ! IN_SEAT )); then
  if (( JOIN_SEAT )); then
    if [[ -z "$SEAT_SCOPE" || ! -e "$SEAT_SCOPE/cgroup.procs" ]]; then
      echo "FAIL: --join-seat-session: no active seat0 session owned by $(id -un)"
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      libinput will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another --socket"
  exit 2
fi
rm -f "$SOCKET_PATH" "$LOCK_PATH"

# ── Processes and cleanup ────────────────────────────────────────────────────
# shellcheck source=portal-session.sh
source "$ROOT/scripts/portal-session.sh"
# Every client PID this script starts, so cleanup signals exactly those (never a pattern).
COMP_PID=""; WATCH_PID=""; CLIENT_PIDS=()
COMP_RC="not-stopped"

stop_compositor() {
  [[ -n "$COMP_PID" ]] || return 0
  kill -TERM "$COMP_PID" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$COMP_PID" 2>/dev/null; then
    echo "WARN: compositor ignored SIGTERM for 5 s; killing it (console may stay graphical)"
    kill -9 "$COMP_PID" 2>/dev/null
  fi
  wait "$COMP_PID" 2>/dev/null
  COMP_RC=$?
  COMP_PID=""
}
stop_watch() {
  [[ -n "$WATCH_PID" ]] || return 0
  kill -TERM "$WATCH_PID" 2>/dev/null
  wait "$WATCH_PID" 2>/dev/null
  WATCH_PID=""
}
stop_client() { # pid
  [[ -n "$1" ]] || return 0
  kill -TERM "$1" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$1" 2>/dev/null && kill -9 "$1" 2>/dev/null
  wait "$1" 2>/dev/null
}
cleanup() {
  local pid
  for pid in "${CLIENT_PIDS[@]}"; do stop_client "$pid"; done
  CLIENT_PIDS=()
  portal_session_stop >>"$LOG_DIR/portals.log" 2>&1
  stop_compositor
  stop_watch
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
  close_log
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ── Helpers ──────────────────────────────────────────────────────────────────
lines() { wc -l < "$1" | tr -d ' '; }
wait_for() { # file pattern seconds
  local deadline=$(( $(date +%s%N) + ${3}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    grep -qE "$2" "$1" && return 0
    sleep 0.05
  done
  return 1
}
geom() { # last geometry of toplevel $1: "x y w h"
  grep -E "compositor.wm: (geometry|map) id=$1 " "$COMP_LOG" | tail -1 |
    sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
settle() { # the toplevel's configure/ack/geometry traffic is quiet and the last one acked
  local id=$1 prev="" cur last deadline=$(( $(date +%s%N) + 5000000000 ))
  while (( $(date +%s%N) < deadline )); do
    cur=$(grep -cE "compositor.(wm|scene): (configure|ack|geometry|grab end|tree root=[0-9]+ kind=toplevel) id=$id " "$COMP_LOG")
    last=$(grep -oE "compositor.wm: configure id=$id serial=[0-9]+" "$COMP_LOG" | tail -1 |
           sed 's/.*serial=//')
    if [[ "$cur" == "$prev" ]] && { [[ -z "$last" ]] ||
         grep -qE "compositor.wm: ack id=$id serial=$last\$" "$COMP_LOG"; }; then
      return 0
    fi
    prev=$cur
    sleep 0.3
  done
  echo "WARN: toplevel $id did not settle within 5 s"
  return 1
}
wait_map() { # app_id seconds — prints the id of a toplevel with that app_id mapped since
             # the current scenario began (earlier windows of the same client do not count)
  local deadline=$(( $(date +%s%N) + ${2}000000000 )) id
  while (( $(date +%s%N) < deadline )); do
    id=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" |
         grep -oE "compositor.wm: map id=[0-9]+ app_id=$1 " | tail -1 | sed -E 's/.*id=([0-9]+).*/\1/')
    [[ -n "$id" ]] && { echo "$id"; return 0; }
    sleep 0.05
  done
  echo 0
  return 1
}
start_client() { # log-name command...
  local log="$LOG_DIR/$1.log"; shift
  WAYLAND_DISPLAY="$SOCKET" "$@" >"$log" 2>&1 &
  LAST_PID=$!
  CLIENT_PIDS+=("$LAST_PID")
}
forget_client() { # pid: stopped, drop it from the cleanup list
  local keep=() p
  for p in "${CLIENT_PIDS[@]}"; do [[ "$p" == "$1" ]] || keep+=("$p"); done
  CLIENT_PIDS=("${keep[@]}")
}

SCEN=""
inject() {
  sudo -n python3 -I "$INJ" --pointer "$POINTER_DEV" --keyboard "$KEYBOARD_DEV" \
    --relative "$RELATIVE_DEV" --output "${OUT_W}x${OUT_H}" --tag "$SCEN" "$@" >>"$INJ_LOG"
}
BEGIN_COMP=0
begin() { SCEN=$1; BEGIN_COMP=$(lines "$COMP_LOG"); }
finish() { # key=value ... — numeric parameters the verdict needs
  local extra="" kv
  for kv in "$@"; do extra+=",\"${kv%%=*}\":${kv#*=}"; done
  printf '{"name":"%s","comp":[%s,%s]%s}\n' "$SCEN" "$BEGIN_COMP" "$(lines "$COMP_LOG")" \
    "$extra" >>"$SCEN_LOG"
}

# ── Start ────────────────────────────────────────────────────────────────────
echo "┌──────────────────────────────────────────────────────────────────────────┐"
echo "│ HANDS OFF for about 6 minutes: do not touch the mouse or keyboard; keep  │"
echo "│ the host pointer OUTSIDE the VM window. Real input mixes with injected   │"
echo "│ input on the same device; if any is seen, the run is reported INVALID.   │"
echo "└──────────────────────────────────────────────────────────────────────────┘"
for (( n = COUNTDOWN; n > 0; n-- )); do printf '     starting in %d ...\r' "$n"; sleep 1; done
(( COUNTDOWN > 0 )) && printf '%-30s\r' ""

sudo -n python3 -I "$INJ" watch >"$WATCH_LOG" 2>&1 &
WATCH_PID=$!
if ! wait_for "$WATCH_LOG" '"kind": "watch"' 3; then
  echo "FAIL: the input watcher did not start:"; cat "$WATCH_LOG"; exit 2
fi

# The configuration file of this run (group R writes it; absent: every default, and never
# the user's own ~/.config/lumen/config.toml).
R_CFG_PATH="$LOG_DIR/R-config/config.toml"
rm -rf "$LOG_DIR/R-config"
echo "1/2  compositor on $SOCKET (LUMEN_LOG=debug) ..."
if (( ! IN_SEAT && JOIN_SEAT )); then
  bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$4" LUMEN_CONFIG="$5" "$2" "$3" --ping-timeout 1500 ${6:+--scale "$6"}' \
    _ "$SEAT_SCOPE" "$COMP" "$SOCKET" "$LOG_DIR" "$R_CFG_PATH" "$SCALE_ARG" >"$COMP_LOG" 2>&1 &
else
  LUMEN_LOG=debug LUMEN_SCREENSHOT_DIR="$LOG_DIR" LUMEN_CONFIG="$R_CFG_PATH" "$COMP" "$SOCKET" --ping-timeout 1500 ${SCALE_ARG:+--scale "$SCALE_ARG"} \
    >"$COMP_LOG" 2>&1 &
fi
COMP_PID=$!
if ! wait_for "$COMP_LOG" "output 0 mode=" 10 || ! wait_for "$COMP_LOG" "libinput seat opened" 3; then
  echo "FAIL: the compositor did not report an output mode and an input seat."
  grep -E "ERROR|WARN|seat|output" "$COMP_LOG" | tail -15
  exit 2
fi
for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
[[ -S "$SOCKET_PATH" ]] || { echo "FAIL: no socket at $SOCKET_PATH"; exit 2; }
read -r OUT_W OUT_H < <(grep -m1 -oE "output 0 mode=[0-9]+x[0-9]+" "$COMP_LOG" |
  sed -E 's/.*mode=([0-9]+)x([0-9]+)/\1 \2/')
echo "     output ${OUT_W}x${OUT_H}"
SCEN=setup; inject move 20 $((OUT_H - 20))

echo "2/2  scenarios ..."

if want A; then
  # A1 — foot with client-side decorations (no decoration manager is advertised).
  begin A1
  WAYLAND_DEBUG=client start_client foot-csd foot --config=/dev/null -o csd.preferred=client
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  finish id="$FOOT_ID"

  if (( FOOT_ID > 0 )); then
    # A2 — drag the title bar (a subsurface) by (+150,+80): foot answers with move.
    begin A2
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 3)); PY=$((Y + 12))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX + 150)) $((PY + 80)) --button left --steps 60
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=150 dy=80

    # A2s — Super+drag (the compositor's own move): exact, and the whole tree follows.
    begin A2s
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 2)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX - 120)) $((PY - 60)) --button left --super --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=-120 dy=-60

    # A3 — resize from the bottom border subsurface near the right corner, (+120,+90).
    # foot's border subsurfaces lie just outside its window geometry.
    begin A3
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W + 2)); PY=$((Y + H + 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX + 120)) $((PY + 90)) --button left --steps 50
    settle "$FOOT_ID"
    finish id="$FOOT_ID"
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # A4 — lumen-square with a synchronized strip, resized; then with a desynchronized one.
  for mode in sub sub-desync; do
    begin "A4-$mode"
    start_client "square-$mode" "$SQUARE" "--$mode"
    SQ_PID=$LAST_PID
    if SQ_ID=$(wait_map lumen-square 5); then
      settle "$SQ_ID"
      read -r X Y W H < <(geom "$SQ_ID")
      PX=$((X + W - 3)); PY=$((Y + H - 3))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX + 150)) $((PY + 100)) --button left --steps 50
      settle "$SQ_ID"
    fi
    stop_client "$SQ_PID"; forget_client "$SQ_PID"
    finish id="$SQ_ID"
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # A5 — GTK 4 maps and stays up.
  if (( ! SKIP_GTK )); then
    begin A5
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-factory gtk4-widget-factory
    GTK_PID=$LAST_PID
    if GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$GTK_ID"
      sleep 1
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID" alive="$ALIVE"
  fi
fi

popup_map() { # surface — "x y w h" of the newest map of that popup
  grep -E "compositor.popup: popup map surface=$1 " "$COMP_LOG" | tail -1 |
    sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
newest_popup() { # the surface id of the newest popup mapped since the scenario began
  tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -oE "compositor.popup: popup map surface=[0-9]+" |
    tail -1 | sed 's/.*=//'
}
wait_popup_maps() { # count seconds — at least `count` popup maps since the scenario began
  local deadline=$(( $(date +%s%N) + ${2}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    (( $(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -c "compositor.popup: popup map ") >= $1 )) && return 0
    sleep 0.05
  done
  return 1
}

if want B; then
  # B1 — the unadjusted placement: bottom_left anchor, bottom_right gravity.
  begin B1
  start_client probe-b1 "$PROBE" popup --size 400x300 --popup 200x100 --anchor-rect 10,10,50,20 \
    --anchor 6 --gravity 8 --adjust 0 --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    wait_popup_maps 1 3
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID"

  # B2 — the window's right edge 50 px from the output's, a 200 px popup opening rightwards
  # off the anchor rect at its right edge: flip, slide or resize must bring it back.
  for adj in 4 1 16; do
    begin "B2-$adj"
    start_client "probe-b2-$adj" "$PROBE" popup --size 400x300 --popup 200x100 \
      --anchor-rect 390,10,10,20 --anchor 4 --gravity 4 --adjust "$adj" --exit-after 30
    PR_PID=$LAST_PID
    if PR_ID=$(wait_map lumen-probe 5); then
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      PX=$((X + W / 2)); PY=$((Y + H / 2)); DX=$(( OUT_W - 50 - (X + W) ))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX + DX)) "$PY" --button left --super --steps 30
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
      inject button left click
      wait_popup_maps 1 3
    fi
    stop_client "$PR_PID"; forget_client "$PR_PID"
    finish id="$PR_ID" adjust="$adj"
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # B3 — three nested grabbing popups, then a click on the empty desktop.
  begin B3
  start_client probe-b3 "$PROBE" popup --size 400x300 --popup 200x100 --anchor-rect 10,10,50,20 \
    --anchor 6 --gravity 8 --chain 3 --grab --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    for n in 1 2; do
      wait_popup_maps "$n" 3
      read -r PX PY PW PH < <(popup_map "$(newest_popup)")
      inject move $((PX + PW / 2)) $((PY + PH / 2)); sleep 0.2
      inject button left click
    done
    wait_popup_maps 3 3
    sleep 0.3
    inject key KEY_A tap
    sleep 0.2
    OUTSIDE_LINE=$(lines "$COMP_LOG")
    inject move 20 $((OUT_H - 20)); sleep 0.2
    inject button left click
    sleep 0.8
    inject key KEY_A tap
    sleep 0.3
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID" outside="${OUTSIDE_LINE:-0}"

  # B4 — a grab quoting the enter serial, not the press that opened the popup.
  begin B4
  start_client probe-b4 "$PROBE" popup --grab-stale --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    sleep 0.8
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID"

  # B5 — Super+drag ends a popup grab; then a non-grabbing popup travels with its window.
  for mode in grab follow; do
    begin "B5-$mode"
    extra=(); [[ $mode == grab ]] && extra=(--grab)
    start_client "probe-b5-$mode" "$PROBE" popup --size 400x300 --popup 200x100 \
      --anchor-rect 10,10,50,20 --anchor 6 --gravity 8 "${extra[@]}" --exit-after 30
    PR_PID=$LAST_PID
    if PR_ID=$(wait_map lumen-probe 5); then
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      PX=$((X + W / 2)); PY=$((Y + H / 2))
      inject move "$PX" "$PY"; sleep 0.2
      inject button left click
      wait_popup_maps 1 3
      sleep 0.2
      # Inside the window but clear of the popup (which opens at its top-left).
      PX=$((X + W - 40)); PY=$((Y + H - 40))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX - 60)) $((PY - 40)) --button left --super --steps 30
      settle "$PR_ID"
      sleep 0.3
    fi
    stop_client "$PR_PID"; forget_client "$PR_PID"
    finish id="$PR_ID" dx=-60 dy=-40
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # B6 — GTK context menus. The point is a text widget in widget-factory's first page at the
  # window size this output gives it; if GTK lays out differently the checks only WARN.
  if (( ! SKIP_GTK )); then
    begin B6
    WAYLAND_DEBUG=client GDK_BACKEND=wayland GSK_RENDERER=cairo \
      start_client gtk-factory-b6 gtk4-widget-factory
    GTK_PID=$LAST_PID
    B6_LINES=()
    if GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$GTK_ID"
      sleep 1
      read -r X Y W H < <(geom "$GTK_ID")
      # (rel x, rel y, then): esc = open and Escape; out = open and click outside;
      # the last two open near the output's bottom and right edges.
      for spec in "1376 529 esc" "1376 529 out" "1376 579 esc" "$((OUT_W - X - 164)) 529 esc"; do
        read -r RX RY THEN <<<"$spec"
        B6_LINES+=("$(lines "$COMP_LOG")")
        inject move $((X + RX)) $((Y + RY)); sleep 0.3
        inject button right click
        sleep 1.2
        if [[ $THEN == esc ]]; then
          inject key KEY_ESC tap
        else
          inject move 20 $((OUT_H - 20)); sleep 0.2
          inject button left click
        fi
        sleep 0.8
      done
      B6_LINES+=("$(lines "$COMP_LOG")")
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    finish id="$GTK_ID" alive="$ALIVE" marks="[$(IFS=,; echo "${B6_LINES[*]}")]" wx="${X:-0}" wy="${Y:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))

    # B7 — a real nested chain: widget-factory's page 2 has a GtkPopoverMenuBar, whose
    # submenus are child xdg_popups (nested mode, unlike the sliding popover menus). Click
    # "Page 2" in the header bar's stack switcher, click "Edit" in the menubar (a grabbing
    # popup of the window), press H (the C_hecks mnemonic: a grabbing popup of that popup,
    # quoting the key serial), then click the empty desktop: both dismissed top-down.
    begin B7
    if (( ${GTK_ID:-0} > 0 )) && (( ALIVE )); then
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 23)); sleep 0.3
      inject button left click
      sleep 1.5
      inject move $((X + 341)) $((Y + 73)); sleep 0.3
      inject button left click
      wait_popup_maps 1 3
      sleep 0.5
      inject key 35 tap   # KEY_H
      wait_popup_maps 2 3
      sleep 0.5
      B7_OUT=$(lines "$COMP_LOG")
      inject move 20 $((OUT_H - 20)); sleep 0.2
      inject button left click
      sleep 1
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="${GTK_ID:-0}" alive="$ALIVE" outside="${B7_OUT:-0}" presses=6
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi
fi

deco_ok() { # id — the toplevel has had a server-side decoration applied since the scenario began
  tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -qE "compositor.deco: decoration apply id=$1 mode=server"
}

if want C; then
  # C1 — negotiation: foot (server), foot insisting on client-side, gtk4-demo (no object or
  # client-side).
  begin C1-server
  WAYLAND_DEBUG=client start_client foot-ssd foot --config=/dev/null
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  finish id="$FOOT_ID"

  if (( FOOT_ID > 0 )); then
    # C2 — drag the title bar by (+100,+60).
    begin C2
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 2)); PY=$((Y - 14))
    inject move "$PX" "$PY"; sleep 0.3
    inject drag "$PX" "$PY" $((PX + 100)) $((PY + 60)) --button left --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=100 dy=60

    # C3 — 4 px outside the right edge, +80.
    begin C3
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W + 4)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.3
    inject drag "$PX" "$PY" $((PX + 80)) "$PY" --button left --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" w0="$W" dw=80

    # C4 — the close button.
    begin C4
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W - 14)) $((Y - 14)); sleep 0.5
    inject button left click
    EXITED=0
    for _ in $(seq 1 20); do kill -0 "$FOOT_PID" 2>/dev/null || { EXITED=1; break; }; sleep 0.1; done
    finish id="$FOOT_ID" exited="$EXITED"
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  begin C1-client
  start_client foot-insists-csd foot --config=/dev/null -o csd.preferred=client
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID"

  if (( ! SKIP_GTK )) && command -v gtk4-demo >/dev/null; then
    begin C1-gtk
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-demo gtk4-demo
    GTK_PID=$LAST_PID
    GTK_ID=$(wait_map org.gtk.Demo4 10) && settle "$GTK_ID"
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID"
  fi

  # C5 — titles: one that fits, then one that does not.
  begin C5
  start_client foot-title foot --config=/dev/null sh -c \
    'sleep 1; printf "\033]2;lumen-title-c5\007"; sleep 1; printf "\033]2;%s\007" "lumen-title-c5 and then a title much too long to fit any window of this size, which the frame must cut with an ellipsis rather than overflow"; sleep 30'
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5)
  sleep 3
  read -r X Y W H < <(geom "$FOOT_ID")
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID" w="${W:-0}"

  # C6 — middle-click lowers: two windows, the top one is lowered under the other.
  begin C6
  start_client foot-low-a foot --config=/dev/null
  A_PID=$LAST_PID
  A_ID=$(wait_map foot 5) && settle "$A_ID"
  begin_line=$BEGIN_COMP
  BEGIN_COMP=$(lines "$COMP_LOG")
  start_client foot-low-b foot --config=/dev/null
  B_PID=$LAST_PID
  B_ID=$(wait_map foot 5) && settle "$B_ID"
  BEGIN_COMP=$begin_line
  read -r X Y W H < <(geom "$B_ID")
  inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
  inject button middle click
  sleep 0.5
  stop_client "$B_PID"; forget_client "$B_PID"
  stop_client "$A_PID"; forget_client "$A_PID"
  finish id="$B_ID" other="$A_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

layer_map() { # namespace — "x y w h" of the newest map of the layer surface with that namespace
  grep -E "compositor.layer: layer map surface=[0-9]+ layer=[a-z]+ namespace=$1 " "$COMP_LOG" |
    tail -1 | sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
wait_since() { # pattern seconds — a compositor line matching since the scenario began
  local deadline=$(( $(date +%s%N) + ${2}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -qE "$1" && return 0
    sleep 0.05
  done
  return 1
}
wait_layer() { # namespace seconds
  wait_since "compositor.layer: layer map surface=[0-9]+ layer=[a-z]+ namespace=$1 " "$2"
}

if want D; then
  # D1 — a 32 px top panel with an exclusive zone; it stays up through D8.
  begin D1
  start_client probe-panel "$PROBE" layer --layer top --anchor top,left,right --size 0x32 \
    --zone 32 --ns d1-panel --exit-after 600
  PANEL_PID=$LAST_PID
  wait_layer d1-panel 5
  start_client square-d1 "$SQUARE"
  SQ_PID=$LAST_PID
  if SQ_ID=$(wait_map lumen-square 5); then
    settle "$SQ_ID"
    read -r X Y W H < <(geom "$SQ_ID")
    PX=$((X + W / 2)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" "$PX" $((PY - 600)) --button left --super --steps 40
    settle "$SQ_ID"
  fi
  stop_client "$SQ_PID"; forget_client "$SQ_PID"
  finish id="$SQ_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D2 — a background surface stretched to every edge, zone -1.
  begin D2
  start_client probe-bg "$PROBE" layer --layer background --anchor top,bottom,left,right \
    --size 0x0 --zone -1 --ns d2-bg --exit-after 600
  BG_PID=$LAST_PID
  wait_layer d2-bg 5
  sleep 0.3
  stop_client "$BG_PID"; forget_client "$BG_PID"
  finish

  # D3 — an overlay at the bottom-right corner, 10 px from the right and 20 from the bottom.
  begin D3
  start_client probe-corner "$PROBE" layer --layer overlay --anchor bottom,right --size 100x80 \
    --margin 0,10,20,0 --ns d3-corner --exit-after 600
  CO_PID=$LAST_PID
  wait_layer d3-corner 5
  sleep 0.3
  stop_client "$CO_PID"; forget_client "$CO_PID"
  finish

  # D4 — an exclusive overlay over a focused window. Keys: A while it is up, B after a
  # (right) click on the window, C after it unmapped.
  begin D4
  start_client probe-d4-window "$PROBE" popup --size 400x300 --exit-after 600
  TOP_PID=$LAST_PID
  if TOP_ID=$(wait_map lumen-probe 5); then
    settle "$TOP_ID"
    wait_for "$LOG_DIR/probe-d4-window.log" "keyboard enter surface=toplevel" 3
    start_client probe-d4-excl "$PROBE" layer --layer overlay --kbd exclusive --size 300x100 \
      --ns d4-excl --unmap-after 4 --exit-after 600
    EX_PID=$LAST_PID
    wait_layer d4-excl 5
    sleep 0.4
    inject key KEY_A tap
    sleep 0.3
    read -r X Y W H < <(geom "$TOP_ID")
    inject move $((X + 20)) $((Y + 20)); sleep 0.2
    inject button right click
    sleep 0.3
    inject key 48 tap   # KEY_B
    wait_since "compositor.layer: layer unmap surface=[0-9]+ reason=null-buffer" 6
    sleep 0.4
    inject key 46 tap   # KEY_C
    sleep 0.3
    stop_client "$EX_PID"; forget_client "$EX_PID"
  fi
  stop_client "$TOP_PID"; forget_client "$TOP_PID"
  finish id="$TOP_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D5 — an on_demand surface under the panel. It has the keys from its map; A after a click
  # on it (which keeps them), B after a (right) click on the window.
  begin D5
  start_client probe-d5-window "$PROBE" popup --size 400x300 --exit-after 600
  TOP_PID=$LAST_PID
  if TOP_ID=$(wait_map lumen-probe 5); then
    settle "$TOP_ID"
    wait_for "$LOG_DIR/probe-d5-window.log" "keyboard enter surface=toplevel" 3
    start_client probe-d5-od "$PROBE" layer --layer top --anchor top --size 200x100 \
      --kbd on_demand --ns d5-ondemand --exit-after 600
    OD_PID=$LAST_PID
    wait_layer d5-ondemand 5
    sleep 0.4
    MAPPED_LINE=$(lines "$COMP_LOG")
    read -r LX LY LW LH < <(layer_map d5-ondemand)
    inject move $((LX + LW / 2)) $((LY + LH / 2)); sleep 0.2
    inject button left click
    sleep 0.3
    inject key KEY_A tap
    sleep 0.3
    read -r X Y W H < <(geom "$TOP_ID")
    inject move $((X + 20)) $((Y + 20)); sleep 0.2
    inject button right click
    sleep 0.3
    inject key 48 tap   # KEY_B
    sleep 0.3
    stop_client "$OD_PID"; forget_client "$OD_PID"
  fi
  stop_client "$TOP_PID"; forget_client "$TOP_PID"
  finish id="$TOP_ID" mapped="${MAPPED_LINE:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D6 — maximize and restore by title-bar double click: foot with the compositor's frame,
  # then gtk4-demo's own header bar.
  begin D6-ssd
  start_client foot-max foot --config=/dev/null
  FOOT_PID=$LAST_PID
  if FOOT_ID=$(wait_map foot 5); then
    settle "$FOOT_ID"
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
    inject button left dblclick
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    MAX_LINE=$(lines "$COMP_LOG")
    # A second panel (bottom, zone 40) shrinks the usable area under the maximized window:
    # it is configured to the new area; when the panel goes, back to the old one.
    start_client probe-d6-dock "$PROBE" layer --layer top --anchor bottom,left,right \
      --size 0x40 --zone 40 --ns d6-dock --exit-after 600
    DOCK_PID=$LAST_PID
    wait_layer d6-dock 5
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    DOCK_LINE=$(lines "$COMP_LOG")
    stop_client "$DOCK_PID"; forget_client "$DOCK_PID"
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    UNDOCK_LINE=$(lines "$COMP_LOG")
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
    inject button left dblclick
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID" max="${MAX_LINE:-0}" dock="${DOCK_LINE:-0}" undock="${UNDOCK_LINE:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  if (( ! SKIP_GTK )) && command -v gtk4-demo >/dev/null; then
    begin D6-gtk
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-demo-max gtk4-demo
    GTK_PID=$LAST_PID
    if GTK_ID=$(wait_map org.gtk.Demo4 10); then
      settle "$GTK_ID"
      sleep 1
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 15)); sleep 0.3
      inject button left dblclick
      sleep 0.5; settle "$GTK_ID"; sleep 0.5
      MAX_LINE=$(lines "$COMP_LOG")
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 15)); sleep 0.3
      inject button left dblclick
      sleep 0.5; settle "$GTK_ID"; sleep 0.5
    fi
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID" max="${MAX_LINE:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi

  # D7 — width 0 without both horizontal anchors: invalid_size at the initial commit.
  begin D7
  start_client probe-d7 "$PROBE" layer --layer top --anchor top --size 0x32 --ns d7-invalid \
    --exit-after 10
  D7_PID=$LAST_PID
  wait "$D7_PID"; D7_RC=$?
  forget_client "$D7_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
  finish rc="$D7_RC" alive="$ALIVE" pid="$D7_PID"

  # D8 — a bottom-anchored panel's popup (flip_y), grabbed with the press that opened it;
  # then a click on the empty desktop.
  begin D8
  start_client probe-d8 "$PROBE" layer --layer top --anchor bottom --size 300x40 \
    --popup 120x80 --anchor-rect 10,10,50,20 --adjust 8 --grab --ns d8-popup --exit-after 600
  LP_PID=$LAST_PID
  if wait_layer d8-popup 5; then
    sleep 0.3
    read -r LX LY LW LH < <(layer_map d8-popup)
    inject move $((LX + 20)) $((LY + 20)); sleep 0.2
    inject button left click
    wait_popup_maps 1 3
    sleep 0.3
    inject key KEY_A tap
    sleep 0.2
    inject move 20 $((OUT_H / 2)); sleep 0.2
    inject button left click
    sleep 0.6
  fi
  stop_client "$LP_PID"; forget_client "$LP_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))

  stop_client "$PANEL_PID"; forget_client "$PANEL_PID"
fi

if want X; then
  # X — parent wl_surface destroyed before the child's wl_subsurface.
  begin X-parent-first
  start_client square-parent-first "$SQUARE" --sub --destroy-parent-first -t 2
  SQ_PID=$LAST_PID
  wait "$SQ_PID"; SQ_RC=$?
  forget_client "$SQ_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
  finish rc="$SQ_RC" alive="$ALIVE"
fi

if want E; then
  SHELL_LOG="$LOG_DIR/shell.log"; SHELL2_LOG="$LOG_DIR/shell-restart.log"
  : >"$SHELL_LOG"; : >"$SHELL2_LOG"
  ebegin() { begin "$1"; SH_BEGIN=$(lines "$2"); }
  efinish() { # shell-log key=value...
    local log=$1; shift
    finish sh_a="$SH_BEGIN" sh_b="$(lines "$log")" "$@"
  }
  # E1 — two windows first, then the shell.
  ebegin E1 "$SHELL_LOG"
  start_client square-e1 "$SQUARE"
  E_SQ_PID=$LAST_PID
  E_SQ_ID=$(wait_map lumen-square 5) && settle "$E_SQ_ID"
  start_client foot-e1 foot --config=/dev/null
  E_FOOT_PID=$LAST_PID
  E_FOOT_ID=$(wait_map foot 5) && settle "$E_FOOT_ID"
  start_client shell env LUMEN_LOG=debug "$SHELL_BIN"
  SHELL_PID=$LAST_PID
  wait_since "compositor.layer: layer map surface=[0-9]+ layer=top namespace=lumen-panel " 5
  wait_for "$SHELL_LOG" "shell.panel: draw entries=2 " 5
  sleep 0.3
  efinish "$SHELL_LOG" sq="${E_SQ_ID:-0}" foot="${E_FOOT_ID:-0}"

  # E2 — click the entry that is not highlighted.
  ebegin E2 "$SHELL_LOG"
  FOC=$(grep -oE "shell.panel: draw entries=[0-9]+ hidden=[0-9]+ focused=-?[0-9]+" "$SHELL_LOG" |
        tail -1 | sed 's/.*focused=//')
  CLICK_IDX=0; [[ "$FOC" == "0" ]] && CLICK_IDX=1
  read -r EX EW < <(grep -oE "shell.panel: layout idx=$CLICK_IDX x=[0-9]+ w=[0-9]+" "$SHELL_LOG" |
                    tail -1 | sed -E 's/.*x=([0-9]+) w=([0-9]+)/\1 \2/')
  if [[ -n "${EX:-}" ]]; then
    inject move $((EX + EW / 2)) 16; sleep 0.3
    inject button left click
    sleep 0.8
  fi
  efinish "$SHELL_LOG" idx="$CLICK_IDX"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # E3 — a third window that retitles itself after a second.
  ebegin E3 "$SHELL_LOG"
  start_client foot-e3 foot --config=/dev/null sh -c \
    'sleep 1; printf "\033]2;lumen-title-e3\007"; sleep 600'
  E3_PID=$LAST_PID
  E3_ID=$(wait_map foot 5)
  wait_for "$SHELL_LOG" 'shell.bridge: title handle=[0-9]+ app_id=foot title="lumen-title-e3"' 8
  sleep 0.5
  efinish "$SHELL_LOG" id="${E3_ID:-0}"

  # E4 — lumen-square's client exits.
  ebegin E4 "$SHELL_LOG"
  stop_client "$E_SQ_PID"; forget_client "$E_SQ_PID"
  wait_for "$SHELL_LOG" "shell.bridge: remove handle=" 3
  sleep 0.5
  efinish "$SHELL_LOG"

  # E5 — restart the shell.
  ebegin E5 "$SHELL2_LOG"
  stop_client "$SHELL_PID"; forget_client "$SHELL_PID"
  wait_since "compositor.shell: bridge destroyed" 3
  sleep 0.3
  start_client shell-restart env LUMEN_LOG=debug "$SHELL_BIN"
  SHELL_PID=$LAST_PID
  wait_for "$SHELL2_LOG" "shell.panel: draw entries=" 5
  sleep 0.5
  efinish "$SHELL2_LOG"

  # E6 — the same binary, copied elsewhere.
  begin E6
  cp "$SHELL_BIN" "$LOG_DIR/fake-shell"
  start_client fake-shell "$LOG_DIR/fake-shell"
  FAKE_PID=$LAST_PID
  for _ in $(seq 1 100); do kill -0 "$FAKE_PID" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$FAKE_PID" 2>/dev/null && kill -TERM "$FAKE_PID"
  wait "$FAKE_PID"; E6_RC=$?
  forget_client "$FAKE_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$SHELL_PID" 2>/dev/null && ALIVE=1
  finish rc="$E6_RC" shell_alive="$ALIVE" pid="$FAKE_PID"

  stop_client "$SHELL_PID"; forget_client "$SHELL_PID"
  stop_client "$E3_PID"; forget_client "$E3_PID"
  stop_client "$E_FOOT_PID"; forget_client "$E_FOOT_PID"
fi

if want F; then
  # Terminals showing one word, or reading pasted text into a file. foot in a UTF-8 locale:
  # in "C" it prints a warning line into the terminal above the word.
  f_text() { # log-name app text — foot|alacritty showing `text` on its first row
    if [[ $2 == foot ]]; then
      LC_ALL=C.UTF-8 WAYLAND_DEBUG=client start_client "$1" foot --config=/dev/null \
        sh -c "printf '%s\n' '$3'; exec sleep 600"
    else
      WAYLAND_DEBUG=client start_client "$1" alacritty --config-file /dev/null \
        -e sh -c "printf '%s\n' '$3'; exec sleep 600"
    fi
  }
  f_cat() { # log-name app file — foot|alacritty running `cat > file`
    rm -f "$3"
    if [[ $2 == foot ]]; then
      LC_ALL=C.UTF-8 WAYLAND_DEBUG=client start_client "$1" foot --config=/dev/null \
        sh -c "cat > '$3'"
    else
      WAYLAND_DEBUG=client start_client "$1" alacritty --config-file /dev/null \
        -e sh -c "cat > '$3'"
    fi
  }
  f_appid() { [[ $1 == foot ]] && echo foot || echo Alacritty; }
  f_word() { # toplevel id — double-click the word on its first row
    local X Y W H
    read -r X Y W H < <(geom "$1")
    inject move $((X + 10)) $((Y + 8)); sleep 0.3
    inject button left dblclick; sleep 0.6
  }
  f_finish_cat() { # pid — Enter, Ctrl+D; wait up to 5 s for the terminal to exit
    inject key KEY_ENTER tap; sleep 0.3
    inject key KEY_LEFTCTRL+KEY_D tap
    for _ in $(seq 1 50); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
    stop_client "$1"; forget_client "$1"
  }
  f_aside() { # id-under id-over — move the window on top right of the other's centre
    local X Y W H X2 Y2 W2 H2 PX PY
    read -r X Y W H < <(geom "$1")
    read -r X2 Y2 W2 H2 < <(geom "$2")
    PX=$((X2 + W2 / 2)); PY=$((Y2 + H2 / 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX + X + W / 2 + 40 - X2)) "$PY" --button left --super --steps 30
    settle "$2"
  }
  f_center() { # id — "x y" of its centre
    local X Y W H
    read -r X Y W H < <(geom "$1")
    echo "$((X + W / 2)) $((Y + H / 2))"
  }

  # F1/F2 — copy by keyboard in one terminal, paste by keyboard into a new one.
  for spec in "F1 foot alacritty lumen-clip-F1-4242" "F2 alacritty foot lumen-clip-F2-5151"; do
    read -r NAME SRC DST TEXT <<<"$spec"
    begin "$NAME"
    f_text "${SRC}-${NAME,,}" "$SRC" "$TEXT"
    SRC_PID=$LAST_PID
    SRC_ID=$(wait_map "$(f_appid "$SRC")" 8) && settle "$SRC_ID"
    sleep 0.5
    f_word "$SRC_ID"
    inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap; sleep 0.5
    f_cat "${DST}-${NAME,,}" "$DST" "$LOG_DIR/$NAME.out"
    DST_PID=$LAST_PID
    DST_ID=$(wait_map "$(f_appid "$DST")" 8) && settle "$DST_ID"
    sleep 0.5
    inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_V tap; sleep 0.5
    f_finish_cat "$DST_PID"
    stop_client "$SRC_PID"; forget_client "$SRC_PID"
    finish src="${SRC_ID:-0}" dst="${DST_ID:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))
  done

  # F3/F3r — primary: select (no copy key) in one terminal, middle-click the other, which
  # does not have the keyboard until that click.
  for spec in "F3 foot alacritty lumen-prim-F3-7777" "F3r alacritty foot lumen-prim-F3r-8888"; do
    read -r NAME SRC DST TEXT <<<"$spec"
    begin "$NAME"
    f_cat "${DST}-${NAME,,}" "$DST" "$LOG_DIR/$NAME.out"
    DST_PID=$LAST_PID
    DST_ID=$(wait_map "$(f_appid "$DST")" 8) && settle "$DST_ID"
    f_text "${SRC}-${NAME,,}" "$SRC" "$TEXT"
    SRC_PID=$LAST_PID
    SRC_ID=$(wait_map "$(f_appid "$SRC")" 8) && settle "$SRC_ID"
    sleep 0.5
    f_aside "$DST_ID" "$SRC_ID"
    sleep 0.3
    f_word "$SRC_ID"
    read -r CX CY < <(f_center "$DST_ID")
    inject move "$CX" "$CY"; sleep 0.3
    inject button middle click; sleep 0.6
    f_finish_cat "$DST_PID"
    stop_client "$SRC_PID"; forget_client "$SRC_PID"
    finish src="${SRC_ID:-0}" dst="${DST_ID:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))
  done

  # F5 — ownership: foot copies, then alacritty; alacritty exits while foot is left focused.
  begin F5
  f_text foot-f5 foot lumen-own-F5-1
  F5_FOOT_PID=$LAST_PID
  F5_FOOT_ID=$(wait_map foot 8) && settle "$F5_FOOT_ID"
  sleep 0.5
  f_word "$F5_FOOT_ID"
  inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap; sleep 0.5
  f_text alacritty-f5 alacritty lumen-own-F5-2
  F5_AL_PID=$LAST_PID
  F5_AL_ID=$(wait_map Alacritty 8) && settle "$F5_AL_ID"
  sleep 0.5
  f_word "$F5_AL_ID"
  inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap; sleep 0.5
  F5_MARK=$(lines "$COMP_LOG")
  stop_client "$F5_AL_PID"; forget_client "$F5_AL_PID"
  sleep 0.8
  finish foot="${F5_FOOT_ID:-0}" al="${F5_AL_ID:-0}" mark="$F5_MARK"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # F6 — an unfocused reader is offered nothing until it is clicked. foot-f5 still owns
  # nothing (alacritty's copy replaced it, then went); foot copies anew once the probe
  # has lost the keyboard to it.
  begin F6
  start_client probe-f6 "$PROBE" selection --set-on never --receive "text/plain;charset=utf-8" \
    --exit-after 60
  F6_PR_PID=$LAST_PID
  F6_PR_ID=$(wait_map lumen-probe 5) && settle "$F6_PR_ID"
  f_text foot-f6 foot lumen-own-F6-3
  F6_FOOT_PID=$LAST_PID
  F6_FOOT_ID=$(wait_map foot 8) && settle "$F6_FOOT_ID"
  sleep 0.5
  f_aside "$F6_PR_ID" "$F6_FOOT_ID"
  F6_M0=$(lines "$LOG_DIR/probe-f6.log")
  f_word "$F6_FOOT_ID"
  inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap; sleep 0.8
  F6_M1=$(lines "$LOG_DIR/probe-f6.log")
  read -r CX CY < <(f_center "$F6_PR_ID")
  inject move "$CX" "$CY"; sleep 0.3
  inject button left click; sleep 1.0
  stop_client "$F6_PR_PID"; forget_client "$F6_PR_PID"
  stop_client "$F6_FOOT_PID"; forget_client "$F6_FOOT_PID"
  stop_client "$F5_FOOT_PID"; forget_client "$F5_FOOT_PID"
  finish m0="$F6_M0" m1="$F6_M1"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # F7 — a serial never sent (serial 1): refused and cancelled. F7r — a source set twice:
  # the client's protocol error, provoked on purpose.
  begin F7
  start_client probe-f7 "$PROBE" selection --stale-serial --exit-after 30
  F7_PID=$LAST_PID
  if F7_ID=$(wait_map lumen-probe 5); then
    settle "$F7_ID"
    read -r CX CY < <(f_center "$F7_ID")
    inject move "$CX" "$CY"; sleep 0.3
    inject button left click; sleep 0.8
  fi
  stop_client "$F7_PID"; forget_client "$F7_PID"
  finish id="${F7_ID:-0}"
  begin F7r
  start_client probe-f7r "$PROBE" selection --reuse-source --exit-after 30
  F7R_PID=$LAST_PID
  if F7R_ID=$(wait_map lumen-probe 5); then
    settle "$F7R_ID"
    read -r CX CY < <(f_center "$F7R_ID")
    inject move "$CX" "$CY"; sleep 0.3
    inject button left click
  fi
  for _ in $(seq 1 30); do kill -0 "$F7R_PID" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$F7R_PID" 2>/dev/null && kill -TERM "$F7R_PID"
  wait "$F7R_PID"; F7R_RC=$?
  forget_client "$F7R_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
  finish rc="$F7R_RC" alive="$ALIVE" pid="$F7R_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # F4 — GTK both ways, through widget-factory's text widget (B6's point).
  if (( ! SKIP_GTK )); then
    begin F4
    WAYLAND_DEBUG=client GDK_BACKEND=wayland GSK_RENDERER=cairo \
      start_client gtk-factory-f4 gtk4-widget-factory
    F4_GTK_PID=$LAST_PID
    if F4_GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$F4_GTK_ID"
      sleep 1
      read -r GX GY GW GH < <(geom "$F4_GTK_ID")
      TX=$((GX + 1376)); TY=$((GY + 529))
      inject move "$TX" "$TY"; sleep 0.3
      inject button left click; sleep 0.4
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.2
      inject type lumen-gtk-f4; sleep 0.3
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.2
      inject key KEY_LEFTCTRL+KEY_C tap; sleep 0.5
      f_cat foot-f4 foot "$LOG_DIR/F4.out"
      F4_CAT_PID=$LAST_PID
      F4_CAT_ID=$(wait_map foot 8) && settle "$F4_CAT_ID"
      sleep 0.5
      inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_V tap; sleep 0.5
      f_finish_cat "$F4_CAT_PID"
      # Round trip: foot copies, GTK pastes over its text and copies that back out.
      f_text foot-f4r foot lumen-clip-f4r
      F4R_PID=$LAST_PID
      F4R_ID=$(wait_map foot 8) && settle "$F4R_ID"
      sleep 0.5
      f_word "$F4R_ID"
      inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap; sleep 0.5
      inject move "$TX" "$TY"; sleep 0.3
      inject button left click; sleep 0.4
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.2
      inject key KEY_LEFTCTRL+KEY_V tap; sleep 0.8
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.2
      inject key KEY_LEFTCTRL+KEY_C tap; sleep 0.5
      # GTK owns the clipboard now: the foot that offered the text may go.
      stop_client "$F4R_PID"; forget_client "$F4R_PID"
      sleep 0.5
      f_cat foot-f4c foot "$LOG_DIR/F4r.out"
      F4C_PID=$LAST_PID
      F4C_ID=$(wait_map foot 8) && settle "$F4C_ID"
      sleep 0.5
      inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_V tap; sleep 0.5
      f_finish_cat "$F4C_PID"
    fi
    ALIVE=0; kill -0 "$F4_GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$F4_GTK_PID"; forget_client "$F4_GTK_PID"
    finish id="${F4_GTK_ID:-0}" alive="$ALIVE"
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi
fi

if want G; then
  # Three columns: the source probe left, the target probe in the middle, the source's
  # second window (G6/G7) right. Probes are 300x200; each scenario starts its own.
  COL1=$((OUT_W / 6)); COL2=$((OUT_W / 2)); COL3=$((OUT_W * 5 / 6)); ROW=$((OUT_H / 2))
  g_map() { # app_id exclude-id seconds — newest toplevel with that app_id mapped since the
            # scenario began, other than exclude-id
    local deadline=$(( $(date +%s%N) + ${3}000000000 )) id
    while (( $(date +%s%N) < deadline )); do
      id=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" |
           grep -oE "compositor.wm: map id=[0-9]+ app_id=$1 " | sed -E 's/.*id=([0-9]+).*/\1/' |
           grep -vx "$2" | tail -1)
      [[ -n "$id" ]] && { echo "$id"; return 0; }
      sleep 0.05
    done
    echo 0
    return 1
  }
  g_center() { local X Y W H; read -r X Y W H < <(geom "$1"); echo "$((X + W / 2)) $((Y + H / 2))"; }
  g_put() { # id cx cy — Super+drag the window's centre to (cx, cy)
    local CX CY
    [[ "$1" != 0 ]] || return 0
    read -r CX CY < <(g_center "$1")
    inject move "$CX" "$CY"; sleep 0.2
    inject drag "$CX" "$CY" "$2" "$3" --super --steps 20
    settle "$1"
  }
  # g_run NAME — starts the target (TGT_ARGS; none when TGT_ARGS=(none)) and the source
  # (SRC_ARGS), places them, then drags from the source's centre to (DX, DY) with DRAG_ARGS.
  g_run() {
    local n=${1,,} SX SY
    G_TGT_PID=""; G_TGT_ID=0; G_SEC_ID=0
    if [[ "${TGT_ARGS[0]:-}" != none ]]; then
      start_client "probe-${n}t" "$PROBE" dnd-target --size 300x200 "${TGT_ARGS[@]}" --exit-after 60
      G_TGT_PID=$LAST_PID
      G_TGT_ID=$(g_map lumen-probe 0 5) && settle "$G_TGT_ID"
    fi
    start_client "probe-${n}s" "$PROBE" dnd-source --size 300x200 "${SRC_ARGS[@]}" --exit-after 60
    G_SRC_PID=$LAST_PID
    G_SRC_ID=$(g_map lumen-probe "$G_TGT_ID" 5) && settle "$G_SRC_ID"
    if [[ " ${SRC_ARGS[*]} " == *" --second-window "* ]]; then
      G_SEC_ID=$(g_map lumen-probe-2 0 5) && settle "$G_SEC_ID"
    fi
    # Topmost first: the second window, the source, then the target.
    g_put "$G_SEC_ID" "$COL3" "$ROW"
    g_put "$G_SRC_ID" "$COL1" "$ROW"
    g_put "$G_TGT_ID" "$COL2" "$ROW"
    sleep 0.3
    read -r SX SY < <(g_center "$G_SRC_ID")
    inject move "$SX" "$SY"; sleep 0.3
    inject drag "$SX" "$SY" "$DX" "$DY" --steps 100 "${DRAG_ARGS[@]}"
    sleep "${G_SETTLE:-1.0}"
  }
  g_stop() {
    local alive=0
    kill -0 "$COMP_PID" 2>/dev/null && alive=1
    stop_client "$G_SRC_PID"; forget_client "$G_SRC_PID"
    if [[ -n "$G_TGT_PID" ]]; then stop_client "$G_TGT_PID"; forget_client "$G_TGT_PID"; fi
    finish src="${G_SRC_ID:-0}" tgt="${G_TGT_ID:-0}" sec="${G_SEC_ID:-0}" alive="$alive"
    SCEN=park; inject move 20 $((OUT_H - 20))
  }

  # G1 (+G9: its icon) — copy|move offered, move preferred, dropped on the target.
  begin G1
  SRC_ARGS=(--actions copy,move --data lumen-dnd-G1 --icon 32x32)
  TGT_ARGS=(--accept text/plain --actions copy,move --preferred move)
  DX=$COL2; DY=$ROW; DRAG_ARGS=()
  g_run G1; g_stop

  # G2 — the user's action from modifiers: Ctrl → copy; Ctrl, then Shift over the target.
  begin G2
  SRC_ARGS=(--actions copy,move --data lumen-dnd-G2)
  DRAG_ARGS=(--mods ctrl)
  g_run G2; g_stop
  begin G2b
  SRC_ARGS=(--actions copy,move --data lumen-dnd-G2b)
  DRAG_ARGS=(--mods ctrl --mods-at 90:shift)
  g_run G2b; g_stop

  # G3 — released over the empty desktop.
  begin G3
  SRC_ARGS=(--actions copy,move)
  DX=$COL2; DY=$((OUT_H - 60)); DRAG_ARGS=()
  g_run G3; g_stop

  # G4 — Escape while over the target.
  begin G4
  DX=$COL2; DY=$ROW; DRAG_ARGS=(--esc-at 90)
  g_run G4; g_stop

  # G5 — a target that accepts nothing.
  begin G5
  TGT_ARGS=(--accept none --actions copy,move --preferred move)
  DRAG_ARGS=()
  g_run G5; g_stop

  # G6 — dropped on the source client's own second window (no other client involved).
  begin G6
  TGT_ARGS=(none)
  SRC_ARGS=(--actions copy,move --data lumen-dnd-G6 --second-window)
  DX=$COL3; DY=$ROW
  g_run G6; g_stop

  # G7 — no source: crossing the target probe shows it nothing; the own window gets it.
  begin G7
  TGT_ARGS=(--accept text/plain --actions copy,move --preferred move)
  SRC_ARGS=(--null-source --second-window)
  g_run G7; g_stop

  # G8 — the source's client exits while the pointer rests on the target.
  begin G8
  SRC_ARGS=(--actions copy,move --exit-during-drag 1.4)
  DX=$COL2; DY=$ROW; DRAG_ARGS=(--hold-ms 1500)
  g_run G8
  g_stop

  # G10 — start_drag quoting the pointer enter's serial instead of the press.
  begin G10
  SRC_ARGS=(--actions copy,move --stale-serial)
  DRAG_ARGS=()
  g_run G10; g_stop

  # G11 — GTK → foot: widget-factory's text view (F4's) filled with numbered lines through
  # the clipboard (a probe owns it), all of it selected and dragged onto foot running `cat`.
  # widget-factory is moved left first: a press raises it, and at its size it would
  # otherwise cover foot. It is taken near its right edge, so the pointer stays well inside
  # the output — at the left edge the move would end in an edge snap (half the output),
  # which resizes the window and moves the text view away from (1376, 529).
  if (( ! SKIP_GTK )); then
    begin G11
    rm -f "$LOG_DIR/G11.out"
    G11_DATA=$(for i in $(seq 12); do printf 'g11-%02d lumen-gtk-dnd lumen-gtk-dnd\n' "$i"; done)
    printf '%s\n' "$G11_DATA" >"$LOG_DIR/G11.want"
    WAYLAND_DEBUG=client GDK_BACKEND=wayland GSK_RENDERER=cairo \
      start_client gtk-factory-g11 gtk4-widget-factory
    G11_GTK_PID=$LAST_PID
    G11_FOOT_PID=""
    if G11_GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$G11_GTK_ID"
      sleep 1
      read -r GX GY GW GH < <(geom "$G11_GTK_ID")
      G11_GRAB=$((GX + GW - 150))
      inject move "$G11_GRAB" $((GY + 400)); sleep 0.2
      inject drag "$G11_GRAB" $((GY + 400)) $((G11_GRAB - 1100)) $((GY + 400)) --super --steps 30
      settle "$G11_GTK_ID"
      read -r GX GY GW GH < <(geom "$G11_GTK_ID")
      TX=$((GX + 1376)); TY=$((GY + 529))
      start_client probe-g11c "$PROBE" selection --data "$G11_DATA" --exit-after 30
      G11_PR_PID=$LAST_PID
      if G11_PR_ID=$(wait_map lumen-probe 5); then
        settle "$G11_PR_ID"
        read -r CX CY < <(g_center "$G11_PR_ID")
        inject move "$CX" "$CY"; sleep 0.3
        inject button left click; sleep 0.5
      fi
      inject move "$TX" "$TY"; sleep 0.3
      inject button left click; sleep 0.4
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.2
      inject key KEY_LEFTCTRL+KEY_V tap; sleep 0.8
      stop_client "$G11_PR_PID"; forget_client "$G11_PR_PID"
      inject key KEY_LEFTCTRL+KEY_A tap; sleep 0.3
      LC_ALL=C.UTF-8 WAYLAND_DEBUG=client start_client foot-g11 foot --config=/dev/null \
        sh -c "cat > '$LOG_DIR/G11.out'"
      G11_FOOT_PID=$LAST_PID
      G11_FOOT_ID=$(wait_map foot 8) && settle "$G11_FOOT_ID"
      sleep 0.5
      read -r FX FY < <(g_center "$G11_FOOT_ID")
      # Pressed on the selected text; GTK starts the drag once past its threshold.
      inject drag "$TX" "$TY" "$FX" "$FY" --steps 80 --hold-ms 400
      sleep 1.0
      # The drop gives foot no keyboard (no compositor does that): click it, then end the
      # last line and the input.
      inject button left click; sleep 0.3
      inject key KEY_ENTER tap; sleep 0.3
      inject key KEY_LEFTCTRL+KEY_D tap
      for _ in $(seq 1 50); do kill -0 "$G11_FOOT_PID" 2>/dev/null || break; sleep 0.1; done
    fi
    [[ -n "$G11_FOOT_PID" ]] && { stop_client "$G11_FOOT_PID"; forget_client "$G11_FOOT_PID"; }
    ALIVE=0; kill -0 "$G11_GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$G11_GTK_PID"; forget_client "$G11_GTK_PID"
    finish id="${G11_GTK_ID:-0}" alive="$ALIVE"
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi
fi

comp_since() { # pattern seconds — a compositor line matching since the scenario began
  local deadline=$(( $(date +%s%N) + ${2}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -qE "$1" && return 0
    sleep 0.05
  done
  return 1
}

if want H; then
  # H1 — the probe binds wl_output and zxdg_output_manager_v1 before mapping.
  begin H1
  start_client probe-h1 "$PROBE" outputs --size 300x200 --exit-after 30
  H_PID=$LAST_PID
  H_ID=$(wait_map lumen-probe 5) && settle "$H_ID"
  wait_for "$LOG_DIR/probe-h1.log" "surface enter surface=toplevel" 3
  stop_client "$H_PID"; forget_client "$H_PID"
  finish id="${H_ID:-0}"

  # H3 — wl_output bound one second after the first configure: the enter is replayed.
  begin H3
  start_client probe-h3 "$PROBE" outputs --size 300x200 --late 1 --exit-after 30
  H_PID=$LAST_PID
  H_ID=$(wait_map lumen-probe 5) && settle "$H_ID"
  wait_for "$LOG_DIR/probe-h3.log" "surface enter surface=toplevel" 4
  stop_client "$H_PID"; forget_client "$H_PID"
  finish id="${H_ID:-0}"

  # H4 — a grabbing popup maps (enter) and is dismissed by an outside click (leave); the
  # toplevel unmaps 4 s after its first configure (leave).
  begin H4
  start_client probe-h4 "$PROBE" outputs --size 300x200 --popup 120x80 --grab --unmap-after 4 \
    --exit-after 30
  H_PID=$LAST_PID
  H_ID=$(wait_map lumen-probe 5) && settle "$H_ID"
  if (( H_ID > 0 )); then
    read -r X Y W H < <(geom "$H_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
    inject button left click; sleep 0.8
    inject move 20 $((OUT_H - 20)); sleep 0.2
    inject button left click; sleep 0.5
    wait_for "$LOG_DIR/probe-h4.log" "toplevel unmapped" 6
    sleep 0.5
  fi
  stop_client "$H_PID"; forget_client "$H_PID"
  finish id="${H_ID:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # H5 — real clients from their own side: a traced foot and a traced gtk4-widget-factory
  # map; each one's wl_output, zxdg_output_v1 and wl_surface.enter are judged from its trace.
  begin H5
  LC_ALL=C.UTF-8 WAYLAND_DEBUG=client start_client foot-h5 foot --config=/dev/null
  H5_FOOT_PID=$LAST_PID
  H5_FOOT_ID=$(wait_map foot 8) && settle "$H5_FOOT_ID"
  H5_GTK_ID=0; H5_GTK_PID=""
  if (( ! SKIP_GTK )); then
    WAYLAND_DEBUG=client GDK_BACKEND=wayland GSK_RENDERER=cairo \
      start_client gtk-factory-h5 gtk4-widget-factory
    H5_GTK_PID=$LAST_PID
    H5_GTK_ID=$(wait_map org.gtk.WidgetFactory4 10) && settle "$H5_GTK_ID"
    sleep 1
  fi
  sleep 0.5
  stop_client "$H5_FOOT_PID"; forget_client "$H5_FOOT_PID"
  if [[ -n "$H5_GTK_PID" ]]; then stop_client "$H5_GTK_PID"; forget_client "$H5_GTK_PID"; fi
  finish foot="${H5_FOOT_ID:-0}" gtk="${H5_GTK_ID:-0}" gtk_skipped="$SKIP_GTK"
fi

if want I; then
  # I1 — lumen-shell up (for the flags), then a probe with a server-side frame that leaves
  # the activation ping unanswered for 4 s.
  begin I1
  I_SHELL_LOG="$LOG_DIR/shell-i.log"
  start_client shell-i env LUMEN_LOG=debug "$SHELL_BIN"
  I_SHELL_PID=$LAST_PID
  comp_since "compositor.shell: bridge bound " 5
  comp_since "compositor.layer: layer map surface=[0-9]+ layer=top namespace=lumen-panel " 5
  start_client probe-i1 "$PROBE" popup --size 300x200 --ssd --title lumen-probe-i1 \
    --ignore-ping-for 4 --exit-after 30
  I_PID=$LAST_PID
  I_ID=$(wait_map lumen-probe 5)
  comp_since "compositor.liveness: responsive client=" 8
  sleep 0.5
  stop_client "$I_PID"; forget_client "$I_PID"
  stop_client "$I_SHELL_PID"; forget_client "$I_SHELL_PID"
  finish id="${I_ID:-0}"

  # I2 — a probe that never answers: three timeouts later it still gets clicks.
  begin I2
  start_client probe-i2 "$PROBE" popup --size 300x200 --ignore-ping-for 1000 --exit-after 30
  I_PID=$LAST_PID
  I_ID=$(wait_map lumen-probe 5)
  comp_since "compositor.liveness: not-responding client=" 4
  sleep 4.5
  if (( I_ID > 0 )); then
    read -r X Y W H < <(geom "$I_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
    inject button left click; sleep 0.5
    inject move $((X + 20)) $((Y + 20)); sleep 0.3
    inject button left click; sleep 0.5
  fi
  ALIVE=0; kill -0 "$I_PID" 2>/dev/null && ALIVE=1
  stop_client "$I_PID"; forget_client "$I_PID"
  finish id="${I_ID:-0}" alive="$ALIVE"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # I3 — a real hang: foot (server-side frame) stopped, clicked, hovered, resumed.
  begin I3
  LC_ALL=C.UTF-8 WAYLAND_DEBUG=client start_client foot-i3 foot --config=/dev/null
  I_FOOT_PID=$LAST_PID
  I_FOOT_ID=$(wait_map foot 5) && settle "$I_FOOT_ID"
  ALIVE=0
  if (( I_FOOT_ID > 0 )); then
    # Past the 2 s between pings since its activation ping, so the press pings it.
    sleep 2.2
    read -r X Y W H < <(geom "$I_FOOT_ID")
    kill -STOP "$I_FOOT_PID"
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
    inject button left click
    comp_since "compositor.liveness: not-responding client=" 4
    sleep 0.3
    inject drag $((X + 20)) $((Y + 20)) $((X + W - 20)) $((Y + H - 20)) --button none --steps 300
    inject drag $((X + W - 20)) $((Y + H - 20)) $((X + 20)) $((Y + 20)) --button none --steps 300
    sleep 0.5
    kill -CONT "$I_FOOT_PID"
    comp_since "compositor.liveness: responsive client=" 4
    sleep 0.5
    inject key KEY_A tap; sleep 0.5
    kill -0 "$I_FOOT_PID" 2>/dev/null && ALIVE=1
  fi
  stop_client "$I_FOOT_PID"; forget_client "$I_FOOT_PID"
  finish id="${I_FOOT_ID:-0}" pid="$I_FOOT_PID" alive="$ALIVE"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want J; then
  wait_map_except() { # app_id seconds id — as wait_map, but never toplevel `id`
    local deadline=$(( $(date +%s%N) + ${2}000000000 )) id
    while (( $(date +%s%N) < deadline )); do
      id=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" |
           grep -oE "compositor.wm: map id=[0-9]+ app_id=$1 " | sed -E 's/.*id=([0-9]+).*/\1/' |
           grep -vx "$3" | tail -1)
      [[ -n "$id" ]] && { echo "$id"; return 0; }
      sleep 0.05
    done
    echo 0
    return 1
  }
  click_center() { # toplevel id
    local X Y W H
    read -r X Y W H < <(geom "$1")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
    inject button left click; sleep 0.4
  }
  # An issuer probe mapped (it takes the keyboard) and clicked: its token in $1.
  issue_token() { # log-name token-file [issuer options...]
    local log=$1 file=$2; shift 2
    start_client "$log" "$PROBE" activation-issuer --token-file "$file" --size 300x200 \
      --exit-after 60 "$@"
    J_ISS_PID=$LAST_PID
    J_ISS_ID=$(wait_map_except lumen-probe 5 "${J_TGT_ID:-0}") && settle "$J_ISS_ID"
    (( J_ISS_ID > 0 )) && click_center "$J_ISS_ID"
    wait_for "$LOG_DIR/$log.log" "^activation token=" 3
  }
  stop_j() { local p; for p in "$@"; do [[ -n "$p" ]] && { stop_client "$p"; forget_client "$p"; }; done; }

  # J1 — granted.
  begin J1
  J_TGT_ID=0
  start_client probe-j1t "$PROBE" activation-target --token-file "$LOG_DIR/J1.go" \
    --title lumen-j1-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map lumen-probe 5) && settle "$J_TGT_ID"
  issue_token probe-j1i "$LOG_DIR/J1.token"
  cp "$LOG_DIR/J1.token" "$LOG_DIR/J1.go" 2>/dev/null
  comp_since "compositor.activation: activate token=.* result=" 4
  sleep 0.5
  finish target="${J_TGT_ID:-0}" issuer="${J_ISS_ID:-0}"
  stop_j "$J_ISS_PID" "$J_TGT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # J2 — the user moved on (clicked foot) between the token and its use; lumen-shell shows the
  # target, a click on its panel entry focuses it.
  begin J2
  J_SHELL_LOG="$LOG_DIR/shell-j.log"
  start_client shell-j env LUMEN_LOG=debug "$SHELL_BIN"
  J_SHELL_PID=$LAST_PID
  comp_since "compositor.layer: layer map surface=[0-9]+ layer=top namespace=lumen-panel " 5
  J_TGT_ID=0
  start_client probe-j2t "$PROBE" activation-target --token-file "$LOG_DIR/J2.go" \
    --title lumen-j2-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map lumen-probe 5) && settle "$J_TGT_ID"
  issue_token probe-j2i "$LOG_DIR/J2.token" --title lumen-j2-issuer
  LC_ALL=C.UTF-8 start_client foot-j2 foot --config=/dev/null
  J_FOOT_PID=$LAST_PID
  J_FOOT_ID=$(wait_map foot 5) && settle "$J_FOOT_ID"
  (( J_FOOT_ID > 0 )) && click_center "$J_FOOT_ID"
  cp "$LOG_DIR/J2.token" "$LOG_DIR/J2.go" 2>/dev/null
  comp_since "compositor.activation: activate token=.* result=" 4
  wait_for "$J_SHELL_LOG" "shell.bridge: flags handle=[0-9]+ flags=2" 3
  sleep 0.5
  read -r EX EW < <(grep -oE 'shell.panel: layout idx=[0-9]+ x=[0-9]+ w=[0-9]+ handle=[0-9]+ title="lumen-j2-target"' \
                    "$J_SHELL_LOG" | tail -1 | sed -E 's/.* x=([0-9]+) w=([0-9]+).*/\1 \2/')
  if [[ -n "${EX:-}" ]]; then
    inject move $((EX + EW / 2)) 16; sleep 0.3
    inject button left click
    wait_for "$J_SHELL_LOG" "shell.bridge: flags handle=[0-9]+ flags=0" 3
    sleep 0.5
  fi
  finish target="${J_TGT_ID:-0}" issuer="${J_ISS_ID:-0}" foot="${J_FOOT_ID:-0}"
  stop_j "$J_FOOT_PID" "$J_ISS_PID" "$J_TGT_PID" "$J_SHELL_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # J3 — a token issued without a serial.
  begin J3
  J_TGT_ID=0
  start_client probe-j3t "$PROBE" activation-target --token-file "$LOG_DIR/J3.go" \
    --title lumen-j3-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map lumen-probe 5) && settle "$J_TGT_ID"
  issue_token probe-j3i "$LOG_DIR/J3.token" --no-serial
  cp "$LOG_DIR/J3.token" "$LOG_DIR/J3.go" 2>/dev/null
  comp_since "compositor.activation: activate token=.* result=" 4
  sleep 0.3
  finish target="${J_TGT_ID:-0}" issuer="${J_ISS_ID:-0}"
  stop_j "$J_ISS_PID" "$J_TGT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # J4 — a token nobody issued.
  begin J4
  start_client probe-j4 "$PROBE" activation-target --token deadbeef --title lumen-j4-target \
    --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map lumen-probe 5)
  comp_since "compositor.activation: activate token=.* result=" 4
  sleep 0.3
  ALIVE=0; kill -0 "$J_TGT_PID" 2>/dev/null && ALIVE=1
  finish target="${J_TGT_ID:-0}" alive="$ALIVE"
  stop_j "$J_TGT_PID"

  # J5 — presented before the first commit: decided at map. J5b: an unknown one.
  begin J5
  J_TGT_ID=0
  issue_token probe-j5i "$LOG_DIR/J5.token"
  start_client probe-j5t "$PROBE" activation-target --token-file "$LOG_DIR/J5.token" --early \
    --title lumen-j5-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map_except lumen-probe 5 "$J_ISS_ID") && settle "$J_TGT_ID"
  sleep 0.3
  finish target="${J_TGT_ID:-0}" issuer="${J_ISS_ID:-0}"
  J5_TGT_PID=$J_TGT_PID J5_ISS_PID=$J_ISS_PID

  # J5b — a launch token the user moved on from: issued by a click in a new issuer, then the
  # user clicks the J5 window (its top-left corner: later windows cascade down and right),
  # then the new window presents the token before its first commit.
  begin J5b
  J_PREV_ID=$J_TGT_ID J_TGT_ID=0
  issue_token probe-j5bi "$LOG_DIR/J5b.token"
  if (( J_PREV_ID > 0 )); then
    read -r X Y W H < <(geom "$J_PREV_ID")
    inject move $((X + 8)) $((Y + 8)); sleep 0.3
    inject button left click; sleep 0.4
  fi
  start_client probe-j5b "$PROBE" activation-target --token-file "$LOG_DIR/J5b.token" --early \
    --title lumen-j5b-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map_except lumen-probe 5 "$J_ISS_ID") && settle "$J_TGT_ID"
  sleep 0.3
  finish target="${J_TGT_ID:-0}" focused="${J_PREV_ID:-0}"
  J5B_PID=$J_TGT_PID

  # J5c — an unknown token before the first commit proves nothing: the new window takes the
  # keyboard like any new window (GTK presents a self-issued token for every new window).
  begin J5c
  start_client probe-j5c "$PROBE" activation-target --token deadbeef --early \
    --title lumen-j5c-target --size 300x200 --exit-after 60
  J_TGT_PID=$LAST_PID
  J_TGT_ID=$(wait_map lumen-probe 5) && settle "$J_TGT_ID"
  sleep 0.3
  finish target="${J_TGT_ID:-0}"
  stop_j "$J_TGT_PID" "$J5B_PID" "$J_ISS_PID" "$J5_TGT_PID" "$J5_ISS_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # J6 — real foot started with the issuer's token in XDG_ACTIVATION_TOKEN.
  begin J6
  J_TGT_ID=0
  issue_token probe-j6i "$LOG_DIR/J6.token"
  J6_TOKEN=$(cat "$LOG_DIR/J6.token" 2>/dev/null)
  LC_ALL=C.UTF-8 XDG_ACTIVATION_TOKEN="$J6_TOKEN" WAYLAND_DEBUG=client \
    start_client foot-j6 foot --config=/dev/null
  J_FOOT_PID=$LAST_PID
  J_FOOT_ID=$(wait_map foot 5) && settle "$J_FOOT_ID"
  comp_since "compositor.activation: activate token=.* id=${J_FOOT_ID:-x} result=(granted|urgent)" 4
  sleep 0.3
  finish foot="${J_FOOT_ID:-0}" issuer="${J_ISS_ID:-0}"
  stop_j "$J_FOOT_PID" "$J_ISS_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want S; then
  # S1 — the fractional-scale protocol at scale 1, and hit-testing at the last pixel.
  begin S1
  start_client probe-s1 "$PROBE" scale --size 300x200 --exit-after 30
  S_PID=$LAST_PID
  S_ID=$(wait_map lumen-probe 5) && settle "$S_ID"
  if (( S_ID > 0 )); then
    read -r X Y W H < <(geom "$S_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
    inject move $((X + W - 1)) $((Y + H / 2)); sleep 0.3
    inject move $((X + W + 1)) $((Y + H / 2)); sleep 0.4
  fi
  stop_client "$S_PID"; forget_client "$S_PID"
  finish id="${S_ID:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # S2 — a cropped, scaled viewport, judged from a screenshot of the frame.
  begin S2
  start_client probe-s2 "$PROBE" scale --buffer 200x100 --crop 100,0,100,50 --dst 300x150 \
    --exit-after 30
  S_PID=$LAST_PID
  S_ID=$(wait_map lumen-probe 5) && settle "$S_ID"
  SHOT=""
  if (( S_ID > 0 )); then
    sleep 0.3
    kill -USR1 "$COMP_PID" 2>/dev/null
    if wait_for "$COMP_LOG" "screenshot [0-9]+x[0-9]+ written to " 4; then
      SHOT=$(grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //')
    fi
  fi
  stop_client "$S_PID"; forget_client "$S_PID"
  finish id="${S_ID:-0}" shot="\"${SHOT}\""
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want P; then
  wait_exit() { # pid seconds — the client ends on its own (it prints its summary first)
    local deadline=$(( $(date +%s%N) + ${2}000000000 ))
    while kill -0 "$1" 2>/dev/null && (( $(date +%s%N) < deadline )); do sleep 0.1; done
    stop_client "$1"; forget_client "$1"
  }
  # P1 — a continuously drawing client: every frame presented, with the flip's timing.
  begin P1
  start_client probe-p1 "$PROBE" presentation --frames 180 --size 400x300 --exit-after 40
  P_PID=$LAST_PID
  P_ID=$(wait_map lumen-probe 5)
  wait_exit "$P_PID" 30
  finish id="${P_ID:-0}"

  # P2 — three commits per frame: two superseded before they can be shown.
  begin P2
  start_client probe-p2 "$PROBE" presentation --burst 3 --frames 90 --size 300x200 --exit-after 30
  P_PID=$LAST_PID
  P_ID=$(wait_map lumen-probe 5)
  wait_exit "$P_PID" 25
  finish id="${P_ID:-0}"

  # P3 — covered by a maximized opaque window: nothing presented, callbacks throttled.
  begin P3
  start_client probe-p3a "$PROBE" presentation --frames 0 --size 300x200 --exit-after 40
  P3A_PID=$LAST_PID
  P3A_ID=$(wait_map lumen-probe 5)
  sleep 1
  start_client probe-p3b "$PROBE" presentation --maximize --frames 120 --title lumen-p3b \
    --exit-after 30
  P3B_PID=$LAST_PID
  wait_exit "$P3B_PID" 25
  sleep 2
  stop_client "$P3A_PID"; forget_client "$P3A_PID"
  finish id="${P3A_ID:-0}"
fi

if want K; then
  # K1 — idle after the timeout, resumed by input, idle again.
  begin K1
  start_client probe-k1 "$PROBE" idle --timeout 1500 --exit-after 20
  K1_PID=$LAST_PID
  sleep 3
  inject move 333 222
  sleep 2.5
  stop_client "$K1_PID"; forget_client "$K1_PID"
  finish

  # K2 — a visible inhibitor holds idling back; covered, it does not.
  begin K2
  start_client probe-k2i "$PROBE" inhibit --size 360x240 --title lumen-k2-inhibit --exit-after 40
  K2I_PID=$LAST_PID
  wait_map lumen-probe 5 >/dev/null
  sleep 0.5
  start_client probe-k2 "$PROBE" idle --timeout 1500 --exit-after 30
  K2_PID=$LAST_PID
  start_client probe-k2in "$PROBE" idle --timeout 1500 --input-only --exit-after 30
  K2IN_PID=$LAST_PID
  sleep 3.5
  K2_COVER_AT=$(python3 -c 'import time;print("%.3f"%time.clock_gettime(time.CLOCK_MONOTONIC))')
  start_client probe-k2c "$PROBE" popup --maximize --title lumen-k2-cover --exit-after 30
  K2C_PID=$LAST_PID
  sleep 3
  for pid in "$K2C_PID" "$K2_PID" "$K2IN_PID" "$K2I_PID"; do stop_client "$pid"; forget_client "$pid"; done
  finish cover_at="$K2_COVER_AT"

  # K3 — power off, nothing committed while dark, a key wakes it and is swallowed.
  begin K3
  start_client probe-k3k "$PROBE" popup --size 300x200 --title lumen-k3-keys --exit-after 40
  K3K_PID=$LAST_PID
  wait_map lumen-probe 5 >/dev/null
  sleep 1
  start_client probe-k3p "$PROBE" power --set off --exit-after 30
  K3P_PID=$LAST_PID
  sleep 2.5
  inject key KEY_A tap
  sleep 1.5
  inject key KEY_B tap
  sleep 1
  for pid in "$K3P_PID" "$K3K_PID"; do stop_client "$pid"; forget_client "$pid"; done
  finish
fi

if want L; then
  shot() { # a screenshot of the next frame; prints its path (empty when none came)
    local n
    n=$(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    local deadline=$(( $(date +%s%N) + 4000000000 ))
    while (( $(date +%s%N) < deadline )); do
      if (( $(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG") > n )); then
        grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
        return 0
      fi
      sleep 0.05
    done
  }
  # L1 — lock, keys and clicks only to the lock surface, a test password unlocks.
  begin L1
  start_client probe-l1w "$PROBE" popup --size 360x240 --title lumen-l1-win --exit-after 60
  L1W_PID=$LAST_PID
  L1_ID=$(wait_map lumen-probe 5) && settle "$L1_ID"
  read -r L1X L1Y L1WW L1WH < <(geom "$L1_ID")
  sleep 0.5
  start_client probe-l1 "$PROBE" lock --password 42 --exit-after 30
  L1_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-l1.log" "^lock locked" 3
  sleep 0.5
  inject key KEY_A tap
  inject move $((L1X + L1WW / 2)) $((L1Y + L1WH / 2))
  inject button left click
  inject type 41
  inject key KEY_ENTER tap
  sleep 0.5
  inject type 42
  inject key KEY_ENTER tap
  wait_for "$LOG_DIR/probe-l1.log" "^lock unlocked" 3
  sleep 0.5
  inject key KEY_B tap
  sleep 0.5
  stop_client "$L1_PID"; forget_client "$L1_PID"
  finish id="${L1_ID:-0}"

  # L2 — the locker dies: abandoned (dark red), keys reach nobody; a replacement unlocks.
  begin L2
  start_client probe-l2 "$PROBE" lock --crash-after 1 --exit-after 20
  L2_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-l2.log" "^lock crash" 5
  sleep 0.5
  L2_SHOT=$(shot)
  inject key KEY_C tap
  sleep 0.3
  stop_client "$L2_PID"; forget_client "$L2_PID"
  start_client probe-l2r "$PROBE" lock --unlock-after 1 --exit-after 20
  L2R_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-l2r.log" "^lock unlocked" 5
  sleep 0.3
  stop_client "$L2R_PID"; forget_client "$L2R_PID"
  finish shot="\"${L2_SHOT}\""

  # L3 — no lock surface: locked after the 1 s wait, a solid colour.
  begin L3
  start_client probe-l3 "$PROBE" lock --no-surfaces --unlock-after 1.5 --exit-after 20
  L3_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-l3.log" "^lock locked" 3
  sleep 0.3
  L3_SHOT=$(shot)
  wait_for "$LOG_DIR/probe-l3.log" "^lock unlocked" 5
  stop_client "$L3_PID"; forget_client "$L3_PID"
  finish shot="\"${L3_SHOT}\""

  # L4 — logind's Unlock finishes a live locker.
  begin L4
  start_client probe-l4 "$PROBE" lock --exit-after 20
  L4_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-l4.log" "^lock locked" 3
  L4_SESSION=$(grep -m1 -oE "compositor.session: session [^ ]+ vt=" "$COMP_LOG" | awk '{print $3}')
  [[ -n "$L4_SESSION" && "$L4_SESSION" != "-" ]] && sudo -n loginctl unlock-session "$L4_SESSION"
  wait_for "$LOG_DIR/probe-l4.log" "^lock finished" 3
  sleep 0.5
  stop_client "$L4_PID"; forget_client "$L4_PID"
  stop_client "$L1W_PID"; forget_client "$L1W_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want M; then
  mshot() { # a screenshot of the next frame; prints its path (empty when none came)
    local n
    n=$(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    local deadline=$(( $(date +%s%N) + 4000000000 ))
    while (( $(date +%s%N) < deadline )); do
      if (( $(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG") > n )); then
        grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
        return 0
      fi
      sleep 0.05
    done
  }
  # M1 — compose into the probe window; the popup at its cursor.
  begin M1
  start_client probe-m1 "$PROBE" text-input --size 500x300 --title lumen-m1-ti --exit-after 90
  M1_PID=$LAST_PID
  M1_ID=$(wait_map lumen-probe 5) && settle "$M1_ID"
  start_client probe-m1im "$PROBE" im --exit-after 90
  M1IM_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-m1im.log" "^im grab keymap" 4
  sleep 0.5
  inject key KEY_A tap; sleep 0.3
  inject key KEY_A tap; sleep 0.5
  M1_SHOT=$(mshot)
  inject key KEY_SPACE tap; sleep 0.3
  inject key KEY_B tap; sleep 0.5
  # A click on the popup (a candidate): the input method gets it, the keyboard stays.
  read -r M1X M1Y M1W M1H < <(geom "${M1_ID:-0}")
  inject move $((M1X + 20 + 60)) $((M1Y + 58 + 20 + 2))
  inject move $((M1X + 20 + 60)) $((M1Y + 58 + 20))
  inject button left click; sleep 0.3
  inject key KEY_F tap; sleep 0.4
  inject move $((M1X + 250)) $((M1Y + 250))
  sleep 0.3
  finish id="${M1_ID:-0}" shot="\"${M1_SHOT}\""

  # M2 — focus elsewhere and back.
  begin M2
  start_client probe-m2 "$PROBE" popup --size 300x200 --title lumen-m2-other --exit-after 30
  M2_PID=$LAST_PID
  wait_map lumen-probe 5 >/dev/null
  sleep 0.8
  inject key KEY_C tap; sleep 0.5
  stop_client "$M2_PID"; forget_client "$M2_PID"
  wait_for "$LOG_DIR/probe-m1im.log" "^im activate" 3
  sleep 0.8
  inject key KEY_D tap; sleep 0.5
  finish

  # M3 — locked while active: no text-input focus until unlocked.
  begin M3
  start_client probe-m3 "$PROBE" lock --unlock-after 2 --exit-after 20
  M3_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-m3.log" "^lock locked" 3
  sleep 0.3
  inject key KEY_E tap
  wait_for "$LOG_DIR/probe-m3.log" "^lock unlocked" 5
  sleep 0.8
  stop_client "$M3_PID"; forget_client "$M3_PID"
  stop_client "$M1_PID"; forget_client "$M1_PID"
  finish

  # M4 — foot: compose into `cat`.
  begin M4
  rm -f "$LOG_DIR/M4.out"
  start_client foot-m4 env WAYLAND_DEBUG=client foot -d info sh -c "cat > '$LOG_DIR/M4.out'"
  M4_PID=$LAST_PID
  wait_map foot 8 >/dev/null
  sleep 1.5
  inject key KEY_A tap; sleep 0.3
  inject key KEY_A tap; sleep 0.4
  inject key KEY_SPACE tap; sleep 0.3
  inject type xyz; sleep 0.3
  inject key KEY_ENTER tap; sleep 0.3
  inject key KEY_LEFTCTRL+KEY_D tap
  for _ in $(seq 1 30); do kill -0 "$M4_PID" 2>/dev/null || break; sleep 0.1; done
  stop_client "$M4_PID"; forget_client "$M4_PID"
  finish

  # M5 — GTK 4's entry.
  begin M5
  if (( SKIP_GTK )) || ! command -v gtk4-demo >/dev/null; then
    finish gtk_skipped=1
  else
    start_client gtk-m5 env NO_AT_BRIDGE=1 GTK_A11Y=none GSK_RENDERER=cairo WAYLAND_DEBUG=client \
      gtk4-demo --run=entry_undo
    M5_PID=$LAST_PID
    wait_map org.gtk.Demo4 10 >/dev/null
    sleep 2
    inject key KEY_A tap; sleep 0.3
    inject key KEY_A tap; sleep 0.5
    inject key KEY_SPACE tap; sleep 0.8
    stop_client "$M5_PID"; forget_client "$M5_PID"
    finish gtk_skipped=0
  fi
  stop_client "$M1IM_PID"; forget_client "$M1IM_PID"
fi

if want T; then
  wait_gone() { # pid seconds
    local deadline=$(( $(date +%s%N) + ${2}000000000 ))
    while kill -0 "$1" 2>/dev/null && (( $(date +%s%N) < deadline )); do sleep 0.1; done
    stop_client "$1"; forget_client "$1"
  }
  # T1 — Mesa's Vulkan FIFO present (fifo-v1, commit-timing-v1 when it uses present timing).
  begin T1
  if command -v vkcube >/dev/null; then
    T1_START=$(date +%s%N)
    WAYLAND_DEBUG=client start_client vkcube-t1 vkcube --wsi wayland --present_mode 2 --c 300
    T1_PID=$LAST_PID
    wait_gone "$T1_PID" 30
    T1_MS=$(( ($(date +%s%N) - T1_START) / 1000000 ))
    finish skipped=0 ms="$T1_MS"
  else
    finish skipped=1
  fi

  # T2 — weston-simple-timing: frames scheduled in the future with commit-timing.
  begin T2
  if command -v weston-simple-timing >/dev/null; then
    WAYLAND_DEBUG=client start_client simple-timing-t2 weston-simple-timing
    T2_PID=$LAST_PID
    sleep 5
    stop_client "$T2_PID"; forget_client "$T2_PID"
    finish skipped=0
  else
    finish skipped=1
  fi

  # T3 — the probe's FIFO stream and timed commits, judged from its presentation feedback.
  begin T3
  start_client probe-t3 "$PROBE" timing --frames 120 --timed 20 --step 55 --size 320x240
  T3_PID=$LAST_PID
  wait_gone "$T3_PID" 40
  finish

  # T4 — single-pixel + viewport + alpha + content type, and its pixels.
  begin T4
  start_client probe-t4 "$PROBE" solid --rgba 0,255,0,255 --alpha 0.5 --content-type video \
    --size 320x240 --exit-after 30
  T4_PID=$LAST_PID
  T4_ID=$(wait_map lumen-probe 5) && settle "$T4_ID"
  SHOT=""
  if (( T4_ID > 0 )); then
    sleep 0.3
    N0=$(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    for _ in $(seq 1 40); do
      (( $(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG") > N0 )) && break
      sleep 0.1
    done
    SHOT=$(grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //')
  fi
  stop_client "$T4_PID"; forget_client "$T4_PID"
  finish id="${T4_ID:-0}" shot="\"${SHOT}\""

  # T5 — tearing-control: a fullscreen window asking for async presentation.
  begin T5
  start_client probe-t5 "$PROBE" tearing --exit-after 30
  T5_PID=$LAST_PID
  T5_ID=$(wait_map lumen-probe 5) && settle "$T5_ID"
  comp_since "async page flip unsupported|async flip allowed" 3
  sleep 0.5
  stop_client "$T5_PID"; forget_client "$T5_PID"
  finish id="${T5_ID:-0}"

  # T6 — a GL client: dma-buf buffers with implicit fences, checked before each apply.
  begin T6
  if command -v weston-simple-egl >/dev/null; then
    WAYLAND_DEBUG=client start_client simple-egl-t6 weston-simple-egl
    T6_PID=$LAST_PID
    T6_ID=$(wait_map "\\S*" 5)
    sleep 4
    stop_client "$T6_PID"; forget_client "$T6_PID"
    finish skipped=0 id="${T6_ID:-0}"
  else
    finish skipped=1
  fi
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want U || want V; then
  # lumen-shell for U and V: icons and the bell reach its panel, its Super tap is the
  # inhibitable grab V1 checks.
  begin UV0
  UV_SHELL_LOG="$LOG_DIR/shell-uv.log"
  start_client shell-uv env LUMEN_LOG=debug "$SHELL_BIN"
  UV_SHELL_PID=$LAST_PID
  comp_since "compositor.shell: bridge bound " 5
  comp_since "compositor.layer: layer map surface=[0-9]+ layer=top namespace=lumen-panel " 5
  finish
fi

if want U; then
  # U1 — lumen-probe dialog: a modal child of a parent window (xdg-dialog-v1).
  begin U1
  start_client probe-u1 "$PROBE" dialog --exit-after 40
  U1_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u1.log" "^dialog mapped" 5
  sleep 0.5
  U1_PARENT=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -oE "compositor.wm: map id=[0-9]+ app_id=lumen-probe " | head -1 | sed -E 's/.*id=([0-9]+).*/\1/')
  U1_DIALOG=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -oE "compositor.wm: map id=[0-9]+ app_id=lumen-probe " | sed -n 2p | sed -E 's/.*id=([0-9]+).*/\1/')
  [[ -n "$U1_DIALOG" ]] && settle "$U1_DIALOG"
  # A click on the parent's top-left corner (the centred dialog does not cover it).
  read -r PX PY _ _ < <(geom "${U1_PARENT:-0}")
  if [[ -n "${PX:-}" ]]; then
    inject move $((PX + 12)) $((PY + 12)); sleep 0.2
    inject button left click; sleep 0.5
  fi
  stop_client "$U1_PID"; forget_client "$U1_PID"
  finish parent="${U1_PARENT:-0}" dialog="${U1_DIALOG:-0}"

  # U2 — GTK 4's modal message dialog (gtk4-demo "Dialogs", Alt+M): the parent blocked.
  begin U2
  if (( ! SKIP_GTK )) && command -v gtk4-demo >/dev/null; then
    WAYLAND_DEBUG=client GSK_RENDERER=cairo start_client gtk-u2 gtk4-demo --run=dialog
    U2_PID=$LAST_PID
    # gtk4-demo --run takes the app id gtk4-demo (not org.gtk.Demo4).
    U2_PARENT=$(wait_map gtk4-demo 10)
    sleep 1.5
    inject key KEY_LEFTALT+KEY_M tap; sleep 1.5
    U2_DIALOG=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -oE "compositor.wm: map id=[0-9]+ app_id=gtk4-demo " | sed -n 2p | sed -E 's/.*id=([0-9]+).*/\1/')
    read -r PX PY _ _ < <(geom "${U2_PARENT:-0}")
    if [[ -n "${PX:-}" && -n "${U2_DIALOG:-}" ]]; then
      inject move $((PX + 15)) $((PY + 60)); sleep 0.2
      inject button left click; sleep 0.6
      inject key KEY_ESC tap; sleep 0.8
    fi
    stop_client "$U2_PID"; forget_client "$U2_PID"
    finish skipped=0 parent="${U2_PARENT:-0}" dialog="${U2_DIALOG:-0}"
  else
    finish skipped=1
  fi

  # U3 — icons: foot names its icon (and must not warn about xdg-toplevel-icon), the probe
  # sends a name and two pixel buffers; both reach the shell.
  begin U3
  start_client foot-u3 foot --app-id foot -o bell.system=yes sh -c 'sleep 1.2; printf "\a"; sleep 30'
  U3_FOOT_PID=$LAST_PID
  U3_FOOT=$(wait_map foot 8)
  start_client probe-u3 "$PROBE" icon --name lumen-probe-u3-icon --sizes 16,32 --exit-after 30
  U3_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u3.log" "^icon set" 5
  sleep 1.5
  stop_client "$U3_PID"; forget_client "$U3_PID"
  finish foot="${U3_FOOT:-0}"

  # U4 — the bell: foot's `\a` (bell.system=yes) and the probe's rings reach the shell, which
  # flashes the window (an overlay layer surface).
  begin U4
  start_client probe-u4 "$PROBE" bell --count 6 --interval 50 --exit-after 30
  U4_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u4.log" "^bell rang" 5
  sleep 0.8
  stop_client "$U4_PID"; forget_client "$U4_PID"
  stop_client "$U3_FOOT_PID"; forget_client "$U3_FOOT_PID"
  finish

  # U5 — xdg-foreign: v2 and v1 exports imported by another client (modal across clients),
  # and `destroyed` when the exporter goes.
  begin U5
  start_client probe-u5e "$PROBE" foreign-export --exit-after 40
  U5E_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u5e.log" "^handle=" 5
  U5_HANDLE=$(grep -m1 -oE "^handle=[0-9a-f]+" "$LOG_DIR/probe-u5e.log" | cut -d= -f2)
  start_client probe-u5i "$PROBE" foreign-import --handle "${U5_HANDLE:-none}" --modal --exit-after 40
  U5I_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u5i.log" "^(imported|destroyed)" 5
  start_client probe-u5v1e "$PROBE" foreign-export --v1 --exit-after 40
  U5V1E_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u5v1e.log" "^handle=" 5
  U5V1_HANDLE=$(grep -m1 -oE "^handle=[0-9a-f]+" "$LOG_DIR/probe-u5v1e.log" | cut -d= -f2)
  start_client probe-u5v1i "$PROBE" foreign-import --v1 --handle "${U5V1_HANDLE:-none}" --exit-after 40
  U5V1I_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-u5v1i.log" "^(imported|destroyed)" 5
  sleep 0.5
  stop_client "$U5E_PID"; forget_client "$U5E_PID"
  stop_client "$U5V1E_PID"; forget_client "$U5V1E_PID"
  wait_for "$LOG_DIR/probe-u5i.log" "^destroyed" 3
  wait_for "$LOG_DIR/probe-u5v1i.log" "^destroyed" 3
  stop_client "$U5I_PID"; forget_client "$U5I_PID"
  stop_client "$U5V1I_PID"; forget_client "$U5V1I_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want V; then
  # V1 — keyboard-shortcuts-inhibit: while the probe inhibits, the shell's Super tap and
  # Super+Return reach it (no launcher); Ctrl+Alt+F2 still switches VT; Super+Escape turns
  # the inhibitor off (inactive), after which the Super tap opens the launcher again.
  begin V1
  start_client probe-v1 "$PROBE" shortcuts-inhibit --exit-after 60
  V1_PID=$LAST_PID
  V1_ID=$(wait_map lumen-probe 5) && settle "$V1_ID"
  wait_for "$LOG_DIR/probe-v1.log" "^inhibit active" 3
  inject key KEY_LEFTMETA tap; sleep 0.6
  inject key KEY_LEFTMETA+KEY_ENTER tap; sleep 0.6
  V1_MARK_VT=$(lines "$COMP_LOG")
  START_TTY=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
  inject key KEY_LEFTCTRL+KEY_LEFTALT+KEY_F2 tap
  if wait_for "$COMP_LOG" "session paused" 5; then
    sleep 0.5
    sudo -n chvt "${START_TTY#tty}"
    wait_for "$COMP_LOG" "session resumed" 6
    sleep 1
  fi
  V1_MARK_HATCH=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA+KEY_ESC tap; sleep 0.6
  V1_MARK_TAP=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA tap; sleep 1.0
  comp_since "namespace=lumen-launcher" 3
  inject key KEY_ESC tap; sleep 0.6
  inject key KEY_LEFTMETA+KEY_ESC tap; sleep 0.6
  stop_client "$V1_PID"; forget_client "$V1_PID"
  finish id="${V1_ID:-0}" vt_mark="$V1_MARK_VT" hatch_mark="$V1_MARK_HATCH" tap_mark="$V1_MARK_TAP"

  # V2 — data control with real tools: wl-copy/wl-paste (ext-data-control-v1), cliphist
  # storing every selection through `wl-paste --watch`, and foot pasting a wl-copy selection.
  # Every wl-copy runs in the foreground (it would fork otherwise) and is stopped by PID.
  begin V2
  if command -v wl-copy >/dev/null && command -v wl-paste >/dev/null; then
    V2_CACHE="$LOG_DIR/V2-cache"; rm -rf "$V2_CACHE"; mkdir -p "$V2_CACHE"
    V2_WATCH_PID=""
    if command -v cliphist >/dev/null; then
      start_client wlpaste-watch-v2 env XDG_CACHE_HOME="$V2_CACHE" wl-paste --watch cliphist store
      V2_WATCH_PID=$LAST_PID
    fi
    sleep 0.5
    WAYLAND_DEBUG=client start_client wlcopy-v2 wl-copy --foreground "lumen-v2-clipboard"
    V2_COPY_PID=$LAST_PID
    sleep 0.8
    WAYLAND_DISPLAY="$SOCKET" timeout 5 wl-paste --no-newline >"$LOG_DIR/V2.paste" 2>"$LOG_DIR/wlpaste-v2.log"
    start_client wlcopy-v2p wl-copy --foreground --primary "lumen-v2-primary"
    V2_PCOPY_PID=$LAST_PID
    sleep 0.8
    WAYLAND_DISPLAY="$SOCKET" timeout 5 wl-paste --primary --no-newline >"$LOG_DIR/V2.primary" 2>>"$LOG_DIR/wlpaste-v2.log"
    # foot pastes it: foot runs `cat` into a file, Ctrl+Shift+V, Enter, Ctrl+D.
    rm -f "$LOG_DIR/V2.out"
    start_client foot-v2 foot --app-id foot-v2 sh -c "cat > '$LOG_DIR/V2.out'"
    V2_FOOT_PID=$LAST_PID
    V2_FOOT=$(wait_map foot-v2 8) && settle "$V2_FOOT"
    sleep 0.5
    inject key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_V tap; sleep 0.5
    inject key KEY_ENTER tap; sleep 0.3
    inject key KEY_LEFTCTRL+KEY_D tap; sleep 0.8
    stop_client "$V2_FOOT_PID"; forget_client "$V2_FOOT_PID"
    # A second copy: the first wl-copy is cancelled (and exits); cliphist holds both.
    start_client wlcopy-v2b wl-copy --foreground "lumen-v2-second"
    V2_COPY2_PID=$LAST_PID
    sleep 1.0
    if [[ -n "$V2_WATCH_PID" ]]; then
      XDG_CACHE_HOME="$V2_CACHE" cliphist list >"$LOG_DIR/V2.cliphist" 2>&1
      stop_client "$V2_WATCH_PID"; forget_client "$V2_WATCH_PID"
    fi
    V2_FIRST_EXITED=0; kill -0 "$V2_COPY_PID" 2>/dev/null || V2_FIRST_EXITED=1
    for p in "$V2_COPY_PID" "$V2_PCOPY_PID" "$V2_COPY2_PID"; do stop_client "$p"; forget_client "$p"; done
    finish skipped=0 foot="${V2_FOOT:-0}" first_exited="$V2_FIRST_EXITED" cliphist=$([[ -n "$V2_WATCH_PID" ]] && echo 1 || echo 0)
  else
    finish skipped=1
  fi

  # V3 — security-context: the probe plays a sandbox engine; a client on its socket sees no
  # session-wide global, and the socket stops accepting when the engine hangs up.
  begin V3
  start_client probe-v3 "$PROBE" security-context --app-id org.lumen.ProbeV3
  V3_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-v3.log" "security-context closed=" 6
  stop_client "$V3_PID"; forget_client "$V3_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want U || want V; then
  stop_client "$UV_SHELL_PID"; forget_client "$UV_SHELL_PID"
fi

if want W; then
  # W1 — a persistent pointer lock: relative motion flows, the pointer stays; focus loss, a VT
  # switch and the session lock end it and it holds again after each; Super+Escape releases.
  begin W1
  W1_LOG="$LOG_DIR/probe-w1.log"
  start_client probe-w1 "$PROBE" constraints --exit-after 120
  W1_PID=$LAST_PID
  W1_ID=$(wait_map lumen-probe 5) && settle "$W1_ID"
  read -r WX WY WW WH < <(geom "${W1_ID:-0}")
  WX=${WX:-0}; WY=${WY:-0}; WW=${WW:-480}; WH=${WH:-320}
  inject move $((WX - 30)) $((WY - 30)); sleep 0.3
  inject move $((WX + WW / 2)) $((WY + WH / 2))
  wait_for "$W1_LOG" "^locked" 3
  W1_M_LOCKED=$(lines "$W1_LOG")
  inject rel 10 -5 --count 20 --interval-ms 8; sleep 0.4
  # The absolute device while locked: its move is a relative delta (+150,+40), nothing more.
  inject move $((WX + WW / 2 + 150)) $((WY + WH / 2 + 40)); sleep 0.4
  W1_M_FOCUS=$(lines "$W1_LOG")
  start_client square-w1 "$SQUARE"
  W1_SQ=$LAST_PID
  wait_map lumen-square 5 >/dev/null; sleep 0.8
  stop_client "$W1_SQ"; forget_client "$W1_SQ"
  sleep 1.0
  W1_M_VT=$(lines "$W1_LOG")
  START_TTY=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
  inject key KEY_LEFTCTRL+KEY_LEFTALT+KEY_F2 tap
  if wait_for "$COMP_LOG" "session paused" 5; then
    sleep 0.5
    sudo -n chvt "${START_TTY#tty}"
    wait_for "$COMP_LOG" "session resumed" 6
    sleep 1
  fi
  W1_M_LOCK=$(lines "$W1_LOG")
  start_client probe-w1l "$PROBE" lock --unlock-after 1 --exit-after 10
  W1L_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-w1l.log" "^lock unlocked" 6
  sleep 0.8
  stop_client "$W1L_PID"; forget_client "$W1L_PID"
  W1_M_ESC=$(lines "$W1_LOG")
  inject key KEY_LEFTMETA+KEY_ESC tap; sleep 0.4
  inject rel 2 0 --count 3 --interval-ms 8; sleep 0.4
  stop_client "$W1_PID"; forget_client "$W1_PID"
  finish id="${W1_ID:-0}" m_locked="$W1_M_LOCKED" m_focus="$W1_M_FOCUS" m_vt="$W1_M_VT" \
    m_lock="$W1_M_LOCK" m_esc="$W1_M_ESC" w="$WW" h="$WH"

  # W2 — confinement to a region of the window.
  begin W2
  W2_LOG="$LOG_DIR/probe-w2.log"
  start_client probe-w2 "$PROBE" constraints --confine --region 40,40,200,120 --exit-after 60
  W2_PID=$LAST_PID
  W2_ID=$(wait_map lumen-probe 5) && settle "$W2_ID"
  read -r WX WY WW WH < <(geom "${W2_ID:-0}")
  WX=${WX:-0}; WY=${WY:-0}
  inject move $((WX + 100)) $((WY + 100))
  wait_for "$W2_LOG" "^confined" 3
  W2_M_IN=$(lines "$W2_LOG")
  inject move $((WX + 400)) $((WY + 300)); sleep 0.3
  W2_M_FAR=$(lines "$W2_LOG")
  inject rel 30 0 --count 5 --interval-ms 8; sleep 0.3
  W2_M_REL=$(lines "$W2_LOG")
  inject move 5 5; sleep 0.3
  W2_M_ESC=$(lines "$W2_LOG")
  inject key KEY_LEFTMETA+KEY_ESC tap; sleep 0.4
  inject move 6 6; sleep 0.4
  stop_client "$W2_PID"; forget_client "$W2_PID"
  finish id="${W2_ID:-0}" m_in="$W2_M_IN" m_far="$W2_M_FAR" m_rel="$W2_M_REL" m_esc="$W2_M_ESC"

  # W3 — a oneshot lock with a cursor hint: Super+Escape warps to the hint, and it is spent.
  begin W3
  W3_LOG="$LOG_DIR/probe-w3.log"
  start_client probe-w3 "$PROBE" constraints --oneshot --hint 60,50 --exit-after 60
  W3_PID=$LAST_PID
  W3_ID=$(wait_map lumen-probe 5) && settle "$W3_ID"
  read -r WX WY WW WH < <(geom "${W3_ID:-0}")
  WX=${WX:-0}; WY=${WY:-0}; WW=${WW:-480}; WH=${WH:-320}
  inject move $((WX + WW / 2)) $((WY + WH / 2))
  wait_for "$W3_LOG" "^locked" 3
  W3_M_ESC=$(lines "$W3_LOG")
  inject key KEY_LEFTMETA+KEY_ESC tap; sleep 0.4
  inject move 8 8; sleep 0.3
  inject move $((WX + WW / 2 + 10)) $((WY + WH / 2 + 10)); sleep 0.6
  stop_client "$W3_PID"; forget_client "$W3_PID"
  finish id="${W3_ID:-0}" m_esc="$W3_M_ESC"

  # W4 — weston-constraints: click to confine, click to lock, click to unlock.
  begin W4
  if command -v weston-constraints >/dev/null; then
    WAYLAND_DEBUG=client start_client weston-w4 weston-constraints
    W4_PID=$LAST_PID
    W4_ID=$(wait_map org.freedesktop.weston.simple-contrains 6) && settle "$W4_ID"
    read -r WX WY WW WH < <(geom "${W4_ID:-0}")
    WX=${WX:-0}; WY=${WY:-0}
    inject move $((WX + 40)) $((WY + 40)); sleep 0.3
    inject button left click; sleep 0.5
    inject move $((OUT_W - 3)) $((OUT_H - 3)); sleep 0.4
    inject button left click; sleep 0.5
    inject rel 5 5 --count 5 --interval-ms 8; sleep 0.4
    inject button left click; sleep 0.5
    stop_client "$W4_PID"; forget_client "$W4_PID"
    finish skipped=0 id="${W4_ID:-0}" w="${WW:-0}" h="${WH:-0}"
  else
    finish skipped=1
  fi

  # W5 — gestures: bound at v3; which devices could make them.
  begin W5
  start_client probe-w5 "$PROBE" gestures --exit-after 5
  W5_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-w5.log" "^gestures bound" 4
  sleep 0.3
  stop_client "$W5_PID"; forget_client "$W5_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want Y; then
  # Screen capture (design-app-compat-capture Part D). grim speaks ext-image-copy-capture,
  # wf-recorder wlr-screencopy; lumen-probe checks what neither tool shows.
  OUT_NAME=$(grep -m1 -oE "output 0 name=\S+" "$COMP_LOG" | sed 's/.*name=//')
  y_shot() { # the compositor's own read-back of the next fully repainted frame: its path
    local n0
    n0=$(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    for _ in $(seq 1 40); do
      (( $(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG") > n0 )) && break
      sleep 0.1
    done
    grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
  }
  y_ppm() { # png ppm — a PNG converted for the verdict (no imaging module needed there)
    ffmpeg -loglevel error -y -i "$1" "$2" 2>/dev/null
  }

  # Y1 — grim of the output: the PNG is the output's size and, pixel for pixel, the frame the
  # compositor reads back itself; the probe window's colour at its place.
  begin Y1
  start_client probe-y1-red "$PROBE" solid --rgba 255,0,0,255 --size 320x240 --exit-after 120
  Y_RED_PID=$LAST_PID
  Y_RED_ID=$(wait_map lumen-probe 5) && settle "$Y_RED_ID"
  inject move 20 $((OUT_H - 20)); sleep 0.5
  ( cd "$LOG_DIR" && WAYLAND_DISPLAY="$SOCKET" timeout 10 grim -o "$OUT_NAME" Y1.png ) \
    >"$LOG_DIR/grim-y1.log" 2>&1
  Y1_SHOT=$(y_shot)
  y_ppm "$LOG_DIR/Y1.png" "$LOG_DIR/Y1.ppm"
  finish id="${Y_RED_ID:-0}" shot="\"${Y1_SHOT}\""

  # Y2 — grim -T of a window covered by another: its own pixels, at its own size.
  begin Y2
  start_client probe-y2-green "$PROBE" solid --rgba 0,255,0,255 --size 480x360 --exit-after 120
  Y_GREEN_PID=$LAST_PID
  Y_GREEN_ID=$(wait_map lumen-probe 5) && settle "$Y_GREEN_ID"
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" toplevels >"$LOG_DIR/probe-y2-list.log" 2>&1
  Y2_TID=$(grep -m1 -oE "^toplevel identifier=[0-9a-f]+ app_id=lumen-probe " \
           "$LOG_DIR/probe-y2-list.log" | sed -E 's/.*identifier=([0-9a-f]+).*/\1/')
  ( cd "$LOG_DIR" && WAYLAND_DISPLAY="$SOCKET" timeout 10 grim -T "${Y2_TID:-none}" Y2.png ) \
    >"$LOG_DIR/grim-y2.log" 2>&1
  y_ppm "$LOG_DIR/Y2.png" "$LOG_DIR/Y2.ppm"
  finish red="${Y_RED_ID:-0}" green="${Y_GREEN_ID:-0}"

  # Y3 — grim -c paints the pointer, grim without it does not (the pointer is a plane).
  begin Y3
  inject move 300 200; sleep 0.5
  ( cd "$LOG_DIR" && WAYLAND_DISPLAY="$SOCKET" timeout 10 grim -c -o "$OUT_NAME" Y3c.png &&
    WAYLAND_DISPLAY="$SOCKET" timeout 10 grim -o "$OUT_NAME" Y3n.png ) >"$LOG_DIR/grim-y3.log" 2>&1
  y_ppm "$LOG_DIR/Y3c.png" "$LOG_DIR/Y3c.ppm"
  y_ppm "$LOG_DIR/Y3n.png" "$LOG_DIR/Y3n.ppm"
  finish x=300 y=200
  stop_client "$Y_GREEN_PID"; forget_client "$Y_GREEN_PID"
  stop_client "$Y_RED_PID"; forget_client "$Y_RED_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # Y4 — a screencast of an animating client (dma-buf and shm): the first frame full, later
  # ones only the moving square's damage; the window's colour where nothing moves.
  begin Y4
  start_client probe-y4-anim "$PROBE" animate --size 300x200 --exit-after 60
  Y4_PID=$LAST_PID
  Y4_ID=$(wait_map lumen-probe-animate 5) && settle "$Y4_ID"
  read -r Y4X Y4Y _ _ < <(geom "$Y4_ID")
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --output "$OUT_NAME" --dmabuf --frames 12 \
    --timeout 8 --expect "0,128,255@$((Y4X + 5)),$((Y4Y + 5))" >"$LOG_DIR/probe-y4-dmabuf.log" 2>&1
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --output "$OUT_NAME" --frames 8 \
    --timeout 8 --expect "0,128,255@$((Y4X + 5)),$((Y4Y + 5))" >"$LOG_DIR/probe-y4-shm.log" 2>&1
  stop_client "$Y4_PID"; forget_client "$Y4_PID"
  finish id="${Y4_ID:-0}"

  # Y5 — a covered window's screencast keeps its frames coming (paced like a shown window,
  # not the 1 s throttle of covered windows).
  begin Y5
  start_client shm-y5 weston-simple-shm
  Y5_SHM_PID=$LAST_PID
  Y5_ID=$(wait_map org.freedesktop.weston.simple-shm 5)
  start_client probe-y5-cover "$PROBE" solid --rgba 0,0,255,255 --size 1200x800 --exit-after 60
  Y5_COVER_PID=$LAST_PID
  Y5_COVER=$(wait_map lumen-probe 5) && settle "$Y5_COVER"
  sleep 0.5
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --app-id org.freedesktop.weston.simple-shm \
    --frames 40 --timeout 6 >"$LOG_DIR/probe-y5.log" 2>&1
  stop_client "$Y5_COVER_PID"; forget_client "$Y5_COVER_PID"
  stop_client "$Y5_SHM_PID"; forget_client "$Y5_SHM_PID"
  finish id="${Y5_ID:-0}" cover="${Y5_COVER:-0}"

  # Y6 — the session lock: a capture from the request on is the black curtain, no frame
  # while locked, the desktop again after unlock.
  begin Y6
  start_client probe-y6-red "$PROBE" solid --rgba 255,0,0,255 --size 320x240 --exit-after 60
  Y6_PID=$LAST_PID
  Y6_ID=$(wait_map lumen-probe 5) && settle "$Y6_ID"
  read -r Y6X Y6Y _ _ < <(geom "$Y6_ID")
  start_client probe-y6-lock "$PROBE" lock --unlock-after 3
  Y6_LOCK_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-y6-lock.log" "^lock locked" 3
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --output "$OUT_NAME" --frames 2 --timeout 1.5 \
    --expect "0,0,0@$((Y6X + 10)),$((Y6Y + 10))" >"$LOG_DIR/probe-y6-locked.log" 2>&1
  for _ in $(seq 1 50); do kill -0 "$Y6_LOCK_PID" 2>/dev/null || break; sleep 0.1; done
  forget_client "$Y6_LOCK_PID"
  sleep 0.5
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --output "$OUT_NAME" --frames 1 --timeout 3 \
    --expect "255,0,0@$((Y6X + 10)),$((Y6Y + 10))" >"$LOG_DIR/probe-y6-unlocked.log" 2>&1
  stop_client "$Y6_PID"; forget_client "$Y6_PID"
  finish id="${Y6_ID:-0}"

  # Y7 — wf-recorder (wlr-screencopy): a 3 s recording of the output has frames.
  begin Y7
  if command -v wf-recorder >/dev/null && command -v ffprobe >/dev/null; then
    start_client probe-y7-anim "$PROBE" animate --size 300x200 --exit-after 20
    Y7_PID=$LAST_PID
    wait_map lumen-probe-animate 5 >/dev/null
    rm -f "$LOG_DIR/Y7.mkv"
    ( cd "$LOG_DIR" && WAYLAND_DISPLAY="$SOCKET" timeout -s INT 3 wf-recorder -f Y7.mkv ) \
      >"$LOG_DIR/wf-recorder-y7.log" 2>&1
    ffprobe -v error -count_frames -select_streams v:0 \
      -show_entries stream=width,height,nb_read_frames -of csv=p=0 "$LOG_DIR/Y7.mkv" \
      >"$LOG_DIR/ffprobe-y7.log" 2>&1
    stop_client "$Y7_PID"; forget_client "$Y7_PID"
    finish skipped=0
  else
    finish skipped=1
  fi

  # Y8 — the toplevel list and a pointer cursor session.
  begin Y8
  start_client probe-y8-win "$PROBE" solid --rgba 0,0,255,255 --size 200x150 --exit-after 30
  Y8_PID=$LAST_PID
  Y8_ID=$(wait_map lumen-probe 5) && settle "$Y8_ID"
  # 3 s: the steps below take about a second (two injections), and the window's `closed`
  # must come before the watch ends (with 1 s it raced them).
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" toplevels --watch 3 >"$LOG_DIR/probe-y8-list.log" 2>&1 &
  Y8_LIST_PID=$!
  start_client probe-y8-cursor "$PROBE" cursor-capture --output "$OUT_NAME" --frames 1 --timeout 3
  Y8_CUR_PID=$LAST_PID
  sleep 0.5
  inject move 400 300; sleep 0.2; inject move 420 310
  stop_client "$Y8_PID"; forget_client "$Y8_PID"
  wait "$Y8_LIST_PID" 2>/dev/null
  for _ in $(seq 1 40); do kill -0 "$Y8_CUR_PID" 2>/dev/null || break; sleep 0.1; done
  stop_client "$Y8_CUR_PID"; forget_client "$Y8_CUR_PID"
  finish id="${Y8_ID:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want Z; then
  # Portals (design-app-compat-capture Part E): xdg-desktop-portal with Lumen's backend and
  # xdg-desktop-portal-wlr, driven by lumen-portal-probe; the chooser and the overlay are
  # answered with injected keys and clicks.
  : > "$LOG_DIR/portals.log"
  rm -rf "$LOG_DIR/Z-pictures"; mkdir -p "$LOG_DIR/Z-pictures"
  rm -f "$LOG_DIR"/Z*.png "$LOG_DIR"/Z*.ppm
  if [[ -x "$PORTAL_PROBE" ]] &&
     XDG_PICTURES_DIR="$LOG_DIR/Z-pictures" XDG_CONFIG_HOME="$LOG_DIR/Z-config" \
       portal_session_start "$SOCKET" "$BUILD" "$LOG_DIR" >>"$LOG_DIR/portals.log" 2>&1; then
    z_probe() { # log-name args... — lumen-portal-probe, its output in LOG_DIR/<log-name>.log
      local log="$LOG_DIR/$1.log"; shift
      "$PORTAL_PROBE" "$@" >"$log" 2>&1
    }
    z_bg() { # log-name args... — the same in the background; Z_BG is its PID
      local log="$LOG_DIR/$1.log"; shift
      "$PORTAL_PROBE" "$@" >"$log" 2>&1 &
      Z_BG=$!
    }
    z_wait() { # pid seconds
      for _ in $(seq 1 $(( $2 * 10 ))); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
      kill -0 "$1" 2>/dev/null && { kill -TERM "$1" 2>/dev/null; sleep 0.2; }
      wait "$1" 2>/dev/null
    }
    z_overlay() { # wait until the portal's overlay (or the chooser) has mapped since $1 lines
      local from=$1
      for _ in $(seq 1 60); do
        tail -n +"$((from + 1))" "$COMP_LOG" | grep -q "layer map surface=[0-9]* layer=overlay namespace=lumen-portal" && return 0
        sleep 0.05
      done
      return 1
    }
    z_shot() { # the compositor's own read-back of the next fully repainted frame: its path
      local n0
      n0=$(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG")
      kill -USR1 "$COMP_PID" 2>/dev/null
      for _ in $(seq 1 40); do
        (( $(grep -c "screenshot [0-9]*x[0-9]* written to " "$COMP_LOG") > n0 )) && break
        sleep 0.1
      done
      grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
    }
    z_png() { # log → the PNG it names, converted to PPM at $2
      local f
      f=$(sed -n 's/^file \(.*\) size=.*/\1/p' "$LOG_DIR/$1.log" | tail -1)
      [[ -n "$f" ]] && ffmpeg -loglevel error -y -i "$f" "$2" 2>/dev/null
    }

    # wait_map looks from BEGIN_COMP on: from now, not from the previous group's begin, or it
    # can return that group's lumen-probe (already gone) before this one has mapped.
    BEGIN_COMP=$(lines "$COMP_LOG")
    start_client probe-z-red "$PROBE" solid --rgba 255,0,0,255 --size 320x240 --exit-after 300
    Z_RED_PID=$LAST_PID
    Z_RED_ID=$(wait_map lumen-probe 5) && settle "$Z_RED_ID"
    read -r ZX ZY ZW ZH < <(geom "${Z_RED_ID:-0}")
    SCEN=park; inject move 20 $((OUT_H - 20)); sleep 0.5

    # Z1 — non-interactive, straight to the backend (no permission store involved).
    begin Z1
    z_probe portal-z1 screenshot --backend --timeout 10
    Z1_SHOT=$(z_shot)
    z_png portal-z1 "$LOG_DIR/Z1.ppm"
    finish id="${Z_RED_ID:-0}" shot="\"${Z1_SHOT}\""

    # Z2 — interactive through the frontend: drag 200x150 inside the red window.
    begin Z2
    n=$(lines "$COMP_LOG")
    z_bg portal-z2 screenshot --interactive --timeout 20
    Z2_PID=$Z_BG
    if z_overlay "$n"; then
      sleep 0.3
      inject move $((ZX + 40)) $((ZY + 40)); sleep 0.2
      inject drag $((ZX + 40)) $((ZY + 40)) $((ZX + 240)) $((ZY + 190)) --button left --steps 30
    fi
    z_wait "$Z2_PID" 10
    z_png portal-z2 "$LOG_DIR/Z2.ppm"
    finish w=200 h=150

    # Z3 — Enter, a click, Escape.
    begin Z3
    n=$(lines "$COMP_LOG")
    z_bg portal-z3-enter screenshot --interactive --timeout 20
    Z3_PID=$Z_BG
    z_overlay "$n" && { sleep 0.3; inject key KEY_ENTER tap; }
    z_wait "$Z3_PID" 10
    n=$(lines "$COMP_LOG")
    z_bg portal-z3-click screenshot --interactive --timeout 20
    Z3_PID=$Z_BG
    z_overlay "$n" && { sleep 0.3; inject move 60 $((OUT_H - 60)); sleep 0.2; inject button left click; }
    z_wait "$Z3_PID" 10
    n=$(lines "$COMP_LOG")
    z_bg portal-z3-esc screenshot --interactive --timeout 20
    Z3_PID=$Z_BG
    z_overlay "$n" && { sleep 0.3; inject key KEY_ESC tap; }
    z_wait "$Z3_PID" 10
    finish skipped=0
    SCEN=park; inject move 20 $((OUT_H - 20))

    # Z4 — PickColor: a click on the red window.
    begin Z4
    n=$(lines "$COMP_LOG")
    z_bg portal-z4 pick-color --timeout 20
    Z4_PID=$Z_BG
    if z_overlay "$n"; then
      sleep 0.3
      inject move $((ZX + ZW / 2)) $((ZY + ZH / 2)); sleep 0.2
      inject move $((ZX + ZW / 2 + 3)) $((ZY + ZH / 2 + 3)); sleep 0.2
      inject button left click
    fi
    z_wait "$Z4_PID" 10
    finish skipped=0
    SCEN=park; inject move 20 $((OUT_H - 20))

    # Z5 — ScreenCast of the output, chosen in Lumen's chooser; a moving client keeps
    # frames coming. The probe offers dma-buf first like OBS and browsers (xdpw falls back to
    # shm where its GBM cannot allocate with modifiers, as on vmwgfx).
    begin Z5
    start_client probe-z5-anim "$PROBE" animate --size 200x120 --interval 16 --exit-after 60
    Z5_ANIM_PID=$LAST_PID
    Z5_ANIM_ID=$(wait_map lumen-probe-animate 5) && settle "$Z5_ANIM_ID"
    n=$(lines "$COMP_LOG")
    z_bg portal-z5 screencast --types 3 --seconds 3 --timeout 20 --dmabuf \
      --expect "255,0,0@$((ZX + 10)),$((ZY + 10))"
    Z5_PID=$Z_BG
    z_overlay "$n" && { sleep 0.5; inject key KEY_ENTER tap; }
    z_wait "$Z5_PID" 15
    finish anim="${Z5_ANIM_ID:-0}"

    # Z6 — ScreenCast of a window: Down selects the first window line of the chooser (the
    # verdict reads which one was chosen and expects that window's size).
    begin Z6
    n=$(lines "$COMP_LOG")
    z_bg portal-z6 screencast --types 3 --seconds 2 --timeout 20
    Z6_PID=$Z_BG
    z_overlay "$n" && { sleep 0.5; inject key KEY_DOWN tap; sleep 0.2; inject key KEY_ENTER tap; }
    z_wait "$Z6_PID" 15
    stop_client "$Z5_ANIM_PID"; forget_client "$Z5_ANIM_PID"
    finish anim_w=200 anim_h=120 red_w="${ZW:-0}" red_h="${ZH:-0}"

    # Z7 — Screenshot of a window (target Window): the only window left is the red one.
    begin Z7
    n=$(lines "$COMP_LOG")
    z_bg portal-z7 screenshot --backend --target 2 --timeout 20
    Z7_PID=$Z_BG
    z_overlay "$n" && { sleep 0.3; inject key KEY_ENTER tap; }
    z_wait "$Z7_PID" 10
    z_png portal-z7 "$LOG_DIR/Z7.ppm"
    finish w="${ZW:-0}" h="${ZH:-0}"

    # Z8 — locked: refused; unlocked: works again.
    begin Z8
    start_client probe-z8-lock "$PROBE" lock --unlock-after 3
    Z8_LOCK_PID=$LAST_PID
    wait_for "$LOG_DIR/probe-z8-lock.log" "^lock locked" 3
    z_probe portal-z8-locked screenshot --backend --timeout 10
    for _ in $(seq 1 50); do kill -0 "$Z8_LOCK_PID" 2>/dev/null || break; sleep 0.1; done
    forget_client "$Z8_LOCK_PID"
    sleep 0.5
    z_probe portal-z8-unlocked screenshot --backend --timeout 10
    finish skipped=0

    stop_client "$Z_RED_PID"; forget_client "$Z_RED_PID"
    SCEN=park; inject move 20 $((OUT_H - 20))
  else
    begin Z1
    finish skipped=1
  fi
  portal_session_stop >>"$LOG_DIR/portals.log" 2>&1
fi

if want R; then
  # R — the configuration file (design-session-and-wm Part C). The compositor runs with
  # LUMEN_CONFIG=$R_CFG from the start (absent: the defaults, as for every other group); the
  # file is written here, by rename as editors do, and the inotify watch applies it.
  R_DIR="$LOG_DIR/R-config"; R_CFG="$R_DIR/config.toml"; mkdir -p "$R_DIR"
  r_write() { # repeat-rate [extra text] — the R configuration, written atomically
    cat >"$R_CFG.new" <<CFG
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"
repeat-rate = $1

[input.mouse]
accel-profile = "flat"
accel-speed = 0.5
natural-scroll = true
scroll-factor = 2.0

[cursor]
size = 48

[bindings]
"Super+Y" = "window.close"
${2:-}
CFG
    mv "$R_CFG.new" "$R_CFG"
  }
  start_client shell-r env LUMEN_LOG=debug "$SHELL_BIN"
  R_SHELL_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-r.log" "key bindings: the compositor's" 5

  # R1 — the file appears: repeat 10/600 and a two-layout keymap reach a new client, the
  # mouse gets flat acceleration and natural scrolling, the cursor 48 px; Super+Y (a
  # configured binding) closes the focused window.
  begin R1
  r_write 10
  comp_since "config: generation [0-9]+ from .* \(0 errors, 0 warnings" 5
  comp_since "compositor.cursor: cursor theme=.* size=48 \(configuration\)" 3
  start_client probe-r1 "$PROBE" keyboard --exit-after 60
  R1_PID=$LAST_PID
  R1_ID=$(wait_map lumen-probe 5) && settle "$R1_ID"
  wait_for "$LOG_DIR/probe-r1.log" "^focus in" 3
  inject key KEY_LEFTMETA+KEY_Y tap
  wait_for "$LOG_DIR/probe-r1.log" "^close requested" 3
  stop_client "$R1_PID"; forget_client "$R1_PID"
  finish id="${R1_ID:-0}"

  # R2 — the layout toggle (grp:alt_shift_toggle): y, then Alt+Shift, then y again is z
  # under the German layout, as a client's own xkb sees it.
  begin R2
  start_client probe-r2 "$PROBE" keyboard --exit-after 90
  R2_PID=$LAST_PID
  R2_ID=$(wait_map lumen-probe 5) && settle "$R2_ID"
  wait_for "$LOG_DIR/probe-r2.log" "^focus in" 3
  inject type y; sleep 0.3
  inject key KEY_LEFTALT+KEY_LEFTSHIFT tap; sleep 0.3
  inject type y; sleep 0.3
  inject key KEY_LEFTALT+KEY_LEFTSHIFT tap; sleep 0.3
  finish id="${R2_ID:-0}"

  # R3 — a live change (rename): repeat 30 reaches the client soon after the save, one new
  # generation, no notice (nothing was wrong).
  begin R3
  T0=$(date +%s%N)
  r_write 30
  wait_for "$LOG_DIR/probe-r2.log" "^repeat rate=30 " 3
  T1=$(date +%s%N)
  sleep 0.5
  finish ms=$(( (T1 - T0) / 1000000 )) notices="$(grep -c 'config: notice:' "$LOG_DIR/shell-r.log")"

  # R4 — the mouse as configured: a wheel notch up scrolls with natural direction (positive
  # values), relative motion is accelerated flat by 1.5 (accel-speed 0.5).
  begin R4
  if [[ -n "${R2_ID:-}" && "${R2_ID:-0}" != 0 ]]; then
    read -r X Y W H < <(geom "$R2_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.3
  fi
  inject wheel 1; sleep 0.3
  inject rel 10 0 --count 3; sleep 0.3
  finish

  # R5 — a syntax error: rejected, the configuration in use stays (repeat 30 is not
  # resent), lumen-ctl reload exits 1 naming the line, the shell shows one notice.
  begin R5
  printf '[input.keyboard\nrepeat-rate = 5\n' >"$R_CFG.new"; mv "$R_CFG.new" "$R_CFG"
  comp_since "config: generation [0-9]+ from .* rejected" 3
  WAYLAND_DISPLAY="$SOCKET" LUMEN_SOCKET="$XDG_RUNTIME_DIR/$SOCKET.varlink" \
    timeout 5 "$BUILD/lumen-ctl" reload >"$LOG_DIR/R5.reload" 2>&1
  R5_RC=$?
  wait_for "$LOG_DIR/shell-r.log" "config: notice: Lumen configuration not used" 3
  sleep 0.3
  finish rc="$R5_RC" notices="$(grep -c 'config: notice:' "$LOG_DIR/shell-r.log")" \
    repeats="$(grep -c '^repeat ' "$LOG_DIR/probe-r2.log")"

  # R6 — a bad value and an unknown key: that key keeps its value (30), the rest applies,
  # one notice; then the file goes and every default returns (repeat 25).
  begin R6
  r_write 5000 '"Super+K" = "window.closee"'
  comp_since "config: generation [0-9]+ from .* \(2 errors, 0 warnings" 3
  sleep 0.3
  R6_MARK=$(lines "$COMP_LOG")
  rm -f "$R_CFG"
  wait_for "$LOG_DIR/probe-r2.log" "^repeat rate=25 " 3
  stop_client "$R2_PID"; forget_client "$R2_PID"
  stop_client "$R_SHELL_PID"; forget_client "$R_SHELL_PID"
  finish removed_mark="$R6_MARK"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want Q; then
  # Q — workspaces (design-session-and-wm Part W). Default bindings: Super+N switches,
  # Super+Shift+N moves the focused window there (following), Super+H minimizes.
  start_client shell-q env LUMEN_LOG=debug "$SHELL_BIN"
  Q_SHELL_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-q.log" "key bindings: the compositor's" 5
  sleep 0.5
  q_ws() { # name [args...] — lumen-probe workspaces into probe-NAME.log
    local n=$1; shift
    WAYLAND_DISPLAY="$SOCKET" timeout 8 "$PROBE" workspaces "$@" >"$LOG_DIR/probe-$n.log" 2>&1
  }
  q_shot() { # → the path of a screenshot of the next frame
    local before
    before=$(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    local deadline=$(( $(date +%s%N) + 4000000000 ))
    while (( $(date +%s%N) < deadline )); do
      (( $(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG") > before )) && break
      sleep 0.05
    done
    grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
  }
  inject move $((OUT_W / 2)) $((OUT_H / 2)); sleep 0.2

  # Q1 — three windows, Super+2 and back.
  begin Q1
  for n in a b c; do
    start_client "probe-q1$n" "$PROBE" window --app-id "q1$n" --title "q1$n" --exit-after 120
    eval "Q1${n^^}_PID=\$LAST_PID"
    eval "Q1${n^^}_ID=\$(wait_map q1$n 5)"
    eval "settle \"\$Q1${n^^}_ID\""
  done
  # The first one gets the keyboard by a click on a corner the others leave free.
  if [[ "${Q1A_ID:-0}" != 0 ]]; then
    read -r X Y W H < <(geom "$Q1A_ID")
    inject move $((X + 6)) $((Y + 6)); sleep 0.2
    inject button left click; sleep 0.4
    inject move 20 $((OUT_H - 20)); sleep 0.2
  fi
  Q1_MARK=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA+KEY_2 tap; sleep 0.6
  q_ws q1-on2 --watch 0.3
  Q1_BACK=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA+KEY_1 tap; sleep 0.6
  finish a="${Q1A_ID:-0}" b="${Q1B_ID:-0}" c="${Q1C_ID:-0}" mark="$Q1_MARK" back="$Q1_BACK" \
    la="$(lines "$LOG_DIR/probe-q1a.log")" lb="$(lines "$LOG_DIR/probe-q1b.log")" \
    lc="$(lines "$LOG_DIR/probe-q1c.log")"

  # Q2 — Super+Shift+2: the focused window (q1a) moves to 2, and the view follows.
  begin Q2
  inject key KEY_LEFTMETA+KEY_LEFTSHIFT+KEY_2 tap; sleep 0.8
  q_ws q2 --watch 0.3
  finish a="${Q1A_ID:-0}" b="${Q1B_ID:-0}" la="$(lines "$LOG_DIR/probe-q1a.log")" \
    lb="$(lines "$LOG_DIR/probe-q1b.log")"

  # Q3 — ext-workspace activate from a client.
  begin Q3
  q_ws q3 --activate 1 --watch 0.6
  sleep 0.3
  finish

  # Q4 — a fullscreen window and its workspace.
  begin Q4
  start_client probe-q4 "$PROBE" window --app-id q4 --title q4 --fullscreen --exit-after 120
  Q4_PID=$LAST_PID
  Q4_ID=$(wait_map q4 5) && settle "$Q4_ID"
  sleep 0.5
  Q4_S1=$(q_shot)
  inject key KEY_LEFTMETA+KEY_2 tap; sleep 0.8
  Q4_S2=$(q_shot)
  inject key KEY_LEFTMETA+KEY_1 tap; sleep 0.8
  Q4_S3=$(q_shot)
  stop_client "$Q4_PID"; forget_client "$Q4_PID"
  sleep 0.5
  finish id="${Q4_ID:-0}" s1="\"$Q4_S1\"" s2="\"$Q4_S2\"" s3="\"$Q4_S3\""

  # Q5 — minimize: Super+H, the panel entry brings it back; then xdg set_minimized.
  begin Q5
  inject move 20 $((OUT_H - 20)); sleep 0.2
  if [[ "${Q1C_ID:-0}" != 0 ]]; then
    read -r X Y W H < <(geom "$Q1C_ID")
    inject move $((X + W - 8)) $((Y + H - 8)); sleep 0.2
    inject button left click; sleep 0.4
  fi
  Q5_MIN=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA+KEY_H tap; sleep 0.8
  Q5_CELL=$(grep -oE 'layout idx=[0-9]+ x=[0-9]+ w=[0-9]+ handle=[0-9]+ title="q1c"' "$LOG_DIR/shell-q.log" | tail -1)
  Q5_CLICK=$(lines "$COMP_LOG")
  if [[ -n "$Q5_CELL" ]]; then
    CX=$(sed -E 's/.* x=([0-9]+) w=([0-9]+).*/\1/' <<<"$Q5_CELL")
    CW=$(sed -E 's/.* x=([0-9]+) w=([0-9]+).*/\2/' <<<"$Q5_CELL")
    inject move $((CX + CW / 2)) 16; sleep 0.3
    inject button left click; sleep 0.8
  fi
  start_client probe-q5 "$PROBE" window --app-id q5 --title q5 --minimize-after 600 --exit-after 60
  Q5_PID=$LAST_PID
  Q5_ID=$(wait_map q5 5)
  wait_for "$LOG_DIR/probe-q5.log" "^minimize" 4; sleep 0.6
  stop_client "$Q5_PID"; forget_client "$Q5_PID"
  finish c="${Q1C_ID:-0}" min="$Q5_MIN" click="$Q5_CLICK" q5="${Q5_ID:-0}" cell="$([[ -n "$Q5_CELL" ]] && echo 1 || echo 0)"

  # Q6 — a window on a workspace that is not shown is still captured.
  begin Q6
  inject move $((OUT_W / 2)) $((OUT_H / 2)); sleep 0.2
  start_client probe-q6-anim "$PROBE" animate --size 240x160 --exit-after 60
  Q6_PID=$LAST_PID
  Q6_ID=$(wait_map lumen-probe-animate 5) && settle "$Q6_ID"
  inject key KEY_LEFTMETA+KEY_3 tap; sleep 0.6
  WAYLAND_DISPLAY="$SOCKET" "$PROBE" capture --app-id lumen-probe-animate --frames 10 \
    --timeout 6 >"$LOG_DIR/probe-q6.log" 2>&1
  inject key KEY_LEFTMETA+KEY_1 tap; sleep 0.4
  stop_client "$Q6_PID"; forget_client "$Q6_PID"
  finish id="${Q6_ID:-0}"

  # Q7 — locked: workspace keys and ext-workspace requests change nothing.
  begin Q7
  start_client probe-q7-lock "$PROBE" lock --unlock-after 3
  Q7_LOCK_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-q7-lock.log" "^lock locked" 3
  Q7_LOCKED=$(lines "$COMP_LOG")
  inject key KEY_LEFTMETA+KEY_2 tap; sleep 0.3
  q_ws q7 --activate 2 --watch 0.3
  for _ in $(seq 1 50); do kill -0 "$Q7_LOCK_PID" 2>/dev/null || break; sleep 0.1; done
  forget_client "$Q7_LOCK_PID"
  sleep 0.5
  q_ws q7-after --watch 0.2
  finish locked="$Q7_LOCKED"

  for v in Q1A_PID Q1B_PID Q1C_PID; do
    [[ -n "${!v:-}" ]] && { stop_client "${!v}"; forget_client "${!v}"; }
  done
  stop_client "$Q_SHELL_PID"; forget_client "$Q_SHELL_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want O; then
  # O — switching and snapping (design-session-and-wm Part K). Default bindings: Alt+Tab and
  # Alt+Shift+Tab (held: the compositor gives lumen-shell a hold while Alt is down),
  # Super+arrows, Alt+F7/Alt+F8; edge snapping by a Super+drag. Positions the compositor logs
  # are logical; the injector takes output pixels (× --scale).
  start_client shell-o env LUMEN_LOG=debug "$SHELL_BIN"
  O_SHELL_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-o.log" "key bindings: the compositor's" 5
  sleep 0.5
  lpx() { awk -v v="$1" -v s="$SCALE" 'BEGIN { printf "%d", v * s + 0.5 }'; }
  o_shot() { # → the path of a screenshot of the next frame
    local before
    before=$(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    local deadline=$(( $(date +%s%N) + 4000000000 ))
    while (( $(date +%s%N) < deadline )); do
      (( $(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG") > before )) && break
      sleep 0.05
    done
    grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
  }
  shl() { lines "$LOG_DIR/shell-o.log"; }
  inject move 20 $((OUT_H - 20)); sleep 0.2
  for n in a b c; do
    start_client "probe-o$n" "$PROBE" window --app-id "o$n" --title "o$n" --size 400x300 \
      --keyboard --exit-after 240
    eval "O${n^^}_PID=\$LAST_PID"
    eval "O${n^^}_ID=\$(wait_map o$n 5)"
    eval "settle \"\$O${n^^}_ID\""
  done
  sleep 0.3

  # O1 — a quick Alt+Tab (released within the open delay): the previous window (ob) gets
  # the keyboard, nothing is drawn.
  begin O1
  O1_SH=$(shl)
  inject key KEY_LEFTALT+KEY_TAB tap; sleep 0.7
  finish a="${OA_ID:-0}" b="${OB_ID:-0}" c="${OC_ID:-0}" sh0="$O1_SH" sh1="$(shl)"

  # O2 — Alt held, Tab, wait (the switcher is drawn), Tab, Alt up: the third window of the
  # MRU order (oa) gets the keyboard. A screenshot while it is shown.
  begin O2
  O2_SH=$(shl)
  inject key KEY_LEFTALT press; sleep 0.05
  inject key KEY_TAB tap; sleep 0.6
  O2_SHOT=$(o_shot)
  inject key KEY_TAB tap; sleep 0.3
  inject key KEY_LEFTALT release; sleep 0.7
  finish a="${OA_ID:-0}" sh0="$O2_SH" sh1="$(shl)" shot="\"$O2_SHOT\""

  # O3 — Alt+Tab, then Escape: nothing changes; the focused application (oa) saw Alt only —
  # no leave, no Tab, no Escape.
  begin O3
  O3_SH=$(shl)
  O3_LA=$(lines "$LOG_DIR/probe-oa.log")
  inject key KEY_LEFTALT press; sleep 0.05
  inject key KEY_TAB tap; sleep 0.5
  inject key KEY_ESC tap; sleep 0.2
  inject key KEY_LEFTALT release; sleep 0.6
  finish a="${OA_ID:-0}" sh0="$O3_SH" sh1="$(shl)" la0="$O3_LA" la1="$(lines "$LOG_DIR/probe-oa.log")"

  # O4 — Alt+Shift+Tab: backwards, the least recent window (oc).
  begin O4
  O4_SH=$(shl)
  inject key KEY_LEFTALT+KEY_LEFTSHIFT+KEY_TAB tap; sleep 0.7
  finish c="${OC_ID:-0}" sh0="$O4_SH" sh1="$(shl)"

  # O5 — Super+arrows on oc: left half, top-left quarter, left half, restore; maximize,
  # restore. Every step's configure (size, tiled states) from the window's side.
  begin O5
  O5_LC=$(lines "$LOG_DIR/probe-oc.log")
  read -r O5_X O5_Y O5_W O5_H < <(geom "${OC_ID:-0}")
  for k in LEFT UP DOWN RIGHT UP DOWN; do
    inject key "KEY_LEFTMETA+KEY_$k" tap; sleep 0.3
    settle "${OC_ID:-0}" >/dev/null
  done
  sleep 0.3
  finish c="${OC_ID:-0}" lc0="$O5_LC" lc1="$(lines "$LOG_DIR/probe-oc.log")" \
    x0="${O5_X:-0}" y0="${O5_Y:-0}" w0="${O5_W:-0}" h0="${O5_H:-0}"

  # O6 — Super+drag oc (on top) to the left edge: the preview shows the left half while held (a
  # screenshot then), the release snaps it there. O6b: drag it out — restored to 400x300
  # under the pointer.
  begin O6
  O6_LB=$(lines "$LOG_DIR/probe-oc.log")
  O6_PRE=$(o_shot)
  read -r X Y W H < <(geom "${OC_ID:-0}")
  PX=$((X + W / 2)); PY=$((Y + H / 2))
  inject move "$(lpx $PX)" "$(lpx $PY)"; sleep 0.2
  inject drag "$(lpx $PX)" "$(lpx $PY)" 0 "$(lpx $PY)" --button left --super --steps 40 \
    --interval-ms 10 --hold-ms 1500 &
  O6_DRAG=$!
  sleep 1.3
  O6_SHOT=$(o_shot)
  wait "$O6_DRAG"
  settle "${OC_ID:-0}" >/dev/null
  O6_MID=$(lines "$COMP_LOG")
  O6_MIDB=$(lines "$LOG_DIR/probe-oc.log")
  read -r X Y W H < <(geom "${OC_ID:-0}")
  PX=$((X + W / 2)); PY=$((Y + H / 2))
  inject move "$(lpx $PX)" "$(lpx $PY)"; sleep 0.2
  inject drag "$(lpx $PX)" "$(lpx $PY)" "$(lpx $((PX + 300)))" "$(lpx $((PY + 40)))" \
    --button left --super --steps 40
  settle "${OC_ID:-0}" >/dev/null
  sleep 0.3
  finish b="${OC_ID:-0}" lb0="$O6_LB" lbm="$O6_MIDB" lb1="$(lines "$LOG_DIR/probe-oc.log")" \
    mid="$O6_MID" py="$PY" pre="\"$O6_PRE\"" shot="\"$O6_SHOT\""

  # O7 — Alt+F7 on oc (focused): Right ×3, Down, Return moves it by
  # (30, 10); the arrows and Return never reach it.
  begin O7
  inject move 20 $((OUT_H - 20)); sleep 0.2
  O7_LB=$(lines "$LOG_DIR/probe-oc.log")
  read -r O7_X O7_Y O7_W O7_H < <(geom "${OC_ID:-0}")
  inject key KEY_LEFTALT+KEY_F7 tap; sleep 0.3
  for k in RIGHT RIGHT RIGHT DOWN; do inject key "KEY_$k" tap; sleep 0.12; done
  inject key KEY_ENTER tap; sleep 0.4
  finish b="${OC_ID:-0}" x0="${O7_X:-0}" y0="${O7_Y:-0}" lb0="$O7_LB" lb1="$(lines "$LOG_DIR/probe-oc.log")"

  # O8 — Alt+F8: Right ×2 picks the right edge (+20), Down the bottom one (+10), Return.
  begin O8
  read -r O8_X O8_Y O8_W O8_H < <(geom "${OC_ID:-0}")
  inject key KEY_LEFTALT+KEY_F8 tap; sleep 0.3
  for k in RIGHT RIGHT DOWN; do inject key "KEY_$k" tap; sleep 0.15; done
  inject key KEY_ENTER tap; sleep 0.3
  settle "${OC_ID:-0}" >/dev/null
  finish b="${OC_ID:-0}" x0="${O8_X:-0}" y0="${O8_Y:-0}" w0="${O8_W:-0}" h0="${O8_H:-0}"

  # O9 — the session locks during a hold: the compositor cancels it, the switcher closes
  # without activating anything, and nothing changes once unlocked.
  begin O9
  O9_SH=$(shl)
  inject key KEY_LEFTALT press; sleep 0.05
  inject key KEY_TAB tap; sleep 0.4
  start_client probe-o9-lock "$PROBE" lock --unlock-after 2
  O9_LOCK_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-o9-lock.log" "^lock locked" 3
  inject key KEY_LEFTALT release; sleep 0.2
  for _ in $(seq 1 40); do kill -0 "$O9_LOCK_PID" 2>/dev/null || break; sleep 0.1; done
  forget_client "$O9_LOCK_PID"
  sleep 0.5
  finish c="${OC_ID:-0}" sh0="$O9_SH" sh1="$(shl)"

  # O11 — a click on a window during a hold: the user went elsewhere — the hold is cancelled,
  # the clicked window gets the keyboard, letting go of Alt activates nothing.
  begin O11
  O11_SH=$(shl)
  read -r X Y W H < <(geom "${OA_ID:-0}")
  inject key KEY_LEFTALT press; sleep 0.05
  inject key KEY_TAB tap; sleep 0.4
  # oa's bottom-left corner: the others (cascaded right of it, oc moved up by O6–O8) leave it.
  inject move "$(lpx $((X + 6)))" "$(lpx $((Y + H - 6)))"; sleep 0.2
  inject button left click; sleep 0.3
  inject key KEY_LEFTALT release; sleep 0.5
  inject move 20 $((OUT_H - 20)); sleep 0.2
  finish a="${OA_ID:-0}" sh0="$O11_SH" sh1="$(shl)"

  # O10 — no shell: Alt+Tab is the compositor's quick switch (the topmost other window).
  stop_client "$O_SHELL_PID"; forget_client "$O_SHELL_PID"
  sleep 0.5
  begin O10
  inject key KEY_LEFTALT+KEY_TAB tap; sleep 0.5
  finish before="${OA_ID:-0}"

  for v in OA_PID OB_PID OC_PID; do
    [[ -n "${!v:-}" ]] && { stop_client "${!v}"; forget_client "${!v}"; }
  done
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

if want N; then
  # N — notifications and the tray (design-session-and-wm Part N). lumen-shell owns
  # org.freedesktop.Notifications and org.kde.StatusNotifierWatcher on the user bus;
  # notify-send (libnotify) and lumen-probe sni are the applications; dbus-monitor records
  # every Notifications signal; gdbus and busctl make the protocol-level calls. The shell's
  # state (next id, do-not-disturb) goes to this run's directory, never the user's.
  N_STATE="$LOG_DIR/N-shell-state"
  rm -f "$N_STATE"
  start_client shell-n env LUMEN_LOG=debug LUMEN_SHELL_STATE="$N_STATE" "$SHELL_BIN"
  N_SHELL_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-n.log" "bus name org.freedesktop.Notifications: owned" 8
  wait_for "$LOG_DIR/shell-n.log" "tray: watcher and host" 3
  start_client n-monitor dbus-monitor --session \
    "type='signal',interface='org.freedesktop.Notifications'"
  N_MON_PID=$LAST_PID
  sleep 0.5
  nsh() { lines "$LOG_DIR/shell-n.log"; }
  nmon() { lines "$LOG_DIR/n-monitor.log"; }
  NGD=(gdbus call --session --dest org.freedesktop.Notifications
       --object-path /org/freedesktop/Notifications --method)
  n_card() { # id → "LX LY BOX CLOSE BUTTONS": the stack's origin on the output, then the
              # card's rects in the stack (x,y,w,h; buttons separated by ';')
    local id=$1 lx ly line
    read -r lx ly < <(grep -oE "compositor.layer: layer map surface=[0-9]+ layer=\w+ namespace=lumen-notifications x=-?[0-9]+ y=-?[0-9]+" \
                      "$COMP_LOG" | tail -1 | sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+)/\1 \2/')
    line=$(grep -oE "notifications: card id=$id box=\S+ close=\S+ buttons=\S+" "$LOG_DIR/shell-n.log" | tail -1)
    [[ -n "$line" && -n "$lx" ]] || return 1
    sed -E "s/.* box=(\S+) close=(\S+) buttons=(\S+)/$lx $ly \1 \2 \3/" <<<"$line"
  }
  n_shot() { # → the path of a screenshot of the next frame
    local before
    before=$(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG")
    kill -USR1 "$COMP_PID" 2>/dev/null
    local deadline=$(( $(date +%s%N) + 4000000000 ))
    while (( $(date +%s%N) < deadline )); do
      (( $(grep -c "screenshot [0-9]*x[0-9]* written to" "$COMP_LOG") > before )) && break
      sleep 0.05
    done
    grep -oE "screenshot [0-9]+x[0-9]+ written to \S+" "$COMP_LOG" | tail -1 | sed 's/.* to //'
  }
  n_at() { # "lx ly card-line" part → global x y of the centre of box/close/button N
    local lx=$1 ly=$2 rect=$3
    IFS=, read -r x y w h <<<"$rect"
    echo "$((lx + x + w / 2)) $((ly + y + h / 2))"
  }

  # N1 — a normal notification expires (reason 1); a critical one stays (critical-timeout 0)
  # and puts the stack on the overlay layer; CloseNotification closes it (reason 3); an unknown
  # id is an error.
  begin N1
  N1_SH=$(nsh); N1_MON=$(nmon)
  N1_ID=$(notify-send -p -a smoke -t 1500 "n1 normal" "expires in 1.5 s")
  N1C_ID=$(notify-send -p -a smoke -u critical "n1 critical" "stays until closed")
  sleep 2.5
  "${NGD[@]}" org.freedesktop.Notifications.CloseNotification "${N1C_ID:-0}" >"$LOG_DIR/N1-close.out" 2>&1
  N1_RC=$?
  "${NGD[@]}" org.freedesktop.Notifications.CloseNotification 999999 >"$LOG_DIR/N1-close2.out" 2>&1
  N1_RC2=$?
  sleep 0.5
  finish id="${N1_ID:-0}" cid="${N1C_ID:-0}" rc="$N1_RC" rc2="$N1_RC2" sh0="$N1_SH" sh1="$(nsh)" \
    mon0="$N1_MON" mon1="$(nmon)"

  # N2 — replaces_id: the same id comes back, the card is replaced in place (one card).
  begin N2
  N2_SH=$(nsh)
  N2_ID=$(notify-send -p -a smoke -t 0 "n2 first" "a")
  sleep 0.4
  N2_ID2=$(notify-send -p -r "${N2_ID:-0}" -a smoke -t 0 "n2 second" "b")
  sleep 0.6
  "${NGD[@]}" org.freedesktop.Notifications.CloseNotification "${N2_ID:-0}" >/dev/null 2>&1
  sleep 0.4
  finish id="${N2_ID:-0}" id2="${N2_ID2:-0}" sh0="$N2_SH" sh1="$(nsh)"

  # N3 — an action button clicked: ActivationToken, ActionInvoked, NotificationClosed(2), in
  # that order; notify-send prints the action. N3b: a click on the card body without a
  # default action dismisses it (2).
  begin N3
  N3_SH=$(nsh); N3_MON=$(nmon)
  start_client n3-send notify-send -p -a smoke -A ok=OK -A cancel=Cancel "n3 action" "click OK"
  N3_PID=$LAST_PID
  for _ in $(seq 1 40); do grep -qE "notifications: card id=[0-9]+ .* buttons=[0-9]" "$LOG_DIR/shell-n.log" && break; sleep 0.05; done
  sleep 0.3
  N3_ID=$(grep -oE 'notify id=[0-9]+ app="smoke" .*summary="n3 action"' "$LOG_DIR/shell-n.log" |
          tail -1 | sed -E 's/notify id=([0-9]+).*/\1/')
  N3_CARD=$(n_card "${N3_ID:-0}") || true
  if [[ -n "$N3_CARD" ]]; then
    read -r LX LY _ _ BTNS <<<"$N3_CARD"
    read -r BX BY < <(n_at "$LX" "$LY" "${BTNS%%;*}")
    inject move "$BX" "$BY"; sleep 0.3
    inject button left click; sleep 0.8
  fi
  for _ in $(seq 1 30); do kill -0 "$N3_PID" 2>/dev/null || break; sleep 0.1; done
  forget_client "$N3_PID"
  N3B_ID=$(notify-send -p -a smoke -t 0 "n3b body click" "dismissed by a click")
  sleep 0.6
  N3B_CARD=$(n_card "${N3B_ID:-0}") || true
  if [[ -n "$N3B_CARD" ]]; then
    read -r LX LY BOX _ <<<"$N3B_CARD"
    IFS=, read -r CX CY _ CH <<<"$BOX"
    inject move $((LX + CX + 30)) $((LY + CY + CH - 6)); sleep 0.3
    inject button left click; sleep 0.6
  fi
  finish id="${N3_ID:-0}" bid="${N3B_ID:-0}" sh0="$N3_SH" sh1="$(nsh)" mon0="$N3_MON" mon1="$(nmon)"
  SCEN=park; inject move 20 $((OUT_H - 20)); sleep 0.2

  # N4 — locked: a notification waits (not shown, its timer stopped) and pops up after the
  # unlock.
  begin N4
  N4_SH=$(nsh)
  start_client probe-n4-lock "$PROBE" lock --unlock-after 3 --exit-after 20
  N4_LOCK_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-n4-lock.log" "^lock locked" 3
  N4_ID=$(notify-send -p -a smoke -t 2000 "n4 locked" "after the unlock")
  sleep 1
  N4_MID=$(nsh)
  for _ in $(seq 1 40); do kill -0 "$N4_LOCK_PID" 2>/dev/null || break; sleep 0.1; done
  forget_client "$N4_LOCK_PID"
  sleep 3
  finish id="${N4_ID:-0}" sh0="$N4_SH" shm="$N4_MID" sh1="$(nsh)"

  # N5 — do not disturb while the screen is captured: a normal one goes to the history, a
  # critical one still pops up; afterwards off. N5b: a middle click on the bell sets it, a
  # second clears it.
  begin N5
  N5_SH=$(nsh)
  OUT_NAME=${OUT_NAME:-$(grep -m1 -oE "output 0 name=\S+" "$COMP_LOG" | sed 's/.*name=//')}
  start_client probe-n5-capture "$PROBE" capture --output "$OUT_NAME" --frames 100000 --timeout 6
  N5_CAP_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-n.log" "do not disturb on \(screen captured\)" 3
  N5_ID=$(notify-send -p -a smoke "n5 quiet" "history only")
  N5C_ID=$(notify-send -p -a smoke -u critical "n5 critical" "shown anyway")
  sleep 0.6
  stop_client "$N5_CAP_PID"; forget_client "$N5_CAP_PID"
  wait_for "$LOG_DIR/shell-n.log" "notifications: do not disturb off" 3
  "${NGD[@]}" org.freedesktop.Notifications.CloseNotification "${N5C_ID:-0}" >/dev/null 2>&1
  read -r BELLX BELLW < <(grep -oE "shell.panel: layout bell x=[0-9]+ w=[0-9]+" "$LOG_DIR/shell-n.log" |
                          tail -1 | sed -E 's/.*x=([0-9]+) w=([0-9]+)/\1 \2/')
  if [[ -n "${BELLX:-}" ]]; then
    inject move $((BELLX + BELLW / 2)) 16; sleep 0.3
    inject button middle click; sleep 0.4
    N5B_ID=$(notify-send -p -a smoke "n5b quiet" "manual do not disturb")
    sleep 0.4
    inject button middle click; sleep 0.4
  fi
  finish id="${N5_ID:-0}" cid="${N5C_ID:-0}" bid="${N5B_ID:-0}" sh0="$N5_SH" sh1="$(nsh)"
  SCEN=park; inject move 20 $((OUT_H - 20)); sleep 0.2

  # N6 — the tray: lumen-probe sni (pixmap icon, a dbusmenu menu) appears in the panel; a left
  # click → ProvideXdgActivationToken then Activate with that token; busctl lists it on the
  # watcher; a right click → its menu (an xdg_popup of the panel with a grab), Down, Return →
  # Event(1, "clicked"); the wheel → Scroll.
  begin N6
  N6_SH=$(nsh)
  start_client probe-n6-sni "$PROBE" sni --pixmap --title "Smoke item" --exit-after 120
  N6_PID=$LAST_PID
  wait_for "$LOG_DIR/probe-n6-sni.log" "^sni registered watcher=" 3
  wait_for "$LOG_DIR/shell-n.log" "shell.panel: layout tray key=[0-9]+ x=[0-9]+" 3
  sleep 0.4
  busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
    org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems >"$LOG_DIR/N6-busctl.out" 2>&1
  busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
    org.kde.StatusNotifierWatcher IsStatusNotifierHostRegistered >>"$LOG_DIR/N6-busctl.out" 2>&1
  read -r TRX TRW < <(grep -oE "shell.panel: layout tray key=[0-9]+ x=[0-9]+ w=[0-9]+" "$LOG_DIR/shell-n.log" |
                      tail -1 | sed -E 's/.*x=([0-9]+) w=([0-9]+)/\1 \2/')
  N6_SHOT=""
  if [[ -n "${TRX:-}" ]]; then
    N6_SHOT=$(n_shot)
    inject move $((TRX + TRW / 2)) 16; sleep 0.3
    inject move $((TRX + TRW / 2)) 17; sleep 0.8   # the tooltip after 500 ms
    inject button left click; sleep 0.8
    inject wheel 1; sleep 0.4
    inject button right click; sleep 1
    inject key KEY_DOWN tap; sleep 0.2
    inject key KEY_ENTER tap; sleep 0.8
  fi
  finish tx="${TRX:-0}" tw="${TRW:-0}" sh0="$N6_SH" sh1="$(nsh)" shot="\"${N6_SHOT}\""
  SCEN=park; inject move 20 $((OUT_H - 20)); sleep 0.2

  # N7 — the shell is killed and started again: the item registers with the new watcher
  # within a second; ids continue after the persisted block.
  begin N7
  N7_SN=$(lines "$LOG_DIR/probe-n6-sni.log")
  kill -KILL "$N_SHELL_PID" 2>/dev/null; wait "$N_SHELL_PID" 2>/dev/null; forget_client "$N_SHELL_PID"
  sleep 0.3
  start_client shell-n2 env LUMEN_LOG=debug LUMEN_SHELL_STATE="$N_STATE" "$SHELL_BIN"
  N_SHELL_PID=$LAST_PID
  wait_for "$LOG_DIR/shell-n2.log" "bus name org.kde.StatusNotifierWatcher: owned" 8
  T0=$(date +%s%N)
  for _ in $(seq 1 40); do
    (( $(grep -c "^sni registered watcher=" "$LOG_DIR/probe-n6-sni.log") >= 2 )) && break
    sleep 0.05
  done
  N7_MS=$(( ($(date +%s%N) - T0) / 1000000 ))
  wait_for "$LOG_DIR/shell-n2.log" "tray: item org.kde.StatusNotifierItem" 3
  N7_ID=$(notify-send -p -a smoke -t 1000 "n7 after restart" "a new id")
  sleep 0.3
  finish ms="$N7_MS" id="${N7_ID:-0}" first="${N1_ID:-0}" sn0="$N7_SN" \
    sn1="$(lines "$LOG_DIR/probe-n6-sni.log")"

  # N8 — idle: a history and a tray item, nothing happening: the shell does not wake up.
  begin N8
  sleep 1.5
  N8_S0=$(awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$N_SHELL_PID/status" 2>/dev/null)
  N8_C0=$(awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$COMP_PID/task/$COMP_PID/status" 2>/dev/null)
  sleep 10
  N8_S1=$(awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$N_SHELL_PID/status" 2>/dev/null)
  N8_C1=$(awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$COMP_PID/task/$COMP_PID/status" 2>/dev/null)
  finish shell=$(( ${N8_S1:-0} - ${N8_S0:-0} )) comp=$(( ${N8_C1:-0} - ${N8_C0:-0} ))

  for v in N6_PID N_SHELL_PID N_MON_PID; do
    [[ -n "${!v:-}" ]] && { stop_client "${!v}"; forget_client "${!v}"; }
  done
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

# ── Teardown ─────────────────────────────────────────────────────────────────
for pid in "${CLIENT_PIDS[@]}"; do stop_client "$pid"; done
CLIENT_PIDS=()
stop_compositor
stop_watch
rm -f "$SOCKET_PATH" "$LOCK_PATH"

VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
CONSOLE=$(sudo -n python3 -I -c 'import fcntl,os,struct,sys
fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY); b=bytearray(4)
fcntl.ioctl(fd,0x4B3B,b); print("text" if struct.unpack("i",b)[0]==0 else "graphics")' "/dev/$VT" 2>/dev/null || echo unknown)

# ── Verdict ──────────────────────────────────────────────────────────────────
python3 -I - "$LOG_DIR" "$COMP_RC" "$CONSOLE" "$POINTER_DEV" "$KEYBOARD_DEV" "$OUT_W" "$OUT_H" "$RELATIVE_DEV" "$SCALE" <<'PY'
import json, math, os, re, sys

log_dir, comp_rc, console, pointer_dev, keyboard_dev = sys.argv[1:6]
out_w, out_h = int(sys.argv[6]), int(sys.argv[7])
scale = float(sys.argv[9]) if len(sys.argv) > 9 else 1.0

def read(name):
    try:
        with open(os.path.join(log_dir, name), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []

comp = read("compositor.log")
inj = [json.loads(l) for l in read("inject.jsonl") if l.startswith("{")]
scen = {s["name"]: s for s in (json.loads(l) for l in read("scenarios.jsonl"))}

GEOM = re.compile(r"compositor\.wm: (?:geometry|map) id=(\d+) (?:app_id=\S* )?x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)(?: cause=(\w+) serial=(\d+))?")
CFG = re.compile(r"compositor\.wm: configure id=(\d+) serial=(\d+) w=(-?\d+) h=(-?\d+) states=(\S+) edges=(\d+) watchdog=(\d)")
ACK = re.compile(r"compositor\.wm: ack id=(\d+) serial=(\d+)$")
GBEGIN = re.compile(r"compositor\.wm: grab begin id=(\d+) mode=(\w+) edges=(\d+) source=(\w+)")
GEND = re.compile(r"compositor\.wm: grab end id=(\d+) mode=(\w+) reason=(\w+)")
TREE = re.compile(r"compositor\.scene: tree root=(\d+) kind=(\w+) id=(\d+) cause=(\w+) nodes=(\S+)")
NODE = re.compile(r"(\d+)(\*?)@(-?\d+),(-?\d+),(\d+)x(\d+)")
SUBMAP = re.compile(r"compositor\.surface: subsurface map surface=(\d+) root=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
POPMAP = re.compile(r"compositor\.popup: popup map surface=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
POPPOS = re.compile(r"compositor\.popup: popup position surface=(\d+) parent=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) .*flipped=(\S+) slid=(\S+) resized=(\S+) constraint=\S+ serial=(\d+) reason=(\w+)")
POPGRAB = re.compile(r"compositor\.popup: popup grab surface=(\d+) result=(\w+) reason=(\S+)")
POPDISMISS = re.compile(r"compositor\.popup: popup dismiss surface=(\d+) reason=(\S+)")
POPCREATE = re.compile(r"compositor\.popup: popup create surface=(\d+) parent=(\d+) parent_kind=(\w+)")
POPDESTROY = re.compile(r"compositor\.popup: popup destroy surface=(\d+)")
KBSURF = re.compile(r"compositor\.wm: focus keyboard-surface kind=(\w+) surface=(\d+)")

def rng(s):
    a, b = s["comp"]
    return range(a, b)

def trees(r, tid):
    """(line, root, nodes{sid: (x,y,w,h)}, root_sid, cause) of toplevel `tid` in range r."""
    out = []
    for i in r:
        m = TREE.search(comp[i])
        if not m or int(m.group(3)) != tid or m.group(2) != "toplevel":
            continue
        nodes, root_sid = {}, None
        for n in NODE.finditer(m.group(5)):
            nodes[int(n.group(1))] = tuple(int(n.group(k)) for k in (3, 4, 5, 6))
            if n.group(2):
                root_sid = int(n.group(1))
        out.append((i, int(m.group(1)), nodes, root_sid, m.group(4)))
    return out

def geometry_at(tid, upto):
    g = None
    for i in range(upto):
        m = GEOM.search(comp[i])
        if m and int(m.group(1)) == tid:
            g = tuple(int(m.group(k)) for k in (2, 3, 4, 5))
    return g

results = []

WLDBG = re.compile(r"^\[[\d:.]+\]\s+(?:\{[^}]*\}\s+)?(-> )?(\w+)[#@](\d+)\.(\w+)\((.*)\)\s*$")

def wl_debug(name):
    """WAYLAND_DEBUG=client lines of client log `name`: (is_request, interface, id, message, args)."""
    out = []
    for l in read(f"{name}.log"):
        m = WLDBG.match(l)
        if m:
            out.append((m.group(1) is not None, m.group(2), int(m.group(3)), m.group(4), m.group(5)))
    return out

def wl_presses(dbg):
    """Distinct pointer button presses a client received (some toolkits bind two pointers)."""
    return {a.split(",")[0].strip() for req, i, _, msg, a in dbg
            if not req and i == "wl_pointer" and msg == "button" and a.replace(" ", "").endswith(",1")}

def wl_popups(dbg):
    """The client's xdg_popups in creation order: {"configures": [(x,y,w,h)], "done": bool,
    "done_seq": int}; object ids are reused after destroy, so they are followed in order."""
    pops, cur, seq = [], {}, 0
    for req, i, oid, msg, a in dbg:
        if req and i == "xdg_surface" and msg == "get_popup":
            m = re.search(r"new id xdg_popup[#@](\d+)", a)
            if m:
                pops.append({"configures": [], "done": False, "done_seq": -1})
                cur[int(m.group(1))] = pops[-1]
        elif not req and i == "xdg_popup" and oid in cur:
            if msg == "configure":
                cur[oid]["configures"].append(tuple(int(v) for v in a.split(",")))
            elif msg == "popup_done":
                cur[oid]["done"] = True
                cur[oid]["done_seq"] = seq
                seq += 1
    return pops

def comp_index(r, rx):
    """Index of the first line in range r matching rx (or the range end)."""
    return next((i for i in r if rx.search(comp[i])), r.stop)

def record(name, ok, detail, warn=False):
    results.append((name, "FAIL" if not ok else "WARN" if warn else "PASS", detail))

def need(name):
    if name not in scen:
        record(name, False, "scenario did not run")
        return None
    return scen[name]

# ── A1: foot CSD ───────────────────────────────────────────────────────────
s = scen.get("A1")
if s is not None:
    tid = s["id"]
    if not tid:
        record("A1 foot CSD maps", False, "foot did not map within 5 s")
    else:
        ts = trees(rng(s), tid)
        root = ts[-1][1] if ts else None
        submaps = [m for i in rng(s) for m in [SUBMAP.search(comp[i])] if m and int(m.group(2)) == (root or -1)]
        above = any(any(v[1] < n[r][1] for k, v in n.items() if k != r)
                    for _, _, n, r, _ in ts if r in n)
        g = geometry_at(tid, s["comp"][1])
        main_h = ts[-1][2][ts[-1][3]][3] if ts and ts[-1][3] in ts[-1][2] else 0
        ok = bool(ts) and len(submaps) >= 4 and above and g and g[3] >= main_h + 20
        record("A1 foot CSD maps", ok,
               f"id={tid} root={root} subsurfaces mapped={len(submaps)} (want >=4), "
               f"title above main surface={above}, geometry h={g[3] if g else '-'} vs main "
               f"surface h={main_h} (want >= +20)")

# ── A2: drag foot's title bar ──────────────────────────────────────────────
s = scen.get("A2")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    b = [m for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    t0 = trees(range(0, s["comp"][0]), tid)
    t1 = trees(rng(s), tid)
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    shifted = False
    if t0 and t1 and dx is not None:
        n0, n1 = t0[-1][2], t1[-1][2]
        common = set(n0) & set(n1)
        shifted = bool(common) and all(n1[k][0] - n0[k][0] == dx and n1[k][1] - n0[k][1] == dy
                                       for k in common)
    # A CSD client starts the move a few motion events in (drag threshold, round trip),
    # so the window follows the pointer from there: a small shortfall is expected.
    ok = (len(b) == 1 and b[0].group(2) == "move" and b[0].group(4) == "client"
          and dx is not None and 0 <= s["dx"] - dx <= 12 and 0 <= s["dy"] - dy <= 8
          and g0[2:] == g1[2:] and shifted)
    record("A2 foot CSD title drag moves", ok,
           f"grab={b[0].group(4) + '/' + b[0].group(2) if b else '-'}; window delta "
           f"{dx},{dy} (pointer +{s['dx']},+{s['dy']}); every node shifted by it={shifted}")

# ── A2s: Super+drag foot ───────────────────────────────────────────────────
s = scen.get("A2s")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    b = [m for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    t0, t1 = trees(range(0, s["comp"][0]), tid), trees(rng(s), tid)
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    shifted = False
    if t0 and t1 and dx is not None:
        n0, n1 = t0[-1][2], t1[-1][2]
        common = set(n0) & set(n1)
        shifted = len(common) >= 5 and all(n1[k][0] - n0[k][0] == dx and n1[k][1] - n0[k][1] == dy
                                           for k in common)
    ok = (len(b) == 1 and b[0].group(4) == "keybinding" and (dx, dy) == (s["dx"], s["dy"])
          and shifted)
    record("A2s foot Super+drag moves the whole tree", ok,
           f"grab={b[0].group(4) if b else '-'}; window delta {dx},{dy} (want {s['dx']},{s['dy']}); "
           f"every node shifted by it={shifted}")

# ── A3: resize from foot's CSD border ──────────────────────────────────────
s = scen.get("A3")
if s is not None:
    tid = s["id"]
    b = [(i, m) for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    e = [(i, m) for i in rng(s) for m in [GEND.search(comp[i])] if m and int(m.group(1)) == tid]
    lo = b[0][0] if b else s["comp"][0]
    hi = s["comp"][1]
    checked, bad, first_bad = 0, 0, ""
    for i, _, nodes, rsid, cause in trees(range(lo, hi), tid):
        g = geometry_at(tid, i)
        if not g:
            continue
        # The title bar: the subsurface at the window geometry's top-left corner.
        title = [v for k, v in nodes.items() if k != rsid and v[0] == g[0] and v[1] == g[1]]
        checked += 1
        if len(title) != 1 or title[0][2] != g[2]:
            bad += 1
            first_bad = first_bad or comp[i]
    # Each geometry change answers a configure the client acked, and no configure goes out
    # while one is unanswered (beyond the watchdog).
    acked, inflight_ok, unanswered = set(), True, None
    commits, commit_ok = 0, True
    for i in range(lo, hi):
        if (m := ACK.search(comp[i])) and int(m.group(1)) == tid:
            acked.add(int(m.group(2)))
            if unanswered is not None and int(m.group(2)) >= unanswered:
                unanswered = None
        elif (m := CFG.search(comp[i])) and int(m.group(1)) == tid:
            if unanswered is not None and m.group(7) == "0":
                inflight_ok = False
            unanswered = int(m.group(2))
        elif (m := GEOM.search(comp[i])) and int(m.group(1)) == tid and m.group(6) == "commit":
            commits += 1
            if int(m.group(7)) not in acked:
                commit_ok = False
    ok = (len(b) == 1 and b[0][1].group(2) == "resize" and b[0][1].group(4) == "client"
          and e and checked > 2 and bad == 0 and inflight_ok and commit_ok and commits > 2)
    record("A3 foot CSD border resize, frame-perfect", ok,
           f"grab={b[0][1].group(4) + '/' + b[0][1].group(2) + ' edges=' + b[0][1].group(3) if b else '-'}; "
           f"{checked} tree dumps, title width != window width in {bad}; {commits} commits, "
           f"all answer acked configures={commit_ok}, one configure in flight={inflight_ok}"
           + (f"; first mismatch: {first_bad}" if first_bad else ""))

# ── A1–A3 from foot's side (WAYLAND_DEBUG=client) ─────────────────────────
if "A1" in scen:
    dbg = wl_debug("foot-csd")
    subs = sum(1 for req, i, _, msg, _ in dbg if req and i == "wl_subcompositor" and msg == "get_subsurface")
    moves = sum(1 for req, i, _, msg, _ in dbg if req and i == "xdg_toplevel" and msg == "move")
    resizes = [a.split(",")[-1].strip() for req, i, _, msg, a in dbg
               if req and i == "xdg_toplevel" and msg == "resize"]
    want_moves = 1 if "A2" in scen else 0
    want_resizes = ["10"] if "A3" in scen else []
    ok = bool(dbg) and subs >= 4 and moves == want_moves and resizes == want_resizes
    record("A1-A3 foot's side: subsurfaces created, move and resize requested", ok,
           f"get_subsurface={subs} (want >=4), xdg_toplevel.move={moves} (want {want_moves}), "
           f"xdg_toplevel.resize edges={resizes} (want {want_resizes}: bottom_right)")

# ── A4: lumen-square --sub / --sub-desync ──────────────────────────────────
for mode in ("sub", "sub-desync"):
    s = scen.get(f"A4-{mode}")
    if s is None:
        continue
    tid = s["id"]
    ts = trees(rng(s), tid)
    if not tid or not ts:
        record(f"A4 lumen-square --{mode}", False, "did not map")
        continue
    same, differ, alone = 0, 0, 0
    prev = None
    for _, _, nodes, rsid, cause in ts:
        others = [v for k, v in nodes.items() if k != rsid]
        if rsid not in nodes or len(others) != 1:
            continue
        r, c = nodes[rsid], others[0]
        if c[2] == r[2] and c[0] == r[0] and c[1] == r[1]:
            same += 1
        else:
            differ += 1
        if prev and cause == "commit" and c[2] != prev[1][2] and r[2] == prev[0][2]:
            alone += 1
        prev = (r, c)
    # lumen-square prints every strip commit; each width it committed must reach the screen
    # (a dump), in --sub only together with a window of that width.
    client_w = {int(m.group(1)) for l in read(f"square-{mode}.log")
                if (m := re.match(r"sub commit size=(\d+)x\d+", l))}
    shown_w = {[v for k, v in n.items() if k != r][0][2] for _, _, n, r, _ in ts
               if r in n and len(n) == 2}
    missing = sorted(client_w - shown_w)
    if mode == "sub":
        ok = same > 2 and differ == 0 and len(client_w) > 2 and not missing
        detail = (f"{same + differ} tree dumps: strip width == window width and at 0,0 in {same}, not in {differ}; "
                  f"client committed {len(client_w)} strip widths, never shown: {missing or 'none'}")
    else:
        ok = alone > 0 and len(client_w) > 2 and not missing
        detail = (f"{len(ts)} tree dumps; strip resized on its own (window unchanged) in {alone}; "
                  f"client committed {len(client_w)} strip widths, never shown: {missing or 'none'}")
    record(f"A4 lumen-square --{mode}", ok, detail)

# ── A5: GTK ────────────────────────────────────────────────────────────────
s = scen.get("A5")
if s is not None:
    ok = s["id"] > 0 and s["alive"] == 1
    record("A5 gtk4-widget-factory maps (cairo)", ok, f"id={s['id']} alive after settle={s['alive'] == 1}")

# ── B: popups ──────────────────────────────────────────────────────────────
def found(rx, r):
    return [m for i in r for m in [rx.search(comp[i])] if m]

def probe_lines(name):
    return read(f"probe-{name}.log")

s = scen.get("B1")
if s is not None:
    tid = s["id"]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    cfg = [l for l in probe_lines("b1") if l.startswith("popup configure idx=0 ")]
    maps = found(POPMAP, rng(s))
    ok = (bool(g) and len(cfg) == 1 and cfg[0].startswith("popup configure idx=0 x=10 y=30 w=200 h=100 ")
          and len(maps) == 1 and (int(maps[0].group(2)), int(maps[0].group(3))) == (g[0] + 10, g[1] + 30))
    record("B1 positioner base placement", ok,
           f"probe configure: {cfg[0] if cfg else '-'} (want x=10 y=30 w=200 h=100); "
           f"map at {maps[0].group(2) + ',' + maps[0].group(3) if maps else '-'} "
           f"(want window {g[0] if g else '?'}+10,{g[1] if g else '?'}+30)")

for adj, what in ((4, "flip"), (1, "slide"), (16, "resize")):
    s = scen.get(f"B2-{adj}")
    if s is None:
        continue
    tid = s["id"]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    pos = found(POPPOS, rng(s))
    maps = found(POPMAP, rng(s))
    if not (g and pos and maps):
        record(f"B2 {what}_x at the output edge", False, "no window, position or map line")
        continue
    p, m = pos[-1], maps[-1]
    mx, mw = int(m.group(2)), int(m.group(4))
    edge_ok = g[0] + g[2] == out_w - 50
    if adj == 4:
        ok = p.group(7) == "x" and mx + mw == g[0] + 390
        want = f"flipped=x, popup right {mx + mw} == anchor rect left {g[0] + 390}"
    elif adj == 1:
        ok = p.group(8) == "x" and mx + mw == out_w
        want = f"slid=x, popup right {mx + mw} == output right {out_w}"
    else:
        ok = p.group(9) == "x" and mx == g[0] + 400 and mw == out_w - (g[0] + 400)
        want = f"resized=x, w={mw} == space left {out_w - (g[0] + 400)}"
    record(f"B2 {what}_x at the output edge", ok and edge_ok,
           f"{want}; window right edge {g[0] + g[2]} (want {out_w - 50})")

s = scen.get("B3")
if s is not None:
    pl = probe_lines("b3")
    grabs = found(POPGRAB, rng(s))
    creates = found(POPCREATE, rng(s))
    out_at = s.get("outside", 0) or s["comp"][1]
    before = range(s["comp"][0], out_at)
    after = range(out_at, s["comp"][1])
    third = creates[2].group(1) if len(creates) >= 3 else "?"
    kb_before = found(KBSURF, before)
    dismissed = [m.group(1) for m in found(POPDISMISS, after) if m.group(2) == "outside-click"]
    order = [l.split("idx=")[1] for l in pl if l.startswith("popup_done ")]
    tail = pl[next((i for i, l in enumerate(pl) if l.startswith("popup_done")), len(pl)):]
    leaked = [l for l in tail if l.startswith(("press ", "release "))]
    kb_after = found(KBSURF, after)
    keys = [i for i, l in enumerate(pl) if l.startswith("key code=30 state=1")]
    enters = [(i, l.split("surface=")[1]) for i, l in enumerate(pl) if l.startswith("keyboard enter")]
    key_targets = [max(((j, t) for j, t in enters if j < k), default=(0, "-"))[1] for k in keys]
    ok = (len(grabs) == 3 and all(m.group(2) == "ok" for m in grabs)
          and kb_before and kb_before[-1].group(1) == "popup" and kb_before[-1].group(2) == third
          and dismissed == [c.group(1) for c in reversed(creates[:3])] and order == ["2", "1", "0"]
          and not leaked and kb_after and kb_after[-1].group(1) == "toplevel"
          and key_targets == ["popup2", "toplevel"])
    record("B3 grab chain, outside click dismisses top-down", ok,
           f"grabs ok={sum(m.group(2) == 'ok' for m in grabs)}/3; keyboard before the click on "
           f"{kb_before[-1].group(1) + ' ' + kb_before[-1].group(2) if kb_before else '-'} (want popup {third}); "
           f"popup_done order {','.join(order) or '-'} (want 2,1,0); click reached a client={bool(leaked)}; "
           f"KEY_A went to {','.join(key_targets) or '-'} (want popup2,toplevel)")

s = scen.get("B4")
if s is not None:
    pl = probe_lines("b4")
    grabs = found(POPGRAB, rng(s))
    ok = (len(grabs) == 1 and grabs[0].group(2) == "denied" and grabs[0].group(3) == "serial"
          and "popup_done idx=0" in pl)
    record("B4 stale grab serial denied", ok,
           f"grab {grabs[0].group(2) + '/' + grabs[0].group(3) if grabs else '-'} (want denied/serial); "
           f"probe popup_done={'popup_done idx=0' in pl}")

s = scen.get("B5-grab")
if s is not None:
    dis = found(POPDISMISS, rng(s))
    b = found(GBEGIN, rng(s))
    ok = (len(b) == 1 and b[0].group(4) == "keybinding" and len(dis) == 1
          and dis[0].group(2) == "wm-grab" and "popup_done idx=0" in probe_lines("b5-grab"))
    record("B5 Super+drag ends the popup grab", ok,
           f"grab begin source={b[0].group(4) if b else '-'}; dismiss reason="
           f"{dis[0].group(2) if dis else '-'} (want wm-grab)")

s = scen.get("B5-follow")
if s is not None:
    tid = s["id"]
    maps = found(POPMAP, rng(s))
    g0 = geometry_at(tid, comp_index(rng(s), POPMAP)) if maps else None
    g1 = geometry_at(tid, s["comp"][1]) if tid else None
    psid = int(maps[0].group(1)) if maps else -1
    moves = [m for i in rng(s) for m in [TREE.search(comp[i])] if m and m.group(2) == "popup"
             and int(m.group(1)) == psid and m.group(4) == "move"]
    last = None
    if moves:
        n = NODE.search(moves[-1].group(5))
        last = (int(n.group(3)), int(n.group(4)))
    first = (int(maps[0].group(2)), int(maps[0].group(3))) if maps else None
    wd = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else None
    pd = (last[0] - first[0], last[1] - first[1]) if last and first else None
    ok = wd == (s["dx"], s["dy"]) and pd == wd and len(moves) >= 2
    record("B5 non-grabbing popup follows its window", ok,
           f"window delta {wd} (want {(s['dx'], s['dy'])}); popup delta {pd}; "
           f"{len(moves)} popup tree moves")

s = scen.get("B6")
if s is not None:
    marks = s.get("marks", [])
    names = ("open+Escape", "open+outside click", "flip at the bottom", "slide at the right")
    if not s["id"] or len(marks) < 5:
        record("B6 GTK context menus", False, "widget-factory did not map")
    for k in range(min(len(marks) - 1, 4)):
        r = range(marks[k], marks[k + 1])
        cr = found(POPCREATE, r)
        if not cr:
            record(f"B6 GTK {names[k]}", True, "no context menu at that point "
                   "(widget-factory's layout differs on this output)", warn=True)
            continue
        sid = cr[0].group(1)
        grab = [m for m in found(POPGRAB, r) if m.group(1) == sid]
        pos = [m for m in found(POPPOS, r) if m.group(1) == sid]
        mp = [m for m in found(POPMAP, r) if m.group(1) == sid]
        kb = found(KBSURF, r)
        g = geometry_at(s["id"], marks[k])
        placed = bool(pos and mp and g) and (int(mp[0].group(2)), int(mp[0].group(3))) == (
            g[0] + int(pos[-1].group(3)), g[1] + int(pos[-1].group(4)))
        inside = bool(mp) and 0 <= int(mp[0].group(2)) and int(mp[0].group(2)) + int(mp[0].group(4)) <= out_w \
            and 0 <= int(mp[0].group(3)) and int(mp[0].group(3)) + int(mp[0].group(5)) <= out_h
        focus = any(m.group(1) == "popup" and m.group(2) == sid for m in kb) and bool(kb) and kb[-1].group(1) == "toplevel"
        ok = (cr[0].group(3) == "toplevel" and grab and grab[0].group(2) == "ok" and placed and inside and focus)
        detail = (f"grab={grab[0].group(2) if grab else '-'}, mapped at window+positioner={placed}, "
                  f"inside the output={inside}, keyboard on it then back on the window={focus}")
        if k == 0:
            # GTK hides a popover by destroying its xdg_popup or by a null-buffer unmap.
            gone = any(m.group(1) == sid for m in found(POPDESTROY, r)) or any(
                f"popup unmap surface={sid} reason=null-buffer" in comp[i] for i in r)
            ok = ok and gone
            detail += f", gone after Escape={gone}"
        elif k == 1:
            dis = [m for m in found(POPDISMISS, r) if m.group(1) == sid]
            ok = ok and bool(dis) and dis[0].group(2) == "outside-click"
            detail += f", dismiss={dis[0].group(2) if dis else '-'}"
        elif k == 2:
            ok = ok and bool(pos) and pos[0].group(7) == "y"
            detail += f", flipped={pos[0].group(7) if pos else '-'} (want y)"
        else:
            ok = ok and bool(pos) and pos[0].group(8) == "x"
            detail += f", slid={pos[0].group(8) if pos else '-'} (want x)"
        record(f"B6 GTK {names[k]}", ok, detail)
    record("B6 widget-factory alive", s.get("alive") == 1, f"alive after the menus={s.get('alive') == 1}")

s = scen.get("B7")
if s is not None:
    r = rng(s)
    out_at = s.get("outside", 0) or s["comp"][1]
    cr = found(POPCREATE, r)
    tid = s["id"]
    if len(cr) < 2:
        # Only a layout difference can make the clicks miss: page 2's menubar is found by
        # position on this output's window size, as B6's text widget.
        record("B7 GTK nested menu chain", len(cr) == 0, f"{len(cr)} popup(s) opened "
               "(widget-factory's page 2 menubar not where it is at 1914x924)", warn=True)
    else:
        p1, p2 = cr[0], cr[1]
        s1, s2 = p1.group(1), p2.group(1)
        grabs = {m.group(1): m.group(2) for m in found(POPGRAB, r)}
        pos = {m.group(1): m for m in found(POPPOS, r)}
        mp = {m.group(1): m for m in found(POPMAP, r)}
        g = geometry_at(tid, s["comp"][0])
        def at(m):
            return (int(m.group(2)), int(m.group(3)))
        placed1 = s1 in pos and s1 in mp and g is not None and \
            at(mp[s1]) == (g[0] + int(pos[s1].group(3)), g[1] + int(pos[s1].group(4)))
        placed2 = s2 in pos and s2 in mp and s1 in mp and \
            at(mp[s2]) == (at(mp[s1])[0] + int(pos[s2].group(3)), at(mp[s1])[1] + int(pos[s2].group(4)))
        kb_before = found(KBSURF, range(s["comp"][0], out_at))
        after = range(out_at, s["comp"][1])
        dismissed = [m.group(1) for m in found(POPDISMISS, after) if m.group(2) == "outside-click"]
        kb_after = found(KBSURF, after)
        ok = (p1.group(3) == "toplevel" and p2.group(3) == "popup" and p2.group(2) == s1
              and grabs.get(s1) == "ok" and grabs.get(s2) == "ok" and placed1 and placed2
              and bool(kb_before) and kb_before[-1].group(1, 2) == ("popup", s2)
              and dismissed == [s2, s1] and bool(kb_after) and kb_after[-1].group(1) == "toplevel")
        record("B7 GTK nested menu chain (menubar Edit > Checks), outside click", ok,
               f"popups {s1} (parent {p1.group(3)}) and {s2} (parent {p2.group(2)}, {p2.group(3)}), "
               f"grabs {grabs.get(s1, '-')}/{grabs.get(s2, '-')}, placed at parent+positioner "
               f"{placed1}/{placed2}, keyboard on {kb_before[-1].group(1, 2) if kb_before else '-'} "
               f"(want popup {s2}), outside-click dismissal order {dismissed} (want [{s2}, {s1}]), "
               f"keyboard back on the window={bool(kb_after) and kb_after[-1].group(1) == 'toplevel'}")

# ── B6/B7 from GTK's side (WAYLAND_DEBUG=client): every popup the compositor created was
# configured with exactly the rect the compositor computed, popup_done reached GTK for
# exactly the popups the compositor dismissed (in the same order), and no outside click
# reached GTK as a button press.
if "B6" in scen and scen["B6"]["id"]:
    dbg = wl_debug("gtk-factory-b6")
    pops = wl_popups(dbg)
    last = scen["B7"]["comp"][1] if "B7" in scen else scen["B6"]["comp"][1]
    r = range(scen["B6"]["comp"][0], last)
    # GTK keeps a popover's wl_surface across shows, so the compositor's surface ids repeat:
    # follow each creation in order, as for the client's object ids.
    comp_pops, cur, dis_order = [], {}, []
    for i in r:
        if (m := POPCREATE.search(comp[i])):
            comp_pops.append([])
            cur[m.group(1)] = len(comp_pops) - 1
        elif (m := POPPOS.search(comp[i])) and m.group(1) in cur:
            comp_pops[cur[m.group(1)]].append(tuple(int(m.group(k)) for k in (3, 4, 5, 6)))
        elif (m := POPDISMISS.search(comp[i])) and m.group(1) in cur:
            dis_order.append(cur[m.group(1)])
    mismatch = [k for k, rects in enumerate(comp_pops)
                if k >= len(pops) or pops[k]["configures"] != rects]
    gtk_done = [k for k, p in sorted(enumerate(pops), key=lambda kp: kp[1]["done_seq"]) if p["done"]]
    presses = len(wl_presses(dbg))
    want_presses = scen["B7"].get("presses", 4) if "B7" in scen else 4
    ok = bool(dbg) and len(pops) == len(comp_pops) and not mismatch and gtk_done == dis_order \
        and presses == want_presses
    record("B6/B7 GTK's side: configures, popup_done, no outside press", ok,
           f"popups compositor/GTK {len(comp_pops)}/{len(pops)}, configure rects differing in "
           f"{len(mismatch)}, popup_done order GTK {gtk_done} vs compositor dismissals {dis_order} "
           f"(popups numbered in creation order), "
           f"button presses GTK received {presses} (want {want_presses}: the clicks inside its "
           f"window, none of the outside ones)")

# ── C: server-side decorations ─────────────────────────────────────────────
DMODE = re.compile(r"compositor\.deco: decoration mode id=(\d+) requested=(\w+) sent=(\w+) serial=(\d+)")
DAPPLY = re.compile(r"compositor\.deco: decoration apply id=(\d+) mode=(\w+) frame=(-?\d+),(-?\d+),(\d+)x(\d+)")
DPRESS = re.compile(r"compositor\.deco: decoration press id=(\d+) zone=(\w+) button=(\d+)")
DTITLE = re.compile(r"compositor\.deco: decoration title id=(\d+) scale=\d+ width=(\d+) ellipsized=(\d) title=\"(.*)\"$")
FPOINT = re.compile(r"compositor\.wm: focus pointer (?:id=(\d+)|decoration id=(\d+) zone=(\w+)|none)")
UNMAP = re.compile(r"compositor\.wm: unmap id=(\d+) ")

def lines_matching(r, rx, tid=None):
    out = []
    for i in r:
        m = rx.search(comp[i])
        if m and (tid is None or int(m.group(1)) == tid):
            out.append((i, m))
    return out

s = scen.get("C1-server")
if s is not None:
    tid = s["id"]
    modes = lines_matching(rng(s), DMODE, tid)
    applies = lines_matching(rng(s), DAPPLY, tid)
    sent_server = [i for i, m in modes if m.group(2) == "server" and m.group(3) == "server"]
    applied = [i for i, m in applies if m.group(2) == "server"]
    f = applies[-1][1] if applies else None
    on_out = f is not None and int(f.group(4)) >= 0 and int(f.group(3)) >= 0 and \
        int(f.group(3)) + int(f.group(5)) <= out_w and int(f.group(4)) + int(f.group(6)) <= out_h
    ok = bool(tid) and bool(sent_server) and bool(applied) and applied[0] > sent_server[0] and on_out
    record("C1 foot negotiates server-side", ok,
           f"id={tid} sent=server at line {sent_server[0] if sent_server else '-'}, applied at "
           f"{applied[0] if applied else '-'}, frame={f.group(3, 4, 5, 6) if f else '-'} on the output={on_out}")

s = scen.get("C1-client")
if s is not None:
    tid = s["id"]
    modes = lines_matching(rng(s), DMODE, tid)
    ts = trees(rng(s), tid)
    nsub = max((len(n) for _, _, n, _, _ in ts), default=0)
    applied_server = lines_matching(rng(s), DAPPLY, tid)
    ok = bool(tid) and bool(modes) and all(m.group(3) == "client" for _, m in modes) and \
        not [m for _, m in applied_server if m.group(2) == "server"] and nsub >= 4
    record("C1 foot insisting on client-side keeps its own frame", ok,
           f"id={tid} sent={[m.group(3) for _, m in modes]} surfaces in its tree={nsub} (want >=4)")

s = scen.get("C1-gtk")
if s is not None:
    tid = s["id"]
    server = [m for _, m in lines_matching(rng(s), DMODE, tid) if m.group(3) == "server"]
    record("C1 gtk4-demo is not told server-side", bool(tid) and not server,
           f"id={tid} server-side configures={len(server)}")

s = scen.get("C2")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    press = lines_matching(rng(s), DPRESS, tid)
    begin_ = [(i, m) for i, m in lines_matching(rng(s), GBEGIN, tid) if m.group(4) == "decoration"]
    # The press is the compositor's: no client had pointer focus from the frame focus line
    # before the press until the grab began.
    leak = False
    if press and begin_:
        leak = any((m := FPOINT.search(comp[i])) and m.group(1) is not None
                   for i in range(press[0][0], begin_[0][0]))
    zone_ok = bool(press) and press[0][1].group(2) == "title"
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    ok = zone_ok and bool(begin_) and begin_[0][1].group(2) == "move" and \
        (dx, dy) == (s["dx"], s["dy"]) and not leak
    record("C2 title bar drag moves", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, grab "
           f"{begin_[0][1].group(2) + ' source=decoration' if begin_ else 'none'}, moved ({dx},{dy}) "
           f"want ({s['dx']},{s['dy']}), client saw the press={leak}")

s = scen.get("C3")
if s is not None:
    tid = s["id"]
    g1 = geometry_at(tid, s["comp"][1])
    press = lines_matching(rng(s), DPRESS, tid)
    begin_ = [m for _, m in lines_matching(rng(s), GBEGIN, tid) if m.group(4) == "decoration"]
    want = s["w0"] + s["dw"]
    # foot sizes to whole cells: the committed width is the configured one rounded down.
    ok = bool(press) and press[0][1].group(2) == "right" and bool(begin_) and \
        begin_[0].group(2) == "resize" and begin_[0].group(3) == "8" and g1 is not None and \
        want - 16 <= g1[2] <= want
    record("C3 resize band drag resizes", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, grab "
           f"{begin_[0].group(2) + ' edges=' + begin_[0].group(3) if begin_ else 'none'}, width "
           f"{s['w0']} -> {g1[2] if g1 else '-'} (want {want - 16}..{want})")

s = scen.get("C4")
if s is not None:
    tid = s["id"]
    press = lines_matching(rng(s), DPRESS, tid)
    close = [i for i in rng(s) if f"compositor.deco: decoration close id={tid}" in comp[i]]
    unmapped = lines_matching(range(s["comp"][0], len(comp)), UNMAP, tid)
    ok = bool(press) and press[0][1].group(2) == "close" and bool(close) and \
        s["exited"] == 1 and bool(unmapped)
    record("C4 close button closes", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, close sent={bool(close)}, "
           f"foot exited within 2 s={s['exited'] == 1}, unmapped={bool(unmapped)}")

# ── C1–C4 from foot's side (WAYLAND_DEBUG=client) ─────────────────────────
if "C1-server" in scen:
    dbg = wl_debug("foot-ssd")
    asked = [a.strip() for req, i, _, msg, a in dbg if req and i == "zxdg_toplevel_decoration_v1" and msg == "set_mode"]
    told = [a.strip() for req, i, _, msg, a in dbg if not req and i == "zxdg_toplevel_decoration_v1" and msg == "configure"]
    presses = len(wl_presses(dbg))
    closes = sum(1 for req, i, _, msg, _ in dbg if not req and i == "xdg_toplevel" and msg == "close")
    want_close = 1 if "C4" in scen else 0
    ok = bool(dbg) and "2" in asked and told[:1] == ["2"] and presses == 0 and closes == want_close
    record("C1-C4 foot's side: server-side told, frame presses never delivered, close received", ok,
           f"set_mode={asked} (2 = server_side), decoration configure={told[:3]} (want 2), "
           f"button presses received={presses} (want 0: every press was on the frame), "
           f"xdg_toplevel.close received={closes} (want {want_close})")

s = scen.get("C5")
if s is not None:
    tid = s["id"]
    titles = lines_matching(rng(s), DTITLE, tid)
    fits = [m for _, m in titles if m.group(4) == "lumen-title-c5" and m.group(3) == "0"]
    room = s["w"] - 2 * 10 - 28
    long_ = [m for _, m in titles if m.group(4).startswith("lumen-title-c5 and then")]
    cut = bool(long_) and long_[-1].group(3) == "1" and int(long_[-1].group(2)) <= room
    record("C5 client titles rasterized, long ones ellipsized", bool(fits) and cut,
           f"id={tid} 'lumen-title-c5' drawn whole={bool(fits)}, long title ellipsized="
           f"{long_[-1].group(3) == '1' if long_ else '-'} width "
           f"{long_[-1].group(2) if long_ else '-'} <= room {room}")

s = scen.get("C6")
if s is not None:
    tid, other = s["id"], s["other"]
    press = lines_matching(rng(s), DPRESS, tid)
    lowered = [i for i in rng(s) if re.search(rf"compositor\.wm: lower id={tid} index=0$", comp[i])]
    # The newer window took the keyboard when it mapped: its frame turned active, the
    # other's inactive.
    active = [i for i in rng(s) if re.search(rf"compositor\.deco: decoration active id={tid} previous={other}$", comp[i])]
    ok = bool(press) and press[-1][1].group(2) == "title" and press[-1][1].group(3) == "274" and \
        bool(lowered) and bool(active)
    record("C6 middle-click on the title lowers", ok,
           f"press zone={press[-1][1].group(2) if press else '-'} button="
           f"{press[-1][1].group(3) if press else '-'}, lowered to the bottom={bool(lowered)}; "
           f"frame active colours moved {other} -> {tid} on map={bool(active)}")

# ── D: layer-shell ─────────────────────────────────────────────────────────
LCREATE = re.compile(r"compositor\.layer: layer create surface=(\d+) layer=(\w+) namespace=(\S+) output=0")
LCFG = re.compile(r"compositor\.layer: layer configure surface=(\d+) serial=(\d+) w=(\d+) h=(\d+)")
LMAP = re.compile(r"compositor\.layer: layer map surface=(\d+) layer=(\w+) namespace=(\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
LEXCL = re.compile(r"compositor\.layer: layer exclusive surface=(\d+) edge=(\w+) zone=(-?\d+)")
LUNMAP = re.compile(r"compositor\.layer: layer unmap surface=(\d+) reason=(\S+)")
LFOCUS = re.compile(r"compositor\.layer: layer focus surface=(\d+) interactivity=(\w+) cause=(\w+)")
USABLE = re.compile(r"compositor\.wm: usable area x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
MAXI = re.compile(r"compositor\.wm: maximize id=(\d+) on=(\d) source=(\w+)")
POPPARENT = re.compile(r"compositor\.popup: popup parent surface=(\d+) parent=(\d+) parent_kind=(\w+)")

def layer_of(r, ns):
    """(surface id, map match) of the layer surface created with namespace `ns` in range r."""
    sid = next((int(m.group(1)) for i in r if (m := LCREATE.search(comp[i])) and m.group(3) == ns), None)
    mp = next((m for i in r if (m := LMAP.search(comp[i])) and int(m.group(1)) == sid), None)
    return sid, mp

def probe_log(name):
    return read(f"{name}.log")

def first_index(r, pred):
    return next((i for i in r if pred(comp[i])), None)

s = scen.get("D1")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d1-panel")
    cfg = [m for i in r if (m := LCFG.search(comp[i])) and int(m.group(1)) == sid]
    excl = [m for i in r if (m := LEXCL.search(comp[i])) and int(m.group(1)) == sid]
    usable = [m.groups() for i in r if (m := USABLE.search(comp[i]))]
    want_u = ("0", "32", str(out_w), str(out_h - 32))
    tid = s["id"]
    gs = [m for i in r if (m := GEOM.search(comp[i])) and int(m.group(1)) == tid]
    map_y = int(gs[0].group(3)) if gs else None
    end_y = int(gs[-1].group(3)) if gs else None
    ok = sid is not None and bool(cfg) and (cfg[0].group(3), cfg[0].group(4)) == (str(out_w), "32") \
        and mp is not None and mp.group(4, 5, 6, 7) == ("0", "0", str(out_w), "32") \
        and bool(excl) and excl[-1].group(2, 3) == ("top", "32") and want_u in usable \
        and map_y is not None and map_y >= 32 and end_y == 32
    record("D1 top panel reserves its zone", ok,
           f"panel surface={sid} configure={cfg[0].group(3, 4) if cfg else '-'} map="
           f"{mp.group(4, 5, 6, 7) if mp else '-'} exclusive={excl[-1].group(2, 3) if excl else '-'} "
           f"usable areas={usable} (want {want_u}); lumen-square mapped at y={map_y} (>=32), "
           f"after a Super+drag 600 px up y={end_y} (want 32)")

s = scen.get("D2")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d2-bg")
    cfg = [m for i in r if (m := LCFG.search(comp[i])) and int(m.group(1)) == sid]
    ok = bool(cfg) and (cfg[0].group(3), cfg[0].group(4)) == (str(out_w), str(out_h)) and \
        mp is not None and mp.group(2) == "background" and \
        mp.group(4, 5, 6, 7) == ("0", "0", str(out_w), str(out_h))
    record("D2 background with zone -1 covers the whole output", ok,
           f"surface={sid} configure={cfg[0].group(3, 4) if cfg else '-'} map="
           f"{mp.group(2, 4, 5, 6, 7) if mp else '-'} (want background 0 0 {out_w} {out_h})")

s = scen.get("D3")
if s is not None:
    sid, mp = layer_of(rng(s), "d3-corner")
    want = (str(out_w - 110), str(out_h - 100), "100", "80")
    record("D3 bottom-right overlay with margins", mp is not None and mp.group(4, 5, 6, 7) == want,
           f"map={mp.group(4, 5, 6, 7) if mp else '-'} want {want}")

def key_lines(lines):
    return [int(m.group(1)) for l in lines if (m := re.match(r"key code=(\d+) state=1", l))]

s = scen.get("D4")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d4-excl")
    tid = s["id"]
    kb = [(i, m.group(1), int(m.group(2))) for i in r if (m := KBSURF.search(comp[i]))]
    to_layer = [i for i, k, x in kb if k == "layer" and x == sid]
    unmap = [i for i in r if (m := LUNMAP.search(comp[i])) and int(m.group(1)) == sid and m.group(2) == "null-buffer"]
    back = [i for i, k, x in kb if k == "toplevel" and unmap and i > unmap[0]]
    stolen = [i for i, k, x in kb if to_layer and unmap and to_layer[0] < i < unmap[0] and k != "layer"]
    excl_keys = key_lines(probe_log("probe-d4-excl"))
    win_keys = key_lines(probe_log("probe-d4-window"))
    deact = [i for i in r if to_layer and i > to_layer[0] and
             (m := CFG.search(comp[i])) and int(m.group(1)) == tid and "activated" not in m.group(5)]
    ok = bool(to_layer) and not stolen and bool(back) and excl_keys == [30, 48] and \
        win_keys == [46] and bool(deact)
    record("D4 exclusive overlay takes the keyboard until it unmaps", ok,
           f"keyboard to the layer={bool(to_layer)}, taken back by the click={bool(stolen)}, "
           f"back to the window after unmap={bool(back)}; overlay got keys {excl_keys} (want [30, 48]), "
           f"window got {win_keys} (want [46]); window deactivated meanwhile={bool(deact)}")

s = scen.get("D5")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d5-ondemand")
    kb = [(i, m.group(1), int(m.group(2))) for i in r if (m := KBSURF.search(comp[i]))]
    early = [i for i, k, x in kb if k == "layer" and i < s["mapped"]]
    focus = [(i, m) for i in r if (m := LFOCUS.search(comp[i]))]
    on_map = [i for i, m in focus if int(m.group(1)) == sid and m.group(3) == "map"]
    released = [i for i, m in focus if int(m.group(1)) == 0 and on_map and i > on_map[0]]
    od_log = probe_log("probe-d5-od")
    od_keys = key_lines(od_log)
    win_keys = key_lines(probe_log("probe-d5-window"))
    ok = bool(early) and bool(on_map) and bool(released) and \
        any(l.startswith("keyboard enter surface=layer") for l in od_log) and \
        od_keys == [30] and win_keys == [48] and mp is not None and int(mp.group(5)) == 32
    record("D5 on_demand: focused on map, released by a click elsewhere", ok,
           f"keys to the layer on map={bool(early)} (cause=map {bool(on_map)}), released={bool(released)}; "
           f"surface keys {od_keys} (want [30]), window keys {win_keys} (want [48]); placed at "
           f"y={mp.group(5) if mp else '-'} under the panel (want 32)")

def maximize_check(name, s, source, ext_top):
    r = rng(s)
    tid, mark = s["id"], s["max"]
    g0 = geometry_at(tid, s["comp"][0] + 1) if tid else None
    # the geometry before the first maximize request
    first_max = next((i for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "1"), None)
    if first_max is not None:
        g0 = geometry_at(tid, first_max)
    on = [m for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "1" and m.group(3) == source]
    off = [m for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "0" and m.group(3) == source]
    want_w, want_h = out_w, out_h - 32 - ext_top
    cfgs = [m for i in r if (m := CFG.search(comp[i])) and int(m.group(1)) == tid and "maximized" in m.group(5)]
    cfg_ok = any((int(m.group(3)), int(m.group(4))) == (want_w, want_h) for m in cfgs)
    gmax = geometry_at(tid, mark) if mark else None
    gend = geometry_at(tid, s["comp"][1])
    placed = gmax is not None and gmax[0] == 0 and gmax[1] == 32 + ext_top and \
        want_w - 16 <= gmax[2] <= want_w and want_h - 24 <= gmax[3] <= want_h
    restored = gend is not None and g0 is not None and gend == g0
    ok = bool(tid) and bool(on) and bool(off) and cfg_ok and placed and restored
    record(name, ok,
           f"id={tid} maximize on/off from {source}={bool(on)}/{bool(off)}, configure "
           f"{[(m.group(3), m.group(4)) for m in cfgs][:2]} want ({want_w},{want_h}); maximized "
           f"geometry {gmax} want x=0 y={32 + ext_top}; restored {gend} want {g0}")

s = scen.get("D6-ssd")
if s is not None:
    maximize_check("D6 maximize fills the usable area (foot, server-side frame)", s, "decoration", 28)
    tid = s["id"]
    gdock = geometry_at(tid, s["dock"]) if s.get("dock") else None
    gundock = geometry_at(tid, s["undock"]) if s.get("undock") else None
    # foot sizes to whole cells, so a height may come out a little short of the configured one.
    dock_ok = gdock is not None and gdock[:2] == (0, 60) and out_h - 60 - 40 - 24 <= gdock[3] <= out_h - 60 - 40
    undock_ok = gundock is not None and gundock[:2] == (0, 60) and out_h - 60 - 24 <= gundock[3] <= out_h - 60
    record("D6 a maximized window follows the usable area", dock_ok and undock_ok,
           f"with a 40 px bottom panel {gdock} (want 0,60 h<={out_h - 100}); after it left {gundock} "
           f"(want 0,60 h<={out_h - 60})")
s = scen.get("D6-gtk")
if s is not None:
    maximize_check("D6 maximize fills the usable area (gtk4-demo, header bar)", s, "client", 0)

s = scen.get("D7")
if s is not None:
    err = any("protocol-error interface=zwlr_layer_surface_v1 code=1" in l for l in probe_log("probe-d7"))
    record("D7 invalid layer state is a protocol error", s["rc"] == 3 and err and s["alive"] == 1,
           f"probe exit={s['rc']} (want 3), invalid_size reported={err}, compositor alive={s['alive'] == 1}")

s = scen.get("D8")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d8-popup")
    parent = [m for i in r if (m := POPPARENT.search(comp[i])) and int(m.group(2)) == sid and m.group(3) == "layer"]
    psid = int(parent[0].group(1)) if parent else None
    grab = [m for i in r if (m := POPGRAB.search(comp[i])) and int(m.group(1)) == psid and m.group(2) == "ok"]
    pos = [m for i in r if (m := POPPOS.search(comp[i])) and int(m.group(1)) == psid]
    pm = [m for i in r if (m := POPMAP.search(comp[i])) and int(m.group(1)) == psid]
    inside = bool(pm) and int(pm[0].group(2)) >= 0 and int(pm[0].group(3)) >= 0 and \
        int(pm[0].group(2)) + int(pm[0].group(4)) <= out_w and int(pm[0].group(3)) + int(pm[0].group(5)) <= out_h
    kb_popup = [i for i in r if (m := KBSURF.search(comp[i])) and m.group(1) == "popup" and int(m.group(2)) == psid]
    dismissed = [m for i in r if (m := POPDISMISS.search(comp[i])) and int(m.group(1)) == psid and m.group(2) == "outside-click"]
    plog = probe_log("probe-d8")
    done = any(l.startswith("popup_done idx=0") for l in plog)
    keys = key_lines(plog)
    ok = psid is not None and bool(grab) and bool(pos) and pos[0].group(7) == "y" and inside and \
        not kb_popup and bool(dismissed) and done and not keys
    record("D8 panel popup: flipped into the output, grab without keyboard, outside click", ok,
           f"popup={psid} of layer surface {sid}, grab ok={bool(grab)}, flipped="
           f"{pos[0].group(7) if pos else '-'} map={pm[0].group(2, 3, 4, 5) if pm else '-'} inside={inside}, "
           f"keyboard on it={bool(kb_popup)} (want no: interactivity none), keys reached the client="
           f"{keys}, dismissed by the outside click={bool(dismissed)}, popup_done={done}")

# ── X: parent destroyed first ──────────────────────────────────────────────
s = scen.get("X-parent-first")
if s is not None:
    ok = s["rc"] == 0 and s["alive"] == 1
    record("X parent wl_surface before child wl_subsurface", ok,
           f"lumen-square exit={s['rc']} (protocol error would be 1), compositor alive={s['alive'] == 1}")

# ── E: lumen-shell ─────────────────────────────────────────────────────────
SH1, SH2 = read("shell.log"), read("shell-restart.log")
BOUND = re.compile(r"compositor\.shell: bridge bound client=\d+ snapshot=(\d+) focus=(\d+)")
BREQ = re.compile(r"compositor\.shell: bridge request=(\w+) id=(\d+) result=(\w+)")
KBID = re.compile(r"compositor\.wm: focus keyboard id=(\d+)$")
SDRAW = re.compile(r"shell\.panel: draw entries=(\d+) hidden=(\d+) focused=(-?\d+) damage=(\d+)(\(full\))?")
SLAYOUT = re.compile(r'shell\.panel: layout idx=(\d+) x=(\d+) w=(\d+) handle=(\d+) title="(.*)"$')
PANELMAP = re.compile(r"compositor\.layer: layer map surface=\d+ layer=top namespace=lumen-panel x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
USABLE = re.compile(r"compositor\.wm: usable area x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")

def sh_lines(log, s):
    return log[s["sh_a"]:s["sh_b"]]

def comp_find(r, rx):
    return [m for i in r if (m := rx.search(comp[i]))]

def layout_titles(lines):
    """idx -> title from the last layout block in `lines`."""
    out = {}
    for l in lines:
        if (m := SLAYOUT.search(l)):
            if m.group(1) == "0":
                out = {}
            out[int(m.group(1))] = m.group(5)
    return out

s = scen.get("E1")
if s is not None:
    r, sh = rng(s), sh_lines(SH1, s)
    bound = comp_find(r, BOUND)
    pm = comp_find(r, PANELMAP)
    usable = [m.group(1, 2, 3, 4) for m in comp_find(r, USABLE)]
    snap = [l for l in sh if "shell.bridge: snapshot toplevels=2" in l]
    focus_after = any("shell.bridge: focus handle=" in l and "none" not in l
                      for l in sh[next((k for k, l in enumerate(sh) if "snapshot toplevels=" in l), len(sh)):])
    draws = [m for l in sh if (m := SDRAW.search(l))]
    titles = layout_titles(sh)
    fidx = int(draws[-1].group(3)) if draws else -1
    ok = bool(bound) and bound[0].group(1) == "2" and bool(pm) and \
        pm[0].group(1, 2, 3, 4) == ("0", "0", str(out_w), "32") and \
        ("0", "32", str(out_w), str(out_h - 32)) in usable and bool(snap) and focus_after and \
        bool(draws) and draws[-1].group(1) == "2" and fidx >= 0 and \
        titles.get(fidx, "lumen-square") != "lumen-square"
    record("E1 shell: snapshot of 2 + focus, 32 px panel, usable area from y=32", ok,
           f"bridge bound snapshot={bound[0].group(1) if bound else '-'}, panel map="
           f"{pm[0].group(1, 2, 3, 4) if pm else '-'}, usable={usable[-1] if usable else '-'}, "
           f"shell snapshot={bool(snap)} focus after it={focus_after}, last draw entries="
           f"{draws[-1].group(1) if draws else '-'} focused idx={fidx} ({titles.get(fidx, '-')!r}; "
           f"want the last mapped window, foot)")

s = scen.get("E2")
if s is not None and "E1" in scen:
    r, sh = rng(s), sh_lines(SH1, s)
    idx = s["idx"]
    titles = layout_titles(SH1[:s["sh_a"]])
    want_id = scen["E1"]["sq"] if titles.get(idx) == "lumen-square" else scen["E1"]["foot"]
    reqs = [m.group(1, 2, 3) for m in comp_find(r, BREQ)]
    kb = [int(m.group(1)) for m in comp_find(r, KBID)]
    draws = [m for l in sh if (m := SDRAW.search(l))]
    clicked = any(f"shell.panel: click idx={idx} " in l for l in sh)
    raised = any(re.search(rf"compositor\.wm: raise id={want_id} index=", comp[i]) for i in r)
    # From the client's side: lumen-square prints every configure it receives; the
    # activating configure the compositor sent during E2 reached it with `activated`.
    sq_ok = True
    if want_id == scen["E1"]["sq"]:
        act = [m.group(2) for i in r if (m := CFG.search(comp[i])) and int(m.group(1)) == want_id
               and "activated" in m.group(5).split(",")]
        got = {mm.group(1) for l in read("square-e1.log")
               if (mm := re.match(r"configure serial=(\d+) .*states=\S*activated", l))}
        sq_ok = bool(act) and act[-1] in got
    ok = clicked and raised and sq_ok and ("raise", str(want_id), "ok") in reqs and \
        ("activate", str(want_id), "ok") in reqs and \
        reqs.index(("raise", str(want_id), "ok")) < reqs.index(("activate", str(want_id), "ok")) and \
        bool(kb) and kb[-1] == want_id and bool(draws) and int(draws[-1].group(3)) == idx
    record("E2 panel click: raise + activate over the bridge, highlight follows", ok,
           f"clicked idx={idx} ({titles.get(idx, '-')!r} = toplevel {want_id}), shell click={clicked}, "
           f"bridge requests={reqs}, raised in the stack={raised}, keyboard focus={kb[-1] if kb else '-'}, "
           f"panel focused idx now={draws[-1].group(3) if draws else '-'}, the window received its "
           f"activating configure={sq_ok}")

s = scen.get("E3")
if s is not None:
    sh = sh_lines(SH1, s)
    added = any("shell.bridge: add handle=" in l for l in sh)
    k = next((k for k, l in enumerate(sh) if 'title="lumen-title-e3"' in l and "shell.bridge: title" in l), None)
    after = [m for l in (sh[k + 1:] if k is not None else []) if (m := SDRAW.search(l))]
    tid = s.get("id", 0)
    fr = [m for i in rng(s) if (m := re.search(rf"compositor\.deco: decoration apply id={tid} mode=server "
                                              r"frame=(-?\d+),(-?\d+),(\d+)x(\d+)", comp[i]))]
    below = bool(tid) and bool(fr) and int(fr[0].group(2)) >= 32
    ok = added and k is not None and bool(after) and after[0].group(4) == "1" and not after[0].group(5) \
        and below
    record("E3 live: a new window below the panel adds an entry, its retitle redraws that entry only", ok,
           f"added={added}, retitled={k is not None}, redraw after it: "
           f"{after[0].group(0) if after else '-'} (want damage=1, not full); new window {tid} frame "
           f"top y={fr[0].group(2) if fr else '-'} (want >= 32, below the panel)")

s = scen.get("E4")
if s is not None:
    sh = sh_lines(SH1, s)
    removed = any("shell.bridge: remove handle=" in l for l in sh)
    draws = [m for l in sh if (m := SDRAW.search(l))]
    ok = removed and bool(draws) and draws[-1].group(1) == "2" and bool(draws[-1].group(5))
    record("E4 a client exits: its entry goes", ok,
           f"removed={removed}, last draw={draws[-1].group(0) if draws else '-'} (want 2 entries, full)")

s = scen.get("E5")
if s is not None:
    r, sh = rng(s), sh_lines(SH2, s)
    lines_ = [comp[i] for i in r]
    i_destroyed = next((k for k, l in enumerate(lines_) if "compositor.shell: bridge destroyed" in l), None)
    i_full = next((k for k, l in enumerate(lines_) if (m := USABLE.search(l)) and
                   m.group(1, 2, 3, 4) == ("0", "0", str(out_w), str(out_h))), None)
    bound = comp_find(r, BOUND)
    i_back = max((k for k, l in enumerate(lines_) if (m := USABLE.search(l)) and m.group(2) == "32"),
                 default=None)
    snap = any("shell.bridge: snapshot toplevels=2" in l for l in sh)
    draws = [m for l in sh if (m := SDRAW.search(l))]
    # The snapshot is in creation order: the restarted panel lists what the old one did.
    before, after = layout_titles(SH1), layout_titles(sh)
    # The zone hold (ADR 011 S6): the dead shell's strip stays reserved until the new panel
    # maps, so the usable area never becomes the whole output in between (and, unchanged, is
    # not logged again: `usable y=32 again` is informational).
    i_hold = next((k for k, l in enumerate(lines_) if "compositor.layer: zone hold on output" in l), None)
    i_release = next((k for k, l in enumerate(lines_)
                      if "zone hold released on output" in l and "a new zone mapped" in l), None)
    ok = i_destroyed is not None and i_full is None and i_hold is not None and \
        i_release is not None and i_release > i_hold and bool(bound) and \
        bound[-1].group(1) == "2" and snap and \
        bool(draws) and draws[-1].group(1) == "2" and before == after
    record("E5 restart: bridge destroyed, panel strip held, fresh snapshot, panel back", ok,
           f"destroyed={i_destroyed is not None}, usable full meanwhile={i_full is not None}, "
           f"zone hold={i_hold is not None}, released by the new panel={i_release is not None}, rebound snapshot="
           f"{bound[-1].group(1) if bound else '-'}, usable y=32 again={i_back is not None}, "
           f"shell snapshot of 2={snap}, draw={draws[-1].group(0) if draws else '-'}, "
           f"same entry order as before={before == after} ({list(after.values())})")

s = scen.get("E6")
if s is not None:
    r = rng(s)
    denied = [comp[i] for i in r if "privileged global zlm_shell_v1 denied" in comp[i]
              and "fake-shell is not the shell binary" in comp[i] and "lumen-shell" in comp[i]]
    rebound = comp_find(r, BOUND)
    fake = read("fake-shell.log")
    said = any("zlm_shell_v1 bind rejected" in l for l in fake)
    ok = s["rc"] == 1 and bool(denied) and not rebound and said and s["shell_alive"] == 1
    record("E6 a copy of the shell binary is refused at bind", ok,
           f"exit={s['rc']} (want 1), compositor warning naming both paths={bool(denied)}, "
           f"shell said why={said}, bridge rebound={bool(rebound)} (want no), real shell alive="
           f"{s['shell_alive'] == 1}")

shell_problems = [l for l in SH1 + SH2 if l.startswith(("[ERROR]", "[WARN]"))]
if "E1" in scen:
    record("E shell logs: no error or warning", not shell_problems,
           f"{len(shell_problems)}" + (f"; first: {shell_problems[0]}" if shell_problems else ""))

# ── F: clipboard and primary selection ─────────────────────────────────────
SELSET = re.compile(r"compositor\.selection: selection set kind=(\w+) client=(\d+) source=(\d+) mimes=(\d+) serial=(\d+)")
SELOFFER = re.compile(r"compositor\.selection: selection offer kind=(\w+) client=(\d+) offer=(\d+) mimes=(\d+) cause=(\S+)")
SELRECV = re.compile(r"compositor\.selection: selection receive kind=(\w+) client=(\d+) source_client=(\d+) mime=\S+ result=(\w+)")
SELREJ = re.compile(r"compositor\.selection: selection reject kind=(\w+) client=(\d+) serial=(\d+) reason=(\w+)")
SELCLEAR = re.compile(r"compositor\.selection: selection clear kind=(\w+) cause=(\S+)")

def out_file(name):
    try:
        with open(os.path.join(log_dir, name), "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None

def transfer_chain(r, kind):
    """In range r: a `selection set` of `kind` by one client, then that selection offered to
    another client as it takes the keyboard (cause=focus), then that client's receive sent.
    Returns (ok, detail)."""
    sets = [(i, m) for i in r if (m := SELSET.search(comp[i])) and m.group(1) == kind]
    if not sets:
        return False, f"no `selection set kind={kind}`"
    i_set, mset = sets[-1] if kind == "primary" else sets[0]
    owner = mset.group(2)
    offer = next(((i, m) for i in range(i_set + 1, r.stop) if (m := SELOFFER.search(comp[i]))
                  and m.group(1) == kind and m.group(2) != owner and m.group(5) == "focus"
                  and int(m.group(4)) > 0), None)
    if not offer:
        return False, f"set by client {owner}, never offered to another client on focus"
    reader = offer[1].group(2)
    recv = next((m for i in range(offer[0] + 1, r.stop) if (m := SELRECV.search(comp[i]))
                 and m.group(1) == kind and m.group(2) == reader and m.group(3) == owner), None)
    ok = recv is not None and recv.group(4) == "sent"
    return ok, (f"set by client {owner} (mimes={mset.group(4)}), offered to client {reader} "
                f"on focus (mimes={offer[1].group(4)}), receive "
                f"{recv.group(4) if recv else 'never'}")

for name, kind, text in (("F1", "clipboard", b"lumen-clip-F1-4242\n"),
                         ("F2", "clipboard", b"lumen-clip-F2-5151\n"),
                         ("F3", "primary", b"lumen-prim-F3-7777\n"),
                         ("F3r", "primary", b"lumen-prim-F3r-8888\n")):
    s = scen.get(name)
    if s is None:
        continue
    got = out_file(f"{name}.out")
    chain_ok, chain = transfer_chain(rng(s), kind)
    detail = f"{name}.out={got!r} (want {text!r}); {chain}"
    ok = got == text and chain_ok
    if kind == "primary":
        # The reader had no keyboard until the middle click, and the press goes out after
        # the click's focus change: the primary offer must come before that `focus keyboard`
        # line (compositor order). foot's own trace must agree — it dispatches one queue;
        # alacritty reads selections on a queue of its own, so its trace order is not wire
        # order.
        r = rng(s)
        dst_id = s["dst"]
        i_set = max((i for i in r if (m := SELSET.search(comp[i])) and m.group(1) == "primary"),
                    default=None)
        i_focus = next((i for i in range(i_set + 1 if i_set is not None else r.stop, r.stop)
                        if f"compositor.wm: focus keyboard id={dst_id}" == comp[i].split("] ", 1)[-1]),
                       None)
        before = i_focus is not None and any(
            (m := SELOFFER.search(comp[i])) and m.group(1) == "primary" and
            m.group(5) == "focus" and int(m.group(4)) > 0 for i in range(i_set, i_focus))
        ok = ok and before
        detail += f"; primary offered before the click's focus change (and press)={before}"
        if name == "F3r":
            dbg = wl_debug("foot-f3r")
            press = next((k for k, (req, i, _, msg, a) in enumerate(dbg) if not req and
                          i == "wl_pointer" and msg == "button" and
                          a.replace(" ", "").endswith(",274,1")), None)
            offered = press is not None and any(
                not req and i == "zwp_primary_selection_device_v1" and msg == "selection" and
                a.strip() != "nil" for req, i, _, msg, a in dbg[:press])
            ok = ok and offered
            detail += f"; foot's trace: primary offer before its middle press={offered}"
    titles = {"F1": "F1 foot → alacritty, Ctrl+Shift+C / Ctrl+Shift+V",
              "F2": "F2 alacritty → foot, Ctrl+Shift+C / Ctrl+Shift+V",
              "F3": "F3 primary foot → alacritty by middle click",
              "F3r": "F3r primary alacritty → foot by middle click"}
    record(titles[name], ok, detail)

s = scen.get("F5")
if s is not None:
    r = rng(s)
    sets = [m for i in range(r.start, s["mark"]) if (m := SELSET.search(comp[i]))
            and m.group(1) == "clipboard"]
    owners = [m.group(2) for m in sets]
    dbg = wl_debug("foot-f5")
    i_cancel = next((k for k, (req, i, _, msg, _a) in enumerate(dbg) if not req and
                     i == "wl_data_source" and msg == "cancelled"), None)
    nil_after = i_cancel is not None and any(
        not req and i == "wl_data_device" and msg == "selection" and a.strip() == "nil"
        for req, i, _, msg, a in dbg[i_cancel:])
    cleared = any((m := SELCLEAR.search(comp[i])) and m.group(1) == "clipboard" and
                  m.group(2) == "source-destroyed" for i in range(s["mark"], r.stop))
    ok = len(owners) >= 2 and owners[0] != owners[-1] and i_cancel is not None and \
        nil_after and cleared
    record("F5 a new owner cancels the old source; the owner exiting clears the selection", ok,
           f"clipboard owners in order {owners}, foot's source cancelled={i_cancel is not None}, "
           f"then foot told selection(nil)={nil_after}, compositor `selection clear "
           f"kind=clipboard cause=source-destroyed`={cleared}")

# F6 — every keyboard enter foot received is immediately preceded by the selection events
# (wl_data_device.selection, zwp_primary_selection_device_v1.selection). foot dispatches one
# queue, so its trace is wire order; alacritty reads the clipboard on a queue of its own
# (smithay-clipboard), whose dispatch order says nothing about the wire.
f_foot = sorted(n[:-4] for n in os.listdir(log_dir)
                if n.startswith("foot-f") and n.endswith(".log"))
bad, enters = [], 0
for name in f_foot:
    dbg = [e for e in wl_debug(name) if not e[0]]
    last_kb = -1
    for k, (_, iface, _, msg, a) in enumerate(dbg):
        if iface != "wl_keyboard" or msg not in ("enter", "leave"):
            continue
        if msg == "enter":
            enters += 1
            since = dbg[last_kb + 1:k]
            kinds = {i for _, i, _, m, _ in since if m == "selection"}
            prev = dbg[k - 1] if k > 0 else None
            if not prev or prev[3] != "selection" or \
                    not {"wl_data_device", "zwp_primary_selection_device_v1"} <= kinds:
                bad.append(f"{name}: enter #{enters} preceded by "
                           f"{prev[1] + '.' + prev[3] if prev else 'nothing'}")
        last_kb = k
s = scen.get("F6")
if s is not None:
    pl = probe_lines("f6")
    before = [l for l in pl[s["m0"]:s["m1"]] if l.startswith("selection offer mimes=")]
    after = pl[s["m1"]:]
    offered = any(l.startswith("selection offer mimes=[") and "text/plain;charset=utf-8" in l
                  for l in after)
    got = next((l for l in after if l.startswith("selection received ")), "")
    ok = not bad and enters > 0 and not before and offered and 'data="lumen-own-F6-3"' in got
    record("F6 selection before every keyboard enter; nothing offered to an unfocused client",
           ok, f"foot keyboard enters checked={enters}, out of order={len(bad)}"
           + (f" (first: {bad[0]})" if bad else "")
           + f"; probe offers while unfocused={len(before)}, offered on click={offered}, "
           f"read: {got or '-'}")

s = scen.get("F7")
if s is not None:
    r = rng(s)
    rej = [m for i in r if (m := SELREJ.search(comp[i])) and m.group(3) == "1"]
    took = [m for i in r if SELSET.search(comp[i])]
    cancelled = "selection cancelled" in probe_lines("f7")
    ok = bool(rej) and rej[0].group(4) == "serial" and cancelled and not took
    record("F7 set_selection quoting serial 1 (never sent) is refused and cancelled", ok,
           f"reject={rej[0].group(0) if rej else '-'}, probe cancelled={cancelled}, "
           f"selection taken={len(took)} (want 0)")
s = scen.get("F7r")
if s is not None:
    err = "protocol-error interface=wl_data_device code=1" in probe_lines("f7r")
    record("F7r a source set twice is wl_data_device.error.used_source", s["rc"] == 3 and err
           and s["alive"] == 1, f"probe exit={s['rc']} (want 3), error seen={err}, "
           f"compositor alive={s['alive'] == 1}")

s = scen.get("F4")
if s is not None:
    got, gotr = out_file("F4.out"), out_file("F4r.out")
    gdbg = wl_debug("gtk-factory-f4")
    gtk_set = any(req and i == "wl_data_device" and msg == "set_selection"
                  for req, i, _, msg, _a in gdbg)
    gtk_recv = any(req and i == "wl_data_offer" and msg == "receive" and
                   a.startswith('"text/plain;charset=utf-8"') for req, i, _, msg, a in gdbg)
    ok = s["id"] != 0 and got == b"lumen-gtk-f4\n" and gotr == b"lumen-clip-f4r\n" and \
        gtk_recv and s["alive"] == 1
    detail = (f"F4.out={got!r} (want b'lumen-gtk-f4\\n'), F4r.out={gotr!r} (want "
              f"b'lumen-clip-f4r\\n'), GTK set a selection={gtk_set}, GTK read "
              f"text/plain;charset=utf-8={gtk_recv}, alive={s['alive'] == 1}")
    # No selection ever set by GTK: the point held no text widget at this window size.
    record("F4 GTK ↔ foot, Ctrl+C / Ctrl+V both ways", ok or not gtk_set,
           detail + ("" if ok or gtk_set else " — no text widget at the point (layout)"),
           warn=not ok and not gtk_set)

# ── G: drag-and-drop ───────────────────────────────────────────────────────
DNDSTART = re.compile(r"compositor\.dnd: dnd start client=(\d+) source=(\d+) origin=(\d+) icon=(\d+) button=(\d+) actions=(\d+) mimes=(\d+)")
DNDREJ = re.compile(r"compositor\.dnd: dnd start rejected client=(\d+) reason=(\S+)")
DNDFOCUS = re.compile(r"compositor\.dnd: dnd focus surface=(\d+) client=(\d+) ")
DNDEND = re.compile(r"compositor\.dnd: dnd end reason=(\w+)")
DNDFIN = re.compile(r"compositor\.dnd: dnd finish client=(\d+) action=(\d+)")
DNDACT = re.compile(r"compositor\.dnd: dnd action action=(\d+)")
RENDER_AREA = re.compile(r"compositor\.render: frame=\d+ composite=\d+ rects=\d+ area=(\d+)")

def g_probe(name, side):
    return probe_lines(f"{name.lower()}{side}")

def g_comp(s, rx):
    return [m for i in rng(s) if (m := rx.search(comp[i]))]

def first_index(lines, pred, start=0):
    return next((k for k in range(start, len(lines)) if pred(lines[k])), None)

def src_actions(lines):
    return [int(l.split("=")[1]) for l in lines if l.startswith("dnd source action=")]

def g_ends(s):
    return [m.group(1) for m in g_comp(s, DNDEND)]

s = scen.get("G1")
if s is not None:
    src, tgt = g_probe("G1", "s"), g_probe("G1", "t")
    starts = g_comp(s, DNDSTART)
    src_client = starts[0].group(1) if starts else None
    foci = [m for m in g_comp(s, DNDFOCUS) if m.group(2) != src_client]
    i_enter = first_index(tgt, lambda l: l.startswith("dnd enter surface=toplevel") and "offer=obj" in l)
    i_drop = first_index(tgt, lambda l: l == "dnd drop", i_enter or 0)
    motions = len([l for l in tgt[(i_enter or 0):(i_drop or 0)] if l.startswith("dnd motion ")])
    got = 'dnd received data="lumen-dnd-G1"' in tgt
    i_perf = first_index(src, lambda l: l == "dnd source drop_performed")
    i_fin = first_index(src, lambda l: l == "dnd source finished", i_perf or 0)
    acts = src_actions(src)
    i_start = first_index(src, lambda l: l.startswith("dnd source start"))
    released = [l for l in src[(i_start or 0):] + tgt if l.startswith("release surface=")]
    ends = g_ends(s)
    fins = g_comp(s, DNDFIN)
    ok = (len(starts) == 1 and len(foci) == 1 and i_enter is not None and i_drop is not None
          and motions >= 10 and got and i_perf is not None and i_fin is not None
          and acts and acts[-1] == 2 and ends == ["drop"] and len(fins) == 1 and not released)
    record("G1 drag copy|move onto a target preferring move: enter, motions, drop, data, finish",
           ok, f"dnd start={len(starts)}, target focus lines={len(foci)} (want 1), target "
           f"motions before drop={motions} (want >=10), drop={i_drop is not None}, data read={got}; "
           f"source drop_performed={i_perf is not None} finished={i_fin is not None} "
           f"actions={acts} (last want 2); end={ends}, finish lines={len(fins)}; "
           f"button releases reaching a probe={len(released)}")

    # G9 — the icon follows the pointer exactly.
    icon_line = next((l for l in src if l.startswith("dnd icon ")), "")
    m_icon = re.search(r"w=(\d+) h=(\d+) dx=(-?\d+) dy=(-?\d+)", icon_line)
    ox, oy = (int(m_icon.group(3)), int(m_icon.group(4))) if m_icon else (0, 0)
    moves, unmapped_after, start_i, end_i = [], False, None, None
    for i in rng(s):
        if start_i is None and DNDSTART.search(comp[i]):
            start_i = i
        if DNDEND.search(comp[i]):
            end_i = i
        m = TREE.search(comp[i])
        if not m or m.group(2) != "dnd_icon":
            continue
        if m.group(4) == "move":
            for n in NODE.finditer(m.group(5)):
                if n.group(2):
                    moves.append((int(n.group(3)), int(n.group(4))))
        unmapped_after = m.group(5) == "-"
    injected = [(r["x"], r["y"]) for r in inj if r.get("tag") == "G1" and r["kind"] == "motion"]
    def matches(p):
        return any(abs(p[0] - (math.floor(x) + 1 + ox)) <= 1 and abs(p[1] - (math.floor(y) + 1 + oy)) <= 1
                   for x, y in injected)
    off = [p for p in moves if not matches(p)]
    full = out_w * out_h
    big = [l for l in comp[(start_i or 0):(end_i or 0)] if (m := RENDER_AREA.search(l)) and int(m.group(1)) >= full]
    ok = bool(m_icon) and len(moves) >= 10 and not off and unmapped_after and not big
    record("G9 the drag icon follows the pointer (offset kept), gone after the drop, no full damage",
           ok, f"icon {icon_line or '-'}; icon moves={len(moves)}, not at an injected position+offset="
           f"{len(off)}" + (f" (first {off[0]})" if off else "") +
           f", unmapped at the end={unmapped_after}, full-output frames during the drag={len(big)}")

for name, want_last, want_seq in (("G2", 1, [1]), ("G2b", 2, [1, 2])):
    s = scen.get(name)
    if s is None:
        continue
    src, tgt = g_probe(name, "s"), g_probe(name, "t")
    acts = src_actions(src)
    comp_acts = [int(m.group(1)) for m in g_comp(s, DNDACT) if m.group(1) != "0"]
    fins = [int(m.group(2)) for m in g_comp(s, DNDFIN)]
    seq_ok = [a for k, a in enumerate(acts) if a and (k == 0 or a != acts[k - 1])] == want_seq
    ok = acts and acts[-1] == want_last and seq_ok and fins == [want_last] and \
        any(l.startswith('dnd received data="lumen-dnd-') for l in tgt)
    title = ("G2 Ctrl held during the drag: the action is copy" if name == "G2" else
             "G2b Ctrl, then Shift over the target: copy, then move")
    record(title, ok, f"source actions {acts}, compositor non-zero actions {comp_acts}, "
           f"finished with action {fins} (want [{want_last}])")

for name, reason, title in (("G3", "no_target", "G3 a release over nothing cancels"),
                            ("G5", "not_accepted", "G5 a target accepting nothing: leave, no drop, cancelled")):
    s = scen.get(name)
    if s is None:
        continue
    src, tgt = g_probe(name, "s"), g_probe(name, "t")
    cancelled = "dnd source cancelled" in src
    performed = "dnd source drop_performed" in src
    tdrop = "dnd drop" in tgt
    left = name != "G5" or ("dnd leave" in tgt and any(l.startswith("dnd enter") for l in tgt))
    ends = g_ends(s)
    ok = cancelled and not performed and not tdrop and left and ends == [reason]
    record(title, ok, f"source cancelled={cancelled} drop_performed={performed}, target drop={tdrop}"
           f" entered+left={left}, end={ends} (want ['{reason}'])")

s = scen.get("G4")
if s is not None:
    src, tgt = g_probe("G4", "s"), g_probe("G4", "t")
    entered = any(l.startswith("dnd enter") for l in tgt)
    i_enter = first_index(tgt, lambda l: l.startswith("dnd enter"))
    left = i_enter is not None and first_index(tgt, lambda l: l == "dnd leave", i_enter) is not None
    esc = [l for l in src + tgt if l.startswith("key code=1 ")]
    ends = g_ends(s)
    ok = entered and left and "dnd drop" not in tgt and "dnd source cancelled" in src and \
        ends == ["escape"] and not esc
    record("G4 Escape over the target cancels the drag and reaches no client", ok,
           f"target entered={entered} left={left} drop={'dnd drop' in tgt}, source cancelled="
           f"{'dnd source cancelled' in src}, end={ends}, Escape key events at a probe={len(esc)}")

s = scen.get("G6")
if s is not None:
    src = g_probe("G6", "s")
    i_enter = first_index(src, lambda l: l.startswith("dnd enter surface=second") and "offer=obj" in l)
    i_drop = first_index(src, lambda l: l == "dnd drop", i_enter or 0)
    got = 'dnd received data="lumen-dnd-G6"' in src
    fin = "dnd source finished" in src
    ok = i_enter is not None and i_drop is not None and got and fin and g_ends(s) == ["drop"]
    record("G6 a drop on the source client's own second window", ok,
           f"enter on second={i_enter is not None}, drop={i_drop is not None}, data read={got}, "
           f"source finished={fin}, end={g_ends(s)}")

s = scen.get("G7")
if s is not None:
    src, tgt = g_probe("G7", "s"), g_probe("G7", "t")
    other = [l for l in tgt if l.startswith("dnd enter")]
    own = any(l.startswith("dnd enter surface=second") and "offer=nil" in l for l in src)
    drop = "dnd drop" in src
    ok = not other and own and drop and g_ends(s) == ["drop"]
    record("G7 a drag with no source stays in its client (enter with no offer on its own window)",
           ok, f"target probe enters={len(other)} (want 0), own second window enter offer=nil={own}, "
           f"drop={drop}, end={g_ends(s)}")

s = scen.get("G8")
if s is not None:
    src, tgt = g_probe("G8", "s"), g_probe("G8", "t")
    i_enter = first_index(tgt, lambda l: l.startswith("dnd enter"))
    left = i_enter is not None and first_index(tgt, lambda l: l == "dnd leave", i_enter) is not None
    exited = "dnd source exiting" in src
    r = rng(s)
    i_end = next((i for i in r if DNDEND.search(comp[i])), None)
    back = i_end is not None and any("compositor.wm: focus pointer id=" in comp[i]
                                    for i in range(i_end, r.stop))
    ends = g_ends(s)
    ok = exited and left and "dnd drop" not in tgt and ends == ["source_destroyed"] and \
        s["alive"] == 1 and back
    record("G8 the source's client exits mid-drag: target left, compositor alive, pointer back",
           ok, f"source exited={exited}, target entered+left={left}, drop={'dnd drop' in tgt}, "
           f"end={ends}, compositor alive={s['alive'] == 1}, pointer focus after={back}")

s = scen.get("G10")
if s is not None:
    src = g_probe("G10", "s")
    rej = g_comp(s, DNDREJ)
    i_start = first_index(src, lambda l: l.startswith("dnd source start"))
    i_rel = first_index(src, lambda l: l.startswith("release surface=toplevel"), i_start or 0)
    kept = i_start is not None and i_rel is not None and not any(
        l == "pointer leave" for l in src[i_start:i_rel])
    ok = bool(rej) and rej[0].group(2) == "serial" and "dnd source cancelled" in src and kept \
        and not g_comp(s, DNDSTART)
    record("G10 start_drag quoting the enter serial is refused; the source keeps the pointer", ok,
           f"rejected={rej[0].group(0).split(': ', 1)[-1] if rej else '-'}, source cancelled="
           f"{'dnd source cancelled' in src}, pointer kept until its own release={kept}")

s = scen.get("G11")
if s is not None:
    got, want = out_file("G11.out"), out_file("G11.want")
    gdbg = wl_debug("gtk-factory-g11")
    started = any(req and i == "wl_data_device" and msg == "start_drag" for req, i, _, msg, _a in gdbg)
    fdbg = wl_debug("foot-g11")
    dropped = any(not req and i == "wl_data_device" and msg == "drop" for req, i, _, msg, _a in fdbg)
    ok = s["id"] != 0 and want is not None and got == want and s["alive"] == 1 and started
    record("G11 GTK → foot: text dragged from widget-factory is dropped into foot", True,
           f"G11.out {len(got or b'')} bytes, {(got or b'').count(bytes([10]))} lines, equal to the "
           f"{len(want or b'')} bytes GTK held={got == want}; GTK called start_drag={started}, "
           f"foot got drop={dropped}, "
           f"GTK alive={s['alive'] == 1}" + ("" if ok else
           " — " + ("dropped data did not arrive" if started else "GTK started no drag (layout/threshold)")),
           warn=not ok)

# ── H: outputs ─────────────────────────────────────────────────────────────
OUTNAME = re.compile(r"compositor\.output: output 0 name=(\S+) mode=(\d+)x(\d+) refresh_mhz=(\d+)")
out_name = next((m.group(1) for l in comp if (m := OUTNAME.search(l))), None)
OUTBIND = re.compile(r"compositor\.output: output bind client=(\d+) output=(\d+) version=(\d+) name=(\S+) replayed=(\d+)")
OUTENTER = re.compile(r"compositor\.output: output (enter|leave) surface=(\d+) output=(\d+) client=(\d+) resources=(\d+)")

s = scen.get("H1")
if s is not None:
    p = probe_lines("h1")
    wl = [l for l in p if l.startswith("wl_output name=")]
    xo = [l for l in p if l.startswith("xdg_output ")]
    want_wl = f"wl_output name={out_name} mode={out_w}x{out_h} "
    want_xo = f"xdg_output position=0,0 size={out_w}x{out_h} name={out_name} description={out_name}"
    enter = f"surface enter surface=toplevel output={out_name}" in p
    ok = bool(out_name) and bool(wl) and wl[-1].startswith(want_wl) and xo and xo[-1] == want_xo and enter
    record("H1 wl_output and xdg-output report the output; the toplevel gets enter", ok,
           f"compositor output name={out_name}; probe: {wl[-1] if wl else '-'} | "
           f"{xo[-1] if xo else '-'} | enter={enter}")

# H2 — foot's own traces: every surface it maps gets wl_surface.enter.
ENTER_RAW = re.compile(r"\] (?:\{[^}]*\} )?(?:discarded )?wl_surface[#@](\d+)\.enter\(wl_output")

def entered_surfaces(name):
    """wl_surface ids that received enter — "discarded" included: foot listens on its main
    surface only, and libwayland-client logs an event for a listener-less proxy that way."""
    return {int(m.group(1)) for l in read(f"{name}.log") if (m := ENTER_RAW.search(l))}

def foot_surfaces(dbg):
    """(main wl_surface id, [subsurface wl_surface ids]) — ids from get_xdg_surface and
    get_subsurface; ids are reused after destroy, which foot does not do for these."""
    main, subs = None, []
    for req, i, oid, msg, a in dbg:
        if req and i == "xdg_wm_base" and msg == "get_xdg_surface" and main is None:
            m = re.search(r"wl_surface[#@](\d+)", a)
            main = int(m.group(1)) if m else None
        elif req and i == "wl_subcompositor" and msg == "get_subsurface":
            m = re.findall(r"wl_surface[#@](\d+)", a)
            if m:
                subs.append(int(m[0]))
    return main, subs

h2 = []
for name, scen_name in (("foot-csd", "A1"), ("foot-ssd", "C1-server")):
    if scen_name not in scen:
        continue
    dbg = wl_debug(name)
    if not dbg:
        continue
    main, subs = foot_surfaces(dbg)
    ent = entered_surfaces(name)
    s_a = scen[scen_name]
    mapped = len({m.group(1) for i in rng(s_a) if (m := SUBMAP.search(comp[i]))}) if scen_name == "A1" else 0
    sub_entered = len([x for x in subs if x in ent])
    h2.append((name, main in ent, sub_entered, mapped, len(subs)))
if h2:
    ok = all(m_ok and (mapped == 0 or sub_entered >= mapped) for _, m_ok, sub_entered, mapped, _ in h2)
    record("H2 foot (A1 CSD, C1 server-side): the main surface and every mapped CSD "
           "subsurface get wl_surface.enter", ok,
           "; ".join(f"{n}: main enter={m}, subsurfaces entered={e} (mapped {mp}, created {c})"
                     for n, m, e, mp, c in h2))

s = scen.get("H3")
if s is not None:
    p = probe_lines("h3")
    i_bind = first_index(p, lambda l: l.startswith("wl_output bind "))
    i_enter = first_index(p, lambda l: l.startswith("surface enter surface=toplevel"))
    i_conf = first_index(p, lambda l: l.startswith("toplevel configure "))
    binds = [m for i in rng(s) if (m := OUTBIND.search(comp[i]))]
    ok = (i_conf is not None and i_bind is not None and i_enter is not None and i_conf < i_bind < i_enter
          and len(binds) == 1 and binds[0].group(5) == "1")
    record("H3 a wl_output bound after mapping replays the enter", ok,
           f"probe order configure/bind/enter={i_conf}/{i_bind}/{i_enter}; compositor "
           + (binds[0].group(0).split(": ", 1)[1] if binds else "no output bind line"))

s = scen.get("H4")
if s is not None:
    p = probe_lines("h4")
    pops = [int(m.group(1)) for i in rng(s) if (m := POPMAP.search(comp[i]))]
    ev = [(m.group(1), int(m.group(2))) for i in rng(s) if (m := OUTENTER.search(comp[i]))]
    pop = pops[0] if pops else None
    pop_ev = [e for e, sid in ev if sid == pop]
    done = "popup_done idx=0" in p
    pop_enter_probe = "surface enter surface=popup0 output=" + str(out_name) in p
    i_unmap = first_index(p, lambda l: l == "toplevel unmapped")
    top_leave = i_unmap is not None and any(
        l.startswith("surface leave surface=toplevel") for l in p[i_unmap:])
    ok = pop is not None and pop_ev == ["enter", "leave"] and done and pop_enter_probe and top_leave
    record("H4 a popup gets enter on map and leave on dismissal; the toplevel leave on unmap", ok,
           f"popup surface={pop} compositor enter/leave={pop_ev} (want enter, leave), probe "
           f"popup enter={pop_enter_probe}, popup_done={done}, toplevel leave after unmap={top_leave}")

# H5/H6 — the output contract from real clients' own traces (WAYLAND_DEBUG=client).
TRLINE = re.compile(r"^\[[\d:.]+\]\s+(?:\{[^}]*\}\s+)?(-> )?(discarded )?(\w+)[#@](\d+)\.(\w+)\((.*)\)\s*$")

def output_contract(name):
    """Judge one traced client log: every wl_output / zxdg_output_v1 value equals the
    compositor's output, and wl_surface.enter/leave alternate per (surface, output) starting
    with enter, naming a wl_output the client bound. Object ids are reused, so a surface's
    state resets at create_surface / destroy. Returns None when the log has no trace."""
    outputs, problems = set(), []
    entered = {}            # surface id -> set of output ids currently entered
    ever = set()            # surface ids (this incarnation) that got an enter
    xdg_surf = {}           # xdg_surface id -> wl_surface id
    toplevels, popups = [], []
    n_enter = n_leave = n_xdg = n_wlo = 0
    traced = False
    for l in read(f"{name}.log"):
        m = TRLINE.match(l)
        if not m:
            continue
        traced = True
        req, iface, oid, msg, a = m.group(1) is not None, m.group(3), int(m.group(4)), m.group(5), m.group(6)
        if req:
            if iface == "wl_registry" and msg == "bind" and '"wl_output"' in a:
                mm = re.search(r"#(\d+)\)?$", a.strip())
                if mm:
                    outputs.add(int(mm.group(1)))
            elif iface == "wl_compositor" and msg == "create_surface":
                mm = re.search(r"wl_surface[#@](\d+)", a)
                if mm:
                    sid = int(mm.group(1)); entered.pop(sid, None); ever.discard(sid)
            elif iface == "wl_surface" and msg == "destroy":
                entered.pop(oid, None); ever.discard(oid)
            elif iface == "xdg_wm_base" and msg == "get_xdg_surface":
                ids = re.findall(r"(?:xdg_surface|wl_surface)[#@](\d+)", a)
                if len(ids) == 2:
                    xdg_surf[int(ids[0])] = int(ids[1])
            elif iface == "xdg_surface" and msg in ("get_toplevel", "get_popup") and oid in xdg_surf:
                (toplevels if msg == "get_toplevel" else popups).append(
                    (xdg_surf[oid], len(toplevels) + len(popups), [False]))
            continue
        if iface == "wl_surface" and msg in ("enter", "leave"):
            mm = re.search(r"wl_output[#@](\d+)", a)
            out = int(mm.group(1)) if mm else -1
            cur = entered.setdefault(oid, set())
            if out not in outputs:
                problems.append(f"wl_surface#{oid}.{msg} names wl_output#{out}, not bound")
            if msg == "enter":
                n_enter += 1
                if out in cur:
                    problems.append(f"wl_surface#{oid} entered wl_output#{out} twice")
                cur.add(out); ever.add(oid)
                for sid, _, flag in sorted(toplevels + popups, key=lambda t: -t[1]):
                    if sid == oid:          # the latest role object on this surface
                        flag[0] = True
                        break
            else:
                n_leave += 1
                if out not in cur:
                    problems.append(f"wl_surface#{oid} left wl_output#{out} it had not entered")
                cur.discard(out)
        elif iface == "wl_output":
            if msg == "mode":
                n_wlo += 1
                v = [x.strip() for x in a.split(",")]
                if len(v) == 4 and (int(v[0]) & 1) and (int(v[1]), int(v[2])) != (out_w, out_h):
                    problems.append(f"wl_output.mode {v[1]}x{v[2]} != {out_w}x{out_h}")
            elif msg == "name" and a.strip('"') != out_name:
                problems.append(f"wl_output.name {a} != {out_name}")
        elif iface == "zxdg_output_v1":
            n_xdg += 1
            v = [x.strip().strip('"') for x in a.split(",")]
            if msg == "logical_position" and v != ["0", "0"]:
                problems.append(f"xdg_output.logical_position {a} != 0, 0")
            elif msg == "logical_size" and v != [str(out_w), str(out_h)]:
                problems.append(f"xdg_output.logical_size {a} != {out_w}, {out_h}")
            elif msg == "name" and a.strip('"') != out_name:
                problems.append(f"xdg_output.name {a} != {out_name}")
    if not traced:
        return None
    return {"problems": problems, "toplevels": [f[0] for _, _, f in toplevels],
            "popups": [f[0] for _, _, f in popups], "enter": n_enter, "leave": n_leave,
            "xdg": n_xdg, "wl_output": n_wlo, "outputs": len(outputs)}

s = scen.get("H5")
if s is not None:
    parts, ok = [], bool(out_name)
    for cname, key in (("foot-h5", "foot"), ("gtk-factory-h5", "gtk")):
        if key == "gtk" and s.get("gtk_skipped"):
            parts.append("gtk: skipped")
            continue
        c = output_contract(cname)
        good = (c is not None and s.get(key, 0) > 0 and c["toplevels"] and all(c["toplevels"])
                and c["xdg"] >= 3 and c["wl_output"] >= 1 and not c["problems"])
        ok = ok and good
        parts.append(f"{key} (id {s.get(key, 0)}): " + ("no trace" if c is None else
                     f"toplevel entered={c['toplevels']} wl_output.mode={c['wl_output']} "
                     f"xdg_output events={c['xdg']} problems={c['problems'][:3]}"))
    xdgc = [l.split(": ", 1)[1] for i in rng(s) if "compositor.output: xdg-output create " in comp[i]
            for l in [comp[i]]]
    sent = [m for i in rng(s) if (m := OUTENTER.search(comp[i])) and m.group(1) == "enter"
            and int(m.group(5)) > 0]
    want_n = 1 if s.get("gtk_skipped") else 2
    xdg_ok = len(xdgc) >= want_n and all(f"w={out_w} h={out_h} name={out_name}" in x for x in xdgc)
    ok = ok and xdg_ok and len({m.group(4) for m in sent}) >= want_n
    record("H5 foot and GTK from their own side: wl_output and xdg-output carry the output's "
           "values, the toplevel gets wl_surface.enter", ok, f"output {out_name} {out_w}x{out_h}; "
           + "; ".join(parts) + f"; compositor: xdg-output create={len(xdgc)} (values ok={xdg_ok}), "
           f"enter sent to {len({m.group(4) for m in sent})} clients")

if any(n in scen for n in ("A1", "B6", "C1-server", "F1", "G11", "H5", "I3", "J6")):
    tot, bad, logs = {"enter": 0, "leave": 0, "pop": 0, "pop_in": 0}, [], 0
    for fn in sorted(os.listdir(log_dir)):
        if not fn.endswith(".log") or not fn.startswith(("foot", "gtk", "alacritty")):
            continue
        c = output_contract(fn[:-4])
        if c is None:
            continue
        logs += 1
        tot["enter"] += c["enter"]; tot["leave"] += c["leave"]
        tot["pop"] += len(c["popups"]); tot["pop_in"] += sum(c["popups"])
        bad += [f"{fn}: {p}" for p in c["problems"]]
    record("H6 every traced real client (foot, alacritty, GTK): output values right, "
           "enter/leave balanced per surface and output", logs > 0 and not bad,
           f"{logs} traces, {tot['enter']} enter / {tot['leave']} leave, GTK/foot popups entered "
           f"{tot['pop_in']}/{tot['pop']}; problems={bad[:4]}")

# ── I: liveness ────────────────────────────────────────────────────────────
NOTRESP = re.compile(r"compositor\.liveness: not-responding client=(\d+) pid=(\d+) after_ms=(\d+) toplevels=(\d+)")
RESP = re.compile(r"compositor\.liveness: responsive client=(\d+) pid=(\d+) was_ms=(\d+)")
PING = re.compile(r"compositor\.liveness: ping client=(\d+) serial=(\d+) reason=(\w+)")
DECOTITLE = re.compile(r'compositor\.deco: decoration title id=(\d+) scale=\d+ width=\d+ ellipsized=\d title="(.*)"$')
DIM = re.compile(r"compositor\.scene: tree root=(\d+) dim=(\d)")
MAPID = re.compile(r"compositor\.wm: map id=(\d+) app_id=(\S+) ")

def i_seq(s, tid):
    """The scenario's liveness, title and dim lines for toplevel `tid`, in order."""
    out = []
    for i in rng(s):
        l = comp[i]
        if (m := NOTRESP.search(l)):
            out.append(("nr", m.group(1), int(m.group(3))))
        elif (m := RESP.search(l)):
            out.append(("ok", m.group(1), int(m.group(3))))
        elif (m := DECOTITLE.search(l)) and int(m.group(1)) == tid:
            out.append(("title", m.group(2)))
        elif (m := DIM.search(l)):
            out.append(("dim", int(m.group(2))))
    return out

s = scen.get("I1")
if s is not None:
    tid = s.get("id", 0)
    seq = i_seq(s, tid)
    kinds = [x[0] if x[0] not in ("title", "dim") else f"{x[0]}:{x[1]}" for x in seq]
    nr = [x for x in seq if x[0] == "nr"]
    i_nr = next((k for k, x in enumerate(seq) if x[0] == "nr"), None)
    i_ok = next((k for k, x in enumerate(seq) if x[0] == "ok"), None)
    t_nr = "lumen-probe-i1 (Not Responding)"
    titled = i_nr is not None and any(x == ("title", t_nr) for x in seq[i_nr:(i_ok or len(seq))])
    dimmed = i_nr is not None and ("dim", 1) in seq[max(0, i_nr - 2):(i_ok or len(seq))]
    plain = i_ok is not None and ("title", "lumen-probe-i1") in seq[i_ok:]
    undim = i_ok is not None and ("dim", 0) in seq[max(0, i_ok - 2):]
    p = probe_lines("i1")
    ignored = any(l.startswith("ping serial=") and l.endswith(" ignored") for l in p)
    late = any(l.startswith("pong serial=") and l.endswith(" late") for l in p)
    sh = read("shell-i.log")
    flags = [int(m.group(1)) for l in sh if (m := re.search(r"shell\.bridge: flags handle=\d+ flags=(\d+)", l))]
    ok = (len(nr) == 1 and nr[0][2] >= 1500 and nr[0][2] < 2500 and i_ok is not None and i_ok > i_nr
          and titled and dimmed and plain and undim and ignored and late and flags[-2:] == [1, 0])
    record("I1 an unanswered ping: not responding (title suffix, dimmed, shell flags=1), then "
           "responsive again", ok,
           f"after_ms={[x[2] for x in nr]} (want one, 1500–2500), responsive={i_ok is not None}, "
           f"suffixed title={titled}, dimmed={dimmed}, plain title again={plain}, undimmed={undim}; "
           f"probe ignored/late pong={ignored}/{late}; shell flags={flags} (want …, 1, 0)")

s = scen.get("I2")
if s is not None:
    p = probe_lines("i2")
    nr = [m for i in rng(s) if (m := NOTRESP.search(comp[i]))]
    presses = [l for l in p if l.startswith("press surface=toplevel")]
    ok = len(nr) == 1 and len(presses) >= 2 and s.get("alive") == 1
    record("I2 a client that never answers stays connected and keeps receiving input", ok,
           f"not-responding lines={len(nr)} (want 1), presses received after it={len(presses)} "
           f"(want >=2), probe alive at the end={s.get('alive') == 1}")

s = scen.get("I3")
if s is not None:
    tid = s.get("id", 0)
    pid = str(s.get("pid", ""))
    nr = [m for i in rng(s) if (m := NOTRESP.search(comp[i]))]
    foot_nr = [m for m in nr if m.group(2) == pid]
    client = foot_nr[0].group(1) if foot_nr else None
    pings = [m.group(3) for i in rng(s) if (m := PING.search(comp[i])) and m.group(1) == client]
    resp = [m for i in rng(s) if (m := RESP.search(comp[i])) and m.group(1) == client]
    titles = [m.group(2) for i in rng(s) if (m := DECOTITLE.search(comp[i])) and int(m.group(1)) == tid]
    suffixed = any(t.endswith(" (Not Responding)") for t in titles)
    killed = [l for l in comp if f"error in client communication (pid {pid})" in l]
    dbg = wl_debug("foot-i3")
    i_press = next((k for k, x in enumerate(dbg) if not x[0] and x[1] == "wl_pointer"
                    and x[3] == "button" and x[4].replace(" ", "").endswith(",1")), None)
    i_key = next((k for k, x in enumerate(dbg) if k > (i_press or 0) and not x[0]
                  and x[1] == "wl_keyboard" and x[3] == "key"), None)
    motions = len([x for x in dbg[(i_press or 0):(i_key or len(dbg))] if not x[0]
                   and x[1] == "wl_pointer" and x[3] == "motion"])
    ok = (bool(foot_nr) and "press" in pings and bool(resp) and suffixed and not killed
          and i_press is not None and i_key is not None and motions <= 3 and s.get("alive") == 1)
    record("I3 a stopped foot: not responding after a click, 600 motions withheld, alive, "
           "responsive after SIGCONT", ok,
           f"pings to foot={pings}, not-responding={len(foot_nr)}, title suffixed={suffixed}, "
           f"responsive={len(resp)}, disconnected={bool(killed)}, alive={s.get('alive') == 1}; "
           f"foot saw {motions} motions between its press and the next key (want <=3 of 600), "
           f"key after resume={i_key is not None}")

# ── J: xdg-activation ──────────────────────────────────────────────────────
TOKEN = re.compile(r"compositor\.activation: token create client=(\d+) token=(\w+) serial=(\d+) surface=(\d+) app_id=(\S+) grantable=(\d) reason=(\w+)")
ACTIVATE = re.compile(r"compositor\.activation: activate token=(\S+) surface=(\d+) id=(\d+) result=(\w+) reason=(\w+)( cause=map)?(?: refused=\w+)?$")
URGENT = re.compile(r"compositor\.activation: urgent id=(\d+) on=(\d)")
FOCUSKB = re.compile(r"compositor\.wm: focus keyboard id=(\d+)$")

def j_lines(s):
    """(index, kind, match) of the scenario's activation and focus lines, in order."""
    out = []
    for i in rng(s):
        l = comp[i]
        for kind, rx in (("token", TOKEN), ("activate", ACTIVATE), ("urgent", URGENT), ("focus", FOCUSKB)):
            if (m := rx.search(l)):
                out.append((i, kind, m))
                break
    return out

def j_activates(seq, tid):
    return [(i, m) for i, k, m in seq if k == "activate" and int(m.group(3)) == tid]

def j_focus_last(seq, tid):
    """The scenario's last keyboard-focus decision is toplevel `tid`. (The window manager acts
    inside the request, so its lines come before the wire's `activate` line.)"""
    foc = [int(m.group(1)) for _, k, m in seq if k == "focus"]
    return bool(foc) and foc[-1] == tid

s = scen.get("J1")
if s is not None:
    tid = s.get("target", 0)
    seq = j_lines(s)
    toks = [m for _, k, m in seq if k == "token"]
    acts = j_activates(seq, tid)
    granted = bool(acts) and acts[-1][1].group(4) == "granted" and acts[-1][1].group(5) == "ok"
    focused = j_focus_last(seq, tid)
    p = probe_lines("j1t")
    i_act = next((k for k, l in enumerate(p) if l.startswith("activation activate token=")), None)
    entered = i_act is not None and any(l.startswith("keyboard enter surface=toplevel") for l in p[i_act:])
    ok = (len(toks) == 1 and toks[0].group(6) == "1" and toks[0].group(7) == "ok" and granted
          and focused and entered)
    record("J1 a token from the focused issuer's click: granted, the target gets the keyboard", ok,
           f"token create grantable/reason={[(m.group(6), m.group(7)) for m in toks]} (want [('1','ok')]); "
           f"activate={[(m.group(4), m.group(5)) for _, m in acts]} (want granted/ok); focus to target "
           f"{tid} after it={focused}; target saw keyboard enter after presenting={entered}")

s = scen.get("J2")
if s is not None:
    tid, foot = s.get("target", 0), s.get("foot", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    urgent_act = bool(acts) and acts[0][1].group(4) == "urgent" and acts[0][1].group(5) == "user_moved_on"
    i_act = acts[0][0] if acts else len(comp)
    marks = [(i, int(m.group(2))) for i, k, m in seq if k == "urgent" and int(m.group(1)) == tid]
    marked = any(on == 1 and i_act - 3 <= i <= i_act for i, on in marks)
    cleared_i = next((i for i, on in marks if on == 0 and i > i_act), None)
    # Focus did not move to the target on the activation; it moved there only with the
    # panel click, whose focus change clears the mark first (`urgent … on=0`, then `focus`).
    early_focus = [i for i, k, m in seq if k == "focus" and int(m.group(1)) == tid and i_act < i
                   and (cleared_i is None or i < cleared_i)]
    later_focus = cleared_i is not None and any(k == "focus" and int(m.group(1)) == tid and
                                                i > cleared_i for i, k, m in seq)
    sh = read("shell-j.log")
    handle = None
    for l in sh:
        if (m := re.search(r'shell\.panel: layout idx=\d+ x=\d+ w=\d+ handle=(\d+) title="lumen-j2-target"', l)):
            handle = m.group(1)
    flags = [int(m.group(2)) for l in sh if (m := re.search(r"shell\.bridge: flags handle=(\d+) flags=(\d+)", l))
             and m.group(1) == handle]
    ok = urgent_act and marked and not early_focus and later_focus and flags[-2:] == [2, 0]
    record("J2 the user clicked foot after the token was issued: urgent, focus stays, the shell "
           "shows it; its panel entry focuses it", ok,
           f"activate={[(m.group(4), m.group(5)) for _, m in acts]} (want urgent/user_moved_on); "
           f"marks={[on for _, on in marks]} (want [1, 0]); focus to the target before the panel "
           f"click={bool(early_focus)}, with it={later_focus}; shell flags for handle {handle}="
           f"{flags} (want …, 2, 0)")

for name, reason in (("J3", "no_serial"), ("J4", "unknown_token")):
    s = scen.get(name)
    if s is None:
        continue
    tid = s.get("target", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    got = [(m.group(4), m.group(5)) for _, m in acts]
    ok = got == [("urgent", reason)] and (name != "J4" or s.get("alive") == 1)
    record(f"{name} {'a token without a serial' if name == 'J3' else 'an unknown token'}: "
           f"urgent ({reason}), no error", ok,
           f"activate={got} (want [('urgent', '{reason}')])"
           + (f", probe alive={s.get('alive') == 1}" if name == "J4" else ""))

s = scen.get("J5")
if s is not None:
    tid = s.get("target", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    got = [(m.group(4), m.group(5), bool(m.group(6))) for _, m in acts]
    focused = j_focus_last(seq, tid)
    ok = got == [("pending", "ok", False), ("granted", "ok", True)] and focused
    record("J5 presented before the first commit: pending, then granted at map", ok,
           f"activate (result, reason, at map)={got} (want pending, then granted at map); "
           f"keyboard to the new window={focused}")

s = scen.get("J5b")
if s is not None:
    tid, prev = s.get("target", 0), s.get("focused", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    got = [(m.group(4), m.group(5), bool(m.group(6))) for _, m in acts]
    stole = any(k == "focus" and int(m.group(1)) == tid for _, k, m in seq)
    marked = any(k == "urgent" and int(m.group(1)) == tid and m.group(2) == "1" for _, k, m in seq)
    kept = j_focus_last(seq, prev)
    ok = got == [("pending", "ok", False), ("urgent", "user_moved_on", True)] and \
        not stole and marked and kept
    record("J5b a launch token the user moved on from, presented before the first commit: the "
           "new window opens without the keyboard, marked", ok,
           f"activate={got} (want pending, then urgent/user_moved_on at map); took the "
           f"keyboard={stole}, marked={marked}, window {prev} kept it={kept}")

s = scen.get("J5c")
if s is not None:
    tid = s.get("target", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    got = [(m.group(4), m.group(5), bool(m.group(6))) for _, m in acts]
    ok = got == [("urgent", "unknown_token", False), ("granted", "new_window", True)] and \
        j_focus_last(seq, tid)
    record("J5c an unknown token before the first commit proves nothing: the new window takes "
           "the keyboard", ok,
           f"activate={got} (want urgent/unknown_token, then granted/new_window at map); "
           f"keyboard to it={j_focus_last(seq, tid)}")

s = scen.get("J6")
if s is not None:
    tid = s.get("foot", 0)
    seq = j_lines(s)
    acts = j_activates(seq, tid)
    got = [(m.group(4), m.group(5)) for _, m in acts]
    granted = bool(acts) and acts[-1][1].group(4) == "granted"
    focused = j_focus_last(seq, tid)
    warned = [l for l in read("foot-j6.log") if re.search(r"xdg.activation", l, re.I)
              and not WLDBG.match(l)]
    dbg = wl_debug("foot-j6")
    presented = any(req and i == "xdg_activation_v1" and msg == "activate" for req, i, _, msg, _ in dbg)
    ok = granted and focused and not warned and presented
    record("J6 foot started with XDG_ACTIVATION_TOKEN: granted, no XDG-activation warning", ok,
           f"foot presented the token={presented}; activate={got} (want granted); foot has the "
           f"keyboard={focused}; foot lines about XDG activation={len(warned)}"
           + (f" (first: {warned[0][:100]})" if warned else ""))

# ── S: viewporter, fractional scale ─────────────────────────────────────────
SGEOM = re.compile(r"compositor\.surface: surface geometry surface=(\d+) buffer=(\d+)x(\d+) scale=(\d+) "
                   r"transform=(\d+) src=(\S+) dst=(\S+) size=(\d+)x(\d+)")

s = scen.get("S1")
if s is not None:
    p = probe_lines("s1")
    i_pref = next((i for i, l in enumerate(p) if l.startswith("preferred-scale ")), None)
    i_cfg = next((i for i, l in enumerate(p) if l.startswith("toplevel configure ")), None)
    pref = p[i_pref].split()[1] if i_pref is not None else "-"
    bufs = [l for l in p if l.startswith("buffer ")]
    m = re.match(r"buffer (\d+)x(\d+) dst (\d+)x(\d+)", bufs[0]) if bufs else None
    one_to_one = bool(m) and m.group(1) == m.group(3) and m.group(2) == m.group(4)
    geo = [g for i in rng(s) if (g := SGEOM.search(comp[i]))]
    comp_ok = bool(geo) and bool(m) and geo[0].group(2) == m.group(1) and \
        geo[0].group(8) == m.group(3) and geo[0].group(9) == m.group(4)
    xs = [float(re.search(r"x=(-?[\d.]+)", l).group(1)) for l in p if l.startswith("pointer position ")]
    w = int(m.group(3)) if m else 0
    last_col = any(w - 1 <= x < w for x in xs)
    left = any(l == "pointer leave" for l in p[next((i for i, l in enumerate(p) if l.startswith("pointer position ") and w - 1 <= float(re.search(r"x=(-?[\d.]+)", l).group(1)) < w), len(p)):])
    ok = pref == "120" and i_cfg is not None and i_pref < i_cfg and one_to_one and comp_ok and last_col and left
    record("S1 fractional-scale: preferred 120 before the first configure, a 1:1 buffer, hit-testing to the last pixel",
           ok, f"preferred={pref} first={'preferred' if i_pref is not None and (i_cfg is None or i_pref < i_cfg) else 'configure'}; "
           f"{bufs[0] if bufs else 'no buffer line'}; compositor {geo[0].group(0)[len('compositor.surface: '):] if geo else '-'}; "
           f"last column seen={last_col} (x={max(xs) if xs else '-'}), then leave={left}")

def ppm(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i + 1:]

s = scen.get("S2")
if s is not None:
    tid = s["id"]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    geo = [m for i in rng(s) if (m := SGEOM.search(comp[i]))]
    geo_ok = bool(geo) and geo[-1].group(6) == "100.00,0.00,100.00x50.00" and geo[-1].group(7) == "300x150"
    img = ppm(s.get("shot", "")) if s.get("shot") else None
    detail_px = "no screenshot"
    px_ok = False
    if img and g:
        w, h, px = img
        x, y, gw, gh = g
        def at(u, v):
            o = (v * w + u) * 3
            return tuple(px[o:o + 3])
        interior = [at(u, v) for v in range(y + 6, y + gh - 6, 7) for u in range(x + 6, x + gw - 6, 7)]
        green = sum(1 for c in interior if c == (0, 255, 0))
        frame = [at(x, y + gh // 2), at(x + gw - 1, y + gh // 2), at(x + gw // 2, y), at(x + gw // 2, y + gh - 1)]
        frame_ok = all(c[0] >= 200 and c[1] >= 200 and c[2] >= 200 for c in frame)
        px_ok = interior and green == len(interior) and frame_ok
        detail_px = f"interior green {green}/{len(interior)}; frame {frame}"
    ok = bool(g) and g[2] == 300 and g[3] == 150 and geo_ok and px_ok
    record("S2 viewport crop + destination: mapped at the destination, the quadrant shown, framed", ok,
           f"geometry={g}; compositor {geo[-1].group(0)[len('compositor.surface: '):] if geo else '-'}; {detail_px}")

# ── P: wp_presentation ──────────────────────────────────────────────────────
PSUM = re.compile(r"presentation summary frames=(\d+) presented=(\d+) discarded=(\d+) pending=(\d+) "
                  r"seq_monotonic=(\d) seq_first=(\d+) seq_last=(\d+) refresh=(\d+) flags=0x([0-9a-f]+) "
                  r"dt_mean=([\d.]+) dt_max=([\d.]+) latency_mean=([\d.]+) sync_output=(\d+) "
                  r"clock_id=(\d+) t_monotonic=(\d)")
PPRES = re.compile(r"presented frame=(\d+) seq=(\d+) t=(\d+)\.(\d+) dt=([\d.]+) refresh=(\d+) "
                   r"flags=0x([0-9a-f]+) sync_output=(\d+) latency=(-?[\d.]+)")
PDISC = re.compile(r"discarded frame=(\d+) at=([\d.]+)")
PFLIP = re.compile(r"compositor\.presentation: flip frame=(\d+) ")
OUTMODE = re.compile(r"compositor\.output: output 0 (?:name=\S+ )?mode=\d+x\d+ refresh_mhz=(\d+)")
out_mhz = next((int(m.group(1)) for l in comp if (m := OUTMODE.search(l))), 0)
want_refresh = (10**12 + out_mhz // 2) // out_mhz if out_mhz else 0

def pres(name):
    lines = probe_lines(name)
    summ = next((m for l in lines if (m := PSUM.search(l))), None)
    shown = [(int(m.group(1)), int(m.group(2)), int(m.group(3)) + int(m.group(4)) / 1e9,
              int(m.group(6)), int(m.group(7), 16), int(m.group(8)), float(m.group(9)))
             for l in lines if (m := PPRES.search(l))]
    gone = [(int(m.group(1)), float(m.group(2))) for l in lines if (m := PDISC.search(l))]
    return summ, shown, gone

s = scen.get("P1")
if s is not None:
    summ, shown, gone = pres("p1")
    flips = sum(1 for i in rng(s) if PFLIP.search(comp[i]))
    if not summ:
        record("P1 continuous client: every frame presented with the flip's timing", False,
               "no summary from the probe")
    else:
        n, presented, discarded, pending = (int(summ.group(k)) for k in (1, 2, 3, 4))
        flags_ok = all(f & 0x5 == 0x5 and not f & 0x8 and bool(f & 0x2) == (seq > 0)
                       for _, seq, _, _, f, _, _ in shown)
        hw = any(f & 0x2 for _, _, _, _, f, _, _ in shown)
        seqs = [seq for _, seq, _, _, _, _, _ in shown]
        seq_ok = all(b > a for a, b in zip(seqs, seqs[1:])) if hw else all(q == 0 for q in seqs)
        refresh_ok = want_refresh > 0 and all(abs(r - want_refresh) <= 1 for _, _, _, r, _, _, _ in shown)
        ts = [t for _, _, t, _, _, _, _ in shown]
        dts = [b - a for a, b in zip(ts, ts[1:])]
        pace_ok = bool(dts) and min(dts) > 0 and sum(dts) / len(dts) >= 0.9 * want_refresh / 1e9
        lat = [l for _, _, _, _, _, _, l in shown]
        lat_ok = bool(lat) and min(lat) > 0 and sum(lat) / len(lat) < 100.0
        sync_ok = all(k >= 1 for _, _, _, _, _, k, _ in shown)
        ok = (summ.group(14) == "1" and n == 180 and presented >= 178 and discarded <= 2 and
              pending == 0 and flags_ok and seq_ok and refresh_ok and pace_ok and lat_ok and
              sync_ok and flips > 0)
        record("P1 continuous client: every frame presented with the flip's timing", ok,
               f"clock_id={summ.group(14)} frames={n} presented={presented} discarded={discarded} "
               f"pending={pending}; flags {sorted({hex(f) for _, _, _, _, f, _, _ in shown})} "
               f"(hw_clock={'yes' if hw else 'no: no vblank counter'}, consistent={flags_ok}); "
               f"seq {seqs[0] if seqs else '-'}..{seqs[-1] if seqs else '-'} ok={seq_ok}; "
               f"refresh {shown[0][3] if shown else '-'} want {want_refresh}; dt mean "
               f"{(sum(dts) / len(dts) * 1e3) if dts else 0:.2f} ms min {(min(dts) * 1e3) if dts else 0:.2f}; "
               f"latency mean {(sum(lat) / len(lat)) if lat else 0:.2f} ms; sync_output={sync_ok}; "
               f"compositor flip lines={flips}")

s = scen.get("P2")
if s is not None:
    summ, shown, gone = pres("p2")
    if not summ:
        record("P2 superseded commits are discarded, the last of each frame presented", False,
               "no summary from the probe")
    else:
        n, presented, discarded, pending = (int(summ.group(k)) for k in (1, 2, 3, 4))
        ok = n == 90 and presented + discarded == 90 and pending == 0 and discarded >= 55 and \
            presented >= 25
        record("P2 superseded commits are discarded, the last of each frame presented", ok,
               f"commits={n} presented={presented} discarded={discarded} pending={pending}")

s = scen.get("P3")
if s is not None:
    _, a_shown, a_gone = pres("p3a")
    _, b_shown, _ = pres("p3b")
    if not b_shown or not a_shown:
        record("P3 a covered client presents nothing and is throttled to ~1 Hz", False,
               f"presented: lower {len(a_shown)}, cover {len(b_shown)}")
    else:
        b0, b1 = b_shown[0][2], b_shown[-1][2]
        covered = [t for _, _, t, _, _, _, _ in a_shown if b0 + 0.1 < t < b1 - 0.1]
        throttled = [at for _, at in a_gone if b0 + 0.1 < at < b1]
        gaps = [y - x for x, y in zip(throttled, throttled[1:])]
        after = [t for _, _, t, _, _, _, _ in a_shown if t > b1 + 0.2]
        ok = (not covered and len(throttled) >= 2 and all(0.9 <= g <= 1.6 for g in gaps) and
              len(after) >= 10)
        record("P3 a covered client presents nothing and is throttled to ~1 Hz", ok,
               f"covered for {b1 - b0:.2f} s: lower presented {len(covered)}, discarded "
               f"{len(throttled)} at intervals {[round(g, 2) for g in gaps]} s; presented "
               f"{len(after)} after the cover left")

# ── K: idle, inhibition, display power ─────────────────────────────────────
KIDLED = re.compile(r"^idled t=([\d.]+) at=([\d.]+)")
KRESUMED = re.compile(r"^resumed t=([\d.]+) at=([\d.]+)")

def k_events(name):
    out = []
    for l in probe_lines(name):
        if (m := KIDLED.match(l)):
            out.append(("idled", float(m.group(1)), float(m.group(2))))
        elif (m := KRESUMED.match(l)):
            out.append(("resumed", float(m.group(1)), float(m.group(2))))
    return out

s = scen.get("K1")
if s is not None:
    ev = k_events("k1")
    moves = [r["t_us"] / 1e6 for r in inj if r.get("tag") == "K1" and r["kind"] == "motion"]
    kinds = [e[0] for e in ev]
    ok = kinds[:3] == ["idled", "resumed", "idled"] and bool(moves)
    if ok:
        first, resumed, again = ev[0], ev[1], ev[2]
        # The probe prints milliseconds (%.3f, rounded) and the injector microseconds: a
        # resume within the same millisecond as the motion can read up to 0.5 ms "early".
        ok = (1.45 <= first[1] <= 1.9 and moves[0] - 0.001 <= resumed[2] <= moves[0] + 0.3 and
              1.45 <= again[2] - resumed[2] <= 1.9)
    record("K1 idled after the timeout, resumed by input, idled again", ok,
           f"events {[(k, round(t, 3)) for k, t, _ in ev]}; injected motion at "
           f"{round(moves[0], 3) if moves else '-'} (resumed at "
           f"{round(ev[1][2], 3) if len(ev) > 1 else '-'})")

s = scen.get("K2")
if s is not None:
    obey, inonly = k_events("k2"), k_events("k2in")
    cover = s.get("cover_at", 0.0)
    idled_obey = [at for k, _, at in obey if k == "idled"]
    idled_in = [t for k, t, _ in inonly if k == "idled"]
    inhibited = any("compositor.idle: idle inhibited=1" in comp[i] for i in rng(s))
    ok = (inhibited and bool(idled_in) and 1.45 <= idled_in[0] <= 1.9 and
          len(idled_obey) == 1 and cover + 1.4 <= idled_obey[0] <= cover + 2.4 and
          not any(k == "resumed" for k, _, _ in obey))
    record("K2 a visible inhibitor holds idling back; covered, it does not (input-only ignores it)", ok,
           f"inhibited seen={inhibited}; input-only idled at t={idled_in[0] if idled_in else '-'}; "
           f"obeying idled {round(idled_obey[0] - cover, 3) if idled_obey else '-'} s after the cover "
           f"started (want 1.5 + map time); obeying events {[(k, round(t, 3)) for k, t, _ in obey]}")

s = scen.get("K3")
if s is not None:
    r = rng(s)
    off = next((i for i in r if "display power off (CRTC ACTIVE=0" in comp[i]), None)
    woke = next((i for i in r if "display woken by input" in comp[i]), None)
    on = next((i for i in r if "compositor.power: display power on" in comp[i]), None)
    dark_commits = [comp[i] for i in range(off + 1, woke)] if off is not None and woke else []
    dark_commits = [l for l in dark_commits if re.search(r"compositor\.render: .*commit=(content|cursor) ", l)]
    full = f"area={out_w * out_h} " if out_w and out_h else "area=?"
    repaint = woke is not None and any(
        "composite=1 rects=1 " + full in comp[i] and "commit=content" in comp[i]
        for i in range(woke, min(r.stop, woke + 40)))
    modes = [l.split()[1] for l in probe_lines("k3p") if l.startswith("power mode=")]
    keys = [l.strip() for l in probe_lines("k3k") if l.startswith("key code=")]
    ok = (off is not None and woke is not None and on is not None and off < woke < on and
          not dark_commits and repaint and modes == ["mode=on", "mode=off", "mode=on"] and
          keys == ["key code=48 state=1", "key code=48 state=0"])
    record("K3 power off: dark with no commits; a key wakes it (swallowed), full repaint, mode events", ok,
           f"off/woken/on lines={off is not None}/{woke is not None}/{on is not None}; commits while "
           f"dark={len(dark_commits)}; full repaint after wake={repaint}; power object {modes}; "
           f"keys at the focused client {keys}")

# ── L: session lock ──────────────────────────────────────────────────────────
LOCKT = re.compile(r"compositor\.lock: session lock: (\w+) -> (\w+)")
LLOCKED = re.compile(r"^lock locked at=([\d.]+) after=([\d.]+)")

def lock_transitions(r):
    return [(m.group(1), m.group(2)) for i in r if (m := LOCKT.search(comp[i]))]

def shot_colour(path):
    img = ppm(path) if path else None
    if not img:
        return None, 0.0
    w, h, px = img
    counts = {}
    n = 0
    for v in range(0, h, 9):
        for u in range(0, w, 9):
            o = (v * w + u) * 3
            c = tuple(px[o:o + 3])
            counts[c] = counts.get(c, 0) + 1
            n += 1
    c, k = max(counts.items(), key=lambda kv: kv[1])
    return c, k / n if n else 0.0

s = scen.get("L1")
if s is not None:
    t = lock_transitions(rng(s))
    lk = probe_lines("l1")
    win = probe_lines("l1w")
    locked = [m for l in lk if (m := LLOCKED.match(l))]
    after = float(locked[0].group(2)) if locked else -1.0
    lock_keys = [l for l in lk if l.startswith("key code=") and l.endswith("state=1")]
    lock_kb = any(l.startswith("keyboard enter surface=lock") for l in lk)
    # The window: the keyboard leaves at the request; it sees none of the keys typed while
    # locked (A=30, the digits 4/1/2 = 5/2/3) and no press; afterwards B (48) reaches it.
    leave_i = next((i for i, l in enumerate(win) if l.startswith("keyboard leave")), None)
    leaked = [l for l in win if re.match(r"key code=(30|5|2|3) state=1", l)]
    pressed_after = [l for l in win[(leave_i or 0):] if l.startswith("press surface=")]
    b_after = any(l == "key code=48 state=1" for l in win[(leave_i or 0):])
    ok = (t[:3] == [("unlocked", "awaiting_surfaces"), ("awaiting_surfaces", "locking"),
                    ("locking", "locked")] and ("locked", "unlocked") in t and
          0.0 <= after <= 0.5 and lock_kb and len(lock_keys) >= 7 and
          "lock password wrong" in lk and "lock password accepted" in lk and
          any(l.startswith("lock unlocked") for l in lk) and leave_i is not None and
          not leaked and not pressed_after and b_after)
    record("L1 lock: locked after the locked frame, input to the lock surface only, a test password unlocks", ok,
           f"transitions {t}; locked {after:.3f} s after the request; lock surface keyboard={lock_kb}, "
           f"keys {len(lock_keys)}; window: leave={leave_i is not None}, leaked keys {leaked}, "
           f"presses while locked {len(pressed_after)}, B after unlock={b_after}")

s = scen.get("L2")
if s is not None:
    t = lock_transitions(rng(s))
    colour, share = shot_colour(s.get("shot", ""))
    win = probe_lines("l1w")
    leaked = [l for l in win if l.startswith("key code=46 ")]
    rep = [m for l in probe_lines("l2r") if (m := LLOCKED.match(l))]
    rep_after = float(rep[0].group(2)) if rep else -1.0
    ok = (("locked", "abandoned") in t and ("abandoned", "locked") in t and
          ("locked", "unlocked") in t and colour == (0x3f, 0, 0) and share > 0.99 and
          not leaked and 0.0 <= rep_after <= 0.1)
    record("L2 locker died: stays locked (dark red), keys to nobody; a replacement is locked at once", ok,
           f"transitions {t}; screenshot {colour} over {share:.3f} of the output; keys leaked {len(leaked)}; "
           f"replacement locked {rep_after:.3f} s after its request")

s = scen.get("L3")
if s is not None:
    t = lock_transitions(rng(s))
    rep = [m for l in probe_lines("l3") if (m := LLOCKED.match(l))]
    after = float(rep[0].group(2)) if rep else -1.0
    colour, share = shot_colour(s.get("shot", ""))
    ok = (0.95 <= after <= 1.6 and ("locking", "locked") in t and colour == (0, 0, 0) and
          share > 0.99)
    record("L3 no lock surface: locked after the 1 s wait, the output a solid colour", ok,
           f"locked {after:.3f} s after the request; transitions {t}; screenshot {colour} over {share:.3f}")

s = scen.get("L4")
if s is not None:
    t = lock_transitions(rng(s))
    lk = probe_lines("l4")
    logind = any("compositor.lock: logind Unlock: unlocking the session" in comp[i] for i in rng(s))
    ok = (logind and any(l.startswith("lock finished") and l.endswith("locked=1") for l in lk) and
          ("locked", "unlocked") in t)
    record("L4 logind Unlock: the live locker is finished, the session unlocked", ok,
           f"logind line={logind}; locker {[l for l in lk if l.startswith('lock ')]}; transitions {t}")

# ── M: text input and input methods ────────────────────────────────────────
IMESHOW = re.compile(r"compositor\.ime: ime popup show surface=(\d+) popup=\d+ x=(-?\d+) y=(-?\d+) (\d+)x(\d+)")

s = scen.get("M1")
if s is not None:
    ti = probe_lines("m1")
    im = probe_lines("m1im")
    tid = s.get("id", 0)
    g = geometry_at(tid, s["comp"][1]) if tid else None
    shows = [m for i in rng(s) if (m := IMESHOW.search(comp[i]))]
    first = shows[0] if shows else None
    want_xy = (g[0] + 20, g[1] + 40 + 18) if g else None
    at_cursor = bool(first) and want_xy is not None and (int(first.group(2)), int(first.group(3))) == want_xy
    rect = any(l.startswith("im popup rect x=0 y=-18 w=2 h=18") for l in im)
    pre = [l for l in ti if l.startswith("ti preedit ")]
    commit = [l for l in ti if l.startswith("ti commit ")]
    done_commit = [l for l in ti if l.startswith('ti done ') and 'text="αα"' in l]
    window_keys = [l for l in ti if re.match(r"key code=(30|57) ", l)]
    b_fwd = "key code=48 state=1" in ti and any(l.startswith("im forwarded code=48 state=1") for l in im)
    # The input method serves M1–M5: count M1's presses, those before its first commit.
    first_commit = next((k for k, l in enumerate(im) if l.startswith("im commit ")), len(im))
    grab_a = sum(1 for l in im[:first_commit] if l == "im key code=30 state=1")
    entered = "ti enter surface=toplevel" in ti and any(l.startswith("im activate") for l in im)
    img = ppm(s.get("shot", "")) if s.get("shot") else None
    on_screen = False
    if img and first:
        w, h, px = img
        x, y = int(first.group(2)) + 5, int(first.group(3)) + 5
        if 0 <= x < w and 0 <= y < h:
            o = (y * w + x) * 3
            on_screen = tuple(px[o:o + 3]) == (0xe0, 0xc0, 0x40)
    # The click on the popup: pointer focus `ime-popup`, the input method gets the press, no
    # keyboard change (KEY_F still goes to the grab and is forwarded to the window).
    r = rng(s)
    ptr = any("compositor.wm: focus pointer ime-popup" in comp[i] for i in r)
    pressed = any(l.startswith("press surface=im-popup") for l in im)
    kept = (not any("compositor.ime: text input leave" in comp[i] for i in r) and
            any(l.startswith("im forwarded code=33 state=1") for l in im) and "key code=33 state=1" in ti)
    ok = (entered and pre[:2] == ['ti preedit "α" 2 2', 'ti preedit "αα" 4 4'] and
          commit[:1] == ['ti commit "αα"'] and bool(done_commit) and not window_keys and
          grab_a == 2 and b_fwd and at_cursor and rect and on_screen and ptr and pressed and kept)
    record("M1 compose: keys to the grab, preedit and commit back; the popup at the cursor", ok,
           f"entered+activated={entered}; preedit {pre[:2]}; commit {commit[:1]}; done with the text="
           f"{bool(done_commit)}; KEY_A at the grab {grab_a}, at the window {len(window_keys)}; B forwarded="
           f"{b_fwd}; popup first shown at {(first.group(2), first.group(3)) if first else None} want {want_xy}; "
           f"text_input_rectangle={rect}; on screen={on_screen}; click on it: pointer={ptr} "
           f"press at the input method={pressed} keyboard kept={kept}")

s = scen.get("M2")
if s is not None:
    r = rng(s)
    ti = probe_lines("m1")
    im = probe_lines("m1im")
    other = probe_lines("m2")
    lines_ = [comp[i] for i in r]
    left = any("compositor.ime: text input leave" in l for l in lines_)
    hidden = any("compositor.ime: ime popup hide" in l for l in lines_)
    deact = any("compositor.ime: input method deactivate" in l for l in lines_)
    back = [i for i in r if "compositor.ime: input method activate" in comp[i]]
    c_other = "key code=46 state=1" in other
    c_grab = any(l.startswith("im key code=46") for l in im)
    d_fwd = any(l.startswith("im forwarded code=32 state=1") for l in im) and "key code=32 state=1" in ti
    ok = left and hidden and deact and bool(back) and c_other and not c_grab and d_fwd
    record("M2 focus elsewhere: left, deactivated, popup hidden, keys direct; back: active again", ok,
           f"leave={left} deactivate={deact} hide={hidden}; KEY_C at the other window={c_other}, at the "
           f"grab={c_grab}; re-activated={bool(back)}; KEY_D forwarded to the window={d_fwd}")

s = scen.get("M3")
if s is not None:
    r = rng(s)
    lines_ = [comp[i] for i in r]
    lk = probe_lines("m3")
    im = probe_lines("m1im")
    left = any("compositor.ime: text input leave" in l for l in lines_)
    deact = any("compositor.ime: input method deactivate" in l for l in lines_)
    hidden = any("compositor.ime: ime popup hide" in l for l in lines_)
    e_lock = "key code=18 state=1" in lk
    e_grab = any(l.startswith("im key code=18") for l in im)
    unlocked = any(l.startswith("lock unlocked") for l in lk)
    again = any("compositor.ime: input method activate" in l for l in lines_)
    ok = left and deact and hidden and e_lock and not e_grab and unlocked and again
    record("M3 locked: no text-input focus, keys to the lock surface; unlocked: active again", ok,
           f"leave={left} deactivate={deact} hide={hidden}; KEY_E at the lock surface={e_lock}, at the "
           f"grab={e_grab}; unlocked={unlocked}; re-activated={again}")

s = scen.get("M4")
if s is not None:
    raw = read("foot-m4.log")
    warn_ti = [l for l in raw if re.search(r"text.input", l, re.I) and not WLDBG.match(l)]
    dbg = wl_debug("foot-m4")
    pre = [a for req, i, _, msg, a in dbg if not req and i == "zwp_text_input_v3" and msg == "preedit_string"]
    com = [a for req, i, _, msg, a in dbg if not req and i == "zwp_text_input_v3" and msg == "commit_string"]
    try:
        out = open(os.path.join(log_dir, "M4.out"), encoding="utf-8").read()
    except OSError:
        out = None
    ok = (not warn_ti and any(a.startswith('"α"') for a in pre) and any(a.startswith('"αα"') for a in pre)
          and any(a.startswith('"αα"') for a in com) and out == "ααxyz\n")
    record("M4 foot: composes through text-input-v3; the file holds the committed text and the forwarded keys", ok,
           f"foot text-input warnings={len(warn_ti)}; preedit {pre[:3]}; commit {com[:2]}; file {out!r}")

s = scen.get("M5")
if s is not None:
    if s.get("gtk_skipped"):
        record("M5 GTK 4 entry composes", True, "skipped (no GTK)", warn=True)
    else:
        dbg = wl_debug("gtk-m5")
        binds = [a for req, i, _, msg, a in dbg if req and msg == "bind" and "zwp_text_input_manager_v3" in a]
        pre = [a for req, i, _, msg, a in dbg if not req and i == "zwp_text_input_v3" and msg == "preedit_string"]
        com = [a for req, i, _, msg, a in dbg if not req and i == "zwp_text_input_v3" and msg == "commit_string"]
        ok = (bool(binds) and any(a.startswith('"αα"') for a in pre) and any(a.startswith('"αα"') for a in com))
        # WARN-level: GTK's own focus and IM-context choices are not this compositor's.
        record("M5 GTK 4 entry composes: preedit and commit through text-input v2", True,
               f"bind {binds[:1]}; preedit {pre[:2]}; commit {com[:1]}", warn=not ok)

if "S1" in scen:
    warned = []
    for name in sorted(os.listdir(log_dir)):
        if name.startswith("foot") and name.endswith(".log"):
            warned += [f"{name}: {l.strip()[:100]}" for l in read(name)
                       if re.search(r"fractional scal", l, re.I) and not WLDBG.match(l)]
    dbg = wl_debug("gtk-factory-h5") if "H5" in scen and not scen["H5"].get("gtk_skipped") else []
    gtk_bound = {i for req, i, _, msg, a in dbg if req and msg == "bind"} if dbg else set()
    gtk_raw = read("gtk-factory-h5.log") if dbg else []
    gtk_frac = any("wp_fractional_scale_manager_v1" in l and "bind" in l for l in gtk_raw)
    gtk_vp = any("wp_viewporter" in l and "bind" in l for l in gtk_raw)
    gtk_pref = any(re.search(r"wp_fractional_scale_v1[#@]\d+\.preferred_scale\(120\)", l) for l in gtk_raw)
    gtk_ok = (not dbg) or (gtk_frac and gtk_vp and gtk_pref)
    ok = not warned and gtk_ok
    record("S3 real clients: foot does not miss fractional scaling; GTK binds both and is told 120", ok,
           f"foot warnings={len(warned)}" + (f" (first: {warned[0]})" if warned else "") +
           (f"; GTK bound fractional={gtk_frac} viewporter={gtk_vp} preferred_scale(120)={gtk_pref}"
            if dbg else "; GTK trace not run (H5 skipped)"))

# ── T: commit-path protocols ────────────────────────────────────────────────
# libwayland's trace stamp: "[HH:MM:SS.uuuuuu]" (1.23+) or "[ms.uuu]" (older).
WLTRACE = re.compile(r"\[\s*(?:(\d+):(\d+):)?(\d+\.\d+)\].*-> (\w+)#(\d+)\.(\w+)\(")
CUQ = re.compile(r"compositor\.surface: content update queued surface=(\d+) cause=\S+ depth=(\d+) "
                 r"waits=(\d+) fifo=(\d) barrier=(\d) target=(\d+)")

def wl_requests(name):
    out = []
    for l in read(name):
        m = WLTRACE.search(l)
        if m:
            if m.group(1) is not None:
                t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
            else:
                t = float(m.group(3)) / 1000.0
            out.append((t, m.group(4), m.group(6)))
    return out

def commit_rate(reqs, request="commit"):
    ts = [t for t, i, r in reqs if i == "wl_surface" and r == request]
    if len(ts) < 10:
        return len(ts), 0.0
    # The steady part: skip the first second (setup, first frames).
    t0 = ts[0] + 1.0
    steady = [t for t in ts if t >= t0]
    if len(steady) < 2:
        return len(ts), 0.0
    return len(ts), (len(steady) - 1) / (steady[-1] - steady[0])

refresh_hz = out_mhz / 1000.0 if out_mhz else 60.0

s = scen.get("T1")
if s is not None:
    if s.get("skipped"):
        record("T1 vkcube FIFO present", True, "vkcube not installed", warn=True)
    else:
        reqs = wl_requests("vkcube-t1.log")
        sets = sum(1 for _, i, r in reqs if i == "wp_fifo_v1" and r == "set_barrier")
        waits = sum(1 for _, i, r in reqs if i == "wp_fifo_v1" and r == "wait_barrier")
        timers = sum(1 for _, i, r in reqs if i == "wp_commit_timer_v1" and r == "set_timestamp")
        # Mesa commits twice per present (the frame with set_barrier + wait_barrier, then an
        # empty wait_barrier commit that throttles the next): frames are the attaches.
        n, rate = commit_rate(reqs, "attach")
        FBP = re.compile(r"wp_presentation_feedback#\d+\.presented\((\d+), (\d+), (\d+),")
        shown = [int(m.group(2)) + int(m.group(3)) / 1e9 for l in read("vkcube-t1.log")
                 if (m := FBP.search(l))]
        gone = sum(1 for l in read("vkcube-t1.log") if re.search(r"wp_presentation_feedback#\d+\.discarded\(", l))
        dts = [b - a for a, b in zip(shown, shown[1:])]
        mean_dt = sum(dts) / len(dts) if dts else 0.0
        queued = [m for i in rng(s) if (m := CUQ.search(comp[i])) and m.group(4) == "1"]
        fifo_ok = sets > 0 and waits > 0
        rate_ok = rate > 0 and rate <= refresh_hz * 1.05 + 1
        period = 1.0 / refresh_hz
        pace_ok = bool(dts) and abs(mean_dt - period) <= 0.15 * period and gone <= max(3, n // 20)
        ok = fifo_ok and rate_ok and pace_ok
        record("T1 vkcube (Mesa Vulkan WSI, FIFO): fifo-v1 barriers, one frame per refresh",
               ok, f"frames={n} rate={rate:.1f}/s refresh={refresh_hz:.2f} Hz; presented {len(shown)} "
               f"discarded {gone}, interval mean {mean_dt * 1e3:.2f} ms; set_barrier={sets} "
               f"wait_barrier={waits} set_timestamp={timers}; compositor held {len(queued)} "
               f"update(s) on the barrier", warn=ok and not queued)

s = scen.get("T2")
if s is not None:
    if s.get("skipped"):
        record("T2 weston-simple-timing", True, "not installed", warn=True)
    else:
        reqs = wl_requests("simple-timing-t2.log")
        stamps = sum(1 for _, i, r in reqs if i == "wp_commit_timer_v1" and r == "set_timestamp")
        timed = [m for i in rng(s) if (m := CUQ.search(comp[i])) and m.group(6) != "0"]
        errs = [l for l in read("simple-timing-t2.log") if "error" in l.lower() and "wl_display" in l]
        ok = stamps > 0 and not errs
        record("T2 weston-simple-timing: commit-timing targets held until their frame", ok,
               f"set_timestamp={stamps}; compositor held {len(timed)} timed update(s); "
               f"protocol errors={len(errs)}", warn=ok and not timed)

s = scen.get("T3")
if s is not None:
    TSUM = re.compile(r"timing summary fifo_frames=(\d+) fifo_presented=(\d+) fifo_discarded=(\d+) "
                      r"dt_min_ms=([\d.]+) dt_mean_ms=([\d.]+) refresh_ms=([\d.]+) timed_frames=(\d+) "
                      r"timed_presented=(\d+) early=(\d+) late=(\d+) delta_min_ms=(-?[\d.]+) "
                      r"delta_max_ms=(-?[\d.]+)")
    summ = next((m for l in probe_lines("t3") if (m := TSUM.search(l))), None)
    queued = [m for i in rng(s) if (m := CUQ.search(comp[i]))]
    if not summ:
        record("T3 probe timing: FIFO one update per refresh, timed updates on target", False,
               "no summary from the probe: " + " | ".join(probe_lines("t3")[-3:]))
    else:
        g = [summ.group(k) for k in range(1, 13)]
        frames, pres, disc = int(g[0]), int(g[1]), int(g[2])
        dt_min, dt_mean, period = float(g[3]), float(g[4]), float(g[5])
        tframes, tpres, early, late = int(g[6]), int(g[7]), int(g[8]), int(g[9])
        fifo_ok = pres >= frames - 1 and disc <= 1 and dt_min >= 0.5 * period and \
            abs(dt_mean - period) <= 0.15 * period
        timed_ok = tpres >= tframes - 1 and early == 0 and late <= 1
        held = sum(1 for m in queued if m.group(4) == "1")
        held_t = sum(1 for m in queued if m.group(6) != "0")
        ok = fifo_ok and timed_ok and held > 0 and held_t > 0
        record("T3 probe timing: FIFO one update per refresh, timed updates on target", ok,
               f"fifo presented {pres}/{frames} discarded {disc}, dt min {dt_min:.2f} mean "
               f"{dt_mean:.2f} ms (refresh {period:.2f}); timed presented {tpres}/{tframes}, "
               f"early {early} late {late}, delta {g[10]}..{g[11]} ms; compositor held "
               f"{held} fifo / {held_t} timed update(s)")

s = scen.get("T4")
if s is not None:
    tid = s["id"]
    rg = list(rng(s))
    solid = [comp[i] for i in rg if "compositor.surface: surface solid " in comp[i]]
    alpha = [comp[i] for i in rg if "compositor.surface: surface alpha " in comp[i]]
    ctype = [comp[i] for i in rg if "surface content type" in comp[i] and "type=video" in comp[i]]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    img = ppm(s.get("shot", "")) if s.get("shot") else None
    detail_px, px_ok = "no screenshot", False
    if img and g:
        w, h, px = img
        x, y, gw, gh = g
        samples = [tuple(px[((v * w + u) * 3):((v * w + u) * 3 + 3)])
                   for v in range(y + 10, y + gh - 10, 23) for u in range(x + 10, x + gw - 10, 23)
                   if 0 <= u < w and 0 <= v < h]
        # Half-alpha green over black: 128 (blending in encoded space, GL) to 188 (linear,
        # CPU / Vulkan); red and blue stay black.
        good = [c for c in samples if 118 <= c[1] <= 198 and c[0] <= 12 and c[2] <= 12]
        px_ok = bool(samples) and len(good) == len(samples)
        detail_px = f"{len(good)}/{len(samples)} samples half green (first {samples[0] if samples else '-'})"
    ok = bool(solid) and "size=320x240" in solid[-1] and "opaque=1" in solid[-1] and \
        bool(alpha) and "multiplier=0.5" in alpha[-1] and bool(ctype) and px_ok
    record("T4 single-pixel + viewport + alpha + content type: a solid, half green, typed video", ok,
           f"{solid[-1].split('compositor.surface: ')[-1] if solid else 'no solid line'}; "
           f"{alpha[-1].split('compositor.surface: ')[-1] if alpha else 'no alpha line'}; "
           f"content type video={bool(ctype)}; geometry={g}; {detail_px}")

s = scen.get("T5")
if s is not None:
    rg = list(rng(s))
    hint = any("surface presentation hint" in comp[i] and "hint=async" in comp[i] for i in rg)
    unsupported = any("async page flip unsupported" in comp[i] for i in rg)
    capable = any("async page flips (tearing-control-v1): available" in l for l in comp)
    ok = hint and (unsupported or capable)
    record("T5 tearing-control: the async hint applied; no async flip without the card's support",
           ok, f"hint applied={hint}; card async flips={'yes' if capable else 'no'}; "
           f"'async page flip unsupported' reported={unsupported}")

s = scen.get("T6")
if s is not None:
    if s.get("skipped"):
        record("T6 weston-simple-egl implicit sync", True, "not installed", warn=True)
    else:
        reqs = wl_requests("simple-egl-t6.log")
        n, rate = commit_rate(reqs)
        dmabuf = sum(1 for _, i, r in reqs if i == "zwp_linux_buffer_params_v1")
        waited = [m for i in rng(s) if (m := CUQ.search(comp[i])) and int(m.group(3)) > 0]
        ok = n > 20 and rate > 0
        record("T6 weston-simple-egl: dma-buf frames keep coming (implicit fences checked)", ok,
               f"commits={n} rate={rate:.1f}/s; dma-buf params requests={dmabuf}; updates that "
               f"waited for their buffer's fences={len(waited)}")

# ── U: dialogs, icons, bell, xdg-foreign (design-app-compat-capture Part B) ──
SHUV = read("shell-uv.log")

def comp_in(s, pattern):
    rx = re.compile(pattern)
    return [comp[i] for i in rng(s) if rx.search(comp[i])]

def centre(g):
    return (g[0] + g[2] / 2.0, g[1] + g[3] / 2.0) if g else None

s = scen.get("U1")
if s is not None:
    pid, did = s.get("parent", 0), s.get("dialog", 0)
    lines_u1 = probe_lines("u1")
    gp = geometry_at(pid, s["comp"][1]) if pid else None
    gd = geometry_at(did, s["comp"][1]) if did else None
    cp, cd = centre(gp), centre(gd)
    centred = bool(cp and cd) and abs(cp[0] - cd[0]) <= 1 and abs(cp[1] - cd[1]) <= 1
    parented = bool(comp_in(s, rf"compositor\.wm: parent id={did} parent={pid} "))
    pressed = comp_in(s, rf"compositor\.wm: modal press id={pid} dialog={did}")
    focus = [l for l in lines_u1 if l.startswith("focus ")]
    buttons = [l for l in lines_u1 if l.startswith("button ")]
    ok = bool(pid and did) and centred and parented and bool(pressed) and \
        bool(focus) and focus[-1] == "focus dialog" and "button parent" not in buttons
    record("U1 xdg-dialog (probe): a modal child centred on its parent; a click on the parent "
           "is consumed and the dialog keeps the keyboard", ok,
           f"parent {pid} {gp} dialog {did} {gd} centred={centred}; parented={parented}; "
           f"modal press lines={len(pressed)}; probe focus {focus[-3:]} buttons {buttons}")

s = scen.get("U2")
if s is not None:
    if s.get("skipped"):
        record("U2 GTK 4 modal dialog", True, "gtk4-demo not available", warn=True)
    else:
        pid, did = s.get("parent", 0), s.get("dialog", 0)
        dbg = wl_debug("gtk-u2")
        got_dialog = any(req and i == "xdg_wm_dialog_v1" and msg == "get_xdg_dialog" for req, i, _, msg, _a in dbg)
        set_modal = any(req and i == "xdg_dialog_v1" and msg == "set_modal" for req, i, _, msg, _a in dbg)
        pressed = comp_in(s, rf"compositor\.wm: modal press id={pid} dialog={did}") if pid and did else []
        presses = wl_presses(dbg)
        ok = bool(did) and got_dialog and set_modal and bool(pressed)
        record("U2 GTK 4 (gtk4-demo Dialogs, Alt+M): xdg_dialog_v1 set_modal; a click on the "
               "parent is consumed and focuses the dialog", ok,
               f"parent {pid} dialog {did}; get_xdg_dialog={got_dialog} set_modal={set_modal}; "
               f"modal press lines={len(pressed)}; presses GTK saw={len(presses)}", warn=not ok)

s = scen.get("U3")
if s is not None:
    foot_log = read("foot-u3.log")
    foot_warn = [l for l in foot_log if "does not implement the xdg-toplevel-icon" in l]
    applied = comp_in(s, r"compositor\.icon: icon applied toplevel=\d+ name=\S+ buffers=\d+")
    shell_icons = [l for l in SHUV if "shell.bridge: icon handle=" in l]
    probe_ok = any("name=lumen-probe-u3-icon buffers=2" in l for l in shell_icons)
    foot_icon = [l for l in shell_icons if "name=foot" in l]
    pref = [l for l in probe_lines("u3") if l.startswith("icon set")]
    ok = not foot_warn and probe_ok and bool(foot_icon) and bool(pref) and "preferred=16,32" in pref[0]
    record("U3 xdg-toplevel-icon: foot's icon name and the probe's name + 2 buffers reach the "
           "shell; foot does not warn", ok,
           f"compositor applied {len(applied)}; shell icon lines {len(shell_icons)} (foot {len(foot_icon)}, "
           f"probe with 2 buffers {probe_ok}); probe {pref[0] if pref else 'no line'}; foot warnings {len(foot_warn)}")

s3, s4 = scen.get("U3"), scen.get("U4")
if s4 is not None:
    rg = list(range(s3["comp"][0] if s3 else s4["comp"][0], s4["comp"][1]))
    sent = [comp[i] for i in rg if re.search(r"compositor\.bell: bell client=\d+ toplevel=\d+ result=sent", comp[i])]
    limited = [comp[i] for i in rg if "result=rate-limited" in comp[i]]
    flashes = [comp[i] for i in rg if "namespace=lumen-bell" in comp[i] and "layer map" in comp[i]]
    shell_bells = [l for l in SHUV if "shell.bridge: bell handle=" in l]
    foot_warn = [l for l in read("foot-u3.log") if "does not implement the XDG system bell" in l]
    foot_bell = any("toplevel=" in l and f"toplevel={s3.get('foot', 0)} " in l for l in sent) if s3 else False
    ok = len(sent) >= 4 and bool(limited) and len(shell_bells) >= 4 and bool(flashes) and not foot_warn
    record("U4 xdg-system-bell: foot's \\a and the probe's rings reach the shell (rate-limited to "
           "4/s per client), which flashes the window", ok,
           f"sent {len(sent)} (foot's {foot_bell}), rate-limited {len(limited)}, shell bells "
           f"{len(shell_bells)}, flash layer maps {len(flashes)}; foot warnings {len(foot_warn)}",
           warn=ok and not foot_bell)

s = scen.get("U5")
if s is not None:
    li, lv = probe_lines("u5i"), probe_lines("u5v1i")
    he, hv = probe_lines("u5e"), probe_lines("u5v1e")
    parented = comp_in(s, r"compositor\.foreign: set_parent_of child=\d+ parent=\d+")
    modal = comp_in(s, r"compositor\.wm: modal id=\d+ on=1 ")
    seq_ok = lambda l: "imported" in l and "destroyed" in l and l.index("imported") < l.index("destroyed")
    ok = any(x.startswith("handle=") for x in he) and any(x.startswith("handle=") for x in hv) and \
        seq_ok(li) and seq_ok(lv) and len(parented) >= 2 and bool(modal)
    record("U5 xdg-foreign v2 and v1: imported across clients (modal over the other client's "
           "window), `destroyed` when the exporter goes", ok,
           f"v2 import {li}; v1 import {lv}; set_parent_of lines {len(parented)}; modal parent lines {len(modal)}")

# ── V: shortcuts inhibit, data control, security context ──
# ── R: the configuration file ───────────────────────────────────────────────
def since(s, rx):
    a, b = s["comp"]
    return [comp[i] for i in range(a, b) if re.search(rx, comp[i])]

s = scen.get("R1")
if s is not None:
    p = probe_lines("r1")
    keymap = [l for l in p if l.startswith("keymap ")]
    rep = [l for l in p if l.startswith("repeat ")]
    dev = since(s, r'compositor\.input: input device ".*" \(mouse\): natural-scroll=on accel-profile=flat accel-speed=0\.50')
    cur = since(s, r"compositor\.cursor: cursor theme=\S+ size=48 \(configuration\)")
    gen = since(s, r"config: config: generation \d+ from .* \(0 errors, 0 warnings")
    ok = bool(keymap) and "layouts=2 names=English (US)|German" in keymap[-1] and \
        rep[-1:] == ["repeat rate=10 delay=600"] and bool(dev) and bool(cur) and bool(gen) and \
        "close requested" in p
    record("R1 config.toml appears: repeat 10/600 and us,de reach a client, the mouse flat 0.5 "
           "with natural scrolling, cursor 48 px; Super+Y (configured) closes the window", ok,
           f"keymap {keymap[-1:]}; repeat {rep[-1:]}; mouse lines {len(dev)}; cursor {len(cur)}; "
           f"generation {gen[-1:]}; closed={'close requested' in p}")

s = scen.get("R2")
if s is not None:
    keys = [l for l in probe_lines("r2") if l.startswith("key code=21 state=1")]
    ok = len(keys) >= 2 and "utf8=y layout=0" in keys[0] and "utf8=z layout=1" in keys[1]
    record("R2 grp:alt_shift_toggle: y, Alt+Shift, y types y then z (German)", ok, f"{keys[:2]}")

s = scen.get("R3")
if s is not None:
    p = probe_lines("r2")
    gen = since(s, r"config: config: generation \d+ from")
    notices = s.get("notices", -1)
    ms = s.get("ms", 99999)
    ok = "repeat rate=30 delay=600" in p and len(gen) == 1 and notices == 0 and ms <= 400
    record("R3 a live edit (rename): repeat 30 reaches the client, one generation, no notice", ok,
           f"{ms} ms from the rename to repeat_info (100 ms debounce); generations {len(gen)}; "
           f"notices so far {notices}", warn=ok and ms > 250)

s = scen.get("R4")
if s is not None:
    p = probe_lines("r2")
    axis = [l for l in p if l.startswith("axis vertical ")]
    rel = [l for l in p if l.startswith("relative ")]
    # Natural (positive for a notch up) and doubled (scroll-factor 2: 15 → 30, one notch → 2).
    up = axis and float(axis[-1].split()[2]) == 30.0 and axis[-1].endswith("discrete=2")
    flat = rel and rel[-1].startswith("relative 15.00,0.00 unaccel 10.00,0.00")
    record("R4 the mouse: natural scrolling doubled by scroll-factor 2 (a notch up: +30, 2 "
           "notches), flat acceleration x1.5",
           bool(up and flat), f"axis {axis[-1:]}; relative {rel[-1:]}")

s = scen.get("R5")
if s is not None:
    rejected = since(s, r"config: config: generation \d+ from .* rejected")
    try:
        reload = open(os.path.join(log_dir, "R5.reload"), encoding="utf-8").read()
    except FileNotFoundError:
        reload = ""
    shown = [l for l in read("shell-r.log") if "config: notice: Lumen configuration not used: 1 error" in l]
    # Shown as a notification (Part 5) once the shell's notification server runs.
    posted = [l for l in read("shell-r.log")
              if re.search(r'shell\.notify: notify id=\d+ app="Lumen" \(the shell\'s own\) '
                           r'summary="Lumen configuration not used"', l)]
    rep = [l for l in probe_lines("r2") if l.startswith("repeat ")][:s.get("repeats", 0)]
    notices = s.get("notices", -1)
    ok = bool(rejected) and s.get("rc") == 1 and "expected ']'" in reload and \
        "rejected" in reload and notices == 1 and bool(shown) and bool(posted) and \
        rep[-1:] == ["repeat rate=30 delay=600"]
    record("R5 a syntax error: rejected, the configuration in use stays, lumen-ctl reload exits 1 "
           "with the line, one notice (a notification)", ok,
           f"rejected {len(rejected)}; lumen-ctl rc={s.get('rc')} {reload.strip()[:100]!r}; "
           f"notices {notices} (notifications {len(posted)}); last repeat then {rep[-1:]}")

s = scen.get("R6")
if s is not None:
    mark = s.get("removed_mark", s["comp"][0])
    kept = since(s, r"input\.keyboard\.repeat-rate: 5000 is outside 0\.\.1000; keeping 30")
    sugg = since(s, r'unknown action "window\.closee" \(did you mean window\.close\?\)')
    rep = [l for l in probe_lines("r2") if l.startswith("repeat ")]
    back = any(re.search(r"config: config: generation \d+ from .* \(0 errors", comp[i])
               for i in range(mark, s["comp"][1]))
    notices = [l for l in read("shell-r.log") if "config: notice:" in l]
    ok = bool(kept) and bool(sugg) and rep[-1:] == ["repeat rate=25 delay=600"] and back and \
        len(notices) == 2
    record("R6 a bad value keeps that key (30), an unknown action suggests one; the file removed: "
           "the defaults return (repeat 25)", ok,
           f"kept {len(kept)}, suggestion {len(sugg)}; last repeat {rep[-1:]}; defaults back={back}; "
           f"notices {len(notices)}")

# ── Q: workspaces ─────────────────────────────────────────────────────────────
def configures(name, upto=None):
    """The probe's configure lines (within its first `upto` lines: as of a scenario's end)."""
    p = probe_lines(name)
    return [l for l in (p[:upto] if upto is not None else p) if l.startswith("configure ")]

def last_suspended(name, upto=None):
    c = configures(name, upto)
    return bool(c) and "suspended" in c[-1]

def ws_summary(name):
    """The last `ws output=… workspaces=…` lines of a lumen-probe workspaces log."""
    out = {}
    for l in probe_lines(name):
        m = re.match(r"ws output=(\S+) workspaces=(\S+)", l)
        if m:
            out[m.group(1)] = m.group(2)
    return out

def focus_ids(r):
    return [int(m.group(1)) for i in r
            for m in [re.search(r"compositor\.wm: focus keyboard id=(\d+)$", comp[i])] if m]

s = scen.get("Q1")
if s is not None:
    a, b, c = s.get("a", 0), s.get("b", 0), s.get("c", 0)
    mark, back, end = s.get("mark", s["comp"][0]), s.get("back", s["comp"][0]), s["comp"][1]
    hidden = [l for l in comp[mark:back] if re.search(r"compositor\.workspace: window id=\d+ hidden", l)]
    shown = [l for l in comp[back:end] if re.search(r"compositor\.workspace: window id=\d+ shown", l)]
    # Every configure after the switch carried `suspended`, the ones after the return do not.
    upto = {"q1a": s.get("la"), "q1b": s.get("lb"), "q1c": s.get("lc")}
    sus = [n for n in upto if any("suspended" in l for l in configures(n, upto[n]))]
    on2 = ws_summary("q1-on2")
    f_on2 = focus_ids(range(mark, back))
    f_back = focus_ids(range(back, end))
    ok = a and b and c and len(hidden) >= 3 and len(shown) >= 3 and len(sus) == 3 and \
        not any(last_suspended(n, upto[n]) for n in upto) and \
        any(v.endswith(",2*") or ",2*," in v for v in on2.values()) and \
        f_on2[-1:] == [0] and f_back[-1:] == [a]
    record("Q1 Super+2 hides the three windows (suspended, no keyboard), lumen-probe sees 2 "
           "active; Super+1 shows them, the one focused there gets the keyboard back", bool(ok),
           f"hidden {len(hidden)} shown {len(shown)}; told suspended {sus}; on 2: {on2}; "
           f"focus while on 2 {f_on2[-1:]}, after {f_back[-1:]} (want {a})")

s = scen.get("Q2")
if s is not None:
    a = s.get("a", 0)
    moved = since(s, rf"workspace move window id={a} to=2 .* follow=1 source=binding")
    summ = ws_summary("q2")
    ind = [l for l in read("shell-q.log") if re.search(r"shell\.panel: workspaces \[1\+ 2\*\+ 3\]", l)]
    la, lb = s.get("la"), s.get("lb")
    ok = bool(moved) and not last_suspended("q1a", la) and last_suspended("q1b", lb) and \
        any(v.startswith("1,2*,3") for v in summ.values()) and bool(ind)
    record("Q2 Super+Shift+2 moves the focused window to 2 and follows; the others stay on 1 "
           "(hidden); the indicator shows `1+ 2*+ 3`", ok,
           f"moved {len(moved)}; q1a suspended={last_suspended('q1a', la)} "
           f"q1b={last_suspended('q1b', lb)}; "
           f"{summ}; indicator lines {len(ind)}")

s = scen.get("Q3")
if s is not None:
    act = since(s, r"compositor\.workspace: workspace activate .* name=1 .* source=ext-workspace")
    summ = ws_summary("q3")
    ok = bool(act) and any(v.startswith("1*") for v in summ.values()) and \
        any(l.startswith("request activate") for l in probe_lines("q3"))
    record("Q3 ext-workspace activate + commit from lumen-probe shows workspace 1", ok,
           f"activate lines {len(act)}; {summ}")

s = scen.get("Q4")
if s is not None:
    blue = (74, 122, 176)
    c1, f1 = shot_colour(s.get("s1"))
    c2, f2 = shot_colour(s.get("s2"))
    c3, f3 = shot_colour(s.get("s3"))
    # Away from it: the screen is not the window (blue) nor its backdrop (black); q1a, also
    # blue, may show as a small part of it.
    gone = not (c2 == blue and f2 > 0.5) and not (c2 == (0, 0, 0) and f2 > 0.5)
    ok = c1 == blue and f1 > 0.8 and gone and c3 == blue and f3 > 0.8
    record("Q4 a fullscreen window: shown on its workspace; on another, neither it nor its black "
           "backdrop; back, it again", ok,
           f"screenshots: {c1} {f1:.2f} | {c2} {f2:.2f} | {c3} {f3:.2f}")

s = scen.get("Q5")
if s is not None:
    c = s.get("c", 0)
    mn, ck, end = s.get("min", s["comp"][0]), s.get("click", s["comp"][0]), s["comp"][1]
    minim = [l for l in comp[mn:ck] if re.search(rf"compositor\.workspace: minimize id={c} on=1 source=binding", l)]
    back = [l for l in comp[ck:end] if re.search(rf"compositor\.workspace: window id={c} shown", l)]
    fb = focus_ids(range(ck, end))
    q5 = s.get("q5", 0)
    xdg = since(s, rf"compositor\.workspace: minimize id={q5} on=1 source=client")
    q5_sus = last_suspended("q5")
    ok = bool(minim) and bool(s.get("cell")) and bool(back) and c in fb and bool(xdg) and q5_sus
    record("Q5 Super+H minimizes (hidden, suspended); its panel entry brings it back with the "
           "keyboard; xdg set_minimized minimizes too", ok,
           f"minimized {len(minim)}; panel cell found={s.get('cell')}; shown again {len(back)}; "
           f"focus after the click {fb[-2:]}; xdg minimize {len(xdg)} (suspended={q5_sus})")

s = scen.get("Q6")
if s is not None:
    p = probe_lines("q6")
    summ = [l for l in p if l.startswith("capture summary")]
    m = re.search(r"frames=(\d+) ready=(\d+)", summ[-1]) if summ else None
    hidden = since(s, rf"compositor\.workspace: window id={s.get('id', 0)} hidden")
    ok = bool(m) and int(m.group(2)) >= 8 and bool(hidden)
    record("Q6 a window on a workspace that is not shown is captured: frames keep coming", ok,
           f"{summ[-1:]}; hidden {len(hidden)}")

s = scen.get("Q7")
if s is not None:
    locked = s.get("locked", s["comp"][0])
    acts = [l for l in comp[locked:s["comp"][1]] if "compositor.workspace: workspace activate" in l]
    ignored = since(s, r"ext-workspace: commit while locked; ignored")
    after = ws_summary("q7-after")
    ok = not acts and bool(ignored) and any(v.startswith("1*") for v in after.values())
    record("Q7 locked: Super+2 and an ext-workspace activate change nothing", ok,
           f"activations while locked {len(acts)}; ignored commits {len(ignored)}; after {after}")

# ── O: switching and snapping (design-session-and-wm Part K) ─────────────────
def shell_o(s):
    """lumen-shell's lines during scenario s."""
    return read("shell-o.log")[s.get("sh0", 0):s.get("sh1", 0)]

def layer_maps(s, ns):
    return [m for i in rng(s)
            for m in [re.search(r"layer map surface=\d+ layer=\w+ namespace=" + ns +
                                r" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", comp[i])] if m]

def usable_before(i):
    u = None
    for l in comp[:i]:
        m = re.search(r"compositor\.wm: usable area x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) output=0", l)
        if m:
            u = tuple(int(m.group(k)) for k in range(1, 5))
    return u

def pixel(img, x, y):
    if not img:
        return None
    w, h, px = img
    if not (0 <= x < w and 0 <= y < h):
        return None
    o = (y * w + x) * 3
    return tuple(px[o:o + 3])

TILED = "tiled_left,tiled_right,tiled_top,tiled_bottom"

s = scen.get("O1")
if s is not None:
    b = s.get("b", 0)
    f = focus_ids(rng(s))
    sw = shell_o(s)
    act = [l for l in sw if re.search(r'switcher: activate \(released, never shown\) "ob"', l)]
    begun = since(s, r"held action switcher\.next begins")
    ok = bool(begun) and f[-1:] == [b] and bool(act) and not layer_maps(s, "lumen-switcher")
    record("O1 a quick Alt+Tab: the previous window gets the keyboard, the switcher is never "
           "drawn (released within the open delay)", ok,
           f"hold began {len(begun)}; focus {f[-1:]} (want {b}); shell {act[-1:] or sw[-3:]}; "
           f"switcher maps {len(layer_maps(s, 'lumen-switcher'))}")

s = scen.get("O2")
if s is not None:
    a = s.get("a", 0)
    f = focus_ids(rng(s))
    sw = shell_o(s)
    opened = [l for l in sw if re.search(r"switcher: open \(3 windows", l)]
    act = [l for l in sw if re.search(r'switcher: activate \(released, shown\) "oa"', l)]
    maps = layer_maps(s, "lumen-switcher")
    lw, lh = out_w / scale, out_h / scale
    centred = bool(maps) and all(abs(int(m.group(1)) - (lw - int(m.group(3))) / 2) <= 1 and
                                 abs(int(m.group(2)) - (lh - int(m.group(4))) / 2) <= 1 for m in maps)
    keys = since(s, r"compositor\.shell: |shell\.bridge: hold key")
    # The screenshot while shown: the switcher's background and the selection's blue inside
    # its rect (at the output's scale).
    img = ppm(s.get("shot", "")) if s.get("shot") else None
    px_ok, detail_px = False, "no screenshot"
    if img and maps:
        x, y, w, h = (int(maps[-1].group(k)) for k in range(1, 5))
        bg = sel = 0
        for v in range(int(y * scale) + 2, int((y + h) * scale) - 2, 3):
            for u in range(int(x * scale) + 2, int((x + w) * scale) - 2, 3):
                c = pixel(img, u, v)
                if c and max(abs(c[0] - 30), abs(c[1] - 30), abs(c[2] - 34)) <= 2:
                    bg += 1
                elif c and max(abs(c[0] - 61), abs(c[1] - 111), abs(c[2] - 180)) <= 2:
                    sel += 1
        px_ok = bg > 50 and sel > 50
        detail_px = f"background px {bg}, selection px {sel}"
    ok = bool(opened) and bool(act) and f[-1:] == [a] and centred and px_ok
    record("O2 Alt held, Tab, Tab: the switcher is drawn after the open delay (centred, at the "
           "output's scale), the third window of the MRU order gets the keyboard on release", ok,
           f"opened {opened[-1:]}; maps {[m.group(0) for m in maps]}; centred={centred}; "
           f"{detail_px}; focus {f[-1:]} (want {a}); {act[-1:] or sw[-4:]}")

s = scen.get("O3")
if s is not None:
    sw = shell_o(s)
    f = focus_ids(rng(s))
    pa = probe_lines("oa")[s.get("la0", 0):s.get("la1", 0)]
    leaked = [l for l in pa if re.match(r"key (15|1) ", l) or l.startswith("keyboard leave")]
    alt = [l for l in pa if l.startswith("key 56 ")]
    cancel = [l for l in sw if re.search(r"switcher: nothing \(escape", l)]
    ok = bool(cancel) and not f and not leaked and len(alt) == 2
    record("O3 Alt+Tab, Escape: nothing changes — no focus change, the application saw Alt "
           "only (no leave, no Tab, no Escape)", ok,
           f"{cancel[-1:] or sw[-3:]}; focus changes {f}; application lines {pa}")

s = scen.get("O4")
if s is not None:
    c = s.get("c", 0)
    f = focus_ids(rng(s))
    sw = shell_o(s)
    act = [l for l in sw if re.search(r'switcher: activate \(released, never shown\) "oc"', l)]
    begun = since(s, r"held action switcher\.previous begins")
    ok = bool(begun) and bool(act) and f[-1:] == [c]
    record("O4 Alt+Shift+Tab: backwards, the least recently used window", ok,
           f"hold {len(begun)}; {act[-1:] or sw[-3:]}; focus {f[-1:]} (want {c})")

s = scen.get("O5")
if s is not None:
    c = s.get("c", 0)
    u = usable_before(s["comp"][0])
    cfgs = [l for l in probe_lines("oc")[s.get("lc0", 0):s.get("lc1", 0)] if l.startswith("configure ")]
    seq = []
    for l in cfgs:
        m = re.match(r"configure w=(\d+) h=(\d+) states=(\S+)", l)
        if m:
            w, h, st = int(m.group(1)), int(m.group(2)), m.group(3)
            tiled = TILED in st
            mx = "maximized" in st
            if not seq or seq[-1] != (w, h, tiled, mx):
                seq.append((w, h, tiled, mx))
    want = []
    if u:
        uw, uh = u[2], u[3]
        hw, hh = (uw + 1) // 2, (uh + 1) // 2
        w0, h0 = s.get("w0", 0), s.get("h0", 0)
        want = [(hw, uh, True, False), (hw, hh, True, False), (hw, uh, True, False),
                (w0, h0, False, False), (uw, uh, False, True), (w0, h0, False, False)]
    g = geometry_at(c, s["comp"][1])
    snaps = since(s, rf"compositor\.snap: snap id={c} to=(left|top-left|none) output=0 source=binding")
    back = g is not None and (g[0], g[1]) == (s.get("x0"), s.get("y0"))
    ok = bool(u) and seq == want and back and len(snaps) >= 4
    record("O5 Super+Left, Up, Down, Right, Up, Down: left half, top-left quarter, left half "
           "(tiled on every edge), restored; maximized, restored to the same place", ok,
           f"usable {u}; configures {seq}; want {want}; at the end {g} (was {s.get('x0')},{s.get('y0')}); "
           f"snap lines {len(snaps)}")

s = scen.get("O6")
if s is not None:
    b, mid = s.get("b", 0), s.get("mid", s["comp"][0])
    u = usable_before(s["comp"][0])
    prev = [l for l in comp[s["comp"][0]:mid] if re.search(r"compositor\.snap: snap preview zone=left output=0", l)]
    hidden = [l for l in comp[s["comp"][0]:mid] if "compositor.snap: snap preview hidden" in l]
    snapped = [l for l in comp[s["comp"][0]:mid] if re.search(rf"compositor\.wm: edge snap id={b} zone=4 output=0", l)]
    restored = [l for l in comp[mid:s["comp"][1]] if re.search(rf"compositor\.snap: drag restore id={b} from=left .* w=400 h=300", l)]
    pb = probe_lines("oc")
    first = [l for l in pb[s.get("lb0", 0):s.get("lbm", 0)] if l.startswith("configure ")]
    second = [l for l in pb[s.get("lbm", 0):s.get("lb1", 0)] if l.startswith("configure ")]
    half_ok = bool(u) and any(re.match(rf"configure w={(u[2] + 1) // 2} h={u[3]} states=\S*{TILED}", l) for l in first)
    back_ok = bool(second) and re.match(r"configure w=400 h=300 states=", second[-1]) is not None and \
        TILED not in second[-1]
    # The preview in the screenshot taken while it showed: at the left edge, above the
    # window, the border (80 % accent) and the fill (25 %) over what the earlier shot had.
    pre, shot = ppm(s.get("pre", "")) if s.get("pre") else None, ppm(s.get("shot", "")) if s.get("shot") else None
    px_ok, px_detail = False, "no screenshots"
    if pre and shot and u:
        y = int(round((s.get("py", 0) - 200) * scale))
        border, fill = (0x31, 0x59, 0x90, 0.8), (0x0F, 0x1C, 0x2D, 0.25)
        def blend(c, under):
            return tuple(c[i] + under[i] * (1 - c[3]) for i in range(3))
        under0, under2 = pixel(pre, 0, y), pixel(pre, 2, y)
        got0, got2 = pixel(shot, 0, y), pixel(shot, 2, y)
        if under0 and under2 and got0 and got2:
            e0, e2 = blend(border, under0), blend(fill, under2)
            px_ok = max(abs(got0[i] - e0[i]) for i in range(3)) <= 4 and \
                max(abs(got2[i] - e2[i]) for i in range(3)) <= 4
            px_detail = (f"y={y}: border px {got0} want {tuple(round(v) for v in e0)} over {under0}; "
                         f"fill px {got2} want {tuple(round(v) for v in e2)} over {under2}")
    ok = bool(prev) and bool(hidden) and bool(snapped) and half_ok and bool(restored) and back_ok and px_ok
    record("O6 Super+drag to the left edge: the preview shows the left half (border and fill "
           "as composited), the release snaps the window there (tiled); dragged out it is "
           "restored to 400x300 under the pointer", ok,
           f"preview {prev[-1:]}; hidden {len(hidden)}; snapped {len(snapped)}; half {half_ok} "
           f"{first[-1:]}; restored {restored[-1:]} {second[-1:]}; {px_detail}")

s = scen.get("O7")
if s is not None:
    b = s.get("b", 0)
    g = geometry_at(b, s["comp"][1])
    began = since(s, rf"compositor\.snap: keyboard move id={b}")
    ended = since(s, rf"compositor\.wm: grab end id={b} mode=move reason=release")
    pb = probe_lines("oc")[s.get("lb0", 0):s.get("lb1", 0)]
    leaked = [l for l in pb if re.match(r"key (103|105|106|108|28) pressed", l)]
    moved = g is not None and (g[0] - s.get("x0", 0), g[1] - s.get("y0", 0)) == (30, 10)
    ok = bool(began) and bool(ended) and moved and not leaked
    record("O7 Alt+F7, Right ×3, Down, Return: the window moved by (30, 10); the keys were the "
           "window manager's", ok,
           f"began {len(began)} ended {len(ended)}; geometry {g} from ({s.get('x0')},{s.get('y0')}); "
           f"keys seen by the window {leaked}")

s = scen.get("O8")
if s is not None:
    b = s.get("b", 0)
    g = geometry_at(b, s["comp"][1])
    began = since(s, rf"compositor\.snap: keyboard resize id={b}")
    want = (s.get("x0", 0), s.get("y0", 0), s.get("w0", 0) + 20, s.get("h0", 0) + 10)
    ok = bool(began) and g == want
    record("O8 Alt+F8, Right ×2, Down, Return: the right edge picked (+20), then the bottom "
           "(+10); the origin stays", ok, f"began {len(began)}; geometry {g}, want {want}")

s = scen.get("O9")
if s is not None:
    sw = shell_o(s)
    cancelled = since(s, r"held action cancelled \(session locked\)")
    closed = [l for l in sw if "switcher: nothing (cancelled by the compositor" in l]
    f = focus_ids(rng(s))
    # Locked, nothing has the keyboard (focus 0); unlocked, the window the user left.
    ok = bool(cancelled) and bool(closed) and f[-1:] in ([s.get("c")], [])
    record("O9 the session locks during a hold: the hold is cancelled, the switcher closes and "
           "activates nothing", ok,
           f"cancelled {len(cancelled)}; {closed[-1:] or sw[-3:]}; focus {f}")

s = scen.get("O11")
if s is not None:
    sw = shell_o(s)
    cancelled = since(s, r"held action cancelled \(click elsewhere\)")
    closed = [l for l in sw if "switcher: nothing (cancelled by the compositor" in l]
    f = focus_ids(rng(s))
    ok = bool(cancelled) and bool(closed) and f[-1:] == [s.get("a")]
    record("O11 a click on a window during a hold cancels it; that window keeps the keyboard",
           ok, f"cancelled {len(cancelled)}; {closed[-1:] or sw[-3:]}; focus {f} (want {s.get('a')})")

s = scen.get("O10")
if s is not None:
    quick = since(s, r"compositor\.wm: quick switch id=(\d+) \(no shell\)")
    f = focus_ids(rng(s))
    # oa had the keyboard (O11's click): another window gets it.
    ok = bool(quick) and bool(f) and f[-1] != s.get("before")
    record("O10 with no shell Alt+Tab is the compositor's quick switch to the topmost other "
           "window", ok, f"{quick[-1:]}; focus {f}")

s = scen.get("V1")
if s is not None:
    lines_v1 = probe_lines("v1")
    a, b = s["comp"]
    vt_mark, hatch_mark, tap_mark = s.get("vt_mark", a), s.get("hatch_mark", a), s.get("tap_mark", a)
    pre = range(a, vt_mark)
    launcher_while = [comp[i] for i in pre if "namespace=lumen-launcher" in comp[i] and "layer map" in comp[i]]
    grab_while = [comp[i] for i in pre if "compositor.shell: grab activated" in comp[i]]
    keys = [l for l in lines_v1 if l.startswith("key code=")]
    meta_seen = "key code=125" in keys
    enter_seen = "key code=28" in keys
    vt_req = any("VT switch to 2 requested" in comp[i] for i in range(vt_mark, hatch_mark))
    paused = any("session paused" in comp[i] for i in range(vt_mark, hatch_mark))
    resumed = any("session resumed" in comp[i] for i in range(vt_mark, hatch_mark))
    inactive = "inhibit inactive" in lines_v1
    launcher_after = any("namespace=lumen-launcher" in comp[i] and "layer map" in comp[i]
                         for i in range(tap_mark, b))
    turned_off = any("turned off by the user (Super+Escape)" in comp[i] for i in range(hatch_mark, tap_mark))
    ok = "inhibit active" in lines_v1 and meta_seen and enter_seen and not launcher_while and \
        not grab_while and vt_req and paused and resumed and inactive and turned_off and launcher_after
    record("V1 keyboard-shortcuts-inhibit: Super and Super+Return reach the probe (no shell grab "
           "or launcher); Ctrl+Alt+F2 still switches VT; Super+Escape turns it off and the Super "
           "tap opens the launcher again", ok,
           f"probe keys {keys[:6]}; launcher while inhibited {len(launcher_while)}, grabs {len(grab_while)}; "
           f"VT requested={vt_req} paused={paused} resumed={resumed}; inactive={inactive} "
           f"turned off={turned_off}; launcher after={launcher_after}")

s = scen.get("V2")
if s is not None:
    if s.get("skipped"):
        record("V2 data control (wl-clipboard)", True, "wl-clipboard not installed", warn=True)
    else:
        def text(name):
            try:
                with open(os.path.join(log_dir, name), encoding="utf-8", errors="replace") as f:
                    return f.read()
            except FileNotFoundError:
                return None
        paste, primary, out, hist = text("V2.paste"), text("V2.primary"), text("V2.out"), text("V2.cliphist")
        dbg = wl_debug("wlcopy-v2")
        ext = any(i == "ext_data_control_manager_v1" for _req, i, _, _m, _a in dbg) or \
            any("ext_data_control_manager_v1" in l for l in read("wlcopy-v2.log"))
        hist_ok = (not s.get("cliphist")) or (hist is not None and "lumen-v2-clipboard" in hist and
                                               "lumen-v2-second" in hist)
        ok = paste == "lumen-v2-clipboard" and primary == "lumen-v2-primary" and \
            out is not None and "lumen-v2-clipboard" in out and ext and hist_ok and s.get("first_exited") == 1
        record("V2 ext-data-control with real tools: wl-copy/wl-paste both selections, cliphist "
               "stores through wl-paste --watch, foot pastes a wl-copy selection", ok,
               f"paste={paste!r} primary={primary!r} foot file={out!r}; wl-copy bound ext data "
               f"control={ext}; cliphist={hist.strip().splitlines() if hist else hist}; first wl-copy "
               f"exited when replaced={s.get('first_exited')}")

s = scen.get("V3")
if s is not None:
    lines_v3 = probe_lines("v3")
    summary = [l for l in lines_v3 if l.startswith("security-context hidden=")]
    closed = [l for l in lines_v3 if l.startswith("security-context closed=")]
    marked = comp_in(s, r"connected sandboxed \(security context engine=org\.lumen\.probe app_id=org\.lumen\.ProbeV3\)")
    ok = bool(summary) and summary[0] == "security-context hidden=OK visible=OK nested=OK" and \
        bool(closed) and closed[0] == "security-context closed=OK" and bool(marked)
    record("V3 security-context: a client on the engine's socket is sandboxed, sees no "
           "session-wide global; the socket stops when the engine hangs up", ok,
           f"{summary[0] if summary else 'no summary'}; {closed[0] if closed else 'no close line'}; "
           f"compositor marked it sandboxed={bool(marked)}")

# ── N: notifications and the tray (design-session-and-wm Part N) ─────────────
def shell_n(s, a="sh0", b="sh1", f="shell-n.log"):
    """lumen-shell's lines (group N) between two marks of scenario s."""
    return read(f)[s.get(a, 0):s.get(b, 0)]

def monitor(s):
    """org.freedesktop.Notifications signals dbus-monitor saw during s: [member, args...]."""
    sigs, cur = [], None
    for l in read("n-monitor.log")[s.get("mon0", 0):s.get("mon1", 0)]:
        m = re.search(r"interface=org\.freedesktop\.Notifications; member=(\w+)", l)
        if l.startswith("signal ") and m:
            cur = [m.group(1)]
            sigs.append(cur)
            continue
        if l.startswith("signal ") or l.startswith("method "):
            cur = None
            continue
        m = re.match(r"\s+(?:uint32|string) (.*)$", l)
        if m and cur is not None:
            cur.append(m.group(1).strip('"'))
    return sigs

def has(lines, rx):
    return any(re.search(rx, l) for l in lines)

s = scen.get("N1")
if s is not None:
    i, c = s.get("id", 0), s.get("cid", 0)
    sh = shell_n(s)
    sig = monitor(s)
    overlay = [l for l in sh if re.search(r"notifications: (overlay layer \(a critical one shown\)|"
                                          r"shown \(\d+ cards?, overlay layer\))", l)]
    ok = (i > 0 and c > 0 and has(sh, rf'notify id={i} app="smoke" urgency=normal .* popup') and
          has(sh, rf'notify id={c} app="smoke" urgency=critical .* popup') and
          ["NotificationClosed", str(i), "1"] in sig and ["NotificationClosed", str(c), "3"] in sig and
          not any(x[:2] == ["NotificationClosed", str(c)] and x[2:] != ["3"] for x in sig) and
          bool(overlay) and s.get("rc") == 0 and s.get("rc2", 0) != 0)
    record("N1 notify-send normal (-t 1500) expires with reason 1; critical stays, on the overlay "
           "layer, CloseNotification closes it (3); an unknown id is an error", ok,
           f"ids {i}/{c}; closed signals {[x for x in sig if x[0] == 'NotificationClosed']}; "
           f"overlay maps {len(overlay)}; CloseNotification rc {s.get('rc')} unknown rc {s.get('rc2')}")

s = scen.get("N2")
if s is not None:
    i, j = s.get("id", 0), s.get("id2", 0)
    sh = shell_n(s)
    two = [l for l in sh if "notifications: draw cards=2" in l]
    ok = i > 0 and i == j and has(sh, rf'notify id={i} \(replaced\) app="smoke".*summary="n2 second"') and not two
    record("N2 replaces_id: the same id back, replaced in place (never two cards)", ok,
           f"id {i} → {j}; replaced lines {len([l for l in sh if '(replaced)' in l])}; two-card draws {len(two)}")

s = scen.get("N3")
if s is not None:
    i, b = s.get("id", 0), s.get("bid", 0)
    sig = [x for x in monitor(s) if x[0] in ("ActivationToken", "ActionInvoked", "NotificationClosed")]
    seq = [x for x in sig if len(x) > 1 and x[1] == str(i)]
    printed = read("n3-send.log")
    ok = (len(seq) == 3 and seq[0][0] == "ActivationToken" and len(seq[0]) == 3 and len(seq[0][2]) >= 16 and
          seq[1] == ["ActionInvoked", str(i), "ok"] and seq[2] == ["NotificationClosed", str(i), "2"] and
          "ok" in printed and ["NotificationClosed", str(b), "2"] in sig)
    record("N3 an action button: ActivationToken, ActionInvoked(ok), NotificationClosed(2) in that "
           "order, notify-send prints ok; a click on a card without a default action dismisses (2)",
           ok, f"id {i}: {seq}; notify-send printed {printed}; body click id {b}: "
               f"{[x for x in sig if len(x) > 1 and x[1] == str(b)]}")

s = scen.get("N4")
if s is not None:
    i = s.get("id", 0)
    locked = shell_n(s, "sh0", "shm")
    after = shell_n(s, "shm", "sh1")
    ok = (has(locked, rf'notify id={i} .*queued \(locked\)') and
          not has(locked, r"notifications: shown") and has(after, r"notifications: shown") and
          has(after, rf"notify id={i} closed reason=1"))
    record("N4 while locked a notification waits (not shown, its timer stopped) and pops up after "
           "the unlock, then expires", ok,
           f"while locked: {[l for l in locked if 'notif' in l][-2:]}; after: "
           f"{[l for l in after if 'notif' in l][:3]}")

s = scen.get("N5")
if s is not None:
    i, c, b = s.get("id", 0), s.get("cid", 0), s.get("bid", 0)
    sh = shell_n(s)
    ok = (has(sh, r"do not disturb on \(screen captured\)") and
          has(sh, rf'notify id={i} .*history \(do not disturb\)') and
          has(sh, rf'notify id={c} .*urgency=critical.* popup') and has(sh, r"do not disturb off") and
          has(sh, r"do not disturb set \(a middle click on the indicator\)") and
          has(sh, rf'notify id={b} .*history \(do not disturb\)') and has(sh, r"do not disturb cleared"))
    record("N5 do not disturb while the screen is captured (normal → history, critical shown), off "
           "after; a middle click on the bell sets and clears it", ok,
           f"{[l.split('] ', 1)[-1] for l in sh if 'disturb' in l]}")

s = scen.get("N6")
if s is not None:
    sn = read("probe-n6-sni.log")
    sh = shell_n(s)
    tok = next((l.split()[2] for l in sn if l.startswith("sni token ")), "")
    act = [l for l in sn if l.startswith("sni activate ")]
    bus = read("N6-busctl.out")
    r6 = rng(s)
    grabs = [l for l in comp[r6.start:r6.stop] if POPGRAB.search(l) and "result=ok" in l]
    maps = [l for l in comp[r6.start:r6.stop] if POPMAP.search(l)]
    # The item's orange disc in its panel cell, in the screenshot.
    img = ppm(s.get("shot", "")) if s.get("shot") else None
    orange = 0
    if img:
        tx, tw = s.get("tx", 0), s.get("tw", 0)
        for y in range(4, 28):
            for x in range(tx, tx + tw):
                c = pixel(img, x, y)
                if c and abs(c[0] - 0xF0) <= 3 and abs(c[1] - 0x90) <= 3 and abs(c[2] - 0x20) <= 3:
                    orange += 1
    ok = (bool(tok) and bool(act) and act[-1].endswith(f"token={tok}") and
          has(sh, r"tray: .* Activate x=\d+ y=\d+ token=yes") and
          any("StatusNotifierItem" in l for l in bus) and any(l.strip() == "b true" for l in bus) and
          bool(grabs) and bool(maps) and "sni menu event id=0 type=opened" in sn and
          "sni menu clicked id=1 label=_Open" in sn and "sni menu event id=0 type=closed" in sn and
          "sni scroll delta=120 orientation=vertical" in sn and has(sh, r'tray: tooltip "Smoke item"') and
          orange >= 100)
    record("N6 the tray: lumen-probe sni in the panel (its pixmap drawn), tooltip, left click → "
           "ProvideXdgActivationToken then Activate with it, busctl lists it, right click → its "
           "dbusmenu as a grabbing popup, Down+Return → Event(1, clicked), wheel → Scroll", ok,
           f"token {tok[:8] or '-'}…; activate {act[-1:]}; busctl {bus}; popup grabs {len(grabs)} "
           f"maps {len(maps)}; menu {[l for l in sn if 'menu' in l][-4:]}; scroll "
           f"{[l for l in sn if 'scroll' in l]}; orange px {orange}")

s = scen.get("N7")
if s is not None:
    sn = read("probe-n6-sni.log")[s.get("sn0", 0):s.get("sn1", 0)]
    i = s.get("id", 0)
    ok = (s.get("ms", 9999) <= 1000 and any(l.startswith("sni registered watcher=") for l in sn) and
          i >= 1 + 64 and has(read("shell-n2.log"), r"tray: item org\.kde\.StatusNotifierItem-\d+-1/StatusNotifierItem id=\"lumen-probe\""))
    record("N7 the shell killed and started again: the item registers with the new watcher within "
           "1 s; ids continue after the persisted block", ok,
           f"re-registered after {s.get('ms')} ms ({[l for l in sn if 'watcher' in l]}); new id {i}")

s = scen.get("N8")
if s is not None:
    sw, cw = s.get("shell", -1), s.get("comp", -1)
    record("N8 idle with a history and a tray item: lumen-shell sleeps (context switches in 10 s)",
           0 <= sw <= 2, f"shell {sw}, compositor main thread {cw} in 10 s", warn=0 <= sw <= 2 and cw > 40)

# ── W: pointer constraints, relative pointer, gestures (design-app-compat-capture Part C) ──
REL_LINE = re.compile(r"^relative (-?[\d.]+),(-?[\d.]+) unaccel (-?[\d.]+),(-?[\d.]+) t=\d+$")
POS_LINE = re.compile(r"^(enter|motion) (-?[\d.]+),(-?[\d.]+)$")

def seg(lines, a, b=None):
    return lines[a:b] if b is not None else lines[a:]

def positions(lines):
    out = []
    for l in lines:
        m = POS_LINE.match(l)
        if m:
            out.append((m.group(1), float(m.group(2)), float(m.group(3))))
    return out

s = scen.get("W1")
if s is not None:
    L = probe_lines("w1")
    ml, mf, mv, mk, me = (s.get(k, 0) for k in ("m_locked", "m_focus", "m_vt", "m_lock", "m_esc"))
    first_locked = next((i for i, l in enumerate(L) if l == "locked"), -1)
    enters = [p for p in positions(L[:first_locked + 1]) if p[0] == "enter"] if first_locked >= 0 else []
    lock_pos = (enters[-1][1], enters[-1][2]) if enters else None
    held = seg(L, ml, mf)
    rels = [REL_LINE.match(l) for l in held]
    rels = [tuple(float(m.group(k)) for k in (1, 2, 3, 4)) for m in rels if m]
    unaccel_ok = sum(1 for r in rels if abs(r[2] - 10) < 0.01 and abs(r[3] + 5) < 0.01)
    abs_rel = [r for r in rels if abs(r[2] - 150) <= 2 and abs(r[3] - 40) <= 2 and r[0] == r[2]]
    moved_while_locked = [l for l in held if l.startswith("motion ")]
    def phase(a, b):
        p = seg(L, a, b)
        return ("unlocked" in p, "locked" in p and p.index("locked") > (p.index("unlocked") if "unlocked" in p else -1))
    focus_off, focus_back = phase(mf, mv)
    vt_off, vt_back = phase(mv, mk)
    lk_off, lk_back = phase(mk, me)
    # Every time the pointer re-entered while it should have been held, and the first motion
    # after the release: at the lock position (the pointer never moved).
    reentries = [p for p in positions(seg(L, ml, me)) if p[0] == "enter"]
    after = [p for p in positions(seg(L, me)) if p[0] == "motion"]
    still = bool(lock_pos) and all(abs(p[1] - lock_pos[0]) < 1 and abs(p[2] - lock_pos[1]) < 1 for p in reentries) \
        and bool(after) and abs(after[0][1] - lock_pos[0]) < 6 and abs(after[0][2] - lock_pos[1]) < 1
    released = "unlocked" in seg(L, me) and any("released by the user (Super+Escape)" in comp[i] for i in rng(s))
    reasons = [re.search(r"inactive \(([^)]*)\)", comp[i]).group(1) for i in rng(s)
               if "compositor.constraint: pointer lock" in comp[i] and "inactive (" in comp[i]]
    ok = first_locked >= 0 and unaccel_ok >= 20 and bool(abs_rel) and not moved_while_locked and \
        focus_off and focus_back and vt_off and vt_back and lk_off and lk_back and still and released
    record("W1 pointer lock (probe): relative motion flows with unaccelerated deltas, the pointer "
           "stays; focus loss, VT switch and session lock end it and it holds again; Super+Escape "
           "releases", ok,
           f"locked at {lock_pos}; relative while held {len(rels)} (unaccel 10,-5: {unaccel_ok}; "
           f"absolute device {abs_rel[0][:2] if abs_rel else 'none'}); motions while held "
           f"{len(moved_while_locked)}; off/back focus={focus_off}/{focus_back} vt={vt_off}/{vt_back} "
           f"lock={lk_off}/{lk_back}; re-entries {len(reentries)} first motion after release "
           f"{after[0][1:] if after else None}; released={released}; reasons {reasons}")

s = scen.get("W2")
if s is not None:
    L = probe_lines("w2")
    ex, ey = 240 - 1 / 256, 160 - 1 / 256
    far = [p for p in positions(seg(L, s.get("m_in", 0), s.get("m_far", 0))) if p[0] == "motion"]
    rel = [p for p in positions(seg(L, s.get("m_far", 0), s.get("m_rel", 0))) if p[0] == "motion"]
    corner = [p for p in positions(seg(L, s.get("m_rel", 0), s.get("m_esc", 0))) if p[0] == "motion"]
    tail_ = seg(L, s.get("m_esc", 0))
    clamp_far = bool(far) and abs(far[-1][1] - ex) < 0.01 and abs(far[-1][2] - ey) < 0.01
    clamp_rel = bool(rel) and all(abs(p[1] - ex) < 0.01 and abs(p[2] - ey) < 0.01 for p in rel)
    clamp_corner = bool(corner) and abs(corner[-1][1] - 40) < 0.01 and abs(corner[-1][2] - 40) < 0.01
    ok = "confined" in L and clamp_far and clamp_rel and clamp_corner and "unconfined" in tail_ and "leave" in tail_
    record("W2 confinement (probe, region 40,40 200x120): moves out of the region stop at its "
           "edges; Super+Escape unconfines", ok,
           f"far move -> {far[-1][1:] if far else None}; relative -> {[p[1:] for p in rel[-1:]]}; "
           f"screen corner -> {corner[-1][1:] if corner else None}; after Super+Escape {tail_[:3]}")

s = scen.get("W3")
if s is not None:
    L = probe_lines("w3")
    tail_ = seg(L, s.get("m_esc", 0))
    warp = [p for p in positions(tail_) if p[0] == "motion"]
    warped = bool(warp) and abs(warp[0][1] - 60) < 0.01 and abs(warp[0][2] - 50) < 0.01
    ok = L.count("locked") == 1 and "unlocked" in tail_ and warped and \
        any(l.startswith("enter ") for l in tail_)
    record("W3 oneshot lock with a cursor hint: Super+Escape warps the pointer to the hint; the "
           "spent lock never holds again", ok,
           f"locked {L.count('locked')}x; first motion after release {warp[0][1:] if warp else None}; "
           f"re-entered={any(l.startswith('enter ') for l in tail_)}")

s = scen.get("W4")
if s is not None:
    if s.get("skipped"):
        record("W4 weston-constraints", True, "weston-constraints not installed", warn=True)
    else:
        dbg = wl_debug("weston-w4")
        confined = [k for k, e in enumerate(dbg) if not e[0] and e[1] == "zwp_confined_pointer_v1" and e[3] == "confined"]
        locked = [k for k, e in enumerate(dbg) if not e[0] and e[1] == "zwp_locked_pointer_v1" and e[3] == "locked"]
        presses = [k for k, e in enumerate(dbg) if not e[0] and e[1] == "wl_pointer" and e[3] == "button"
                   and e[4].replace(" ", "").endswith(",1")]
        w, h = s.get("w", 0), s.get("h", 0)
        clamped = False
        if confined and locked:
            ms = [dbg[k][4] for k in range(confined[0], locked[0]) if not dbg[k][0] and dbg[k][1] == "wl_pointer" and dbg[k][3] == "motion"]
            if ms:
                _t, x, y = (float(v) for v in ms[-1].split(","))
                clamped = x < w and y < h and x > w - 1 and y > h - 1
        held_rel, held_motion = 0, 0
        if locked:
            end = next((k for k in presses if k > locked[0]), len(dbg))
            for k in range(locked[0], end):
                e = dbg[k]
                held_rel += (not e[0] and e[1] == "zwp_relative_pointer_v1" and e[3] == "relative_motion")
                held_motion += (not e[0] and e[1] == "wl_pointer" and e[3] == "motion")
        unlocked = any(e[0] and e[1] == "zwp_locked_pointer_v1" and e[3] == "destroy" for e in dbg)
        ok = bool(confined) and bool(locked) and clamped and held_rel >= 5 and held_motion == 0 and unlocked
        record("W4 weston-constraints: click confines (motion clamped inside the surface), click "
               "locks (relative_motion, no wl_pointer.motion), click unlocks", ok,
               f"confined={bool(confined)} clamped={clamped} locked={bool(locked)} relative while "
               f"locked {held_rel} motion {held_motion}; unlocked={unlocked}")

s = scen.get("W5")
if s is not None:
    L = probe_lines("w5")
    devices = [comp[i].split("compositor.input: ", 1)[1] for i in range(len(comp))
               if "compositor.input: input device " in comp[i]]
    gesture_devices = [d for d in devices if d.endswith(" gesture") or " gesture " in d]
    ok = "gestures bound version=3" in L
    record("W5 pointer gestures: bound at v3" + ("" if gesture_devices else
           " (no device makes gestures here: bind only)"), ok,
           f"probe {L[-1:] if L else 'no output'}; devices {sorted(set(devices))}")

# ── Y: screen capture (design-app-compat-capture Part D) ──
CAPREADY = re.compile(r"^frame (\d+) ready damage=(full|partial) rects=(\S*) area=(\d+) transform=(\d+) "
                      r"time=([\d.]+) latency_ms=([\d.]+)")

def capture_frames(name):
    return [m for l in read(name) if (m := CAPREADY.match(l))]

def expects(name):
    return [l for l in read(name) if l.startswith("expect ")]

s = scen.get("Y1")
if s is not None:
    grim_img = ppm(os.path.join(log_dir, "Y1.ppm"))
    shot = ppm(s.get("shot", "")) if s.get("shot") else None
    g = geometry_at(s["id"], s["comp"][1]) if s.get("id") else None
    same, red_ok, detail = False, False, "no image"
    if grim_img and shot:
        same = grim_img == shot
        w, h, px = grim_img
        diff = sum(1 for k in range(0, min(len(px), len(shot[2])), 3)
                   if px[k:k + 3] != shot[2][k:k + 3]) if not same else 0
        if g:
            x, y, gw, gh = g
            c = tuple(px[((y + gh // 2) * w + x + gw // 2) * 3:((y + gh // 2) * w + x + gw // 2) * 3 + 3])
            red_ok = c == (255, 0, 0)
        detail = (f"grim {w}x{h}, compositor read-back {shot[0]}x{shot[1]}: "
                  f"{'identical' if same else f'{diff} pixels differ'}; window centre red={red_ok}")
    ok = same and red_ok and grim_img is not None and grim_img[0] == out_w and grim_img[1] == out_h
    record("Y1 grim -o: the output's size, pixel for pixel the compositor's own read-back", ok, detail)

s = scen.get("Y2")
if s is not None:
    img = ppm(os.path.join(log_dir, "Y2.ppm"))
    detail, ok = "no image: " + " | ".join(read("grim-y2.log")[-2:]), False
    if img:
        w, h, px = img
        colours = {tuple(px[k:k + 3]) for k in range(0, len(px), 3 * 7)}
        ok = (w, h) == (320, 240) and colours == {(255, 0, 0)}
        detail = f"{w}x{h}, colours {sorted(colours)[:4]} (covered by a green window on screen)"
    record("Y2 grim -T: a covered window is captured with its own pixels at its own size", ok, detail)

s = scen.get("Y3")
if s is not None:
    a = ppm(os.path.join(log_dir, "Y3c.ppm"))
    b = ppm(os.path.join(log_dir, "Y3n.ppm"))
    ok, detail = False, "no images"
    if a and b and a[0] == b[0]:
        w = a[0]
        diff = [k // 3 for k in range(0, min(len(a[2]), len(b[2])), 3) if a[2][k:k + 3] != b[2][k:k + 3]]
        xs = [p % w for p in diff]
        ys = [p // w for p in diff]
        box = (min(xs), min(ys), max(xs), max(ys)) if diff else None
        px, py = s["x"], s["y"]
        ok = bool(diff) and box[0] >= px - 64 and box[1] >= py - 64 and box[2] <= px + 64 and \
            box[3] <= py + 64
        detail = f"{len(diff)} pixels differ, within {box} (pointer at {px},{py})"
    record("Y3 grim -c paints the pointer, grim without it does not", ok, detail)

s = scen.get("Y4")
if s is not None:
    for kind in ("dmabuf", "shm"):
        f = capture_frames(f"probe-y4-{kind}.log")
        e = expects(f"probe-y4-{kind}.log")
        later = f[1:]
        small = [m for m in later if m.group(2) == "partial" and int(m.group(4)) <= out_w * out_h // 50]
        ok = len(f) >= 4 and f[0].group(2) == "full" and later and len(small) == len(later) and \
            bool(e) and all(" OK " in l for l in e)
        areas = sorted(int(m.group(4)) for m in later)
        record(f"Y4 screencast into {kind}: the first frame full, then only the moving square",
               ok, f"{len(f)} frames, later areas {areas[:3]}…{areas[-1:] if areas else ''} px; "
               f"{e[-1] if e else 'no pixel check'}")

s = scen.get("Y5")
if s is not None:
    f = capture_frames("probe-y5.log")
    rate = 0.0
    if len(f) >= 2:
        span = float(f[-1].group(6)) - float(f[1].group(6))
        rate = (len(f) - 2) / span if span > 0 else 0.0
    ok = len(f) >= 20 and rate >= 10.0
    record("Y5 a covered window being cast keeps drawing at the output's pace", ok,
           f"{len(f)} frames of weston-simple-shm behind a covering window, {rate:.1f}/s "
           f"(a covered window not being captured is throttled to 1/s)")

s = scen.get("Y6")
if s is not None:
    locked = read("probe-y6-locked.log")
    unlocked = read("probe-y6-unlocked.log")
    lf = capture_frames("probe-y6-locked.log")
    curtain = any(" OK " in l for l in expects("probe-y6-locked.log"))
    held = any(l.startswith("frame 2 timeout") for l in locked) and len(lf) == 1
    after = any(" OK " in l for l in expects("probe-y6-unlocked.log"))
    lock_lines = [l for l in read("probe-y6-lock.log") if l.startswith("lock locked") or
                  l.startswith("lock unlocked")]
    ok = curtain and held and after and len(lock_lines) == 2
    record("Y6 locked: the curtain (black), no frame while locked, the desktop after unlock", ok,
           f"curtain black={curtain}; second frame held={held}; after unlock the window={after}; "
           f"locker {lock_lines}")

s = scen.get("Y7")
if s is not None:
    if s.get("skipped"):
        record("Y7 wf-recorder (wlr-screencopy)", True, "wf-recorder or ffprobe missing", warn=True)
    else:
        lines = [l for l in read("ffprobe-y7.log") if l.strip()]
        parts = lines[-1].split(",") if lines else []
        ok = len(parts) == 3 and parts[0] == str(out_w) and parts[1] == str(out_h) and \
            parts[2].isdigit() and int(parts[2]) >= 5
        record("Y7 wf-recorder (wlr-screencopy v3): a recording of the output with frames", ok,
               f"ffprobe width,height,frames = {lines[-1] if lines else 'nothing'}")

s = scen.get("Y8")
if s is not None:
    L = read("probe-y8-list.log")
    tops = [l for l in L if l.startswith("toplevel identifier=")]
    ids_ok = tops and all(re.match(r"toplevel identifier=[0-9a-f]{32} ", l) for l in tops)
    closed = any(l.startswith("closed identifier=") for l in L)
    finished = any("finished=1" in l for l in L)
    C = read("probe-y8-cursor.log")
    cur_ok = "cursor enter" in C and any(l.startswith("cursor position ") for l in C) and \
        any(l.startswith("cursor hotspot ") for l in C) and \
        any(re.match(r"cursor frame 1 size=\d+x\d+ visible_px=[1-9]", l) for l in C)
    active = any("compositor.capture: screen capture active" in l for l in comp)
    record("Y8 toplevel list (32-hex identifiers, closed, finished) and a cursor session; "
           "the capture indicator", bool(ids_ok) and closed and finished and cur_ok and active,
           f"{len(tops)} toplevel(s), closed={closed}, finished={finished}; cursor "
           f"{[l for l in C if not l.startswith('constraints')][:5]}; indicator on={active}")

# ── Z: portals (design-app-compat-capture Part E) ──
def portal_log(name):
    return read(f"{name}.log")

def portal_response(name):
    return next((int(l.split()[1]) for l in portal_log(name) if l.startswith("response ")), None)

def portal_file(name):
    for l in portal_log(name):
        m = re.match(r"file (.*) size=(\d+)x(\d+)$", l)
        if m:
            return m.group(1), int(m.group(2)), int(m.group(3))
    return None

s = scen.get("Z1")
if s is not None and s.get("skipped"):
    record("Z portals", False, "the portal session could not be set up: "
           + " | ".join(read("portals.log")[-3:]))
elif s is not None:
    f = portal_file("portal-z1")
    img = ppm(os.path.join(log_dir, "Z1.ppm"))
    shot = ppm(s.get("shot", "")) if s.get("shot") else None
    private = bool(f) and "/lumen-portal/" in f[0] and os.path.exists(f[0]) and \
        (os.stat(f[0]).st_mode & 0o777) == 0o600
    same = bool(img and shot) and img == shot
    ok = portal_response("portal-z1") == 0 and f is not None and (f[1], f[2]) == (out_w, out_h) and \
        private and same
    record("Z1 Screenshot (backend): a private PNG of the output, pixel for pixel the compositor's",
           ok, f"response {portal_response('portal-z1')}, {f}, private 0600={private}, "
           f"identical to the read-back={same}")

s = scen.get("Z2")
if s is not None:
    f = portal_file("portal-z2")
    img = ppm(os.path.join(log_dir, "Z2.ppm"))
    colours = {tuple(img[2][k:k + 3]) for k in range(0, len(img[2]), 3 * 5)} if img else set()
    in_pics = bool(f) and "/Z-pictures/Screenshots/Screenshot from " in f[0]
    ok = portal_response("portal-z2") == 0 and f is not None and (f[1], f[2]) == (s["w"], s["h"]) and \
        colours == {(255, 0, 0)} and in_pics
    record("Z2 interactive Screenshot (frontend): the dragged area, its exact size, all red", ok,
           f"response {portal_response('portal-z2')}, {f}, colours {sorted(colours)[:3]}, "
           f"in the pictures directory={in_pics}")

s = scen.get("Z3")
if s is not None:
    fe, fc = portal_file("portal-z3-enter"), portal_file("portal-z3-click")
    re_, rc, rx = portal_response("portal-z3-enter"), portal_response("portal-z3-click"), \
        portal_response("portal-z3-esc")
    ok = re_ == 0 and rc == 0 and rx == 1 and fe is not None and fc is not None and \
        (fe[1], fe[2]) == (out_w, out_h) and (fc[1], fc[2]) == (out_w, out_h)
    record("Z3 interactive: Enter → all outputs, a click → that output, Escape → cancelled (1)",
           ok, f"enter {re_} {fe and fe[1:]}, click {rc} {fc and fc[1:]}, escape {rx}")

s = scen.get("Z4")
if s is not None:
    col = next((l for l in portal_log("portal-z4") if l.startswith("color ")), "")
    ok = portal_response("portal-z4") == 0 and col == "color 255,0,0"
    record("Z4 PickColor (frontend): the clicked window's colour", ok,
           f"response {portal_response('portal-z4')}, {col or 'no colour'}")

def cast_summary(name):
    L = portal_log(name)
    started = next((l for l in L if l.startswith("started ")), "")
    fmt = next((l for l in L if l.startswith("format ")), "")
    summ = next((m for l in L if (m := re.match(r"screencast frames=(\d+) seconds=([\d.]+) fps=([\d.]+)(?: buffers=(\w+))?", l))), None)
    exp = next((l for l in L if l.startswith("expect ")), "")
    return started, fmt, summ, exp

s = scen.get("Z5")
if s is not None:
    started, fmt, summ, exp = cast_summary("portal-z5")
    xd = read("xdpw.log")
    chooser = [l for l in xd if "chooser: chose 'Monitor: " in l]
    frames = int(summ.group(1)) if summ else 0
    fps = float(summ.group(3)) if summ else 0.0
    ok = f"format {out_w}x{out_h} " in fmt and frames >= 30 and fps >= 20 and " OK " in exp and \
        bool(chooser) and "source_type=1" in started
    record("Z5 ScreenCast (frontend → xdg-desktop-portal-wlr → PipeWire): the output chosen in "
           "Lumen's chooser, frames at the client's pace, the right pixels", ok,
           f"{started}; {fmt}; {summ.group(0) if summ else 'no stream'}; {exp or 'no pixel check'}; "
           f"chooser {chooser[-1].split('portal: ')[-1] if chooser else 'not used'}")

s = scen.get("Z6")
if s is not None:
    started, fmt, summ, exp = cast_summary("portal-z6")
    frames = int(summ.group(1)) if summ else 0
    chose = [l.split("chose ")[-1] for l in read("xdpw.log") if "chooser: chose 'Window: " in l]
    want = None
    if chose and "animate" in chose[-1]:
        want = (s["anim_w"], s["anim_h"])
    elif chose and "solid" in chose[-1]:
        want = (s["red_w"], s["red_h"])
    ok = want is not None and f"format {want[0]}x{want[1]} " in fmt and frames >= 5 and \
        "source_type=2" in started
    record("Z6 ScreenCast of a window chosen in the chooser: that window's size", ok,
           f"chose {chose[-1] if chose else 'nothing'}; {started}; {fmt}; "
           f"{summ.group(0) if summ else 'no stream'}")

s = scen.get("Z7")
if s is not None:
    f = portal_file("portal-z7")
    img = ppm(os.path.join(log_dir, "Z7.ppm"))
    colours = {tuple(img[2][k:k + 3]) for k in range(0, len(img[2]), 3 * 7)} if img else set()
    ok = portal_response("portal-z7") == 0 and f is not None and (f[1], f[2]) == (s["w"], s["h"]) and \
        colours == {(255, 0, 0)}
    record("Z7 Screenshot of a window (target Window): its own pixels at its own size", ok,
           f"response {portal_response('portal-z7')}, {f}, colours {sorted(colours)[:3]}")

s = scen.get("Z8")
if s is not None:
    locked, unlocked = portal_response("portal-z8-locked"), portal_response("portal-z8-unlocked")
    ok = locked == 2 and unlocked == 0 and portal_file("portal-z8-locked") is None
    record("Z8 locked: no screenshot (response 2, no file); after unlock it works", ok,
           f"locked {locked}, unlocked {unlocked}")

# The backend's private screenshots (Z1, Z8) stay in $XDG_RUNTIME_DIR/lumen-portal until a
# running backend expires them; judged, they go.
for z_log in ("portal-z1", "portal-z8-unlocked"):
    f = portal_file(z_log) if scen.get("Z1") is not None else None
    if f and "/lumen-portal/Screenshot-" in f[0]:
        try:
            os.remove(f[0])
        except OSError:
            pass

# ── Protocol errors, both sides ────────────────────────────────────────────
# A protocol error the compositor posts ends with libwayland-server's "error in client
# communication (pid N)"; the client side prints libwayland-client's "<iface>#<id>: error
# <code>: <message>" (GTK adds "Error 71 (Protocol error) dispatching to Wayland display").
# Provoked on purpose: D7's invalid layer state, E6's refused shell copy and F7r's reused
# selection source.
expected_pids = {str(scen[n]["pid"]) for n in ("D7", "E6", "F7r") if n in scen and scen[n].get("pid")}
comm = [m.group(1) for l in comp if (m := re.search(r"error in client communication \(pid (\d+)\)", l))]
unexpected = [p for p in comm if p not in expected_pids]
CLIENT_ERR = re.compile(r"\w+[#@]\d+: error \d+: |[Pp]rotocol error|wl_display[#@]1\.error\(|"
                        r"Lost connection to Wayland compositor|^err: |protocol-error ")
EXPECTED_ERR_LOGS = {"probe-d7.log", "fake-shell.log", "probe-f7r.log"}
client_errs = []
for name in sorted(os.listdir(log_dir)):
    if not name.endswith(".log") or name in ("compositor.log", "run.log") or name in EXPECTED_ERR_LOGS:
        continue
    for l in read(name):
        if CLIENT_ERR.search(l):
            client_errs.append(f"{name}: {l.strip()[:160]}")
            break
# The two expected ones did happen, each with its own error.
d7_err = any("protocol-error interface=zwlr_layer_surface_v1 code=1" in l for l in read("probe-d7.log"))
e6_err = any("error 3: privileged global denied" in l for l in read("fake-shell.log"))
ok = not unexpected and not client_errs and (("D7" not in scen) or d7_err) and \
    (("E6" not in scen) or e6_err) and sorted(comm) == sorted(expected_pids)
record("run: no protocol error except the provoked ones (D7, E6, F7r), either side", ok,
       f"compositor 'error in client communication' pids {comm} (expected {sorted(expected_pids)}), "
       f"client logs with an error: {len(client_errs)}"
       + (f"; first: {client_errs[0]}" if client_errs else "")
       + f"; D7 error seen by the probe={d7_err}, E6 error seen by the copy={e6_err}")

# ── Whole run ──────────────────────────────────────────────────────────────
BUDGET = ("over budget in recent frames",)
# E6 provokes exactly one: a copy of lumen-shell refused at bind.
EXPECTED_WARN = ("privileged global zlm_shell_v1 denied: client pid=",)
# L2 provokes these: its locker exits while locked.
LOCK_WARN = ("session lock: locked -> abandoned", "session lock abandoned (the locker exited)")
# The present watchdog fires when a queued flip has not completed for 250 ms. With a flip
# demonstrably queued (a `commit=content` or `commit=deferred` render line just before it:
# a flip was submitted, or the next one is waiting behind it) that is the host's
# GPU/flip latency — on this VM SVGA3D fences have been seen to take ~280 ms under load,
# the same cause drag-smoke's g reports as a WARN — not a compositor stall. Without one it
# stays a failure.
STALL = "no present in 250ms"
host_stalls, errors = [], []
for i, l in enumerate(comp):
    if l.startswith("[WARN]") and STALL in l and any(
            re.search(r"compositor\.render: .* commit=(content|deferred) ", comp[j])
            for j in range(max(0, i - 30), i)):
        host_stalls.append(l)
    elif l.startswith("[WARN]") and "E6" in scen and any(e in l for e in EXPECTED_WARN) and \
            "fake-shell" in l:
        pass
    elif l.startswith("[WARN]") and "L2" in scen and any(e in l for e in LOCK_WARN):
        pass
    elif l.startswith("[WARN] config: ") and ("R5" in scen or "R6" in scen) and \
            "R-config/config.toml" in l:
        # R5 and R6 write a broken file on purpose: its problems are warnings by design.
        pass
    elif l.startswith("[ERROR]") or (l.startswith("[WARN]") and not any(b in l for b in BUDGET)):
        errors.append(l)
banned = [l for l in comp if any(t in l for t in ("unregistered texture",
          "shm staging returned no buffer", "texture update queue full"))]
empty = [l for l in comp if (m := TREE.search(l)) and any(int(n.group(5)) <= 0 or int(n.group(6)) <= 0
         for n in NODE.finditer(m.group(5)))]
offscreen = [l for l in comp if (m := SUBMAP.search(l)) and not (
    int(m.group(3)) < out_w and int(m.group(4)) < out_h and
    int(m.group(3)) + int(m.group(5)) > 0 and int(m.group(4)) + int(m.group(6)) > 0)]
offscreen += [l for l in comp if (m := POPMAP.search(l)) and not (
    int(m.group(2)) < out_w and int(m.group(3)) < out_h and
    int(m.group(2)) + int(m.group(4)) > 0 and int(m.group(3)) + int(m.group(5)) > 0)]
offscreen += [l for l in comp if (m := re.search(
    r"compositor\.layer: layer map surface=\d+ layer=\w+ namespace=\S+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", l))
    and not (int(m.group(1)) >= 0 and int(m.group(2)) >= 0 and
             int(m.group(1)) + int(m.group(3)) <= out_w and int(m.group(2)) + int(m.group(4)) <= out_h)]
record("run: clean log, tree dumps sane, clean exit, console text",
       not errors and not banned and not empty and not offscreen and comp_rc == "0" and console == "text",
       f"errors+warnings={len(errors)} banned={len(banned)} empty-nodes={len(empty)} "
       f"offscreen-subsurfaces/popups/layers={len(offscreen)} compositor exit={comp_rc} console={console}"
       + (f"; first: {(errors + banned + empty + offscreen)[0]}" if errors or banned or empty or offscreen else ""))
if host_stalls:
    record("run: host present stalls (flip queued, not completed in 250 ms)", True,
           f"{len(host_stalls)} (SVGA3D flip/fence latency, as drag-smoke's g)", warn=True)

# ── Foreign input (as drag-smoke.sh) ───────────────────────────────────────
watch = [json.loads(l) for l in read("watch.jsonl") if l.startswith("{")]
inj_ax = {r["ax"] for r in inj if r["kind"] == "motion" and "ax" in r}
relative_dev = sys.argv[8] if len(sys.argv) > 8 else "/dev/input/event3"
inj_rel = {}
for r in inj:
    if r["kind"] == "rel":
        k = (r["dx"], r["dy"])
        inj_rel[k] = inj_rel.get(k, 0) + 1
seen_rel = {}
inj_wheel = {}
for r in inj:
    if r["kind"] == "wheel":
        inj_wheel[r["clicks"]] = inj_wheel.get(r["clicks"], 0) + 1
seen_wheel = {}
inj_ay = {r["ay"] for r in inj if r["kind"] == "motion" and "ay" in r}
BTN = {"left": 0x110, "right": 0x111, "middle": 0x112}
inj_keys = {}
for r in inj:
    if r["kind"] == "button":
        k = (pointer_dev, BTN[r["button"]], 1 if r["pressed"] else 0)
    elif r["kind"] == "key":
        k = (keyboard_dev, r["code"], 1 if r["pressed"] else 0)
    else:
        continue
    inj_keys[k] = inj_keys.get(k, 0) + 1
seen_keys, foreign = {}, []
for w in watch:
    if w.get("kind") == "dropped":
        foreign.append((w["t_us"], f"{w['dev']}: SYN_DROPPED (events lost; cannot vouch)"))
        continue
    if w.get("kind") != "seen":
        continue
    dev = w["dev"]
    if dev == relative_dev and w["ev"] and all(e[0] == 2 and e[1] == 8 for e in w["ev"]):
        # A wheel frame (REL_WHEEL, group R): the injector's when one of its writes had it.
        clicks = w["ev"][0][2]
        seen_wheel[clicks] = seen_wheel.get(clicks, 0) + 1
        if seen_wheel[clicks] > inj_wheel.get(clicks, 0):
            foreign.append((w["t_us"], f"{dev}: REL_WHEEL {clicks}"))
        continue
    if dev == relative_dev and all(e[0] == 2 and e[1] in (0, 1) for e in w["ev"]):
        # A relative frame (REL_X/REL_Y): the injector's when one of its writes had exactly it.
        dx = next((e[2] for e in w["ev"] if e[1] == 0), 0)
        dy = next((e[2] for e in w["ev"] if e[1] == 1), 0)
        seen_rel[(dx, dy)] = seen_rel.get((dx, dy), 0) + 1
        if seen_rel[(dx, dy)] > inj_rel.get((dx, dy), 0):
            foreign.append((w["t_us"], f"{dev}: REL {dx},{dy}"))
        continue
    for etype, code, value in w["ev"]:
        if dev not in (pointer_dev, keyboard_dev):
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))
        elif etype == 3 and dev == pointer_dev and code in (0, 1):
            if value not in (inj_ax if code == 0 else inj_ay):
                foreign.append((w["t_us"], f"{dev}: {'ABS_X' if code == 0 else 'ABS_Y'}={value}"))
        elif etype == 1 and value != 2:
            k = (dev, code, value)
            seen_keys[k] = seen_keys.get(k, 0) + 1
            if seen_keys[k] > inj_keys.get(k, 0):
                foreign.append((w["t_us"], f"{dev}: key/button {code}={value}"))
        elif etype == 4:
            pass
        elif etype != 1:
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))

width = max(len(r[0]) for r in results)
print()
print("─" * 78)
for name, status, detail in results:
    print(f"{status}  {name:<{width}}  {detail}")
print("─" * 78)
print(f"logs: {log_dir}")
print()
if foreign:
    print(f"VERDICT: INVALID — {len(foreign)} input event(s) the injector did not send; "
          f"first: {foreign[0][1]}. Re-run hands off, host pointer outside the VM window.")
    sys.exit(3)
if any(r[1] == "FAIL" for r in results):
    print("VERDICT: FAIL")
    sys.exit(1)
warned = [r[0] for r in results if r[1] == "WARN"]
print("VERDICT: PASS" + (f" (warnings: {', '.join(warned)})" if warned else ""))
sys.exit(0)
PY
exit $?
