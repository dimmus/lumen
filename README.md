# Lumen

Linux windowing system, desktop environment, and UI library — greenfield C++26, Clang, Wayland-only, Vulkan-first.

## Quick start (Linux)

```bash
./scripts/fetch-protocols.sh
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
or

```bash
# Configure + build (once)
cmake --preset debug          # or: cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset debug  # or: cmake --build build

# Run all tests
ctest --preset release        # if you used a preset
# or, for a plain build dir:
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}"
ctest --test-dir build --output-on-failure

# One test
ctest --test-dir build -R test_protocol_bind --output-on-failure

# Direct binary
./build/test_kms_device

# Run a session on the console: compositor + lumen-shell panel + foot. Leave with the panel's
# "Log out" button (click, then "Confirm?") or Ctrl+Alt+Backspace; Ctrl+Alt+F<n> switches VTs
./scripts/run-desktop.sh                       # from a text console (tty)
./scripts/run-desktop.sh --join-seat-session   # over SSH; see --help for -t, --decorations, -- CMD

# Opt-in vkms KMS test
LUMEN_TEST_VKMS=1 ctest --test-dir build -R test_kms_vkms.

# Sanitizer suites
cmake --preset asan && cmake --build --preset asan && ctest --preset asan (same for tsan)
```

## Running the desktop

From the active console session (or from SSH inside the seat session's cgroup, so logind
lets libinput open devices — see `scripts/drag-smoke.sh --join-seat-session`):

```bash
./build/release/lumen-compositor lumen-0 &           # socket $XDG_RUNTIME_DIR/lumen-0
./build/release/lumen-shell --display lumen-0 &      # the top panel / task list
WAYLAND_DISPLAY=lumen-0 foot &                       # any Wayland application
```

`lumen-shell` is privileged: the compositor accepts its `zlm_shell_v1` bind only from the
configured shell binary — by default the `lumen-shell` next to `lumen-compositor`, or
`lumen-compositor --shell PATH`. A copy elsewhere is refused (it exits 1 and the
compositor logs both paths). The shell exits 0 on SIGTERM and 1 when refused or when the
compositor goes away; `lumen-shell.service` restarts it. `LUMEN_LOG=debug` on either
process shows the bridge traffic (`compositor.shell`, `shell.bridge`, `shell.panel`,
`shell.launcher`, `shell.apps`, `shell.idle`).

Key bindings:

| Keys | Action | Owner |
|------|--------|-------|
| Ctrl+Alt+F1…F12 | Switch virtual terminal (logind `Seat.SwitchTo`); the session pauses while another VT is in front and resumes with a full modeset when switched back | compositor (always, also with no shell) |
| Ctrl+Alt+Backspace | Log out: every window is asked to close, then the compositor stops | compositor (always) |
| Super+left drag / Super+right drag | Move / resize the window under the pointer | compositor (placeholder) |
| Super (tap) | Open / close the launcher (also the panel's Apps button). Super pressed and released alone; the key still reaches the focused window | shell (key grab, zlm_shell_v1 v4) |
| Super+Return | Start a terminal (`$TERMINAL`, `xdg-terminal-exec`, or the first of foot, Alacritty, kitty, … installed) | shell (key grab) |
| Super+Escape | Turn the focused window's shortcuts inhibitor off (and on again) and release its pointer lock or confinement: a VM viewer, remote desktop or game that took the desktop's shortcuts or the pointer gives them back | compositor (only when that window inhibits or constrains the pointer; never inhibitable) |

A window that inhibits shortcuts (keyboard-shortcuts-inhibit: virt-viewer, remote desktops)
receives Super, Super+Return and the other shell keys while it has the keyboard; VT switching,
Ctrl+Alt+Backspace and Super+Escape stay the compositor's, and the shell's lock key
(Super+L) is never inhibited.

The launcher lists the applications installed as XDG desktop entries
(`$XDG_DATA_HOME/applications`, then each `$XDG_DATA_DIRS`'s, honouring `NoDisplay`,
`Hidden`, `OnlyShowIn`/`NotShowIn` and `TryExec`): type to filter, arrows or Tab to choose,
Return or a click to start, Escape or a click elsewhere to close. Every application it
starts runs as its own systemd user service, `app-lumen-<id>@<random>.service` in
`app.slice` — detached from the shell and the terminal you started the session from, so it
outlives both — with its output in the journal:
`journalctl --user -u 'app-lumen-*'`. With systemd older than 250 (or
`LUMEN_SHELL_LAUNCH=scope`) it runs in its own transient scope,
`app-lumen-<id>-<random>.scope`, instead. Without systemd --user (or with
`LUMEN_SHELL_LAUNCH=fork`) it is started by double fork + `setsid`.

`lumen-compositor` options: `[socket-name]`, `-f FPS` (frame clock without a display mode),
`--decorations=server|client`, `--shell PATH`, `--scale S` — the output scale, fractional
allowed (`1.25`, `1.5`, `2`; rounded to n/120, the unit `wp_fractional_scale_v1` speaks;
default: 2 on a HiDPI panel, else 1), `--output-scale NAME=S` — one output's scale
(repeatable, e.g. `--output-scale DP-2=1.5`), `--virtual-outputs WxH[@S],…` — virtual
outputs instead of the display (tests; no card is opened), and `--ping-timeout MS` — how
long a client may leave an `xdg_wm_base.ping` unanswered before it is shown as not
responding (title suffix, dimmed window, the shell's flag; it is never killed). Default
5000 (Mutter's); 0 disables pinging. `run-desktop.sh --scale S` passes the scale on.

Every connected monitor is used (ADR 008): each is composited and flipped on its own, laid
out left to right (a VM host's arrangement on VMware/virtio), with its own `wl_output`,
scale, panel and frame clock. New windows open on the monitor under the pointer. Monitors
may come and go while Lumen runs (udev hotplug): a new one is lit and laid out, a resized VM
window re-modesets its monitor, an unplugged one's windows move to another and come back
when it returns — also across VT switches, display power-off and the session lock.
`scripts/monitor-smoke.sh --join-seat-session` checks all of this on a VMware guest, whose
virtual monitors `build/release/lumen-vmw-layout` (a test tool) adds, resizes and removes.

Monitors are configured with any `wlr-output-management` client — `wlr-randr` (mode, custom
mode, position, scale, transform, adaptive sync, on/off; `--dryrun` asks the kernel without
changing anything) or `kanshi` (profiles per set of monitors, applied on hotplug); any
same-user client that is not a Flatpak may. What a client applies is remembered in
`$XDG_CONFIG_HOME/lumen/outputs.toml` — per monitor (mode, scale, transform, adaptive sync)
and per set of monitors connected together (enabled, position) — and restored at the next
start and whenever that set is connected again, before the first modeset. The file can be
edited while Lumen is not running; `LUMEN_OUTPUT_CONFIG=PATH` uses another one,
`LUMEN_OUTPUT_CONFIG=0` none.

At a fractional scale, clients that speak `wp_fractional_scale_v1` + `wp_viewporter`
(foot, GTK 4, Qt 6, Chromium, Firefox, lumen-shell) render at the output's pixel density
and are drawn 1:1, pixel-exact; others render at the integer ceiling and are scaled down.
`scripts/scale-smoke.sh --join-seat-session` checks this on the console at 1.25, 1.5, 1.75
and 2 from screenshots of the composited frame. Debug aid: `kill -USR1 <lumen-compositor>`
writes the next fully repainted frame as a PPM to `$LUMEN_SCREENSHOT_DIR` (default
`$XDG_RUNTIME_DIR`), GL path.

Protocols served: `wl_compositor` (v6), `wl_subcompositor`, `wl_seat` (v9), `wl_output`
(v4), `wl_data_device_manager`, `xdg_wm_base` (v6), `zxdg_decoration_manager_v1`,
`zwlr_layer_shell_v1` (v5), `wp_cursor_shape_manager_v1`, `zwp_linux_dmabuf_v1` (v5),
`zxdg_output_manager_v1` (v3), `zwp_primary_selection_device_manager_v1`,
`xdg_activation_v1`, `wp_viewporter`, `wp_fractional_scale_manager_v1`, `wp_presentation`
(v2: CLOCK_MONOTONIC, the KMS flip's time and vblank counter per commit, or `discarded`),
`ext_idle_notifier_v1` (v2), `zwp_idle_inhibit_manager_v1`, `zwlr_output_power_manager_v1`,
`ext_session_lock_manager_v1`, `zwp_text_input_manager_v3` (v2),
`zwp_input_method_manager_v2`, `zwp_virtual_keyboard_manager_v1`, `wp_fifo_manager_v1`,
`wp_commit_timing_manager_v1`, `wp_tearing_control_manager_v1`, `wp_content_type_manager_v1`,
`wp_single_pixel_buffer_manager_v1`, `wp_alpha_modifier_v1`,
`wp_linux_drm_syncobj_manager_v1` (only where the card has timeline syncobjs),
`xdg_wm_dialog_v1`, `xdg_toplevel_icon_manager_v1`, `xdg_system_bell_v1`,
`zxdg_exporter_v2`/`zxdg_importer_v2` and v1, `zwp_keyboard_shortcuts_inhibit_manager_v1`,
`ext_data_control_manager_v1`, `zwlr_data_control_manager_v1` (v2),
`wp_security_context_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_relative_pointer_manager_v1`,
`zwp_pointer_gestures_v1` (v3), `ext_foreign_toplevel_list_v1`,
`ext_output_image_capture_source_manager_v1`, `ext_foreign_toplevel_image_capture_source_manager_v1`
(GL and CPU backends), `ext_image_copy_capture_manager_v1`, `zwlr_screencopy_manager_v1` (v3), and
the private `zlm_shell_v1` (v6; the shell only). Windows covered by an opaque window are not
composited, and their frame callbacks slow to one a second (a window being captured keeps the
outputs' pace).

Screen capture (ADR 009 Part 4): grim (`-o` an output, `-T` a window, also a covered one),
wf-recorder and xdg-desktop-portal-wlr work; frames carry only what changed and are copied
right after the composite — never a capture of its own — so an idle screencast costs nothing.
While the session is locked a capture shows black; the panel shows a red dot while anything
but the shell captures. Sandboxed clients see none of the capture globals.

Portals (ADR 009 Part 5): browsers, OBS and screenshot tools go through xdg-desktop-portal.
ScreenCast is xdg-desktop-portal-wlr's (PipeWire over ext-image-copy-capture); Screenshot
(interactive area/screen/window choice, `$XDG_PICTURES_DIR/Screenshots`), PickColor and the
cast's source chooser are `xdg-desktop-portal-lumen`'s. `run-desktop.sh` exports the session
to the systemd user manager, starts both backends from the build and WirePlumber if needed,
restarts the frontend, and undoes it all at logout (`--no-portals` skips it); see
docs/architecture.md § Portals. Runtime: xdg-desktop-portal, xdg-desktop-portal-wlr,
xdg-desktop-portal-gtk, PipeWire and WirePlumber; build: libsystemd, libpng (to save files).
`scripts/app-smoke.sh --only Z` drives it with `lumen-portal-probe`.

Commits are content updates in a per-surface queue (ADR 009): an update whose buffer is not
ready (explicit-sync acquire point, or a GL client's implicit fences), whose fifo barrier is
set, or whose commit-timing target is ahead waits there — never in the composite — and
applies in order when it can. Vulkan FIFO swapchains (Mesa) get one frame per refresh, and
timed frames are never shown early. `scripts/app-smoke.sh --only T` checks this with vkcube,
weston-simple-timing, weston-simple-egl and `lumen-probe timing|solid|tearing`.

Windows and the clipboard (ADR 009 Part 2): modal dialogs (GTK 4, Qt 6) block their parent —
it stays visible, a click on it focuses the dialog — and every dialog opens centred on its
parent; window icons and the system bell reach the panel (the bell flashes the window), the
icons drawn from the XDG icon theme (PNG through libpng, SVG through librsvg, both optional
build dependencies) or the window's own pixels; clipboard tools (`wl-copy`/`wl-paste`,
`cliphist` with `wl-paste --watch cliphist store`, clipboard managers) work through
data control; Flatpak apps are recognised by their security context and see none of the
session-wide globals (data control, layer shell, lock, output management, …).
`scripts/app-smoke.sh --only U,V` checks these on the console.

Screen blanking: `lumen-shell` powers the outputs off after 10 minutes without input
(`--blank-after S`, 0 never; idle inhibitors such as a playing video hold it off while their
window is visible). Any input wakes them — the key or click that wakes the screen reaches no
application. The compositor itself has no timeouts; to let another idle daemon decide:

```bash
./build/release/lumen-shell --display lumen-0 --idle-policy=none &
WAYLAND_DISPLAY=lumen-0 swayidle -w timeout 600 'wlopm --off "*"'   # or any zwlr_output_power client
```

Screen locking: any `ext-session-lock-v1` locker works (swaylock, gtklock, hyprlock, …); Lumen
ships none. `lumen-shell --lock-command 'swaylock -f'` makes the shell start it on Super+L,
on `loginctl lock-session`, before the system sleeps and before blanking (lock first, then
blank; `--no-lock-on-blank` blanks only), and again if it exits while locked; a locked
screen blanks after `--blank-locked-after S` (60). `run-desktop.sh --lock-command CMD`
passes it on. The session is not shown until the locker unlocks: a locker that crashes
leaves it locked on dark red until a new one takes over or `loginctl unlock-session`.
With an external daemon instead:

```bash
WAYLAND_DISPLAY=lumen-0 swayidle -w timeout 300 'swaylock -f' timeout 600 'wlopm --off "*"' \
    before-sleep 'swaylock -f' lock 'swaylock -f'
```

Input methods: fcitx5 and ibus (their Wayland frontends speak input-method-v2 with
virtual-keyboard-v1) work as ordinary clients — start one in the session
(`WAYLAND_DISPLAY=lumen-0 fcitx5 -d`). GTK 4, foot and other text-input-v3 clients get its
preedit, commits and candidate window (placed at their cursor); `GTK_IM_MODULE` should stay
unset. Text inputs are entered only while an input method runs, so without one clients keep
their own compose handling.

## Architecture

**Full architecture guide:** [docs/architecture.md](docs/architecture.md) — processes, module layers, workflows, protocols.

**AI agent instructions:** [AGENTS.md](AGENTS.md) — coding principles, decision framework, anti-patterns.

| Layer | Modules |
|-------|---------|
| Foundation | `lx.foundation`, `lx.sync`, `lx.trace` |
| Runtime | `lx.runtime`, `lx.runtime.executor`, `lx.scheduler` |
| Platform | `lx.wayland.*`, `lx.input`, `lx.drm`, `lx.session` |
| Graphics | `lx.gfx`, `lx.text`, `lx.layout`, `lx.scene` |
| Desktop | `lx.compositor`, `lx.compositor.*`, `lx.shell`, `lx.shell.*`, `lx.ui` |
| Apps | `lx.app`, `lx.ui.builder` |
| Desktop (portal) | `lx.portal` (xdg-desktop-portal-lumen) |

- **Multi-process:** compositor, shell, apps
- **Stacking-first WM** with optional tiling plugin (`PolicyRegistry`)
- **State sync:** compositor → shell snapshot + delta (`zlm_policy_bridge_v1`)
- **First-class threading:** `lx.sync`, `lx.runtime.executor`, immutable double-buffered frames ([docs/subsystems/threading.md](docs/subsystems/threading.md))
- **Errors & logging:** `lx.foundation.error`, `lx.trace` ([docs/subsystems/errors-and-logging.md](docs/subsystems/errors-and-logging.md))
- **Zero-copy:** dmabuf → Vulkan import ([docs/subsystems/dmabuf-vulkan-import.md](docs/subsystems/dmabuf-vulkan-import.md))
- **Performance pipeline:** snapshot v2, scanout, import cache, budgets ([docs/subsystems/rendering-performance.md](docs/subsystems/rendering-performance.md))

UML index: [docs/uml/README.md](docs/uml/README.md)

**Public API reference:** [docs/api/README.md](docs/api/README.md)

## Build options

| Option | Default | Description |
|--------|---------|-------------|
| `LUMEN_PROTOCOLS_P1` | ON | Generate P1 upstream protocols |
| `LUMEN_PROTOCOLS_P2` | OFF | Generate P2 upstream protocols |
| `LUMEN_ENABLE_TILING_PLUGIN` | ON | Tiling plugin compile flag |
| `LUMEN_VULKAN_VALIDATION` | OFF | Vulkan validation layers |
| `LUMEN_BUILD_PORTAL` | ON | xdg-desktop-portal-lumen (needs libsystemd and wayland-client) |

## systemd (user session)

Install unit files from `deploy/systemd/`. Socket: `$XDG_RUNTIME_DIR/lumen-0`.
`cmake --install` also installs the portal backend (libexec), `lumen.portal`,
`lumen-portals.conf`, its D-Bus service and user unit, and xdg-desktop-portal-wlr's `Lumen`
config.

## License

MIT
