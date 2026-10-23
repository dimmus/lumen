# Unit tests
cd build && ctest --output-on-failure

# vkms Vulkan scanout (needs modprobe vkms + DRM master)
LUMEN_TEST_VKMS=1 ./test_kms_vulkan_scanout

# TTY (needs DRM master, not nested GNOME)
chmod +x scripts/tty-smoke.sh && ./scripts/tty-smoke.sh lumen-smoke

# Input, end to end: does a keystroke reach a client? Run from the TTY you logged into.
./scripts/input-smoke.sh 30

Runs unattended and writes logs, because there is nowhere to watch it — the compositor
takes DRM master and puts the console into graphics mode, so there is no second terminal
and stdout is not visible while it runs. Type and move the mouse during the countdown; the
verdict prints once the console comes back.

It checks the halves separately, because they fail for different reasons and the fix
differs. lumen-input-probe covers devices, libinput and xkb; wev covers routing to a
client. If the probe fails the script stops there rather than starting a compositor, since
a client test on top of a dead input stack only adds noise.

# Interactive move/resize, end to end, unattended (~30 s; hands off, host pointer outside the VM)
./scripts/drag-smoke.sh                       # from the TTY you logged into
./scripts/drag-smoke.sh --join-seat-session   # from SSH: compositor joins the seat session

Runs the compositor at LUMEN_LOG=debug with lumen-square and drives it with injected evdev
events (scripts/inject-input.py, root via sudo -n; needs an absolute pointer such as a VM
console mouse at /dev/input/event2 and a keyboard at event1). Each scenario is judged from
both the compositor's window-manager log and lumen-square's stdout: client move; client
resize from the bottom-right and top-left corners (opposite corner fixed); the 120x90
min-size clamp; Super+left move with no button reaching the client; click-to-focus/raise
with two windows; a client killed mid-grab. Two whole-run checks: frame-perfect resize (one
configure in flight, every committed size equals its configure) and input-to-present
latency, measured from the injector's CLOCK_MONOTONIC write time to the page flip that
first shows the motion. Latency fails on a median over 1.5 refresh periods + 4 ms (Lumen's
own pacing) and warns on a p95 over 2 periods + 4 ms, splitting every sample into queued
(behind the previous flip), compositor, GPU (fence) and display time so a warning says
whose time it was. On this vmwgfx host the tail is SVGA3D fence latency, not Lumen.

A VM console forwards the host pointer through the same device the injector writes to, so
a hand on the mouse corrupts the run. The script says so with a countdown, records every
frame on every input node (inject-input.py watch), and reports any frame it did not
inject as VERDICT: INVALID (exit 3) instead of a list of misleading failures. Exit codes:
0 PASS (possibly with warnings), 1 FAIL, 2 could not run, 3 INVALID. Logs in
drag-smoke-logs/.

# Real applications, end to end, unattended (~6 min; hands off, as drag-smoke)
./scripts/app-smoke.sh --join-seat-session [--only A,B,C,D,E,F,G,H,I,J,X,R] [--skip-gtk]

Same machinery as drag-smoke (seat session, injector, foreign-input watch, SIGTERM-only
cleanup of the PIDs it started, exit codes 0/1/2/3, logs in app-smoke-logs/), with real
clients. Judged from the compositor log, chiefly the surface tree's
`compositor.scene: tree root=… nodes=<sid>[*]@x,y,wxh;…` dumps, which list every drawn
surface of a window in paint order at each change, and from the clients' side too: the
probes' and lumen-square's stdout, `WAYLAND_DEBUG=client` traces of foot (A, C, F, G11, H5,
I3, J6), alacritty (F) and gtk4-widget-factory (B6/B7, F4, G11, H5), and lumen-shell's own log
(E, I1, J2). About 6 minutes for all groups (91 checks); `--only` runs a subset.
Group A (subsurfaces):
A1 foot with client-side decorations (`-o csd.preferred=client`: since step C the compositor
offers server-side ones, and a client that insists on its own keeps them) maps its title bar
and borders as subsurfaces, the
title above the main surface and inside the window geometry; A2 dragging that title bar
moves the window and every node by the same delta (a CSD client starts the move a few
pixels in, after its drag threshold); A2s Super+drag moves the window and every node by
exactly the pointer delta; A3 resizing from foot's border subsurface (just
outside the window geometry) keeps the title bar exactly as wide as the window in every
dump, one configure in flight, every geometry change answering an acked configure (foot
rounds to cells, so committed ≠ configured is expected); A4 lumen-square `--sub` keeps its
synchronized strip at the window's width and origin in every dump during a resize, and
`--sub-desync` shows the strip resizing alone (in both, every strip width lumen-square printed
as committed must reach a dump); A5 gtk4-widget-factory maps and stays up. Client-side
evidence for A1–A3: foot runs with `WAYLAND_DEBUG=client`, and its own requests must show
the subsurfaces (≥4 `wl_subcompositor.get_subsurface`), exactly one `xdg_toplevel.move` (A2)
and one `xdg_toplevel.resize` with edges 10, bottom-right (A3).
X: lumen-square `--destroy-parent-first` destroys the parent wl_surface before the strip's
wl_subsurface without a protocol error. Every run: no ERROR/WARN (budget summaries
excepted; the 250 ms present watchdog with a flip already submitted is a WARN row —
SVGA3D fences have been measured at 0.28–1.2 s under GTK or resize load, the same host
latency drag-smoke's g reports — while one without a submitted flip fails), no unregistered texture or dropped staging, positive node sizes, subsurfaces on
the output, clean exit, console back in text mode. GTK runs with `GDK_BACKEND=wayland
GSK_RENDERER=cairo` (shm, no GL probing differences in the VM); its default GL renderer
was checked by hand on this host (dma-buf imported on the GL render thread).

Group B (popups) uses `lumen-probe popup` (tests/clients, built with the tests) for exact
geometry, and GTK for real menus: B1 the unadjusted placement (bottom_left anchor,
bottom_right gravity) is configured and mapped at the parent-relative rect; B2 with the
probe's right edge 50 px from the output's, a popup opening rightwards is flipped
(`flip_x`: its right edge on the anchor rect's left edge), slid (`slide_x`: against the
output edge) or resized (`resize_x`: to the 50 px left); B3 three nested grabbing popups —
the keyboard (a KEY_A) goes to the topmost, a click on the empty desktop dismisses all three
top-down (`popup_done` 2, 1, 0), reaches no client, and the keyboard returns to the window;
B4 a grab quoting a stale serial (the `enter`, not the press) is denied with an immediate
`popup_done`; B5 Super+drag ends a popup grab (`reason=wm-grab`), and a non-grabbing popup
moves with its window by exactly the window's delta; B6 gtk4-widget-factory's text context
menu (right-click) is grabbed, mapped at window + positioner rect, takes the keyboard, closes
on Escape and on an outside click, flips upward near the output's bottom and slides at its
right edge. B7 a real GTK nested chain: GTK 4's popover menus open submenus inside the same
popover (sliding), but a GtkPopoverMenuBar's submenus are child xdg_popups. B7 clicks "Page 2"
in widget-factory's header bar, opens the page's menubar Edit menu with a click (a grabbing
popup of the window) and its C_hecks submenu with the H mnemonic (a grabbing popup of that
popup, `parent_kind=popup`, quoting the key serial); both map at parent + positioner rect,
the keyboard is on the submenu, and a click on the empty desktop dismisses the submenu then
the menu (`reason=outside-click`) and returns the keyboard to the window. B6/B7 run
widget-factory with `WAYLAND_DEBUG=client` and are also judged from GTK's side: every
`xdg_popup.configure` it received carries exactly the rect the compositor's `popup position`
line computed (popups followed in creation order — GTK reuses a popover's wl_surface), it
received `popup_done` for exactly the popups the compositor dismissed, in the same order, and
exactly six button presses (its four right clicks, the page switch and the Edit click — none
of the outside clicks). B6's and B7's points are on widget-factory's layout at the size this
1914x924 output gives it; on another output those rows only WARN when no menu opens there.
F10 does not open a menu on widget-factory 4.24's first page (it has no primary menu button,
and F10 only selects page 2's menubar without opening it), so the design's F10 step was
replaced by the right-click context menu and the menubar click.

Group C (server-side decorations, `compositor.deco` lines): C1 foot asks for server-side and
gets it — `decoration mode … sent=server`, then `decoration apply … mode=server` with the
frame on the output; `foot -o csd.preferred=client` is answered client-side and draws its own
subsurface frame; gtk4-demo (no decoration object) is never told server-side. C2 dragging the
compositor's title bar starts a `source=decoration` move that moves the window by exactly
the pointer delta (no client had pointer focus for that press). C3 dragging from 4 px
outside the right edge (the invisible resize band) is a `mode=resize edges=8` grab; the
width follows to within foot's cell rounding. C4 a click on the close button sends
`xdg_toplevel.close`: foot exits within 2 s and unmaps. C5 a title the client sets is
rasterized (`decoration title … title="…"`) and a title too long for the window is ellipsized
to fit the room left of the close button. From foot's side (`WAYLAND_DEBUG=client` for
C1–C4): it sent `set_mode(server_side)`, its first decoration configure was server_side, it
received not a single `wl_pointer.button` across C2–C4 (every press was on the frame), and
it received `xdg_toplevel.close`. C6 the frame of the window that takes the keyboard turns active (`decoration active`) and a
middle click on its title bar lowers it to the bottom. The frame's pixels are checked without a session by
`test_decoration_geometry` (the frame's draws composited on the CPU backend: baked colours
per zone, active/inactive, hover/pressed, maximized) and `test_gl_composite` (a swatch
centre stretched on the GL backend without bleed); text rasterization by `test_text_raster`.

Group D (wlr-layer-shell, `compositor.layer` lines) uses `lumen-probe layer`; a 32 px top
panel from D1 stays up through D8. D1 the panel is configured full-width, maps at 0,0, its
zone makes `usable area x=0 y=32 …`; lumen-square maps below it and a Super+drag 600 px up
stops with the window at y=32. D2 a background surface with zone -1 is configured to the
whole output despite the panel. D3 an overlay anchored bottom-right with margins 10 (right)
and 20 (bottom) maps exactly there. D4 an exclusive overlay over a focused probe window takes
every key (`focus keyboard-surface kind=layer`; KEY_A and, after a right click on the window,
KEY_B reach the overlay, not the window), the window is deactivated meanwhile, and after the
overlay unmaps the keyboard returns (KEY_C reaches the window). D5 an on_demand surface gets
no keyboard when it maps, gets it from a click (KEY_A), and loses it to a right click on the
window (KEY_B to the window); it is placed below the panel (y=32). D6 maximize: a title-bar
double click on foot's server-side frame fills the usable area minus the title bar (0,60
1914x864 on this output), follows a second 40 px bottom panel coming and going, and a second
double click restores the exact previous geometry; gtk4-demo's header-bar double click
(`xdg_toplevel.set_maximized`) fills exactly 0,32 1914x892 and restores. D7 a layer surface
with width 0 and no left+right anchors is the client's protocol error (`zwlr_layer_surface_v1`
code 1, probe exit 3); the compositor carries on. D8 a popup of a bottom panel
(`zwlr_layer_surface_v1.get_popup`) flips upward into the output, grabs without taking the
keyboard (the panel's interactivity is none), and is dismissed by an outside click. The
injector's `button … dblclick` sends the two clicks 170 ms apart from one process.

Group E (lumen-shell, `compositor.shell` lines in the compositor log and the shell's own
`shell.bridge` / `shell.panel` lines in shell.log, both at `LUMEN_LOG=debug`), with
lumen-square and foot as windows. E1 with both mapped, lumen-shell binds: `bridge bound …
snapshot=2`, the shell logs `snapshot toplevels=2` and the `focus handle=` sent right after
it, its panel maps through layer-shell at 0,0 full-width × 32 (`namespace=lumen-panel`), the
usable area becomes y=32, and its first draw has two entries with foot's (mapped last, so
focused) highlighted. E2 a click on the other entry (its x/w from the shell's `layout` line)
sends `bridge request=raise … result=ok` then `request=activate … result=ok` for exactly that
window, the window manager raises it (`raise id=`) and focuses it (`focus keyboard id=`),
lumen-square receives that activating configure (same serial, `states=…activated` in its
stdout), and the panel's highlight moves to it. E3 a new foot that retitles itself
(`printf '\033]2;…\007'`) maps below the panel (its frame's top at y ≥ 32), adds an entry
and renames it live, and that redraw damages one entry, not the panel. E4 lumen-square exits: its entry
goes. E5 the shell is stopped (`bridge destroyed`, the usable area back to the whole
output) and started again: a fresh snapshot of the two current windows, listed in the order
the old panel showed them (the snapshot is in creation order), the panel and the y=32 usable
area return. E6 a copy of the lumen-shell binary at another path exits 1 —
`zlm_shell_v1 bind rejected` in its own log, the compositor's warning naming both paths, no
new bridge — while the real shell keeps running. Neither shell log has an error or warning.

Group F (clipboard and primary selection; `compositor.selection` lines, foot's
`WAYLAND_DEBUG=client` traces, the files the pasting terminal's `cat` writes, compared byte
for byte; foot runs with `LC_ALL=C.UTF-8`, as in "C" it prints a locale warning into the
terminal). F1 foot shows a word, a double click selects it and Ctrl+Shift+C copies it; a new
alacritty running `cat > F1.out` is offered the selection as it takes the keyboard
(`selection offer … cause=focus`), Ctrl+Shift+V, Enter and Ctrl+D leave exactly the word and
a newline in the file, and the log has `selection set` by foot, the offer to alacritty and
`selection receive … result=sent`. F2 the mirror, alacritty → foot. F3 primary: alacritty
running `cat` maps first, foot showing a word maps over it and is moved aside (Super+drag),
the word is double-clicked (no copy key), and a middle click on alacritty — which has no
keyboard until that click — pastes it; the primary offer goes out before the click's
`focus keyboard` line and so before the press. F3r the reverse into foot, whose own trace
shows the primary offer before its middle press. F4 GTK both ways through widget-factory's
text widget (B6's point): typed text copied with Ctrl+C pastes into foot, and text copied in
foot pastes into GTK with Ctrl+V and back out again (GTK's trace has
`wl_data_offer.receive("text/plain;charset=utf-8"`); WARN when GTK never set a selection
(its layout put no text widget there). F5 foot copies, then alacritty: foot's source is
`cancelled`; alacritty exits with foot left focused: `selection clear kind=clipboard
cause=source-destroyed` and foot is told `selection(nil)`. F6 every `wl_keyboard.enter` in
foot's traces is immediately preceded by `wl_data_device.selection` and
`zwp_primary_selection_device_v1.selection` (alacritty reads selections on a queue of its
own, so its trace is dispatch order, not wire order), and a `lumen-probe selection
--receive` that lost the keyboard is offered nothing while foot copies, then on a click is
offered the selection and reads it. F7 a probe quoting serial 1 is refused (`selection
reject … reason=serial`) and its source cancelled, the selection unchanged; F7r a probe
setting one source twice gets `wl_data_device` error 1 (used_source), provoked on purpose.

Group G (drag-and-drop; `compositor.dnd` lines, the probes' stdout, `tree root=… kind=dnd_icon`
dumps). Each scenario starts a `lumen-probe dnd-target` and a `lumen-probe dnd-source`, puts
them side by side (Super+drag; the source's optional second window to the right) and drags
from the source's centre in 100 injected steps. G1 copy|move offered, the target prefers
move: one `dnd focus` on the target, at least 10 `dnd motion` lines before `dnd drop`, the
data read and finished, the source's `drop_performed`, `finished` and last action 2, `dnd end
reason=drop`, and no button release reaching either probe. G9 (G1's 32x32 icon attached at
-16,-16): every `cause=move` dump of the icon node is an injected position plus that offset
(±1 px), the icon is unmapped at the end, and no frame during the drag damages the whole
output. G2 Ctrl held → copy (action 1 to the end); G2b Ctrl, then Shift over the target →
copy, then move. G3 a release over the empty desktop: `cancelled`, no `drop_performed`,
`reason=no_target`. G4 Escape over the target: `leave` without `drop`, `cancelled`,
`reason=escape`, and no Escape key event at either probe. G5 a target that accepts nothing:
`leave`, no drop, `cancelled`, `reason=not_accepted`. G6 a drop on the source client's own
second window. G7 a drag with no source: the target probe it crosses sees nothing, the own
second window gets `enter … offer=nil` and the drop. G8 the source probe exits abruptly while
the pointer rests on the target: `leave`, `reason=source_destroyed`, the compositor alive,
and `focus pointer id=` again after the release. G10 start_drag quoting the pointer enter's
serial: `dnd start rejected … reason=serial`, the source `cancelled`, and it keeps the pointer
until its own release. G11 GTK → foot (WARN-level): widget-factory is moved left, its text
view filled with twelve numbered lines through the clipboard (a probe owns it), all of it
selected and dragged onto foot running `cat`; the file must equal the lines.

Group H (outputs; `compositor.output` lines, `lumen-probe outputs` stdout, foot's, alacritty's
and GTK's traces).
H1 a probe binding `wl_output` v4 and `zxdg_output_manager_v1` v3 before mapping reports the
output's name (the compositor's `output 0 name=` line, `Virtual-1` here), mode and logical
rect, and its toplevel gets `enter`. H2 foot's own traces from A1 (client-side frame) and C1
(server-side): the main surface gets `wl_surface.enter`, and so does every CSD subsurface
the compositor mapped (foot listens on its main surface only, so libwayland-client logs the
others as `discarded … enter`). H3 a probe binding `wl_output` a second after its first
configure: the compositor's `output bind … replayed=1`, then the probe's enter. H4 a
grabbing popup gets `enter` when it maps and `leave` when an outside click dismisses it, and
the toplevel `leave` when it unmaps (null attach). H5 real clients from their own side: a
traced foot and a traced gtk4-widget-factory map; in each trace `wl_output.mode` and `name`
and `zxdg_output_v1.logical_position` (0,0), `logical_size` (the mode) and `name` equal the
compositor's output, and the toplevel's `wl_surface` (from `get_xdg_surface` +
`get_toplevel`) gets `enter`; the compositor logs an `xdg-output create … w= h= name=` with
the same values for both and `output enter … resources>0` to both. H6 every traced
real-client log of the run (foot, alacritty and GTK from A, B6, C, F, G11, H5, I3, J6): the
same values, and per (surface, output) `enter`/`leave` alternate starting with `enter` — no
double enter, no leave without enter — and name a `wl_output` the client bound (object ids
are followed through `create_surface`/`destroy`, since libwayland reuses them; GTK's menus
are counted as popups entered/created).

Group I (liveness; the compositor runs with `--ping-timeout 1500`; `compositor.liveness`
lines). I1 lumen-shell up, then a probe with a server-side frame that leaves the activation
ping unanswered for 4 s: exactly one `not-responding` (after 1500–2500 ms), the frame title
`lumen-probe-i1 (Not Responding)`, `tree root=… dim=1`, the shell's `flags handle=…
flags=1`; when the probe answers: `responsive`, the plain title, `dim=0`, `flags=0`. I2 a
probe that never answers is marked once and, three timeouts later, still connected and
still receiving clicks. I3 a real hang: foot (server-side frame) is stopped with SIGSTOP and
clicked (`ping … reason=press`): `not-responding` and the title suffix; 600 injected motions
over it reach it as at most three (the motion before it was marked and the one resync
motion after) — none of them queue up while it is stopped, and libwayland's 1 MiB buffer
keeps it connected anyway; after SIGCONT it is `responsive` and receives a key.

Group J (xdg-activation; `compositor.activation` lines, `lumen-probe activation-issuer` /
`activation-target` stdout, lumen-shell's log, foot's trace). A target probe maps first,
then an issuer probe (which takes the keyboard) is clicked and writes the token it got for
that press; the script hands it to the target. J1 `token create … grantable=1 reason=ok`,
`activate … result=granted reason=ok`, the target ends with the keyboard and sees
`keyboard enter`. J2 lumen-shell up; between the issue and the hand-over foot maps and is
clicked: `result=urgent reason=user_moved_on`, `urgent id=<target> on=1`, focus stays on
foot, the shell's `flags handle=<target> flags=2`; a click on the target's panel entry
focuses it, `on=0`, `flags=0`. J3 a token issued without a serial: `result=urgent
reason=no_serial`. J4 a token nobody issued (`deadbeef`): `result=urgent
reason=unknown_token`, no protocol error. J5 a target presenting the token before its first
commit: `result=pending`, then `result=granted reason=ok cause=map` and the new window has the
keyboard; J5b a launch token the user moved on from (a click in the J5 window after it was
issued): at map `result=urgent reason=user_moved_on`, the new window never takes the keyboard
and is marked; J5c an unknown token before the first commit proves nothing: at map
`result=granted reason=new_window` and the new window has the keyboard (GTK presents a
self-issued token, serial 0, for every new window). J6 a real foot started with
`XDG_ACTIVATION_TOKEN` set to the issuer's token presents it (`xdg_activation_v1.activate`
in its trace), is granted and has the keyboard, and logs nothing about XDG activation (it
warned "compositor does not implement XDG activation" while the global was missing).

Whole run, both sides of the wire: exactly three protocol errors happen, the provoked ones
(D7's probe, E6's copy and F7r's probe): libwayland-server's `error in client communication
(pid N)` appears in the compositor log for those PIDs only, each of them saw its error, and
no other client
log (foot, GTK, the probes, lumen-square, the shell) has a libwayland-client
`<interface>#<id>: error <code>:` line, `wl_display.error`, GTK's `Protocol error` or
`Lost connection`, or foot's `err:`.
The shell's pure parts are covered by `test_task_model` (snapshot, deltas, stale handles,
order, UTF-8-safe title cut, the panel layout and its repaint/damage diff) and the
bridge end to end by `test_shell_bridge` (compositor and lx.shell in one process, the
privilege check pointed at the test binary: capabilities, snapshot + focus, the panel's
first commit, title/create/destroy deltas, request_activate answered by focus_changed, a
restarted shell's fresh snapshot).

Protocol errors are covered without a session by `test_protocol_conformance` (in-process
compositor, one connection per case, `wl_display_get_protocol_error`; xdg-decoration's
`already_constructed`, `invalid_mode`, `orphaned` and version 1 `unconfigured_buffer`
included, and every wlr-layer-shell error: `role`, `invalid_layer` (also from `set_layer`),
`already_constructed`, `invalid_anchor`, `invalid_keyboard_interactivity` (also on_demand on
v3), `invalid_size` ×2, `invalid_exclusive_edge` ×2, `invalid_surface_state` ×2, layer
`get_popup` on a committed popup, `defunct_role_object`; a `zlm_shell_v1` bind from a binary
that is not the configured shell is the display's `implementation` error; the data device's
`used_source`, `invalid_source` (a drag source as the selection), `invalid_action_mask`
(twice, an undefined bit, after use) and a selection offer's `invalid_finish` and
`invalid_offer`; the clipboard's selection reaching a client before its keyboard `enter`, a
receive's fd round trip, no forwarding of a type not offered, an offer going inert when its
client loses the keyboard, a serial never sent refused and cancelled, a new owner
cancelling the old source, `selection(NULL)` when the owner destroys its source, and the
same for the primary selection; drag-and-drop's `role` (an icon with another role) and
`used_source` on the data device, the source's `invalid_action_mask` after start_drag, the
offer's `invalid_action_mask`, `invalid_action` (two preferred bits) and `invalid_finish`
(before the drop), and a drag across two clients — the origin's `wl_pointer.leave`, the
target's `source_actions`, `enter`, `action`, `motion`, `drop`, `leave`, the source's
`target`, `action`, `dnd_drop_performed`, `send`, `dnd_finished`, the data read through the
dropped offer, no button release reaching a client — then a stale serial cancelled with
pointer focus untouched, a target that accepted nothing (leave, cancelled), Escape, and the
source's client exiting mid-drag; xdg-activation's `already_used` (a token committed
twice), an unknown token and requests after commit ignored without error, a token whose
requesting surface is gone still usable, a popup's surface ignored, and the policy on the
wire — a token from the focused issuer's click focuses another client's window, a spent
token does not mark the focused window, a click in a third window between issue and use
marks the target demands-attention without focusing it, a serial never delivered marks it
too, focusing the window clears the mark, and a token presented before the first commit is
decided at map (granted: the new window takes the keyboard; unknown: it does not, and opens
marked just below the focused window) — these use the compositor's `inject_*` virtual input
seam where they need input; one client hoarding activation token builders, data sources,
data devices and `wl_output` objects (as many as the old shared tables held) must not cost
another client its token, source, device or `wl_output.done` (it used to get `no_memory`,
fatal); destroying the `xdg_wm_base` a ping went out on drops that ping (the client used to
stay awaited, then "not responding", for good); and out-of-order
client disconnects must leave no server client slot behind — they used to corrupt a
client's destroy list and could spin the compositor forever), positioner placement and the popup
grab/dismissal state machine by `test_positioner` (pure), and layer arrangement (every
anchor combination, margins, zones, the two passes, exclusive-edge deduction, closing) and
the layer shell's configure/map/keyboard state machine by `test_layer_arrange` (pure). The
data-transfer pieces with no Wayland in them are `test_data_transfer` (the inline mime list:
order, duplicates, overflow by count and by bytes, reset; drag-and-drop mask checks, the
modifier-chosen action and the whole negotiation table, version-2 sides included) and
`test_input_serials` (the per-client serial log: delivered ranges only, compression of
consecutive serials, the ring keeping the newest 32 ranges and rejecting older serials,
wraparound at 2^32, the newest-press queries, LRU eviction and forget). `test_dnd_grab`
drives the drag-and-drop grab with fake wire ops over a real surface tree: target changes vs
motion, drop vs cancel and their reasons, Escape, the source-less drag kept inside its
client, the modifier-chosen action on change only, a target unmapped or destroyed, the icon
following the pointer per tick with its accumulated offset (mapped in its layer, gone at the
end, reusable), and no allocation on the motion and tick paths (a counting `operator new`).
`test_output_membership` covers output membership (rects over one, two and three outputs,
edge-touching rects, the primary output by area, the preferred scale — ceiling of the
largest — and transform) and the output manager (stable slots, the generation counter, the
virtual HEADLESS-1 without connectors). `test_liveness` drives the ping/pong tracker on a
fake clock: one outstanding ping per client and 2 s between pings, timeout → not
responding, a late pong → responsive, a wrong serial ignored, the periodic ping of the
focused client, a client without an `xdg_wm_base`, a ping cancelled with the object it went
out on (also clearing "not responding"), forget, disabled with 0, a full table never
evicting a hung client. `test_output_contract` checks the output contracts on the wire
against an in-process compositor: `wl_output` v4 and xdg-output v3/v2 values and their
`done`s, `enter` on map for toplevels, subsurfaces and popups, `leave` on unmap, the enter
replayed to a late `wl_output` bind, nothing preferred at scale 1, and an output change seen
by a frame tick (a transform) resent to the bound `wl_output` and as
`preferred_buffer_transform` to the surface on it. `test_shell_bridge` also checks that
`toplevel_flags` reaches a v2 bridge (after `snapshot_toplevel`, and as deltas on the
right window) and never a v1 one. `test_activation_policy` checks xdg-activation's policy
row by row (`decide_grantable`; `decide_activation`'s nine rows in order, the user-time rule
with the issuer or the target focused, a focused client with no press, nothing focused),
serial wraparound, the lifetime boundary, and the token table (exact-text lookup only,
`forget_surface`, single use, eviction of the oldest and of an expired token) and the token
text (32 lowercase hex characters, 10 000 unique).

# The login session, end to end (~1 min; ADR 011, design-session-and-wm T.3)
./scripts/session-smoke.sh                 # from the console or SSH; --watchdog adds the watchdog run

Installs the build into a scratch prefix (`--prefix`, default `<build>/session-smoke-prefix`;
every installed file must be under it) and runs `run-desktop.sh --systemd --prefix …`: the
prefix's units copied into `$XDG_RUNTIME_DIR/systemd/user` for this run only (drop-ins send
the logs to the log directory), `lumen-session` started with the active seat0 session's
`XDG_SESSION_ID`, so the compositor runs in `user@.service` exactly as after a login. Checks:
`lumen-compositor.service` active with `StatusText=Serving …` (READY), `graphical-session.target`
active, the exported environment in the user manager, logind's session `Type=wayland`, the
shell's unit and panel, an `app-*@autostart.service` started after READY, `lumen-ctl status`,
`health --json` (the `systemd` part: ready, exported, watchdog on), `outputs`, `windows`; the
shell killed with SIGKILL comes back (`NRestarts=1`) with the same snapshot, the zone hold
keeps a maximized window's geometry and the panel maps again; `systemctl --user reload`;
`lumen-ctl logout` ends the session with lumen-session exit 0, the session `Type` back to `tty`,
the console shown, and run-desktop.sh's own checks: the manager's environment and its
enabled/linked unit files exactly as before, no session unit left (units that failed during
the session — an autostart entry needing X11 — are reset). `--watchdog`: a second session with
`WatchdogSec=4` survives 6 s on pings, then the compositor stopped with SIGSTOP is aborted by
systemd and the session ends cleanly (lumen-session: `Lumen exited: watchdog`). Ends with
`VERDICT: PASS|FAIL`. `XDG_CONFIG_HOME` is a directory under the logs. The reload check also
wants a second configuration generation logged before `systemctl --user reload` returns
(READY comes once the reload is applied).

# The configuration file (ADR 011 Part 2, design-session-and-wm T.1, T.3)
./scripts/app-smoke.sh --join-seat-session --countdown 0 --only R   # ~40 s

app-smoke's compositor runs with `LUMEN_CONFIG=<logs>/R-config/config.toml` (absent until
group R writes it: every other group sees the defaults, never the user's file). Group R, with
lumen-shell and `lumen-probe keyboard` (a client printing every keymap with its layouts,
repeat_info, key with the keysym and text its own xkb produces, modifiers, relative motion and
scroll event): R1 the file appears by rename — repeat 10/600 and a `us,de` keymap reach a new
client, the mouse is configured flat 0.5 with natural scrolling (the compositor's
`input device "…" (mouse): …` line), the cursor 48 px, and the configured `Super+Y =
window.close` closes the probe; R2 `grp:alt_shift_toggle`: y, Alt+Shift, y types y then z;
R3 a live edit reaches the client (repeat 30, ~150 ms: 100 ms debounce), one generation, no
notice; R4 a wheel notch up (injected `REL_WHEEL`, `inject-input.py wheel`) scrolls positive
(natural) and doubled (scroll-factor 2: +30, two notches), and relative motion is ×1.5 (flat
0.5); R5 a syntax error is rejected — nothing
resent, `lumen-ctl reload` exits 1 naming the line, one panel notice; R6 a bad value keeps
that key's value (`keeping 30`), an unknown action suggests the right one, and removing the
file brings every default back (repeat 25/600). R's intentional `config` warnings are the
only ones the clean-log check allows.

Unit tests: `test_config` (lx.config alone: inline tables; defaults; the documented
`deploy/lumen/config.toml` parses to exactly the defaults and names every key; every value
kind; bad values keep the previous value; unknown keys and sections with suggestions; a
syntax error rejects with the layer and its line; layers merge key by key, devices by name,
bindings per combination, `none` removes; `split_layers` round trip; the bindings grammar —
modifiers, keysyms, buttons, taps, `Super++`, reserved combinations, unknown actions,
arguments, `allow-when-locked`, duplicates, actions not available yet; the bounded
diagnostics; `locate_config` with XDG and `LUMEN_CONFIG`; the inotify watch through a
directory appearing, an editor's rename, an in-place write, a removal and a symlink's target;
the 256 KiB bound; `check_keymap` with xkbcommon's reason), `test_input_config` (pointer
settings → libinput calls through recording ops: a touchpad taps, a mouse never does,
"default" restores the device's default, unsupported settings skipped and reported, a
`[[input.device]]` override reaches only that device), `test_keybindings` (the binding table:
exact modifiers, buttons, taps, locked and inhibited filters, shell actions only with a v7
shell and the legacy grab otherwise, `binding_table` without `shell.lock` when no lock command
is set), `test_input_keymap` (a new keymap adopts locks, the clamped layout and held keys;
`cycle_layout`; xkbcommon's reason on a failed compile), `test_shell_bridge` (v7: `config` on
bind with the file's bytes behind their marker, `config_diagnostic`/`config_done`,
Super+Return as `action shell.terminal` with the shell's key-press serial, a Super tap as
`shell.launcher`), `test_control` (Reload answered with its diagnostics). ctest runs every
test with `LUMEN_CONFIG=0`.

# Workspaces (ADR 011 Part 3, design-session-and-wm W, T.3 group Q)
./scripts/app-smoke.sh --join-seat-session --countdown 0 --only Q   # ~60 s
./scripts/monitor-smoke.sh --join-seat-session --countdown 0 --only add,workspaces

Group Q runs lumen-shell and `lumen-probe window` clients (a plain window printing every
configure's states, `suspended` included) and `lumen-probe workspaces` (an ext-workspace-v1
client printing every event and, at each `done`, `ws output=OUT workspaces=1*,2,3`): Q1 three
windows, Super+2 hides them (each told `suspended`, the keyboard nowhere, the probe sees 2
active), Super+1 shows them and gives the keyboard back to the window focused there; Q2
Super+Shift+2 moves the focused window and follows (the shell's indicator logs
`workspaces [1+ 2*+ 3]`); Q3 an ext-workspace activate + commit from the probe; Q4 a
fullscreen window and its black backdrop are gone from a screenshot on another workspace and
back on its own; Q5 Super+H minimizes, a click on the panel entry restores (keyboard
included), and `xdg_toplevel.set_minimized` minimizes; Q6 a window on a workspace that is not
shown is still captured (`lumen-probe capture`, frames keep coming); Q7 while locked, Super+2
and an ext-workspace activate change nothing (`ext-workspace: commit while locked; ignored`).
monitor-smoke's `workspaces` group puts two workspaces on Virtual-2 (lumen-vmw-layout),
unplugs it — the active workspace's window stays visible on Virtual-1, the other workspace is
appended there — and plugs it back: both return, the active one active again, and the shell's
indicator on Virtual-2 shows `1+ 2*+ 3` (checked with `lumen-ctl workspaces` and `windows`).

Unit tests of workspaces: `test_workspaces` (the model alone: dynamic invariants, a workspace
left empty goes, static counts and names and a smaller count merging into the last, index past
the end, neighbours, focus memory and urgency, hotplug — the active one merged into the
fallback's, the others appended with their homes, the return with the active one restored,
static merging by position —, stranded workspaces adopted by the next output, `assign`, ids
never reused), `test_ext_workspace` (in-process with two virtual outputs: the bind order and
one `done`, `output_enter` for a wl_output bound later, a window joining the active workspace
and the empty one after it, activate + commit hiding and `suspended`, focus memory, the
removal order `workspace_leave` → `removed` → `done`, inert handles, minimize and sticky, one
commit of assign + activate moving a workspace with its window to the other output as one
`done`, `stop` → `finished`), `test_shell_bridge` (v7: `toplevel_workspace` after the snapshot,
again after a late ext-workspace bind with the shell's own handle, the sticky flag, the
minimized state), `test_task_model` (membership, the current-workspace filter, minimized
dimmed, the indicator's cells, hit and diff), `test_control` (`ListWorkspaces`, a window's
workspace, the `workspace` event), `test_window_protocols` (a sandboxed client does not see
ext_workspace_manager_v1), `test_config` (the workspace and minimize default bindings).

```bash
# Switching and snapping (ADR 011 Part 4, design-session-and-wm K, T.3)
./scripts/app-smoke.sh --join-seat-session --countdown 0 --only O                # ~45 s
./scripts/app-smoke.sh --join-seat-session --countdown 0 --only O --scale 1.25   # fractional
./scripts/monitor-smoke.sh --join-seat-session --countdown 0 --only add,snap
```

Group O runs lumen-shell and three `lumen-probe window --keyboard` clients (each prints its
configures with the tiled states and what its wl_keyboard is told): O1 a quick Alt+Tab focuses
the previous window and the switcher is never mapped (`switcher: activate (released, never
shown)`); O2 Alt held, Tab, wait, Tab, release: the switcher's layer surface is mapped centred
(a screenshot shows its background and selection colours inside its rect, at the run's scale)
and the third MRU window gets the keyboard; O3 Alt+Tab then Escape: no focus change, the
focused window saw Alt's press and release only — no leave, no Tab, no Escape; O4
Alt+Shift+Tab: the least recent window; O5 Super+Left, Up, Down, Right, Up, Down: the
window's configures are the left half, the top-left quarter, the left half (all four tiled
states), its old size, maximized, its old size — and it is back at its old place; O6 a
Super+drag to the left edge: `snap preview zone=left`, a screenshot while held has the
preview's border and fill composited exactly over what was there before (premultiplied, ±4),
the release snaps it to the left half; dragged out again it is restored to 400x300
(`drag restore`); O7 Alt+F7, Right ×3, Down, Return moves it by (30, 10) and the window saw
none of those keys; O8 Alt+F8, Right ×2, Down, Return: 20 wider, 10 taller, the origin kept;
O9 the session locked during a hold: `held action cancelled (session locked)`, the switcher
closes without activating; O11 a click on a window during a hold cancels it (`held action
cancelled (click elsewhere)`) and that window keeps the keyboard; O10 with the shell stopped,
Alt+Tab is the compositor's quick switch. `--scale S` (group O only) runs the compositor at that output scale; the script turns
the logged logical positions into the injector's output pixels. monitor-smoke's `snap` group
(two outputs): Super+Left from a left half crosses to the right half of the output to the
left and Super+Right back; a Super+drag dropped at the edge the outputs share does not snap,
one at Virtual-2's outer edge does; unplugged, the snapped window is the same half of
Virtual-1, plugged back Virtual-2's again.

Unit tests of switching and snapping: `test_switcher` (the task model's MRU order — seeded
from stacking, focus to the front, a new window at the back, the 750 ms focus-follows-mouse
debounce and an explicit focus that counts —, the switcher model's scopes, minimized windows,
modal dialogs, Alt+grave's application filter, the start selection, wrap, the user's bindings
before Tab/Shift+Tab/arrows/Home/End, Escape and Return, the open delay and a quick release,
a window going during the hold; the view's layout: minimum width, centred cells, rows, pages,
hit), `test_wm_geometry` (halves and quarters of odd sizes with and without a frame, a
minimum or maximum anchored at the outer corner, the tiled states, the whole Windows 11
transition table and its closure, edge zones with corners and with a neighbouring output,
drag-to-restore's point), `test_keybindings` (a hold: presses diverted, a modifier's press
and earlier keys' releases passed on, the primary modifier ending it, Super over Alt over
Ctrl over Shift; `peek_binding` naming a diverted key's action without disarming a tap;
`switcher.next` matching with no shell), `test_shell_bridge` (v7 in process: Alt+Tab is
`held_action switcher.next`, Tab and Shift+Tab arrive as `key` with their bindings, Alt's
release is `released`, the application saw only Alt's and Shift's events and no leave; the
shell ending the hold gives the keys back at once), `test_config` (the Part 4 default
bindings, a held action without a modifier refused).

```bash
# Notifications and the tray (ADR 011 Part 5, design-session-and-wm N, T.3)
sudo pacman -S --needed libnotify                                              # notify-send
./scripts/app-smoke.sh --join-seat-session --countdown 0 --only N                # ~60 s
./scripts/monitor-smoke.sh --join-seat-session --countdown 0 --only add,notify
```

Group N runs lumen-shell (its state — the next notification id and do-not-disturb — in the
run's directory through `LUMEN_SHELL_STATE`, never the user's), `dbus-monitor` on every
`org.freedesktop.Notifications` signal, `notify-send` (libnotify, the only package the tests
need), `gdbus` and `busctl` for protocol calls, and `lumen-probe sni`
(`tests/clients/lumen-probe-sni.cpp`: a StatusNotifierItem with a pixmap icon and a
com.canonical.dbusmenu menu on sd-bus, logging every call it gets): N1 `notify-send -t 1500`
expires with `NotificationClosed(id, 1)`, a critical one stays and moves the popups to the
overlay layer, `CloseNotification` closes it with 3 and an unknown id is an error; N2
`notify-send -r` gets the same id back and the card is replaced in place; N3 a click on an
action button (its rect from the shell's `notifications: card` debug line) gives
`ActivationToken`, `ActionInvoked`, `NotificationClosed(2)` in that order and notify-send
prints the action, a click on a card without a default action dismisses it; N4 a
notification while locked (`lumen-probe lock`) is queued, shown after the unlock, then
expires; N5 `lumen-probe capture` turns do-not-disturb on (normal → history, critical shown)
and off, a middle click on the bell sets and clears it; N6 the probe's item in the panel (its
orange disc counted in a screenshot of its cell), its tooltip, a left click
(`ProvideXdgActivationToken`, then `Activate` with that token), `busctl get-property` on the
watcher, a right click opening its menu as a grabbing popup, Down+Return clicking Open
(`Event(1, "clicked")`, then `"closed"`), the wheel (`Scroll(120, vertical)`); N7 the shell
killed and started again: the item registers with the new watcher within a second, ids
continue after the persisted block; N8 ten idle seconds with a history and a tray item: no
context switch of the shell. monitor-smoke's `notify` group (two outputs): with a window on
Virtual-2 focused the popups map on Virtual-2; Virtual-2 unplugged, they map on Virtual-1.
app-smoke's and monitor-smoke's lumen-shell keep their state in the run's directory
(`LUMEN_SHELL_STATE`).

Unit tests of notifications and the tray: `test_notifications` (ids and persisted blocks,
`replaces_id` in place and unknown, timeouts by urgency, hover and lock pauses, the lock
queue, close reasons and CloseNotification's refusal, do-not-disturb and critical, capture,
invoking actions and resident, senders leaving, the history bound and transient, the popup
stack and its timers, the rate limit; the markup subset with entities, links and their
schemes, unknown and unbalanced tags; image data: RGB, RGBA, padding, short data, formats,
sizes; wrapped bodies: width, truncation with "…", hard breaks, long words, CJK, spans and
hit testing; the popup layout; the D-Bus server over a socketpair: capabilities, server
information, Notify with hints and image data, ActivationToken → ActionInvoked →
NotificationClosed(2), CloseNotification and its error, replaces_id), `test_tray` (service
strings by name, path and `bus/path`; the registry; IconPixmap conversion and choice; menu
labels, mnemonics and layout; a GetLayout reply with a toggle, a separator, an invisible
item and a submenu; the watcher over a socketpair: RegisterStatusNotifierItem and its
signal, a duplicate, a path without a sender refused, the properties, and the host reading
the item's properties with GetAll from the other end), `test_config` (the notification
actions bind).

Unit tests of the session: `test_session_discovery` (the logind session order —
XDG_SESSION_ID, the process's session, then the display and active seat sessions only for a
process in no session; UID, seat and class checks; a seatless SSH session never takes the
console's), `test_health` (levels per part, process, memory and output, the worst-of rule, the
miss ring), `test_control` (io.lumen.Compositor over socketpairs: every method's reply,
GetHealth answered asynchronously and timing out to the host, the errors, IDL validation
before a handler, Subscribe and its overflow drop, a peer of another UID refused, the socket
file mode 0600, replaced when stale and removed at close) and `test_lumen_session`
(`tests/test_lumen_session.sh`: lumen-session against a fake systemctl — the imported set
and the denylist, exact restore after a clean and a crashed compositor, a value systemctl
escaped left alone, a second session and an inactive VT refused, `--cleanup`, the fallback
without a user manager and its autostart, the login-shell re-exec).

# Sanitizers
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

# Fuzzing (opt-in, clang only)
cmake -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DLUMEN_BUILD_FUZZERS=ON
cmake --build build/fuzz
./build/fuzz/fuzz_wayland_dispatch build/fuzz/corpus -max_total_time=600

The compositor's socket is its one untrusted input, so the request-dispatch path is what
gets fuzzed: raw bytes go down a real client connection and libwayland decodes them exactly
as it would in production, then Lumen's handlers run.

Always pass the corpus directory. The wire format rejects almost every random byte string
at the header, so an unseeded run never reaches argument unmarshalling — coverage sits
around 13 and stays there, versus ~108 from the seeds. See tests/fuzz/corpus/README.md.

`LUMEN_BUILD_FUZZERS=ON` instruments the whole build, not just the harness: without
coverage feedback into lx.compositor and lx.wayland.server, libFuzzer degenerates into
random input generation. The giveaway is a coverage count that never leaves single digits.

A crash here is a compositor any client can kill, so treat findings as release-blocking.
`ctest -R fuzz` runs a 10-second smoke test — enough to catch a harness that stopped
building or crashes immediately, not a substitute for a long out-of-band campaign.

All four configurations must be green. A sanitizer suite that starts red cannot gate
anything — the first real finding gets waved through as "the usual noise".

## Sanitizer suppressions

Rule: **suppressions are scoped to third-party shared objects only.** Never suppress by
symbol name. `race_top:pthread_mutex_destroy` would silence the driver, but Lumen's own
`std::mutex` lowers to exactly that symbol, so a real "destroyed while another thread held
it" bug would disappear with it.

Both suppression files apply to every test (`LUMEN_SANITIZER_ENV` in `CMakeLists.txt`).
That is safe precisely because they match on object name: a test that never loads a
graphics driver matches nothing, and a test that later grows a compositor gets the right
behavior without anyone having to remember to add it to a list.

Only the blunt instrument is opt-in — `LUMEN_SANITIZER_ENV_VULKAN_ICD`, which turns the
leak check off outright and is applied to just `test_dmabuf_composite` and
`test_kms_vulkan_scanout`.

### TSan — `tests/sanitizers/tsan.supp`

Mesa lavapipe (`libvulkan_lvp.so`, the software Vulkan ICD used on hosts without a GPU)
calls `pthread_mutex_destroy` / `pthread_cond_destroy` on primitives its own llvmpipe
worker threads are still locking and broadcasting on. Real race — Mesa's, in Mesa's
threads, on Mesa's objects.

Verified before suppressing: across 12 runs, every racing access frame resolved to
`libvulkan_lvp.so` (`#0` an intercepted libc call, `#1` the driver). Zero Lumen frames in
any racing access; Lumen appears only at `#2`+ in the thread-creation stack, because
`gfx::device_selector::select_best()` is what makes the driver spawn those threads.

`race:libvulkan_lvp.so` is the narrowest form that works. `called_from_lib:` does not
suppress these — the reports come from TSan's pthread interceptors rather than from
instrumented driver code (verified: 4/4 runs still failed).

Confirm the suppression is still doing exactly its job, and nothing more:

```
# Should print "Matched 4 suppressions" and pass
TSAN_OPTIONS="suppressions=$PWD/tests/sanitizers/tsan.supp:print_suppressions=1" \
    ./build/tsan/test_dmabuf_composite

# Should fail — proves the suppression is load-bearing, not decorative
TSAN_OPTIONS= ./build/tsan/test_dmabuf_composite
```

To check it has not started hiding Lumen races, add a deliberate one to the test (two
threads incrementing a `static long`, with a `std::this_thread::yield()` in the loop so
`RelWithDebInfo` cannot collapse it to a single store) and confirm TSan still reports it
with the suppression active.

### LSan — `tests/sanitizers/lsan.supp` for Mesa EGL/GL

Bringing up an EGL display allocates Mesa driver-global state that lives until process
exit, and creating a GL context allocates gallium state with the same lifetime. LSan
reports both at exit. These resolve to real shared objects (`libEGL_mesa.so`,
`libgallium-*.so`), so they get a proper object-scoped suppression. `libgallium` carries
its Mesa version in the filename, so that pattern is deliberately a prefix.

Any test that starts a compositor can reach this through
`compositor_impl::setup_gl_present`, which is the reason the file is applied suite-wide
rather than to a hand-maintained list of GL tests.

### LSan — `detect_leaks=0` for the Vulkan ICD

lavapipe leaks 224 bytes in 4 allocations of per-thread state, because the driver is
`dlclose`d while its worker threads are still running.

Unlike the EGL leaks above, this one cannot be suppressed. The ICD is unloaded before
LSan's exit check, so its frames
resolve to `<unknown module>` and no `leak:` pattern can match them — LSan matches on
symbolized text. Two rejected alternatives:

- `LD_PRELOAD` of the ICD keeps it mapped and does make the leak disappear, but it
  disappears with *or without* a suppression file, i.e. preloading changes the program
  rather than the report. It would also perturb driver selection on hosts with a real GPU.
- `__lsan_disable()` around device creation does not help: LSan's disable is per-thread,
  and the leaked allocations happen on the driver's own worker threads.

So these two tests run with `LSAN_OPTIONS=detect_leaks=0`. That disables the exit leak
check for them only — every other ASan check (use-after-free, overflow, out-of-bounds)
stays active, and leak detection stays on for all other tests. Reproduce the underlying
report with `LSAN_OPTIONS= ./build/asan/test_dmabuf_composite`.
