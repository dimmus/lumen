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
errors common in legacy compositors. A surface with an image description
(wp_color_manager_v1) is decoded and converted by its own transform — "Colour management" in
§3.

On Vulkan the blend target is an R16G16B16A16_SFLOAT intermediate (8 B/px, 66 MiB at 4K)
that the encode pass reads into the scanout buffer. The one `vulkan_compositor` draws every
output, so it keeps one intermediate (with its framebuffer and encode descriptor set) **per
target size**, up to four: an output's scanout slots share one, a second output of the same
size takes its own while one of the four is free (the intermediate holds its output's newest
frame, see "Damage: multi-rect, buffer age, idle skip" in §3), and outputs of different
sizes no longer free and reallocate it — after draining the GPU — each time they alternate.
An intermediate unused for 600 frames (its output gone or resized) is freed; a fifth size
evicts the least recently used. Measured on lavapipe, 1918×928 alternating with 1278×618
(`bench_composite --vk`, 5 runs, medians): before, 2,800 page faults and +4.4 ms CPU per
composite over the same-size cases; after, 0 faults and no excess (CPU 122.4 → 118.3 ms
per composite). `test_dmabuf_composite` asserts one allocation per size and identical
pixels across alternations.

---

## 1.2 Present backend selection

Chosen **once at startup**, in `compositor::start`, from `config::present`:

| `present_backend` | Composite | Scanout buffer |
|-------------------|-----------|----------------|
| `vulkan` | `gfx::vulkan_compositor` — GPU composite (Vulkan 1.3, dynamic rendering) | Vulkan image exported as dma-buf → `drm::kms_framebuffer` |
| `gl` | `gfx::gl_compositor` — GLES 2 quads via EGL on GBM | `gbm_bo` (SCANOUT\|RENDERING) → `EGLImage` → FBO, exported → `drm::kms_framebuffer` |
| `cpu` | `gfx::cpu_compositor` — damage-limited row blitter | `drm::kms_dumb_framebuffer` (CREATE_DUMB + mmap) |
| `automatic` (default) | hardware Vulkan → hardware GL → CPU; no KMS → headless | |

`automatic` reads `gfx::device_info::is_software_rasterizer`
(`VK_PHYSICAL_DEVICE_TYPE_CPU` — llvmpipe, SwiftShader) and, for GL,
`egl_device::is_software_renderer()` (`GL_RENDERER` naming llvmpipe/softpipe/swrast). Each
backend declines itself rather than accepting a downgrade.

**Vulkan floor: 1.3** (ADR-004 D4). Instances ask for 1.3, and a physical device that reports
less is not listed (`device_selector::probe_devices`, `device::create`): to `automatic` it is
"no hardware Vulkan", and GL composites. That is NVK on Kepler, PowerVR, panvk on Bifrost and
possibly hasvk — all of which have GL drivers; RADV, ANV, NVK (Maxwell+), NVIDIA (Turing+),
turnip and panvk (Valhall) report 1.4, v3dv 1.3. In exchange every pass is dynamic rendering
(`vkCmdBeginRendering` — no `VkRenderPass`, no `VkFramebuffer`, pipelines made for a format),
every barrier synchronization2, and timeline semaphores core. For comparison, wlroots' Vulkan
renderer is built on render passes, and KWin and Mutter composite on GL. This changes how a
frame is recorded, not the work in it: on lavapipe (the only Vulkan on the measuring host)
`bench_composite --vk` and `lumen-ui-bench --backend vulkan` read the same before and after
within run-to-run noise (S4 record + submit p50 40–48 µs before, 38–41 µs after, 2026-10-28).
A hardware number needs RADV or ANV.

**Which GPU.** The Vulkan device is the one behind the KMS card, not the first one the
loader lists. `compositor::start` opens the card (`drm::find_cards`), passes its dev_t to
`gfx::device_selector::select_best(drm_node)` as plain major/minor numbers, and
`gfx::choose_physical_device` picks the physical device whose
`VkPhysicalDeviceDrmPropertiesEXT` primary node (VK_EXT_physical_device_drm), else render
node, is that card. On a hybrid laptop the loader commonly lists the discrete GPU first
while the panel hangs off the integrated one; compositing there sends every frame across
PCIe, and client dma-bufs and the scanout target belong to different devices, which defeats
zero-copy import and direct scanout. Without a match — no device has the extension, or the
card has no Vulkan device behind it (simpledrm, evdi, vkms, a GPU without a Vulkan driver)
— the first hardware device is used, as before matching existed, and the
`compositor.gfx` line saying which device and how is a warning. A hybrid laptop, then the
same laptop driving a DisplayLink dock (evdi, card2):

```
Vulkan device Intel(R) Graphics (integrated) for card 226:1: matched by primary node
Vulkan device Intel(R) Graphics (integrated) for card 226:2: no device owns the card, first hardware device composites
```

The start-up probe (instance only, beside the KMS open) lists the devices with their nodes
(`device_selector::probe_devices`); the choice is made from that list once the card is open,
so `automatic` asks whether *the card's* device is hardware, and the logical device is
created only after the card is known. The GL path needs none of this: its EGL display is
created on the card's own GBM device.

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
waits on the fence rather than the compositor waiting on the GPU. The GPU still has a
fullscreen quad to draw; it just has a whole refresh period to do it in. Before the fence,
the same two lines measured 5.3 ms and 11.4 ms, all of it the render thread sitting in
`glFinish`.

**The linear-light shader is the GPU's main cost, and usually the identity.** Decoding and
re-encoding sRGB is six `pow()` per fragment, and for an sRGB source with no colour tint —
nearly every draw — it gives back the texel it started from. The GL backend therefore links
the composite shader twice: as is, and with `LUMEN_IDENTITY`, which only scales the
premultiplied texel by opacity and tint alpha. Measured on SVGA3D, submit plus GPU completion
per frame: one fullscreen draw 5.4 → 3.0 ms, 50 blended 240×180 draws 6.2 → 3.2–4.3 ms.
Two programs rather than a branch on a uniform: the branch measured the same GPU time but
cost about 16 µs of submission per draw, which Mesa spends re-checking the shader against
the uniform's value.

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

### Fence deadline hints

A GPU that idles between frames ramps its clock up only after it has been busy for a while,
so a frame submitted to a cold GPU can miss its vblank even though the work fits. dma-fence
deadlines (Linux 6.4) tell the driver when a fence is needed: i915/xe waitboost a fence whose
deadline is near, msm boosts its devfreq; a driver without a `set_deadline` hook accepts the
hint and ignores it. Lumen hints two kinds of fence (`lx.gfx:fence_deadline`):

| Fence | Deadline | Where |
|-------|----------|-------|
| The composite's sync_file (GL native fence, Vulkan), which becomes `IN_FENCE_FD` | The vblank the tick that built the frame aimed at (`output_ui::aim_ns`, published to render in `output_pipeline::fence_deadline_ns`) | Render, right after the composite — at submission, before the commit |
| A client's implicit-sync dma-buf fence (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` snapshot) | The start of the repaint that would latch the update on its pace output (`repaint_scheduler::next_start`, via `cu_latch`) — the fence is needed by the latch, not by the vblank after | UI, in the commit, only for a fence still pending |
| A client's explicit-sync acquire point | The same latch time | UI: `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE` on the zero-timeout query the commit already makes — no extra ioctl |

A hidden surface (shown on no lit output) gets no hint: its GPU is not hurried for a frame
nobody sees. The kernel's atomic helper also sets the vblank deadline on `IN_FENCE_FD` when
the commit runs; Lumen's hint lands earlier, while the composite executes, and covers a frame
composited behind a pending flip whose commit waits a tick.

**Cost and fallback.** One ioctl per hinted sync_file, no allocation, no log on the frame
path. Support is probed by use: the first `ENOTTY` from `SYNC_IOC_SET_DEADLINE` (a kernel
before 6.4) turns hints off for that thread's instance for good, and a timeline query whose
`WAIT_DEADLINE` flag is refused (`EINVAL`) is repeated without it and never sent with it
again — the behaviour is then exactly what it was without hints. Availability is logged once,
at debug, under `gfx` when it is first known.

**On vmwgfx** (this project's measurement host, kernel 7.2) the ioctl is accepted
and has no effect: SVGA3D fences have no deadline hook and the host's GPU clocks are not the
guest's to raise. There is nothing to measure there; the plumbing is covered by
`test_fence_deadline` (the real ioctl on a udmabuf sync_file where `/dev/udmabuf` is
accessible, and ENOTTY on a non-sync_file) and the fallbacks by fakes.

### GPU time of a Vulkan composite

While `compositor.render` logs at debug, the Vulkan composite times every frame on the GPU
(`vulkan_compositor::set_gpu_timing`); at any other level nothing is recorded, read or logged,
and the command buffers are the ones they were. Three timestamps per frame: the top of the
command buffer, after the composite pass and after the encode pass — so the line splits the
frame into `composite_us` (acquires of fresh imports, a UI stream's atlas copies, the
composite pass) and `encode_us` (the linear intermediate into the scanout target), which is
the split a damage or encode change needs measured. A blit frame and a linear frame with UI
primitives (`begin_linear_frame` … `end_linear_frame`) are timed alike.

- **Pool:** one `VK_QUERY_TYPE_TIMESTAMP` pool of 3 × 2 queries (the two frame slots), made at
  `initialize` when the queue family has `timestampValidBits` and the device a
  `timestampPeriod`; no per-frame create or destroy. Each timed frame resets its slot's three
  queries inside its own command buffer.
- **No stall:** results are read without `VK_QUERY_RESULT_WAIT_BIT`, and only from a slot whose
  fence has signalled — polled once per render tick (`take_gpu_times`, `vkGetFenceStatus`), or
  when the slot is recorded again, which waited for that fence anyway. A frame is usually
  reported on the next tick; on lavapipe, whose sync_file export completes the work first, in
  the same one.
- **Log:** `compositor.render: gpu frame=F composite_us=… encode_us=… output=N`, `frame=` being
  that output's `frame=` line. The black frame of a refused power-off is timed but not
  listed.

This is not the `gpu` of the latency split (`compositor.latency`, drag-smoke's `g`): that one
is the composite's sync_file signal time minus the commit — how long KMS still waited for the
GPU — and comes from the same exported fence on GL and Vulkan. The timestamps are the frame's
own execution time, queueing behind other GPU work excluded, and they exist where the split
has nothing: a device that cannot export a sync_file (the render thread blocks, and the GPU
time hides inside `render_us`). On lavapipe the exported sync_file is an already-signalled
stub with a boot-time timestamp, so the split's `gpu` is 0 there, never a measurement. GL
composites are not timed this way.

**Measured** (lavapipe, 1920×1080, `bench_composite --vk` without and with `--gpu-timing`, 3
runs each, medians): timing off is the baseline within noise on every line (composite full
frames 97.1 vs 98.0 ms, cursor frame 2.12 vs 2.00, terminal line 3.62 vs 3.56). Timing on,
full frames are within noise (110.9 vs 111.9 ms) and the GPU time covers 99 % of the wall time,
as it must on a CPU rasteriser; small damage frames cost +0.6 ms (cursor 2.63, terminal line
4.22 ms), lavapipe flushing its rasteriser at each bottom-of-pipe timestamp — on a GPU a
timestamp is a write by the command processor. The split shows where lavapipe's time goes:
encode is 46 % of a full frame (50.6 of 110.5 ms). `test_dmabuf_composite`
(`gpu_timing_reports_each_frames_passes`) and `test_deco_titles` (`vk_gpu_timing_times_the_linear_pass`,
also under the validation layer) check that both passes report non-zero time no longer than
the frame's wall time and that a timed frame's pixels equal an untimed one's.

### A real client, end to end: vkmark

`scripts/vkmark-bench.sh --join-seat-session` (manual, not in ctest; about 3.5 minutes, and it
takes the display) runs vkmark as a Wayland client on the KMS console, each mode with its own
compositor at `LUMEN_LOG=debug`: `bare` (no shell), `panel` (`lumen-shell --no-background`) and
`shell` (panel and the built-in background). Per mode, two vkmark runs on an 800×600 window:

- **`--present-mode immediate`** (clear, cube, texture, desktop; 5 s each). vkmark commits as
  fast as buffers come back, so its FPS is what the commit → upload/import →
  `wl_buffer.release` path sustains. Composites stay at one per refresh at most; newer
  commits replace older ones in between.
- **`--present-mode fifo`** (clear and cube; 10 s each, both well inside a refresh period for
  the client). Stutter is the spread of the intervals between presented frames, from the
  `compositor.presentation: flip` lines: mean, stddev, p99, and `late`, the frames shown more
  than 1.5 refresh periods after the previous one.

vkmark's output is timestamped as it is written (it prints a scene's name when the scene
starts and its FPS when the scene ends), so every flip and `compositor.render` line is
attributed to a scene. The first 0.5 s of each scene is left out. The compositor's CPU time
is sampled from `/proc` at the same moments. A static panel or background must add nothing
to a frame that changed only vkmark's window. The summary compares `panel` and `shell` with
`bare` and warns when the damage (`rects`, `area`) or the `draws` per composite differ, or
when the panel redrew during a scene. It also prints render_us and CPU time per frame for
each mode. Results go to `vkmark-bench-logs/summary.{txt,json}`.

**Baseline** on this host. vkmark runs on lavapipe (software Vulkan): its WSI hands the
compositor shm buffers, which are uploaded on the GL path (SVGA3D, 1918×928 at 60 Hz). Use
the numbers only to compare runs of the script on the same host. Each value is the median of
3 runs on main 7ef5039; `cpu/frame` is the compositor's CPU time, all threads, per vkmark
frame.

```
immediate        FPS (bare/panel/shell)  composites/s        render_us p50       cpu/frame µs
  clear          94 / 88 / 89            31* / 59 / 59       1713 / 1417 / 1373  1233 / 1838 / 1679
  cube           88 / 85 / 85            59 / 57 / 57        1549 / 1556 / 1514  1708 / 1748 / 1659
  texture        74 / 75 / 75            56 / 57 / 57        1378 / 1362 / 1343  1604 / 1652 / 1633
  desktop        68 / 68 / 68            57 / 55 / 55        1074 / 1069 / 1085  1724 / 1638 / 1683
fifo             FPS   interval mean / sd / p99 ms   late per ~570 frames  render_us p50 / p95
  clear  bare    56    17.7 / 3.1 / 31.3             15                    1106 / 4482
         panel   57    17.6 / 3.1 / 31.2             12                    1109 / 3887
         shell   56    17.7 / 2.8 / 30.8             12                    1121 / 3265
  cube   bare    57    17.5 / 3.2 / 31.5             18                    1185 / 4276
         panel   56    17.7 / 3.2 / 31.3             17                    1152 / 4290
         shell   56    17.6 / 3.2 / 31.6             15                    1159 / 4502
```

\* Composites locked to every other refresh in that scene: the open issue below.

What this shows:

- **The panel and the background cost nothing per frame.** In every scene, mode and run,
  each composite was one rect of 480 000 px (the window) and one draw: the panel and
  background draws are culled outside the damage. The panel never redrew during a scene, and
  render_us and CPU per frame are within run-to-run noise of `bare`.
- **Stutter is not the shell's.** In fifo mode about 2–3 % of frames are late in all three
  modes (56–57 of 60 shown per second, p99 two refresh periods). The p95 render_us of 3–4.5 ms
  is the SVGA3D upload and composite. The emulated vblank's flip timestamps also jitter
  (intervals of 11–23 ms between consecutive flips).
- **Open issue: under an immediate-mode flood, composites sometimes lock to every other
  refresh.** Here every composite takes every second snapshot (the `frame=` index steps by 2),
  so the rate halves to about 31 per second. render_us is about 1.7 ms, so the composite cost is
  not the cause. When it happens it holds for the whole scene. It has been seen in all three modes, and
  before and after the `draws=` change. Over 7 runs it hit `clear`, the first scene, in 12 of
  21 cases, and `cube` once. vkmark's FPS goes up then (91–95), because buffers come back
  sooner when fewer frames are uploaded.

`compositor.render`'s `draws=` field (the draws the backend reported submitting) was added for
this comparison. With debug logging on, the numbers above did not change outside noise
(3 runs after the change against the 3 before). With debug logging off, nothing is formatted
or logged.

### GL composite properties

| Property | Behavior |
|----------|----------|
| Context | EGL on `EGL_PLATFORM_GBM_KHR`, surfaceless, GLES 2 — the floor every driver supports |
| Thread | Built on the UI thread during `start()`, released, then claimed by the render thread in `render_loop`. An EGL context is current on one thread at a time |
| Scanout | 3 × `gbm_bo` (SCANOUT\|RENDERING) → `EGLImage` → renderbuffer → FBO, same slot rotation as the other backends |
| dma-buf clients | `EGL_EXT_image_dma_buf_import` → sampled in place, no copy. Imported **on the render thread** (where the context lives) against a compositor texture id per `wl_buffer` (`surface_manager::set_render_thread_import`); the UI-side Vulkan import cache is not used, since on hosts that pick GL its only Vulkan device is usually a software rasterizer that cannot import at all. The `EGLImage` is made once per buffer and reused while the client re-commits it |
| shm clients | `glTexSubImage2D` in RGBA (BGRA measured slower on SVGA3D) of only the **buffer-age region**: the union of surface damage since that buffer's texture was last written, plus the previous upload's damage (the other GL name missed it). Staged through `shm_upload_pool` — a transient block per update, sized by the region, returned right after the upload |
| Upload hazard | Each texture keeps **two** GL names and alternates. Uploading into the texture the previous frame is still sampling stalls the driver — measured 48 ms vs 22 ms per frame on SVGA3D. Each name tracks its own storage size: a client resizing every frame (interactive resize) alternates sizes across the names, and a sub-image upload into storage of another size is a GL error that leaves stale pixels on screen |
| Damage | `glScissor` over the damage rect, unflipped: the target is a framebuffer object over the scanout buffer, where window y = 0 is memory row 0, the top line KMS scans (GL's bottom-left origin applies only to window-system framebuffers). The scissor is damage ∩ clip — not ∩ dst, which the quad already is — so every unclipped draw of a pass shares it |
| State changes | Scissor, scissor test and blend are set only when they change, and the program only when consecutive draws differ (linear-light or identity). Mesa does not drop a redundant call: on SVGA3D a repeated `glScissor` cost 1.7 µs per draw, a changed one 4 µs, a scissor-test toggle 7 µs and a blend toggle 2.6 µs; 50 draws against 16 damage strips went from 0.77 to 0.62 ms of submission |
| Blending | `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` — sources are premultiplied |
| Sampling | The source box (float, wp_viewport) and the buffer transform as an affine basis in three uniforms (`gfx::source_basis_for`, shared with Vulkan and CPU); UVs `highp` where the fragment stage has it (mediump cannot address texels past ~2048); `GL_LINEAR` throughout — a 1:1 draw on whole pixels samples texel centres exactly; a half-texel clamp keeps crops from bleeding |
| GPU sync | `EGL_ANDROID_native_fence_sync` → sync_file (deadline-hinted to its vblank) → `atomic_commit_request::in_fence_fd`; `glFinish` only as fallback |

**Modules:** `lx.gfx.gl_renderer` — `egl_device`, `gl_scanout_target`, `gl_compositor`

Errors use `error_domain::gl` with `lx::gl_err`, so a log line names the renderer that
actually failed.

### CPU composite properties

| Property | Behavior |
|----------|----------|
| Occlusion cull | Topmost opaque draw covering the damage rect skips the clear **and** every draw beneath it |
| Visible rects | A draw the visibility pass found partly covered writes only its visible rects; a part starting mid-row steps from the row's start as the whole draw would, so its pixels are bit-identical |
| Fast path | Identity (whole-texel source the size of dst, normal transform) + opaque + compatible format → row `memcpy` |
| Translucent formats | An ARGB window at full opacity is mostly opaque pixels: each run of them is copied, only the rest blended, and a fully opaque pixel never decodes (the 8-bit round trip is exact). A fullscreen such window: 15 → 4 ms |
| Conversion | Fused into the blit (`copy` / `swap_rb`); no separate pass |
| Scaling, crop, transform | Every non-identity draw inverse-maps pixel centres through the shared source basis, 16.16 fixed point, two adds per pixel: nearest for an integer magnification of whole texels (exact), bilinear otherwise, taps clamped to the source box |
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
| Vulkan upload (copies pixels out during drain) | `shm_upload_pool` — a block per in-flight update, the whole image | Block returned once the upload is recorded |
| GL upload, driver takes BGRA at any stride (`GL_EXT_texture_format_BGRA8888` + `GL_EXT_unpack_subimage`) | None: `shm_direct_uploads` — the render thread uploads the damaged region straight from the client's buffer | The `wl_buffer` is held, with an external `wl_shm_pool` reference, until the upload; released from `tick_ui` after it |
| GL upload, otherwise (and compositor images) | `shm_upload_pool` — a block per in-flight update, sized by the damaged region | Block returned right after `glTexSubImage2D`; blocks idle 1 s are unmapped |

`shm_upload_pool` caps what is staged and not yet uploaded at 64 MiB
(`k_max_in_flight_bytes`). Past that, a commit gets no block, and its update is dropped the
way a full ring dropped it: the buffer is released and the texture keeps its previous
content until the next commit. The cap only bounds a render thread that has stopped
draining. Without it the pool would grow with every commit. The first block is always
granted, so one update larger than the cap still goes through.

On Vulkan the pool's block then goes into the texture one of two ways. With
`VK_EXT_host_image_copy` usable for RGBA8 (`host_image_copier`: the format supports host
transfer, the copy can target SHADER_READ_ONLY_OPTIMAL, and the driver reports
`optimalDeviceAccess`, i.e. host-transfer usage costs no compression), `upload_rgba` writes
the image from the host — no second copy into a staging buffer, no queue copy, no submit,
and the 16 MiB staging ring is never allocated. Otherwise the ring and a
`vkCmdCopyBufferToImage`, as before (the ring now made on the first such upload). Either way
`upload_rgba` first waits for the frames in flight, so the host write adds no wait of its
own. `bench_composite` prints both (`-- Vulkan shm upload --`); lavapipe numbers say nothing
about a discrete GPU, where the host write crosses PCIe into ReBAR memory instead of the
copy engine reading host memory.

Vulkan used a shared `shm_staging_ring` until the pool took its place. The ring's slots
grew to the largest buffer they had carried and were never returned, so 57 MB stayed
resident after a 1080p client disconnected (anon memory 97.9 MB, against 43–44 MB with the
pool). With the pool the blocks are unmapped once idle.
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
texture's first two uploads (both GL names). The CPU store *is* the texture, so each of its
two slots is stamped with the surface commit its content came from, and a commit rewrites
only the damage since that commit (`shm_store_region`): exact buffer age per slot, which
stays right when one slot is written twice in a row because the other was still being read.
The Vulkan upload still takes whole images.

Damage stays **several rects**, not one box (`client_damage`, four rects merged by the
output region's 25 % rule): a terminal that changes its cursor line and its status line
sends two strips, and their bounding box is the window between them. The rects carry
through the surface's damage history, the buffer-age region and the upload — the GL backend
writes each with its own `glTexSubImage2D` from the client's memory
(`gl_compositor::upload_client_regions`) — and into the scene's damage, so the repaint is
two strips as well.

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
path still apply to XRGB and fully opaque ARGB windows. A partial opaque region (a CSD
window's body inside its shadow and rounded corners) leaves the draw blended but seeds the
visibility pass (§ Occlusion culling) and clips the draws under it to what shows; drawing
its opaque part without blending is a follow-up.

Vulkan draws `blend_mode::opaque` with a second composite pipeline that has blending off, as
GL and CPU already did (the frame switches pipeline only where the blend changes; descriptor
sets, push constants and dynamic state stay bound). A source of alpha 1 replaces the
destination either way (src·1 + dst·0 is exact), so the pixels are byte-identical
(`opaque_draws_without_blending_match_blended_ones`) and the F16 destination is not read.
Measured on lavapipe (`bench_composite 1920 1080 --vk`, 5 runs, median CPU per frame): whole
target, full damage 231.4 → 207.2 ms; one opaque fullscreen draw 188.5 → 177.5 ms. An XRGB
dma-buf is still sampled through a B8G8R8A8 view, so its X byte is its alpha on both paths.

**Opacity is applied once.** The node's opacity (alpha-modifier-v1) rides
`draw_command::opacity` only; every backend multiplies it with the tint's alpha. The scene
used to fold it into the tint as well, so a surface at alpha 0.5 came out at 0.25 — dormant
while every opacity was 1. A draw whose effective alpha is 0 is skipped by every backend
(`draw_invisible`) but still counts for the visibility pass, so a surface fading in from 0
keeps its frame callbacks.

### Solid colours: no texture, no upload

A `wp_single_pixel_buffer_v1` is a colour, not pixels: the surface node emits a solid draw
(`draw_command::solid`, the premultiplied encoded colour) of its logical size, the buffer is
released at apply, and nothing is staged, uploaded or sampled. GL draws an opaque untinted
one as a scissored `glClear` (no shader, no blend), anything else through the composite
shader with the colour in place of the sample (`use_solid`), so it is decoded, tinted and
faded exactly like a texel; the CPU path fills spans (no destination read when opaque) or
blends per pixel; Vulkan substitutes the colour in the shader (a push constant flag; a 1x1
descriptor keeps the pipeline's sampler set bound when a frame has no other). An opaque one
occludes for the visibility pass and the CPU clear cull like any opaque draw.

### Content updates wait off the frame path

An update that cannot apply yet (a fifo barrier, a commit-timing target, an unsignalled
fence) waits in the content-update queue, not in the composite: render never waits on a
client's GPU work, and a slow client delays only itself. Costs: an unblocked dma-buf commit
does one `poll(…, 0)` per distinct plane (the implicit-readiness check) and nothing else; a
blocked one takes a pool record and, per fence, one epoll add and remove; a tick with nothing
queued checks one counter. Frame callbacks moved from an inline 64-entry array per update to
a shared node pool, which halves what a commit copies. fifo pacing clears barriers right
after each output's publish, so a FIFO client gets exactly one update per refresh; a
commit-timing target arms a stopped clock for exactly the tick that presents it (an idle
desktop with a timed update wakes once). See [architecture.md](../architecture.md) "Explicit
sync, buffer readiness and the commit-path protocols".

### Server-side frames: quad primitives and atlas glyphs

A window frame (`lx.compositor:decoration`) emits its four border fills and its title fill
as UI **quad primitives** (ADR-010 §4.11): radius 0, no border, one colour, on the projected
whole-pixel rects the swatch blits used to cover — so the pixels are those of the blits (the
CPU compositor at scale 1 and 1.25 is pixel-identical in every frame state,
`test_decoration_geometry`). Colours are the palette's sRGB bytes as encoded values; a
dimmed (not-responding) frame carries the dim already applied with `cpu_compositor`'s byte
rule (`(c · 166) >> 8`), so no `tint` is involved. The title text and the close glyph are
**`mono_sprite`s** (ADR-010 Phase 2) from the compositor's own glyph atlas (`chrome_atlas`):
the title is shaped and fitted by `lx.text` and its glyphs rasterised into the atlas on the
UI thread when the title, its ellipsis cut or the window's output scales change (≈80 µs for
a 26-glyph title in a size not seen yet, ≈1–3 µs when its glyphs are cached — not a frame
cost), one run per output scale the frame is on so it is drawn 1:1 on each; the close glyph
is the analytic "×" rasterised per scale. Only the hover / pressed circle stays a blit of
the 64x64 atlas. There are no per-window title textures any more (no upload, no texture id,
no retirement per title change). Damage is unchanged: the frame's four strips (title bar plus
borders, old and new apart), never the client area; a hover or press repaints the close
button only. No shadow and no rounded corner yet — a visual change, left for Phase 3.

Dimming. The fills and the glyph colours carry the byte rule on every backend, so a dimmed
bar's fill and text follow one rule everywhere (Phase 1 left the title blits tinted in linear
light on GL and Vulkan, apart from their fills): on GL the dimmed fill and the full-coverage
glyph pixels equal the CPU's bytes, on Vulkan within 1/255 (`test_deco_titles`). The circle
blit still carries `k_dim_tint`.

### Screen capture: copies ride the composite

A capture (ext-image-copy-capture, wlr-screencopy) costs nothing while no session exists: one
counter test per tick and per composite. An output session with a pending frame copies the
frame the output has just composited, on the render thread, right after the composite —
only the damage since the composite it last copied (a per-output ring of the last eight
composites' damage, reset when the targets are made again) plus what the client damaged.
A requested frame starts no clock: it waits for its output to composite something new, so
an idle desktop with an idle screencast costs no wakeups (10 s idle with wf-recorder
running: 25 context switches, the same as without it).

| Copy into | GL path | Render time per frame (1914x924, SVGA3D) |
|-----------|---------|------------------------------------------|
| nothing (an animating window) | — | 0.34 ms median |
| client dma-buf (LINEAR XRGB) | one `glBlitFramebuffer` per damage rect into the buffer, imported once as a framebuffer; a native fence the UI's poller waits on | 0.50 ms |
| client shm | the same blit into the session's LINEAR GBM buffer behind a native fence; a worker waits on it, maps the buffer and copies the damaged rows into the shm | 0.50 ms (was 12.1 ms with a `glReadPixels` into a pixel-pack buffer mapped on render: SVGA3D has no asynchronous read-back, the map stalls) |
| wf-recorder (screencopy, shm) | as shm | 0.54 ms (was 11.5 ms) |

The pointer is drawn onto the copy for `paint_cursors` (a GL quad, or a CPU blend after the
shm copy) — the hardware plane is not in the scanout. The CPU backend composites the
frame's draws for the damaged rects straight into the client's shm; Vulkan reads its target
back synchronously (shm only). A window session composites its own capture view (scene
views 16–23), built only when a frame is pending and the view has damage. See
[architecture.md](../architecture.md) "Screen capture".

**Through the portals (ADR 009 Part 5).** A ScreenCast is xdg-desktop-portal-wlr's
ext-image-copy-capture session: per delivered frame the compositor's one copy into xdpw's
PipeWire buffer, nothing else in the compositor (measured: 54 frames/s of a moving client
at 1914x924 into shm buffers; pw-dump shows xdpw's `Video/Source` node linked to the
consumer). Idle, xdg-desktop-portal-lumen and xdpw do not wake (0 context switches in
10 s); a cast delivers frames only when the output changes, so its idle cost is the
compositor's per-frame copy machinery times the desktop's own damage rate — with
lumen-shell's panel repainting about once a second here, 24 → 154 compositor context
switches per 10 s (UI ≈ 6.5, render ≈ 4, worker ≈ 3 per copied shm frame). A screenshot
is one shm capture per output (60–90 ms per request end to end at 1914x924, the PNG included) and a PNG
written at zlib level 1 with the Sub filter: 30 ms against grim's default 114 ms for 16 %
more bytes.

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

**One snapshot buffer per output view** (ADR 008 M3). Damage is noted once in layout
coordinates and fanned out to the pending region of every view it touches (≤ 16 rect tests,
no allocation), projected per view and clipped to the view's pixels; a tick builds and
publishes only the views of the outputs that are due *and* damaged, each with its own
projection, so a flip-pending output never holds another output's slots, and an output
nothing happened on costs nothing. A view's draw list holds only draws that reach its
pixels (each surface node culls itself against the view's rect when it emits).

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
| Vulkan | `vulkan_compositor::composite_region` (and `ui_vk_renderer::composite` with `region`): the linear intermediate is persistent and holds the output's newest complete frame, so the composite pass loads it and redraws only this frame's damage — per rect: clear, then every draw (batch) meeting the rect, scissored to it — while the encode pass writes `damage ∪ age(slot)` into the target (loaded, one scissored triangle per rect). The chain is checked, not assumed: the intermediate records the target its frame went into, and anything else (first frame, new or resized intermediate, a whole-target `composite` such as the power-off black frame, a failed frame, device re-init) composites the whole target; a target not yet written is encoded whole. Pixels equal a full repaint byte for byte (`test_dmabuf_composite` `damage_limited_frames_match_a_full_repaint`, `test_deco_titles` `vk_damage_limited_titles_match_a_full_repaint`). lavapipe, 1920×1080, 3 slots (`bench_composite --vk`, 5 runs, medians [range]): a 64×64 cursor moving 125.1 [120.0–131.2] → 1.86 [1.54–2.30] ms per frame (CPU 232.6 → 1.8 ms), one terminal line 122.9 [121.2–126.2] → 3.31 [3.04–3.58] ms (CPU 231.2 → 4.6 ms), full damage 126.4 [121.7–128.9] → 123.5 [122.8–125.7] ms (CPU 231.9 → 233.3 ms, within the run-to-run spread) |
| KMS | Every rect since the last successful commit, as separate FB_DAMAGE_CLIPS: the plane's previous framebuffer predates this frame |

A tick whose snapshot carries no damage composites nothing and commits nothing. On this
host (vmwgfx, GL path) an idle compositor with no clients went from 47 to 24 scheduler ticks
of CPU per 5 s (9.4 % → 4.8 % of a core); the remainder was the 1 ms Wayland poll and the
free-running 60 Hz tick, not compositing. Both are gone: the loop waits on its fds and the
frame clock stops when idle, so an idle desktop (shell and a terminal) now costs no
measurable CPU (0 ticks per 5 s, 5 wakeups/s).

### Output scale: projection, edge rounding, pixel exactness

The scene is laid out in logical coordinates; `scene::output_projection` (origin, scale
n/120) is applied only when draws are emitted and damage is noted, so snapshots carry
output pixels and the render thread does no scale arithmetic (ADR 007, D1). Costs: a few
multiplies per draw in `project_node`, none per pixel; nothing allocates.

- **Edge rounding relative to the window** (D2): a window's tree container is an anchor; its
  root lands on `round((root − origin)·s)` and every node (subsurfaces, the server-side
  frame) rounds both edges relative to it. A fractional-scale client renders `round(w·s)`
  pixels, which is exactly the root's projected width wherever it sits: **1:1, no
  resampling**. Measured, not asserted: `test_viewport_geometry` checks the property for
  every root offset 0..119 and width 1..2000 at six scales; `test_gl_composite` draws a
  1949-pixel one-pixel checker 1:1 and reads it back bit for bit; `scripts/scale-smoke.sh`
  screenshots the live frame at 1.25, 1.5, 1.75 and 2 and finds every pixel of a probe's
  checker in place (0 pixels off, foot and GTK 4 buffers `round(size·s)`).
- **Damage** is projected by flooring the start and ceiling the end, widened by one pixel at
  a non-integer scale (edge rounding moves an edge by up to half a pixel either way); a
  resampled surface's content damage is widened by half a source texel in output pixels
  (`surface_manager::resample_margin`, wlroots' buffer-bleed rule). Whole-node damage (map,
  move, resize) needs no margin: a draw never paints outside its destination.
- **Buffer damage stays partial at any scale**: `damage` and `damage_buffer` are converted
  into each other's space at apply (`:viewport`), so the shm upload copies the buffer
  region that changed, not the whole buffer, for scaled, cropped and rotated clients.
- **Compositor-drawn pixels**: titles rasterized at `10 pt·s` and drawn 1:1, xcursor themes
  loaded at `round(24·s)`, client cursor images resampled once per change — never per frame.
  A scale that appears later (an output configuration) loads its theme then, once.
- **Output transform** (ADR 008 §C.4): emitters draw in the untransformed content space;
  the view turns each draw once after emission (`gfx::transform_rect` on its rect and clip,
  `gfx::compose_output_transform` on its buffer transform — integer arithmetic, a table
  lookup) and `project_damage` turns damage. Cost: a few integer operations per draw per
  turned output, nothing at normal transform; no renderer changed (every backend already
  samples through `source_basis_for` for all eight transforms), occlusion and KMS damage
  stay axis-aligned rects. A rotated output with a fractional scale is still 1:1: the
  turned rect of a whole-pixel rect is whole pixels. The cursor plane's image is turned
  once per image change on render, not per motion.

### Render scale: HiDPI supersampling and the area filter

An output's render scale (`output_state::render_scale`; config.toml `[[output]]
render-scale`, `--render-scale`, `--output-render-scale NAME=S`; user guide "Render scale")
is the scale clients are hinted to render at — `wp_fractional_scale_v1.preferred_scale`
the largest render scale of the outputs a surface is on, `wl_surface.preferred_buffer_scale`
its ceiling (`preferred_scale_120` / `preferred_buffer_scale` in `:output`, the only place
either is derived). Nothing else moves: the layout, `wl_output.scale`, xdg-output and the
scene's projection stay the output scale's, so a supersampled surface is an ordinary
resampled draw — its buffer is render/scale times the size of the output-pixel rect it is
drawn into. A change moves `output_manager::generation` but not `layout_generation`:
`outputs_changed` re-derives every surface's hints (sent only where they changed) and no
`wl_output` event, view or clock is touched. Cursor surfaces are hinted the output scale —
the cursor plane shows a buffer 1:1 — and so are the compositor's own pixels (titles,
xcursor themes), drawn at the output scale as before. A window capture is taken at the
window's render scale (`window_scale` follows the hint), so it gets the sharp buffer.
Lumen does not scan client buffers out directly yet; when it does, a buffer at a render
scale that is not the output's fails the size check like any resampled buffer.

**The filter.** A shrinking draw sampled bilinearly reads 2×2 texels around one point of a
footprint wider than a texel: content is weighted by where each pixel happens to land, and
fine strokes alias and shimmer as they move. umbriel's render_scale (noctalia-dev/umbriel
PR 573) changes no renderer: its buffers are sampled as any scaled buffer is. Lumen draws every draw that
shrinks its source by more than a texel per pixel with the area (box) filter on all three
backends (`gfx::downsample_footprint_for` decides, per draw):

- **Vulkan** (`downsample.glsl`, included by `composite.frag` and `composite_cm.frag`): one
  push-constant flag (the former padding word: the block stays 112 bytes), the footprint
  derived in the shader from the basis already pushed. Along each source axis, each pair of
  neighbouring texels is one bilinear tap placed at the ratio of their coverage and weighted
  by its sum: exactly the box filter, at most 3×3 taps for a footprint of up to 4 texels per
  axis (`k_max_downsample`), one tap at 2:1.
- **GL**: the same in GLSL ES 1.00, as separate programs (`LUMEN_DOWNSAMPLE` variants of the
  linear, identity and colour-managed programs) built on the first downscaled draw — not a
  branch on a uniform, which Mesa specialises per draw on SVGA3D (the identity variant's lesson), so a frame without
  such a draw runs exactly the programs it ran before.
- **CPU**: the texels under the footprint weighted by their exact 16.16 coverage, one
  rounding per channel.

An axis shrunk exactly 2:1 from a whole texel is not filtered: every pixel centre lands on
a texel corner there, where one bilinear tap already is the 2×2 average. The common case —
render scale 2 on a scale-1 output — therefore costs the compositor what any 2:1 draw always
did. Magnified and 1:1 draws are untouched (`test_downsample` checks both on every backend,
and `bench_composite --hash` frames are identical to main's on cpu, GL and Vulkan).

Measured (`test_downsample`, dev VM: cpu, SVGA3D GL, lavapipe Vulkan), a text-like pattern
of 1- and 2-texel strokes against the exact area filter, worst over four sub-texel shifts,
RMS / largest error of 255:

| Ratio | Bilinear (before) | Area filter |
|-------|-------------------|-------------|
| 4:3 (render 2 on a 1.5 output) | 10.2 / 33 | ≤ 0.33 / 1.0 |
| 8:5 (render 2 on 1.25) | 24.0 / 102 | ≤ 0.36 / 1.2 |
| 2:1 (render 2 on 1) | 0.35 / 0.5 | unchanged (no filter needed) |
| 3:1 | 83.5 / 220 | ≤ 0.28 / 1.0 |
| 4:1 | 55.2 / 184 | ≤ 0.46 / 1.1 |

The bilinear error moves with the content's phase (shimmer, the spread of the per-shift
RMS: 2.7 at 8:5, 11.7 at 4:1); the filter's spread is ≤ 0.06.

**Cost.** Clients pay first: a render scale r over the scale s draws and keeps (r/s)² as
many pixels (4x at 2 over 1) — their render time, buffer memory, and for shm clients the
compositor's upload of every damaged byte, which grows the same way. The composite of a
2:1 draw costs what it did (no filter). A non-integer ratio pays the filter, per pixel of a
redrawn downscaled surface; `bench_composite --downsample` (a fullscreen 1918×928 draw,
dev VM, interleaved A/B against main, two runs each, ms):

| Backend | 1:1 | 2:1 | 4:3 bilinear → area | 8:5 bilinear → area |
|---------|-----|-----|---------------------|---------------------|
| GL (SVGA3D) | 3.9–4.4 | 5.0–5.3 | 4.1–4.3 → 9.7–11.6 | 4.3–5.1 → 10.6–11.7 |
| CPU, 1 thread | 1.5–3.1 | 25–33 | 24–25 → 69–82 | 25–36 → 65–101 |
| Vulkan (lavapipe) | 86–145 | 87–112 | 87–151 → 126–129 | 87–114 → 151–199 |

The GL filter is fetch-bound: at ratios up to 2 it reads at most 2×2 bilinear taps, the
floor for an exact box, and a branch-free four-fetch variant measured the same. This cost
also reaches what predates render scales: an integer-only client on a fractional output
(rendering at the ceiling, 2 on a 1.5 output) is a 4:3 downscale and now takes the filter.

End to end on the dev VM's console (GL compositor on SVGA3D, a 1918×928 output at scale
1), `lumen-hello --frames 300` repainting its whole 640×480 window every frame, render
scale 1 vs 2, two interleaved runs each (p50s; the client's frame time is callback → commit
on its UI thread):

| Client tier | Buffer | Client frame p50 | Client CPU / frame | Compositor render_us p50 | fps |
|-------------|--------|------------------|--------------------|--------------------------|-----|
| GL (dmabuf), render 1 | 640×480 | 0.55–0.81 ms | 2.1–2.4 ms | 630–823 | 57 |
| GL (dmabuf), render 2 | 1280×960 | 0.59–0.60 ms | 2.5–2.9 ms | 737–754 | 55–57 |
| CPU (shm), render 1 | 640×480 | 0.52 ms | 2.0–2.1 ms | 1078–1084 | 57 |
| CPU (shm), render 2 | 1280×960 | 2.3–2.4 ms | 3.7–3.9 ms | 1672–1718 | 53–54 |

A GPU client's composite is unchanged (a dmabuf sampled 2:1: no upload, no filter); its
own CPU grows ~0.4 ms a frame and its GPU work — not visible to the frame clock here —
with the 4x pixels. A CPU-tier client's raster grows 4.5x and the compositor's shm upload
adds ~0.6 ms a frame. Buffers: 1.2 → 4.9 MiB each. foot streaming text into a 896×592
window (shm) showed the same shape: its buffer 1792×1184 at render scale 2 (1344×888 drawn
at 4:3 through the filter on a 1.5 output), compositor render_us p50 1.4 → 4.6 ms at 1x,
dominated by the upload. On hardware with a real GPU the upload and the filter shrink; the
client's render_scale² stays.

**Compared.** macOS renders a HiDPI "looks like" mode into a backing store at twice the
logical size and resamples the whole composited frame to the panel once per frame — every
pixel, the desktop's own drawing included, and a second full-frame pass. Mutter's
`scale-monitor-framebuffer` puts monitor framebuffers in physical pixels and, since
wp_fractional_scale_v1, has clients render at the exact (fractional) scale; integer-only
clients render at the ceiling and are scaled down, which is supersampling by necessity, not a
setting, and there is no way to ask clients for more than the monitor scale. KWin is the
same: fractional clients at the scale, legacy ones at the ceiling. umbriel's render_scale is
the setting Lumen follows (per output, clients hinted through both protocols, geometry
unchanged, mirrors publishing no hints — Lumen's captures never hint at all). Where Lumen
goes further: the area filter on every backend, measured against the exact box filter, so
a non-integer ratio does not shimmer; no extra
full-frame pass, unlike macOS — each surface is filtered as it is composited, damage stays
partial and the compositor's own pixels are drawn at the output's resolution directly; the
2:1 case free; applied live on a configuration reload; the cursor kept at the output scale.

### Occlusion culling and frame-callback throttling (the visibility pass)

`scene_graph::commit_frame` runs one top-down pass over the draws of each published frame
(`scene::mark_occlusion`, ADR 007 P3), in paint order before batching:

- Up to **eight opaque rects** are kept, seeded from draws that cover their destination with
  full alpha: `blend_mode::opaque` (a format without alpha or an opaque region over the whole
  buffer), opacity and tint alpha 1, translation only. A UI quad that is a solid square on
  whole pixels at alpha 1 (a decoration fill) seeds one too, at its place in paint order; no
  primitive is ever marked (§6, UI primitives in the compositor). A blended draw at full
  alpha seeds its `opaque_rects` (see below). When more are found the eight largest stay. A draw inside one of them, or outside the output, is marked
  `occluded`; GL, Vulkan and CPU skip it (`draws_culled`). The pixels are identical, so damage and buffer age are
  untouched — what is saved is the GPU fill of windows under a maximized one.
- **Partial opaque regions.** ARGB clients with an opaque region that excludes shadow margins
  and rounded corners (GTK 3/4, libadwaita, Firefox) used to occlude nothing. `wire_surface`
  now passes the region's two largest rects, clipped to the surface (none for an
  over-approximated region), through `surface_manager` to `surface_node::set_opaque_rects`;
  the node projects them into `draw_command::opaque_rects` on a 1:1 draw at full opacity and
  tint (start ceiled, end floored at the surface-to-dst ratio, so a fractional scale rounds
  inwards; a turned output turns them with the draw; a resampled or solid draw seeds none),
  and the pass seeds them clipped like the draw's own rect. Two, because the pass hides a
  draw only inside one rect: past the body and one corner-trimmed strip, a rounded region's
  rects are a radius tall and hide nothing window-sized. A whole-surface opaque surface is
  unchanged (its `dst` seeds, no rects). Measured (`bench_composite 1920 1080 --vk`, five
  stacked GTK-style CSD windows over a wallpaper, two under the top window's body, 5 runs,
  median [range]): draws culled 0 → 2, pixels drawn 4.98 M → 4.38 M (−12 %); CPU composite
  39.2 [38.6–40.1] → 36.6 [34.7–37.6] ms; GL on SVGA3D, fence waited, 7.67 [7.53–7.79] →
  7.05 [6.88–7.15] ms; Vulkan on lavapipe (with the no-blend pipeline) 170.0 → 146.1 ms.
  UI cost: list build plus pass 0.40 → 0.52 µs, the pass alone 0.05 → 0.13 µs per frame.
  Covered windows also get the throttled frame callbacks below.
- **Visible rects (partly covered draws).** A draw the rects cover only in part used to be
  drawn whole (wlroots clips each node to its visible region). The pass now subtracts them
  from its clipped rect, largest first, and stores what is left inline as up to
  `gfx::k_max_visible_rects` (4) disjoint rects (`draw_command::visible`, copied to
  `blit_command::visible`); a rect that would split it into more is skipped, so the part kept
  only over-covers, and a draw nothing is left of — a wallpaper under two tiled halves — is
  `occluded`. A cut that narrows a part's rows is made only when it hides at least a quarter
  of the part's width: below that, splitting each row costs the CPU path more than the pixels
  it saves (a 300x400 menu over a fullscreen window, 1-thread composite 1.69 ms whole vs
  1.77 ms cut; break-even near 480 of 1920 px, whatever the height). GL, Vulkan and CPU draw
  it once per visible rect as one more scissor (same uniforms / push constants), so the
  pixels are bit-identical; nothing is reordered. Measured (`bench_composite 1920 1080 --vk`,
  3 interleaved runs, median, before → after; GL on SVGA3D and Vulkan on lavapipe fence
  waited, CPU one thread; the 20-window row from 6 interleaved pairs against the same draws
  drawn whole):

  | Scene | Pixels drawn | CPU ms | GL ms | Vulkan ms |
  |-------|-------------|--------|-------|-----------|
  | 5 stacked CSD windows | 4.38 M → 2.62 M (−40 %) | 31.7 → 15.1 | 6.65 → 4.73 | 143 → 107 |
  | 3 cascaded opaque | 3.71 M → 2.40 M (−35 %) | 30.2 → 19.9 | 6.04 → 5.02 | 105 → 101 |
  | 6 cascaded opaque | 5.27 M → 2.96 M (−44 %) | 30.6 → 20.8 | 8.54 → 5.56 | 109 → 105 |
  | 4 cascaded CSD | 4.55 M → 2.81 M (−38 %) | 34.6 → 24.0 | 6.86 → 4.97 | 137 → 102 |
  | maximised under a panel | 4.15 M → 2.07 M (−50 %) | 30.9 → 2.4 | 6.70 → 4.31 | 103 → 92 |
  | two tiled halves | 4.15 M → 2.07 M (−50 %) | 29.5 → 2.5 | 6.84 → 4.11 | 100 → 93 |
  | fullscreen window + small opaque menu | 2.19 M (drawn whole) | 1.61 → 1.64 (same draws) | 4.62 | 94 |
  | 20 cascaded windows, CSD and opaque | 7.68 M → 4.41 M (−43 %) | 39.3 → 32.3 | 9.36 → 7.87 | 137 → 139 |

  Lavapipe gains little on opaque cascades (its fixed full-target encode pass dominates), and
  on a stack of 20 cascaded windows the many thin scissored parts cost it slightly more than
  the fill they save (+1.5 %; +7–12 % before the quarter-width rule). `automatic` never picks
  lavapipe. The pass costs 0.05–0.13 → 0.07–0.54 µs per frame on the first six scenes and
  about 3–4 µs at 21 draws (the 20-window stack).
- Cost: O(draws × 8 × 4) rect operations per published frame, a parallel
  `scene_node*`/`bool` scratch array of `k_max_draw_commands` in the scene graph (no
  allocation), one virtual call per on-screen node. Conservative by construction: overlaps
  the eight rects cannot express count as visible.
- Each surface node with a draw on screen records the frame; `surface_node::visible_in(f)`
  is an O(1) compare. **Presentation feedback** samples by it (a covered surface's update is
  not "shown"), and **frame callbacks** are paced by it: surfaces on screen in the last
  published frame are answered after the flip (held to the repaint deadline, §6.1), the
  others at most every 995 ms (niri's
  `FRAME_CALLBACK_THROTTLE`). Those throttled callbacks do not keep the frame clock running;
  the 1 s housekeeping timer answers them while it is stopped. Measured on SVGA3D: a probe
  covered by a maximized opaque window drops from ~30 commits/s to one per 1.00 s (app-smoke
  P3) and is back at the display rate the frame it is uncovered.

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

### Window animations

Off by default (`[animations] enabled`, user-guide). On, a window opens fading in from 0.9 of
its size and closes fading out to it, minimizes and comes back the same way, the windows of a
workspace slide with a switch on their output, and a placement the window manager makes —
maximize, restore, snap, a tile — moves and resizes the window there instead of jumping.

**Visual only.** The window manager's geometry, hit-testing, configures and the client never
see a motion: the client is configured for its final size at once, and its new buffer is what
is drawn, resampled into the in-between rect. A motion is a `scene::node_animation` on the
window's tree container — `p' = to + (p − from) · scale` and an opacity — that
`scene_graph::emit_node` applies to the draws the subtree has just emitted
(`draw_list::animate_range`): surface draws edge by edge with rounding (draws that tiled still
tile; a translation stays 1:1, so a slide never resamples), primitives (a server-side frame)
exactly, with radii, borders and colours scaled. With a primitive stream the range takes its
orders again from where it now lies, so nothing emitted later sorts under it. An inactive
`node_animation` costs one test per window: with animations off the scene, the draws and the
pixels are those of the build before them (`bench_composite --hash`: every frame hash, draw,
cull and pixel count identical, CPU, GL and lavapipe).

**Springs, not curves.** Every value — the visual rect's four offsets from the rest frame, the
scale, the opacity, a slide's offset — is a damped spring of unit mass (`stiffness`, default
800: ω₀ ≈ 28.3 rad/s; `damping-ratio`, default 1: critically damped), evaluated in closed form
from the instant it was released (`spring_step`), so a frame's pose depends on its time alone,
and a retarget mid-flight continues from the shown state at its velocity. Time is the frame's
**predicted presentation**: the next vblank of the first served output's phase-locked clock
(§6.1), never earlier than the last. A value stops — landing exactly on its target — at the
first frame whose remaining reach `√(x² + v²/ω₀²)` (its energy as a displacement: nothing it
does later goes further) is under **half an output pixel** (opacity: 1/255; scale: half a
pixel at the window's corners). An open or close at the defaults is 18 frames (300 ms at
60 Hz), a 1280 px slide 23.

**Frames only while something moves.** `window_manager::needs_tick` is true while a motion
runs; when the last one settles its rest is damaged once and the frame clock stops. Measured
live (GL, vmwgfx, 60 Hz, `compositor.render` frame lines): opening lumen-square 18 frames,
then **0 in 3 s** idle; closing it 18, then 0; weston-simple-egl killed (its ghost holding the
dma-buf) 18, then 0, no warning. Off: 1 frame each.

**Scoped to the window.** An animation changes only its own window's draws: it never turns
culling, damage or anything else off for the scene or another output. A workspace slide is
confined to the output view of the workspace (`node_animation::view`): there it moves 1:1,
on any other output a leaving window is not drawn and an entering one is drawn at rest.
Capture views (screenshots, screencasts) draw where a motion ends: an arriving window at rest,
nothing of a leaving one (`node_animation::leaving`: a closing window's ghost, a window
minimizing or sliding off its workspace), so a capture never shows a closed window or two
workspaces at once; they are damaged when a window starts or stops leaving. Damage is the old
and new visual bounds (the frame and everything the tree draws, mapped, a pixel wider), on that
output only for a slide.

**Closing: a ghost.** At unmap the window manager copies the window's frame (with its title
run) and surface nodes into a container of its own at the window's place in the window layer
(`make_ghost`; restacking skips ghosts), which fades out and is freed when the motion ends.
The copies draw the textures the surfaces showed: `hold_content` keeps each texture's dated
retirement waiting (retirements are queued at the next tick while animations are on, so a
null-buffer commit that retires before the unmap is still caught) and keeps its dma-buf
*current* in `buffer_lifecycle_tracker` (`hold` / `release_hold`) — never released to the
client, never evicted from the import cache — until the ghost lets go; then it is released
once the frames that sampled it have completed, as at any detach. Holds are per surface: a
window that unmaps again while an older ghost of it still fades ends that ghost first, and a
held buffer the surface commits again is the surface's once more (the hold ending never
releases what is on screen).

**Never against the pointer.** A grab on a window ends its motion at once (`begin_grab`), and
no placement animates while one runs — drag-to-restore follows the pointer, interactive
resize stays frame-perfect (`drag-smoke.sh` with animations on: PASS, `h` all committed sizes
matched, window-move latency median 11.8 ms as off). Fullscreen is not animated.

**What an animating frame costs** (`bench_composite 1920 1080 --animate`: each scene's top
window at 0.95 of its size, opacity 0.6 — resampled, blended, and no longer hiding what is
under it), medians of 3, against the same scenes at rest:

| Scene | GL (SVGA3D), fence waited | CPU, 1 thread |
|-------|---------------------------|---------------|
| stacked CSD windows | 4.3 → 6.9 ms | 17.2 → 70.1 ms |
| 3 cascaded opaque windows | 4.8 → 5.6 ms | 22.8 → 47.3 ms |
| 6 cascaded opaque windows | 5.4 → 6.2 ms | 22.9 → 44.7 ms |
| 4 cascaded CSD windows | 5.3 → 6.2 ms | 26.4 → 51.6 ms |
| maximised window under a panel | 4.5 → 5.1 ms | 3.4 → 4.9 ms |
| two tiled halves | 4.6 → 5.7 ms | 5.0 → 52.4 ms |
| fullscreen + small opaque menu | 4.7 → 4.8 ms | 2.5 → 5.4 ms |

On a GPU it is a fraction of a millisecond to 2.6 ms for the length of the motion. On the CPU
backend a large resampled window costs 20–50 ms a frame: animations are not for hosts whose
only renderer is the CPU. Lavapipe (Vulkan on the CPU): +5–15 % (113–150 ms a frame either
way). Nothing allocates per frame: the motions are a fixed table of 64, the ghosts' copies are
made at the unmap.

### Backdrop blur (ext-background-effect-v1)

A client names a region of its surface (`ext_background_effect_surface_v1.set_blur_region`,
double-buffered, copied at the request); what is behind that region is shown blurred. The
global announces `blur` where the composite backend draws it — GL ES 3 (it needs
`glBlitFramebuffer`) and Vulkan — and no capability on the CPU path, which accepts regions and
ignores them (there is no CPU blur: one would have to be measured cheap enough first, and the
CPU path already spends 2–25 ms a frame compositing). lumen-shell's panel asks for it
with `[panel] blur = true` (off by default: an opaque panel, as before).

**The region.** Clipped to the surface and made disjoint at commit (a wl_region's rects may
overlap, and a pixel under two blur draws would be blurred twice); up to eight rects, more
blur over the region's bounds. On output views the surface node emits one blur draw per rect
under its own draw (`draw_command::blur`, the region's bounds `blur_box`, the frame's number
for it `blur_source`), at the surface's opacity and tint, moved and faded with an animating
window; capture views emit none (a window capture is the window, not what is behind it).

**The blur** (`lx.gfx:renderer` states it exactly): dual Kawase (Bjørge, SIGGRAPH 2015; the
passes scenefx uses) at half resolution. The region's *footprint* is its bounds grown by 64 px
and clipped to the target, origin and size even. Level 0 is the footprint halved — each texel
the mean of its 2×2 block (GL: a 2:1 `glBlitFramebuffer` with linear filtering; Vulkan: a
bilinear tap at the block corner of the linear F16 intermediate) — then three down passes (4
centre + 4 diagonal taps, ÷8) and three up passes (4 axis + 2 × 4 diagonal taps, ÷12) at radius
1, every tap clamped to its level's texels; each pixel of the region samples level 0
bilinearly. That is a blur of σ ≈ 15 output pixels whose support (29 level-0 texels, 60 px)
fits inside the margin, so the region's pixels never see the footprint's edge except where it
meets the screen's (edge extension there). Levels are RGBA8 on GL (blended in the encoded
space, as GL composites) and 16-bit float in linear light on Vulkan. A Vulkan level is drawn
one texel past its size: a tap clamped to the last texel centre still weighs its neighbour by
zero, and an uninitialised half float there can be a NaN, which zero does not cancel.

**Where it runs.** In the frame that draws the region, at the point of its first blur draw,
from the target itself: everything painted before the blur draw is the backdrop, primitives
(server-side title bars) included — on GL the stream path's surface runs go through the same
`composite_into`, on Vulkan `record_blits` ends the linear pass, draws the levels and resumes
it. In a primitive stream a blur draw takes its order from its footprint, so what is under any
of it sorts before it and nothing emitted above it sorts in between. Levels are made per
output and region on first use (rounded up, so a resizing region does not reallocate every
frame), freed when the output's frames hold no blur draw any more; on a 1920-wide output a
32 px panel's levels are 61 200 texels (0.23 MiB on GL, 0.47 MiB on Vulkan) — not four
full-size buffers per output.

**The cache** (umbriel's background blur cache, `gfx::blur_planner`). A region's blurred level 0
is kept and drawn again — no passes, no target read — while its *backdrop signature* holds:
everything painted under its footprint before it, in paint order, hashed with each draw's
geometry, opacity, tint, blending, visible rects and content serial (bumped on every commit,
even of the same buffer), a primitive's instance bytes and a lower region's own signature
(`scene::blur_backdrop_signature`), plus the clear colour and the region's bounds. The region's
own surface and whatever is above it may change freely: a panel redrawing its clock draws the
cached blur under its new pixels.

**Damage.** A stale region whose draws reach the screen (not occluded, opacity × tint alpha
above zero) adds its footprint to the frame's damage, and the footprint gets a rect of its own
(`blur_planner::isolate`, on the damage and again on the region with the buffer age folded
in): it and every rect meeting it become their bounds, repeated until that rect meets no other.
The whole footprint is then redrawn in one pass before the blur draw reads it, and the blurred
pixels are reported changed; rects away from it are composited as they were. A backend blurs a
stale region only in a pass whose rect holds its whole footprint and culls the draw in any
other, so a split footprint is never read half redrawn. A fresh region changes no pixel by
itself. Blur draws are drawn inside their region only (scissored like any draw), so the blur
never leaks across the region's edges.

The cost that remains: a window under the footprint that commits makes the region stale (its
content serial is in the signature) even when its damage is far below, so each such commit
redraws the footprint beside its own damage. Live, foot maximised under a `[panel] blur` panel
on a 1918×928 output, one text row rewritten every 0.2 s: 780 px per frame with the blur off,
184 908 px with it on (the 1918×96 footprint plus the row, two rects). Collapsing the frame
to its bounds instead, as the first version did, composited 1 122 030 px per frame.

**Visibility.** A blur draw is never an occluder (its pixels are what the blur makes), and
once the visibility pass reaches a visible one it drops every occluder above it that meets its
footprint: a draw under the footprint is drawn although a window above the region hides it,
because the blur reads it. A region wholly covered by an opaque draw is occluded like any draw
and not blurred. Other outputs are untouched: the blur state is per output, and a frame
without blur draws runs none of this (the backend sees `draw_list::blur_source_count() == 0`).

**Not drawn.** A screen capture's re-composite (only when the software pointer must be left
out) draws the cached blur on GL and none on Vulkan (capture command buffers are not the
frame's linear pass); a window capture shows no blur; the CPU path offers none.

Measured (`bench_composite 1920 1080 --blur --vk`, medians of 4; a full-screen textured window
under a 1920×32 panel blur with a 72 % overlay, fence waited; "blurred again": the backdrop
changes under the footprint every frame; "window row update": a 960×16 row of the window far
below the panel, its commit leaving the backdrop stale):

| Case | GL, SVGA3D | Vulkan, lavapipe |
|------|-----------:|-----------------:|
| full repaint, no blur draw | 4.44 ms | 98.3 ms |
| full repaint, cached blur | 4.58 ms | 100.1 ms |
| full repaint, blurred again | 5.65 ms | 114.4 ms |
| panel update (120×24), no blur draw | 1.94 ms | 1.19 ms |
| panel update, cached blur | 1.91 ms | 1.23 ms |
| panel update + backdrop moving, blurred again (frame grown to the 1920×96 footprint) | 3.42 ms | 24.4 ms |
| backdrop moving, no blur draw | 1.97 ms | 1.47 ms |
| window row update, no blur draw (15 360 px) | 1.65 ms | 1.23 ms |
| window row update, blurred again (row + footprint, 199 680 px) | 3.38 ms | 23.7 ms |
| the same, frame collapsed to its bounds (first version, 1 067 520 px) | 5.38 ms | 65.0 ms |

So a cached panel blur costs nothing measurable, and blurring it again about 1.2 ms on SVGA3D
(+1.5 ms over a small update, most of it redrawing the 1920×96 footprint); lavapipe pays for
its rasteriser (the pass ended and resumed flushes its tiles). Off path — no surface asking —
the frame hashes and draw/cull counts of every `bench_composite` scene are identical to the
tree without the feature, and the timings within run-to-run noise. Correctness:
`test_backdrop_blur` against the chain computed in the test from its definition (GL within 2
levels, Vulkan within 1, blit and stream paths; nothing outside the region changes; the cached
blur is the same pixels), the planner, the scene's draws, the visibility pass and the
signature; `test_protocol_conformance` (errors, the capability, the region reaching the scene);
app-smoke T7 (a live window's stripes blurred to an even grey inside the region, exact
outside it).

### Colour management (wp_color_manager_v1)

A client may say what its pixels mean — an image description: transfer function, primaries,
luminances — and every backend then draws that surface through one per-draw transform into
the composite's **working space**: linear light, BT.709 primaries, D65, 1.0 = the output's
reference white, unbounded (negative is outside BT.709, above 1 a highlight). The maths lives
once, in `lx.gfx:color` (`color.cppm`); the shaders and the CPU path follow it.

| Step | What | |
|------|------|---|
| E | decode | piecewise sRGB (`compound_power_2_4`, version 1's `srgb`); a power curve V^γ mirrored through 0 (`gamma22` is γ = 2.2, `set_tf_power` any γ in [1, 10]); `ext_linear` unbounded; ST 2084 PQ in units of 10 000 cd/m² |
| M | 3 × 3, linear light | source primaries → XYZ (built from the chromaticity columns, so CIE XYZ's own y = 0 primaries work) → Bradford from the source white to D65 → BT.709; times the anchor s |
| s | luminance anchor | (L − min) / (ref − min): PQ s = 10 000 / (ref − min) (L = min + 10 000·E), everything else s = (max − min) / (ref − min). sRGB's defaults (0.2 / 80 / 80) give s = 1; PQ's (0.005 / 10 000 / 203) put 203 cd/m² at 1.0 |
| T | tone map | when the content can exceed the output's white: peak P from max_cll, else the mastering maximum, else the primary maximum, anchored the same way and capped by what the signal can encode. On the largest channel x, identity up to the knee k = 0.8; above it t = (x − k)/(1 − k) goes through the extended Reinhard f(t) = t·(1 + t/w²)/(1 + t), w = (P − k)/(1 − k), y = k + (1 − k)·f(t); all three channels scale by y/x (hue kept). Continuous with slope 1 at the knee, monotonic, P lands exactly on 1.0, so nothing below the content's own peak clips. PQ at its defaults (P = 49.3) puts reference white at 0.9; content declared no brighter than its reference white (max_cll = ref: SDR in a PQ container, as Chromium sends it) is not tone mapped |

What is outside the output's gamut after M (negative channels) is clipped per channel by the
encode, as everywhere else; what is above the content's declared peak (a description lying
about its range) clips at white. A surface without a description is sRGB, and so is one whose
transform works out as the identity (sRGB curve, BT.709, no headroom): `color_transform::active`
is false and the draw takes the ordinary path — the same shader, the same pixels.

**Backends.** Vulkan: a draw with an active transform binds composite_cm.frag (the composite
shader plus E, M, T; made with its opaque twin on the first such draw, per attachment format)
and pushes 160 bytes instead of 112 — the matrix rows with γ, P and an "encode here" flag for
encoded UI targets; one pipeline layout with a 160-byte range serves both, where the device has
it (`maxPushConstantsSize` ≥ 160 — every desktop driver; otherwise not offered). GL: a third
program, LUMEN_COLOR, in highp (PQ's exponents), built on the first such draw and outside the
per-frame uniform loop of the two ordinary programs; offered when the fragment stage has highp.
It blends in the encoded space the GL backend always blends in. CPU: the draw's texels go
through `color_convert` (a 256-entry decode table per draw for opaque texels, the curve itself
for translucent ones, the blend tables to encode); a solid colour is converted once. All three
draw blit and UI-stream frames, and screen captures, through the same code.

**Protocol** (`wire_color_management.cpp`, color-management-v1 version 3). Offered where the
backend draws it: the perceptual intent; features parametric, set_primaries, set_tf_power,
set_luminances, set_mastering_display_primaries, windows_bt2100; gamma22, ext_linear,
st2084_pq, compound_power_2_4 (srgb to version 1); all ten named primaries of version 1.
Without a capable backend (headless) only the intent and the outputs' description. Every
output is SDR sRGB (BT.709, compound_power_2_4, 0.2 / 80 / 80 cd/m²); it is what each surface's
feedback prefers, and it never changes. Descriptions are plain values copied into the pending
state (double-buffered with wl_surface.commit, cached for synchronized subsurfaces), validated
and turned into a transform when made; degenerate primaries fail the description, not the
client.

**Not done.** HDR output: the KMS half (HDR_OUTPUT_METADATA, Colorspace, 10-bit scanout), a PQ
encode and a working space relative to an HDR output's white — every output stays SDR, so
image_description_changed is never sent. ICC profiles (no parser). Windows-scRGB: every client
buffer format Lumen accepts is 8-bit (ARGB/XRGB8888, shm and dma-buf), so no buffer could carry
its range; PQ content arrives in 8 bits (it decodes correctly, with the banding 8-bit PQ has).
HLG, BT.1886, extended target volumes and the other rendering intents are not offered.

**Cost** (`bench_composite 1920 1080 --color --vk`, a full-screen opaque window repainted
whole, fence waited, medians of 3; GL on SVGA3D, Vulkan on lavapipe):

| Backend | undescribed | gamma 2.2 sRGB | sRGB curve, BT.2020 | PQ BT.2020 (tone mapped) |
|---------|-------------|----------------|---------------------|--------------------------|
| GL | 4.46 ms | 8.54 ms | 8.70 ms | 8.96 ms |
| Vulkan | 97.7 ms | 98.2 ms | 99.0 ms | 128.2 ms |
| CPU, 1 thread | 1.88 ms | 37.7 ms | 34.7 ms | 64.9 ms |
| CPU, 4 threads | 2.37 ms | 24.9 ms | 24.1 ms | 43.6 ms |

A described surface pays for its curve: on GL what the identity program saves an undescribed
one (the pow()s; SVGA3D emulates the shader on the host); on lavapipe little but PQ's
exponents; on the CPU path a float transform per pixel (16–30 ns), where an undescribed opaque
window is a row copy. Only the described surface's own pixels pay. Off path — no client
describing anything — every `bench_composite` scene's frame hashes and draw/cull counts are
those of the tree without the feature (3 interleaved pairs, and 3 Vulkan pairs on lavapipe)
and the timings within run-to-run noise; the protocol adds nothing per frame. On the CPU path
`color_convert` stays out of line and shares the per-pixel test with the tint: inlined into
the pixel loop it slowed undescribed draws too (channel swap, 1 thread: 4.6 → 5.7 ms, 6
interleaved pairs; 4.6–4.8 ms again once out of line, 16 pairs).

---

## 4. Per-output present mode

| Mode | GPU tier | Behavior |
|------|----------|----------|
| `mailbox` | Discrete | Latest frame, may drop — **lowest latency** |
| `fifo_relaxed` | Integrated | Adaptive vsync — **balanced** |
| `fifo` | Integrated | Strict vsync — **no tear** |
| `auto_select` | Any | discrete→mailbox, integrated→fifo_relaxed |

The tier is the compositing device's `VkPhysicalDeviceType` (`gfx::tier_of`: discrete GPU →
discrete, anything else → integrated), of the device matched to the card (§1.2), handed to
`output_manager::set_gpu_tier`. It used to be guessed from which `/dev/dri` nodes existed
(`card1` or `renderD129` meant "discrete"), which is wrong whenever simpledrm took card0 and
the only GPU is card1.

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

Before publishing, `draw_list::batch_for_render()` sorts the surface draws into strict
painter's order (sort key, then texture id as a tie-break; no opaque-first partition —
there is no depth buffer), then builds the primitive stream of ADR-010 §4.3 over the
surfaces and the UI primitives (`lx.gfx:primitives`) emitted into the same list: every
primitive took an `order` from the list's overlap grid at emission (one more than the
highest order under its bounds, so non-overlapping primitives share an order), a surface
draw takes one at push once the list has a primitive stream (those pushed before it starts
take theirs back to front when it does), and the stream is stable-sorted by `(order, kind,
page)` on packed 64-bit keys in a preallocated scratch and cut into batches wherever the
kind or atlas page changes, with the union of each run's clipped bounds kept for damage
skipping. One instanced draw per batch is the target for the UI backends (≤ 8 for the
10 000-quad scene, ADR-010 G3). A list without primitives has no stream: it is only sorted,
never touches the grid, its `refs()` / `batches()` are empty, and it reaches the
compositor's backends as the sorted `draw_command` array converted to blits, as before. With
primitives (server-side decorations), CPU and GLES 3.1 draw the stream through `:ui_cpu` /
`:ui_gl` and the other paths draw it lowered to blits — see "UI primitives in the
compositor" below.

`pipeline_cache::acquire(pipeline_key)` returns cached pipeline handle (stub until P0 Vulkan).

`merged_render_pass` batches `textured_quad` draws in a single pass when scanout is not used.

**Module:** `lx.gfx.pipeline` — `pipeline_cache`, `merged_render_pass`, `textured_quad`

See [../uml/33-gpu-resource-pools.mmd](../uml/33-gpu-resource-pools.mmd).

### UI primitives: the atlas and the CPU reference renderer (ADR-010 Phase 1)

`lx.gfx:ui_cpu` rasterises the sorted primitive stream into an XRGB8888/ARGB8888 buffer,
inside the damage rects only, in row bands of about 128 KiB of target so every primitive
crossing a band is drawn while the band is in L2 (drawing a 1919×1080 frame rect by rect,
the destination load was the hottest instruction in `perf`). The partition states the pixel
rules the GL and Vulkan backends copy; its own output is deterministic (no FMA, no libm
transcendental) and is pinned by `tests/golden/ui/*.png` (`test_ui_golden`, bit-exact).
A quad's fully covered interior is written without the SDF, and the clear under an opaque
square quad covering a band is skipped — both proven bit-identical by the tests.

`lx.gfx:atlas` packs tiles on bucketed shelves into CPU-owned pages; GPU backends upload
each page's dirty-rect list. When an allocation finds no room, tiles unused for 120 frames
are freed in place; a page that was above 85 % occupancy is marked and repacked
synchronously at the next `atlas::begin_frame` — never inside the allocation, since a
repack moves live tiles and a uv already emitted in the frame being built would then name
another tile's pixels.

Measured on the F8 host (VMware SVGA3D VM, 2 vCPUs, release preset; load 0.6–0.9;
`./scripts/ui-bench.sh --backend cpu`, `test_atlas`):

| | Value |
|--|--|
| Glyph packing (10–20 px UI glyph boxes) | 89.0 % of a 1024² page live when it first has to grow |
| Page rebuild, 2 000 live glyphs repacked | 0.62 ms |
| Page rebuild, 1 024 tiles of 64² on a 2048² page | 1.3 ms |
| S2 raster (one 784 px clock cell) | p50 13 µs, p95 63–113 µs |
| S3 raster (10 000 bordered rounded quads, full 1919×1080 repaint) | p50 25.6–27.5 ms, p95 36–69 ms |

The two `cpu` runs differ by more than 5 % at p95, so no `iritur-cpu` baseline is committed
yet; `none` stays the regression gate.

### UI primitives on GLES 3.1 (`lx.gfx:ui_gl`, ADR-010 Phase 1)

`lx.gfx:ui_gl` draws the same stream into a framebuffer object of any current GLES 3.1
context. The vertex stage turns each instance into its whole-pixel raster window (`ui_cpu`'s
pixel-centre rule), so the content mask costs no fragment work and the scissor is left to
the damage rects; the fragment programs transcribe `ui_cpu`'s functions. Against the CPU
references every golden case is within 1/255 on SVGA3D (and within 2/255 on llvmpipe), G10
asking for 3.

Instances go into a 3 × 2 MiB ring — one persistent coherent mapping with
`GL_EXT_buffer_storage`, mapped ranges otherwise, a fence per slot — and the vertex stage
reads them as a std430 storage buffer at a per-batch uniform offset. The ring is bound whole,
once per frame, with the slot in the offset: rebinding per slot with `glBindBufferRange`
made SVGA3D keep reading the previous slot's offset (stale quads on screen). Per-instance
attributes (`ui_gl_fetch::attributes`, for contexts without vertex-stage storage blocks)
measure the same here.

What the host offers and does not: SVGA3D exposes `GL_EXT_disjoint_timer_query` but answers
every query with 0, so `initialize` calibrates once and falls back to "fence-wall" (flush to
fence signalled); `GL_OES_get_program_binary` is listed with zero binary formats, so the
program cache (`$XDG_CACHE_HOME/lumen/gl-program-<hash>.bin`) is inactive and every start
compiles the five programs, 95–155 ms. llvmpipe has a format: 22.5 ms cold, 11.1 ms from the
cache (correctness host only; not a performance number).

Measured on the F8 host (`./scripts/ui-bench.sh --backend gl`):

| | S2 (one 784 px clock cell) | S3 (10 000 bordered rounded quads, full 1919×1080) |
|--|--|--|
| Draw calls, instance bytes | 1, 3 920 B | 1, 800 080 B |
| `submit` (CPU, `ui_gl_renderer::draw`) p50 / p95 | 373–385 µs / 2.4–3.5 ms | 0.70–0.83 ms / 1.7–5.9 ms |
| `gpu` (fence-wall) p50 / p95 | 1.6–1.9 ms / 3.4–5.4 ms | 8.75–9.26 ms / 10.5–13.6 ms |
| CPU reference (`raster`, above) p50 | 13 µs | 25.6–27.5 ms |

Three runs, load 1.0–1.8 (another worktree was benchmarking); they differ by more than 5 % at
p95, so no `iritur-gl` baseline is committed. Where the time goes on this host:

- `submit` is mostly the driver: a frame is about six vmwgfx ioctls, and `glFenceSync` (the
  ring slot's fence) is where Mesa's svga driver flushes its command buffer. For S3 the
  gather of 800 KB into the coherent mapping is ~0.5 ms against 0.2 ms into host memory (the
  bench's own `encode`); the mapped-range ring (`--gl-ring mapped`) measured a lower p50
  (0.51 ms) and a higher p95 (2.4 ms) in one 300-frame pair, not enough to change the default.
- `gpu`: with trivial shaders S3 still costs 4.3–4.6 ms (2.3 ms without any instance fetch),
  so a third of the frame is per-instance fixed cost on SVGA3D; the shading itself is the
  other half. Six-vertex triangles and four-vertex instanced strips measure the same.
- The UI-thread stages run 40–50 % slower beside the GPU work than with `--backend none` in
  the same minutes (S3 emit+order+encode p50 1.44–1.55 ms against 1.02 ms): the host's 3D
  work shares the vCPUs.
- **G3 is open** (ADR-010 §7 Phase 1, as built): its ≤ 1.0 ms for S3's emit + order +
  encode is missed on every backend — 1.13 ms p50 / 2.42 ms p95 with `none` (the committed
  baseline says `"G3": "fail"`), 1.44–1.55 ms p50 here. Emit is two thirds of it; closing
  it is carried into Phase 2, and `scripts/ui-bench.sh` warns while it stays open.
- G5 for `gl` rows: no `operator new` in any steady frame; `mallinfo2` moves by 592 B (S3)
  and 0.9–7 KB (S2) inside Mesa (sync objects, command buffers), which the script counts as
  a G5 failure — the driver's heap, not the frame path's.

### UI primitives on Vulkan (`lx.gfx:ui_vk`, ADR-010 Phase 1)

`lx.gfx:ui_vk` draws the same stream with the same GLSL: `shaders/ui/*.glsl` is one set of
files that `ui_gl` embeds as strings and compiles at run time and `ui_vk` compiles to SPIR-V
at build time (`ui_<kind>.vert/.frag` add the Vulkan prologue — the ring as a std430 buffer
at a push-constant offset, the globals block, the eight page samplers). Against the CPU
references every golden case is within 2/255, and within 2/255 of `ui_gl` on SVGA3D (G10:
3 and 2); `test_ui_golden_vk_validation` runs the Vulkan cases under
VK_LAYER_KHRONOS_validation and requires an empty error log (a run with synchronization
validation on, `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`, logged nothing either).

The only Vulkan on the F8 host is lavapipe, a CPU device: these are **lavapipe numbers,
correctness only**. Pipeline creation for the five pipelines is 42–65 ms cold and the same
warm: lavapipe keeps nothing in a `VkPipelineCache` (its data is a truncated 20-byte header,
which `ui_vk` does not write to disk), so the warm start is untested on a caching driver.
Since the 1.3 floor the pipelines are made for a format, so one set per format serves
`ui_vk`'s own pass and the compositor's linear pass, and there is no framebuffer per
attachment (before, a fifth attachment evicted one and waited for the ring). The same
persisted cache now holds the compositor's composite and encode pipelines:
`compositor::setup_vulkan_present` makes `ui_vk` first and passes `pipeline_cache()` to
`vulkan_compositor::initialize`. On lavapipe neither shows in a number — it caches nothing,
and compositor start-up with the shared cache measured the same as with none (2026-10-28,
cold and warm, Mesa's shader cache on and off). The gain is for drivers whose
`VkPipelineCache` holds binaries (RADV, ANV, NVIDIA) and is unmeasured until run there.
`lumen-ui-bench --backend vulkan` runs every scenario (S3: one draw, 800 080 instance bytes,
`submit` p50 ≈ 0.17 ms, `gpu` ≈ 60 ms of llvmpipe shading per full 1919×1080 frame); its rows
are labelled `vulkan-software` and scripts/ui-bench.sh neither compares them nor makes them a
baseline. No `operator new` in a steady frame; lavapipe's own threads move `mallinfo2` by
0.2–1.2 KB per run.

### UI primitives in the compositor (ADR-010 §4.11)

`lx.compositor:ui_composite` (`frame_painter`) is what `composite_output` calls per output
frame:

- **No primitive in the snapshot** (no decorated window on the output): the draw commands
  become blits in draw order and go to `cpu_compositor::composite_region`,
  `gl_compositor::composite_into` (the call `composite_region` makes) or
  `vulkan_compositor::composite` — the path as it was, and no primitive renderer, grid or
  stream is touched (`test_ui_composite` compares the bytes and counts the renderer's frames).
- **With primitives**, CPU and GL draw the snapshot's sorted stream with `:ui_cpu` /
  `:ui_gl` into the same scanout target and damage rects: runs of `surface` refs through the
  compositor's own blit code (`cpu_compositor::blend`, `gl_compositor::composite_into` with
  no flush — one submit at the end, fenced by `gl_compositor::fence_frame`), primitive
  batches through the UI renderer, in stream order. Both renderers are made when the backend
  starts (`ui_gl` programs from the binary cache, warmed with one quad on SVGA3D).
- **Vulkan** records the stream inside `vulkan_compositor`'s linear F16 pass
  (`ui_vk_renderer::composite`, Phase 2): the compositor begins its frame and records its
  acquires, `:ui_vk` its atlas copies, the pass is begun (whole, or per damage rect — see
  the damage table), `surface` refs go through
  `record_blits` with the compositor's own composite pipeline (decoded as `composite` decodes
  them), primitive batches through `:ui_vk` pipelines made for that pass, then the encode
  pass and one submit; an empty submit after it fences `:ui_vk`'s ring slot. In the pass a
  push-constant flag makes the shaders decode instance colours to premultiplied linear light
  and raise glyph coverage to the `text.contrast` exponent (`k_text_contrast_dark`, 1.67:
  the compositor's text is light on dark). Against the CPU compositor on lavapipe: fills,
  borders and window content within 1/255, glyph edges within 9/255 (6/255 at scale 1.25),
  mean stem weight +1.5 % (+14.7 % without the exponent); zero validation messages. A frame
  without primitives is `composite` as before, byte for byte.
- **Screen capture and the headless dump** draw the stream too: a capture's re-composite
  (CPU always, GL when the software pointer is left out, window captures) goes through
  `paint_cpu_capture` / `paint_gl_capture`, the frame's draws as the capture adjusted them
  standing in for the surface refs and every primitive moved by the crop; the headless dump
  walks the stream in order (`paint_headless`: blits, solid quads, glyphs blended from the
  atlas).
- **Without a primitive renderer** — a GLES 2 context (`egl_device` asks for 3.1 and falls
  back to 2.0) — the stream is lowered (`gfx::lower_to_blits`): a solid square quad on whole
  pixels becomes a solid blit, anything else (the title glyphs among it) is dropped and
  counted, with one warning per run of frames that drop. A GLES 2 host shows frames without
  titles.
- **The glyph atlas across threads.** `chrome_atlas` is written by the UI thread (a title
  change rasterises under its mutex) and read by the render thread: every paint whose stream
  has sprites holds the mutex while the renderer samples (CPU) or uploads (GL, Vulkan) the
  pages. The UI thread takes it only on a glyph miss, so a steady frame never waits.
- **Colour space.** Primitive colours are premultiplied and sRGB-encoded (ADR-010 §4.5);
  `:ui_cpu` and `:ui_gl` blend them in that space, as a client window does, while the CPU
  compositor blends *surfaces* in linear light through its tables and the GL one in encoded
  space. Opaque square chrome never blends, so it is the same colour on all three.
- **Diagnostics.** `ui_draw_stats` / `ui_gl_draw_stats` carry what the compositor's blit
  code reported for the surface runs (`surface_draws_skipped`, `_unsupported` on CPU,
  `_culled`), and `paint_cpu` / `paint_gl` fold them into `composite.draws_skipped` /
  `draws_unsupported`, so the unregistered-texture and unsupported-format warnings (and
  `report_unregistered_draw`) fire on decorated frames as on plain ones.
- **Threads (CPU).** `:ui_cpu`'s row bands run on the render thread, and its surface runs go
  through `cpu_compositor::blend`, which draws one band: on a frame with primitives the
  `row_band_pool` workers (`composite_thread_count` > 0) stay idle, while a frame without
  primitives still uses them. The default is 0 threads and the 2-vCPU F8 host cannot show the
  loss; running `:ui_cpu`'s disjoint bands on the compositor's pool is the follow-up, to be
  measured on a host with more cores. Opaque pixels (source alpha 1) are written without
  reading the target, which matters for a write-combining dumb buffer: a 1-px border quad
  has no interior span, so every one of its pixels goes through the per-pixel blend.
- **Visibility.** Only a quad that lowers to a blit (radius 0, borderless, one colour,
  integral bounds and clip) at alpha 1 occludes the draws under it — it is drawn identically
  on every path. Primitives are never culled: they draw wherever damage reaches them.
- **Turned outputs.** The order grid covers the content space emitters draw in; after
  emission `draw_list::transform_primitives` turns bounds, clips, per-corner radii and
  per-edge borders (and a gradient's angle) with the draws, and marks each sprite with the
  transform (flags bits 1–3): the renderers map a pixel of the turned box back into the
  sprite's own through `source_basis_for`, so a title on a turned output is the content's
  title turned, pixel for pixel (`test_deco_titles`).

Render-thread cost per frame, `bench_ui_composite` (1920×1080, solid background, six
overlapping 480×320 windows, each with its frame when decorated; `paint_cpu` / `paint_gl`
on published snapshots as `composite_output` calls them — the compositor's virtual outputs
composite only into capture buffers on CPU and cannot run GL, so the bench drives the same
calls in-process; render node `renderD128`, SVGA3D; median and p95 of 300 frames):

| Backend | Damage | Undecorated p50 / p95 | Decorated p50 / p95 |
|---------|--------|-----------------------|---------------------|
| CPU | whole frame | 1.84–2.43 ms / 3.2–6.3 ms | 1.71–1.75 ms / 2.8–3.6 ms (stream) |
| CPU | one window (480×320) | 27 µs / 38–117 µs | 32 µs / 44–100 µs (stream) |
| GL, render thread | whole frame | 380–387 µs / 3.2 ms | 748 µs / 5.7–6.1 ms (stream) |
| GL, render thread | one window | 360–375 µs / 3.0–3.5 ms | 627–633 µs / 3.6–5.2 ms (stream) |
| GL, until the fence signals | whole frame | 9.3–10.7 ms / 14–15 ms | 10.4–12.1 ms / 15–21 ms |
| GL, until the fence signals | one window | 2.6–2.9 ms / 4.5–4.6 ms | 3.2–3.4 ms / 6.6–7.6 ms |

Two runs, load average 0.6–0.8 (2 vCPUs, the VM's own display session running beside them;
p95 on this host is mostly scheduling noise). On the CPU the stream path costs what the blit
path costs: `:ui_cpu` starts each band at the topmost ref that covers it opaquely (the same
cull as `cpu_compositor::composite`), and its L2-sized bands repaint a full frame no slower
than the composite's whole-rect passes; a window update over decorated windows is +5 µs. On
GL the stream path adds ≈ 0.25–0.37 ms of render-thread time per frame (the ring slot and
encode, then one `composite_into` setup per run of surfaces between the frames' quads —
unflushed, one submit at the end; flushing each run, as at first, cost twice that) and
0.5–1.5 ms to the frame's GPU completion on SVGA3D. A frame with no decorated window on the
output pays none of it. The lowered path (GLES 2) is the blit path plus one solid blit per
fill. On the UI thread, `commit_frame` of the decorated scene (30 quads, 7 surfaces: every
draw takes an order from the overlap grid, the refs are sorted and cut into batches) is
12 µs p50 for a full repaint and 5 µs for a window update, against 6 µs and under 1 µs
without decorations (load 0.3).

Title bars as glyphs (Phase 2). The same bench with a title on every frame ("Window N - Lumen
Terminal: ~/src/lumen", the compositor's font, 26–30 glyphs): before, each title one
premultiplied texture blitted as a surface; after, its glyphs as mono sprites from
`chrome_atlas` and the close glyph a sprite. 400 frames, p50 / p95; before three runs at load
0.17–0.35, after five at load 0.1–0.85:

| Backend | Damage | Titles as textures | Titles as glyphs |
|---------|--------|--------------------|------------------|
| CPU | whole frame | 1.81–1.85 ms / 3.7–4.1 ms | 1.87–2.08 ms / 3.6–4.4 ms (1.89–1.91 ms at load 0.1) |
| CPU | one window | 32 µs / 58–105 µs | 36–45 µs / 150–250 µs |
| GL, render thread | whole frame | 719–831 µs / 5.3–6.7 ms | 698–802 µs / 4.5–6.7 ms |
| GL, render thread | one window | 568–620 µs / 4.8–5.4 ms | 543–639 µs / 5.2–5.8 ms |
| GL, until the fence signals | whole frame | 5.99–6.04 ms | 5.91–6.05 ms |
| GL, until the fence signals | one window | 2.57–2.70 ms | 2.54–2.74 ms |

On GL the change is within the noise: ≈180 more instances in the ring and one more batch per
title run, against six texture binds fewer. On the CPU a full repaint costs 3–5 % more —
`:ui_cpu` shades each glyph pixel (colour × coverage, one blend) where the texture blit
copied premultiplied pixels — and a window update a few µs where a title crosses its damage.
On the UI thread `commit_frame` of the decorated scene goes from 16 µs to 23–26 µs p50 for a
full repaint (each glyph one `atlas::use` and one sprite; a title's glyphs share one order)
and from 6 µs to 13 µs for a window update. No frame allocates (`test_deco_titles`
`steady_titled_frame_allocates_nothing`, 32 frames). Vulkan has no hardware here: lavapipe
correctness only, no numbers.

---

## 6.1 Frame clock and scanout rotation

**Three scanout slots**, not two. With two, the slot the display is scanning and the slot a
queued flip is about to scan leave nothing writable, so every tick that overlapped a flip
had to be dropped. `compositor_impl::pick_back_slot` returns the slot that is neither on
screen nor queued, so the composite always has somewhere to go.

A frame composited while a flip is queued waits in `composited_slot_` and is committed as
soon as the CRTC is free. If another frame lands first it simply takes its place — mailbox
semantics, so what reaches the display is always the newest content.

**Idle slot catch-up (GL).** The third slot is drawn into only when a frame is composited
while a flip is queued, which is the first busy moment. Until then it keeps the age it
started with: the whole output, or everything damaged since. So that busy frame repainted
the whole output. The drag smoke (G9) caught one of these in the middle of a drag.

Now, once an output has composited nothing for 200 ms and nothing is queued, each slot off
screen with an age of at least a quarter of the output is brought up to date. It is copied
from the slot on screen, which then holds the newest frame, with `glBlitFramebuffer` over
just that age (`gl_compositor::copy_region`, OpenGL ES 3.0). Its age is then empty, and the
next frame drawn into it repaints only its own damage.

The rules:

- **Not between frames.** It never runs between the frames of an animation. Those frames
  repaint the same region anyway, and catching up between them copied about 1.3 Mpx twice
  per frame.
- **Small ages are left alone.** Copying them twice per cursor blink would cost more than
  the repaint they save.
- **An owed catch-up keeps render from reporting quiet.** The idle housekeeping tick then
  runs it, so it happens at most a second later.

Measured on vmwgfx:

- In the drag scenarios, the only full-output frames left are the first one or two after
  start-up.
- Over a whole app-smoke run, the mean repaint area per frame fell from 467k px to 385k px
  (−18 %). The full-output frames left (267 → 248) are frames whose own damage is the whole
  output: a workspace switch, a maximise.

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

**Several outputs** (ADR 008 M6). Each output has its own `repaint_scheduler` (phase from
its own CRTC's flips, its own repaint estimate and quiet count) inside
`scheduler::frame_clocks`; **one** `vsync_timer` is armed at the earliest start among the
running clocks, and a tick serves every output due within 1 ms of it (two outputs in phase
cost one wakeup). An output's clock stops after its own two quiet ticks while the others
run; a dark output's never starts. A served clock's next start is never sooner than one
period after the one just served, so a repaint estimate above the period ("run flat out")
means a tick per refresh, not one per millisecond — and that floor also holds when a flip
re-phases the clock (`frame_clocks::on_present`). On a display whose flips complete when the
frame lands instead of at a vblank (vmwgfx), re-phasing to each flip with a large estimate
(one slow frame in the window) used to aim the next start less than a period after the last:
FIFO clients were presented 8–14 ms apart for a while (app-smoke T1/T3 after long runs). On a
vblank-locked display the floor never binds. Measured on vmwgfx: with three outputs an
idle desktop costs 7 context switches in 5 s and no CPU tick; with two, a client
presenting 180 frames on one output left the other at 0 composites and 0 commits.

**Frame callbacks are answered after the flip, timed to the next repaint.** Sent at the
repaint start (the tick), the client's reply has no repaint to catch until the following one:
a frame every two periods (33 ms between presents measured with `lumen-probe presentation`).
Sent at the flip itself (wlroots, at the output's frame event), a client that draws at once
commits a whole period before the repaint that shows it: commit-to-present is a full
refresh. So the flip handler holds them until the output's next repaint start minus its
clients' **answer time** — the peak, over the last 8 answers, of release-to-first-damaging-
commit, plus 1.5 ms — and `callback_timer_` releases them (sway's per-view
`max_render_time`, measured instead of configured). Until an answer was measured, and when
the lead exceeds the period, they go out at the flip as before; a late answer raises the
lead for the next frame, and its worst case is the flip-time behaviour. A tick that finds
callbacks still held answers them itself.

**An output's first frame starts at once** (`repaint_scheduler::next_start` with no flip
yet: no phase to aim at, where it used to wait for half a period), and **waits for a
starting client**: while clients are connected — or a connection is still in the socket's
backlog — and none has mapped a surface, an output that has never shown a frame is left out
of the tick (which still runs: it sends the configures) for at most one refresh period. The
client's first commit restarts the stopped clock, so the modeset carries its window. Without
that the empty desktop went out first and the window a flip later — on vmwgfx the second
flip after a modeset lands 15–50 ms after its commit. With no client, the output lights at
once.

Measured on vmwgfx (SVGA3D, 2 vCPUs, 1914×924) with a 256×256 shm client drawing on every
frame callback, commit to `wp_presentation` presented, 10 runs × 300 frames, medians:

| Callbacks | p50 | p95 | fps | compositor CPU |
|-----------|-----|-----|-----|----------------|
| at the flip | 16.7 ms | 19.3 ms | 56.6 | 7.0 % |
| held to the repaint deadline | 5.1 ms | 7.1 ms | 60.0 | 8.0 % |

**While the output is powered off** (zwlr_output_power_v1, ADR 007 I4) the display gate is
up exactly as for a paused session: no composite, no commit, the frame clock stopped and the
1 Hz paused tick answering frame callbacks. The CRTC goes off with one blocking ACTIVE=0
commit (the cursor plane with it); coming back is the resume procedure — full damage and one
full modeset with ACTIVE=1, then partial damage again. Measured on vmwgfx (app-smoke K3, a
Perfetto trace of `lumen-probe power --set off` and an injected key): 0 `kms.commit` and 0
`composite` spans over 3.1 s dark, one `kms.power_off`, 3 `tick_ui` and 4 `tick_render` (the
paused tick and the posted task) — 2.0 thread wakeups a second for the whole compositor; the
first frame after the wake is one rect of the full 1914x924. The idle timer adds at most one
wakeup per notification timeout, never one per input event (`:idle`'s lazy deadline).

```
flip completes ──▶ card fd readable (epoll) ──▶ kms_device::dispatch_events
                     └─ routed by CRTC ──▶ presentation_flip_handler(pipeline)
                          ├─ clocks.on_present(slot) → shared timer re-armed at the
                          │    earliest start (next vblank − peak repaint cost − margin)
                          ├─ wp_presentation feedback of that output
                          └─ frame callbacks of the surfaces it paces (not for a
                               cursor-only flip): released by callback_timer_ at
                               next start − clients' answer time, or now
```

The 250 ms stall watchdog still paces callbacks by tick if flips stop arriving, so a broken
output cannot wedge clients.

---

## 6.2 Start-up

The start-up path is part of the product: a desktop that appears late is slow however fast
its frames are. `compositor::start` records an instant per phase on the timeline
(`LUMEN_TRACE_FILE`, category `startup`): `start entered`, `kms opened`, `graphics probed`,
`egl ready`, `gl program ready`, `present path ready`, `gl paths warmed`, `outputs ready`,
`socket bound`, `cursor ready`, `globals registered`, `input started`, `shell bridge
installed`. What keeps it short:

- **Overlap.** The hardware-Vulkan probe runs beside the logind/KMS open; input devices and
  the cursor theme load beside the EGL/GL set-up (threading.md, start-up helpers). The probe
  skips software Vulkan drivers (`gfx::hardware_probe_scope`): 20 ms → 5 ms here.
- **Warm GL paths.** `warm_gl_paths` draws once through each client texture path (shm BGRA
  opaque and with alpha, resampled, RGBA) into a scanout slot not yet shown, flushed and not
  waited for: a virtual GPU builds its shader and surface variants during start-up instead
  of on the first client frame.
- **First frame**: starts at once and carries a starting client's window (§6.1).
- **No fontconfig.** The first title bar (and the shell's first panel paint) used to start
  fontconfig on the UI thread, 18–40 ms. Both processes load their faces from the last
  run's persisted matches instead (text.md §4): the first decorated `tick_ui` 24.6 ms →
  1.7–2.8 ms on vmwgfx. Without them (a first run, a font change) the compositor shapes its
  title font on a start-up helper (`begin_title_font`), so fontconfig starts beside the
  present path and not in that tick: 35.0 ms → 1.3–2.1 ms from an empty cache.

What remains is Mesa: `egl ready` takes 50–65 ms on vmwgfx (loading the driver and its LLVM,
creating the screen), which every GL compositor pays.

Measured against other compositors on the same host (vmwgfx, 2 vCPUs, one 1914×924 output,
GL renderers, bare compositors: weston `--shell=kiosk --no-config`, sway with an empty
config, Hyprland with logo/splash/news off and its default Xwayland start), each started on
tty1 by a probe that execs it, connects as soon as the socket exists, maps a 256×256 shm
window and waits for its `wp_presentation` presented timestamp; 10 interleaved rounds,
medians:

| | Lumen | weston 15.0 | sway 1.12 | Hyprland 0.56 |
|---|---|---|---|---|
| socket ready | 84 ms | 80 | 88 | 224 |
| window configured | 96 ms | 89 | 171 | 527 |
| **first frame presented** | **106 ms** (94–117) | 115 (104–134) | 379 | 617 |
| commit → present p50 / p95 | **5.1 / 7.1 ms** | 11.1 / 15.8 | 1.2 / 4.8 at 561 fps | 9.2 / 18.8 |
| compositor CPU, client animating | **8.0 %** | 9.4 | 34.9 | 73.5 |
| private memory (RssAnon) | **19.1 MB** | 23.1 | 20.2 | 89.8 |
| shutdown | **29 ms** | 33 | 34 | 126 |

That table predates the cursor theme moving off the critical path: three traced runs since
put the window's configure 4–6 ms after the socket. sway's latency comes from compositing
every commit as it arrives (561 fps on a virtual display whose flips do not wait for a
vblank), at four times Lumen's CPU.

## 7. DRM page-flip + presentation feedback

The frame clock is phase-locked to page flips (§6.1); every flip also resolves
`wp_presentation` feedback:

1. `lx.drm.atomic` — `kms_pipeline` (one per CRTC) keeps the in-flight commit's `user_tag` (the
   composited frame index) and whether it carried a frame, and reports them in
   `page_flip_event` with the kernel's `sequence`, CLOCK_MONOTONIC timestamp, CRTC and
   whether the driver timestamps vblanks (`vblank_timestamp`; probed once with
   `drmCrtcGetSequence`). A blocking modeset's completion is reported as a synthetic flip
   (`hw = false`, sequence 0).
2. `presentation_tracker::bind_page_flip(pipeline)` — wires flip → `presentation_feedback`
   (tag, content, hw, vblank clock, seq) → the compositor's flip handler (UI).
3. The flip handler calls `presentation_flipped(frame = tag, {time, refresh, seq, flags})`;
   the table presents every update sampled at or before that frame whose surface that very
   frame shows (each publish records a sampled surface's visibility in a 64-frame bitmask;
   render composites only the newest snapshot, so the sampling frame may never have been
   shown) and discards the ones a newer update replaced in it; one hidden in the flipped
   frame waits for a flip that shows it or for its supersession. Two lists (queued, sampled) of in-flight slots are walked, a
   handful per frame; nothing allocates and nothing logs above debug.

```cpp
p.presentation.bind_page_flip(*p.kms);      // per output pipeline
// on flip: kms_device::dispatch_events → pipeline → handler → presentation_flipped(tag, slot, info)
```

**Per-CRTC flips, combined modesets** (ADR 008 M5). Frames are one nonblocking atomic
commit per CRTC with its own flip event, so outputs flip independently: a combined frame
commit would tie every output to the slowest and fail with EBUSY whenever any CRTC still
has a flip pending (wlroots, KWin, Mutter and niri all flip per CRTC). Configuration — the
first frames at start, a resume — is one `commit_modeset` of every pipeline that needs one
(TEST_ONLY first, CRTCs no output uses disabled, the neutral properties of each pipeline),
carrying each output's first frame; it waits up to 100 ms for every such output to have a
frame, then falls back to per-output modesets. The flip events of one card are read once
(`kms_device::dispatch_events`) and routed to the pipeline by CRTC id; a combined commit's
event is recognised by its cookie (device, generations, and a bit per pipeline the commit
armed and that pipeline's event consumes — separate modesets in flight together on one
cookie keep their own events). CRTCs another master lit are read again at each resume, and
the resume's modesets turn them off with every connector the kernel has on them.

**Hotplug** (ADR 008 Part B) uses the same two commit kinds and adds nothing per frame:

- A **connected monitor** gets its pipeline from render: targets at the preferred mode, a
  CRTC and planes no other pipeline holds, and a TEST_ONLY of the new pipeline with its real
  first framebuffer (the kernel tests it against the state the other CRTCs are in, so it is
  a test of the whole state); refused, the next mode in sway's order (preferred, then area ×
  refresh) is tried. Its first frame is a modeset of that pipeline alone; the other outputs
  keep flipping.
- A **mode change** (a VM window resize: the preferred mode changed) is tested the same way
  with a real framebuffer of the new size (`modeset_entry::mode` overrides the pipeline's
  mode for the test) before anything a client sees changes; then the output switches, and
  render takes the new targets right before compositing the first frame built at the new
  size — never compositing an older one into them — and modesets with it. The old targets
  stay until that modeset's flip. At a VT resume the probe comes first and the switch is
  part of the resume's one combined modeset, so a resize while away no longer fails the
  resume.
- A **disconnected monitor**'s CRTC is turned off once its last flip drained (CRTC off,
  connector unrouted, its planes detached, `connector_gone` for an MST port that vanished),
  then its targets and pipeline go. Retires run before the adds of the same batch, and an
  add waits (≤ 1 s) while a retiring CRTC is still lit, so a new output is allocated and
  tested with the CRTCs and bandwidth of the gone ones free.
- **Exit** hands the scanout framebuffers over with CLOSEFB (Linux 6.8; RMFB before it):
  the last frame stays on screen until the console's CRTC state is put back or the next
  master commits, instead of a black frame between (architecture.md § Session
  pause/resume). Nothing changes per frame; runtime releases keep RMFB.
- **Zero work when nothing changes**: without a uevent nothing runs (the netlink socket is
  filtered to drm in the kernel); a uevent whose probe matches the outputs (an identical
  layout, our own probe's detect) costs one worker probe (≈ 0.6 ms of ioctls on vmwgfx) and
  a diff on UI — no client event, no render work, no allocation.

**Output configuration** (ADR 008 Part C) adds nothing per frame either:

- **Test**: one TEST_ONLY of every pipeline the configuration changes, with real
  framebuffers of the new modes (kept staged ≤ 5 s for the switch, so the apply allocates
  nothing more) and new outputs through the CRTC allocator with the same test. A `test` or
  `--dryrun` stops there: nothing on screen moves even for a mode the kernel refuses.
- **Apply**: changed modes go through the hotplug path's switch (at the first frame of the
  new size, in the combined modeset); position, scale and transform repaint the views they
  touch without a modeset; adaptive sync is a modeset (`VRR_ENABLED`). Measured on vmwgfx:
  wlr-randr `--mode` = 1 TEST_ONLY + 1 modeset of that output; `--pos`/`--scale`/`--transform`
  0 modesets; kanshi re-applying the restored state 0 modesets.
- **Start**: the persisted state is applied before the first modeset, so a configured
  session still comes up in one combined modeset.
- **Adaptive sync clock**: with VRR the output's `repaint_scheduler` has no vblank phase; a
  repaint starts on demand, no sooner than one refresh period (the mode's maximum rate)
  after the last flip, minus the repaint estimate (niri's VRR clock). Cursor-only commits
  still go out at once (pointer latency over refresh stability, §G of the design).

Measured on vmwgfx (no vblank counter, flags `vsync | hw_completion`, seq 0): 180/180 frames
presented with strictly increasing timestamps 34 ms apart (the SVGA3D host's flip rate),
commit → presented 21 ms on average; weston-presentation-shm reports `[s_e_]`, mpv binds
`wp_presentation` v2 and paces by it.

See [../uml/29-present-feedback-sequence.mmd](../uml/29-present-feedback-sequence.mmd), [../uml/32-kms-atomic-damage.mmd](../uml/32-kms-atomic-damage.mmd).

---

## 8. Timeline semaphore pool

Avoid per-frame `VkSemaphore` create/destroy on explicit-sync paths.

**Module:** `lx.gfx.semaphore_pool` — `timeline_semaphore_pool`, `timeline_semaphore_handle`

Accessible via `gfx::device::semaphores()`.

---

## 9. Content-type hints

Clients set the hint with wp_content_type_v1 (double-buffered with the surface state);
`surface_manager::content_hint_for` reports it:

| Hint | Scanout priority |
|------|------------------|
| `game` | Highest |
| `video` | High |
| `photo` | Default |
| `none` | Default |
| `subsurface_ui` | Lowest — prefer composite |

For `scanout_manager::evaluate` when multiple fullscreen candidates exist (direct scanout is
not implemented yet), and for the connector's `content type` property of the output a window
is fullscreen on, where the connector has one (none on vmwgfx; not programmed yet).

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
| Screen capture (policy, render side, GL objects) | `lx.compositor:capture`, `lx.compositor:capture_engine`, `lx.gfx:gl_capture` |

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
| [42-capture-sequence.mmd](../uml/42-capture-sequence.mmd) | Screen capture: session, job, copy, completion |
