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

# Real applications, end to end, unattended (~2 min; hands off, as drag-smoke)
./scripts/app-smoke.sh --join-seat-session [--only A,B,C,D,E,X] [--skip-gtk]

Same machinery as drag-smoke (seat session, injector, foreign-input watch, SIGTERM-only
cleanup of the PIDs it started, exit codes 0/1/2/3, logs in app-smoke-logs/), with real
clients. Judged from the compositor log, chiefly the surface tree's
`compositor.scene: tree root=… nodes=<sid>[*]@x,y,wxh;…` dumps, which list every drawn
surface of a window in paint order at each change, and from the clients' side too: the
probes' and lumen-square's stdout, `WAYLAND_DEBUG=client` traces of foot (A, C) and
gtk4-widget-factory (B6/B7), and lumen-shell's own log (E). Group A (subsurfaces):
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

Whole run, both sides of the wire: exactly two protocol errors happen, the two provoked (D7's
probe and E6's copy): libwayland-server's `error in client communication (pid N)` appears in
the compositor log for those two PIDs only, each of them saw its error, and no other client
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
that is not the configured shell is the display's `implementation` error; and out-of-order
client disconnects must leave no server client slot behind — they used to corrupt a
client's destroy list and could spin the compositor forever), positioner placement and the popup
grab/dismissal state machine by `test_positioner` (pure), and layer arrangement (every
anchor combination, margins, zones, the two passes, exclusive-edge deduction, closing) and
the layer shell's configure/map/keyboard state machine by `test_layer_arrange` (pure).

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
