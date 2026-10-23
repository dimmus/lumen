> **Status:** Current (v0.3) — see code for implementation truth

# Lumen Architecture — UML 2 Diagrams

Codename for the Linux windowing system, desktop environment, and UI library stack.

## Decisions

| Area | Choice |
|------|--------|
| Language | C++26, Clang |
| EFL | Greenfield, EFL-inspired layering |
| Display | Wayland-only |
| Rendering | Vulkan-first (discrete + integrated) |
| Processes | Multi-process (compositor, shell, apps) |
| UI | Scene graph core + fluent builder widgets + compiled themes |
| WM | Stacking-first, optional tiling plugin |
| Security | Unix permissions default, optional Flatpak portal |

## Diagram index

| File | Type | Description |
|------|------|-------------|
| [01-package-layers.mmd](01-package-layers.mmd) | Package | Layered module dependencies |
| [02-component-deployment.mmd](02-component-deployment.mmd) | Component | Processes and kernel interfaces |
| [03-runtime-events.mmd](03-runtime-events.mmd) | Class | Event loop and event bus |
| [04-scene-graph.mmd](04-scene-graph.mmd) | Class | Scene graph and Vulkan draw path |
| [05-layout.mmd](05-layout.mmd) | Class | Incremental layout engine |
| [06-ui-toolkit.mmd](06-ui-toolkit.mmd) | Class | Widgets and compiled themes |
| [07-compositor-shell.mmd](07-compositor-shell.mmd) | Class | Compositor vs shell policy split |
| [08-frame-sequence.mmd](08-frame-sequence.mmd) | Sequence | One-frame hot path |
| [09-wayland-protocols.mmd](09-wayland-protocols.mmd) | Package | Protocol layer stack |
| [10-protocol-dispatch.mmd](10-protocol-dispatch.mmd) | Class | libwayland-backed Wayland server seam |
| [11-lumen-private-protocols.mmd](11-lumen-private-protocols.mmd) | Class | zlm_* DE protocols |
| [12-window-management.mmd](12-window-management.mmd) | Class | Stacking-first + tiling plugin |
| [13-state-machines.mmd](13-state-machines.mmd) | State | Surface, Seat, Widget, ZlmToplevel, Session pause/resume, Display power, Session lock |
| [14-fluent-builder.mmd](14-fluent-builder.mmd) | Class | Fluent builder API |
| [15-dual-gpu.mmd](15-dual-gpu.mmd) | Class | Discrete vs integrated render paths |
| [16-security.mmd](16-security.mmd) | Component | Unix creds; sandboxed clients (security-context listeners, the global filter); capture for sandboxed apps only through the portals |
| [17-systemd-deployment.mmd](17-systemd-deployment.mmd) | Deployment | The login session: lumen-session, the compositor bound to graphical-session.target, the shell supervised by systemd, XDG autostart, the control socket and lumen-ctl, each launched app a transient service, the portals with the exported session environment (ADR 010) |
| [18-focus-sequence.mmd](18-focus-sequence.mmd) | Sequence | Stacking focus change |
| [19-dmabuf-vulkan-class.mmd](19-dmabuf-vulkan-class.mmd) | Class | dmabuf → Vulkan import |
| [20-dmabuf-vulkan-sequence.mmd](20-dmabuf-vulkan-sequence.mmd) | Sequence | Frame commit import path |
| [21-shell-state-sync.mmd](21-shell-state-sync.mmd) | Sequence | Compositor → shell snapshot, key grabs, session lock (v5), icons, parents, bell, capture state (v6) |
| [22-thread-model.mmd](22-thread-model.mmd) | Component | UI / render / worker threads |
| [23-frame-handoff-sequence.mmd](23-frame-handoff-sequence.mmd) | Sequence | Immutable snapshot handoff |
| [24-executor-class.mmd](24-executor-class.mmd) | Class | executor, strand, snapshot_buffer |
| [25-compositor-vsync-sequence.mmd](25-compositor-vsync-sequence.mmd) | Sequence | Compositor vsync → UI → render |
| [26-scanout-decision.mmd](26-scanout-decision.mmd) | Activity | Direct scanout decision tree |
| [27-snapshot-v2-handoff.mmd](27-snapshot-v2-handoff.mmd) | Sequence | Snapshot buffer v2 — zero-copy handoff |
| [28-import-cache-sequence.mmd](28-import-cache-sequence.mmd) | Sequence | dmabuf import cache hit/miss |
| [29-present-feedback-sequence.mmd](29-present-feedback-sequence.mmd) | Sequence | DRM page-flip + presentation feedback |
| [30-backpressure-policy.mmd](30-backpressure-policy.mmd) | State | Snapshot back-pressure (drop/stall/triple-buffer) |
| [31-hybrid-scanout-overlays.mmd](31-hybrid-scanout-overlays.mmd) | Activity | Hybrid scanout + overlay plane stack |
| [32-kms-atomic-damage.mmd](32-kms-atomic-damage.mmd) | Sequence | KMS atomic commit + damage blob |
| [33-gpu-resource-pools.mmd](33-gpu-resource-pools.mmd) | Class | Pipeline cache + timeline semaphore pool |
| [34-memory-pressure-response.mmd](34-memory-pressure-response.mmd) | Activity | Memory pressure fan-out |
| [35-buffer-lifecycle.mmd](35-buffer-lifecycle.mmd) | State | Buffer import → release loop |
| [36-gpu-memory-budget.mmd](36-gpu-memory-budget.mmd) | Activity | Per-client GPU import limits |
| [37-overflow-policies.mmd](37-overflow-policies.mmd) | Activity | Fixed-cap overflow behavior |
| [38-health-evaluation.mmd](38-health-evaluation.mmd) | Activity | Health, pulled: a request → UI and render facts → evaluate → report (ADR 010 S8) |
| [39-hotplug-sequence.mmd](39-hotplug-sequence.mmd) | Sequence | Hotplug: uevent → worker probe → diff → output ops (add, test/switch mode, retire) → clients (ADR 008 Part B) |
| [40-output-config-sequence.mmd](40-output-config-sequence.mmd) | Sequence | Output configuration: a wlr-output-management apply/test → protocol checks → whole-configuration TEST_ONLY → switch → combined modeset → succeeded/failed (revert) → outputs.toml on the worker |
| [41-content-update-queue.mmd](41-content-update-queue.mmd) | Sequence | Content-update queue: a commit's blockers (fences, fifo barrier, commit-timing target), the readiness poller, the tick's flush points, release points (ADR 009 Part A) |
| [42-capture-sequence.mmd](42-capture-sequence.mmd) | Sequence | Screen capture: a session, the job (curtain, held, output copy), the copy on render after the composite (dma-buf blit + fence to the poller, shm blit + worker read), the lock epoch, `ready` (ADR 009 Part 4) |
| [43-session-startup-sequence.mmd](43-session-startup-sequence.mmd) | Sequence | Session start-up: lumen-session, the environment import, the compositor in user@.service (logind control, socket, environment export, READY), the shell and autostart, the end (ADR 010) |
| [44-workspace-model.mmd](44-workspace-model.mmd) | Sequence | Workspaces: a binding switches, visibility and focus follow, ext-workspace's diff and done, zlm v7 toplevel_workspace, the shell's indicator activating, hotplug (ADR 010 Part 3) |
| [45-held-action-sequence.mmd](45-held-action-sequence.mmd) | Sequence | The window switcher's held action: Alt+Tab gives the shell a hold, presses go to it while Alt is down (the application keeps the keyboard), a quick release switches without drawing, Escape / lock cancel |
| [46-notifications-tray-sequence.mmd](46-notifications-tray-sequence.mmd) | Sequence | Notifications and the tray: names on the user bus, Notify → a popup (or the lock queue, or the history under do-not-disturb), expiry and an action with its activation token, an item registering, its icon, Activate and its dbusmenu menu |

**Narrative architecture doc:** [../architecture.md](../architecture.md)

Render with any Mermaid-compatible viewer (GitHub, VS Code Mermaid extension, mermaid.live).
