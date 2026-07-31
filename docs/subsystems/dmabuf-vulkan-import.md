> **Status:** Current (v0.3) — see code for implementation truth

# Subsystem: dmabuf → Vulkan Import Pipeline

Zero-copy client buffer compositing for the Lumen compositor (P0 hot path).

## Overview

Applications submit frames via `linux-dmabuf-v1` with optional explicit sync
(`linux-drm-syncobj-v1`). The compositor imports DMA-BUF file descriptors into
Vulkan images without copying pixel data, composites into the scene graph, and
presents to KMS.

## Components

| Component | Module | Responsibility |
|-----------|--------|----------------|
| `CompositorSurfaceManager` | `lx.compositor` | Commit handling, desc extraction |
| `DmabufImporter` | `lx.gfx` | Vulkan external memory import |
| `SyncobjBridge` | `lx.gfx` | DRM syncobj ↔ Vulkan timeline semaphore |
| `DmabufImportCache` | `lx.gfx.import_cache` | Reuse imports keyed by client/buffer/modifier |
| `BufferLifecycleTracker` | `lx.compositor.buffer_lifecycle` | import → release → wl_buffer.release |
| `SurfaceNode` | `lx.scene` | Retained client surface in scene graph |
| `DeviceSelector` | `lx.gfx` | Discrete vs integrated import caps |

## Data flow

See UML:

- Class: [../uml/19-dmabuf-vulkan-class.mmd](../uml/19-dmabuf-vulkan-class.mmd)
- Sequence: [../uml/20-dmabuf-vulkan-sequence.mmd](../uml/20-dmabuf-vulkan-sequence.mmd)

## DmabufDesc (canonical internal form)

Extracted once per `wl_buffer` from Wayland plane metadata:

```cpp
struct dmabuf_plane {
    int     fd;       // dup'd for importer lifetime
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
};

struct dmabuf_desc {
    uint32_t width, height;
    uint32_t format;      // DRM fourcc
    uint8_t  plane_count;
    dmabuf_plane planes[4];
};
```

## Import algorithm (Vulkan)

1. **Capability probe** at startup (`vkGetPhysicalDeviceExternalBufferProperties`).
2. **Modifier check** against compositor allow-list (from dmabuf feedback).
3. **Import** via `VkExternalMemoryImageCreateInfo` + `VkImportMemoryFdInfoKHR`.
4. **Layout transition** `UNDEFINED → SHADER_READ_ONLY_OPTIMAL` on first use.
5. **Sync** — wait client timeline point before sampling; signal before release.
6. **Release** — `vkDestroyImage`, close fd, send `wl_buffer.release`.

## GPU tier behavior

| Tier | Import path | Present |
|------|-------------|---------|
| Discrete | Prefer optimal tiling, full modifier set | Mailbox |
| Integrated | Linear/optimal based on scanout caps | FIFO relaxed |
| Embedded | Single-plane RGB formats only; strict allow-list | Direct scanout when possible |

## Error handling

| Condition | Action |
|-----------|--------|
| Unsupported modifier | Reject commit; send protocol error to client |
| Import fails | Fallback: SHM buffer copy path (dev only) |
| Sync timeout | Drop frame; log; don't stall compositor |
| Client disconnect | Release all imported images for client |

## Implementation phases

| Phase | Deliverable |
|-------|-------------|
| D0 | `DmabufDesc` extraction from `zwp_linux_buffer_params_v1` |
| D1 | Basic import (single-plane ARGB8888) |
| D2 | Multi-plane + modifiers + feedback protocol |
| D3 | drm-syncobj timeline integration |
| D4 | Direct scanout optimization (integrated) |

## Related docs

- [rendering-performance.md](rendering-performance.md) — scanout, snapshot v2, budgets
- [../uml/19-dmabuf-vulkan-class.mmd](../uml/19-dmabuf-vulkan-class.mmd)

## Related protocols

- `linux-dmabuf-v1` (P0) — buffer submission
- `linux-drm-syncobj-v1` (P0) — explicit sync
- `wp_presentation` (P1) — frame timing feedback

## Code stubs

- `modules/lx.gfx/dmabuf_import.cppm` — importer interface
- `modules/lx.gfx/syncobj_bridge.cppm` — drm-syncobj ↔ Vulkan timeline semaphore bridge
- `modules/lx.gfx/semaphore_pool.cppm` — reusable timeline semaphores
- `modules/lx.gfx/import_cache.cppm` — import reuse cache

Direct scanout (plane assignment, overlay stacks) is **not implemented**: the compositor
always takes the Vulkan composite path.
