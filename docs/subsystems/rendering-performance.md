> **Status:** Current (v0.3) — see code for implementation truth

# Rendering Performance

High-efficiency compositor pipeline: zero-copy handoffs, direct scanout, present
timing, import caching, GPU resource pooling, and hot-path CPU budgets.

**Related:** [architecture.md](../architecture.md) · [threading.md](threading.md) ·
[dmabuf-vulkan-import.md](dmabuf-vulkan-import.md) · [errors-and-logging.md](errors-and-logging.md)

---

## 1. Design goals

| Goal | Mechanism |
|------|-----------|
| Zero-copy client pixels | dmabuf → Vulkan import |
| Zero-copy frame handoff | `snapshot_buffer` v2 — index swap, const ref acquire |
| Zero-copy draw build | `build_draw_list_into(write_draw_list())` — no interim copy |
| Low latency (discrete) | `present_mode::mailbox` + `backpressure_policy::triple_buffer` |
| Tear-free (integrated) | `present_mode::fifo_relaxed` + direct scanout |
| Minimal GPU state change | `batch_for_render()` + `pipeline_cache` + import cache |
| Bounded CPU work | `hot_path_budget` + `budget_tracker` |
| Accurate pacing | DRM page-flip + `presentation_tracker::bind_page_flip` |
| Partial KMS updates | `kms_damage_region` → `presentation_feedback.kms_damage` |
| Video/game priority | `content_hint` on `surface_commit` → scanout evaluation |
| Color-correct compositing | Linear-light workspace; encode once at output |

---

## 1.1 Color compositing space

All GPU compositing runs in **linear light** (`transfer_function::linear`). Surface
colors are decoded using each draw command's `src_space` and tagged `encoding`; the
output transfer function is applied **once** at the end of the render pass (before
KMS scanout or swapchain present). This prevents the sRGB-in-linear-space blending
errors common in legacy compositors.

---

## 1.2 Present backend selection

Chosen **once at startup**, in `compositor::start`, from `config::present`:

| `present_backend` | Composite | Scanout buffer |
|-------------------|-----------|----------------|
| `vulkan` | `gfx::vulkan_compositor` — GPU render pass | Vulkan image exported as dma-buf → `drm::kms_framebuffer` |
| `gl` | `gfx::gl_compositor` — GLES 2 quads via EGL on GBM | `gbm_bo` (SCANOUT\|RENDERING) → `EGLImage` → FBO, exported → `drm::kms_framebuffer` |
| `cpu` | `gfx::cpu_compositor` — damage-limited row blitter | `drm::kms_dumb_framebuffer` (CREATE_DUMB + mmap) |
| `automatic` (default) | hardware Vulkan → hardware GL → CPU; no KMS → headless | |

`automatic` reads `gfx::device_info::is_software_rasterizer`
(`VK_PHYSICAL_DEVICE_TYPE_CPU` — llvmpipe, SwiftShader) and, for GL,
`egl_device::is_software_renderer()` (`GL_RENDERER` naming llvmpipe/softpipe/swrast). Each
backend declines itself rather than accepting a downgrade.

**Why GL exists alongside Vulkan.** Mesa ships hardware drivers for devices it has no
Vulkan driver for — vmwgfx/SVGA3D under VMware being the case this was built for, but also
older radeon/nouveau and many SoCs. On those, EGL/GLES is the only accelerated path, and
without it the compositor falls back to software on hardware that is perfectly capable.

**Why the CPU path exists alongside both.** With a software Vulkan device the GPU path
still runs, but each frame then makes several passes over the whole surface to do the work
of one:

```
client shm ──swizzle──▶ staging ──memcpy──▶ VkBuffer ──copy+tile──▶ VkImage
                                                                      │
                                                       fullscreen fragment shader
                                                                      ▼
                                                            linear scanout image ──flip──▶
```

The CPU path collapses that to a single pass:

```
client shm ──memcpy (native order)──▶ staging ──blit (damage-limited)──▶ dumb FB ──flip──▶
```

Scanout is allocated **XR24**, which is already the byte order `wl_shm` clients use, so an
unscaled opaque surface reaches the display with no channel conversion at all.

`tests/bench_composite.cpp` prints the per-pass cost on the host in question — run it
rather than assuming these ratios transfer.

### Measured: VMware SVGA3D, 1918×928

Render-thread cost per frame — how long the tick is occupied, which is what the budget
cares about:

```
CPU   composite, opaque fullscreen             1.8 ms
GL    composite, dmabuf client (no upload)     0.1 ms
GL    composite, shm client (full upload)      2.7 ms
```

The GL figures are **submission** cost, not GPU wall time. The composite exports a fence
instead of blocking (see below), so the GPU work overlaps the rest of the tick and the flip
waits on the fence rather than the compositor waiting on the GPU. The GPU still needs
roughly 1.7 ms for a fullscreen quad; it just has a whole refresh period to do it in. Before
the fence, the same two lines measured 5.3 ms and 11.4 ms, all of it the render thread
sitting in `glFinish`.

Two things follow, and neither is intuitive:

**The client's buffer type dominates, not the backend.** A dma-buf client is sampled where
it already lives; an shm client has to be uploaded every frame, and on a virtualised GPU
that upload costs more than the composite. This is why `zwp_linux_dmabuf` matters more for
frame rate than any backend choice.

**For shm clients the two backends are close; for dma-buf they are not.** GL's shm figure
(2.7 ms) is dominated by the upload, which puts it in the same range as the CPU blitter
(1.8 ms) — and before the fence landed it was four times worse. For dma-buf clients there is
no contest: 0.1 ms of render thread, because nothing is copied at all. The CPU backend
cannot composite a dma-buf client without mapping tiled GPU memory, so a desktop running
real applications wants GL or Vulkan regardless. Set `config::present` explicitly if a
specific deployment measures otherwise.

### GL → KMS synchronisation

Nothing synchronises GL against the page flip that reads the scanout buffer. Rather than
block the render thread until the GPU is done, the composite exports the frame's completion
as a **sync_file** and the atomic commit waits on it — the display controller holds the flip
back instead of the compositor holding the frame back.

```
composite ─ eglCreateSyncKHR(EGL_SYNC_NATIVE_FENCE_ANDROID)
          ─ glFlush                     (the fence needs its commands submitted)
          ─ eglDupNativeFenceFDANDROID  → sync_file
                                          │
                     atomic_commit_request::in_fence_fd
                                          ▼
                            KMS holds the flip until the GPU signals
```

The FD is **owned by the caller** (`gl_composite_stats::out_fence_fd`) and must reach a
commit or be closed. In `compositor_impl` it travels with `composited_slot_` in
`composited_fence_fd_`, because a commit can be deferred a tick and the fence belongs to the
frame in that slot, not to the tick that commits it. Superseding an uncommitted frame, or
shutting the render loop down, closes it.

Without `EGL_ANDROID_native_fence_sync` the composite falls back to `glFinish`. Committing
unsynchronised would show a partly-drawn frame, so the stall is the correct fallback — it is
not optional.

### GL composite properties

| Property | Behavior |
|----------|----------|
| Context | EGL on `EGL_PLATFORM_GBM_KHR`, surfaceless, GLES 2 — the floor every driver supports |
| Thread | Built on the UI thread during `start()`, released, then claimed by the render thread in `render_loop`. An EGL context is current on one thread at a time |
| Scanout | 3 × `gbm_bo` (SCANOUT\|RENDERING) → `EGLImage` → renderbuffer → FBO, same slot rotation as the other backends |
| dma-buf clients | `EGL_EXT_image_dma_buf_import` → sampled in place, no copy. Imported **on the render thread** (where the context lives) against a compositor texture id per `wl_buffer` (`surface_manager::set_render_thread_import`); the UI-side Vulkan import cache is not used, since on hosts that pick GL its only Vulkan device is usually a software rasterizer that cannot import at all. The `EGLImage` is made once per buffer and reused while the client re-commits it |
| shm clients | `glTexSubImage2D` in RGBA (BGRA measured slower on SVGA3D) of only the **buffer-age region**: the union of surface damage since that buffer's texture was last written, plus the previous upload's damage (the other GL name missed it). Staged through `shm_upload_pool` — a transient block per update, sized by the region, returned right after the upload |
| Upload hazard | Each texture keeps **two** GL names and alternates. Uploading into the texture the previous frame is still sampling stalls the driver — measured 48 ms vs 22 ms per frame on SVGA3D. Each name tracks its own storage size: a client resizing every frame (interactive resize) alternates sizes across the names, and a sub-image upload into storage of another size is a GL error that leaves stale pixels on screen |
| Damage | `glScissor` over the damage rect, unflipped: the target is a framebuffer object over the scanout buffer, where window y = 0 is memory row 0, the top line KMS scans (GL's bottom-left origin applies only to window-system framebuffers) |
| Blending | `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` — sources are premultiplied |
| GPU sync | `EGL_ANDROID_native_fence_sync` → sync_file → `atomic_commit_request::in_fence_fd`; `glFinish` only as fallback |

**Modules:** `lx.gfx.gl_renderer` — `egl_device`, `gl_scanout_target`, `gl_compositor`

Errors use `error_domain::gl` with `lx::gl_err`, so a log line names the renderer that
actually failed.

### CPU composite properties

| Property | Behavior |
|----------|----------|
| Occlusion cull | Topmost opaque draw covering the damage rect skips the clear **and** every draw beneath it |
| Fast path | Unscaled + opaque + compatible format → row `memcpy` |
| Conversion | Fused into the blit (`copy` / `swap_rb`); no separate pass |
| Scaling | 16.16 fixed-point nearest, one add per pixel step |
| Damage | Composite is clipped to `snapshot.damage()` |
| Parallelism | `row_band_pool` — disjoint row bands, render thread participates |
| Write-combining | Opaque draws never read the destination back; blending does, and is slow on a scanout mapping |

`config::composite_thread_count` defaults to **0**. The fullscreen opaque case is a row
memcpy, which one core already saturates memory bandwidth with, so bands only add dispatch
latency there; raise it for scenes dominated by blended or converting draws.

**Modules:** `lx.gfx.cpu_renderer` — `cpu_compositor`, `pixel_surface`, `row_band_pool` ·
`lx.drm` — `kms_dumb_framebuffer`

### SHM staging

Two strategies, because the consumers differ:

| Consumer | Staging | Lifetime |
|----------|---------|----------|
| Vulkan upload (copies pixels out during drain) | `shm_staging_ring` — one FIFO for all surfaces | Slot free once the upload is recorded |
| GL upload, driver takes BGRA at any stride (`GL_EXT_texture_format_BGRA8888` + `GL_EXT_unpack_subimage`) | None: `shm_direct_uploads` — the render thread uploads the damaged region straight from the client's buffer | The `wl_buffer` is held, with an external `wl_shm_pool` reference, until the upload; released from `tick_ui` after it |
| GL upload, otherwise (and compositor images) | `shm_upload_pool` — a block per in-flight update, sized by the damaged region | Block returned right after `glTexSubImage2D`; blocks idle 1 s are unmapped |
| CPU composite (samples staged rows in place) | `shm_pixel_store` — two slots per texture | Slot held until a later commit supersedes it; a retired texture's storage is freed once neither slot is in flight |

**GL uploads from client memory.** Staging copied every damaged pixel on the UI thread
(with an RGBA swizzle) and the driver then copied it again. Where the driver takes the
client's BGRA rows at the client's stride, the render thread uploads them in place, inside
`wl_shm_buffer_begin_access`/`end_access` against a truncated pool. Two rules keep this sound:

- **Release timing.** The release that went out at commit now waits until the upload.
  A buffer committed again before that is released once, after the last read.
- **Early destroy.** A client may destroy the `wl_buffer` before the upload. Its destroy
  listener runs while the buffer is still readable and copies the region out under the
  table's lock. The render thread never reads a dead buffer, and the UI thread never waits
  on a frame.

Measured on vmwgfx across an interactive resize of `foot`, A/B in one build with two runs
each:

- The UI thread's share fell from 4.3–4.8% to 2.4–2.8%: `shm.stage` is gone.
- The render thread's `texture_update` rose from 3.8–4.4% to 5.7–7.3%. The copy now happens
  in the driver, so total CPU is about the same on SVGA.
- The median flip gap went from 19.3 ms to 16.3 ms. The resize now lands on vblank.

A shared FIFO cannot serve the CPU path: the composite reads the rows well after the
drain, so another surface's commits would recycle the slot underneath and the texture would
start showing the wrong pixels. GL left the shared 8-slot ring when subsurfaces arrived: a
client-side-decorated window commits about nine surfaces in one frame, the ring ran out,
and the updates it refused were never shown.

It then used per-texture staging, and that leaked in practice: nothing retired an entry
when its texture went, `vector::clear` kept the capacity even when something did, and
slots only grew. Every buffer the compositor had ever seen kept two window-sized copies
resident — a session's heap grew by about 120 MB in minutes of normal use (measured: glibc
reported 169 MB of a 174 MB heap in use). GL now stages through `shm_upload_pool`: blocks
live only from commit to upload, carry only the buffer-age region, are mmap'd directly
(large freed malloc blocks raise glibc's dynamic mmap threshold and fragment the brk heap),
recycle in steady state without syscalls, and are unmapped after 1 s idle. One busy client
still cannot starve another: blocks are per update, not a fixed shared ring.

Buffer age, not plain damage: Wayland damage is relative to the surface's previous
content, whichever buffer held it, while a texture belongs to one buffer. A client cycling
three buffers would otherwise leave each texture missing what changed while the others were
shown. `shm_upload_region` keeps an 8-commit damage history per surface; a buffer moved to
another surface, or one not shown for longer than the history, is staged whole, as are a
texture's first two uploads (both GL names). The CPU and Vulkan paths still stage whole
buffers: the CPU store *is* the texture, and the Vulkan upload takes whole images.

### Texture updates wait for their snapshot

Every `texture_update` carries `visible_from_frame`: the index of the snapshot that was being
built when it was queued (`p0_protocol_context::building_frame` — the tick's frame during
its dispatch, the next one once that frame is published, unchanged when a publish is
dropped). The render thread applies an update only when it composites a snapshot at least
that new; anything newer waits in a fixed 256-entry ring, and so does everything queued
behind it, which keeps FIFO order across a forget and a re-registration. Without it, a
buffer re-committed under the same texture id between two ticks was uploaded before the
*previous* snapshot composited, and its new pixels showed under the previous frame's
geometry — a window drawn at its new size inside its old frame, a synchronized subsurface
ahead of its parent. A full ring applies the oldest early (one warn).

### Blend from content

`surface_node` draws with `blend_mode::opaque` only when the content is opaque — a client
format without alpha (`XRGB8888`, `XBGR8888`, …), or an opaque region covering the whole
buffer — and the node opacity is 1. Everything else blends (premultiplied). Blending used
to be chosen from opacity alone, which drew every ARGB client's transparent pixels (GTK
shadows, rounded corners, CSD borders) as black. The CPU occlusion cull and `memcpy` fast
path still apply to XRGB and fully opaque ARGB windows. Opaque-region sub-rects (splitting a
draw into an opaque and a blended part) are a follow-up.

### Server-side frames: no new draw type

A window frame (`lx.compositor:decoration`) is drawn with the existing textured-quad path on
every backend: there is no solid-fill draw, and none was added. Border and title fills
stretch the centre 2x2 of a 4x4 colour swatch in one 64x64 atlas texture (the GL path's
half-texel inset keeps linear filtering inside the swatch; CPU nearest sampling is exact),
with `blend_mode::opaque`, so the CPU occlusion cull and the GL blend-off path still apply.
Colours are baked into the swatches rather than produced by `tint`, whose encoding semantics
would have to be proven identical on GL, CPU and Vulkan. The title is one premultiplied
texture per window and focus colour, rasterized on the UI thread by `lx.text` when the title
changes or its ellipsis cut moves (≈0.1–0.3 ms, not per frame); a focus change swaps the
texture id, a move or a resize that keeps the cut costs no texture work. 7–8 draws per
decorated window. Title texture ids are allocated upward and never reused, so a dated
retirement can never drop a later texture that happened to get the same id. Damage is the
frame's four strips (title bar plus borders, old and new apart), never the client area,
which the window's own nodes account for; a hover or press repaints the close button only.

### Texture retirement is dated, not immediate

`texture_update::forget` carries `retire_after_frame` — the first frame index whose
snapshot is guaranteed not to reference the texture (the frame after the buffer was
destroyed; the surface node is detached before the next `commit_frame` publishes). The
render thread queues the retirement and applies it only once it has composited a frame that
recent.

Queue order alone is not enough. A snapshot published while the buffer was still alive can
still be waiting to composite, and it references the texture **by id**:

```
tick_ui(N)      commit → draw list references texture T, snapshot N published
   (between ticks)  client disconnects → forget(T), retire_after_frame = N+1
tick_render     acquire snapshot N → drain → composite
                 forget applied here ⇒ snapshot N draws a texture that is gone
```

On the Vulkan path that surfaces as `draw referenced an unregistered texture`. On the CPU
path it is worse than a warning: the retirement releases the `shm_pixel_store` slot the
draw is still sampling, so the surface can render another client's pixels.

A texture is retired when its `wl_buffer` is destroyed and no surface shows it, or — when
the buffer object went first — when the surface stops showing it (a new buffer, or the
surface destroyed). The surface manager's buffer record goes with the `wl_buffer` object
(`forget_buffer`); it used to be kept forever, and after 256 buffers every later commit
failed.

---

## 2. Render path decision tree

Evaluated **every frame** on the render thread (after snapshot acquire):

```mermaid
flowchart TD
    START([tick_render])
    CAND[surface_manager.collect_scanout_candidates]
    EVAL[scanout_manager.evaluate]
    DIRECT{direct_scanout?}
    HYBRID{hybrid?}
    PLANE[assign_plane]
    OVER[assign_overlay_stack]
    VK[vulkan_composite — merged_render_pass]
    PRESENT[present — mailbox/fifo per output]

    START --> CAND --> EVAL
    EVAL --> DIRECT
    DIRECT -->|yes| PLANE --> PRESENT
    DIRECT -->|no| HYBRID
    HYBRID -->|yes| PLANE --> OVER --> PRESENT
    HYBRID -->|no| VK --> PRESENT
```

### Direct scanout eligibility (all required)

- Single fullscreen opaque surface on output
- Format/modifier supported by primary plane
- No overlapping translucent windows above (hybrid uses overlay stack)
- `config.enable_direct_scanout == true`
- `content_hint::video` / `content_hint::game` preferred when multiple candidates
- `lx.drm::plane_manager::can_direct_scanout(output)`

**Module:** `lx.compositor.scanout` — `scanout_manager`, `scanout_plan`, `overlay_layer`

See [../uml/26-scanout-decision.mmd](../uml/26-scanout-decision.mmd), [../uml/31-hybrid-scanout-overlays.mmd](../uml/31-hybrid-scanout-overlays.mmd).

---

## 3. Snapshot buffer v2 + back-pressure

**Problem (v1):** `publish(draw_list)` copied up to 4096 `draw_command` structs per frame.

**Solution (v2):** UI writes into the **write slot** in place; `try_publish()` swaps atomic
read index only; render holds a **const reference** (no copy).

```
UI thread                          Render thread
─────────                          ─────────────
write_draw_list()  ──mutate──▶  slot[(read+1) % 3]
build_draw_list_into(slot)         (no interim copy)
batch_for_render()
try_publish(d, idx) ──swap──▶   read_idx
  drop / stall if full             acquire() → const&
                                   record Vulkan
                                   release(idx)
```

### Back-pressure policies

| Policy | Behavior |
|--------|----------|
| `drop_latest` | Skip publish when `in_flight >= max_in_flight` — lowest latency |
| `stall_ui` | `try_publish` returns `false` — UI waits for render slot |
| `triple_buffer` | 3 slots, default `max_in_flight = 2` — mailbox-friendly |

Configure via `compositor::config::snapshot`:

```cpp
compositor::config cfg{
    .snapshot = {
        .backpressure = scene::backpressure_policy::triple_buffer,
        .max_in_flight = 2,
    },
};
```

| API | Thread | Cost |
|-----|--------|------|
| `write_draw_list()` | UI | O(1) slot access |
| `try_publish()` | UI | O(1) atomic index swap |
| `acquire()` | Render | O(1) const ref |
| `release()` | Render | O(1) in-flight counter |

**Module:** `lx.scene.snapshot` — `snapshot_buffer`, `backpressure_policy`, `snapshot_config`

See [../uml/27-snapshot-v2-handoff.mmd](../uml/27-snapshot-v2-handoff.mmd), [../uml/30-backpressure-policy.mmd](../uml/30-backpressure-policy.mmd).

### Draw list build (R1 — scaffold complete)

`scene_graph::commit_frame` calls `build_draw_list_into(write_draw_list())` directly.
No interim `draw_list` copy.

### Damage: multi-rect, buffer age, idle skip

Damage is a `gfx::damage_region`: up to 16 rects, the same capacity as `kms_damage_region`,
held inline. `add` merges two rects only when their bounding box wastes at most 25 % over
their union, so a window dragged a few pixels stays one rect while a window moved across
the screen stays two (old and new position) instead of one bbox spanning both. On overflow
the pair whose merge wastes least is merged. wlroots gets the same shape from a pixman
region in a `wlr_damage_ring`; this allocates nothing, and its rect count is exactly what
FB_DAMAGE_CLIPS takes.

| Stage | Damage used |
|-------|-------------|
| Scene → snapshot | Global rects noted by the window manager (old + new node rect on a move or resize) and by surface commits (client damage translated by the node origin). Carried forward over snapshots render never acquired |
| Composite | `frame damage ∪ age(slot)`: each of the three scanout slots records what it missed since it was last drawn into. Without this a damage-limited composite into a slot two frames old leaves ghost trails behind a moving window |
| GL | `gl_compositor::composite_region` — state set once, then per rect: scissor, clear, draw only the draws whose dst meets the rect; one fence per frame |
| CPU | `cpu_compositor::composite_region` — the existing per-rect path (with its occlusion cull) once per rect |
| Vulkan | Full repaint whenever anything is damaged — correct for any buffer age |
| KMS | Every rect since the last successful commit, as separate FB_DAMAGE_CLIPS: the plane's previous framebuffer predates this frame |

A tick whose snapshot carries no damage composites nothing and commits nothing. On this
host (vmwgfx, GL path) an idle compositor with no clients went from 47 to 24 scheduler ticks
of CPU per 5 s (9.4 % → 4.8 % of a core); the remainder was the 1 ms Wayland poll and the
free-running 60 Hz tick, not compositing. Both are gone: the loop waits on its fds and the
frame clock stops when idle, so an idle desktop (shell and a terminal) now costs no
measurable CPU (0 ticks per 5 s, 5 wakeups/s).

### Cursor: hardware plane, cursor-only commits, software fallback

The pointer is a KMS cursor plane (`lx.drm:cursor`), so moving it costs plane properties on
an atomic commit and **no composite**:

| Piece | Choice |
|-------|--------|
| Plane | `type == CURSOR`, reachable from the CRTC's index, scans out ARGB8888. The primary plane is likewise chosen by `type` now — with cursor planes exposed, taking the first plane on the CRTC is no longer safe |
| Size | First `SIZE_HINTS` entry the largest theme image fits; else `DRM_CAP_CURSOR_WIDTH/HEIGHT` (64 when absent). Images are padded to it: vmwgfx accepts nothing else |
| Hotspot | `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT` set at open, `HOTSPOT_X/Y` on every commit when the plane has them. Virtualised drivers (vmwgfx, virtio-gpu, qxl) hide the cursor plane from atomic clients without the cap, and draw the host pointer from the hotspot |
| Buffers | Two dumb buffers; a new image goes into the one not on screen, shown by the commit that switches `FB_ID` (drivers re-read the image on an FB change) |
| Validation | One `TEST_ONLY` commit before the plane first rides a frame; rejection → software for the session, never a failed frame. A content commit that fails with the cursor on it is followed by a cursor-only `TEST_ONLY` to tell which half was refused |
| Timing | Render reads the cursor mailbox right before it builds the commit (latest position). The cursor rides the content commit when there is one — pointer and the frame under it change on the same vblank — otherwise a cursor-only commit goes out on the deadline-aligned tick. Its flip re-arms the frame clock but is not a content present: it answers no frame callbacks and does not rotate scanout slots |
| Rule | A cursor-only commit needs the plane's CRTC in the state (`involves_crtc()`): a commit with no CRTC cannot carry a flip event (EINVAL). Turning on an off plane, moving it, or turning it off all qualify |

`LUMEN_CURSOR=software` skips the plane (debugging, screenshots of the composite). The
software cursor is a node in the scene's overlay layer: a fixed power-of-two square texture
(`k_cursor_texture_id`, so every image change is an in-place update on every backend),
`blend = premultiplied`, old and new rects damaged separately.

Measured on this host (vmwgfx, GL path, 1918×928), 3 s of 125 Hz absolute motion with no
clients, `render_us` from the `compositor.render` debug line:

| Cursor | Render tick per frame (mean / max) | Compositor CPU during motion |
|--------|------------------------------------|------------------------------|
| Hardware plane | 211 µs / 3.0 ms, `composite=0 commit=cursor` on every frame | 26 ticks / 3 s |
| Software (`LUMEN_CURSOR=software`) | 675 µs / 9.9 ms, `composite=1` (≈1.9 kpx) every frame | 42 ticks / 3 s |

---

## 4. Per-output present mode

| Mode | GPU tier | Behavior |
|------|----------|----------|
| `mailbox` | Discrete | Latest frame, may drop — **lowest latency** |
| `fifo_relaxed` | Integrated | Adaptive vsync — **balanced** |
| `fifo` | Integrated | Strict vsync — **no tear** |
| `auto_select` | Any | discrete→mailbox, integrated→fifo_relaxed |

**Module:** `lx.compositor.output` — `output_manager`, `present_mode`, `output_state`

---

## 5. dmabuf import cache + LRU

**Cache key:** `(client_id, buffer_id, modifier)`

```
on_commit → import_cache.acquire(key, desc)
         → hit: reuse VkImage
         → miss: importer.import + store (evict LRU if at cap)
each frame → tick(frame_index): evict stale (use_count==0, age > threshold)
client disconnect → evict_client(client_id)
```

Configure via `compositor::config::import_cache`:

```cpp
gfx::import_cache_config{
    .max_entries = 512,
    .stale_frame_threshold = 120,
};
```

**Module:** `lx.gfx.import_cache` — `dmabuf_import_cache`, `import_cache_key`, `import_cache_config`

See [../uml/28-import-cache-sequence.mmd](../uml/28-import-cache-sequence.mmd).

---

## 6. Draw batching + pipeline cache

Before `try_publish()`, `draw_list::batch_for_render()` partitions opaque and
translucent draws, preserves paint order for alpha blending, and uses texture ID
only as a tiebreaker within a batch.

`pipeline_cache::acquire(pipeline_key)` returns cached pipeline handle (stub until P0 Vulkan).

`merged_render_pass` batches `textured_quad` draws in a single pass when scanout is not used.

**Module:** `lx.gfx.pipeline` — `pipeline_cache`, `merged_render_pass`, `textured_quad`

See [../uml/33-gpu-resource-pools.mmd](../uml/33-gpu-resource-pools.mmd).

---

## 6.1 Frame clock and scanout rotation

**Three scanout slots**, not two. With two, the slot the display is scanning and the slot a
queued flip is about to scan leave nothing writable, so every tick that overlapped a flip
had to be dropped. `compositor_impl::pick_back_slot` returns the slot that is neither on
screen nor queued, so the composite always has somewhere to go.

A frame composited while a flip is queued waits in `composited_slot_` and is committed as
soon as the CRTC is free. If another frame lands first it simply takes its place — mailbox
semantics, so what reaches the display is always the newest content.

**The tick is phase-locked to the display.** A timer free-running at `target_fps` drifts
against the real vblank cadence: ticks land while a flip is still queued and their work is
wasted, and the visible rate settles well below the refresh rate for reasons that look like
slow rendering. Each completed flip re-arms the frame timer just ahead of the next vblank
via `runtime::timer_source::schedule_at`, which is valid from inside the timer's own
callback. The interval comes from the active mode's `refresh_millihz`, not from
`config::target_fps` — that is only the fallback when there is no mode to read.

The lock only holds if the flip is *seen* when it happens. The DRM card is an `fd_source`
in the UI loop's epoll set, so the flip handler runs the moment the flip event is queued
(`drm_fd_callback`), not at the next tick or poll, and the phase is taken from the kernel's flip timestamp (CLOCK_MONOTONIC, the
clock `steady_clock` reads) when it is within one period of `now`. Draining at the tick and
using the tick's own time as the flip time put the phase up to a period late, so ticks ran
at *period − lead*: faster than the display. Measured with `scripts/drag-smoke.sh` on
vmwgfx at 60 Hz over the move and resize drags, about one composite in three then landed
behind a still-pending flip (`commit=deferred`, 60 of 171) and content presents were a
median 21.6 ms apart; with the flip drained promptly it is 4–10 of ~160, mostly resize
frames whose shm upload overran the period, and 15.9 ms. (That fix drained flips from a
1 ms polling pump. The loop now waits on the card fd instead, which sees the flip sooner
and costs nothing while idle.)

**Between bursts the clock stops** — see [threading.md](threading.md). A restart is aimed
at the next deadline extrapolated from the last flip, so it stays on the display's phase.

```
flip completes ──▶ card fd readable (epoll) ──▶ presentation_flip_handler
                     ├─ present_completed_ = true   (frame callbacks may fire;
                     │                               not for a cursor-only flip)
                     └─ vsync_timer.schedule_at(next vblank − peak repaint cost − margin)
```

The 250 ms stall watchdog still paces callbacks by tick if flips stop arriving, so a broken
output cannot wedge clients.

---

## 7. DRM page-flip + presentation feedback

**Current (P0 stub):** timer-driven vsync on UI thread.

**Scaffold (R3):**

1. `lx.drm.atomic` — `kms_atomic_commit`, `kms_damage_region`, `page_flip_handler`
2. `presentation_tracker::bind_page_flip(kms_atomic)` — wires flip → feedback
3. `on_page_flip(page_flip_event)` — populates `kms_damage`, `frame_latency_ms`
4. `wp_presentation` — `on_feedback()` (P0 Wayland wire)

```cpp
presentation.bind_page_flip(compositor.kms_atomic());
// on flip: presentation.on_page_flip(event) → handler callback
```

See [../uml/29-present-feedback-sequence.mmd](../uml/29-present-feedback-sequence.mmd), [../uml/32-kms-atomic-damage.mmd](../uml/32-kms-atomic-damage.mmd).

---

## 8. Timeline semaphore pool

Avoid per-frame `VkSemaphore` create/destroy on explicit-sync paths.

**Module:** `lx.gfx.semaphore_pool` — `timeline_semaphore_pool`, `timeline_semaphore_handle`

Accessible via `gfx::device::semaphores()`.

---

## 9. Content-type hints

Clients (or compositor heuristics) set `content_hint` on `surface_commit`:

| Hint | Scanout priority |
|------|------------------|
| `game` | Highest |
| `video` | High |
| `normal` | Default |
| `subsurface_ui` | Lowest — prefer composite |

Used by `scanout_manager::evaluate` when multiple fullscreen candidates exist.

---

## 10. Hot-path CPU budgets

Default budgets (override via `compositor::config::hot_path`):

| Field | Default | Meaning |
|-------|---------|---------|
| `max_tick_ui_us` | 2000 µs | UI thread per-frame ceiling |
| `max_tick_render_us` | 4000 µs | Render thread per-frame ceiling |
| `max_draw_commands` | 4096 | Draw list capacity |
| `max_import_cache_entries` | 512 | Import cache size |
| `max_frame_latency_ms` | 32 ms | End-to-end latency target |

Compositor logs **`warn`** when a tick exceeds budget (never on every frame at `info`).

---

## 11. Module map

| Concern | Module |
|---------|--------|
| GL composite (EGL/GLES/GBM) | `lx.gfx.gl_renderer` |
| CPU composite + row bands | `lx.gfx.cpu_renderer` |
| CPU-writable scanout | `lx.drm` (`kms_dumb_framebuffer`) |
| Present backend selection | `lx.compositor` (`config::present`) |
| Snapshot v2 + back-pressure | `lx.scene.snapshot` |
| Direct scanout + hybrid overlays | `lx.compositor.scanout` |
| Content hints | `lx.compositor.surface` |
| Present mode | `lx.compositor.output` |
| Import cache LRU | `lx.gfx.import_cache` |
| Pipeline cache | `lx.gfx.pipeline` |
| Semaphore pool | `lx.gfx.semaphore_pool` |
| KMS atomic + damage | `lx.drm.atomic` |
| Present timing | `lx.scheduler.presentation` |
| CPU budgets | `lx.scheduler.budget` |

---

## 12. Implementation phases

| Phase | Deliverable | Scaffold |
|-------|-------------|----------|
| **R0** | v2 snapshot, scanout, import_cache, budget | ✅ |
| **R1** | Build into write slot; back-pressure policy | ✅ scaffold |
| **R2** | Direct scanout + hybrid overlay stack | ✅ API; `assign_*` → P0 |
| **R3** | DRM page-flip + presentation bridge | ✅ scaffold |
| **R4** | KMS damage blobs + LRU eviction | ✅ scaffold |
| **P0** | Real Wayland + dmabuf + Vulkan present | Runtime |

**Architecture is efficiency-ready.** Runtime proof waits on P0 implementation.

---

## 13. UML index

| Diagram | Topic |
|---------|-------|
| [26-scanout-decision.mmd](../uml/26-scanout-decision.mmd) | Scanout decision tree |
| [27-snapshot-v2-handoff.mmd](../uml/27-snapshot-v2-handoff.mmd) | Snapshot v2 sequence |
| [28-import-cache-sequence.mmd](../uml/28-import-cache-sequence.mmd) | Import cache hit/miss |
| [29-present-feedback-sequence.mmd](../uml/29-present-feedback-sequence.mmd) | Page-flip + presentation |
| [30-backpressure-policy.mmd](../uml/30-backpressure-policy.mmd) | Drop vs stall vs triple-buffer |
| [31-hybrid-scanout-overlays.mmd](../uml/31-hybrid-scanout-overlays.mmd) | Hybrid primary + overlay stack |
| [32-kms-atomic-damage.mmd](../uml/32-kms-atomic-damage.mmd) | Atomic commit + damage blob |
| [33-gpu-resource-pools.mmd](../uml/33-gpu-resource-pools.mmd) | Pipeline cache + semaphore pool |
