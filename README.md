# Lumen

A fast, modern Wayland desktop — compositor, shell and UI library — written from scratch in
C++26.

![Lumen: fastfetch and btop snapped to quarters, a notification, the panel](docs/images/desktop.png)

## Why Lumen

- **Fast where you feel it.** On the same real machine as weston and sway (a Broadwell
  notebook, hardware Vulkan): the socket is up in 370 ms (weston 9 s, sway 10.4 s), a
  client's commit reaches the screen in 9.5 ms at the median (weston 22, sway 33), and a
  vsync'd client rides 60.00 fps with a 0.00 ms frame-interval spread. An idle desktop
  costs no measurable CPU: the frame clock stops.
  [Measurements](docs/subsystems/rendering-performance.md#62-start-up)
- **Zero copy.** dma-buf clients are composited where they live (~0.2 ms of render thread
  per frame; a moving 64×64 cursor frame costs 0.09 ms of GPU), with explicit sync, a
  per-surface commit queue (FIFO, commit timing) and presentation feedback that carries
  the real flip time.
- **Never does work twice.** Damage-limited compositing, occlusion culling, covered windows
  throttled to one frame callback a second, screen capture copied from the composite it
  already drew, so an idle screencast is free.
- **Runs on what you have.** Vulkan first, GLES where Mesa has no Vulkan driver (VMs, older
  GPUs), a CPU compositor where the only GPU is a software one — picked automatically. The
  display card is found from sysfs, not assumed.
- **Hard to break.** Compositor and shell are separate processes: window management, key
  bindings and workspaces keep working while the shell restarts, and a hung compositor is
  caught by the systemd watchdog.
- **Secure by default.** The privileged shell protocol binds only from the configured shell
  binary; Flatpak apps see no capture, clipboard-control, layer-shell or output-management
  globals.
- **A UI engine of its own.** The panel, launcher, Alt+Tab switcher, notifications, tray
  menus, tooltips and the portal's dialogs are drawn by Lumen's own declarative,
  retained-mode UI engine — descriptors reconciled onto a node tree, damage-scoped GPU
  primitives, text shaped off-thread, themes compiled at load, client-side key repeat —
  no widget toolkit, no shared-memory canvases. An idle shell makes zero context switches;
  a panel redraw costs ~250 µs of CPU. [The engine, measured](docs/subsystems/ui-engine.md)
- **A complete desktop.** Panel, launcher, Alt+Tab, dynamic workspaces, edge snapping and
  optional master-stack tiling per workspace, notifications, system tray, idle and lock,
  input methods (fcitx5, ibus) and portals for screenshots, screencasts and colour picking.
- **Real monitors.** Hotplug, pixel-exact fractional scaling, adaptive sync, VT switching,
  wlr-randr and kanshi, setups remembered per set of monitors.
- **About 50 Wayland protocols**, from xdg-shell to linux-drm-syncobj, ext-image-copy-capture
  and ext-session-lock.
- **Easy to operate.** One `config.toml`, applied as you save it and validated with
  suggestions; `lumen-ctl` for health, outputs, windows and every action.

## Quick start

```bash
./scripts/install-deps.sh            # Arch, Debian testing, Ubuntu 26.04+, Fedora 44+, openSUSE Tumbleweed
cmake --preset release && cmake --build --preset release
./scripts/run-desktop.sh             # from a text console; over SSH add --join-seat-session
```

Log out with the panel's **Log out** or Ctrl+Alt+Backspace. Super opens the launcher,
Super+Return a terminal.

## Documentation

- [User guide](docs/user-guide.md) — building and testing, key bindings, the shell, monitors,
  protocols, login session, configuration, `lumen-ctl`
- [Architecture](docs/architecture.md) — processes, module layers, frame pipeline
- [The UI engine](docs/subsystems/ui-engine.md) — `lx.ui` and `lx.ui.host` as built, measured,
  and compared
- [API reference](docs/api/README.md) · [ADRs](docs/adr) · [Agent rules](AGENTS.md)

## License

MIT
