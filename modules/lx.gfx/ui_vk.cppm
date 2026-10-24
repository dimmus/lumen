module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdint.h>

import lx.foundation;
import lx.runtime;

export module lx.gfx:ui_vk;

import std;

import :primitives;
import :atlas;
import :damage;
import :renderer;
import :vk_renderer;
import :ui_cpu;

export namespace lx::gfx {

// ── The UI primitive renderer, Vulkan (ADR-010 §4.4, Phase 1) ───────────────────────
//
// Draws one view's sorted primitive stream (`ui_stream`, the input `ui_cpu_renderer` and
// `ui_gl_renderer` take) into a colour attachment, inside the damage rects, at pixel parity
// with the CPU reference (G10: within 3/255 per channel) and with `:ui_gl` (2/255). The pixel
// rules are `:ui_cpu`'s; the shaders are shaders/ui/*.glsl — the very files `:ui_gl` compiles
// for GLES — compiled to SPIR-V at build time (ui_<kind>.vert/.frag add the Vulkan prologue,
// shaders/ui/vk_*.glsl) and embedded in the binary.
//
// Device. The `vk_context` the compositor's `vulkan_compositor` uses (`device::context()`):
// Vulkan 1.1, no feature or extension beyond what `device::create` enables. The renderer
// has its own command pool, command buffers and fences on the context's queue family; the
// queue is shared, so `draw` runs on the thread that submits to it (render in the
// compositor, ui in a client — ADR-010 §4.12). A CPU device (lavapipe) is accepted and
// reported in `caps().software`: correct, never a performance number.
//
// Instances. One host-visible, host-coherent buffer, mapped once: `k_ui_vk_ring_slots` slots
// of `k_ui_vk_slot_bytes`, used as a ring — a frame writes its slot, the slot's fence closes
// it, and the slot is reused three frames later once the fence has signalled. All kinds pack
// into the slot, each batch contiguous and in sorted order (`ui_encode::encode`, the same
// encoder `:ui_gl` uses). The vertex stage reads its instance from the slot as a std430
// `uint[]` at `base + gl_InstanceIndex · stride`: `base`, the batch's first word, is a push
// constant (with the batch's atlas page), and every draw is `vkCmdDraw(6, n, 0, 0)` — the
// unit quad from `gl_VertexIndex`, no vertex buffer. (The push constant rather than
// `firstInstance`: kinds have different strides, and a batch's offset need not be a multiple
// of its stride.)
//
// Descriptors. One layout, set 0: binding 0 the globals (a std140 block: the target size;
// each slot has its own 256-byte block after the ring), binding 1 the slot's instances,
// binding 2 eight combined image samplers, the atlas pages by global page id (0–3 R8, 4–5
// RGBA8; unused entries a 1×1 dummy). One set per ring slot, rewritten only when a page image
// changed since the slot last drew — after that slot's fence, so never under the GPU.
//
// Pipelines. Five, one per program of `:ui_gl` — shadow, quad, mono sprite (also `path`),
// poly sprite, underline — created for a colour format and a render pass of it (loadOp
// LOAD, the attachment in COLOR_ATTACHMENT_OPTIMAL before and after), cached per format
// (`k_ui_vk_pipeline_sets`; render pass compatibility is the format). `surface` primitives
// go through the compositor's composite pipeline (`vulkan_compositor::record_blits`), made
// for the same pass. All of them come out of one VkPipelineCache persisted at
// $XDG_CACHE_HOME/lumen/pipeline-cache-<pipelineCacheUUID>.bin: read at `initialize` when its
// header (vendor, device, pipelineCacheUUID) matches the device, written (temporary + rename)
// when creating pipelines grew it, and at `shutdown` if it is still unwritten — never when
// the driver's data would not pass that header check itself (lavapipe returns a truncated
// 20-byte header and caches nothing: on it a warm start is a cold one).
//
// Blending. ONE, ONE_MINUS_SRC_ALPHA into an 8-bit UNORM attachment holding encoded values
// (ADR-010 §4.5): the CPU's premultiplied source-over, one byte rounding per primitive.
//
// Atlas. One device-local image per created page (R8 for 0–3, RGBA8 for 4–5), sized to the
// page. Before the render pass, each page with dirty rects gets one vkCmdCopyBufferToImage
// with one region per rect, from the frame slot's staging range (`k_ui_vk_staging_bytes`,
// host-coherent, mapped once), between a barrier into TRANSFER_DST and one back to
// SHADER_READ_ONLY. A page that grew is a new image, uploaded whole (an image another frame
// may still sample is destroyed after waiting for the ring's fences — page growth, not the
// frame path). An upload larger than the slot's staging range — whole pages, after growth or
// for an atlas the renderer has not seen — goes through a buffer made for that frame and
// freed when its slot comes round again (`draw_stats::atlas_transient`). Nothing waits for
// the queue to go idle. The atlas's dirty lists are cleared, and a new page image counted as
// holding its contents, only once the frame that uploads them is submitted: a frame that
// fails before its submit records copies that never run, so the next draw uploads the same
// rects again (a new page whole, from UNDEFINED). A frame whose surface draw fails to record
// inside the pass is still ended and submitted, so its copies and the compositor's acquires
// (`prepare_blits`) execute as their CPU-side state says they did.
//
// Damage. One render pass over the whole attachment; per damage rect: the scissor, a
// vkCmdClearAttachments of the rect (when a clear colour is given), then every batch whose
// bounds meet the rect — `ui_gl_renderer`'s order and skip rule.
//
// GPU timing. Timestamps at the top and bottom of the frame's command buffer when the queue
// family has valid timestamp bits; otherwise the wall time from submit to the frame's fence.
//
// Nothing in `draw` allocates on the host except the transient staging buffer above.

inline constexpr unsigned k_ui_vk_ring_slots = 3;
inline constexpr unsigned k_ui_vk_slot_bytes = 2u << 20u;
/// Per ring slot: the atlas bytes one frame can upload without a transient buffer (a glyph
/// run's dirty rects are a few KiB; a whole 1024² RGBA8 page is 4 MiB).
inline constexpr unsigned k_ui_vk_staging_bytes = 1u << 20u;
/// Image entries of descriptor binding 2 (`k_atlas_pages` used).
inline constexpr unsigned k_ui_vk_page_slots = 8;
/// Colour formats with a pipeline set at once; another evicts the oldest.
inline constexpr unsigned k_ui_vk_pipeline_sets = 4;
/// Framebuffers kept (one per attachment: a client's swapchain images, the tests' targets).
inline constexpr unsigned k_ui_vk_framebuffers = 4;
/// Copy regions per page per frame (ADR-010 §4.12's upload list); more dirty rects than this
/// upload the page whole.
inline constexpr unsigned k_ui_vk_upload_regions = 256;

/// VkFormat values of the attachments the renderer is built for (the partition interface
/// carries no Vulkan type).
inline constexpr std::uint32_t k_ui_vk_rgba8_unorm = 37; ///< VK_FORMAT_R8G8B8A8_UNORM
inline constexpr std::uint32_t k_ui_vk_bgra8_unorm = 44; ///< VK_FORMAT_B8G8R8A8_UNORM

static_assert(k_atlas_pages <= k_ui_vk_page_slots);

/// Feature switches for `initialize` (on by default; the tests turn them off to prove the
/// fallback paths draw the same pixels).
struct ui_vk_options {
    bool pipeline_cache = true; ///< read / write the on-disk VkPipelineCache
    bool timestamps = true;     ///< timestamp queries for `wait_gpu_time`
    /// The attachment format `initialize` builds the pipelines for (others are built on first
    /// use): a VkFormat, `k_ui_vk_rgba8_unorm` or `k_ui_vk_bgra8_unorm`.
    std::uint32_t format = k_ui_vk_rgba8_unorm;
};

struct ui_vk_caps {
    char device_name[256]{};
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;
    char pipeline_cache_uuid[33]{}; ///< hex
    /// VK_PHYSICAL_DEVICE_TYPE_CPU (lavapipe, SwiftShader): correct, never a performance number.
    bool software = false;
    bool timestamps = false;          ///< used by `wait_gpu_time`
    double timestamp_period_ns = 0.0; ///< VkPhysicalDeviceLimits::timestampPeriod
    bool pipeline_cache = false;      ///< the cache file is read and written
    char pipeline_cache_path[600]{};
};

/// Pipeline setup: cold (an empty cache) or warm (the cache file read at `initialize`).
struct ui_vk_setup_stats {
    std::uint64_t module_ns = 0;   ///< shader modules (CLOCK_MONOTONIC)
    std::uint64_t pipeline_ns = 0; ///< every pipeline set made so far: render pass + pipelines
    unsigned pipeline_sets = 0;
    unsigned pipelines = 0;
    std::uint64_t cache_bytes_loaded = 0; ///< 0: no file, or a rejected one
    bool cache_rejected = false; ///< a file was there, its header did not match the device
    unsigned cache_writes = 0;
    std::uint64_t cache_bytes_written = 0;
};

struct ui_vk_draw_stats {
    unsigned rects = 0;               ///< damage rects drawn
    unsigned batches_drawn = 0;       ///< (batch, rect) pairs that met
    unsigned batches_skipped = 0;     ///< (batch, rect) pairs whose bounds missed
    unsigned draw_calls = 0;          ///< instanced draws plus surface draws
    unsigned primitives_encoded = 0;  ///< instances written to the ring
    unsigned primitives_skipped = 0;  ///< bad index, missing or mismatched page, bad uv, no compositor
    unsigned overflow_drops = 0;      ///< primitives of batches that did not fit the slot (dropped)
    std::uint64_t instance_bytes = 0; ///< bytes written to the slot
    unsigned atlas_uploads = 0;       ///< copy regions (dirty rects, or whole pages)
    unsigned atlas_copies = 0;        ///< vkCmdCopyBufferToImage calls (one per page uploaded)
    std::uint64_t atlas_upload_bytes = 0;
    bool atlas_transient = false;     ///< the upload outgrew the slot's staging range
    unsigned slot = 0;                ///< ring slot of this frame
    std::uint64_t slot_wait_ns = 0;   ///< waiting for the slot's previous fence
};

/// What `ui_vk_renderer::composite` takes beyond the frame (ADR-010 §4.5, §4.11).
struct ui_vk_composite_options {
    /// The `mono_sprite` coverage exponent in the linear pass (theme `text.contrast`):
    /// `k_text_contrast_dark` for light text on dark chrome, `k_text_contrast_light` for dark on
    /// light.
    float text_contrast = k_text_contrast_dark;
};

/// One compositor frame drawn by `ui_vk_renderer::composite`: the UI renderer's side (instances,
/// atlas uploads, draws) and the compositor's (surfaces, the out fence — owned by the caller,
/// as `vulkan_compositor::composite`'s).
struct ui_vk_composite_stats {
    ui_vk_draw_stats ui{};
    composite_stats composite{};
};

/// How `wait_gpu_time` measured.
enum class ui_vk_timing : std::uint8_t {
    none,       ///< no frame was timed
    timestamp,  ///< timestamps at the top and bottom of the frame's command buffer
    fence_wall, ///< no timestamps: wall time from submit to the frame's fence
};

struct ui_vk_gpu_time {
    ui_vk_timing source = ui_vk_timing::none;
    std::uint64_t ns = 0;
};

/// A colour attachment to draw into: the image and a 2D colour view of it, `format` a
/// VkFormat with a UNORM blend (`k_ui_vk_rgba8_unorm`, `k_ui_vk_bgra8_unorm`). The image is in
/// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL before a draw and is left there. `serial` names the
/// attachment for the framebuffer cache: unique per image view over the renderer's life (a
/// view handle can be reused once destroyed; a serial is not).
struct ui_vk_attachment {
    void* image = nullptr;
    void* view = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t format = k_ui_vk_rgba8_unorm;
    std::uint64_t serial = 0;
};

/// An offscreen colour image the tests and the bench draw into (encoded values, no sRGB
/// view): device-local, optimal tiling, cleared to 0 and left in COLOR_ATTACHMENT_OPTIMAL.
class ui_vk_target {
public:
    ui_vk_target() = default;
    ~ui_vk_target();
    ui_vk_target(const ui_vk_target&) = delete;
    ui_vk_target& operator=(const ui_vk_target&) = delete;
    ui_vk_target(ui_vk_target&& other) noexcept;
    ui_vk_target& operator=(ui_vk_target&& other) noexcept;

    /// Waits for the GPU once (the clear): set-up, not the frame path.
    [[nodiscard]] static lx::result<ui_vk_target> create(const vk_context& ctx, unsigned width,
                                                         unsigned height,
                                                         std::uint32_t format = k_ui_vk_rgba8_unorm);

    [[nodiscard]] bool valid() const { return image_ != nullptr; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    [[nodiscard]] ui_vk_attachment attachment() const {
        return {image_, view_, width_, height_, format_, serial_};
    }

    /// Reads the target back as XRGB8888 / ARGB8888 words (B, G, R, A in memory), rows top
    /// first, `stride_px` words apart (0: the width); `opaque` writes 255 in the fourth byte.
    /// Submits after whatever was submitted before and waits: tests and the bench only.
    [[nodiscard]] lx::result<void> read_back(std::span<std::uint32_t> out, unsigned stride_px = 0,
                                             bool opaque = true) const;

private:
    void destroy();

    vk_context ctx_{};
    void* image_ = nullptr;
    void* memory_ = nullptr;
    void* view_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
    std::uint32_t format_ = k_ui_vk_rgba8_unorm;
    std::uint64_t serial_ = 0;
};

class ui_vk_renderer {
public:
    ui_vk_renderer() = default;
    /// Calls `shutdown`.
    ~ui_vk_renderer();
    ui_vk_renderer(const ui_vk_renderer&) = delete;
    ui_vk_renderer& operator=(const ui_vk_renderer&) = delete;

    /// Creates the ring, the descriptor layout and sets, the shader modules and the pipelines
    /// for `options.format`, reading the pipeline cache first. Set-up: it waits for the GPU
    /// once (the dummy page's first layout).
    [[nodiscard]] lx::result<void> initialize(const vk_context& ctx,
                                              const ui_vk_options& options = {});
    /// Waits for the ring's frames (their fences, not the queue), writes the pipeline cache if
    /// it grew since it was last written, and frees everything.
    void shutdown();
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const ui_vk_caps& caps() const { return caps_; }
    [[nodiscard]] const ui_vk_setup_stats& setup_stats() const { return setup_; }

    /// Where `surface` refs' textures are registered (same device). Borrowed; null skips
    /// surfaces (counted in `primitives_skipped`). Lifetime: the compositor's descriptor sets
    /// and images are recorded into this renderer's command buffers, which the compositor's
    /// own fences do not cover, so the renderer registers itself as the compositor's external
    /// user (`vulkan_compositor::add_external_user`): before the compositor destroys,
    /// re-uploads or re-points a texture a `draw` recorded, it waits for this renderer's
    /// frames (`wait_idle`). A compositor shutting down detaches the renderer (the pointer is
    /// cleared). Not used by `composite`, which records into the compositor's own frame.
    void set_surface_compositor(vulkan_compositor* compositor);

    /// The affinity `draw` and `wait_gpu_time` must run on (ADR-010 §4.12): `render` in the
    /// compositor, `ui` in a client. `any` (the default) checks nothing — the tests and the
    /// bench, which drive the renderer from one thread.
    void set_owner_affinity(lx::runtime::affinity owner) { owner_ = owner; }

    /// Times each frame's GPU work, read with `wait_gpu_time`. Off by default.
    void set_gpu_timing(bool on) { timing_ = on; }

    /// Draws `stream` into `target` inside `damage`, clearing each damage rect to `clear`
    /// (premultiplied, encoded) first when given. Uploads the atlas's dirty pages first and,
    /// once the frame is submitted, clears its dirty lists (`atlas` may be null when the
    /// stream has no sprites; a frame that fails before its submit leaves them for the next
    /// draw). An empty region draws nothing and submits nothing. Submits; never waits for the
    /// GPU except for a ring slot still in flight three frames on.
    [[nodiscard]] lx::result<ui_vk_draw_stats> draw(const ui_vk_attachment& target,
                                                    const ui_stream& stream, atlas* atlas,
                                                    const damage_region& damage,
                                                    std::optional<lx::color> clear = std::nullopt);
    [[nodiscard]] lx::result<ui_vk_draw_stats> draw(const ui_vk_target& target,
                                                    const ui_stream& stream, atlas* atlas,
                                                    const damage_region& damage,
                                                    std::optional<lx::color> clear = std::nullopt) {
        return draw(target.attachment(), stream, atlas, damage, clear);
    }

    /// ADR-010 §4.11: the compositor's frame for `target` with `stream` recorded inside
    /// `compositor`'s linear F16 pass, then its encode pass, in one command buffer of the
    /// compositor's (`vulkan_compositor::begin_linear_frame` … `end_linear_frame`): the atlas
    /// copies before the pass, `surface` refs through `record_blits` (decoded as `composite`
    /// decodes them), primitive batches through this renderer's pipelines made for that pass,
    /// with instance colours converted to premultiplied linear light and `mono_sprite`
    /// coverage raised to `options.text_contrast` (the push constant's linear flag; shaders/ui
    /// `lx_instance_color`, `lx_text_coverage`). The whole target is drawn (Vulkan repaints
    /// every frame) and cleared to `clear` first. The ring slot is fenced by an empty submit
    /// after the compositor's; the atlas's dirty lists are cleared once that submit is made.
    /// On an error nothing is submitted, the compositor's acquires are un-marked and the atlas
    /// stays dirty. Render affinity in the compositor (`set_owner_affinity`).
    [[nodiscard]] lx::result<ui_vk_composite_stats>
    composite(vulkan_compositor& compositor, render_target& target, lx::color clear,
              const ui_stream& stream, atlas* atlas, const ui_vk_composite_options& options = {});

    /// The GPU time of the last timed frame; waits for it. `none` when timing is off or no
    /// frame was drawn since the last call.
    [[nodiscard]] lx::result<ui_vk_gpu_time> wait_gpu_time();

    /// Waits for every frame in the ring (their fences). Before destroying an attachment the
    /// renderer may still be drawing into.
    void wait_idle();

private:
    enum pipeline_index : unsigned { p_shadow, p_quad, p_mono, p_poly, p_underline, p_count };
    struct pipeline_set {
        std::uint32_t format = 0;
        void* render_pass = nullptr;
        void* pipelines[p_count]{};
        /// Made for a pass the caller owns (the compositor's linear pass): not destroyed here,
        /// no framebuffer of ours uses it.
        bool external = false;
    };
    struct framebuffer_entry {
        std::uint64_t serial = 0;
        void* view = nullptr;
        void* render_pass = nullptr;
        void* framebuffer = nullptr;
        unsigned width = 0;
        unsigned height = 0;
    };
    struct page_image {
        void* image = nullptr;
        void* memory = nullptr;
        void* view = nullptr;
        unsigned size = 0;
    };
    struct transient_buffer {
        void* buffer = nullptr;
        void* memory = nullptr;
    };

    /// The pipeline set for `format`, made for our own LOAD pass, or, with `external_pass`, for
    /// that pass (any format the blend supports: the compositor's R16G16B16A16_SFLOAT).
    [[nodiscard]] lx::result<unsigned> ensure_pipelines(std::uint32_t format,
                                                        void* external_pass = nullptr);
    [[nodiscard]] lx::result<void*> ensure_framebuffer(const ui_vk_attachment& target,
                                                       void* render_pass);
    [[nodiscard]] lx::result<void> upload_atlas(atlas& a, void* command_buffer, unsigned slot,
                                                ui_vk_draw_stats& stats);
    [[nodiscard]] lx::result<void> create_page(unsigned page, unsigned size, bool r8);
    void destroy_page(page_image& p);
    void free_transient(unsigned slot);
    void write_page_descriptors(unsigned slot);
    void save_pipeline_cache();
    void wait_slots();
    static void wait_external_thunk(void* self);
    static void detach_external_thunk(void* self);

    vk_context ctx_{};
    ui_vk_caps caps_{};
    ui_vk_setup_stats setup_{};
    ui_vk_options options_{};
    void* command_pool_ = nullptr;
    void* command_buffers_[k_ui_vk_ring_slots]{};
    void* fences_[k_ui_vk_ring_slots]{};
    bool submitted_[k_ui_vk_ring_slots]{};
    void* query_pool_ = nullptr;
    unsigned timestamp_bits_ = 0;
    /// The ring slots, then one 256-byte globals block per slot. Mapped once.
    void* ring_buffer_ = nullptr;
    void* ring_memory_ = nullptr;
    std::byte* ring_mapped_ = nullptr;
    void* staging_buffer_ = nullptr;
    void* staging_memory_ = nullptr;
    std::byte* staging_mapped_ = nullptr;
    transient_buffer transient_[k_ui_vk_ring_slots]{};
    void* sampler_ = nullptr;
    void* dummy_image_ = nullptr;
    void* dummy_memory_ = nullptr;
    void* dummy_view_ = nullptr;
    void* set_layout_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* sets_[k_ui_vk_ring_slots]{};
    std::uint64_t set_generation_[k_ui_vk_ring_slots]{};
    std::uint64_t page_generation_ = 0;
    void* modules_[2 * p_count]{}; ///< vertex, fragment per pipeline
    void* pipeline_cache_ = nullptr;
    std::size_t cache_bytes_saved_ = 0; ///< the cache's size when last read or written
    pipeline_set pipeline_sets_[k_ui_vk_pipeline_sets]{};
    unsigned next_pipeline_set_ = 0;
    framebuffer_entry framebuffers_[k_ui_vk_framebuffers]{};
    unsigned next_framebuffer_ = 0;
    page_image pages_[k_atlas_pages]{};
    const atlas* atlas_seen_ = nullptr;
    /// A page image made and not yet uploaded by a submitted frame: its next upload is whole,
    /// from UNDEFINED.
    bool page_whole_pending_[k_atlas_pages]{};
    /// Pages this frame's `upload_atlas` recorded: their dirty lists are cleared at submit.
    bool page_uploaded_[k_atlas_pages]{};
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
    unsigned frame_ = 0;
    int timed_slot_ = -1; ///< slot of the last timed frame, until `wait_gpu_time`
    std::uint64_t timed_submit_ns_ = 0;
    /// Per stream batch: word offset in the slot and instances written (0: not drawn).
    std::uint32_t batch_word_[k_max_batches]{};
    std::uint32_t batch_count_[k_max_batches]{};
    /// VkBufferImageCopy[k_ui_vk_upload_regions] (56 bytes each, 8-aligned): the regions of
    /// one page's copy, reused page after page (a copy reads them when it is recorded).
    std::uint64_t regions_[k_ui_vk_upload_regions * 7]{};
    vulkan_compositor* compositor_ = nullptr;
    bool ready_ = false;
    bool timing_ = false;
};

} // namespace lx::gfx

// ── Shaders and helpers ─────────────────────────────────────────────────────────

namespace lx::gfx::ui_vk_detail {

#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)

// glslc -mfmt=c: the SPIR-V of shaders/ui_<kind>.{vert,frag} as initializer lists.
constexpr std::uint32_t k_shadow_vert[] =
#include "shaders/ui_shadow.vert.spv.h"
    ;
constexpr std::uint32_t k_shadow_frag[] =
#include "shaders/ui_shadow.frag.spv.h"
    ;
constexpr std::uint32_t k_quad_vert[] =
#include "shaders/ui_quad.vert.spv.h"
    ;
constexpr std::uint32_t k_quad_frag[] =
#include "shaders/ui_quad.frag.spv.h"
    ;
constexpr std::uint32_t k_mono_vert[] =
#include "shaders/ui_mono.vert.spv.h"
    ;
constexpr std::uint32_t k_mono_frag[] =
#include "shaders/ui_mono.frag.spv.h"
    ;
constexpr std::uint32_t k_poly_vert[] =
#include "shaders/ui_poly.vert.spv.h"
    ;
constexpr std::uint32_t k_poly_frag[] =
#include "shaders/ui_poly.frag.spv.h"
    ;
constexpr std::uint32_t k_underline_vert[] =
#include "shaders/ui_underline.vert.spv.h"
    ;
constexpr std::uint32_t k_underline_frag[] =
#include "shaders/ui_underline.frag.spv.h"
    ;

struct spirv {
    const std::uint32_t* code;
    std::size_t bytes;
};

/// By pipeline index (shadow, quad, mono, poly, underline), vertex then fragment.
constexpr spirv k_modules[] = {
    {k_shadow_vert, sizeof k_shadow_vert},       {k_shadow_frag, sizeof k_shadow_frag},
    {k_quad_vert, sizeof k_quad_vert},           {k_quad_frag, sizeof k_quad_frag},
    {k_mono_vert, sizeof k_mono_vert},           {k_mono_frag, sizeof k_mono_frag},
    {k_poly_vert, sizeof k_poly_vert},           {k_poly_frag, sizeof k_poly_frag},
    {k_underline_vert, sizeof k_underline_vert}, {k_underline_frag, sizeof k_underline_frag},
};

#endif

#if defined(LUMEN_HAS_VULKAN)

/// Each slot's globals block (std140 `lx_globals`: vec2 target, vec2 pad), after the ring.
constexpr std::size_t k_globals_stride = 256;
constexpr std::size_t k_globals_offset =
    static_cast<std::size_t>(k_ui_vk_ring_slots) * k_ui_vk_slot_bytes;
constexpr std::size_t k_ring_bytes = k_globals_offset + k_ui_vk_ring_slots * k_globals_stride;
constexpr std::uint64_t k_fence_timeout_ns = 2'000'000'000ull;

/// The push constants of every UI pipeline (both stages): the batch's first word in the slot,
/// its atlas page, and the target's blending space — `linear` 1 inside the compositor's F16
/// pass, where colours are decoded and glyph coverage goes through `contrast` (shaders/ui
/// vk_vertex.glsl, vk_fragment.glsl).
struct push_block {
    std::uint32_t base = 0;
    std::uint32_t page = 0;
    std::uint32_t linear = 0;
    float contrast = 1.f;
};
static_assert(sizeof(push_block) == 16);

[[nodiscard]] inline lx::error vk_fail(const char* message) {
    return lx::make_error(lx::vulkan_err::device_lost, message);
}

[[nodiscard]] inline lx::error vk_oom(const char* message) {
    return lx::make_error(lx::vulkan_err::out_of_memory, message);
}

[[nodiscard]] inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] inline int find_memory(VkPhysicalDevice phys, std::uint32_t bits,
                                     VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return static_cast<int>(i);
    return -1;
}

/// A buffer with its own allocation, mapped when `mapped` is given.
[[nodiscard]] inline bool make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                                      VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                      VkBuffer& buffer, VkDeviceMemory& memory, void** mapped) {
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    const int type = find_memory(phys, req.memoryTypeBits, props);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = static_cast<std::uint32_t>(type);
    if (type < 0 || vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS ||
        (mapped && vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS)) {
        if (memory)
            vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/// A device-local 2D image with one mip and a colour view of it.
[[nodiscard]] inline bool make_image(VkDevice dev, VkPhysicalDevice phys, VkFormat format,
                                     unsigned width, unsigned height, VkImageUsageFlags usage,
                                     VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, image, &req);
    const int type = find_memory(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = static_cast<std::uint32_t>(type);
    bool ok = type >= 0 && vkAllocateMemory(dev, &mai, nullptr, &memory) == VK_SUCCESS &&
              vkBindImageMemory(dev, image, memory, 0) == VK_SUCCESS;
    if (ok) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        ok = vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS;
    }
    if (!ok) {
        if (memory)
            vkFreeMemory(dev, memory, nullptr);
        vkDestroyImage(dev, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
    }
    return ok;
}

inline void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/// A command buffer for set-up work (target creation, read-back, the dummy page): its own
/// pool, submitted once and waited for. Never on the frame path.
class one_shot {
public:
    explicit one_shot(const lx::gfx::vk_context& ctx)
        : dev_{static_cast<VkDevice>(ctx.device)}, queue_{static_cast<VkQueue>(ctx.queue)} {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx.queue_family;
        if (vkCreateCommandPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS)
            return;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev_, &ai, &cb_) != VK_SUCCESS) {
            cb_ = VK_NULL_HANDLE;
            return;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb_, &begin) != VK_SUCCESS)
            cb_ = VK_NULL_HANDLE;
    }
    ~one_shot() {
        if (pool_)
            vkDestroyCommandPool(dev_, pool_, nullptr);
    }
    one_shot(const one_shot&) = delete;
    one_shot& operator=(const one_shot&) = delete;

    [[nodiscard]] VkCommandBuffer cb() const { return cb_; }

    [[nodiscard]] bool submit_and_wait() {
        if (!cb_ || vkEndCommandBuffer(cb_) != VK_SUCCESS)
            return false;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev_, &fci, nullptr, &fence) != VK_SUCCESS)
            return false;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb_;
        const bool ok = vkQueueSubmit(queue_, 1, &submit, fence) == VK_SUCCESS &&
                        vkWaitForFences(dev_, 1, &fence, VK_TRUE, k_fence_timeout_ns) == VK_SUCCESS;
        vkDestroyFence(dev_, fence, nullptr);
        return ok;
    }

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
};

/// $XDG_CACHE_HOME/lumen (or ~/.cache/lumen), created; false when neither is set or the
/// directory cannot be made.
[[nodiscard]] inline bool cache_dir(char (&out)[512]) {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    int n = 0;
    if (xdg && *xdg) {
        n = std::snprintf(out, sizeof out, "%s", xdg);
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home)
            return false;
        n = std::snprintf(out, sizeof out, "%s/.cache", home);
    }
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof out - 16)
        return false;
    ::mkdir(out, 0700);
    std::strcat(out, "/lumen");
    if (::mkdir(out, 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

/// VkPipelineCache data starts with a header (VK_PIPELINE_CACHE_HEADER_VERSION_ONE): its
/// length, its version, the vendor and device ids and the pipelineCacheUUID. A file whose
/// header names another device or driver build is not handed to the driver.
[[nodiscard]] inline bool cache_header_matches(std::span<const std::uint8_t> data,
                                               const VkPhysicalDeviceProperties& props) {
    if (data.size() < 32)
        return false;
    std::uint32_t words[4]{};
    std::memcpy(words, data.data(), sizeof words);
    return words[0] >= 32 && words[0] <= data.size() &&
           words[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && words[2] == props.vendorID &&
           words[3] == props.deviceID &&
           std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

/// The UI pipelines' render pass for one colour attachment of `format`: the attachment's
/// contents kept (loadOp LOAD — a damage rect is all that is drawn), COLOR_ATTACHMENT_OPTIMAL
/// before and after. Its writes wait for the previous frame's and for a read-back's copy.
[[nodiscard]] inline VkRenderPass make_render_pass(VkDevice dev, VkFormat format) {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &pass) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pass;
}

[[nodiscard]] inline bool supported_format(std::uint32_t format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

#endif // LUMEN_HAS_VULKAN

} // namespace lx::gfx::ui_vk_detail

// ── ui_vk_target ────────────────────────────────────────────────────────────────

namespace {

/// Framebuffer-cache names for `ui_vk_target`s: never reused within a process.
std::atomic<std::uint64_t> g_ui_vk_target_serial{1};

} // namespace

lx::gfx::ui_vk_target::~ui_vk_target() { destroy(); }

lx::gfx::ui_vk_target::ui_vk_target(ui_vk_target&& other) noexcept { *this = std::move(other); }

lx::gfx::ui_vk_target& lx::gfx::ui_vk_target::operator=(ui_vk_target&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    ctx_ = other.ctx_;
    image_ = other.image_;
    memory_ = other.memory_;
    view_ = other.view_;
    width_ = other.width_;
    height_ = other.height_;
    format_ = other.format_;
    serial_ = other.serial_;
    other.image_ = nullptr;
    other.memory_ = nullptr;
    other.view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    other.serial_ = 0;
    return *this;
}

void lx::gfx::ui_vk_target::destroy() {
#if defined(LUMEN_HAS_VULKAN)
    if (auto dev = static_cast<VkDevice>(ctx_.device); dev) {
        if (view_)
            vkDestroyImageView(dev, static_cast<VkImageView>(view_), nullptr);
        if (image_)
            vkDestroyImage(dev, static_cast<VkImage>(image_), nullptr);
        if (memory_)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(memory_), nullptr);
    }
#endif
    image_ = nullptr;
    memory_ = nullptr;
    view_ = nullptr;
    width_ = 0;
    height_ = 0;
}

lx::result<lx::gfx::ui_vk_target> lx::gfx::ui_vk_target::create(const vk_context& ctx,
                                                                unsigned width, unsigned height,
                                                                std::uint32_t format) {
#if defined(LUMEN_HAS_VULKAN)
    using namespace ui_vk_detail;
    if (!ctx.valid())
        return lx::not_implemented("lx::gfx::ui_vk_target::create");
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || !supported_format(format))
        return lx::invalid_argument("ui_vk_target::create: bad size or format");
    auto dev = static_cast<VkDevice>(ctx.device);
    ui_vk_target t;
    t.ctx_ = ctx;
    t.width_ = width;
    t.height_ = height;
    t.format_ = format;
    t.serial_ = g_ui_vk_target_serial.fetch_add(1, std::memory_order_relaxed);
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    if (!make_image(dev, static_cast<VkPhysicalDevice>(ctx.physical_device),
                    static_cast<VkFormat>(format), width, height,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    image, memory, view))
        return vk_oom("ui_vk_target: image creation failed");
    t.image_ = image;
    t.memory_ = memory;
    t.view_ = view;

    one_shot once{ctx};
    if (!once.cb())
        return vk_fail("ui_vk_target: no command buffer");
    image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (!once.submit_and_wait())
        return vk_fail("ui_vk_target: clear failed");
    return t;
#else
    (void)ctx;
    (void)width;
    (void)height;
    (void)format;
    return lx::not_implemented("lx::gfx::ui_vk_target::create");
#endif
}

lx::result<void> lx::gfx::ui_vk_target::read_back(std::span<std::uint32_t> out, unsigned stride_px,
                                                  bool opaque) const {
#if defined(LUMEN_HAS_VULKAN)
    using namespace ui_vk_detail;
    if (!valid())
        return lx::not_implemented("lx::gfx::ui_vk_target::read_back");
    if (stride_px == 0)
        stride_px = width_;
    if (stride_px < width_ ||
        out.size() < static_cast<std::size_t>(stride_px) * (height_ - 1u) + width_)
        return lx::invalid_argument("ui_vk_target::read_back: destination too small");
    auto dev = static_cast<VkDevice>(ctx_.device);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * 4u;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!make_buffer(dev, static_cast<VkPhysicalDevice>(ctx_.physical_device), bytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     buffer, memory, &mapped))
        return vk_oom("ui_vk_target::read_back: no host-visible buffer");
    bool ok = false;
    {
        one_shot once{ctx_};
        if (once.cb()) {
            auto image = static_cast<VkImage>(image_);
            image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {width_, height_, 1};
            vkCmdCopyImageToBuffer(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                                   &copy);
            image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            ok = once.submit_and_wait();
        }
    }
    if (ok) {
        const auto* src = static_cast<const std::uint32_t*>(mapped);
        const bool rgba = format_ == VK_FORMAT_R8G8B8A8_UNORM;
        for (unsigned y = 0; y < height_; ++y) {
            std::uint32_t* row = out.data() + static_cast<std::size_t>(y) * stride_px;
            const std::uint32_t* in = src + static_cast<std::size_t>(y) * width_;
            for (unsigned x = 0; x < width_; ++x) {
                std::uint32_t w = in[x];
                // R, G, B, A bytes to the B, G, R, A of an XRGB / ARGB word (a BGRA image
                // already is one).
                if (rgba)
                    w = (w & 0xFF00FF00u) | ((w & 0xFFu) << 16u) | ((w >> 16u) & 0xFFu);
                row[x] = opaque ? (w | 0xFF000000u) : w;
            }
        }
    }
    vkUnmapMemory(dev, memory);
    vkFreeMemory(dev, memory, nullptr);
    vkDestroyBuffer(dev, buffer, nullptr);
    if (!ok)
        return vk_fail("ui_vk_target::read_back: copy failed");
    return {};
#else
    (void)out;
    (void)stride_px;
    (void)opaque;
    return lx::not_implemented("lx::gfx::ui_vk_target::read_back");
#endif
}

// ── ui_vk_renderer: set-up ──────────────────────────────────────────────────────

lx::gfx::ui_vk_renderer::~ui_vk_renderer() {
    shutdown();
    set_surface_compositor(nullptr);
}

void lx::gfx::ui_vk_renderer::set_surface_compositor(vulkan_compositor* compositor) {
    if (compositor == compositor_)
        return;
    if (compositor_)
        compositor_->remove_external_user(this);
    compositor_ = compositor;
    // A full table (four users) leaves this renderer unregistered: its surface draws would be
    // unprotected, so it draws none.
    if (compositor_ &&
        !compositor_->add_external_user({this, &wait_external_thunk, &detach_external_thunk}))
        compositor_ = nullptr;
}

void lx::gfx::ui_vk_renderer::wait_external_thunk(void* self) {
    static_cast<ui_vk_renderer*>(self)->wait_slots();
}

void lx::gfx::ui_vk_renderer::detach_external_thunk(void* self) {
    static_cast<ui_vk_renderer*>(self)->compositor_ = nullptr;
}

lx::result<void> lx::gfx::ui_vk_renderer::initialize(const vk_context& ctx,
                                                     const ui_vk_options& options) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    using namespace ui_vk_detail;
    shutdown();
    if (!ctx.valid())
        return lx::not_implemented("lx::gfx::ui_vk_renderer::initialize");
    if (!supported_format(options.format))
        return lx::invalid_argument("ui_vk_renderer: unsupported attachment format");
    ctx_ = ctx;
    options_ = options;
    caps_ = {};
    setup_ = {};
    auto dev = static_cast<VkDevice>(ctx.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx.physical_device);
    const auto fail = [this](lx::error e) {
        shutdown();
        return e;
    };

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::snprintf(caps_.device_name, sizeof caps_.device_name, "%s", props.deviceName);
    caps_.vendor_id = props.vendorID;
    caps_.device_id = props.deviceID;
    caps_.api_version = props.apiVersion;
    caps_.software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        std::snprintf(caps_.pipeline_cache_uuid + 2 * i, 3, "%02x", props.pipelineCacheUUID[i]);
    caps_.timestamp_period_ns = static_cast<double>(props.limits.timestampPeriod);
    if (props.limits.minUniformBufferOffsetAlignment > k_globals_stride ||
        props.limits.minStorageBufferOffsetAlignment > k_globals_stride)
        return fail(vk_fail("ui_vk: buffer offset alignment above 256 bytes"));
    {
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, nullptr);
        VkQueueFamilyProperties family_props[16]{};
        families = families > 16 ? 16 : families;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, family_props);
        if (ctx.queue_family < families)
            timestamp_bits_ = family_props[ctx.queue_family].timestampValidBits;
    }

    // Command buffers and fences, one per ring slot, from a pool of our own: the compositor's
    // pool is its own to reset.
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkCreateCommandPool failed"));
    command_pool_ = pool;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = k_ui_vk_ring_slots;
    VkCommandBuffer cbs[k_ui_vk_ring_slots]{};
    if (vkAllocateCommandBuffers(dev, &cbai, cbs) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkAllocateCommandBuffers failed"));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (unsigned i = 0; i < k_ui_vk_ring_slots; ++i) {
        command_buffers_[i] = cbs[i];
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS)
            return fail(vk_fail("ui_vk: vkCreateFence failed"));
        fences_[i] = fence;
    }
    if (options.timestamps && timestamp_bits_ > 0 && caps_.timestamp_period_ns > 0.0) {
        VkQueryPoolCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2 * k_ui_vk_ring_slots;
        VkQueryPool qp = VK_NULL_HANDLE;
        if (vkCreateQueryPool(dev, &qci, nullptr, &qp) == VK_SUCCESS) {
            query_pool_ = qp;
            caps_.timestamps = true;
        }
    }

    // The instance ring and the globals blocks: one host-coherent buffer, mapped for life.
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        if (!make_buffer(dev, phys, k_ring_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         buffer, memory, &mapped))
            return fail(vk_oom("ui_vk: instance ring allocation failed"));
        ring_buffer_ = buffer;
        ring_memory_ = memory;
        ring_mapped_ = static_cast<std::byte*>(mapped);
    }
    // The atlas staging ranges, one per slot.
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        if (!make_buffer(dev, phys, static_cast<VkDeviceSize>(k_ui_vk_ring_slots) * k_ui_vk_staging_bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         buffer, memory, &mapped))
            return fail(vk_oom("ui_vk: staging allocation failed"));
        staging_buffer_ = buffer;
        staging_memory_ = memory;
        staging_mapped_ = static_cast<std::byte*>(mapped);
    }

    // Pages are read with texelFetch; the sampler is there because the binding is a combined
    // image sampler.
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkCreateSampler failed"));
    sampler_ = sampler;

    // The page slots no atlas page fills: a 1×1 transparent texel, readable from the start.
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        if (!make_image(dev, phys, VK_FORMAT_R8G8B8A8_UNORM, 1, 1,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image, memory,
                        view))
            return fail(vk_oom("ui_vk: dummy page creation failed"));
        dummy_image_ = image;
        dummy_memory_ = memory;
        dummy_view_ = view;
        one_shot once{ctx};
        if (!once.cb())
            return fail(vk_fail("ui_vk: no command buffer"));
        image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        image_barrier(once.cb(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        if (!once.submit_and_wait())
            return fail(vk_fail("ui_vk: dummy page clear failed"));
    }

    // Set 0: globals, instances, the page slots. One push range for both stages.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = k_ui_vk_page_slots;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 3;
    dlci.pBindings = bindings;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &set_layout) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkCreateDescriptorSetLayout failed"));
    set_layout_ = set_layout;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(push_block);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkCreatePipelineLayout failed"));
    pipeline_layout_ = pipeline_layout;

    VkDescriptorPoolSize sizes[3]{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, k_ui_vk_ring_slots};
    sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_ui_vk_ring_slots};
    sizes[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_ui_vk_ring_slots * k_ui_vk_page_slots};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = k_ui_vk_ring_slots;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptor_pool) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkCreateDescriptorPool failed"));
    descriptor_pool_ = descriptor_pool;
    VkDescriptorSetLayout layouts[k_ui_vk_ring_slots]{};
    for (auto& l : layouts)
        l = set_layout;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool;
    dsai.descriptorSetCount = k_ui_vk_ring_slots;
    dsai.pSetLayouts = layouts;
    VkDescriptorSet sets[k_ui_vk_ring_slots]{};
    if (vkAllocateDescriptorSets(dev, &dsai, sets) != VK_SUCCESS)
        return fail(vk_fail("ui_vk: vkAllocateDescriptorSets failed"));
    for (unsigned i = 0; i < k_ui_vk_ring_slots; ++i) {
        sets_[i] = sets[i];
        // Slot i's buffers never move: written once.
        VkDescriptorBufferInfo globals{};
        globals.buffer = static_cast<VkBuffer>(ring_buffer_);
        globals.offset = k_globals_offset + i * k_globals_stride;
        globals.range = 16;
        VkDescriptorBufferInfo instances{};
        instances.buffer = static_cast<VkBuffer>(ring_buffer_);
        instances.offset = static_cast<VkDeviceSize>(i) * k_ui_vk_slot_bytes;
        instances.range = k_ui_vk_slot_bytes;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &globals;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &instances;
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
        write_page_descriptors(i);
    }

    // Shader modules, kept for the pipelines of formats made later.
    const std::uint64_t m0 = now_ns();
    for (unsigned i = 0; i < 2 * p_count; ++i) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = k_modules[i].bytes;
        smci.pCode = k_modules[i].code;
        VkShaderModule module_handle = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &smci, nullptr, &module_handle) != VK_SUCCESS)
            return fail(vk_fail("ui_vk: shader module creation failed"));
        modules_[i] = module_handle;
    }
    setup_.module_ns = now_ns() - m0;

    // The pipeline cache, from disk when the file's header names this device.
    {
        std::vector<std::uint8_t> data;
        if (options.pipeline_cache) {
            char dir[512];
            if (cache_dir(dir)) {
                std::snprintf(caps_.pipeline_cache_path, sizeof caps_.pipeline_cache_path,
                              "%s/pipeline-cache-%s.bin", dir, caps_.pipeline_cache_uuid);
                caps_.pipeline_cache = true;
                if (std::FILE* f = std::fopen(caps_.pipeline_cache_path, "rb")) {
                    if (std::fseek(f, 0, SEEK_END) == 0) {
                        const long size = std::ftell(f);
                        if (size > 0 && size < (64l << 20) && std::fseek(f, 0, SEEK_SET) == 0) {
                            data.resize(static_cast<std::size_t>(size));
                            if (std::fread(data.data(), 1, data.size(), f) != data.size())
                                data.clear();
                        }
                    }
                    std::fclose(f);
                    if (!data.empty() && !cache_header_matches(data, props)) {
                        setup_.cache_rejected = true;
                        data.clear();
                    }
                }
            }
        }
        VkPipelineCacheCreateInfo pcci{};
        pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        pcci.initialDataSize = data.size();
        pcci.pInitialData = data.empty() ? nullptr : data.data();
        VkPipelineCache cache = VK_NULL_HANDLE;
        if (vkCreatePipelineCache(dev, &pcci, nullptr, &cache) != VK_SUCCESS) {
            // A driver may still refuse data its header admits: start empty.
            setup_.cache_rejected = !data.empty();
            data.clear();
            pcci.initialDataSize = 0;
            pcci.pInitialData = nullptr;
            if (vkCreatePipelineCache(dev, &pcci, nullptr, &cache) != VK_SUCCESS)
                return fail(vk_fail("ui_vk: vkCreatePipelineCache failed"));
        }
        pipeline_cache_ = cache;
        setup_.cache_bytes_loaded = data.size();
        std::size_t size = 0;
        (void)vkGetPipelineCacheData(dev, cache, &size, nullptr);
        cache_bytes_saved_ = data.empty() ? 0 : size;
    }

    ready_ = true;
    if (auto made = ensure_pipelines(options.format); !made) {
        const lx::error e = made.get_error();
        shutdown();
        return e;
    }
    return {};
#else
    (void)ctx;
    (void)options;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::initialize");
#endif
}

void lx::gfx::ui_vk_renderer::wait_slots() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return;
    VkFence fences[k_ui_vk_ring_slots]{};
    std::uint32_t n = 0;
    for (unsigned i = 0; i < k_ui_vk_ring_slots; ++i)
        if (fences_[i] && submitted_[i])
            fences[n++] = static_cast<VkFence>(fences_[i]);
    if (n > 0)
        (void)vkWaitForFences(dev, n, fences, VK_TRUE, ui_vk_detail::k_fence_timeout_ns);
#endif
}

void lx::gfx::ui_vk_renderer::wait_idle() { wait_slots(); }

void lx::gfx::ui_vk_renderer::shutdown() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        wait_slots();
        save_pipeline_cache();
        for (unsigned i = 0; i < k_ui_vk_ring_slots; ++i)
            free_transient(i);
        for (framebuffer_entry& f : framebuffers_)
            if (f.framebuffer)
                vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(f.framebuffer), nullptr);
        for (pipeline_set& set : pipeline_sets_) {
            for (void* p : set.pipelines)
                if (p)
                    vkDestroyPipeline(dev, static_cast<VkPipeline>(p), nullptr);
            if (set.render_pass && !set.external)
                vkDestroyRenderPass(dev, static_cast<VkRenderPass>(set.render_pass), nullptr);
        }
        for (page_image& p : pages_)
            destroy_page(p);
        if (pipeline_cache_)
            vkDestroyPipelineCache(dev, static_cast<VkPipelineCache>(pipeline_cache_), nullptr);
        for (void* m : modules_)
            if (m)
                vkDestroyShaderModule(dev, static_cast<VkShaderModule>(m), nullptr);
        if (descriptor_pool_)
            vkDestroyDescriptorPool(dev, static_cast<VkDescriptorPool>(descriptor_pool_), nullptr);
        if (pipeline_layout_)
            vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(pipeline_layout_), nullptr);
        if (set_layout_)
            vkDestroyDescriptorSetLayout(dev, static_cast<VkDescriptorSetLayout>(set_layout_),
                                         nullptr);
        if (dummy_view_)
            vkDestroyImageView(dev, static_cast<VkImageView>(dummy_view_), nullptr);
        if (dummy_image_)
            vkDestroyImage(dev, static_cast<VkImage>(dummy_image_), nullptr);
        if (dummy_memory_)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(dummy_memory_), nullptr);
        if (sampler_)
            vkDestroySampler(dev, static_cast<VkSampler>(sampler_), nullptr);
        if (staging_memory_) {
            vkUnmapMemory(dev, static_cast<VkDeviceMemory>(staging_memory_));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(staging_memory_), nullptr);
        }
        if (staging_buffer_)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(staging_buffer_), nullptr);
        if (ring_memory_) {
            vkUnmapMemory(dev, static_cast<VkDeviceMemory>(ring_memory_));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(ring_memory_), nullptr);
        }
        if (ring_buffer_)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(ring_buffer_), nullptr);
        if (query_pool_)
            vkDestroyQueryPool(dev, static_cast<VkQueryPool>(query_pool_), nullptr);
        for (void* f : fences_)
            if (f)
                vkDestroyFence(dev, static_cast<VkFence>(f), nullptr);
        if (command_pool_)
            vkDestroyCommandPool(dev, static_cast<VkCommandPool>(command_pool_), nullptr);
    }
#endif
    command_pool_ = nullptr;
    for (unsigned i = 0; i < k_ui_vk_ring_slots; ++i) {
        command_buffers_[i] = nullptr;
        fences_[i] = nullptr;
        submitted_[i] = false;
        transient_[i] = {};
        sets_[i] = nullptr;
        set_generation_[i] = 0;
    }
    query_pool_ = nullptr;
    ring_buffer_ = nullptr;
    ring_memory_ = nullptr;
    ring_mapped_ = nullptr;
    staging_buffer_ = nullptr;
    staging_memory_ = nullptr;
    staging_mapped_ = nullptr;
    sampler_ = nullptr;
    dummy_image_ = nullptr;
    dummy_memory_ = nullptr;
    dummy_view_ = nullptr;
    set_layout_ = nullptr;
    pipeline_layout_ = nullptr;
    descriptor_pool_ = nullptr;
    page_generation_ = 0;
    for (void*& m : modules_)
        m = nullptr;
    pipeline_cache_ = nullptr;
    cache_bytes_saved_ = 0;
    for (pipeline_set& set : pipeline_sets_)
        set = {};
    next_pipeline_set_ = 0;
    for (framebuffer_entry& f : framebuffers_)
        f = {};
    next_framebuffer_ = 0;
    for (page_image& p : pages_)
        p = {};
    atlas_seen_ = nullptr;
    timed_slot_ = -1;
    ready_ = false;
    caps_.timestamps = false;
    ctx_ = {};
}

void lx::gfx::ui_vk_renderer::save_pipeline_cache() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev || !pipeline_cache_ || !caps_.pipeline_cache || !caps_.pipeline_cache_path[0])
        return;
    auto cache = static_cast<VkPipelineCache>(pipeline_cache_);
    std::size_t size = 0;
    if (vkGetPipelineCacheData(dev, cache, &size, nullptr) != VK_SUCCESS || size == 0 ||
        size == cache_bytes_saved_)
        return; // nothing new since the file was read or written
    // Remembered as queried: the next call compares sizes, not contents (and a driver may
    // write fewer bytes than it asked for).
    cache_bytes_saved_ = size;
    std::vector<std::uint8_t> data(size);
    if (vkGetPipelineCacheData(dev, cache, &size, data.data()) != VK_SUCCESS)
        return;
    // Only what a later `initialize` would accept: lavapipe answers the size query with 32 and
    // then writes a 20-byte, truncated header (it keeps no pipelines in the cache anyway).
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(ctx_.physical_device), &props);
    if (!ui_vk_detail::cache_header_matches({data.data(), size}, props))
        return;
    // A temporary renamed over the file: a reader never sees half a cache.
    char tmp[640];
    std::snprintf(tmp, sizeof tmp, "%s.%d", caps_.pipeline_cache_path, static_cast<int>(::getpid()));
    std::FILE* out = std::fopen(tmp, "wb");
    if (!out)
        return;
    const bool ok = std::fwrite(data.data(), 1, size, out) == size;
    if (std::fclose(out) == 0 && ok && std::rename(tmp, caps_.pipeline_cache_path) == 0) {
        ++setup_.cache_writes;
        setup_.cache_bytes_written += size;
    } else {
        std::remove(tmp);
    }
#endif
}

lx::result<unsigned> lx::gfx::ui_vk_renderer::ensure_pipelines(std::uint32_t format,
                                                               void* external_pass) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    using namespace ui_vk_detail;
    for (unsigned i = 0; i < k_ui_vk_pipeline_sets; ++i) {
        const pipeline_set& p = pipeline_sets_[i];
        if (p.render_pass && p.format == format && p.external == (external_pass != nullptr) &&
            (!external_pass || p.render_pass == external_pass))
            return i;
    }
    if (!external_pass && !supported_format(format))
        return lx::invalid_argument("ui_vk_renderer: unsupported attachment format");
    auto dev = static_cast<VkDevice>(ctx_.device);
    const unsigned index = next_pipeline_set_;
    next_pipeline_set_ = (next_pipeline_set_ + 1u) % k_ui_vk_pipeline_sets;
    pipeline_set& set = pipeline_sets_[index];
    if (set.render_pass) {
        // A fifth format: the oldest goes, after the frames that may use it.
        wait_slots();
        for (framebuffer_entry& f : framebuffers_)
            if (f.framebuffer && f.render_pass == set.render_pass) {
                vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(f.framebuffer), nullptr);
                f = {};
            }
        for (void* p : set.pipelines)
            if (p)
                vkDestroyPipeline(dev, static_cast<VkPipeline>(p), nullptr);
        if (!set.external)
            vkDestroyRenderPass(dev, static_cast<VkRenderPass>(set.render_pass), nullptr);
        set = {};
    }

    const std::uint64_t t0 = now_ns();
    VkRenderPass pass = external_pass ? static_cast<VkRenderPass>(external_pass)
                                      : make_render_pass(dev, static_cast<VkFormat>(format));
    if (!pass)
        return vk_fail("ui_vk: vkCreateRenderPass failed");

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Premultiplied source-over in the encoded space, both colour and alpha (ADR-010 §4.5).
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineShaderStageCreateInfo stages[p_count][2]{};
    VkGraphicsPipelineCreateInfo infos[p_count]{};
    for (unsigned i = 0; i < p_count; ++i) {
        for (unsigned s = 0; s < 2; ++s) {
            stages[i][s].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i][s].stage = s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[i][s].module = static_cast<VkShaderModule>(modules_[2 * i + s]);
            stages[i][s].pName = "main";
        }
        VkGraphicsPipelineCreateInfo& g = infos[i];
        g.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        g.stageCount = 2;
        g.pStages = stages[i];
        g.pVertexInputState = &vertex_input;
        g.pInputAssemblyState = &input_assembly;
        g.pViewportState = &viewport_state;
        g.pRasterizationState = &raster;
        g.pMultisampleState = &multisample;
        g.pColorBlendState = &blend_state;
        g.pDynamicState = &dynamic;
        g.layout = static_cast<VkPipelineLayout>(pipeline_layout_);
        g.renderPass = pass;
        g.subpass = 0;
        g.basePipelineIndex = -1;
    }
    VkPipeline made[p_count]{};
    if (vkCreateGraphicsPipelines(dev, static_cast<VkPipelineCache>(pipeline_cache_), p_count,
                                  infos, nullptr, made) != VK_SUCCESS) {
        for (VkPipeline p : made)
            if (p)
                vkDestroyPipeline(dev, p, nullptr);
        if (!external_pass)
            vkDestroyRenderPass(dev, pass, nullptr);
        return vk_fail("ui_vk: vkCreateGraphicsPipelines failed");
    }
    setup_.pipeline_ns += now_ns() - t0;
    ++setup_.pipeline_sets;
    setup_.pipelines += p_count;
    set.format = format;
    set.render_pass = pass;
    set.external = external_pass != nullptr;
    for (unsigned i = 0; i < p_count; ++i)
        set.pipelines[i] = made[i];
    // New pipelines grew the cache: keep them for the next start.
    save_pipeline_cache();
    return index;
#else
    (void)format;
    (void)external_pass;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::ensure_pipelines");
#endif
}

lx::result<void*> lx::gfx::ui_vk_renderer::ensure_framebuffer(const ui_vk_attachment& target,
                                                              void* render_pass) {
#if defined(LUMEN_HAS_VULKAN)
    for (const framebuffer_entry& f : framebuffers_)
        if (f.framebuffer && f.serial == target.serial && f.view == target.view &&
            f.render_pass == render_pass && f.width == target.width && f.height == target.height)
            return f.framebuffer;
    auto dev = static_cast<VkDevice>(ctx_.device);
    framebuffer_entry& slot = framebuffers_[next_framebuffer_];
    next_framebuffer_ = (next_framebuffer_ + 1u) % k_ui_vk_framebuffers;
    if (slot.framebuffer) {
        // Evicted: the frames in flight may still use it.
        wait_slots();
        vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(slot.framebuffer), nullptr);
        slot = {};
    }
    auto view = static_cast<VkImageView>(target.view);
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = static_cast<VkRenderPass>(render_pass);
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = target.width;
    fbci.height = target.height;
    fbci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS)
        return ui_vk_detail::vk_fail("ui_vk: vkCreateFramebuffer failed");
    slot = {target.serial, target.view, render_pass, fb, target.width, target.height};
    return static_cast<void*>(fb);
#else
    (void)target;
    (void)render_pass;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::ensure_framebuffer");
#endif
}

// ── ui_vk_renderer: atlas pages ─────────────────────────────────────────────────

void lx::gfx::ui_vk_renderer::destroy_page(page_image& p) {
#if defined(LUMEN_HAS_VULKAN)
    if (auto dev = static_cast<VkDevice>(ctx_.device); dev) {
        if (p.view)
            vkDestroyImageView(dev, static_cast<VkImageView>(p.view), nullptr);
        if (p.image)
            vkDestroyImage(dev, static_cast<VkImage>(p.image), nullptr);
        if (p.memory)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(p.memory), nullptr);
    }
#endif
    p = {};
}

lx::result<void> lx::gfx::ui_vk_renderer::create_page(unsigned page, unsigned size, bool r8) {
#if defined(LUMEN_HAS_VULKAN)
    page_image& p = pages_[page];
    if (p.image) {
        // A page that grew: the frames in flight may still sample the old image.
        wait_slots();
        destroy_page(p);
    }
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    if (!ui_vk_detail::make_image(static_cast<VkDevice>(ctx_.device),
                                  static_cast<VkPhysicalDevice>(ctx_.physical_device),
                                  r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, size, size,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  image, memory, view))
        return ui_vk_detail::vk_oom("ui_vk: atlas page image creation failed");
    p = {image, memory, view, size};
    // Every set points at the new view before it next draws.
    ++page_generation_;
    return {};
#else
    (void)page;
    (void)size;
    (void)r8;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::create_page");
#endif
}

void lx::gfx::ui_vk_renderer::write_page_descriptors(unsigned slot) {
#if defined(LUMEN_HAS_VULKAN)
    VkDescriptorImageInfo images[k_ui_vk_page_slots]{};
    for (unsigned i = 0; i < k_ui_vk_page_slots; ++i) {
        void* view = i < k_atlas_pages && pages_[i].view ? pages_[i].view : dummy_view_;
        images[i].sampler = static_cast<VkSampler>(sampler_);
        images[i].imageView = static_cast<VkImageView>(view);
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = static_cast<VkDescriptorSet>(sets_[slot]);
    write.dstBinding = 2;
    write.descriptorCount = k_ui_vk_page_slots;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = images;
    vkUpdateDescriptorSets(static_cast<VkDevice>(ctx_.device), 1, &write, 0, nullptr);
    set_generation_[slot] = page_generation_;
#else
    (void)slot;
#endif
}

void lx::gfx::ui_vk_renderer::free_transient(unsigned slot) {
#if defined(LUMEN_HAS_VULKAN)
    transient_buffer& t = transient_[slot];
    if (auto dev = static_cast<VkDevice>(ctx_.device); dev) {
        if (t.memory) {
            vkUnmapMemory(dev, static_cast<VkDeviceMemory>(t.memory));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(t.memory), nullptr);
        }
        if (t.buffer)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(t.buffer), nullptr);
    }
#endif
    transient_[slot] = {};
}

lx::result<void> lx::gfx::ui_vk_renderer::upload_atlas(atlas& a, void* command_buffer,
                                                       unsigned slot, ui_vk_draw_stats& stats) {
#if defined(LUMEN_HAS_VULKAN)
    using namespace ui_vk_detail;
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    // An atlas this renderer has not drawn from (in a submitted frame): every page it has
    // goes up whole. `atlas_seen_`, like the dirty lists, is committed at submit.
    const bool fresh_atlas = atlas_seen_ != &a;
    for (bool& u : page_uploaded_)
        u = false;

    // Plan: which pages go up, whole or by rects, and how many bytes that is.
    bool whole[k_atlas_pages]{};
    bool upload[k_atlas_pages]{};
    std::size_t need = 0;
    for (unsigned i = 0; i < k_atlas_pages; ++i) {
        const atlas_page_view view = a.page(i);
        if (!view.valid())
            continue;
        const unsigned bpt = view.format == atlas_format::r8 ? 1u : 4u;
        if (pages_[i].size != view.size) {
            if (auto made = create_page(i, view.size, view.format == atlas_format::r8); !made)
                return made.get_error();
            page_whole_pending_[i] = true;
        }
        const std::span<const lx::rect2i> dirty = a.dirty_rects(i);
        whole[i] = page_whole_pending_[i] || fresh_atlas || a.dirty_full(i) ||
                   dirty.size() > k_ui_vk_upload_regions;
        upload[i] = whole[i] || !dirty.empty();
        if (whole[i]) {
            need += static_cast<std::size_t>(view.stride) * view.size;
        } else {
            for (const lx::rect2i& r : dirty)
                if (r.width > 0 && r.height > 0)
                    need += ((static_cast<std::size_t>(r.width) * bpt * static_cast<std::size_t>(r.height)) + 3u) & ~std::size_t{3};
        }
    }
    if (need == 0)
        return {};

    // The slot's staging range, or a buffer for this frame when the upload outgrows it.
    VkBuffer source = static_cast<VkBuffer>(staging_buffer_);
    std::byte* mapped = staging_mapped_ + static_cast<std::size_t>(slot) * k_ui_vk_staging_bytes;
    VkDeviceSize base = static_cast<VkDeviceSize>(slot) * k_ui_vk_staging_bytes;
    if (need > k_ui_vk_staging_bytes) {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* m = nullptr;
        if (!make_buffer(static_cast<VkDevice>(ctx_.device),
                         static_cast<VkPhysicalDevice>(ctx_.physical_device), need,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         buffer, memory, &m))
            return vk_oom("ui_vk: atlas staging allocation failed");
        transient_[slot] = {buffer, memory};
        source = buffer;
        mapped = static_cast<std::byte*>(m);
        base = 0;
        stats.atlas_transient = true;
    }

    auto* regions = reinterpret_cast<VkBufferImageCopy*>(regions_);
    static_assert(sizeof(VkBufferImageCopy) * k_ui_vk_upload_regions <= sizeof regions_);
    std::size_t cursor = 0;
    for (unsigned i = 0; i < k_atlas_pages; ++i) {
        if (!upload[i])
            continue;
        const atlas_page_view view = a.page(i);
        const unsigned bpt = view.format == atlas_format::r8 ? 1u : 4u;
        std::uint32_t count = 0;
        if (whole[i]) {
            const std::size_t bytes = static_cast<std::size_t>(view.stride) * view.size;
            std::memcpy(mapped + cursor, view.pixels, bytes);
            VkBufferImageCopy& c = regions[count++];
            c = {};
            c.bufferOffset = base + cursor;
            c.bufferRowLength = view.stride / bpt;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent = {view.size, view.size, 1};
            cursor += bytes;
            stats.atlas_upload_bytes += static_cast<std::uint64_t>(view.size) * view.size * bpt;
        } else {
            for (const lx::rect2i& r : a.dirty_rects(i)) {
                if (r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0 ||
                    static_cast<unsigned>(r.x + r.width) > view.size ||
                    static_cast<unsigned>(r.y + r.height) > view.size)
                    continue;
                // The rect's rows, packed.
                const std::size_t row = static_cast<std::size_t>(r.width) * bpt;
                for (int y = 0; y < r.height; ++y)
                    std::memcpy(mapped + cursor + static_cast<std::size_t>(y) * row,
                                view.pixels + static_cast<std::size_t>(r.y + y) * view.stride +
                                    static_cast<std::size_t>(r.x) * bpt,
                                row);
                VkBufferImageCopy& c = regions[count++];
                c = {};
                c.bufferOffset = base + cursor;
                c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                c.imageSubresource.layerCount = 1;
                c.imageOffset = {r.x, r.y, 0};
                c.imageExtent = {static_cast<std::uint32_t>(r.width),
                                 static_cast<std::uint32_t>(r.height), 1};
                cursor += (row * static_cast<std::size_t>(r.height) + 3u) & ~std::size_t{3};
                stats.atlas_upload_bytes += static_cast<std::uint64_t>(row) * r.height;
            }
        }
        if (count > 0) {
            auto image = static_cast<VkImage>(pages_[i].image);
            // A whole upload replaces every texel: the old contents need not survive. Earlier
            // frames may still be sampling the page (write after read): the barrier waits
            // for their fragment stage.
            image_barrier(cb, image,
                          whole[i] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyBufferToImage(cb, source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count,
                                   regions);
            image_barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ++stats.atlas_copies;
            stats.atlas_uploads += count;
        }
        page_uploaded_[i] = true; // dirty list cleared once the frame is submitted
    }
    return {};
#else
    (void)a;
    (void)command_buffer;
    (void)slot;
    (void)stats;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::upload_atlas");
#endif
}

// ── ui_vk_renderer: the frame ───────────────────────────────────────────────────

lx::result<lx::gfx::ui_vk_draw_stats> lx::gfx::ui_vk_renderer::draw(
    const ui_vk_attachment& target, const ui_stream& stream, atlas* atlas,
    const damage_region& damage, std::optional<lx::color> clear) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    using namespace ui_vk_detail;
    lx::runtime::assert_affinity(owner_);
    if (!ready_)
        return lx::not_implemented("lx::gfx::ui_vk_renderer::draw");
    if (!target.image || !target.view || target.width == 0 || target.height == 0 ||
        target.serial == 0)
        return lx::invalid_argument("ui_vk_renderer::draw: invalid attachment");

    ui_vk_draw_stats stats{};
    const lx::rect2i full{0, 0, static_cast<int>(target.width), static_cast<int>(target.height)};
    lx::rect2i rects[k_damage_rects]{};
    unsigned rect_count = 0;
    if (damage.full) {
        rects[rect_count++] = full;
    } else {
        for (unsigned i = 0; i < damage.count && i < k_damage_rects; ++i) {
            const lx::rect2i r = ui_encode::intersect(damage.rects[i], full);
            if (r.width > 0 && r.height > 0)
                rects[rect_count++] = r;
        }
    }
    if (rect_count == 0)
        return stats;

    auto pipelines = ensure_pipelines(target.format);
    if (!pipelines)
        return pipelines.get_error();
    const pipeline_set& set = pipeline_sets_[*pipelines];
    auto framebuffer = ensure_framebuffer(target, set.render_pass);
    if (!framebuffer)
        return framebuffer.get_error();

    auto dev = static_cast<VkDevice>(ctx_.device);
    // The slot: wait (rarely) for the frame three back that used it.
    const unsigned slot = frame_ % k_ui_vk_ring_slots;
    stats.slot = slot;
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (submitted_[slot]) {
        const std::uint64_t w0 = now_ns();
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, k_fence_timeout_ns) != VK_SUCCESS)
            return vk_fail("ui_vk: ring slot fence never signalled");
        stats.slot_wait_ns = now_ns() - w0;
        submitted_[slot] = false;
        if (timed_slot_ == static_cast<int>(slot))
            timed_slot_ = -1; // its timestamps are about to be reset
    }
    free_transient(slot);

    // Encode straight into the slot; the globals after the ring.
    std::byte* slot_bytes = ring_mapped_ + static_cast<std::size_t>(slot) * k_ui_vk_slot_bytes;
    ui_encode::encode_counts counts{};
    stats.instance_bytes = ui_encode::encode(stream, atlas, slot_bytes, k_ui_vk_slot_bytes,
                                             batch_word_, batch_count_, counts);
    stats.primitives_encoded = counts.encoded;
    stats.primitives_skipped = counts.skipped;
    stats.overflow_drops = counts.overflow_drops;
    const float globals[4] = {static_cast<float>(target.width), static_cast<float>(target.height),
                              0.f, 0.f};
    std::memcpy(ring_mapped_ + k_globals_offset + slot * k_globals_stride, globals, sizeof globals);

    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS)
        return vk_fail("ui_vk: vkBeginCommandBuffer failed");
    const bool timed = timing_ && query_pool_;
    auto qp = static_cast<VkQueryPool>(query_pool_);
    if (timed) {
        vkCmdResetQueryPool(cb, qp, 2 * slot, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2 * slot);
    }

    // Before the pass: atlas copies, the compositor's acquires for the surfaces.
    if (atlas) {
        if (auto up = upload_atlas(*atlas, cb, slot, stats); !up) {
            (void)vkEndCommandBuffer(cb);
            return up.get_error();
        }
    }
    const std::size_t batch_total =
        stream.batches.size() < k_max_batches ? stream.batches.size() : k_max_batches;
    bool any_surface = false;
    for (std::size_t b = 0; b < batch_total && !any_surface; ++b)
        any_surface = stream.batches[b].kind == primitive_kind::surface;
    bool prepared_blits = false;
    if (any_surface && compositor_) {
        if (auto prepared = compositor_->prepare_blits(cb, stream.surfaces.data(),
                                                       static_cast<unsigned>(stream.surfaces.size()));
            !prepared) {
            (void)vkEndCommandBuffer(cb);
            return prepared.get_error();
        }
        prepared_blits = true;
    }
    if (set_generation_[slot] != page_generation_)
        write_page_descriptors(slot);

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = static_cast<VkRenderPass>(set.render_pass);
    rpbi.framebuffer = static_cast<VkFramebuffer>(*framebuffer);
    rpbi.renderArea.extent = {target.width, target.height};
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = static_cast<float>(target.width);
    viewport.height = static_cast<float>(target.height);
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cb, 0, 1, &viewport);

    auto layout = static_cast<VkPipelineLayout>(pipeline_layout_);
    auto descriptor_set = static_cast<VkDescriptorSet>(sets_[slot]);
    int bound_pipeline = -1;
    bool set_bound = false;
    lx::result<void> failed{};
    for (unsigned n = 0; n < rect_count && failed; ++n) {
        const lx::rect2i rect = rects[n];
        ++stats.rects;
        VkRect2D scissor{};
        scissor.offset = {rect.x, rect.y};
        scissor.extent = {static_cast<std::uint32_t>(rect.width),
                          static_cast<std::uint32_t>(rect.height)};
        vkCmdSetScissor(cb, 0, 1, &scissor);
        if (clear) {
            VkClearAttachment ca{};
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = 0;
            ca.clearValue.color = {{clear->r, clear->g, clear->b, clear->a}};
            VkClearRect cr{};
            cr.rect = scissor;
            cr.layerCount = 1;
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
        for (std::size_t b = 0; b < batch_total; ++b) {
            const batch& bt = stream.batches[b];
            if (!ui_encode::meets(bt.bounds, rect)) {
                ++stats.batches_skipped;
                continue;
            }
            ++stats.batches_drawn;
            if (bt.kind == primitive_kind::surface) {
                const std::size_t end = static_cast<std::size_t>(bt.first) + bt.count;
                for (std::size_t i = bt.first; i < end && i < stream.refs.size(); ++i) {
                    const std::uint32_t index = stream.refs[i].index;
                    if (!compositor_ || index >= stream.surfaces.size()) {
                        ++stats.primitives_skipped;
                        continue;
                    }
                    // The compositor's pipeline and set: ours are bound again after.
                    auto drawn = compositor_->record_blits(
                        cb, set.render_pass, target.format, pipeline_cache_, target.width,
                        target.height, &stream.surfaces[index], 1, rect, true);
                    bound_pipeline = -1;
                    set_bound = false;
                    vkCmdSetViewport(cb, 0, 1, &viewport);
                    vkCmdSetScissor(cb, 0, 1, &scissor);
                    if (!drawn) {
                        failed = drawn.get_error();
                        break;
                    }
                    stats.draw_calls += drawn->draws_submitted;
                    stats.primitives_skipped += drawn->draws_skipped;
                }
                if (!failed)
                    break;
                continue;
            }
            const std::uint32_t count = batch_count_[b];
            if (count == 0)
                continue;
            unsigned index = p_quad;
            switch (bt.kind) {
            case primitive_kind::shadow: index = p_shadow; break;
            case primitive_kind::quad: index = p_quad; break;
            case primitive_kind::mono_sprite:
            case primitive_kind::path: index = p_mono; break;
            case primitive_kind::poly_sprite: index = p_poly; break;
            case primitive_kind::underline: index = p_underline; break;
            case primitive_kind::surface: break;
            }
            if (bound_pipeline != static_cast<int>(index)) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  static_cast<VkPipeline>(set.pipelines[index]));
                bound_pipeline = static_cast<int>(index);
            }
            if (!set_bound) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                        &descriptor_set, 0, nullptr);
                set_bound = true;
            }
            const push_block push{batch_word_[b], bt.page, 0u, 1.f};
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof push, &push);
            vkCmdDraw(cb, 6, count, 0, 0);
            ++stats.draw_calls;
        }
    }
    vkCmdEndRenderPass(cb);
    if (timed)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * slot + 1);
    // Never submitted: the compositor's acquires recorded above did not happen, so the next
    // frame records them again (`abandon_prepared_blits`); the atlas stays dirty.
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        if (prepared_blits && compositor_)
            compositor_->abandon_prepared_blits();
        return vk_fail("ui_vk: vkEndCommandBuffer failed");
    }
    // Submitted even when a surface draw failed to record (`failed`): the atlas copies and
    // the compositor's acquires recorded before the pass are already counted as done on the
    // CPU side, so they must run. The error is returned after the submit.
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(static_cast<VkQueue>(ctx_.queue), 1, &submit, fence) != VK_SUCCESS) {
        if (prepared_blits && compositor_)
            compositor_->abandon_prepared_blits();
        // The fence was reset and nothing will signal it: the slot is not in flight.
        return vk_fail("ui_vk: vkQueueSubmit failed");
    }
    submitted_[slot] = true;
    ++frame_;
    if (atlas) {
        for (unsigned i = 0; i < k_atlas_pages; ++i)
            if (page_uploaded_[i]) {
                atlas->clear_dirty(i);
                page_whole_pending_[i] = false;
                page_uploaded_[i] = false;
            }
        atlas_seen_ = atlas;
    }
    if (timing_) {
        timed_slot_ = static_cast<int>(slot);
        timed_submit_ns_ = now_ns();
    }
    if (!failed)
        return failed.get_error();
    return stats;
#else
    (void)target;
    (void)stream;
    (void)atlas;
    (void)damage;
    (void)clear;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::draw");
#endif
}

lx::result<lx::gfx::ui_vk_composite_stats> lx::gfx::ui_vk_renderer::composite(
    vulkan_compositor& compositor, render_target& target, lx::color clear, const ui_stream& stream,
    atlas* atlas, const ui_vk_composite_options& options) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    using namespace ui_vk_detail;
    lx::runtime::assert_affinity(owner_);
    if (!ready_ || !compositor.ready())
        return lx::not_implemented("lx::gfx::ui_vk_renderer::composite");
    if (!target.valid())
        return lx::invalid_argument("ui_vk_renderer::composite: invalid target");
    ui_vk_composite_stats out{};
    ui_vk_draw_stats& stats = out.ui;
    auto dev = static_cast<VkDevice>(ctx_.device);

    // The ring slot, as `draw` takes it: fenced by the empty submit after the compositor's.
    const unsigned slot = frame_ % k_ui_vk_ring_slots;
    stats.slot = slot;
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (submitted_[slot]) {
        const std::uint64_t w0 = now_ns();
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, k_fence_timeout_ns) != VK_SUCCESS)
            return vk_fail("ui_vk: ring slot fence never signalled");
        stats.slot_wait_ns = now_ns() - w0;
        submitted_[slot] = false;
        if (timed_slot_ == static_cast<int>(slot))
            timed_slot_ = -1;
    }
    free_transient(slot);
    std::byte* slot_bytes = ring_mapped_ + static_cast<std::size_t>(slot) * k_ui_vk_slot_bytes;
    ui_encode::encode_counts counts{};
    stats.instance_bytes = ui_encode::encode(stream, atlas, slot_bytes, k_ui_vk_slot_bytes,
                                             batch_word_, batch_count_, counts);
    stats.primitives_encoded = counts.encoded;
    stats.primitives_skipped = counts.skipped;
    stats.overflow_drops = counts.overflow_drops;
    const float globals[4] = {static_cast<float>(target.width()),
                              static_cast<float>(target.height()), 0.f, 0.f};
    std::memcpy(ring_mapped_ + k_globals_offset + slot * k_globals_stride, globals, sizeof globals);

    auto frame = compositor.begin_linear_frame(target, stream.surfaces.data(),
                                               static_cast<unsigned>(stream.surfaces.size()));
    if (!frame)
        return frame.get_error();
    auto cb = static_cast<VkCommandBuffer>(frame->command_buffer);
    // Before the pass: the atlas copies; then this slot's page descriptors and the pipelines
    // made for the compositor's pass.
    if (atlas) {
        if (auto up = upload_atlas(*atlas, cb, slot, stats); !up) {
            compositor.abandon_linear_frame();
            return up.get_error();
        }
    }
    if (set_generation_[slot] != page_generation_)
        write_page_descriptors(slot);
    auto pipelines = ensure_pipelines(frame->vk_format, frame->render_pass);
    if (!pipelines) {
        compositor.abandon_linear_frame();
        return pipelines.get_error();
    }
    const pipeline_set& set = pipeline_sets_[*pipelines];

    compositor.begin_linear_pass(clear);
    const lx::rect2i full{0, 0, static_cast<int>(frame->width), static_cast<int>(frame->height)};
    VkViewport viewport{};
    viewport.width = static_cast<float>(frame->width);
    viewport.height = static_cast<float>(frame->height);
    viewport.maxDepth = 1.f;
    VkRect2D scissor{};
    scissor.extent = {frame->width, frame->height};
    auto layout = static_cast<VkPipelineLayout>(pipeline_layout_);
    auto descriptor_set = static_cast<VkDescriptorSet>(sets_[slot]);
    int bound_pipeline = -1;
    bool set_bound = false;
    stats.rects = 1;
    const std::size_t batch_total =
        stream.batches.size() < k_max_batches ? stream.batches.size() : k_max_batches;
    for (std::size_t b = 0; b < batch_total; ++b) {
        const batch& bt = stream.batches[b];
        if (!ui_encode::meets(bt.bounds, full)) {
            ++stats.batches_skipped;
            continue;
        }
        ++stats.batches_drawn;
        if (bt.kind == primitive_kind::surface) {
            const std::size_t end = static_cast<std::size_t>(bt.first) + bt.count;
            for (std::size_t i = bt.first; i < end && i < stream.refs.size(); ++i) {
                const std::uint32_t index = stream.refs[i].index;
                if (index >= stream.surfaces.size()) {
                    ++stats.primitives_skipped;
                    continue;
                }
                // Decoded into linear light (`encoded` false), as `composite` draws them.
                auto drawn = compositor.record_blits(cb, frame->render_pass, frame->vk_format,
                                                     pipeline_cache_, frame->width, frame->height,
                                                     &stream.surfaces[index], 1, full, false);
                bound_pipeline = -1;
                set_bound = false;
                vkCmdSetViewport(cb, 0, 1, &viewport);
                vkCmdSetScissor(cb, 0, 1, &scissor);
                if (!drawn) {
                    compositor.abandon_linear_frame();
                    return drawn.get_error();
                }
                stats.draw_calls += drawn->draws_submitted;
                out.composite.draws_submitted += drawn->draws_submitted;
                out.composite.draws_skipped += drawn->draws_skipped;
                out.composite.draws_culled += drawn->draws_culled;
            }
            continue;
        }
        const std::uint32_t count = batch_count_[b];
        if (count == 0)
            continue;
        unsigned index = p_quad;
        switch (bt.kind) {
        case primitive_kind::shadow: index = p_shadow; break;
        case primitive_kind::quad: index = p_quad; break;
        case primitive_kind::mono_sprite:
        case primitive_kind::path: index = p_mono; break;
        case primitive_kind::poly_sprite: index = p_poly; break;
        case primitive_kind::underline: index = p_underline; break;
        case primitive_kind::surface: break;
        }
        if (bound_pipeline != static_cast<int>(index)) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              static_cast<VkPipeline>(set.pipelines[index]));
            bound_pipeline = static_cast<int>(index);
        }
        if (!set_bound) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                    &descriptor_set, 0, nullptr);
            set_bound = true;
        }
        const push_block push{batch_word_[b], bt.page, 1u, options.text_contrast};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof push, &push);
        vkCmdDraw(cb, 6, count, 0, 0);
        ++stats.draw_calls;
    }

    auto ended = compositor.end_linear_frame();
    if (!ended)
        return ended.get_error(); // abandoned there: nothing submitted, the atlas stays dirty
    out.composite.out_fence_fd = ended->out_fence_fd;
    // This slot's instances, staging range and page descriptors are free once the compositor's
    // frame is: an empty submit signals the slot's fence after it.
    vkResetFences(dev, 1, &fence);
    if (vkQueueSubmit(static_cast<VkQueue>(ctx_.queue), 0, nullptr, fence) == VK_SUCCESS) {
        submitted_[slot] = true;
    } else {
        // No fence for the slot: wait for the queue instead (an error path, never steady).
        (void)vkQueueWaitIdle(static_cast<VkQueue>(ctx_.queue));
    }
    ++frame_;
    if (atlas) {
        for (unsigned i = 0; i < k_atlas_pages; ++i)
            if (page_uploaded_[i]) {
                atlas->clear_dirty(i);
                page_whole_pending_[i] = false;
                page_uploaded_[i] = false;
            }
        atlas_seen_ = atlas;
    }
    return out;
#else
    (void)compositor;
    (void)target;
    (void)clear;
    (void)stream;
    (void)atlas;
    (void)options;
    return lx::not_implemented("lx::gfx::ui_vk_renderer::composite");
#endif
}

lx::result<lx::gfx::ui_vk_gpu_time> lx::gfx::ui_vk_renderer::wait_gpu_time() {
#if defined(LUMEN_HAS_VULKAN)
    using namespace ui_vk_detail;
    lx::runtime::assert_affinity(owner_);
    ui_vk_gpu_time out{};
    if (!ready_ || timed_slot_ < 0)
        return out;
    const auto slot = static_cast<unsigned>(timed_slot_);
    timed_slot_ = -1;
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, k_fence_timeout_ns) != VK_SUCCESS)
        return vk_fail("ui_vk: frame fence never signalled");
    if (query_pool_) {
        std::uint64_t ticks[2]{};
        if (vkGetQueryPoolResults(dev, static_cast<VkQueryPool>(query_pool_), 2 * slot, 2,
                                  sizeof ticks, ticks, sizeof ticks[0],
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return vk_fail("ui_vk: timestamp query failed");
        const std::uint64_t mask =
            timestamp_bits_ >= 64 ? ~0ull : ((1ull << timestamp_bits_) - 1ull);
        const std::uint64_t delta = (ticks[1] - ticks[0]) & mask;
        out.source = ui_vk_timing::timestamp;
        out.ns = static_cast<std::uint64_t>(static_cast<double>(delta) * caps_.timestamp_period_ns);
    } else {
        out.source = ui_vk_timing::fence_wall;
        out.ns = now_ns() - timed_submit_ns_;
    }
    return out;
#else
    return lx::not_implemented("lx::gfx::ui_vk_renderer::wait_gpu_time");
#endif
}
