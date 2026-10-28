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
| `drm_timeline_table` | `lx.gfx.drm_timeline` | linux-drm-syncobj-v1 timelines: acquire-point queries and eventfds, release points (now, or after a sync_file) |
| `content_queue`, `readiness_poller` | `lx.compositor.content_update` | Holds a commit until its buffer is ready (acquire point signalled, or implicit write fences done) |
| `DmabufImportCache` | `lx.gfx.import_cache` | Reuse imports keyed by client/buffer/modifier |
| `BufferLifecycleTracker` | `lx.compositor.buffer_lifecycle` | import → release → wl_buffer.release |
| `SurfaceNode` | `lx.scene` | Retained client surface in scene graph |
| `DeviceSelector` | `lx.gfx` | Discrete vs integrated import caps |

## Data flow

See UML:

- Class: [../uml/19-dmabuf-vulkan-class.mmd](../uml/19-dmabuf-vulkan-class.mmd)
- Sequence: [../uml/20-dmabuf-vulkan-sequence.mmd](../uml/20-dmabuf-vulkan-sequence.mmd)

## Readiness before apply (ADR 009)

A dma-buf content update is applied only once its buffer is ready, so the composite never
samples (or waits on) a buffer the client's GPU is still writing:

1. **Explicit sync** (`wp_linux_drm_syncobj_surface_v1`; offered only where the card has
   timeline syncobjs and `DRM_IOCTL_SYNCOBJ_EVENTFD`, and the composite exports a fence):
   the acquire point is queried (`drmSyncobjTimelineWait`, timeout 0, `WAIT_FOR_SUBMIT`);
   unsignalled, an eventfd armed with `DRM_IOCTL_SYNCOBJ_EVENTFD` (flags 0: *signalled*) is
   watched by the readiness poller. An update is never applied before its acquire point
   signals: one the poller cannot watch (no entry, no eventfd) is queried again at each
   tick of a lit output until it signals or an entry is armed, and an update the queue has
   no room for disconnects its client (`no_memory`) rather than apply early — a pool
   reserve keeps room for clients that queue little. The release point is signalled at
   once when the buffer is never sampled (superseded, dropped, its surface gone), otherwise
   when the buffer lifecycle retires it — only after a newer commit superseded it (every
   composite that shows it samples it again) and the last frame that could sample it,
   window capture views included, completed — with the newest fence of render's composites
   and capture copies attached (a temporary syncobj and `drmSyncobjTransfer`, Mutter's
   `set_sync_point`). A wl_buffer destroyed while on screen hands its point to the
   lifecycle record, signalled the same way. Only dma-buf buffers: shm and single-pixel
   buffers are `unsupported_buffer`.
2. **Implicit sync** (every other dma-buf): one `poll(POLLIN, 0)` per distinct plane fd —
   already signalled is the norm (always on vmwgfx) — else `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`
   with `DMA_BUF_SYNC_READ` (a snapshot of the write fences) watched by the poller.

`wl_buffer.release` is still sent in both cases, by the same rule: a dma-buf on screen is
held until a newer commit replaces it (as in wlroots, Mutter and KWin). See
[threading.md](threading.md) for the
thread each step runs on.

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

## Other composite backends and the feedback protocol

The GL backend (`lx.gfx:gl_renderer`) imports client dma-bufs itself, on the render thread
where its context lives: `surface_manager::set_render_thread_import(true)` gives each
dma-buf `wl_buffer` a compositor texture id (as for shm), the UI-side Vulkan import cache
is skipped, and the drain makes one `EGLImage` per buffer, reused while the client
re-commits it. On hosts that pick GL, the only Vulkan device is usually a software
rasterizer whose import fails outright (`vkAllocateMemory`), which is why the UI-side
import is not attempted there.

`zwp_linux_dmabuf_v1` v4+ feedback sends what the protocol requires: `format_table` (one
sealed memfd for the process: ARGB8888/XRGB8888, linear), `main_device` and one tranche
for it (the render node of the KMS card, found through sysfs at startup), then `done`. The
legacy `format`/`modifier` events go only to v1–v3 binds. A feedback with no format table
crashed GTK 4 at display open. Formats and modifiers beyond linear 8888 are a follow-up
(they need the EGL/Vulkan query per format).

### How an imported buffer is drawn

Whatever the backend, a client buffer reaches the composite as a `blit_command` whose
`dst` is in output pixels and whose `src` is a **float** source box in buffer pixels
(`wp_viewport.set_source` × the buffer scale, turned into the buffer by the inverse
transform), plus `buffer_xform` (`wl_surface.set_buffer_transform`). Each backend samples it
through the same affine basis (`gfx::source_basis_for`): GL as uniforms, Vulkan in the push
constants (`src_uv` = origin and +x, `uv_dy` = +y), CPU by inverse mapping. A dma-buf client
at a fractional output scale that renders `round(size·scale)` pixels is drawn 1:1 from its
imported image — no intermediate copy, no resampling (ADR 007).

## The producing side: Lumen's own clients

Lumen's clients allocate what this path imports through `lx.gfx:swapchain` (ADR-010 §4.10):
the render node of the feedback's main device, a fourcc and modifier list from its tranches
(`choose_client_format`), and per window a swapchain of three dma-buf images — GBM bos drawn
by GLES 3.1, or Vulkan images with `VK_EXT_image_drm_format_modifier` exported as dma-bufs —
or two shm images on the CPU tier. A re-committed image is the same `(client, buffer,
modifier)` key, so the import cache hits. Explicit sync is per image: a DRM syncobj timeline
(GL: the frame's native fence imported at the acquire point; Vulkan: a timeline semaphore
exported as the syncobj), which `drm_timeline_table::import` takes as any client's. Without
it the Vulkan tier attaches its frame fence to the dma-buf (`DMA_BUF_IOCTL_IMPORT_SYNC_FILE`),
so this side's implicit wait sees it.

## Error handling

| Condition | Action |
|-----------|--------|
| Unsupported modifier | Reject commit; send protocol error to client |
| Import fails | Fallback: SHM buffer copy path (dev only) |
| Fence not signalled | The content update waits in the queue; the previous content stays on screen; nothing stalls |
| Timeline syncobj op fails | `drm_err::timeline_failed`, logged at most every 600th time; an `import_timeline` of a non-syncobj fd is the client's `invalid_timeline` error |
| Client disconnect | Release all imported images for client |

## Implementation phases

| Phase | Deliverable |
|-------|-------------|
| D0 | `DmabufDesc` extraction from `zwp_linux_buffer_params_v1` |
| D1 | Basic import (single-plane ARGB8888) |
| D2 | Multi-plane + modifiers + feedback protocol |
| D3 | drm-syncobj timeline integration (ADR 009 Part 1: acquire wait before apply, release points, implicit readiness) |
| D4 | Direct scanout optimization (integrated) |

## Related docs

- [rendering-performance.md](rendering-performance.md) — scanout, snapshot v2, budgets
- [../uml/19-dmabuf-vulkan-class.mmd](../uml/19-dmabuf-vulkan-class.mmd)

## Related protocols

- `linux-dmabuf-v1` — buffer submission
- `linux-drm-syncobj-v1` — explicit sync
- `wp_presentation` — frame timing feedback
- `wp_viewporter`, `wp_fractional_scale_v1` — source crop, destination size, scale

## Code stubs

- `modules/lx.gfx/dmabuf_import.cppm` — importer interface
- `modules/lx.gfx/syncobj_bridge.cppm` — drm-syncobj ↔ Vulkan timeline semaphore bridge
- `modules/lx.gfx/semaphore_pool.cppm` — reusable timeline semaphores
- `modules/lx.gfx/import_cache.cppm` — import reuse cache

Direct scanout (plane assignment, overlay stacks) is **not implemented**: the compositor
always takes the Vulkan composite path.
