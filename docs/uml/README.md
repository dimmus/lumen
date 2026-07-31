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
| [13-state-machines.mmd](13-state-machines.mmd) | State | Surface, Seat, Widget, ZlmToplevel |
| [14-fluent-builder.mmd](14-fluent-builder.mmd) | Class | Fluent builder API |
| [15-dual-gpu.mmd](15-dual-gpu.mmd) | Class | Discrete vs integrated render paths |
| [16-security.mmd](16-security.mmd) | Component | Unix creds + optional Flatpak |
| [17-systemd-deployment.mmd](17-systemd-deployment.mmd) | Deployment | systemd user session units |
| [18-focus-sequence.mmd](18-focus-sequence.mmd) | Sequence | Stacking focus change |
| [19-dmabuf-vulkan-class.mmd](19-dmabuf-vulkan-class.mmd) | Class | dmabuf → Vulkan import |
| [20-dmabuf-vulkan-sequence.mmd](20-dmabuf-vulkan-sequence.mmd) | Sequence | Frame commit import path |
| [21-shell-state-sync.mmd](21-shell-state-sync.mmd) | Sequence | Compositor → shell snapshot |
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
| [38-health-evaluation.mmd](38-health-evaluation.mmd) | Activity | Health sample → report |

**Narrative architecture doc:** [../architecture.md](../architecture.md)

Render with any Mermaid-compatible viewer (GitHub, VS Code Mermaid extension, mermaid.live).
