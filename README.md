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
allowed (`1.25`, `1.5`, `2`; rounded to n/120, the unit `wp_fractional_scale_v1` speaks),
and `--ping-timeout MS` — how long a client may leave an `xdg_wm_base.ping` unanswered
before it is shown as not responding (title suffix, dimmed window, the shell's flag; it is
never killed). Default 5000 (Mutter's); 0 disables pinging. `run-desktop.sh --scale S`
passes the scale on.

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
`zwp_input_method_manager_v2`, `zwp_virtual_keyboard_manager_v1`, and the private
`zlm_shell_v1` (v5; the shell only). Windows covered by an opaque window are not
composited, and their frame callbacks slow to one a second.

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
| Optional | `lx.portal` (Flatpak builds) |

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
| `LUMEN_BUILD_FLATPAK_PORTAL` | OFF | Flatpak portal adapter |

## systemd (user session)

Install unit files from `deploy/systemd/`. Socket: `$XDG_RUNTIME_DIR/lumen-0`.

## License

MIT
