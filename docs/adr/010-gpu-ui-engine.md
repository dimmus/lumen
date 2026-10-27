> **Status:** Accepted. Phases 0–3 implemented: the primitive stream and `lumen-ui-bench`
> (Phase 0), the CPU, GL and Vulkan primitive renderers and the atlas (Phase 1), the
> `lx.text` rewrite with title bars on atlas glyphs (Phase 2), and flex layout, style, the
> theme compiler, One Dark / One Light, Lucide icons and the §5.6 components with
> `lumen-ui-gallery` (Phase 3). Gates G3, G4 and G10 pass; G2 is not met on the
> development host (§7 Phase 3). Next is Phase 4, the client runtime: `lx.wayland.client`
> and `lx.app` are still stubs, so no client draws with the engine yet, and `lx.shell`
> still draws through `shm_canvas` until Phase 5. §2 is the tree as it stood before the
> work began, §7 the order of work with an "As built" note per finished phase, §8 the
> measurement that gates each step.

# ADR-010: A GPU UI engine for Lumen

**Date:** 2026-10-22
**Scope:** `lx.gfx` (new `:primitives`, `:atlas`, `:ui_vk`, `:ui_gl`, `:ui_cpu`; the three
compositors gain primitive kinds), `lx.scene` (`draw_list` carries primitives, not only
blits), `lx.text` (rewritten: faces, itemisation, shaping cache, lines, glyph raster),
`lx.layout` (flexbox with a cache), `lx.ui` (`style`, `theme`, `draw_scope`, concrete
components; the legacy `:widgets` / `:widget_node` and `lx.ui.builder` deleted),
`lx.wayland.client` (real), `lx.app` (window, swapchain, input), `lx.shell` (panel and
launcher move onto `lx.ui`; `shm_canvas` deleted), `lx.compositor` (`:decoration` draws
through the same primitives), `tools/lumen-theme`, `tests/bench_ui.cpp`
(`lumen-ui-bench`), `scripts/ui-bench.sh`, `tests/bench/baseline/`.
**Deciders:** project owner
**Related:** [ADR-005](005-desktop-roadmap.md) Phase 3 · [ui-architecture.md](../subsystems/ui-architecture.md) ·
[rendering-performance.md](../subsystems/rendering-performance.md) · [threading.md](../subsystems/threading.md) ·
[memory-management.md](../subsystems/memory-management.md)

---

## 1. Decision

Lumen gets **one primitive renderer** for every pixel it draws itself: the compositor's
window frames, the shell's panel and launcher, and application windows built on `lx.app`.
Its design centre is GPUI (the framework under Zed), chosen because it is the fastest
desktop UI renderer in production use that is also simple enough to keep correct across
three backends: a flat stream of seven instanced primitive kinds, rounded quads and
shadows evaluated analytically in the fragment shader, glyphs and icons as atlas sprites,
one draw call per run of equal kind. Lumen takes that design and removes the costs GPUI
still pays:

| | GPUI | Lumen |
|--|------|-------|
| Element tree | Rebuilt and laid out every frame | Retained, reconciled by key; only dirty subtrees relayout (`lx.ui` already does this) |
| Repaint | Whole window every frame it draws | Damage rects ∪ buffer age, scissored per rect; damage reported to the compositor so KMS gets `FB_DAMAGE_CLIPS` end to end |
| Per-frame allocation | `Vec` growth, `Arc` churn | Arena descriptors, fixed-capacity streams, persistently mapped instance ring; **zero heap allocations in a steady frame** is a benchmark gate |
| Backends | Vulkan only on Linux (Blade) | Vulkan, GLES 3.1, CPU — the compositor's three, same primitive stream; runs on vmwgfx-class hardware where Zed does not start |
| Handoff to the compositor | Vulkan WSI swapchain | dmabuf with DRM format modifiers from `zwp_linux_dmabuf_v1` feedback, explicit sync via `wp_linux_drm_syncobj_v1`, sampled in place (0.1 ms render-thread cost measured, vs 2.7 ms for shm) |
| Colour | 8-bit sRGB, Hsla per instance | Premultiplied `f16` linear or encoded per window; the UI surface blends in the output encoding (GPUI's look, exactly), the compositor composites windows in linear light as it does today |
| Instance size | Quad ≈ 156 B | Quad 80 B, sprite 64 B, shadow 64 B |

The visual language is Zed's: the One Dark / One Light token set, its elevation and
shadow stack, its 4 px spacing scale with three densities, 14 px UI type on a 16 px rem,
1 px borders, small radii, ghost elements that appear on hover (§5). Themes are read in
Zed's JSON schema, so every published Zed theme works unchanged.

What is explicitly **not** decided here: a fourth backend, compute-shader path rendering
(Vello-style), or a retained GPU display list. Each is deferred until the benchmarks in §8
show the scenes that need it.

---

## 2. Where the tree stood (October 2026, before Phase 0)

The starting point, kept as written; §7's "As built" notes say what changed. Line
references are to the checkout of 2026-10-22.

**F1 — Nothing draws UI on the GPU.** `lx.gfx` composites `blit_command`s (textured or
solid rectangles with a rectangular clip, opacity, tint; `renderer.cppm:19-56`). There is
no rounded corner, border, gradient, shadow, glyph or path in any backend. The Vulkan
compositor issues one `vkCmdDraw(6)` per blit with a 112-byte push constant and no vertex
or instance buffer (`vk_renderer.cppm:275-297, 1898-2016`), rebuilds the whole target every
frame (no damage), and creates its pipeline with no `VkPipelineCache`
(`vk_renderer.cppm:847`). `gfx::pipeline_cache` is a 64-entry key table, not a pipeline
cache (`renderer.cppm:186-225`). `gfx::render_pass` is an empty class
(`lx.gfx.cppm:131-134`); `gfx::glyph_atlas` hands out a new texture id per lookup
(`glyph_atlas.cppm:34-40`).

**F2 — `lx.ui` is the right model with no output.** The declarative core
(`element` → `reconciler` → `ui_node` → `measure/arrange` → `emit(draw_scope&)`) exists and
is tested (`tests/test_ui_reconcile.cpp`). `draw_scope` has two primitives, `quad()` and
`fill()` (`node.cppm:236-263`), and `fill()` never sets `draw_command::solid`, so every
backend skips it (`vk_renderer.cppm:1906-1909`, `cpu_renderer.cppm:545-550`). `text_node`
emits a solid bar (`widgets_decl.cppm:168-172`). `ui_root::tick` re-emits the whole tree
and discards the damage ledger (`root.cppm:59-90`); `reducer_node::rebuild_if_dirty` has no
caller. `style.cppm` keys properties by `const char*`; `theme::load` and
`theme_compiler::compile` return `not_implemented`.

**F3 — No consumer reaches the screen through `lx.ui`.** `lumen-hello` ticks a root into a
local `draw_list` and throws it away. `lx.wayland.client::display::connect` is
`not_implemented`; `lx.app` has no frame loop, buffer or renderer. `lumen-shell` does not
import `lx.ui`: the panel and launcher paint on the CPU through `shm_canvas` (`fill`,
`fill_rounded`, `blit`, `text`) into `wl_shm` buffers, with libwayland-client called
directly. The compositor's title bars are rasterised by `lx.text` on the UI thread and
uploaded as textures (`decoration.cppm:889-947`).

**F4 — Text is single-line, single-face, CPU-only.** `lx.text` holds one `FT_Face` at a
time behind a mutex and reloads on any family/size change (`lx.text.cppm:142-276`);
`shape_line` guesses segment properties (no bidi, no script itemisation, no fallback);
the glyph cache is a 1 MiB coverage arena cleared wholesale at 75 % load; no subpixel
positioning, no colour glyphs.

**F5 — Layout is a stub.** `flex_node::layout` stacks children and ignores alignment,
grow and shrink; `grid_node` overlaps spanned cells; `dirty_` is never read
(`lx.layout.cpp`).

**F6 — The two-toolkit debt from ADR-005 F7 is still there.** `:widgets`, `:widget_node`
and `lx.ui.builder` compile but do nothing.

**F7 — Measurement exists for the compositor, not for UI.** Chrome-trace export
(`LUMEN_TRACE_FILE`), spans on `tick_ui`/`tick_render`, counters, and the hand-measured
tables in rendering-performance.md are compositor-side. `bench_frame_timing` times a CPU
clear of a 64×64 headless buffer. No CTest labels, no stored baselines, no regression
gate. `LX_TRACE_SCOPE` cannot be used twice in one scope (`lx.trace.cppm:125-126`, `##`
blocks `__LINE__`), and `budget_tracker::end_tick_render` is never called.

**F8 — Host for the first measurements.** VMware SVGA3D (vmwgfx): EGL 1.5, GLES 3.1 and
GL 4.3 through Mesa, no hardware Vulkan (lavapipe only, which the compositor correctly
refuses), 2 vCPUs. `perf`, `heaptrack`, fribidi, libunibreak, HarfBuzz, FreeType and
fontconfig are installed; no IBM Plex or Lilex fonts.

---

## 3. Goals and the bar

Targets are gates for §7, measured by §8 on the host in F8 unless a row says otherwise.
They are set from what the compositor already achieves and from what GPUI, egui and GTK4
measure on similar scenes; §8.6 says how each comparison is run.

| # | Metric | Target | Why this number |
|---|--------|--------|-----------------|
| G1 | Idle window (no animation) | 0 wakeups/s, 0 GPU submits, 0 frame callbacks requested | The compositor idles at 0 ticks per 5 s; a client must not undo that |
| G2 | Steady frame, shell panel (≈300 nodes, one clock cell changes) | ≤ 150 µs UI-thread CPU; damage ≤ 1 rect, ≤ 2 % of the surface | A panel repaint today is a CPU `shm_canvas` cell repaint; the GPU path must not cost more CPU than that |
| G3 | 10 000 animated rounded quads | ≤ 1.0 ms CPU (emit + order + encode); ≤ 8 draw calls; ≤ 800 KB instance upload | 80 B per quad; GPUI batches the same scene into a handful of draws |
| G4 | 100 000-glyph text wall, scrolling | ≤ 2.0 ms CPU per frame; 0 shaping calls per frame after warm-up; ≤ 1 draw call per atlas page | Shaping and line layout are cached in retained nodes; scrolling moves sprites |
| G5 | Heap allocations per steady frame | 0 (`heaptrack` flat over 10 000 frames; `mallinfo2` delta 0) | memory-management.md §7; the compositor's hot path is already arena-only |
| G6 | Input → photon, hover highlight in the shell | median ≤ 1 refresh period + 2 ms | drag-smoke gates window moves at 1.5 periods + 4 ms; UI hover has less work |
| G7 | Cold start → first frame, `lumen-hello` | ≤ 30 ms with a warm pipeline cache, ≤ 150 ms cold | The compositor's first frame is 106 ms (rendering-performance.md §6.2) |
| G8 | Anonymous RSS, `lumen-shell` | ≤ 25 MB | Compositor RssAnon 19.1 MB; a client with one atlas page and one swapchain has less state |
| G9 | Instance bytes | quad 80 B, sprite 64 B, shadow 64 B, underline 48 B | §4.2; GPUI's quad is ≈156 B |
| G10 | Pixel parity across backends | per-primitive golden images: max channel error ≤ 2/255 Vulkan vs GL, ≤ 3/255 vs CPU | Three backends must look the same or the theme is three themes |
| G11 | Resize storm (30 resizes, window with text) | heap flat; no frame > 2× median; swapchain reallocation ≤ 1 per size | The compositor passed the same test in PR #19 |

---

## 4. Architecture

### 4.1 One renderer, three consumers

```
                      lx.ui (describe → reconcile → layout → emit)
                                   │ primitives
                                   ▼
              lx.scene::draw_list (primitive stream + order + damage)
                 │                              │
   in-process    │                              │  client process
   compositor    ▼                              ▼
   ┌─────────────────────────┐    ┌────────────────────────────────────┐
   │ composite pass           │    │ lx.app::window                     │
   │ surfaces (client dmabuf) │    │  swapchain: 3 × dmabuf (vk | gbm)  │
   │ + chrome primitives      │    │  or wl_shm (cpu)                    │
   │ → scanout image          │    │ → wl_surface.attach + damage_buffer │
   └─────────────────────────┘    │   + syncobj acquire/release points  │
                                  └────────────────────────────────────┘
                 both call: lx.gfx::{ui_vk | ui_gl | ui_cpu}::draw(target, stream, damage)
```

The backend code is the same object in both processes. What differs is the target (a
scanout image the compositor owns versus a dmabuf the client exports) and the damage
source (scene damage versus the `lx.ui` ledger). Layering is unchanged:
`lx.ui → lx.scene → lx.gfx`; `lx.app` and `lx.shell` sit above `lx.ui`
(architecture.md §3). No Vulkan, EGL or GBM type crosses out of `lx.gfx`
(AGENTS.md §1.1).

### 4.2 Primitive set

GPUI's seven kinds, one content mask (axis-aligned clip) on each, every one an instanced
unit quad expanded from `gl_VertexIndex` with no vertex buffer (the trick GPUI's
`to_device_position` uses, and `composite.vert` already does).

| Kind | Draws | Fragment technique |
|------|-------|--------------------|
| `quad` | Rounded rectangle, per-corner radius, per-edge border width, solid or 2-stop linear gradient, solid or dashed border | Rounded-box SDF; border = inner SDF with widths subtracted; AA = `saturate(0.5 − d)`; dashes from arc length along the perimeter; gradient stops interpolated in sRGB or Oklab (GPUI's two colour spaces) |
| `shadow` | Blurred rounded rectangle, outer or inset | Evan Wallace's closed-form Gaussian over a rounded box (`erf` along x, 4-sample integral along y) — the function GPUI's shadow shader implements |
| `mono_sprite` | Glyphs, icons, any R8 coverage tile × colour | One atlas fetch, `color × coverage^contrast` (§4.6 on contrast) |
| `poly_sprite` | Images, colour emoji; optional grayscale, per-corner radius, opacity | RGBA8 atlas fetch, rounded SDF mask |
| `path` | Vector fill from a triangle list with `st` coverage | Two passes like GPUI: triangles accumulate winding into an R16F coverage tile with additive blend; the tile is drawn as a sprite |
| `underline` | Straight or wavy line of given thickness | Analytic; wavy = sine offset before the SDF |
| `surface` | External texture (client dmabuf, video, captured output) — today's `blit_command` | Unchanged `composite.frag` path (source basis, 8 transforms, decode per `src_transfer`) |

Instance layouts (bytes). Coordinates are output pixels, `f32`; colours premultiplied
`f16×4`, converted once at emission.

```
quad       80  bounds f32×4 │ clip f32×4 │ radii f16×4 │ border_w f16×4 │ bg0 f16×4 │
              bg1 f16×4 │ border_rgba f16×4 │ angle f16, style u8, space u8, pad u32
shadow     64  bounds │ clip │ radii f16×4 │ rgba f16×4 │ sigma f32 │ spread f32 │ flags u32 │ pad
sprite     64  bounds │ clip │ uv u16×4 (atlas texels) │ page u8, flags u8, pad u16 │
              rgba f16×4 │ radii f16×4 (poly) or rotation f16×4 (mono)
underline  48  bounds │ clip │ rgba f16×4 │ thickness f16, wavy u16 │ pad u32
path       64  as sprite; page = coverage atlas; rgba = fill
surface    —   existing blit_command, unchanged
```

`bg1`, `angle` and `space` are the gradient; a solid quad leaves `bg1 = bg0`. A 64-byte
quad with gradients in a side table was considered and rejected: the saving is 16 B per
quad and one indirection per fragment, and G3 is already met at 80 B.

Caps follow memory-management.md §3: `k_max_primitives = 16 384` per view (the
compositor's 4 096 draw cap stays for surfaces), `overflow_action::warn_and_drop`.

### 4.3 Order and batching

GPUI assigns each primitive an `order` from a bounds tree (one more than the highest
order of anything it overlaps), sorts each kind's array by `order`, and walks all kinds
in `(order, kind)` sequence, cutting a batch whenever the kind or the sprite's atlas page
changes. Non-overlapping primitives of one kind therefore batch together even when the
paint order interleaves them with other kinds.

Lumen keeps the rule and replaces the tree with an **overlap grid**: a fixed array of
64 × 64 px cells per view (510 cells at 1920 × 1080, 4 KB), each holding the highest order
drawn into it. An emitted primitive's order is `1 + max` over the cells it covers, then it
writes its order back. No allocation, O(covered cells) per primitive, and a full-surface
primitive costs 510 compares. A `layer` (overlay, popup, drag) pushes a base order so
nothing below can interleave with it — GPUI's `push_layer`.

Then: stable sort of the primitive index array by `(order, kind, page)` (the existing
`batch_for_render` scratch permutation, extended), a single pass that cuts runs, one
instanced draw per run. For the shell panel this is typically 4–6 draws; the gate for G3 is
8.

### 4.4 GPU resources per backend

| | Vulkan (`:ui_vk`) | GLES 3.1 (`:ui_gl`) | CPU (`:ui_cpu`) |
|--|--------------------|----------------------|------------------|
| Floor | Vulkan 1.1 (the compositor's current target; move to 1.3 with ADR-005's D4) | GLES 3.1 — instancing, SSBO, `f16` attributes; the host exposes 3.1 on SVGA3D. GLES 2 hosts get the CPU renderer for UI (the compositor's GLES 2 blit path is untouched) | Row-band pool already in `cpu_renderer` |
| Instances | One host-visible, persistently mapped ring: 3 slots × 2 MB, all kinds packed, `firstInstance` per batch; `vkCmdDraw(6, n, 0, base)` | Same ring in a `GL_SHADER_STORAGE_BUFFER` (3.1) read by `gl_InstanceID`; `glDrawArraysInstanced` | Not applicable: the stream is rasterised directly within damage rects, SDF per pixel for quads/shadows, nearest/bilinear for sprites |
| Pipelines | 7, one layout: set 0 = globals UBO + 16-entry sampler array (atlas pages); push constant = instance base. `VkPipelineCache` persisted to `$XDG_CACHE_HOME/lumen/pipeline-cache-<pipelineCacheUUID>.bin`, replacing the key table in `renderer.cppm` | 7 programs, uniform block for globals, `GL_TEXTURE_2D_ARRAY` per atlas format; program binaries cached with `GL_OES_get_program_binary` | — |
| Surfaces | Descriptor set per imported image as today | `EGLImage` per buffer as today | As today |
| Atlas upload | Staging ring → one `vkCmdCopyBufferToImage` with N regions before the pass | `glTexSubImage3D` per dirty rect, coalesced | memcpy |
| Sync | One timeline semaphore per window (`timeline_semaphore_pool`), value per frame; exported as a syncobj for the compositor | `EGL_ANDROID_native_fence_sync` → sync_file → `drmSyncobjImportSyncFile` for the acquire point; `glFinish` fallback as the GL compositor already does | `wl_shm`, no sync needed |
| Damage | `VK_KHR_dynamic_rendering`-style load; per rect: scissor + clear + draw the batches whose bounds meet it (bounds per batch kept from the sort) | `glScissor` per rect, exactly `gl_compositor::composite_rects` | Clip to rects |
| GPU timing | Timestamp query per batch kind (bench builds only) | `GL_EXT_disjoint_timer_query` when present; fence-wait wall time otherwise | — |

The ring is sized for 25 000 primitives per frame; overflow drops with one warning per
run, never grows. The existing Vulkan compositor's `upload_rgba` (which waits for every
frame slot before each upload) is replaced by the staging ring on the same path.

As built in Phase 1, `:ui_vk` differs from the Vulkan column in these details: the batch's
offset is a push constant (its first word in the ring slot, with its atlas page), not
`firstInstance` — the kinds have different strides, so a batch's offset is not a multiple
of its own; the pipelines are five (`path` shares mono's, as in `:ui_gl`) plus the
compositor's composite pipeline for `surface`, made for the UI pass by
`vulkan_compositor::record_blits`; the sampler array has 8 entries (`k_atlas_pages` = 6 used,
the rest a 1×1 dummy), indexed through a switch because the compositor's device does not
enable `shaderSampledImageArrayDynamicIndexing`; at Vulkan 1.1 the "dynamic rendering-style
load" is a render pass with loadOp LOAD and a `vkCmdClearAttachments` per damage rect; the
timestamps bracket the frame, not each kind. The GLSL is one set of files (shaders/ui/*.glsl)
that `:ui_gl` compiles at run time and `:ui_vk` at build time. `renderer.cppm`'s key table
stays: the compositor's `frame_renderer` still uses it. The compositor's `upload_rgba` still
waits on its frame slots — the staging ring belongs to `:ui_vk` and has not been moved under
the compositor's shm uploads yet. The atlas's dirty lists (and a new page image's "uploaded"
state) are committed only when the frame is submitted; a frame whose surface draw fails to
record inside the pass is still submitted, so the copies and acquires recorded before it run.
`surface` draws record `vulkan_compositor`'s images and descriptor sets into `:ui_vk`'s
command buffers, which the compositor's fences do not cover. As built in Phase 2 both Phase 1
open items are closed: `vulkan_compositor` tracks external users (`add_external_user`: a
`ui_vk_renderer` given a surface compositor registers its `wait_idle`; `record_blits` on a
command buffer other than the compositor's own frame marks the texture, and
`forget_texture`, `upload_rgba` over it and `bind_imported` of another image under its id
wait for every external user first), and a `vkEndCommandBuffer` / `vkQueueSubmit` that fails
after `prepare_blits` un-marks the acquires it recorded (`abandon_prepared_blits`), so the
next frame records them again. The compositor's own Vulkan frames with primitives record
`:ui_vk` inside its linear pass (`ui_vk_renderer::composite`, §4.11), where its fences cover
everything.

### 4.5 Colour and precision

- Theme colours are stored twice at compile time: sRGB `u8×4` for tools, premultiplied
  linear `f16×4` for the renderer. No conversion at emission.
- A client UI window renders into an 8-bit sRGB-encoded swapchain
  (`VK_FORMAT_B8G8R8A8_SRGB` image view; GBM `XRGB8888` + `GL_FRAMEBUFFER_SRGB` off) and
  **blends in the encoded space**. This is what GPUI, browsers and every mainstream
  toolkit do and what the theme's alpha values (`#74ade83d` selections, `0.12` shadows)
  were tuned against; blending the same values in linear light visibly changes them and
  thins text. The compositor still composites *windows* in linear light (§1.1 of
  rendering-performance.md); the two are different layers and both stay correct.
- When a window asks for HDR / wide gamut (`wp_color_management` later), the swapchain is
  `R16G16B16A16_SFLOAT` scRGB, blending is linear, and `mono_sprite` applies a coverage
  contrast exponent (theme `text.contrast`, default 1.0 dark / 1.2 light, validated
  against the sRGB rendering in the golden tests, §8.4). The compositor's own chrome
  renders inside its linear F16 pass and uses the same exponent.
  Measured in Phase 2 (`test_ui_golden` `text_contrast_keeps_stem_coverage`: IBM Plex Sans at
  10–16 px, coverage from the CPU reference, the linear blend composed from it): the starting
  values do not hold the 3 % bound — linear blending thickens One Dark's text by 26 % at
  exponent 1.0 and thins One Light's by 31 % at 1.2. The exponent has to rise for
  light-on-dark and fall below 1 for dark-on-light: 1.58–1.77 (1.67 used) dark and 0.54–0.61
  (0.57 used) light keep the mean stem coverage within 3 % (`gfx::k_text_contrast_dark`,
  `k_text_contrast_light`). The first linear target is the Vulkan compositor's F16 pass
  (§4.11): its light-on-dark title text at 1.67 has a mean stem weight +1.5 % of the CPU
  compositor's encoded blend, +14.7 % at 1.0 (`test_deco_titles`, lavapipe).
- 10-bit output: when `zwp_linux_dmabuf_v1` feedback offers `XRGB2101010` the client
  swapchain uses it; the compositor already scans out A2B10G10R10.

### 4.6 Text

`lx.text` is rewritten as partitions; the public result is a `text_layout` retained on a
`text_node` and invalidated only when text, width, font or scale change.

| Partition | Does | Library | Cache |
|-----------|------|---------|-------|
| `:faces` | Family resolution, face loading, metrics | fontconfig + FreeType | LRU of 32 `(family, weight, style)` faces, each with its `hb_font_t`; `FT_Set_Char_Size` per use, not per load. Bundled fonts registered with `FcConfigAppFontAddDir` |
| `:itemize` | Split a paragraph into runs of one (script, direction, face) | fribidi (UAX #9), HarfBuzz script, fontconfig charset for fallback | Fallback face per `(primary face, script, Unicode block)` |
| `:shape` | Shape one run | HarfBuzz, features from the style | `shape_cache`: 4 096 entries keyed by `(face id, size × scale, features, direction, FNV-1a of the UTF-8)` → glyph ids, advances, offsets, clusters in an arena; LRU |
| `:lines` | Line breaking, ellipsis, alignment, wrap | libunibreak (UAX #14, grapheme breaks for the caret) | Lines live in the node's `text_layout` |
| `:raster` | Glyph → atlas tile | FreeType, unhinted outlines, grayscale AA, `FT_LOAD_COLOR` for CBDT/sbix/COLRv0 emoji | `glyph_cache` keyed by `(face, size, subpixel x variant, glyph)` → atlas tile |

Rasterisation choices, each a gate in §8.4: **4 horizontal subpixel variants**
(GPUI's `SUBPIXEL_VARIANTS = 4`), vertical position snapped to whole pixels; no hinting
by default, light vertical autohint selectable per theme for scales below 1.5; LCD
subpixel AA deferred (needs `wl_output.subpixel` and dual-source blending, listed in §7
Phase 6). Emoji go to the RGBA8 page as `poly_sprite`.

Atlas (`lx.gfx:atlas`, replacing `glyph_atlas.cppm`): shelf packing with bucketed
shelves (the étagère algorithm GPUI uses), pages of 1024² growing to 2048², at most 4 R8
and 2 RGBA8 pages; a page above 85 % occupancy with stale tiles is rebuilt from its live
set on the worker thread and swapped at the next frame. Each tile records the last frame
it was drawn; eviction never touches a tile used in the last 120 frames (the import
cache's stale threshold).

As built in Phase 1: the rebuild is synchronous on the UI thread, not on the worker, and
runs at `atlas::begin_frame`. An `allocate` that finds no room frees stale tiles in place
only — a rebuild moves live tiles, and a uv looked up earlier in the same frame must keep
naming its pixels — and marks a page above 85 % for the rebuild at the next frame (0.6–1.3 ms
measured, rendering-performance.md §6). The worker rebuild into a shadow page swapped at
`begin_frame` is left for when S4 shows the synchronous one on a frame.

As built in Phase 2 (`:itemize`, `:lines`): libunibreak is called from `:itemize`, which decodes
each paragraph once and computes its bidi levels, scripts, UAX #29 grapheme and UAX #14 break
flags in one pass; `:lines` consumes the flags. fribidi is skipped for a paragraph with nothing
right-to-left in it. The fallback key's "Unicode block" is a fixed slice of 128 code points,
negative answers included; a cluster of Common / Inherited characters stays in the face of the
run before it. A line cuts a run's glyphs at a cluster boundary instead of reshaping the two
halves (the shape cache does not keep HarfBuzz's unsafe-to-break flags), so a re-wrap at any
width shapes nothing; a forced break inside a word or an ellipsis cut keeps the glyph forms the
whole word was shaped with. With `max_lines` the ellipsis cut stays within the last line's
wrapped extent (as CSS `line-clamp`); an unwrapped line is cut at any grapheme. The retained
`text_layout` replaces the old one-line class of that name (same setters, `bounding_size` now
counts every line); `wrap_text` / `render_wrapped` keep their own path for the shell until
Phase 5. Measured on an S4-like wall (2 000 paragraphs of Latin / Cyrillic / Arabic, 142 KB,
97 k glyphs; `test_text_lines`, load 0.7, five runs): cold layout 28–54 ms, a fresh layout over
a warm shape cache 15–21 ms p50 (itemization 7–9 ms of it, no shaping calls), re-wrap at a new
width 0.49–0.67 ms p50 / 0.8–4 ms p95 with no shaping; 6.9 MB of layout storage (vector
growth slack included). A whole-document relayout is therefore worker work (below); scrolling
does not relayout.

As built in Phase 2 (`:raster`, `lx.ui:text_node`): `lx.text` depends on `lx.gfx` for the atlas
(tile ids and `atlas::insert` / `lookup` / `touch` only). `glyph_cache` is a set-associative
table (16 384 entries, 8 ways; a full set replaces the way the atlas saw drawn longest ago and
releases its tile only when the atlas already counts it stale, else leaves it to the 120-frame
aging, since retained emitters touch the atlas, not the table) keyed by (face, size × scale, variant, glyph, hinting); the
pen rounds to the nearest quarter pixel (`glyph_pen`), the variant moves the outline before
FreeType renders it; colour glyphs are rasterised once at variant 0 and answer every variant;
a tile the atlas evicted is rasterised again on the frame that needs it. The text node is
`text_block_node` (the placeholder `text_node` of `:widgets_decl` keeps its name until Phase 3):
it binary-searches the lines the clip reaches (widened by an em) before touching a glyph, takes
the face lock once per emit and only on a miss, and emits through `draw_scope::glyph` with
colours packed once per run. The golden text cases use only bundled fonts plus two subsets in
`tests/fonts` (IBM Plex Sans Arabic and Twemoji Mozilla COLRv0, renamed): CPU, GL and Vulkan
(lavapipe) agree within 2/255.

Shaping and rasterising happen on the **worker** affinity when a layout invalidation is
large (a whole document) and inline on the UI thread when it is small (a label), with the
boundary set by measurement in §8.3 S4. As built: the boundary is 4 KiB of text (a cold
reshape costs ~0.21 µs a byte here; S4's whole document 31–38 ms); shaping and wrapping move to
the worker, glyph rasterising stays on the UI thread where the atlas lives (§7 Phase 2, as
built).

### 4.7 Layout

`lx.layout:flex` implements the CSS flexbox subset Taffy gives GPUI: direction, wrap,
`justify-content`, `align-items`/`align-self`, `gap`, `flex-grow`/`shrink`/`basis`,
`min`/`max`, padding, margin, border box, `position: absolute` with insets, `overflow:
hidden | scroll`. Grid is a later partition. The engine works on `ui_node`'s existing
two-pass `measure(constraints, size_hint)` / `arrange(bounds)` contract and keeps the
rule that `measure` is pure.

Caching, which GPUI does not have because it rebuilds the tree: each `ui_node` keeps a
4-entry `(constraints, size_hint) → size` cache (Yoga's shape), cleared by
`invalidate_layout`; dirtiness propagates to the root; `arrange` of a clean subtree with
unchanged bounds is skipped. G2 depends on this: a clock-cell change relayouts one cell.

### 4.8 Style, tokens and the theme compiler

`style.cppm`'s string-keyed properties are replaced by a packed `style` value (≈96 B)
carried in element props, with a GPUI-shaped builder so a description reads like GPUI:

```cpp
box(s.children)
  | styled(style{}.flex_row().gap(sp::b08).px(sp::b12).h(px(32))
                  .bg(tok::surface_background).border_1(tok::border_variant)
                  .rounded(radius::md).shadow(elevation::elevated_surface))
```

- `tok::*` are `u16` indices into the theme's colour table; `sp::*` is the spacing scale
  (§5.3) resolved through the density; lengths are 32-bit `{unit:2, value:30}` (px, rem,
  percent, auto).
- `tools/lumen-theme` compiles a theme JSON in **Zed's schema**
  (`https://zed.dev/schema/themes/v0.2.0.json`) into `.lxtheme`: a header, the colour
  table in both encodings, the status and player tables, syntax styles, font settings.
  `theme::load` is one read and a bounds check; `theme_compiler::compile` loses its
  `not_implemented`. Debug builds hot-reload on inotify from the worker thread.
- Components (§5.6) are `ui_node`s in `lx.ui:components`, above the primitives, as
  ui-architecture.md §9 already asks.

### 4.9 Frame loop and damage (client window)

```
event (wl_pointer / wl_keyboard / text-input / timer)
  → action → reducer → invalidate_{paint,layout,animate}
frame due (wl_callback, only requested when the ledger is non-empty or something animates)
  → arena reset, describe, reconcile (nodes reused by key)
  → layout dirty subtrees (§4.7)
  → emit whole tree into the primitive stream (v1; §7 Phase 6 measures subtree caching)
  → order (grid), sort, cut batches; write instances into the ring slot
  → damage = ledger ∪ age(buffer)         (buffer age per swapchain image, as the compositor's scanout slots)
  → acquire buffer (release point signalled, or wl_buffer.release)
  → record: per damage rect scissor + batches that meet it; submit; export sync
  → wl_surface.attach, damage_buffer × rects, set_acquire_point, commit
  → ledger cleared only after a successful commit
```

Idle rule (G1): no ledger entries and no animation → no frame callback, no submit, no
timer. Animations run off the frame callback, never off a free-running timer.
`invalidate_animate` requests exactly one next frame.

As built (Phase 3): the emit need not be the whole tree. `ui_root::set_emit_scope(
emit_scope::damage)` clips the root to the frame's damage, so a consumer that keeps its last
frame emits, orders and encodes only what it repaints (S2's clock frame: 7 nodes walked, 10
primitives, instead of 141). With it the buffer's age is known before the tick, not after:
acquire → age → `ui_damage().add(age damage)` → tick → record per rect. `emit_scope::full`
remains the default.

### 4.10 Client pixel path

Selected once per window in `lx.app`, the same order and the same refusal rules as the
compositor's `present_backend::automatic`:

1. **Vulkan on the render node** (hardware device only; lavapipe refused). Formats and
   modifiers from `zwp_linux_dmabuf_v1` feedback for the compositor's main device; images
   created with `VK_EXT_image_drm_format_modifier`, exported with
   `VK_EXT_external_memory_dma_buf`; acquire and release points on
   `wp_linux_drm_syncobj_surface_v1` from the window's timeline semaphore
   (`VK_KHR_external_semaphore_fd` as syncobj). The compositor already implements the
   syncobj protocol (ADR-009 Part 1) and imports through `dmabuf_import_cache` by
   `(client, buffer, modifier)`, so a re-committed swapchain image is a cache hit.
2. **GLES 3.1 on GBM** (declines a software `GL_RENDERER`). `gbm_bo_create_with_modifiers2`
   from the same feedback, `EGLImage` → renderbuffer → FBO; native fence → sync_file →
   syncobj acquire point; implicit sync fallback.
3. **`wl_shm` + CPU renderer.** Two buffers, `damage_buffer` per rect. This is also the
   path for CI golden images.

Three images per swapchain, as the compositor's three scanout slots and for the same
reason (rendering-performance.md §6.1). Fractional scale via `wp_fractional_scale_v1` +
`wp_viewport`; the UI lays out in logical pixels and `draw_scope` projects once at
emission exactly as `scene::output_projection` does. `wp_presentation` feedback is
consumed for the latency measurement in §8.3 S9.

### 4.11 Compositor-side use

`compositor::frame_node` (server-side decorations) emits `quad`, `shadow` and
`mono_sprite` primitives instead of textured blits of CPU-rasterised titles; the title
goes through `lx.text` into the compositor's atlas, 1:1 at the output scale as today
(rendering-performance.md §Output scale). `cursor_node` and `fullscreen_backdrop` are
untouched (`surface` and solid `quad`). `compositor_impl.cpp:8574-8593` stops converting
`draw_command` to `blit_command`: the backends take the stream. The visibility pass keeps
treating only radius-0, alpha-1 quads as occluders.

As built in Phase 1 (`lx.compositor:ui_composite`, rendering-performance.md §6 "UI primitives
in the compositor"): `frame_node`'s border and title fills are quads (the same rects and
colours as the swatch blits, pixel-identical on the CPU compositor); the title and close
button are still blits, and no shadow or rounding was added (a visual change, after Phase 2).
A snapshot without primitives keeps the blit conversion and the backends' old calls
unchanged; with primitives, CPU and GLES 3.1 draw the stream through `:ui_cpu` / `:ui_gl` in
the compositor's target and damage. The Vulkan composite, GLES 2 contexts and screen
capture's re-composites take the stream *lowered* to blits (`gfx::lower_to_blits`: solid
square quads become solid blits, other primitives are dropped, counted and warned about):
`:ui_vk` does not yet record into `vulkan_compositor`'s linear F16 pass — that, with
linear instance colours, is the remaining step for primitives that do not lower. Occluders
are quads that lower (radius 0, borderless, one colour, integral rects) at alpha 1;
primitives are never culled.

As built in Phase 2 (`lx.compositor:decoration`, `:ui_composite`; rendering-performance.md §1.2
"Server-side frames", §6): the title text and the close glyph are `mono_sprite`s in the
compositor's own atlas (`chrome_atlas`: one `gfx::atlas` for every output, written by UI under
its mutex, read by the render thread's renderer under it). `chrome_text` shapes and fits a
title with the single-line `lx.text` API — the CPU titles' font, ellipsis and not-responding
rules, placement unchanged — and rasterises its glyphs through a `text::glyph_cache` (4
subpixel variants, light hinting below 1.5) once per title change and per output scale the
frame is on (`decoration_manager::set_window_scales`, up to 4 runs): each output draws its own
scale 1:1, a view of another scale resamples the largest run. The close glyph is the analytic
"×" rasterised per scale (not a font glyph: no font on every host has the same one); the
hover / pressed circle stays a blit. The per-window CPU title textures, their upload and their
texture ids are gone. The Vulkan compositor no longer lowers: `ui_vk_renderer::composite`
records the stream inside `vulkan_compositor`'s linear F16 pass (`begin_linear_frame`,
`begin_linear_pass`, `end_linear_frame`), with instance colours decoded to premultiplied linear
light and glyph coverage raised to `k_text_contrast_dark` by the shaders under a push-constant
flag; frames without primitives are `composite` byte for byte. Screen capture's
re-composites (`paint_cpu_capture`, `paint_gl_capture`: the crop moves the primitives too) and
the headless dump (`paint_headless`) draw the stream; only GLES 2 still lowers, dropping the
glyphs. On a turned output the view marks each sprite with its transform and the renderers
sample the tile back through `source_basis_for`. Dimmed frames' fills and glyphs carry one
byte rule on every backend — the Phase 1 disagreement between dimmed fills and dimmed title
blits on GL and Vulkan is gone. Measured (`test_deco_titles`): GL within 1/255 of the CPU
compositor, Vulkan (lavapipe) within 1/255 outside the text and 9/255 on glyph edges with the
stem weight +1.5 %; render-thread cost on CPU +3–5 % for a full repaint, GL unchanged within
noise; no frame allocates.

### 4.12 Threads and allocation

| Work | Affinity | Allocation |
|------|----------|------------|
| describe, reconcile, layout, emit, order, encode | `ui` | UI arena (descriptors, shape results), fixed streams; retained nodes `new` only on shape change (unchanged) |
| record + submit, atlas copy | `render` in the compositor; `ui` in a client (one window, one queue; a render thread for clients is not justified until S3 shows a submit cost above 300 µs) | Ring slot, no allocation |
| shaping, glyph raster, SVG raster, page rebuild, theme reload | `worker` | Worker arena; results posted with `executor.post(ui, …)` |

`assert_affinity()` at `draw_scope` construction, `ui_vk::draw` entry and
`atlas::rasterize` entry. As built in Phase 1: the renderers' affinity depends on the
consumer, so `ui_vk_renderer` and `ui_gl_renderer` assert at `draw` and `wait_gpu_time`
entry against an owner the consumer binds with `set_owner_affinity` (the compositor binds
`ui_gl` to `render` after its start-up warm draw; `any`, the default, checks nothing — tests
and the bench); the atlas asserts `ui` at `allocate`, `write`, `release`, `rebuild` and
`begin_frame` (there is no `rasterize` before Phase 2). Every per-frame container is fixed-capacity with an
`overflow_action` (memory-management.md §3): primitive stream, batch list (≤ 1 024), damage
ledger (16), overlap grid, instance ring, atlas upload list (≤ 256 regions).

### 4.13 Partition map and layer check

| New / changed | Layer | Depends on |
|---------------|-------|-----------|
| `lx.gfx:primitives` (instance structs, packing helpers, `order_grid`), `:atlas`, `:ui_vk`, `:ui_gl`, `:ui_cpu` | Graphics | `lx.foundation`, `lx.runtime` |
| `lx.text:faces`, `:itemize`, `:shape`, `:lines`, `:raster` | Graphics | `lx.gfx:atlas` (tile handles only) |
| `lx.layout:flex` | Graphics | `lx.foundation` |
| `lx.scene` (`draw_list` of primitives, one overlap grid per list) | Compositor | `lx.gfx` |
| `lx.ui:style`, `:theme`, `:draw_scope`, `:components`, `:text_node` | Desktop | `lx.scene`, `lx.text`, `lx.layout` |
| `lx.wayland.client` (real), `lx.app:window`, `:swapchain`, `:input` | Platform / Application | `lx.gfx` (through `lx.scene`), `lx.ui` |
| `lx.shell` (panel, launcher on `lx.ui`) | Desktop | `lx.ui` |
| `tools/lumen-theme` | tool | the `lx.ui` facade (its `:theme`, `:theme_compile` and `:theme_defaults` partitions; see Phase 3's deviations) |

The overlap grid (`order_grid`) is a geometry utility with no scene dependency, so it lives
in `lx.gfx:primitives` beside the structs whose `order` it assigns; `lx.scene::draw_list`
owns one instance per list and is what emitters take orders from.

No upward edge is introduced; `cmake/LumenCXXModules.cmake` will reject one at configure
time if it is.

---

## 5. Visual language: GPUI as Zed ships it

Values below are Zed's current defaults (One Dark / One Light theme files,
`crates/ui/src/styles`, `assets/settings/default.json`), captured 2026-10-22. They are the
initial content of `assets/themes/one-dark.json` and `one-light.json` and of the
`lx.ui:components` defaults; the theme compiler accepts any other Zed theme.

### 5.1 Colour tokens

| Token | One Dark | One Light | Used for |
|-------|----------|-----------|----------|
| `background` | `#3b414d` | `#dcdcdd` | App background, status bar, title bar |
| `surface.background` | `#2f343e` | `#ebebec` | Panels, panes, tab bar, inactive tab |
| `elevated_surface.background` | `#2f343e` | `#ebebec` | Popovers, menus, tooltips, modals |
| `editor.background` | `#282c33` | `#fafafa` | Editable areas, active tab, toolbar |
| `element.background` | `#2e343e` | `#ebebec` | Buttons, inputs |
| `element.hover` | `#363c46` | `#dfdfe0` | |
| `element.active` / `.selected` | `#454a56` | `#cacaca` | |
| `element.disabled` | `#2e343e` | `#ebebec` | |
| `ghost_element.background` | transparent | transparent | Toolbar buttons, list rows: invisible until hovered |
| `ghost_element.hover` / `.active` / `.selected` | `#363c46` / `#454a56` / `#454a56` | | |
| `border` | `#464b57` | `#c9c9ca` | Panel edges, dividers |
| `border.variant` | `#363c46` | `#dfdfe0` | Subtle dividers, popover edges |
| `border.focused` | `#47679e` | `#7d82e8` | Focus ring |
| `border.selected` | `#293b5b` | `#cbcdf6` | |
| `border.disabled` | `#414754` | `#d3d3d4` | |
| `text` | `#dce0e5` | `#242529` | |
| `text.muted` | `#a9afbc` | `#58585a` | Secondary labels |
| `text.placeholder` / `.disabled` | `#878a98` | `#7e8086` | |
| `text.accent` | `#74ade8` | `#5c78e2` | Links, active items |
| `icon` / `.muted` / `.disabled` / `.accent` | as `text.*` | as `text.*` | |
| `drop_target.background` | `#83899480` | | |
| `scrollbar.thumb.background` | `#c8ccd44c` | | Thumb; `thumb.border #363c46`, track transparent, `track.border #2e333c` |
| `search.match_background` | `#74ade866` | | |
| `error` / `.background` / `.border` | `#d07277` / `#d072771a` / `#4c2b2c` | `#d36151` / `#fbdfd9` / `#f6c6bd` | |
| `warning` (= `modified`, `conflict`) | `#dec184` / `#dec1841a` / `#5d4c2f` | `#a48819` / `#faf2e6` / `#f4e7d1` | |
| `success` (= `created`) | `#a1c181` / `#a1c1811a` / `#38482f` | `#669f59` / `#dfeadb` / `#c8dcc1` | |
| `info` (= `renamed`) | `#74ade8` / `#74ade81a` / `#293b5b` | `#5c78e2` / `#e2e2fa` / `#cbcdf6` | |
| `hint` | `#788ca6` / `#5a6f891a` / `#293b5b` | `#7274a7` / `#e2e2fa` / `#cbcdf6` | |
| `hidden` / `ignored` / `unreachable` | `#878a98`, `#878a98`, `#a9afbc` | | |
| `predictive` | `#5a6a87` | `#9b9ec6` | Ghost text |
| `players[0..7]` cursor | `#74ade8 #be5046 #bf956a #b477cf #6eb4bf #d07277 #dec184 #a1c181` | | Selection = same at alpha `3d` |
| `terminal.ansi.*` | black `#282c34`, red `#e06c75`, green `#98c379`, yellow `#e5c07b`, blue `#61afef`, magenta `#c678dd`, cyan `#56b6c2`, white `#abb2bf` (+ bright/dim variants in the file) | | |

Syntax colours (keyword `#b477cf`, string `#a1c181`, function `#73ade9`, type
`#6eb4bf`, comment `#5d636f`, number/boolean `#bf956a`, property `#d07277`, …) ride along
in the same table for a future editor component and the terminal.

### 5.2 Elevation and shadows

| Elevation | Background token | Shadows (offset, blur, black at alpha) |
|-----------|------------------|-----------------------------------------|
| `background` | `background` | none |
| `surface` | `surface.background` | none |
| `editor_surface` | `editor.background` | none |
| `elevated_surface` (popover, menu, tooltip) | `elevated_surface.background` | (0, 2) blur 3 α 0.12; (0, 1) blur 0 α 0.06 dark / 0.03 light |
| `modal_surface` | `elevated_surface.background` | (0, 2) blur 3 α 0.12 dark / 0.06 light; (0, 3) blur 6 α 0.08 / 0.06; (0, 6) blur 12 α 0.04; (0, 1) blur 0 α 0.12 / 0.04 |

Elevated and modal surfaces carry a 1 px `border.variant` border. These map one to one
onto `shadow` primitives (one per entry) and a `quad`.

### 5.3 Spacing

Zed's `DynamicSpacing`, in px at the default rem, three densities:

| Step | compact | default | comfortable |
|------|---------|---------|-------------|
| b00 / b01 / b02 / b03 | 0 / 1 / 1 / 2 | 0 / 1 / 2 / 3 | 0 / 2 / 4 / 4 |
| b04 / b06 / b08 | 2 / 3 / 4 | 4 / 6 / 8 | 6 / 8 / 10 |
| b12 / b16 / b20 | 10 / 14 / 18 | 12 / 16 / 20 | 14 / 18 / 22 |
| b24 / b32 / b40 / b48 | 20 / 28 / 36 / 44 | 24 / 32 / 40 / 48 | 28 / 36 / 44 / 52 |

### 5.4 Typography

| | Value |
|--|-------|
| rem | `ui_font_size` = 16 px; everything scales with it |
| UI text | `TextSize::Default` 14 px (0.875 rem); `Small` 12; `XSmall` 10; `Large` 16. Line height is GPUI's `TextStyle` default, φ = 1.618 relative (`phi()`), unless a component sets its own |
| Headlines | 0.88 / 1.0 / 1.125 (default) / 1.27 / 1.43 rem, line height 1.6 rem |
| Buffer (editor, terminal) | 15 px, line height `comfortable` 1.618 (`standard` 1.3) |
| Weights | 400 regular; 700 for `emphasis.strong`; italic for links and predictive text |
| UI family | Zed ships "Zed Plex Sans" (an IBM Plex Sans fork, OFL). Lumen bundles **IBM Plex Sans** under `assets/fonts/` with fallback to the fontconfig `sans-serif` match (DejaVu Sans on this host) |
| Mono family | Zed's `.ZedMono` aliases **Lilex** (OFL); Lumen bundles Lilex; JetBrains Mono (installed here) is the first fallback |

### 5.5 Shape, borders, iconography, motion

- Radii, GPUI's scale on a 16 px rem (`gpui_macros` `corner_suffixes`): `xs` 2 px
  (badges, key hints, the checkbox), `sm` 4 px (buttons, list rows), `md` 6 px (text inputs:
  input_field.rs `rounded_md`), `lg` 8 px (every elevated surface — popovers, menus,
  tooltips, notifications, modals: styled_ext.rs `elevated` rounds `lg` for `elevation_1`,
  `_2` and `_3`), `xl` 12 px, `full` for pills. Tabs, panels and dividers are square.
  *Corrected at Phase 3's gate:* this list first gave popovers, tooltips and menus `md` and
  inputs `sm`; Zed rounds the first three `lg` and inputs `md` (zed a5c4ca8).
- Borders are 1 px: `border` between regions, `border.variant` inside a region,
  `border.focused` as the focus ring (no glow, no offset). The switch is the exception: a
  2 px border, transparent until focused (toggle.rs `border_2`).
- Icons are 16 px monochrome (`IconSize`: 10 px `indicator`, 12 px `xsmall`, 14 px `small`,
  16 px `medium`, 48 px `xlarge`), tinted with `icon.*`. Zed's
  own set is bespoke; Lumen bundles **Lucide** (ISC) at the same sizes and rasterises each
  `(icon, size, scale)` once on the worker into the R8 atlas (`librsvg` under
  `LUMEN_HAS_RSVG`, as `lx.shell` already links it).
- Motion is restrained: popovers and tooltips fade in over 100–150 ms ease-out, tooltips
  after a 500 ms hover delay; nothing bounces; `reduced_motion` (already in `focus_style`)
  removes all transitions.
- Hover is a background change only (`element.hover` / `ghost_element.hover`), never a
  colour change of the label — Lumen's rule. Zed departs from it in one place: a
  `ButtonStyle::Transparent` button turns its label and icon `Color::Muted` while hovered
  (button_like.rs `hovered`); Lumen keeps the label. Selected rows use
  `ghost_element.selected`; the keyboard focus ring and the selection are separate states.
- Scrollbars: Zed hides them 1 s after scrolling stops (`ShowScrollbar::Auto`, a 400 ms fade);
  Lumen's show whenever the content overflows (Phase 3; autohide is open).

### 5.6 Component set (first release)

`label`, `headline`, `icon`, `button` (filled, outlined, ghost, with icon), `icon_button`,
`toggle`, `checkbox`, `switch`, `text_input` (caret, selection, IME through
text-input-v3), `list` + `list_item` (keyboard navigable, virtualised past 100 rows),
`scroll_area` with the Zed scrollbar, `tab_bar` + `tab`, `divider`, `tooltip`,
`popover_menu`, `context_menu`, `modal`, `toast`, `progress`, `panel` (toolbar + body),
`status_bar`, `key_binding` hint, `avatar`. The shell panel and launcher are built only
from these.

---

## 6. Comparison

Where Lumen matches the field and where it goes past it, per the project's bar; §8.6
turns the claims into numbers on the same host.

| Engine | Model | What Lumen takes | Where Lumen goes further |
|--------|-------|------------------|---------------------------|
| **GPUI** (Zed) | Immediate element tree, Taffy, 7 instanced kinds, SDF quads, erf shadows, atlas sprites, (order, kind) batching, Blade/Vulkan | All of the renderer design and the visual language | Retained tree with cached layout; damage + buffer age; allocation-free frames; GL and CPU backends; dmabuf + explicit sync; HDR-capable `f16` path; Zed themes without Zed |
| **egui** | Immediate mode, one textured-triangle mesh, tessellated rounding | Nothing in the renderer | Analytic AA (no tessellation), text in an atlas with subpixel variants, idle at 0 frames, batches by kind rather than one mesh per frame |
| **Flutter / Impeller** | Retained layer tree, Vulkan/Metal, stencil-cover paths, SDF text optional | Damage via layer raster cache | Lumen needs no raster cache because primitives are cheap; Impeller's per-draw uniform binding is replaced by instancing |
| **GTK 4** (ngl / Vulkan) | Retained render-node tree, diffed per frame, rounded-rect and shadow shaders, glyph atlas | Node-diff damage is the same idea as the ledger | GTK re-uploads and rebinds per node; Lumen instances. GTK blends in sRGB too |
| **Qt Quick** | Scene graph, batched geometry, distance-field text | Batching across the tree | Qt tessellates rounded rectangles and uses distance-field glyphs (soft at small sizes); Lumen's quads are analytic and glyphs are rasterised per subpixel variant |
| **Slint** | Retained, FemtoVG/Skia or software | Software renderer for small devices | Same set of primitives on all three backends with golden parity |
| **WebRender** | Retained display list, GPU cache textures, picture caching | The idea that instances stay small | Not needed: a desktop UI has 10³–10⁴ primitives, not 10⁶ |
| **Vello** | Compute rasteriser, arbitrary paths | Nothing yet | Deferred: UI scenes are quads and glyphs; revisit if S12 (paths) matters |

Honest gaps: GPUI's element library and Zed's component polish are years ahead; Lumen's
first component set is §5.6. GPUI's subpixel (LCD) sprite kind is deferred here. Impeller
and Skia have better arbitrary-path quality than the winding-tile approach.

---

## 7. Realization plan

Each phase ends with its gate from §3 green on the host in F8 (GL and CPU backends) and
the Vulkan backend green under lavapipe for correctness only. Effort is a rough size for
one focused developer; it is an ordering aid, not a commitment.

### Phase 0 — Foundations and measurement first (≈1 week) — **done**

- `lx.gfx:primitives`: the structs of §4.2, `static_assert`ed sizes, packing helpers
  (`f16`, premultiply once), and the overlap grid `order_grid` (§4.13).
- `lx.scene::draw_list` v3: primitive stream + index array, its own overlap grid,
  `(order, kind, page)` sort reusing the `order_` scratch; `surface` kind wraps today's
  `draw_command` unchanged so the compositor keeps working.
- Fix `draw_scope::fill` (set `solid`), `LX_TRACE_SCOPE` (`__COUNTER__`), call
  `budget_tracker::end_tick_render`; add spans `ui.describe`, `ui.layout`, `ui.emit`,
  `ui.encode`, `ui.submit` and counters `ui.primitives`, `ui.batches`, `ui.instance_bytes`,
  `ui.damage_px`, `ui.atlas_upload_bytes`.
- `tests/bench_ui.cpp` → `lumen-ui-bench` with the JSON schema of §8.5, scenarios S1–S3
  running against the CPU reference renderer; `scripts/ui-bench.sh`; CTest label `bench`
  excluded by the default test presets, `ctest --preset bench` runs it; `tests/bench/baseline/<host>.json` committed.
- Gate: G9 by `static_assert`; the bench prints and stores numbers.

### Phase 1 — The primitive renderer (≈3 weeks) — **done**

- `lx.gfx:ui_cpu` first (the reference: SDF per pixel, exact, slow is fine), then
  `:ui_vk` (7 pipelines, instance ring, `VkPipelineCache` on disk, timestamp queries),
  then `:ui_gl` (GLES 3.1 SSBO + instancing, program binary cache).
- `lx.gfx:atlas` with shelf packing, page rebuild on the worker, staged uploads.
- Golden-image tests per primitive per backend (§8.4) with the parity thresholds of G10;
  the sRGB-space blending of §4.5 checked against a reference PNG rendered by the CPU path.
- The three compositors accept the stream: `quad` and `shadow` reach the screen through
  `frame_node` for server-side decorations (title text stays CPU-rasterised until Phase 2).
- Gate: G3 and G10 on GL and CPU here; Vulkan parity on lavapipe.

As built: G10 is met on CPU, GL and Vulkan (lavapipe), and G3's ≤ 8 draws and ≤ 800 KB
hold (one draw, 800 080 B). G3's CPU time closed at 1.13 ms p50 / 2.42 ms p95 with
`--backend none`. Emit was two thirds of that (0.83 ms p50), and was carried into Phase 2.

The Phase 2 pass on emit, order and encode measured each change A/B, interleaved, on this
host (2 vCPU, bench pinned to CPU 1, `setarch -R`, the 1 ms frame clock), with load
checked before each run. S3, µs, median over the rounds:

| Change | emit+order+encode p50 / p95 | emit | order | encode | layout |
|--------|----------------------------|------|-------|--------|--------|
| Phase 1 (inline 64-slot child array, 640 B nodes) | 1220–1437 / 2372–3327 | 870 | 115 | 150 | 420 |
| Child pointers out of line (pooled spans) | 800 / 1600 | 470 | 123 | 125 | 160 |
| Inline `orders()`, one-cell `order_grid::assign`, `push_quad` | 700 / 1400 | 375 | 123 | 140 | 170 |
| One kind on one page, no surface: one batch in push order, no sort | 524 / 1289 | 381 | 31 | 102 | 168 |
| Instance written in place (`append_quad`), clip floats cached per scope | 482 / 1097 | 346 | 31 | 97 | 168 |
| Inline 2 × 2-cell grid path; stream aliases the push order, no copy | 459 / 1091 | 330 | 18 | 98 | 162 |
| Nodes and spans from one retained-tree pool | 451 / 1161 | 324 | 18 | 95 | 146 |

Two things tried and dropped, for no gain: an inline `draw_scope::quad`, and
non-temporal stores in `encode_instances`. The second saved 14 µs, but only on the bench's
own copy (the backends gather through `ui_encode`).

**G3 p95 still fails on this host under the frame clock**: p50 is about 0.45 ms, p95 1.05–1.16 ms.
p95 runs about 2.4× p50 on every stage, including the bench's compute-only `update`, so
the gap is host jitter: the vCPU sleeps between frames and wakes with cold caches. It is
not one slow path. Without the clock (`--period-us 0`) S3 is 0.40 ms p50 / 0.82 ms p95,
which passes. Warm, the emit costs 18 ns per quad at 1 000–3 000 quads and 26 ns at
10 000, once the working set (≈ 1.4 MB of nodes, 800 KB of instances, 800 KB of ring)
outgrows what the host leaves of the L3. The baseline was not refreshed with these
numbers, so `scripts/ui-bench.sh` still warns while the gate fails in both.

### Phase 2 — Text (≈3 weeks) — **done**

- `lx.text` partitions of §4.6; `text_node` emits `mono_sprite` runs from a retained
  `text_layout`; emoji as `poly_sprite`; `underline` for links and spell marks.
- Shape cache, face LRU, fallback itemisation, UAX #14 breaking, ellipsis; bidi correct
  for Arabic and Hebrew test strings in `tests/test_text_lines.cpp`.
- Title bars switch to atlas glyphs. `lx.shell`'s `shm_canvas::text` is left until Phase 5.
- Gate: G4 (S4) on GL; `test_text_raster` and `test_text_shaping` extended, not replaced.

As built (S4 and G4; `lumen-ui-bench --scenario S4`, tests/bench/baseline/iritur-none.json):
S4 is one `text_block_node` holding 2 000 seeded lines of 50 glyphs (Latin / Cyrillic / Arabic
words, IBM Plex Sans 14 px, line height φ, Arabic from the host's DejaVu Sans fallback; 139 375
B, 99 771 glyphs, 7.9 MB of layout) under a scroll node that moves its origin 3 px a frame at
1920 × 1080. The first frame lays the document out inline (57–75 ms, 3 673 shaping calls, 402
glyphs rasterised, 40 KB uploaded); a steady frame lays nothing out and emits the ~50 lines its
clip reaches: 2 096 mono sprites, **0 shaping calls**, 45 glyphs rasterised over 1 800 frames
(new subpixel variants as lines scroll in; 3 740 atlas bytes in all, at most 318 in a frame),
two batches — the background quad and **one sprite batch on the one R8 page (one draw call on
GL)**, 128 912 instance bytes — and no heap allocation. G4's sum (layout + emit + order +
encode) with `--backend none`, load 0.4–0.7, three runs: p50 153–156 µs, p95 1.08–1.20 ms;
beside the GL backend (two runs, the gate): p50 169 µs, p95 0.46–0.52 ms (`submit` p50 0.45 ms,
`gpu` fence-wall p50 4.7 ms); frames back to back (`--period-us 0`): p50 81 µs, p95 0.18 ms.
**G4 passes on GL and on `none`**, but the `none` p95 is 7× its p50 and that tail is the host:
at the 1 ms frame clock the bench idles ~0.85 ms a frame and the vCPU then runs in bursts about
8× slower (a fixed spin loop run before the tick takes up to 10× its usual time in some frames,
and with it warming the core first the emit's slow frames mostly go; S3, whose frames outlast
the period, never idles and shows 2.2×). What brought it
there from p50 249 µs / p95 1.98 ms at first: `glyph_cache` hashes the key with one 64-bit mix
and compares a 32-bit tag per way before the key, and `find_placed` probes and looks up and
touches the tile once (`atlas::use`) — emit −25 %; a line's sprites share one order from the
grid (`draw_scope::group_order`, a floor of the previous group's + 1 so a block's groups
ascend), which removes the per-glyph grid lookup and leaves `batch_for_render` an ascending
stream to copy instead of sort — order −50 %; and the text node keeps each glyph's placement
(box and tile) until the layout runs a pass, so a scrolling frame costs one `atlas::use` per
glyph — emit 104 → 64 µs back to back. Shaping by invalidation size (§4.6, the same run, cold
= shape cache empty, warm = a new layout over a warm cache): a 14-byte label 3.3 µs cold; one
76-byte line 15–36 µs (0.7–0.8 ms the first time a script needs a fontconfig fallback search);
a 669-byte paragraph 0.13 ms cold, 53 µs warm; a 6.9 KB page 1.4–1.5 ms cold, 0.7–1.1 ms warm;
the whole document 31–38 ms cold, 20–22 ms warm, a re-wrap 0.8 ms. The whole-document reshape
exceeds a 60 Hz frame, so it moved off the UI thread: text of 4 KiB or more
(`lx.ui::k_text_async_min_bytes`, an inline cold reshape under ~1 ms) is shaped and wrapped on
the worker affinity when the block has a `text_async_shaping` (the worker's own shape cache and
itemizer over the block's face cache) into a staging layout, handed back with
`executor.post(ui, …)` and swapped in at the next UI frame, the old layout shown meanwhile
(`test_text_node`). The worker's shape pass gives the face lock to a waiter between paragraphs
(`face_cache::contended`): the UI thread waited at most 1.3 ms while a 150 KB document shaped.
GL baseline rows: none committed — the two GL runs differ by more than 5 % at p95.

As built (title bars, §4.11): server-side titles and the close glyph are atlas glyphs on all
three compositors — CPU and GL through `:ui_cpu` / `:ui_gl` with the compositor's
`chrome_atlas`, Vulkan through `:ui_vk` recording inside its linear F16 pass — and the
per-window CPU title textures are deleted. `test_deco_titles`: the CPU compositor against
committed references (tests/golden/compositor, scale 1 and 1.25, every title rule and button
state), on turned outputs pixel-exact against the content turned, GL within 1/255 of it
(G10's 3/255 is the bound; both blend UI in the encoded space; the circle blit's edge, blended
per compositor as every blit always was, is masked), Vulkan within 2/255 outside the text and
16/255 on glyph edges (measured 1 and 9/255; the bound leaves room for a hardware driver's
pow and rounding) with the mean stem weight within 3 % (measured +1.5 %), zero validation
messages; capture re-composites and the headless dump show the titles; a steady frame
allocates nothing. Render-thread cost per decorated frame, `bench_ui_composite` (six titled
480×320 windows at 1920×1080): CPU full repaint 1.81–1.85 → 1.87–2.08 ms p50 (1.89–1.91 ms
at load 0.1), one window 32 → 36–45 µs; GL render thread 719–831 → 698–802 µs, GPU completion
5.99–6.04 → 5.91–6.05 ms; UI-thread `commit_frame` 16 → 23–26 µs. A title change costs
≈80 µs on the UI thread when its glyphs are new, 1–3 µs when they are cached.

### Phase 3 — Layout, style, theme, components (≈2 weeks) — **done** (G2 not met on this host)

- `lx.layout:flex` with the per-node cache; `grow_node::grow_factor()` finally read.
- `style` value + builder; `lx.ui:theme` binary format; `tools/lumen-theme` compiling
  `assets/themes/one-dark.json` and `one-light.json` (Zed schema) at build time; bundled
  fonts and Lucide icons under `assets/`.
- Components of §5.6 as `ui_node`s; focus and keyboard navigation on the new tree
  (`focus.cppm` rewritten off `widget*`); `reducer_node` wired into `ui_root`.
- Gate: G2 on a synthetic panel tree (S2); a screenshot of the component gallery reviewed
  against Zed side by side.

As built (layout, §4.7): `lx.layout:flex` (`solve_flex` over a `flex_style`, on
`lx.foundation` only) follows CSS Flexbox §9 in the shape of Taffy's `flexbox.rs` and lays out
463 of Taffy's 685 Chrome-generated flexbox fixtures exactly as Chrome does
(`test_layout_flex`, transcribed by `scripts/flex-fixtures.py`; the rest use text measurement,
RTL, baseline, aspect ratio, sizing keywords, scrollbar gutters or non-flex display, and two
need `position: static` or Taffy's known-but-indefinite sizes). Not offered: `baseline` (no
baseline query on `ui_node`), `aspect-ratio` beyond filling a missing side. Per-item and
per-line state is caller-provided (`ui_node::k_max_children` of each on the stack in lx.ui):
a solve allocates nothing. `lx.ui:flex`'s `flex_node` runs it through `measure` / `arrange`;
`box()` became a flex column or row; `grow` and `sizing` are read as `flex-grow` and min /
max; text answers `size_hint::min` with its longest word (`text_layout::min_content_width`).
The 4-entry measure cache lives in a 72-byte side-pool block a node takes when first
measured, behind a 32-bit slot in what was padding: `sizeof(ui_node)` stays 80 B, and S3
measured A/B interleaved against main is unchanged (emit + order + encode p50 447–457 µs vs
447–466 µs, load 0.7). Dirtiness propagates to the root (stopping below a dirty ancestor),
`place` skips a clean node at unchanged bounds, `ui_root` places the root without measuring
it, and props that change nothing layout reads mark nothing (`apply` returning `bool`). On a
301-node panel tree (10 rows × 29 labels; `lumen-ui-bench --scenario S2`, "layout_solve",
load 0.5–0.8, six runs): every node dirty 66 µs p50 / 153–200 µs p95 (1 170 measure calls,
301 arranges), the root alone dirty over warm caches 1.1–1.4 µs / 1.1–5.3 µs (10 cache hits,
one arrange), one label dirty 6.9 µs / 7.5–13.8 µs (5 measure calls — the label and its row —
3 arranges, 37 skipped); 0 allocations. A paint-only S2 frame's layout stage fell from
p95 56.6 µs to 1.7 µs (the clean root is not walked).

As built (style, theme, icons, §4.8, §5): `lx.ui:theme` defines the tokens — every colour key
of Zed's theme schema except the vim mode colours (167, with the 14 status triples), 8
players × 3 and the 46 syntax colours of the One themes: 237 `tok`s, each an index into the
`.lxtheme` colour table — and the format: a 128-byte header (magic, version, sizes, offsets,
FNV-1a checksum of the rest), the sRGB `u8×4` and premultiplied linear `f16×4` tables, the
status and player tables (token triples), 46 syntax styles (colour token, weight, italic,
background), the fonts of §5.4 and the density. A theme is 3 816 B; `theme::load` is one
`read` and a bounds, version and checksum check (11.9 µs from the page cache), immutable
afterwards, failing with `io_err` or the new `ui_err` domain (`theme_format`,
`theme_version`). `tools/lumen-theme` compiles Zed's own `assets/themes/one/one.json`,
vendored unmodified with its MIT licence (one family file, not the `one-dark.json` /
`one-light.json` pair this section names), through `theme_compiler` (a strict RFC 8259
reader; build-time code with std containers) into `themes/one-dark.lxtheme` and
`one-light.lxtheme` in the build tree (≈11 ms a run), installed to
`share/lumen/themes` and found through `LUMEN_THEME_DIR`, the build tree or the install. A
missing key resolves as it does in Zed: derived rules — a status background from its
colour at 25 %, the selection from player 0, version-control and diff-hunk colours from the
status colours, the active scrollbar thumb, minimap and panel overlay colours from their
neighbours — then the value of its appearance in Lumen's base theme (`lx.ui:theme_defaults`,
generated by `scripts/theme-base-table.py`): One Dark's and One Light's values from the MIT
`one.json`, and for the 32 tokens that file leaves unset Lumen's own picks, each another One
token at a fixed alpha. No value is taken from Zed's GPL-licensed crates. Every fallback and
ignored key is reported. One Dark: 207 tokens from the file, 19 derived, 11 from the base
theme; One Light
205 / 19 / 13; every value the file gives, and every §5.1 value above, comes out as written
(`test_ui_theme`). The fallback theme compiled into the binary is generated constants
(`theme_builtin.cpp`, `lumen-theme --emit-cpp`, committed): compiling it during the build
would make lx.ui depend on a tool that links lx.ui; `test_ui_theme` fails when it differs from
what the build compiles. The UI thread has one active theme and density; changing either
bumps `theme_generation`, on which `ui_root::tick` describes again, clears every measure cache
and repaints everything; flex solves take the rem from the active theme (`ui_rem_px`, S2's
solve costs unchanged). `theme_watcher` (inotify on the file's directory) reloads on the
worker and applies on UI in every build type; hooking it to a client's event loop is Phase 4.

`lx.ui:style` is the packed `style` with the builder above — 112 B, not ≈96: its 21 lengths
(sizes, basis, gaps, padding, margin, insets) at 32 bits are 84 B alone; the flex enums pack
into one word, grow and shrink are halves, the paint and text fields u16 tokens and bytes. A
spacing step rides in a length of unit `auto` with value step + 1, so a style stays bitwise
comparable and density-independent. `lx.ui:styled` resolves it in `apply` (layout through the
density into the flex engine; one packed quad and the elevation's shadows, emitted under the
style's opacity; an elevation with no `bg` takes §5.2's background token, an elevated or modal
one without a border the 1 px `border.variant`); a paint-only change damages the bounds without
a relayout; `styled_node` is 504 B; repainting a styled tree allocates nothing. Text fields are
resolved and inherited (`inherited_text`) but not yet read by `text_block_node`, whose props
carry their own colour and font — the components (§5.6) bridge the two.

Icons: 53 Lucide 1.61.0 SVGs (26 KB with the licence and a README of SHA-256s;
`trash` is the old `trash-2`) in `lx.ui:icon` — in lx.ui rather than lx.gfx because the set,
its files and its tint tokens are the UI's, and librsvg is dlopen'd as lx.shell does, so no
module gains a library edge (the GLib and cairo headers stay in an implementation unit). An
`icon_cache` rasterises (icon, device px) once into an R8 atlas tile, the alpha of librsvg's
rendering as coverage (63 µs for a 16 px icon), tinted per draw. A miss rasterises
synchronously on the UI thread by default (counted; a steady frame only hits); with `set_async`
it is rasterised on the worker (four jobs, buffers allocated with the cache) and written into
the atlas on the UI strand, damaging the bounds that asked. Golden images `styled_surfaces`
(elevated and modal stacks over `surface.background`, borders, radii xs…full, opacity) and
`icons` (16 / 14 / 12 px and a 20 px raster at scale 1.25): GL within 1/255 of the CPU
reference, Vulkan (lavapipe) within 2/255 of CPU and 2/255 of GL. Without librsvg the build
succeeds, icons draw nothing and those cases print SKIP.

As built (focus, reducers, motion, the basic components, §5.5–§5.6): `ui_node` carries 16
interaction and bookkeeping bits (`node_flag`: focusable, focused, focus ring, hovered,
pressed, interactive, disabled, a trap's scope, a reducer's opaque children, repaint,
referenced, hover intent) in what was padding — `sizeof(ui_node)` stays 80 — and holders of
node pointers across frames (focus, hover, press, timers, traps) are `node_observer`s a dying
node tells. `lx.ui:focus` is rewritten on that tree (the old `widget*` manager had no callers
and is gone): tab / shift-tab in tree order with positive `focus_order` first and negative
skipped, traps for stage 4's modals (8 deep, rejected past that), programmatic focus,
focus-visible as GPUI decides it (the ring after keyboard input only), arrow / home / end
within groups, the magnifier hook kept. `lx.ui:interact` adds the pointer state (hover and
press per interactive node, a release over the pressed node activates it, a press focuses
without a ring), `handler` (a 32-byte trivially copyable callable: function pointers, reducer
bindings, small lambdas), the frame clock, `transition` + `animate` (100–150 ms ease-out,
frames only while running, `reduced_motion` jumps to the end) and a 32-entry timer table
(hover intent at 500 ms; `ui_root::next_deadline_ns` for the event loop, no frames while a
delay waits). Reducers are wired into `ui_root`: a dispatch queues its reducer (intrusive
list) and the next tick describes that subtree alone; the reconciler never enters a reducer's
children, so a describe of the tree around it keeps its state and nodes. Damage became exact
for relayout: `place` damages a node the layout moved (old and new bounds) and one whose look
changed with its layout (`mark_repaint`: a label's text), never an ancestor arranged in
place; a text block's colour change repaints it. A text block's height is no longer cut to
an unfixed available height, so a 23 px φ line overflows a 22 px button centred, as in GPUI.

The components (`lx.ui:control`, `:components`) are builders over one node, `control_node` —
a flex container painting a rounded, bordered box whose background and border follow the
pointer (rest / hover / pressed, each a token, faded, blended with a second token or darkened,
Zed's `Hsla::blend` and `Theme::darken`) — plus `text_block_node` and `icon_node`: `label`,
`headline`, `button` (Zed's six `ButtonStyle`s, `subtle` being the ghost; five
`ButtonSize`s; with icon), `icon_button` (wide, square), `toggle_button`, `checkbox`,
`toggle_switch`, `divider`, `progress_bar`, `avatar`, `key_binding`, `key_binding_hint`,
`tab`, `tab_bar`, `panel` (Zed's workspace toolbar + body), `status_bar`. Every size, padding,
token and state rule is taken from crates/ui at zed a5c4ca8 and cited in the source;
`test_ui_components` checks them at the default density — button 22 / 18 / 28 / 32 px with
`b04` / `b08` padding, icon button 24 × 22 and 20 × 20, checkbox box 16 px offset 2 in its
20 px holder, switch 38 × 26 around a 32 × 20 track with the 12 px thumb 3 px from the end,
divider 1 px, progress 8 px with a 4 px fill, avatar 16 px, key 14 px in an 18 px stroke, a
key hint's cap 16 px, tab and tab bar 32 px with 31 px content, status bar 30 px, toolbar
45 px. Departures from Zed: every focusable control draws the §5.5 ring (Zed varies it), the
transparent button keeps its label colour on hover (§5.5), the checkbox and switch are tab
stops (keyboard-operable), a lone toggle button sizes to its content, avatars may show
initials. The golden images `components_states` (each control at rest, hovered, focused and
disabled) and `components_gallery` are within 1/255 of the CPU reference on GL and 2/255 of
CPU and GL on Vulkan (lavapipe). Measured (`test_ui_components`, load 0.8–1.4, frames back to
back): the gallery tree of every component (94 nodes, 184 primitives) ticks a full repaint in
11.4–13.9 µs p50 (15–22 µs p95), a hover move 11.4–18.5 µs, a focus move 11.4–20 µs, a
describe of the whole unchanged tree 24–40 µs, with 0 allocations and a flat `mallinfo2` over
300 frames of each; a hover repaints exactly that control's bounds; a fade is 9 frames over
150 ms and then none, an idle tree asks for no frame and sets no deadline. S2–S4 A/B against
the stage before, interleaved (load 0.8–1.6): S2 tick p50 12.9–14.3 vs 13.2–14.1 µs, S3
layout p50 134–145 vs 143–150 µs, S4 within the host's bimodal noise both ways.

As built (overlays, scrolling, lists, text input and the rest of §5.6): `lx.ui:overlay` is
GPUI's deferred draw on the retained tree. An `overlay_node` sits anywhere in the tree,
zero-size and absolute in the flow; while open it is registered with the UI thread's
`overlay_layer` (16 slots, `overflow_action::reject`), which after the tree's layout measures
its content against the window less an 8 px margin, places it beside its anchor — the overlay
node's parent, a surface point or the window — with GPUI's anchored rules (flip to the other
side, hang from the anchor's other end, then shift inside the margin: `place_overlay`), and
emits it after the tree so the overlap grid orders it above what it covers. The pointer tests
overlays first, topmost first (`pointer_state`'s resolver), a modal's scrim blocking the
tree; a press outside an overlay dismisses it top down (a press on a popover's trigger is the
trigger's), Escape the topmost, an activated menu item its menu chain; modals and menus trap
the focus (stage 3's traps) and give it back. Opening is a prop edge (`open`, with
`on_dismiss` telling the app) or the node's own (`open_at`: a tooltip, a context menu, a
submenu). `node_flag::layer` makes the content's bounds surface coordinates.

Viewports: `ui_node::child_viewport` (offset and clip per child), with `surface_rect` /
`local_point` mapping a node's rect or a surface point through every viewport above it —
damage, hover and hit tests, the IME's cursor rectangle and the magnifier all go through it,
and a tree without a viewport maps nothing (a live-viewport count; `sizeof(ui_node)` stays
80, S2's solve and tick unchanged: cold 66 µs, warm 1.1 µs, leaf 6.9 µs p50). A node clipping
to its bounds pushes `draw_scope::push_clip_local` (offset by the origin; `push_clip` stays
absolute). `lx.ui:scroll`'s `scroll_area` lays its content out once at full size and draws
it under the offset: a wheel or a thumb drag measures and arranges nothing and damages the
viewport (`test_ui_components`: 0 measures, 0 arranges, damage = the viewport). Its
scrollbar is Zed's `ScrollbarStyle::Regular` (scrollbar.rs at a5c4ca8: 6 px thumb, 4 px
padding, 14 px strip, 25 px minimum thumb, `thumb_ranges`' placement, track click centring
the thumb), in the §5.1 tokens; it shows whenever the content overflows (no autohide).
`lx.ui:list` holds rows of one fixed height — Zed's `ListItem` spacing: dense 23 px (a 14 px
label on the φ line), extra dense 21, sparse 31 — in chunks of 16 under a reducer host:
up to 100 rows all exist; past that only the chunks covering the visible rows ± 8 do, in a
fixed pool where chunk k is slot k mod the pool size, so a scroll re-describes a chunk in
place and never creates a node (10 000 rows in a 400 px viewport: 48 rows live, 152 nodes in
the tree; scrolling 2 rows a frame after a warm-up pass, tick p50 15 µs / p95 88 µs with a
chunk re-described every ~8 frames, 0 allocations, the node count constant). `ui_root`
rebuilds what its own layout asked for (a list whose viewport moved) and lays out again on
the same frame. Selection (`ghost_element.selected`) and the list's focus ring are separate;
up / down / home / end / page keys move the selection into view, enter activates. S5's
property holds: the pointer moving one row per frame over a 1 000-row list damages one or
two rects (the two rows, merged when adjacent), measures and arranges nothing (tick p50
13.6 µs).

`lx.ui:text_input` edits a fixed buffer (the app's `text_buffer`, or 1 KiB of its own) with
`overflow_action` at the capacity, through a `text_block_node` it owns: carets on lx.text's
grapheme boundaries (a ZWJ family is one step and one backspace), left / right following the
paragraph direction with the caret where `text_layout::caret` puts it (an Arabic field's
caret starts at the right and moves left), word jumps, home / end, shift and pointer
selection (double and triple clicks), backspace / delete, `clipboard::global()` for ctrl+c / x
/ v, a 500 ms blink on a UI timer that stops with the focus, horizontal scrolling by origin.
`ime_target` is the interface text-input-v3 will drive: one `ime_update` per `done` applied in
the protocol's order (delete surrounding, commit, preedit), the preedit shown underlined at
the caret with its own cursor, `ime_query` for the surrounding text and the cursor rectangle;
`ui_root::ime()` is the focused field. Typing and deleting allocate nothing.
`lx.ui:popups` builds menus (Zed's `ContextMenu`: inset dense items with an icon, label and
key binding, separators, headers, disabled items, toggles, one-level submenus opened by
click or right arrow), `popover_menu`, `context_menu`, `modal` (the modal-surface box rounded
`lg`, 5 rem from the top over a scrim of `elevated_surface.background` at 80 %, Zed's modal
layer) and toasts (Zed's status toast with GPUI's `shadow_lg`, stacked at the bottom,
dismissing themselves after 10 s, paused on hover); `button(…).tooltip(text)` is Zed's
tooltip (500 ms, at the pointer + (8, 10), 100 ms fade). Departures: overlay radii follow §5.5
(`md`) where Zed's `elevation_2` rounds `lg`; the scrollbar thumb carries the theme's border
token; submenus open on a click or key, not on hover. Measured (`test_ui_components`, load
0.3–0.6): a frame with a menu open ticks in 4.5 µs p50 (repaint, hover, arrow keys) with 0
allocations, an idle one is no frame; a tooltip's fade is 6 frames; the six new golden images
(`menu_open`, `tooltip`, `modal`, `toast_stack`, `scroll_area`, `text_input`) are within
1/255 of the CPU reference on GL and 2/255 of CPU and GL on Vulkan (lavapipe).

As built (the gate: S2 on the components, G2, the gallery). `lumen-ui-bench --scenario S2` is
no longer a replica of 24 quads: it is a shell panel described only with §5.6 components,
`div` flex boxes, the compiled One Dark theme (`one-dark.lxtheme`) and real text and icons —
Activities, ten workspace buttons, six pinned launchers, six window buttons with icon and
title, six status texts (two with meters), an input-source button, a notification button,
nine tray icon buttons, an avatar and, in a fixed 120 px cell in the middle, a clock — at
1920 × 36 in a 1920 × 1080 surface. It is **301 `ui_node`s**: 117 drawn, 184 in the 34
tooltips its buttons carry (closed: an overlay node and its content, never laid out or
drawn). The clock is a reducer; once a second (every 60th frame; 3600 frames = 60 s by
default, `--clock-every` changes it) the bench dispatches the new time, and the tick describes
that reducer's subtree alone, reshapes the label (one shaping call), re-measures the path to
the root over every other node's cache — 9 measure calls, 2 cache hits, 5 arranges per clock
frame — and damages **1 rect of 1 357 px, 0.065 % of the surface**. Between clock frames the
root produces nothing. With `emit_scope::damage` (§4.9) a clock frame walks 7 nodes and emits
10 primitives in 2 batches. Measured (`lumen-ui-bench --scenario S2 --frames 36000`, 600 clock
frames per run at the bench's 1 ms frame clock, `setarch -R`, pinned to CPU 1; load 0.04–0.45;
`tests/bench/baseline/iritur-none.json`, rebaselined at load 0.31, holds one of them):

| G2 sum (update + tick + order + encode) | p50 | p95 | Stages, p50 |
|---|---|---|---|
| clock frame, `none` (3 runs) | 90.0–91.1 µs | 480.7–484.2 µs | reconcile 12–14, layout 59–60, emit 8.0–8.5, order 0.9, encode 0.4 µs |
| clock frame, `gl` (2 runs) | 93.1–95.9 µs | 538.7–543.6 µs | the same; `submit` 392–395 µs, GPU 1.55–1.60 ms (fence-wall) |
| frames between (`update_idle`), `none` | 0.9 µs | 5.6–6.3 µs | — |
| clock frame back to back (`--period-us 0`) | 7.0–7.5 µs | 24.7–26.4 µs | reconcile 0.7, layout 4.8–5.0, emit 1.1 µs |

**G2 is not met on this host**: damage passes (1 rect, 0.065 % ≤ 2 %), p50 is under the
bound, p95 is 3.2× over it. The frame is cheap warm (7 µs) and expensive woken from idle,
which is how S2 runs and how a panel runs: 91 µs p50, of which the clock label's reshape is
most of the layout stage (≈45 µs — itemisation, a HarfBuzz call on cold code and font tables,
the wrap), std::format and the label's describe most of the reconcile. The p95 is the host's:
a fixed 13 µs arithmetic loop timed on the same cadence (1 ms frame clock, one frame in 60)
has p50 13.3 µs and p95 115–116 µs, against p95 16.9 µs back to back — on this VM a frame
woken from a sleep stalls for ~100–450 µs one time in twenty or more, the more often the
longer it runs. A pass is not claimed. What Phase 3 did to the frame: the scene's first version (241
nodes, the whole tree emitted) measured 146 µs p50 / 1 140 µs p95 with the emit at 55 µs p50
(147 primitives in 15 batches, over S2's cap of 8); emitting inside the damage, skipping a clipping container's
children through an empty clip, culling text blocks beside the clip before any glyph and icons
before the cache, and one formatting pass in `frame_format` brought it to the numbers above
(solving small flex containers with 8-entry rather than 64-entry stack scratch was tried and
measured nothing, and was left out). G5 holds on `none` (0
`operator new`, `mallinfo2` delta 0 over 36 000 frames); on `gl` there is no `operator new`
either, and `mallinfo2` grows 288–480 B over a run from Mesa's own allocations (the previous
S2 grew 960 B on `gl` the same way). The flex solve cost table (`layout_solve`, the synthetic
301-node tree) read cold 103–122 µs / leaf 9.5–12.6 µs on this day where it read 66 / 6.9 µs
before; the same table built from the previous stage's bench, and from this stage with its
lx.ui changes reverted, reads the same 117–121 µs — the host moved, not the code.
`sizeof(ui_node)` is 80 B, `sizeof(style)` 112 B, `styled_node` 504 B.

The gallery (`lumen-ui-gallery`, ui-architecture.md §10) draws every component in its states
under One Dark and One Light; `docs/images/ui-gallery-one-{dark,light}.png` are its CPU
renderings (1040 × 1008, 1 496 primitives, ≈180 KB each), the `ui_gallery` test redraws them
bit for bit and on GL within 1/255. Compared with Zed's sources value by value
(ui-architecture.md §10): menus and tooltips now round `lg` as Zed's `elevation_2` does (they
were `md`, and §5.5 said `md` — corrected above; the `menu_open` and `tooltip` references were
rewritten for it, nothing else moved).

Deviations from this phase's plan, across its stages: the `style` is 112 B, not ≈96 B;
`tools/lumen-theme` compiles Zed's single `one.json` family file (vendored with its MIT
licence) instead of a `one-dark.json` / `one-light.json` pair; icons live in `lx.ui:icon`, not
`lx.gfx`, with librsvg dlopen'd; the flex engine has no `baseline` alignment and uses
`aspect-ratio` only to fill a missing side, and grid is later; theme hot reload
(`theme_watcher`) exists in every build type, its event-loop hook is Phase 4; the old
`widget*` focus manager was deleted, not ported (no callers); `text_block_node` does not read
a style's text fields (the components pass colour and font); `ui_root` gained
`emit_scope::damage`, which §4.9 did not plan; S2 ignores `--scale` (its sizes come from the
theme's rem); `tools/lumen-theme` links and imports the `lx.ui` facade, not a format-only
library (§4.13 planned "`lx.ui:theme` format only"): the format, the compiler and Zed's
defaults are lx.ui partitions built on lx.ui's `style` and token types, and moving them into a
library below lx.ui would move those types with them for one build tool that no process
links. Components that depart from Zed, on purpose: every focusable control draws the
§5.5 focus ring (Zed varies it); a transparent button keeps its label colour on hover (§5.5;
Zed mutes it); the checkbox and switch are tab stops; a lone toggle button sizes to its
content; avatars may show initials; submenus open on a click, space, enter or → rather than
on hover; the scrollbar does not autohide. G2's p95 is open, as above.

Review fixes (after the gate). Bounds: a text input's text is cut, at a character boundary,
to `min(storage, max_bytes, k_text_input_capacity)` whenever the props are applied (an app
buffer assigned more, a smaller `max_bytes`), and an insertion's room saturates, so nothing
writes past the node's fixed buffers. Layout: percentages now have a base across the
`ui_node` boundary (`percent_base_scope`: the engine's `child_query::parent_*` while a flex
container measures, its arranged content box while it places, a decorator's, scroll area's,
list's or the overlay layer's box, the window at the root), the measure cache keys an entry by
it only when the measurement read it (the block grew from 72 to 104 B; S2's clock frame still
measures 9 nodes with 2 hits and arranges 5), and a node with percentage padding or border is
re-arranged at unchanged bounds. Damage: `ui_node::ink_outset` (a shadow's 3σ + spread +
offset) widens what `place`, `invalidate_paint`, `invalidate_layout` and `animate` damage; an
overlay damages its content's painted extent when it opens and closes, and the whole window
on every frame of a scrim's fade; paint-only prop changes on opacity, clip, flex and box nodes
damage the node. A reducer or list rebuild that runs out of the frame arena is queued again
for the next frame's (dropped, as logged, only when it overflowed an arena nothing else used).
Fidelity against Zed's sources: the modal layer occludes without a scrim unless a modal asks
(`modal(…).scrim()`), its title in `text`; status toasts keep `border.variant` with a square
muted close button; menu icons `Default` (chevron `Muted`); the scrollbar draws no track and a
borderless thumb blended over the surface; the switch's ring is its 2 px border; the input
field is 32 px (the editor's 1.2 line); the key binding hint is in the buffer font; a selected
text button keeps its icon colour; disabled checkbox and switch labels keep their colour and
the checkbox's hover group is its 20 px box; an `Indicator` square icon button has no padding;
Lucide strokes are drawn 1.8 units wide (Zed's 1.2 px at 16 px). The gallery is now 1 492
primitives (no close glyph on unhovered tabs, as Zed shows none); the `icons`,
`components_states`, `components_gallery`, `menu_open`, `tooltip`, `modal`, `toast_stack`,
`scroll_area` and `text_input` references were rewritten.

### Phase 4 — The client runtime (≈3 weeks) — **next**

- `lx.wayland.client` as a thin libwayland-client wrapper (registry, `xdg_shell`,
  `zwp_linux_dmabuf_v1` v5 feedback, `wp_linux_drm_syncobj_v1`, `wp_viewporter`,
  `wp_fractional_scale_v1`, `wp_presentation`, `zwp_text_input_v3`, `wl_seat`), moving the
  shell's existing hand-written bindings where they fit.
- `lx.app:window` with the swapchain tiers of §4.10, input routing into `ui_node::on_*`,
  the frame loop of §4.9, damage reporting, idle rule.
- `lumen-hello` shows a window; `lumen-ui-bench --client` runs S1, S5, S7, S9, S10 against
  the live compositor; `scripts/ui-bench.sh --e2e` joins injector timestamps with
  `compositor.latency: present` lines the way `drag-smoke.sh` does.
- Gate: G1, G5, G6, G7, G11.

As built (stage 1, `lx.wayland.client`). The stub is replaced by a thin libwayland-client
wrapper in six partitions (`:types`, `:display`, `:surface`, `:xdg`, `:seat`, `:dmabuf`;
docs/api/README.md lists them), Platform layer on `lx.foundation` / `lx.runtime` /
`lx.trace` only, no Wayland type exported. Objects are non-movable RAII (destroy request on
destruction), listeners are static thunks over the proxy's user data, and every proxy is
tagged so `surface::from_native` maps a `wl_surface` in an event back to its wrapper only
when it is one. Globals bind at a capped version (wl_compositor 6, wl_shm 2, xdg_wm_base 7
required — a missing one fails connect with `wayland_err::missing_global`; wl_seat 10,
wl_output 4, decoration 2, linux-dmabuf 5, syncobj 1, viewporter 1, fractional-scale 1,
presentation 2, text-input-v3 2, cursor-shape 2, single-pixel 1, xdg-activation 1,
xdg-output 3 optional, absent ones named in one start-up line). Pointer events are folded per
`wl_pointer.frame` (axis_discrete folded into value120), the keymap fd is compiled by
xkbcommon (MAP_PRIVATE, the v7 rule), configure sequences are latched whole and acked by the
owner, dmabuf feedback becomes one immutable snapshot per `done` (8 tranches × 512 entries,
`warn_and_drop` past them; the two blocks swap, nothing is copied), presentation feedback
keeps 8 slots per surface. `display_source` puts the connection on `lx::runtime::event_loop`
the way GTK's GSource and calloop's WaylandSource do — prepare_read or dispatch what is queued,
flush, read or cancel — and on a full socket waits for POLLOUT: the loop gained
`set_write_interest` / `on_writable` for it (GTK 4 exits on that EAGAIN). New `wayland_err`
codes `connect_failed`, `missing_global`, `connection_lost`, `object_failed`. The compositor
now sends `axis_value120` to wl_pointer ≥ 8 instead of `axis_discrete`, which the protocol
forbids there. Measured (`test_wayland_client`, 42 cases, in-process compositor on a
1280×800@1.25 virtual output, load 1.2–3.8): connect with three roundtrips 1.6–14 ms; a
steady frame-callback cycle (request, commit, flush, dispatch) 48–64 µs p50, 300–745 µs p95
on the client thread, with **0 `operator new`** over 300 frames and libwayland's own 9
`malloc` / 9 `free` per frame (a `wl_proxy` per callback and a `wl_closure` per message, as
every libwayland toolkit pays; net 0); an idle connection on the loop wakes it 0 times in
200 ms with 0 socket reads; a full socket answers `again` after 6 144 requests and drains.
Under ASan + LSan the test is clean. The headless compositor advertises linux-dmabuf v5 with
main device 0, a 2-entry table and one tranche, no `wp_linux_drm_syncobj_manager_v1` (no
DRM device; vmwgfx has no timeline syncobjs either), so explicit sync is untested here.
Deviation: the shell's hand-written bindings are not moved yet — the wrapper keeps
`native()` / `native_global()` and a `global_handler` for extension globals (zlm_shell_v1,
layer-shell) so they can move in Phase 5 with app-smoke as their check.

As built (stage 2, the client pixel path: `lx.gfx:swapchain`). The swapchain lives in
`lx.gfx`, not in `lx.app:swapchain` as §4.13 has it: allocation and the GPU targets are GBM,
EGL and Vulkan objects, which may not leave `lx.gfx` (AGENTS.md §1.1). Its interface carries
fds, fourcc, modifiers, dev_t, plane offsets / strides and `void*`; `lx.app` (stage 3) makes
one wl_buffer per image when `swapchain_listener::on_image_created` says so, through
`lx.wayland.client`, which `lx.gfx` never imports. `choose_client_format` takes the feedback's
tranches in the compositor's order (scanout-flagged first when the surface feedback has one,
the render device's only; XRGB then ARGB for opaque windows, ARGB for translucent ones, 2:10:10:10
only when asked); `open_render_node` opens the main device's render node through
`drmGetDeviceFromDevId`. `client_gpu` picks the tier with `present_backend::automatic`'s order
and refusal rules — Vulkan on a hardware device whose DRM node (VK_EXT_physical_device_drm,
`choose_physical_device`) is the render node, lavapipe refused before its device is made; else
GLES 3.1 on GBM through `:gl_renderer`'s `egl_device`, a software `GL_RENDERER` refused; else
wl_shm + `ui_cpu` — and a compositor that names no main device (the headless one) gets cpu.
`LUMEN_UI_BACKEND` / `client_gpu_options::force` override it (logged); `allow_software` lets a
test force lavapipe, labelled software. `client_swapchain` makes images on demand up to three
(two for shm) — a compositor that releases promptly keeps fewer alive: against the in-process
compositor the cpu tier never needed a second image and GL / Vulkan two — keeps buffer age per
image (0 undefined; a four-frame damage history), hands out the free image with the newest
content, never blocks (`acquire` returns none while every image is with the compositor),
releases by wl_buffer.release or, with explicit sync, release points (`collect_releases`, an
eventfd from DRM_IOCTL_SYNCOBJ_EVENTFD for the loop), and on `resize` destroys free images,
keeps busy ones until released (six, then `warn_and_drop`) and counts one allocation per new
size. GL: `gbm_bo_create_with_modifiers2` → `…_with_modifiers` → `gbm_bo_create` (logged), the
EGLImage imported from the bo's own planes (so wrong export metadata fails on our side first),
renderbuffer + FBO; explicit sync: the frame's native fence imported into the image's DRM
syncobj at its acquire point (`drm_timeline_ops::import_sync_file`: a temporary syncobj, then a
transfer; `create` / `handle_to_fd` added for the client's own syncobjs). Timelines are per
image, not per window, as Mesa's WSI keeps them: the compositor releases in its order, not
ours, and a timeline point waits for every earlier one. Vulkan: `VK_EXT_image_drm_format_modifier`
images from the feedback's list filtered by `vkGetPhysicalDeviceImageFormatProperties2`
(renderable, exportable as dma-buf), foreign-queue ownership barriers recorded once per image
and submitted around `:ui_vk`'s draw, a timeline semaphore per image exported as an opaque fd
that must be a DRM syncobj (what the compositor's `drm_timeline_table::import` takes); without
that, implicit sync done properly: the frame's SYNC_FD into the dma-buf
(`DMA_BUF_IOCTL_IMPORT_SYNC_FILE`) and the compositor's reads waited by the GPU on the next
acquire (`EXPORT_SYNC_FILE`), CPU fence wait only as the logged last resort. `gfx::device::create`
now enables the `timelineSemaphore` feature it advertised (the compositor's timeline pool used
it without). cpu: one memfd per swapchain size, sealed against shrink and grow, `ui_cpu` drawing
into the mapping; `lx.wayland.client::shm_pool` gained a borrowed-fd `create`. Measured
(`test_client_swapchain`, 44 cases, in-process compositor on a 1280×800 virtual output, load
0.1): three golden frames per tier read back from the compositor's side — the cpu tier through
an ext-image-copy-capture output capture (max error 0), GL (SVGA3D, gbm path 2, LINEAR, the
plane fd the compositor registered imported into a GL context of the test's) within 1/255,
Vulkan (lavapipe, software) within 2/255; ages 0,0,0 then 3 (0,0 then 2 for shm) under
round-robin release; a busy acquire returns in 0.1 µs; resize and a 30-size storm allocate once
per size; 0 `operator new` per steady present on every tier (libwayland and the drivers'
`malloc`s reported: 6 / 17 / 15 per frame for cpu / GL / Vulkan). Per-frame present cost at
640×480, a 48×48 quad moving (two damage rects plus age): cpu p50 47 µs / p95 627 µs, GL p50
559 µs / p95 3.8 ms (of which `:ui_gl`'s draw and flush 449 µs, the swapchain 55 µs; SVGA3D's
submission dominates: on this host the CPU tier is ten times cheaper for small damage), Vulkan
(lavapipe) p50 1.09 ms. The headless compositor offers linux-dmabuf v5 with main device 0 and
one tranche (ARGB8888 / XRGB8888, LINEAR) and no syncobj manager; vmwgfx has no syncobjs at
all and cannot mmap its dma-bufs. So explicit sync is proven against a fake kernel timeline
(the real native fence, three real sync_files, the image busy until its release point and the
eventfd waking), the wire half (set_acquire_point / set_release_point) is untested here, and the
in-process compositor runs without a Vulkan driver in the test (lavapipe cannot import vmwgfx
buffers; its bookkeeping import registers them as received). Deviation: a 17th damage rect
merges the cheapest pair (`damage_region`), not the bounding box.

### Phase 5 — Adoption and deletion (≈2 weeks) — not started

- `lx.shell` panel, launcher and the portal overlay on `lx.ui`; `shm_canvas` deleted;
  the background keeps its single-pixel-buffer and worker-rendered image paths.
- Delete `lx.ui:widgets`, `:widget_node`, `lx.ui.builder` and `gfx::render_pass`
  (ADR-005 F7, step 13); `lx.app` no longer imports the builder.
- Compositor decorations fully on primitives; `compositor_impl` passes the stream through.
- Docs: `docs/subsystems/ui-engine.md` (this design as current), ui-architecture.md §9
  rewritten, rendering-performance.md gains a UI section with the measured tables,
  api/README.md and architecture.md §3 for the new partitions, UML for the client frame
  loop and the swapchain.
- Gate: G8; `scripts/app-smoke.sh` and `drag-smoke.sh` unchanged and green; the §8.6
  comparison table filled in for GPUI, egui and GTK 4 on this host.

### Phase 6 — Measured optimisations (ongoing, each behind a benchmark)

Taken only when S-scenario numbers justify them, in this order of expected value:
subtree emit caching (skip re-emitting clean subtrees; measure on S5 with 10 000 rows),
LCD subpixel text via dual-source blending, 4× MSAA or analytic coverage for `path`,
Oklab gradients, descriptor indexing for surfaces, worker-thread shaping threshold tuning,
Vulkan 1.3 dynamic rendering (with ADR-005 D4), a render thread for clients if S3 shows a
submit cost worth hiding.

---

## 8. Benchmarking plan

### 8.1 Principles

- **Measure the pipeline stage, not the frame.** Every number is attributed to
  `describe | reconcile | layout | emit | order+sort | encode | submit | gpu | present`
  through the spans and counters of Phase 0, exported as a Chrome trace
  (`LUMEN_TRACE_FILE`) and summarised by the bench.
- **Report distributions.** p50 / p95 / p99 / max over ≥ 600 frames after 120 warm-up
  frames; a scenario fails on p95, not on the mean.
- **Same host, same conditions.** Release preset, `performance` governor where it exists,
  bench process pinned to one core and the compositor to the other on this 2-core VM,
  display at the native mode, nothing else running. The JSON records kernel, Mesa and
  driver versions, `GL_RENDERER` / `VkPhysicalDeviceProperties`, git SHA, and the preset.
- **Backends are measured separately and never averaged.** The GL and CPU rows on
  this host are the regression gate; Vulkan rows on lavapipe are parity-only and labelled
  `software`. Hardware Vulkan numbers come from a second host and sit in their own
  baseline file.
- **Nothing is asserted that the bench did not print.** Tables in docs cite the JSON file
  they came from.

### 8.2 Metrics

| Group | Metric | Source |
|-------|--------|--------|
| CPU | per-stage µs (p50/p95/p99), UI-thread µs per frame, worker µs per frame | spans |
| GPU | per-kind batch µs (Vulkan timestamps / `GL_EXT_disjoint_timer_query`), fence-wait wall time fallback, submit→fence µs | backend |
| Work | primitives, batches, draw calls, instance bytes, atlas uploads (count, bytes), damage px and rects, occluded primitives | counters |
| Memory | `heaptrack` allocations in the steady window (must be 0), `mallinfo2` delta, RssAnon, atlas bytes, ring bytes, VRAM via `VK_EXT_memory_budget` when available | bench + `/proc` |
| Power / idle | voluntary + involuntary context switches over 10 s idle (`/proc/PID/status`), wakeups/s (`perf stat -e context-switches,cpu-clock`), GPU busy where the driver exposes it (`gpu_busy_percent` on amdgpu, `intel_gpu_top` on i915; none on vmwgfx) | scripts |
| Latency | input → present (injector timestamp → `wp_presentation` feedback and the compositor's `compositor.latency: present` line), frame callback → commit | `ui-bench --e2e` |
| Start-up | `exec` → first commit, first commit → first present, pipeline-cache hit or miss | timeline instants |
| Correctness | golden image diffs per backend; frames with p99 > 2 × p50 (jank count) | tests + bench |

### 8.3 Scenarios (`lumen-ui-bench --scenario <id>`)

Deterministic (seeded), each with a headless mode (CPU backend, no compositor) and a
client mode (live compositor). Sizes are the defaults; `--scale` multiplies them.

| Id | Scene | Exercises | Gate |
|----|-------|-----------|------|
| S1 | Empty window, 10 s | idle rule | G1 |
| S2 | Shell panel built from the §5.6 components (301 nodes: workspaces, launchers, a task list, status texts and meters, tray icon buttons with tooltips, a clock) at 1920 × 36 in a 1920 × 1080 surface; the clock changes once per second for 60 s | retained tree, layout cache, damage locality, damage-scoped emission | G2 |
| S3 | 10 000 rounded quads with borders in a grid, each drifting | emit, order grid, encode, instancing throughput | G3 |
| S4 | 2 000 lines × 50 glyphs of mixed Latin/Cyrillic/Arabic, scrolling 3 px per frame | shape cache, atlas, mono sprite batches, bidi correctness | G4 |
| S5 | 1 000-row list, pointer moves one row per frame | hover damage ≤ 2 rects, no relayout outside two rows | G2, G5 |
| S6 | Modal with the 4-shadow stack over S4 | shadow fragment cost, overlay ordering | G10 |
| S7 | 30 resizes of a text window, 200 ms apart | relayout, swapchain reallocation, heap | G11 |
| S8 | Launcher open / type three letters / close, 20 times | real shell path: popup, text input, list virtualisation | G2, G6 |
| S9 | Hover a button, 125 Hz injected motion, 3 s | input → photon | G6 |
| S10 | Cold start of `lumen-hello` ×10, with and without the pipeline cache | start-up | G7 |
| S11 | 400 icons + 100 images in a grid, 4 scales | poly atlas, SVG raster on worker, rounded sprites | G10 |
| S12 | 1 000 vector paths (icons as paths rather than sprites) | path coverage pass | informs Phase 6 |

### 8.4 Golden images and parity

`tests/test_ui_golden.cpp` renders a fixed set of primitive cases (radii 0/2/6/8/full,
border widths 0–3, dashed, gradients at 0°/45°/90°, the five shadow entries of §5.2, a
text block at 10/12/14/16 px and scales 1/1.25/1.5/2, emoji, underlines, a clipped
sprite) on every available backend and compares against the CPU reference with the
G10 thresholds. The reference PNGs are committed; a change to them is a reviewed change.
A second case set renders the same block in sRGB-space and linear-space blending and
asserts the `text.contrast` correction keeps the mean stem coverage within 3 %.

### 8.5 Harness and regression gate

- `lumen-ui-bench` prints a table and writes `bench-<scenario>-<backend>.json`:
  `{host, driver, git, preset, scenario, backend, frames, stages:{name:{p50,p95,p99,max}},
  counters:{…}, memory:{…}, gpu:{…}}`.
- `scripts/ui-bench.sh [--backend gl|cpu|vulkan] [--e2e] [--compare baseline.json]`
  builds release, runs every scenario on every available backend, stores results under
  `tests/bench/results/<host>-<driver>/<date>-<sha>.json`, and compares against
  `tests/bench/baseline/<host>-<driver>.json`: **fail** on p95 regressions above 10 % for
  any stage, on any steady-frame allocation, on a draw-call count above the scenario's
  cap, or on a golden diff; **warn** at 5 %. Updating a baseline is a deliberate commit.
- CTest: label `bench`; the default test presets exclude it; `ctest --preset bench` runs
  the headless rows in under two minutes (a `-L bench` on top of another preset still
  meets that preset's exclude).
- End-to-end (`--e2e`) reuses `scripts/inject-input.py` and the `--join-seat-session`
  machinery of `drag-smoke.sh`; both processes write Chrome traces on the same
  `CLOCK_MONOTONIC`, merged into one Perfetto view per run.

### 8.6 Comparison protocol

The same scenes in other engines, on this host, observed from outside the process so no
engine is instrumented differently: process CPU time from `/proc/PID/stat`, RssAnon,
context switches at idle, commit rate and present latency from the compositor's own
`compositor.latency` debug lines (it sees every client the same way), and start-up as
the launcher's `exec` timestamp to that client's first `surface.apply` span in the
compositor's Chrome trace (both on `CLOCK_MONOTONIC`).

| Engine | Scenes | How |
|--------|--------|-----|
| GPUI | S1, S3, S4, S5 | `gpui` examples adapted from `crates/gpui/examples` (`hello_world`, `text`, a list); Vulkan required — runs on the hardware-Vulkan host only |
| egui (`eframe`, glow) | S1, S3, S4, S5 | The `egui_demo_app` widget gallery plus a text-wall demo |
| GTK 4 | S1, S4, S5 | `gtk4-widget-factory` (installed) with `GSK_RENDERER=ngl` and `vulkan` |
| Slint | S1, S3, S5 | Gallery example, FemtoVG and software renderers |
| Qt Quick / Flutter | if installed | `qml` text wall; Flutter desktop gallery with Impeller |

Results go into `docs/subsystems/ui-engine.md` as a table with the host description, the
file they came from, and one line per engine on what was not comparable.

### 8.7 What success looks like in the docs

After Phase 5, rendering-performance.md §1.2 has a "Measured: UI" block in the same
style as the compositor's, with the G1–G11 rows filled from JSON, and a sentence per row
on what moved it. AGENTS.md §5 "Performance invariants" gains: *primitive stream and
instance ring are fixed-capacity; a steady UI frame allocates nothing; idle windows
request no frame callback; themes are compiled, not parsed at runtime.*

---

## 9. Risks and open questions

| Risk | Mitigation |
|------|------------|
| No hardware Vulkan on the development host | GL and CPU are the gate here; a hardware-Vulkan host is needed before Phase 1 is called done for Vulkan. Lavapipe parity catches logic errors, not performance |
| SVGA3D may not expose `GL_EXT_disjoint_timer_query` | Fence-wait wall time is the fallback and is labelled as such in the JSON |
| Text that looks thinner or heavier than Zed's | §4.5 blends in the encoded space by default, as GPUI does; the linear path's contrast exponent is validated by the golden set before it is used anywhere |
| Zed theme schema changes | The compiler pins `v0.2.0` and reports unknown keys; older themes load |
| Fonts and icons licensing | IBM Plex Sans and Lilex are OFL, Lucide is ISC; Zed's own icon set is not bundled |
| Descriptor indexing or `f16` attributes missing on old GPUs | Sampler arrays are fixed at 16 and `f16` is only used for colours; both are Vulkan 1.1 / GLES 3.1 core |
| Compositor regressions while the stream replaces `blit_command` | Phase 0 keeps `surface` byte-compatible; `test_dmabuf_composite`, `test_gl_composite`, `test_cpu_composite`, app-smoke and drag-smoke must stay green at every phase |
| Scope creep into an editor | The component set is §5.6; syntax tables are compiled but no editor component is in this plan |

Open: whether `lx.shell` keeps a CPU fallback for the panel on GLES 2 hosts (the CPU
renderer already covers it, at a measured cost in S2). Answered in Phase 2: the compositor
shares one atlas across its outputs — a glyph's key carries its size × scale, so the runs a
title keeps per output scale are different tiles of the same pages, and a page set per scale
would only split a few hundred tiles (§4.11). A GLES 2 compositor has no primitive renderer
and draws its frames without titles (the lowered path drops glyphs); a lowering of mono
sprites to blits of a coverage texture is the fix if such hosts matter.

---

## 10. Document maintenance

| Phase | Update |
|-------|--------|
| 0 | `docs/api/README.md` (`lx.gfx:primitives`), rendering-performance.md §6 (batching now real) |
| 1 | rendering-performance.md §1.2 (UI measured block), UML 33 (pools: ring, atlas, pipeline cache) |
| 2 | ui-architecture.md §6 (text emission), a `docs/subsystems/text.md` |
| 3 | ui-architecture.md §§4–5, 9; `docs/api/README.md` (style, theme, components) |
| 4 | threading.md (client frame loop), memory-management.md §3 (new caps), architecture.md §2.3 and §6 |
| 5 | This ADR → Accepted; `docs/subsystems/ui-engine.md`; architecture.md §13 status table; AGENTS.md §5 invariants |
