module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <stdint.h>

import lx.foundation;

export module lx.gfx:vk_renderer;

import std;

import :damage;
import :dmabuf;
import :renderer;

export namespace lx::gfx {

/// Opaque Vulkan handles handed from `lx::gfx::device` to the renderer. Kept as `void*`
/// so this partition's interface stays free of Vulkan types, and so the partition does
/// not have to import the primary module interface that owns `device`.
struct vk_context {
    void* device = nullptr;
    void* physical_device = nullptr;
    void* queue = nullptr;
    void* command_pool = nullptr;
    unsigned queue_family = 0;
    bool supports_queue_family_foreign = false;
    /// VK_KHR_external_semaphore_fd. Lets a frame's completion be exported as a sync_file
    /// the atomic commit waits on, instead of the render thread blocking on the GPU.
    bool supports_external_semaphore_fd = false;

    [[nodiscard]] bool valid() const {
        return device && physical_device && queue && command_pool;
    }
};

/// GPU composite target whose memory is exportable as a dmabuf, so the same image the
/// compositor renders into can be handed to KMS as a scanout framebuffer.
class render_target {
public:
    render_target() = default;
    ~render_target();

    render_target(const render_target&) = delete;
    render_target& operator=(const render_target&) = delete;
    render_target(render_target&& other) noexcept;
    render_target& operator=(render_target&& other) noexcept;

    /// `format` is the scanout encoding. `xrgb8888`/`argb8888` are the 8-bit SDR path;
    /// `xrgb2101010`/`argb2101010` give ten bits per channel, which is what a wide-gamut or
    /// HDR output needs — the composite always blends in 16-bit float linear regardless,
    /// and the encode pass converts into whichever of these the display wants.
    [[nodiscard]] static lx::result<render_target> create(
        const vk_context& ctx, unsigned width, unsigned height,
        lx::pixel_format format = lx::pixel_format::xrgb8888);

    [[nodiscard]] lx::pixel_format format() const { return format_; }

    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    [[nodiscard]] bool valid() const { return image_ != nullptr; }

    /// Fresh dmabuf FD for `drmModeAddFB2WithModifiers`. The caller owns the FD.
    [[nodiscard]] lx::result<exported_dmabuf> export_dmabuf() const;

    [[nodiscard]] void* vk_image() const { return image_; }
    [[nodiscard]] void* vk_image_view() const { return view_; }
    [[nodiscard]] void* vk_memory() const { return memory_; }

private:
    void destroy();

    vk_context ctx_{};
    void* image_ = nullptr;
    void* memory_ = nullptr;
    void* view_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
    lx::pixel_format format_ = lx::pixel_format::xrgb8888;
};

struct composite_stats {
    unsigned draws_submitted = 0;
    /// Draws whose texture id was never registered. Non-zero means the commit path and
    /// the render path disagree — it must not be papered over with a placeholder.
    unsigned draws_skipped = 0;
    /// Clipped away entirely, so never submitted. Expected, not an error.
    unsigned draws_culled = 0;
    /// sync_file signalled when this frame's GPU work completes, or -1 when the device
    /// cannot export one — in which case `composite` already blocked instead.
    ///
    /// **Owned by the caller**, which must either hand it to an atomic commit as
    /// `in_fence_fd` or close it. Leaking it leaks a file descriptor per frame. Mirrors
    /// `gl_composite_stats::out_fence_fd` so the present path treats both backends alike.
    int out_fence_fd = -1;
};

/// Records and submits the compositor's textured-quad pass.
class vulkan_compositor {
public:
    vulkan_compositor() = default;
    ~vulkan_compositor();

    vulkan_compositor(const vulkan_compositor&) = delete;
    vulkan_compositor& operator=(const vulkan_compositor&) = delete;

    [[nodiscard]] lx::result<void> initialize(const vk_context& ctx);
    void shutdown();
    [[nodiscard]] bool ready() const { return pipeline_ != nullptr; }

    /// Make a dmabuf-imported client image samplable under `image.image_id`.
    [[nodiscard]] lx::result<void> bind_imported(const imported_image& image);

    /// Upload shared-memory pixels (tightly packed RGBA8) under `texture_id`.
    [[nodiscard]] lx::result<void> upload_rgba(unsigned texture_id, unsigned width,
                                               unsigned height, const unsigned char* rgba);

    void forget_texture(unsigned texture_id);

    /// The whole of `target`: every pixel composited and encoded. Starts no chain: the
    /// intermediate it leaves is not continued from (`composite_region`).
    [[nodiscard]] lx::result<composite_stats> composite(render_target& target, lx::color clear,
                                                         const blit_command* cmds,
                                                         unsigned count);
    /// `gl_compositor::composite_region` for Vulkan: only `region` of `target` is written —
    /// this frame's damage plus what `target` missed since it was last drawn (its buffer
    /// age). The linear intermediate is persistent and holds the newest complete frame of a
    /// chain: when `previous` is the target the chain's last frame went into, the composite
    /// pass redraws only `changed` (what changed since that frame) into it, each rect
    /// cleared and then drawn through, scissored to the rect, draws meeting no rect skipped.
    /// Otherwise (the first frame, `previous` null, a new or resized intermediate, another
    /// chain drew into it since, a failed frame) the composite pass covers the whole
    /// target. A target this compositor has not written yet is encoded whole. Pixels are
    /// those of a full repaint: each pixel sees the same clear and draws in the same order.
    [[nodiscard]] lx::result<composite_stats>
    composite_region(render_target& target, lx::color clear, const blit_command* cmds,
                     unsigned count, const damage_region& region, const damage_region& changed,
                     const render_target* previous);

    /// Transfer function the encode pass writes the scanout buffer in. sRGB for an ordinary
    /// display; PQ or HLG for an HDR one.
    void set_output_transfer(lx::transfer_function tf) { output_transfer_ = tf; }
    [[nodiscard]] lx::transfer_function output_transfer() const { return output_transfer_; }

    /// ADR-010 §4.4: `surface` primitives in a UI stream (`:ui_vk`) are this compositor's
    /// draws, recorded into the UI renderer's own render pass. `prepare_blits` records, on
    /// `command_buffer` and outside any render pass, what `composite` records before its own:
    /// the acquire of freshly imported client images; and it makes the 1×1 texture a solid
    /// draw binds when `cmds` has one (an upload submitted on its own, once). Call it before
    /// beginning the pass `record_blits` draws in.
    [[nodiscard]] lx::result<void> prepare_blits(void* command_buffer, const blit_command* cmds,
                                                 unsigned count);
    /// Records `cmds` into the render pass the caller has begun on `command_buffer` — one
    /// colour attachment of `vk_format` (a VkFormat), `width` × `height` — with the composite
    /// shaders in a pipeline made for a pass compatible with `render_pass` (one per format,
    /// cached; `pipeline_cache` is a VkPipelineCache or null), scissored to `scissor` and each
    /// draw's clip. `encoded`: the attachment holds encoded values (a UI target, ADR-010
    /// §4.5), so texels are not decoded to linear light — tint and opacity scale the encoded
    /// values, as `gl_compositor` and `cpu_compositor::blend` do. Leaves its pipeline and
    /// descriptor set bound: the caller binds its own again.
    ///
    /// Lifetime: on a command buffer that is not this compositor's own frame (a UI renderer's),
    /// the textures' images, views and descriptor sets outlive what this compositor's fences
    /// cover, so each texture recorded there is marked as externally used, and
    /// `forget_texture`, `upload_rgba` over it and `bind_imported` of another image under its
    /// id first call every registered external user's `wait` (`add_external_user`). On this
    /// compositor's own frame (`begin_linear_frame`) its fences cover them, as for
    /// `composite`. A pass of this compositor's own (`render_pass` its linear pass) draws with
    /// its own composite pipeline.
    [[nodiscard]] lx::result<composite_stats> record_blits(void* command_buffer, void* render_pass,
                                                           unsigned vk_format, void* pipeline_cache,
                                                           unsigned width, unsigned height,
                                                           const blit_command* cmds, unsigned count,
                                                           lx::rect2i scissor, bool encoded);

    // ── External users and the UI pass (ADR-010 §4.4, §4.11) ──────────────────────

    /// Someone that records this compositor's textures into command buffers of its own (a
    /// `ui_vk_renderer` drawing a stream's `surface` refs through `record_blits`). `wait(user)`
    /// returns once every frame the user submitted is complete — called before a texture such
    /// a frame recorded is destroyed, re-uploaded or re-pointed. `detach(user)`: this
    /// compositor is shutting down (the device is idle; drop the pointer to it).
    struct external_user {
        void* user = nullptr;
        void (*wait)(void* user) = nullptr;
        void (*detach)(void* user) = nullptr;
    };
    static constexpr unsigned k_external_users = 4;
    /// False when `k_external_users` are registered already (the user must then not record
    /// this compositor's textures) or `u` has no `wait`. Registering a user twice is a no-op.
    [[nodiscard]] bool add_external_user(external_user u);
    void remove_external_user(void* user);

    /// The acquires of freshly imported images that the last `prepare_blits` (or
    /// `begin_linear_frame`) recorded did not run: the command buffer holding them was never
    /// submitted (its vkEndCommandBuffer or vkQueueSubmit failed). Marks those images
    /// unacquired again, so the next frame records their acquire.
    void abandon_prepared_blits();

    /// A frame whose UI stream `:ui_vk` records inside this compositor's linear F16 pass
    /// (`ui_vk_renderer::composite`): what `composite` does, split where the UI renderer
    /// records its own work.
    struct linear_frame {
        void* command_buffer = nullptr;
        /// The linear pass `begin_linear_pass` begins: one R16G16B16A16_SFLOAT attachment,
        /// linear light. Pipelines made for it are compatible with every frame's.
        void* render_pass = nullptr;
        unsigned vk_format = 0;
        unsigned width = 0;
        unsigned height = 0;
        /// Where the pass draws: the whole target, else `rects[0..rect_count)` — each one
        /// begun with `begin_linear_rect`, then drawn through scissored to it.
        bool full = true;
        const lx::rect2i* rects = nullptr;
        unsigned rect_count = 0;
    };
    /// `composite`'s set-up for `target` — the linear intermediate, the encode pipeline, the
    /// framebuffer, the solid texture when `cmds` has a solid draw — then waits for the frame
    /// slot, begins its command buffer and records the acquires of fresh imports. The caller
    /// records its work outside the pass (atlas copies), then `begin_linear_pass`, its draws
    /// (`record_blits` with `render_pass` for surfaces), then `end_linear_frame`; on an
    /// error in between, `abandon_linear_frame`. `region`, `changed`, `previous`: as
    /// `composite_region` (null `region`: the whole target, as `composite`).
    [[nodiscard]] lx::result<linear_frame>
    begin_linear_frame(render_target& target, const blit_command* cmds, unsigned count,
                       const damage_region* region = nullptr,
                       const damage_region* changed = nullptr,
                       const render_target* previous = nullptr);
    /// Begins the linear pass, with the full-target viewport and scissor set. A whole-target
    /// pass is cleared to `clear` (encoded, decoded here as `composite` does); a damage pass
    /// clears each rect as `begin_linear_rect` begins it.
    void begin_linear_pass(lx::color clear);
    /// A damage pass (`linear_frame::full` false): scissors to `rect` and clears it to the
    /// pass's clear colour. Draws for the rect follow. Nothing for a whole-target pass.
    void begin_linear_rect(lx::rect2i rect);
    /// Ends the pass, records the encode pass into the target, ends and submits the command
    /// buffer as `composite` does (the out fence exported when the device can). On failure
    /// the frame is abandoned (its acquires un-marked) and nothing was submitted.
    [[nodiscard]] lx::result<composite_stats> end_linear_frame();
    /// Ends the command buffer unsubmitted and un-marks its acquires.
    void abandon_linear_frame();

    /// Linear intermediates allocated so far. One per target size in use, kept across frames:
    /// a test asserts that outputs of different sizes do not reallocate each other's.
    [[nodiscard]] unsigned linear_allocations() const { return linear_allocations_; }

    /// Copy the target back into tightly packed RGBA8 for golden-image verification.
    /// Synchronous (it waits for the GPU): tests and one-off reads only, never per frame.
    [[nodiscard]] lx::result<void> read_back(const render_target& target, unsigned char* rgba,
                                             unsigned capacity);

    /// Screen capture's read-back slots (one per capture session). Each keeps a persistent,
    /// mapped host-visible buffer of the target's size (B8G8R8A8 rows `width * 4` bytes
    /// apart: wl_shm's ARGB8888/XRGB8888 byte order), a command buffer and a fence — made on
    /// first use, kept while the size fits, so a screencast allocates nothing per frame.
    static constexpr unsigned k_readback_slots = 64;
    /// Render thread: records a copy of `rects` (target pixels; all of it when `count` is
    /// 0) into `slot`'s buffer, at their own place, and submits it without waiting. The
    /// slot's previous read must have been waited for (`read_back_wait`).
    [[nodiscard]] lx::result<void> read_back_async(unsigned slot, const render_target& target,
                                                   const lx::rect2i* rects, unsigned count);
    /// Any thread (the one the caller handed the slot to): waits up to `timeout_ns` for
    /// `slot`'s read to land; its rows, or null when it failed or timed out.
    [[nodiscard]] const unsigned char* read_back_wait(unsigned slot,
                                                      unsigned long long timeout_ns) const;
    /// Render thread: frees `slot` (its capture session ended). Waits for its read first.
    void read_back_free(unsigned slot);

private:
    static constexpr unsigned k_max_textures = 256;

    struct texture_slot {
        unsigned id = 0;
        void* image = nullptr;
        void* view = nullptr;
        void* memory = nullptr;
        void* descriptor_set = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        bool owns_image = false;
        bool layout_ready = false;
        bool used = false;
        /// Recorded by an external user since the last wait for them (`record_blits`).
        bool external_use = false;
    };

    /// Makes `linear_` the linear-light intermediate of this size the composite pass renders
    /// into: the one holding the frame encoded into `previous` (a VkImage) when there is one,
    /// else one kept for this size that no chain continues from, else — for a `chained`
    /// frame, while a slot is free — a new one (a second output of the same size), else
    /// the least recently used of this size, else a new one (`linear_targets_`).
    [[nodiscard]] lx::result<void> ensure_linear_target(unsigned width, unsigned height,
                                                        const void* previous, bool chained);
    /// After the ensure_* calls: where this frame's composite and encode passes draw
    /// (`pass_*`, `encode_*`, see `composite_region`). The intermediate holds no complete
    /// frame until the frame is submitted (`finish_frame`).
    void plan_frame(const render_target& target, const damage_region* region,
                    const damage_region* changed, const render_target* previous, bool chained);
    /// After a successful submit: the target is written, and a chained frame's
    /// intermediate holds it complete.
    void finish_frame(const render_target& target);
    /// Begins the composite pass on `command_buffer` per the plan: cleared whole, or loaded.
    void begin_composite_pass(void* command_buffer, unsigned width, unsigned height,
                              lx::color clear);
    /// Records the encode pass into the current framebuffer per the plan.
    void record_encode_pass(void* command_buffer, const render_target& target);
    /// Inside a loaded composite pass: clears `rect` to the pass's clear colour and
    /// scissors to it.
    void clear_pass_rect(void* command_buffer, lx::rect2i rect);
    /// `composite` (`chained` false: starts no chain) and `composite_region`.
    [[nodiscard]] lx::result<composite_stats>
    composite_frame(render_target& target, lx::color clear, const blit_command* cmds,
                    unsigned count, const damage_region& region, const damage_region& changed,
                    const render_target* previous, bool chained);
    /// Creates the encode pass and pipeline for a scanout format, reusing them while the
    /// format is unchanged. Recreated on a format change — switching an output between 8
    /// and 10 bits is rare, and caching one of each is not worth the bookkeeping.
    [[nodiscard]] lx::result<void> ensure_encode_pipeline(lx::pixel_format format);
    struct linear_target;
    void destroy_linear_target(linear_target& t);
    void destroy_encode_pipeline();

    [[nodiscard]] int find_texture(unsigned id) const;
    /// The pipeline layout's sampler set must be bound for every draw, a solid one too (it
    /// never samples it): a 1x1 texture under this reserved id provides one when the frame
    /// has bound no other set yet. Created on the first solid draw.
    static constexpr unsigned k_solid_texture_id = 0xFFFF'FFFEu;
    [[nodiscard]] int alloc_texture(unsigned id);
    void destroy_texture(texture_slot& slot);
    [[nodiscard]] lx::result<void> ensure_framebuffer(const render_target& target);
    [[nodiscard]] lx::result<void> create_descriptor(texture_slot& slot);
    [[nodiscard]] lx::result<void> ensure_staging_ring(unsigned need_bytes);
    /// Submits the recorded slot. When `out_fence_fd` is non-null and the device can
    /// export sync_files, the submit signals a semaphore exported into `*out_fence_fd` and
    /// this returns without waiting — the display controller holds the flip back instead of
    /// the render thread holding the frame back. Falls back to blocking when it cannot.
    [[nodiscard]] lx::result<void> submit_current_slot(bool wait_for_completion,
                                                       int* out_fence_fd = nullptr);
    /// Blocks until every submitted frame slot has completed. Required before destroying
    /// any resource a queued command buffer might reference.
    void wait_all_slots_idle();

    /// `record_blits`' pipelines, one per attachment format (a pipeline is compatible with
    /// every render pass compatible with the one it was made for: one colour attachment of
    /// the format). Fixed: a fifth format evicts the first.
    static constexpr unsigned k_pass_pipelines = 4;
    struct pass_pipeline {
        unsigned format = 0;
        void* pipeline = nullptr;
        /// The same without blending, for `blend_mode::opaque` draws (`opaque_pipeline_`).
        void* opaque_pipeline = nullptr;
    };
    pass_pipeline pass_pipelines_[k_pass_pipelines]{};
    /// Records the acquire of every imported image not yet acquired (outside a render pass).
    void record_acquires(void* command_buffer);
    /// Shared by `composite` and `prepare_blits`: the 1×1 texture solid draws bind.
    [[nodiscard]] lx::result<void> ensure_solid_texture(const blit_command* cmds, unsigned count);
    /// Before destroying or rewriting texture `slot`: when an external user recorded it, waits
    /// for every external user (and clears every slot's mark — they are all done).
    void wait_external(const texture_slot& slot);

    external_user external_[k_external_users]{};
    /// Texture slots `record_acquires` marked acquired in the command buffer being recorded.
    std::uint16_t acquired_[k_max_textures]{};
    unsigned acquired_count_ = 0;
    /// The command buffer of a `begin_linear_frame` not yet ended (null: none), its target
    /// and whether its pass is open.
    void* linear_cb_ = nullptr;
    render_target* linear_target_ = nullptr;
    bool linear_pass_open_ = false;

    static constexpr unsigned k_frame_slots = 2;
    static constexpr unsigned k_staging_ring_bytes = 16u * 1024u * 1024u;

    vk_context ctx_{};
    void* render_pass_ = nullptr;
    void* pipeline_ = nullptr;
    /// `pipeline_` with blending off, for `blend_mode::opaque` draws, as GL and CPU draw them:
    /// a source of alpha 1 replaces the destination either way (src·1 + dst·0 is exact), so
    /// the pixels are the same and the destination is not read.
    void* opaque_pipeline_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* descriptor_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* sampler_ = nullptr;
    void* command_buffers_[k_frame_slots]{};
    void* fences_[k_frame_slots]{};
    /// One binary semaphore per slot, signalled by that slot's submit and exported as a
    /// sync_file for KMS. Per slot rather than one shared: an exported semaphore must not
    /// be re-signalled until the export has been consumed.
    void* slot_semaphores_[k_frame_slots]{};
    bool can_export_fence_ = false;
    unsigned submit_slot_ = 0;
    void* staging_buffer_ = nullptr;
    void* staging_memory_ = nullptr;
    unsigned char* staging_mapped_ = nullptr;
    unsigned staging_capacity_ = 0;
    unsigned staging_offset_ = 0;

    /// One framebuffer per render target. A double-buffered scanout alternates targets
    /// every frame, so a single-entry cache would destroy and recreate it per frame.
    static constexpr unsigned k_max_framebuffers = 4;
    struct framebuffer_slot {
        void* framebuffer = nullptr;
        void* image = nullptr;
        /// A frame was submitted into it: what it holds outside a region is kept.
        bool written = false;
    };
    framebuffer_slot framebuffers_[k_max_framebuffers]{};
    void* framebuffer_ = nullptr;

    /// Linear-light intermediates. The composite pass blends into one; the encode pass reads
    /// it and writes the scanout buffer. One per target size, so outputs of different sizes
    /// sharing this compositor do not reallocate (and drain the GPU) every time they alternate;
    /// an output's targets share one, which keeps its newest frame (`content`), and a second
    /// output of the same size gets its own while a slot is free. Freed once unused for
    /// `k_linear_idle_frames` frames (an output gone or resized); a fifth size evicts the least
    /// recently used.
    static constexpr unsigned k_linear_targets = 4;
    static constexpr std::uint64_t k_linear_idle_frames = 600;
    struct linear_target {
        void* image = nullptr;
        void* memory = nullptr;
        void* view = nullptr;
        void* framebuffer = nullptr;
        /// The encode pass's set, pointed at `view`. Its own, allocated with the pool: a frame
        /// in flight for one size must not see it re-pointed at another's.
        void* encode_set = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        /// `linear_frame_` of the last frame drawn into it.
        std::uint64_t last_used = 0;
        /// The target (VkImage) the complete frame it holds was encoded into, by a chained
        /// frame; null: nothing continues from it. Unique among the intermediates.
        const void* content = nullptr;
    };
    linear_target linear_targets_[k_linear_targets]{};
    /// The current frame's (`ensure_linear_target`).
    linear_target* linear_ = nullptr;
    std::uint64_t linear_frame_ = 0;
    unsigned linear_allocations_ = 0;

    /// The frame being recorded (`plan_frame`): the composite pass covers the whole target
    /// or `pass_rects_`, the encode pass the whole target or `encode_rects_`. A chained
    /// frame leaves its intermediate continuable (`linear_target::content`).
    bool pass_full_ = true;
    lx::rect2i pass_rects_[k_damage_rects]{};
    unsigned pass_count_ = 0;
    bool encode_full_ = true;
    lx::rect2i encode_rects_[k_damage_rects]{};
    unsigned encode_count_ = 0;
    bool frame_chained_ = false;
    /// The composite pass's clear value (linear light) for `begin_linear_rect`.
    float pass_clear_[4]{};
    /// The current framebuffer's `framebuffers_` entry.
    unsigned framebuffer_index_ = 0;
    /// The composite pass that loads the intermediate (SHADER_READ_ONLY_OPTIMAL in and
    /// out), compatible with `render_pass_`: a damage frame's.
    void* load_render_pass_ = nullptr;

    void* encode_render_pass_ = nullptr;
    /// The encode pass that loads the target (GENERAL in and out), compatible with
    /// `encode_render_pass_`: a region's.
    void* encode_load_render_pass_ = nullptr;
    void* encode_pipeline_ = nullptr;
    void* encode_pipeline_layout_ = nullptr;
    void* encode_set_layout_ = nullptr;
    lx::pixel_format encode_format_ = lx::pixel_format::xrgb8888;
    bool encode_ready_ = false;

    lx::transfer_function output_transfer_ = lx::transfer_function::srgb;

    texture_slot textures_[k_max_textures]{};

    struct readback_slot {
        void* buffer = nullptr;
        void* memory = nullptr;
        unsigned char* mapped = nullptr;
        void* command_buffer = nullptr;
        void* fence = nullptr;
        unsigned capacity = 0;
        /// A copy was submitted with `fence` and not waited for by render yet.
        bool submitted = false;
    };
    readback_slot readbacks_[k_readback_slots]{};
};

} // namespace lx::gfx

#if defined(LUMEN_HAS_VULKAN)

namespace {

/// Mirrors the `quad_push` block in composite.vert/.frag. Field order matters: each vec4
/// sits at a 16-byte-aligned offset, which the layout below satisfies naturally (dst at 0,
/// src_uv at 32, tint at 48, src_bounds at 64, transfer at 80, uv_dy at 88, solid at 96)
/// for 112 bytes total — inside the 128 every driver guarantees for push constants.
struct quad_push {
    float dst[4]{};
    float target[2]{};
    float opacity = 1.f;
    /// 1: a solid draw — `solid` replaces the sample (`blit_command::solid`).
    float use_solid = 0.f;
    /// The source box as an affine basis in normalized texture coordinates: where the
    /// destination's top-left lands (u, v) and its +x direction (du, dv); `uv_dy` is the +y
    /// direction. Crop, scale and the buffer transform in one (`source_basis_for`).
    float src_uv[4]{0.f, 0.f, 1.f, 0.f};
    float tint[4]{1.f, 1.f, 1.f, 1.f};
    /// Half-texel-inset sampling bounds for `src_uv`: u_lo, v_lo, u_hi, v_hi. The fragment
    /// shader clamps to these so a magnified crop cannot bleed texels from outside the
    /// source rectangle — which is precisely what viewporter must not do.
    float src_bounds[4]{0.f, 0.f, 1.f, 1.f};
    /// Transfer function to decode the sampled texel with. Matches `transfer_slot`.
    int transfer = 1;
    float pad1 = 0.f;
    float uv_dy[2]{0.f, 1.f};
    /// Premultiplied, encoded colour of a solid draw.
    float solid[4]{0.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(quad_push) == 112, "quad_push must match composite.vert/.frag");

/// Mirrors the `encode_push` block in encode.frag.
struct encode_push {
    int transfer = 1;
    float pad0 = 0.f;
    float pad1 = 0.f;
    float pad2 = 0.f;
};

/// Shader-side selector for a transfer function. Must match the `decode` branch order in
/// composite.frag; `lx::transfer_function`'s own values are an independent enum.
[[nodiscard]] inline int transfer_slot(lx::transfer_function tf) {
    switch (tf) {
    case lx::transfer_function::linear: return 0;
    case lx::transfer_function::gamma22: return 2;
    case lx::transfer_function::pq: return 3;
    case lx::transfer_function::hlg: return 4;
    case lx::transfer_function::srgb:
    default: return 1;
    }
}

// sRGB, not UNORM. The shader writes linear light, and an sRGB attachment makes the
// fixed-function blender decode the destination to linear, blend, and re-encode on write —
// which is what makes alpha compositing correct. The DRM fourcc for scanout is unchanged:
// DRM formats carry no transfer function, so KMS still sees ARGB8888.
//
// This is the SDR half of the color pipeline. Wider storage (A2B10G10R10 or RGBA16F, with
// PQ output for HDR) needs a linear intermediate plus an explicit encode pass, because no
// 10-bit format has an sRGB variant for the hardware to encode into.
// The composite always renders here: 16-bit float, linear light. Blending is a weighted
// average of light, so the attachment it blends into has to hold linear values, and 16-bit
// float holds them without banding in the darks the way 8- or 10-bit linear would.
constexpr VkFormat k_linear_format = VK_FORMAT_R16G16B16A16_SFLOAT;

/// Scanout format for a `pixel_format`. UNORM, not sRGB: the encode pass applies the
/// transfer function explicitly, which is the only option at 10 bits and keeps the two
/// depths on one code path.
[[nodiscard]] inline VkFormat vk_scanout_format(lx::pixel_format f) {
    switch (f) {
    case lx::pixel_format::argb2101010:
    case lx::pixel_format::xrgb2101010:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case lx::pixel_format::rgba8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr VkFormat k_target_format = VK_FORMAT_B8G8R8A8_UNORM;

/// The scissors a draw is recorded with: `scissor` (non-empty), or for a draw the
/// visibility pass found partly covered (`blit_command::visible`) each visible rect ∩
/// `scissor` that is not empty. Written to `out`; returns their count (0: nothing to draw).
[[nodiscard]] unsigned visible_scissors(const lx::gfx::blit_command& cmd, lx::rect2i scissor,
                                        VkRect2D out[lx::gfx::k_max_visible_rects]) {
    const auto put = [](VkRect2D& r, int x0, int y0, int x1, int y1) {
        r.offset = {x0, y0};
        r.extent = {static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0)};
    };
    if (cmd.visible_count == 0) {
        put(out[0], scissor.x, scissor.y, scissor.x + scissor.width, scissor.y + scissor.height);
        return 1;
    }
    unsigned n = 0;
    for (unsigned k = 0; k < cmd.visible_count && k < lx::gfx::k_max_visible_rects; ++k) {
        const lx::rect2i& v = cmd.visible[k];
        const int x0 = std::max(scissor.x, v.x);
        const int y0 = std::max(scissor.y, v.y);
        const int x1 = std::min(scissor.x + scissor.width, v.x + v.width);
        const int y1 = std::min(scissor.y + scissor.height, v.y + v.height);
        if (x1 > x0 && y1 > y0)
            put(out[n++], x0, y0, x1, y1);
    }
    return n;
}

/// One blit's push constants: destination, the source box as a basis with its half-texel
/// sampling bounds (`tex_w` × `tex_h`: the sampled texture; 0 for a solid draw), tint,
/// opacity and the transfer slot to decode with.
[[nodiscard]] quad_push make_quad_push(const lx::gfx::blit_command& cmd, unsigned tex_w,
                                       unsigned tex_h, unsigned target_w, unsigned target_h,
                                       int transfer) {
    quad_push push{};
    push.dst[0] = static_cast<float>(cmd.dst.x);
    push.dst[1] = static_cast<float>(cmd.dst.y);
    push.dst[2] = static_cast<float>(cmd.dst.width);
    push.dst[3] = static_cast<float>(cmd.dst.height);
    push.target[0] = static_cast<float>(target_w);
    push.target[1] = static_cast<float>(target_h);
    push.opacity = cmd.opacity;
    if (cmd.solid) {
        push.use_solid = 1.f;
        push.solid[0] = cmd.solid_color.r;
        push.solid[1] = cmd.solid_color.g;
        push.solid[2] = cmd.solid_color.b;
        push.solid[3] = cmd.solid_color.a;
    }

    // Source box in normalized coordinates (empty src: the whole texture), turned by
    // the buffer transform — the same basis the GL backend uses.
    {
        float u0 = 0.f, v0 = 0.f, du = 1.f, dv = 1.f;
        if (tex_w > 0 && tex_h > 0) {
            const float tw = static_cast<float>(tex_w);
            const float th = static_cast<float>(tex_h);
            const lx::rect2f box = lx::gfx::effective_source(cmd.src, tex_w, tex_h);
            u0 = box.x / tw;
            v0 = box.y / th;
            du = box.width / tw;
            dv = box.height / th;
            // Inset by half a texel on each side: linear filtering samples between
            // texel centers, so without this a magnified crop reads its neighbours
            // across the crop edge. Degenerate rects collapse to the center rather than
            // inverting.
            const float half_u = 0.5f / tw;
            const float half_v = 0.5f / th;
            float u_lo = u0 + half_u;
            float v_lo = v0 + half_v;
            float u_hi = u0 + du - half_u;
            float v_hi = v0 + dv - half_v;
            if (u_lo > u_hi) u_lo = u_hi = 0.5f * (u_lo + u_hi);
            if (v_lo > v_hi) v_lo = v_hi = 0.5f * (v_lo + v_hi);
            push.src_bounds[0] = u_lo;
            push.src_bounds[1] = v_lo;
            push.src_bounds[2] = u_hi;
            push.src_bounds[3] = v_hi;
        }
        const lx::gfx::source_basis b = lx::gfx::source_basis_for(cmd.buffer_xform);
        push.src_uv[0] = u0 + b.origin[0] * du;
        push.src_uv[1] = v0 + b.origin[1] * dv;
        push.src_uv[2] = b.dx[0] * du;
        push.src_uv[3] = b.dx[1] * dv;
        push.uv_dy[0] = b.dy[0] * du;
        push.uv_dy[1] = b.dy[1] * dv;
    }

    push.tint[0] = cmd.tint.r;
    push.tint[1] = cmd.tint.g;
    push.tint[2] = cmd.tint.b;
    push.tint[3] = cmd.tint.a;
    push.transfer = transfer;
        return push;
}

lx::error vk_error(const char* message) {
    return lx::make_error(lx::vulkan_err::device_lost, message);
}

int find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

#endif

#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)

namespace {

// glslc -mfmt=c emits a bare C initializer list, so the SPIR-V lives in the binary and
// the compositor never depends on shader files at runtime.
constexpr uint32_t k_composite_vert_spv[] =
#include "shaders/composite.vert.spv.h"
    ;
constexpr uint32_t k_composite_frag_spv[] =
#include "shaders/composite.frag.spv.h"
    ;
constexpr uint32_t k_encode_vert_spv[] =
#include "shaders/encode.vert.spv.h"
    ;
constexpr uint32_t k_encode_frag_spv[] =
#include "shaders/encode.frag.spv.h"
    ;
    ;

VkShaderModule create_shader(VkDevice dev, const uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module_handle = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &info, nullptr, &module_handle) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module_handle;
}

/// The composite pipeline (composite.vert/.frag, premultiplied source-over — or no blending
/// when `blend` is false, for opaque draws — viewport and scissor dynamic) for
/// `render_pass`'s one colour attachment. Null on failure.
VkPipeline create_composite_pipeline(VkDevice dev, VkPipelineLayout pipeline_layout,
                                     VkRenderPass render_pass, VkPipelineCache cache,
                                     bool blend = true) {
    VkShaderModule vert = create_shader(dev, k_composite_vert_spv, sizeof(k_composite_vert_spv));
    VkShaderModule frag = create_shader(dev, k_composite_frag_spv, sizeof(k_composite_frag_spv));
    if (!vert || !frag) {
        if (vert)
            vkDestroyShaderModule(dev, vert, nullptr);
        if (frag)
            vkDestroyShaderModule(dev, frag, nullptr);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // Quad corners come from gl_VertexIndex, so there is no vertex input state.
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

    VkPipelineColorBlendAttachmentState attachment_blend{};
    attachment_blend.blendEnable = blend ? VK_TRUE : VK_FALSE;
    attachment_blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment_blend.colorBlendOp = VK_BLEND_OP_ADD;
    attachment_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment_blend.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &attachment_blend;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState = &viewport_state;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pColorBlendState = &blend_state;
    gpci.pDynamicState = &dynamic;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    gpci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pipeline_result =
        vkCreateGraphicsPipelines(dev, cache, 1, &gpci, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    if (pipeline_result != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

} // namespace

#endif

lx::gfx::render_target::~render_target() { destroy(); }

lx::gfx::render_target::render_target(render_target&& other) noexcept
    : ctx_{other.ctx_}, image_{other.image_}, memory_{other.memory_}, view_{other.view_},
      width_{other.width_}, height_{other.height_}, format_{other.format_} {
    other.image_ = nullptr;
    other.memory_ = nullptr;
    other.view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
}

lx::gfx::render_target& lx::gfx::render_target::operator=(render_target&& other) noexcept {
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
    other.image_ = nullptr;
    other.memory_ = nullptr;
    other.view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

void lx::gfx::render_target::destroy() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
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

lx::result<lx::gfx::render_target> lx::gfx::render_target::create(const vk_context& ctx,
                                                                  unsigned width,
                                                                  unsigned height,
                                                                  lx::pixel_format format) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ctx.valid() || width == 0 || height == 0)
        return lx::not_implemented("lx::gfx::render_target::create");

    auto dev = static_cast<VkDevice>(ctx.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx.physical_device);

    // Linear tiling with an exportable allocation: KMS consumes the same memory as a
    // DRM_FORMAT_MOD_LINEAR framebuffer, so no blit or copy sits between the two.
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_scanout_format(format);
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    render_target target;
    target.ctx_ = ctx;
    target.width_ = width;
    target.height_ = height;
    target.format_ = format;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
        return vk_error("render target vkCreateImage failed");
    target.image_ = image;

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, image, &reqs);

    const int type_index =
        find_memory_type(phys, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index < 0)
        return vk_error("no device-local memory type for render target");

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &export_info;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS)
        return vk_error("render target vkAllocateMemory failed");
    target.memory_ = memory;

    if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS)
        return vk_error("render target vkBindImageMemory failed");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vk_scanout_format(target.format_);
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        return vk_error("render target vkCreateImageView failed");
    target.view_ = view;

    return target;
#else
    (void)ctx;
    (void)width;
    (void)height;
    return lx::not_implemented("lx::gfx::render_target::create");
#endif
}

lx::result<lx::gfx::exported_dmabuf> lx::gfx::render_target::export_dmabuf() const {
#if defined(LUMEN_HAS_VULKAN)
    if (!image_ || !memory_)
        return lx::not_implemented("lx::gfx::render_target::export_dmabuf");

    imported_image shim{};
    shim.width = width_;
    shim.height = height_;
    shim.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    shim.vk_image = image_;
    shim.vk_memory = memory_;

    dmabuf_exporter exporter{ctx_.device, ctx_.physical_device};
    return exporter.export_image(shim);
#else
    return lx::not_implemented("lx::gfx::render_target::export_dmabuf");
#endif
}

lx::gfx::vulkan_compositor::~vulkan_compositor() { shutdown(); }

int lx::gfx::vulkan_compositor::find_texture(unsigned id) const {
    if (id == 0)
        return -1;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used && textures_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

int lx::gfx::vulkan_compositor::alloc_texture(unsigned id) {
    if (const int existing = find_texture(id); existing >= 0)
        return existing;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used)
            continue;
        textures_[i] = {};
        textures_[i].id = id;
        textures_[i].used = true;
        return static_cast<int>(i);
    }
    return -1;
}

void lx::gfx::vulkan_compositor::destroy_texture(texture_slot& slot) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        // Uploads are submitted without waiting, so a queued command buffer may still
        // reference this image. Destroying it first is a use-after-free inside the driver.
        // A UI renderer's frames that recorded it are not covered by those fences.
        wait_external(slot);
        wait_all_slots_idle();
        if (slot.view)
            vkDestroyImageView(dev, static_cast<VkImageView>(slot.view), nullptr);
        if (slot.owns_image && slot.image)
            vkDestroyImage(dev, static_cast<VkImage>(slot.image), nullptr);
        if (slot.owns_image && slot.memory)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(slot.memory), nullptr);
        if (slot.descriptor_set && descriptor_pool_) {
            auto set = static_cast<VkDescriptorSet>(slot.descriptor_set);
            vkFreeDescriptorSets(dev, static_cast<VkDescriptorPool>(descriptor_pool_), 1, &set);
        }
    }
#endif
    slot = {};
}

void lx::gfx::vulkan_compositor::wait_all_slots_idle() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return;
    VkFence fences[k_frame_slots]{};
    unsigned count = 0;
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        if (fences_[i])
            fences[count++] = static_cast<VkFence>(fences_[i]);
    }
    if (count > 0)
        (void)vkWaitForFences(dev, count, fences, VK_TRUE, 1'000'000'000ull);
#endif
}

void lx::gfx::vulkan_compositor::wait_external(const texture_slot& slot) {
    if (!slot.external_use)
        return;
    for (const external_user& u : external_)
        if (u.wait)
            u.wait(u.user);
    for (texture_slot& t : textures_)
        t.external_use = false;
}

bool lx::gfx::vulkan_compositor::add_external_user(external_user u) {
    if (!u.user || !u.wait)
        return false;
    for (const external_user& e : external_)
        if (e.user == u.user)
            return true;
    for (external_user& e : external_)
        if (!e.user) {
            e = u;
            return true;
        }
    return false;
}

void lx::gfx::vulkan_compositor::remove_external_user(void* user) {
    for (external_user& e : external_)
        if (e.user == user && user) {
            // Whatever it recorded is waited for before it goes: it can no longer be asked.
            if (e.wait)
                e.wait(e.user);
            e = {};
        }
}

void lx::gfx::vulkan_compositor::abandon_prepared_blits() {
    for (unsigned i = 0; i < acquired_count_; ++i) {
        texture_slot& t = textures_[acquired_[i]];
        if (t.used && !t.owns_image)
            t.layout_ready = false;
    }
    acquired_count_ = 0;
}

void lx::gfx::vulkan_compositor::forget_texture(unsigned id) {
    if (const int slot = find_texture(id); slot >= 0)
        destroy_texture(textures_[static_cast<unsigned>(slot)]);
}

lx::result<void> lx::gfx::vulkan_compositor::initialize(const vk_context& ctx) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ctx.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::initialize");
    shutdown();
    ctx_ = ctx;

    auto dev = static_cast<VkDevice>(ctx_.device);

    VkAttachmentDescription color{};
    color.format = k_linear_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The encode pass samples this next, so hand it over already readable — that transition
    // is exactly what a render pass final layout is for, and saves an explicit barrier.
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // The previous frame's encode pass may still be sampling this intermediate and its
    // composite pass writing it: this frame's writes wait for both. Both composite passes
    // carry it (render pass compatibility wants their dependencies identical).
    VkSubpassDependency pass_dep{};
    pass_dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    pass_dep.dstSubpass = 0;
    pass_dep.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    pass_dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pass_dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    pass_dep.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &pass_dep;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &render_pass) != VK_SUCCESS)
        return vk_error("vkCreateRenderPass failed");
    render_pass_ = render_pass;

    // A damage frame's: the intermediate keeps the previous frame (LOAD), left readable by
    // the pass before.
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkRenderPass load_pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &load_pass) != VK_SUCCESS)
        return vk_error("load vkCreateRenderPass failed");
    load_render_pass_ = load_pass;

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS)
        return vk_error("vkCreateSampler failed");
    sampler_ = sampler;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings = &binding;

    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &descriptor_layout) != VK_SUCCESS)
        return vk_error("vkCreateDescriptorSetLayout failed");
    descriptor_layout_ = descriptor_layout;

    // One set per texture, allocated on registration and reused: the frame path must not
    // create or destroy descriptor objects.
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = k_max_textures + k_linear_targets;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = k_max_textures + k_linear_targets; // + the encode pass's sets
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptor_pool) != VK_SUCCESS)
        return vk_error("vkCreateDescriptorPool failed");
    descriptor_pool_ = descriptor_pool;

    // The encode pass samples one image — a linear intermediate — so it needs its own
    // layout and one set per intermediate, allocated once here and pointed at it when the
    // intermediate is made.
    VkDescriptorSetLayout encode_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &encode_layout) != VK_SUCCESS)
        return vk_error("encode vkCreateDescriptorSetLayout failed");
    encode_set_layout_ = encode_layout;

    VkDescriptorSetLayout encode_layouts[k_linear_targets]{};
    for (VkDescriptorSetLayout& l : encode_layouts)
        l = encode_layout;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool;
    dsai.descriptorSetCount = k_linear_targets;
    dsai.pSetLayouts = encode_layouts;

    VkDescriptorSet encode_sets[k_linear_targets]{};
    if (vkAllocateDescriptorSets(dev, &dsai, encode_sets) != VK_SUCCESS)
        return vk_error("encode vkAllocateDescriptorSets failed");
    for (unsigned i = 0; i < k_linear_targets; ++i)
        linear_targets_[i].encode_set = encode_sets[i];

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(quad_push);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descriptor_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push_range;

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout) != VK_SUCCESS)
        return vk_error("vkCreatePipelineLayout failed");
    pipeline_layout_ = pipeline_layout;

    VkPipeline pipeline =
        create_composite_pipeline(dev, pipeline_layout, render_pass, VK_NULL_HANDLE);
    if (!pipeline)
        return vk_error("composite pipeline creation failed");
    pipeline_ = pipeline;
    VkPipeline opaque_pipeline =
        create_composite_pipeline(dev, pipeline_layout, render_pass, VK_NULL_HANDLE, false);
    if (!opaque_pipeline)
        return vk_error("composite pipeline creation failed");
    opaque_pipeline_ = opaque_pipeline;

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = static_cast<VkCommandPool>(ctx_.command_pool);
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = k_frame_slots;

    VkCommandBuffer command_buffers[k_frame_slots]{};
    if (vkAllocateCommandBuffers(dev, &cbai, command_buffers) != VK_SUCCESS)
        return vk_error("vkAllocateCommandBuffers failed");
    for (unsigned i = 0; i < k_frame_slots; ++i)
        command_buffers_[i] = command_buffers[i];

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS)
            return vk_error("vkCreateFence failed");
        fences_[i] = fence;
    }

    // Exportable semaphores turn the frame's completion into a sync_file KMS can wait on,
    // which is what lets the render thread stop blocking on the GPU. Optional: a device
    // without the extension falls back to waiting, which is correct but serialises the CPU
    // against the GPU every frame.
    can_export_fence_ = ctx_.supports_external_semaphore_fd;
    if (can_export_fence_) {
        VkExportSemaphoreCreateInfo export_info{};
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

        VkSemaphoreCreateInfo sem_ci{};
        sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_ci.pNext = &export_info;

        for (unsigned i = 0; i < k_frame_slots; ++i) {
            VkSemaphore sem = VK_NULL_HANDLE;
            if (vkCreateSemaphore(dev, &sem_ci, nullptr, &sem) != VK_SUCCESS) {
                // Not fatal — drop back to the blocking path rather than failing startup.
                can_export_fence_ = false;
                break;
            }
            slot_semaphores_[i] = sem;
        }
    }

    if (auto staging = ensure_staging_ring(k_staging_ring_bytes); !staging)
        return staging.get_error();

    return {};
#else
    (void)ctx;
    return lx::not_implemented("lx::gfx::vulkan_compositor::initialize");
#endif
}

void lx::gfx::vulkan_compositor::shutdown() {
    // External users drop their pointer to this compositor (whatever they submitted is waited
    // for below, with the device).
    for (external_user& e : external_) {
        if (e.detach)
            e.detach(e.user);
        e = {};
    }
    acquired_count_ = 0;
    linear_cb_ = nullptr;
    linear_target_ = nullptr;
    linear_pass_open_ = false;
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        vkDeviceWaitIdle(dev);
        for (unsigned i = 0; i < k_max_textures; ++i) {
            if (textures_[i].used)
                destroy_texture(textures_[i]);
        }
        for (unsigned i = 0; i < k_max_framebuffers; ++i) {
            if (framebuffers_[i].framebuffer)
                vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(framebuffers_[i].framebuffer),
                                     nullptr);
        }
        for (unsigned i = 0; i < k_frame_slots; ++i) {
            if (fences_[i])
                vkDestroyFence(dev, static_cast<VkFence>(fences_[i]), nullptr);
            if (command_buffers_[i]) {
                auto cb = static_cast<VkCommandBuffer>(command_buffers_[i]);
                vkFreeCommandBuffers(dev, static_cast<VkCommandPool>(ctx_.command_pool), 1, &cb);
            }
        }
        if (staging_memory_) {
            if (staging_mapped_)
                vkUnmapMemory(dev, static_cast<VkDeviceMemory>(staging_memory_));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(staging_memory_), nullptr);
        }
        if (staging_buffer_)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(staging_buffer_), nullptr);
        for (unsigned i = 0; i < k_frame_slots; ++i) {
            if (slot_semaphores_[i])
                vkDestroySemaphore(dev, static_cast<VkSemaphore>(slot_semaphores_[i]), nullptr);
            slot_semaphores_[i] = nullptr;
        }
        can_export_fence_ = false;
        for (unsigned i = 0; i < k_readback_slots; ++i)
            read_back_free(i);

        destroy_encode_pipeline();
        for (linear_target& t : linear_targets_) {
            destroy_linear_target(t);
            t.encode_set = nullptr; // freed with the pool below
        }
        linear_ = nullptr;
        if (encode_set_layout_)
            vkDestroyDescriptorSetLayout(dev, static_cast<VkDescriptorSetLayout>(encode_set_layout_),
                                         nullptr);
        encode_set_layout_ = nullptr;

        for (pass_pipeline& p : pass_pipelines_) {
            if (p.pipeline)
                vkDestroyPipeline(dev, static_cast<VkPipeline>(p.pipeline), nullptr);
            if (p.opaque_pipeline)
                vkDestroyPipeline(dev, static_cast<VkPipeline>(p.opaque_pipeline), nullptr);
        }
        if (pipeline_)
            vkDestroyPipeline(dev, static_cast<VkPipeline>(pipeline_), nullptr);
        if (opaque_pipeline_)
            vkDestroyPipeline(dev, static_cast<VkPipeline>(opaque_pipeline_), nullptr);
        if (pipeline_layout_)
            vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(pipeline_layout_),
                                    nullptr);
        if (descriptor_pool_)
            vkDestroyDescriptorPool(dev, static_cast<VkDescriptorPool>(descriptor_pool_),
                                    nullptr);
        if (descriptor_layout_)
            vkDestroyDescriptorSetLayout(
                dev, static_cast<VkDescriptorSetLayout>(descriptor_layout_), nullptr);
        if (sampler_)
            vkDestroySampler(dev, static_cast<VkSampler>(sampler_), nullptr);
        if (render_pass_)
            vkDestroyRenderPass(dev, static_cast<VkRenderPass>(render_pass_), nullptr);
        if (load_render_pass_)
            vkDestroyRenderPass(dev, static_cast<VkRenderPass>(load_render_pass_), nullptr);
    }
#endif
    framebuffer_ = nullptr;
    for (unsigned i = 0; i < k_max_framebuffers; ++i)
        framebuffers_[i] = {};
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        fences_[i] = nullptr;
        command_buffers_[i] = nullptr;
    }
    staging_buffer_ = nullptr;
    staging_memory_ = nullptr;
    staging_mapped_ = nullptr;
    staging_capacity_ = 0;
    staging_offset_ = 0;
    submit_slot_ = 0;
    pipeline_ = nullptr;
    opaque_pipeline_ = nullptr;
    for (pass_pipeline& p : pass_pipelines_)
        p = {};
    pipeline_layout_ = nullptr;
    descriptor_pool_ = nullptr;
    descriptor_layout_ = nullptr;
    sampler_ = nullptr;
    render_pass_ = nullptr;
    load_render_pass_ = nullptr;
    ctx_ = {};
}

lx::result<void> lx::gfx::vulkan_compositor::create_descriptor(texture_slot& slot) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto layout = static_cast<VkDescriptorSetLayout>(descriptor_layout_);
    if (!dev || !layout || !descriptor_pool_ || !slot.view)
        return lx::not_implemented("lx::gfx::vulkan_compositor::create_descriptor");

    if (!slot.descriptor_set) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = static_cast<VkDescriptorPool>(descriptor_pool_);
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev, &dsai, &set) != VK_SUCCESS)
            return vk_error("vkAllocateDescriptorSets failed");
        slot.descriptor_set = set;
    }

    VkDescriptorImageInfo image_info{};
    image_info.sampler = static_cast<VkSampler>(sampler_);
    image_info.imageView = static_cast<VkImageView>(slot.view);
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = static_cast<VkDescriptorSet>(slot.descriptor_set);
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    return {};
#else
    (void)slot;
    return lx::not_implemented("lx::gfx::vulkan_compositor::create_descriptor");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::bind_imported(const imported_image& image) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::bind_imported");
    if (!image.vk_image || image.image_id == 0) {
        return lx::make_error(lx::vulkan_err::import_failed,
                              "bind_imported without a Vulkan image");
    }

    const int index = alloc_texture(image.image_id);
    if (index < 0) {
        return lx::make_error(lx::vulkan_err::import_failed, "texture table full");
    }
    auto& slot = textures_[static_cast<unsigned>(index)];
    if (slot.image == image.vk_image && slot.view)
        return {};

    auto dev = static_cast<VkDevice>(ctx_.device);
    if (slot.view) {
        // The view and the descriptor set are re-pointed: a UI renderer's frame that recorded
        // them must be done first.
        wait_external(slot);
        vkDestroyImageView(dev, static_cast<VkImageView>(slot.view), nullptr);
        slot.view = nullptr;
    }

    slot.image = image.vk_image;
    slot.memory = image.vk_memory;
    slot.width = image.width;
    slot.height = image.height;
    slot.owns_image = false;
    slot.layout_ready = false;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = static_cast<VkImage>(slot.image);
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = image.format == static_cast<lx::fourcc>(lx::pixel_format::rgba8888)
                     ? VK_FORMAT_R8G8B8A8_UNORM
                     : VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        return vk_error("imported image vkCreateImageView failed");
    slot.view = view;

    return create_descriptor(slot);
#else
    (void)image;
    return lx::not_implemented("lx::gfx::vulkan_compositor::bind_imported");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::upload_rgba(unsigned texture_id, unsigned width,
                                                          unsigned height,
                                                          const unsigned char* rgba) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::upload_rgba");
    if (texture_id == 0 || width == 0 || height == 0 || !rgba) {
        return lx::invalid_argument("upload_rgba with empty source");
    }

    // Present no longer blocks, so the frame this reads may still be in flight. Read-back
    // is for tests and screenshots and is explicitly not on the frame path, so waiting here
    // is the right trade — the alternative is a barrier against work the caller cannot see.
    wait_all_slots_idle();

    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);

    const int index = alloc_texture(texture_id);
    if (index < 0) {
        return lx::make_error(lx::vulkan_err::import_failed, "texture table full");
    }
    auto& slot = textures_[static_cast<unsigned>(index)];
    // The copy below rewrites an image a UI renderer's frame may still sample.
    wait_external(slot);

    // Reallocate only on a size change; a resizing client is rare, a redrawing one is not.
    if (slot.image && (slot.width != width || slot.height != height)) {
        const unsigned keep_id = slot.id;
        destroy_texture(slot);
        slot.id = keep_id;
        slot.used = true;
    }

    if (!slot.image) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
            return vk_error("shm texture vkCreateImage failed");

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(dev, image, &reqs);
        const int type_index =
            find_memory_type(phys, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type_index < 0) {
            vkDestroyImage(dev, image, nullptr);
            return vk_error("no device-local memory type for shm texture");
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyImage(dev, image, nullptr);
            return vk_error("shm texture vkAllocateMemory failed");
        }
        if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(dev, memory, nullptr);
            vkDestroyImage(dev, image, nullptr);
            return vk_error("shm texture vkBindImageMemory failed");
        }

        slot.image = image;
        slot.memory = memory;
        slot.width = width;
        slot.height = height;
        slot.owns_image = true;
        slot.layout_ready = false;

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
            return vk_error("shm texture vkCreateImageView failed");
        slot.view = view;

        if (auto described = create_descriptor(slot); !described)
            return described.get_error();
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    if (auto staged = ensure_staging_ring(k_staging_ring_bytes); !staged)
        return staged.get_error();

    if (bytes > staging_capacity_) {
        return lx::make_error(lx::vulkan_err::import_failed,
                              "upload exceeds staging ring capacity");
    }
    if (staging_offset_ + bytes > staging_capacity_) {
        // Wrapping reuses memory an earlier submission may still be reading, and only the
        // slot fences tell us when that is done. Drain them before rewinding.
        VkFence all[k_frame_slots]{};
        unsigned pending = 0;
        for (unsigned i = 0; i < k_frame_slots; ++i) {
            if (fences_[i])
                all[pending++] = static_cast<VkFence>(fences_[i]);
        }
        if (pending > 0 &&
            vkWaitForFences(dev, pending, all, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
            return vk_error("vkWaitForFences timed out rewinding staging ring");
        staging_offset_ = 0;
    }

    const VkDeviceSize ring_offset = static_cast<VkDeviceSize>(staging_offset_);
    std::memcpy(staging_mapped_ + staging_offset_, rgba, static_cast<std::size_t>(bytes));
    staging_offset_ = static_cast<unsigned>((staging_offset_ + bytes + 255u) & ~255u);

    const unsigned frame_slot = submit_slot_;
    auto cb = static_cast<VkCommandBuffer>(command_buffers_[frame_slot]);
    auto fence = static_cast<VkFence>(fences_[frame_slot]);
    // Fences start signaled, so the first use of each slot does not block.
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming upload slot");
    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = static_cast<VkImage>(slot.image);
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    copy.bufferOffset = ring_offset;
    vkCmdCopyBufferToImage(cb, static_cast<VkBuffer>(staging_buffer_),
                           static_cast<VkImage>(slot.image),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_sample = to_transfer;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_sample);

    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(false); !submitted)
        return submitted.get_error();

    slot.layout_ready = true;
    return {};
#else
    (void)texture_id;
    (void)width;
    (void)height;
    (void)rgba;
    return lx::not_implemented("lx::gfx::vulkan_compositor::upload_rgba");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_staging_ring(unsigned need_bytes) {
#if defined(LUMEN_HAS_VULKAN)
    if (staging_buffer_ && staging_capacity_ >= need_bytes)
        return {};
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);
    if (!dev || !phys)
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_staging_ring");

    const VkDeviceSize capacity =
        need_bytes > k_staging_ring_bytes ? static_cast<VkDeviceSize>(need_bytes)
                                          : static_cast<VkDeviceSize>(k_staging_ring_bytes);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = capacity;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS)
        return vk_error("staging ring vkCreateBuffer failed");

    VkMemoryRequirements buffer_reqs{};
    vkGetBufferMemoryRequirements(dev, buffer, &buffer_reqs);
    const int staging_type =
        find_memory_type(phys, buffer_reqs.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging_type < 0) {
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("no host-visible memory type for staging ring");
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = buffer_reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(staging_type);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkAllocateMemory failed");
    }
    if (vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkBindBufferMemory failed");
    }

    void* mapped = nullptr;
    if (vkMapMemory(dev, memory, 0, capacity, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkMapMemory failed");
    }

    staging_buffer_ = buffer;
    staging_memory_ = memory;
    staging_mapped_ = static_cast<unsigned char*>(mapped);
    staging_capacity_ = static_cast<unsigned>(capacity);
    staging_offset_ = 0;
    return {};
#else
    (void)need_bytes;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_staging_ring");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::submit_current_slot(bool wait_for_completion,
                                                                 int* out_fence_fd) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto queue = static_cast<VkQueue>(ctx_.queue);
    const unsigned slot = submit_slot_;
    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (!dev || !queue || !cb || !fence)
        return lx::not_implemented("lx::gfx::vulkan_compositor::submit_current_slot");

    // Export the frame's completion only when the caller wants it and the device can. The
    // signal has to be requested at submit time, so this decides before the submit.
    const bool want_export = out_fence_fd != nullptr && can_export_fence_ &&
                             slot_semaphores_[slot] != nullptr;
    auto sem = static_cast<VkSemaphore>(want_export ? slot_semaphores_[slot] : nullptr);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (want_export) {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &sem;
    }
    if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        return vk_error("vkQueueSubmit failed");

    bool exported = false;
    if (want_export) {
        VkSemaphoreGetFdInfoKHR get{};
        get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        get.semaphore = sem;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

        auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR"));
        int fd = -1;
        if (get_fd && get_fd(dev, &get, &fd) == VK_SUCCESS && fd >= 0) {
            *out_fence_fd = fd;
            exported = true;
        }
    }

    // Blocking is the fallback, not the design. With an exported fence the display
    // controller waits for the GPU; without one the render thread has to, which serialises
    // the CPU against the GPU for the whole frame.
    if (wait_for_completion && !exported) {
        const VkResult waited = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull);
        if (waited != VK_SUCCESS)
            return vk_error("vkWaitForFences timed out");
    }

    submit_slot_ = (submit_slot_ + 1u) % k_frame_slots;
    return {};
#else
    (void)wait_for_completion;
    (void)out_fence_fd;
    return lx::not_implemented("lx::gfx::vulkan_compositor::submit_current_slot");
#endif
}

void lx::gfx::vulkan_compositor::destroy_encode_pipeline() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return;
    if (encode_pipeline_)
        vkDestroyPipeline(dev, static_cast<VkPipeline>(encode_pipeline_), nullptr);
    if (encode_pipeline_layout_)
        vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(encode_pipeline_layout_),
                                nullptr);
    if (encode_render_pass_)
        vkDestroyRenderPass(dev, static_cast<VkRenderPass>(encode_render_pass_), nullptr);
    if (encode_load_render_pass_)
        vkDestroyRenderPass(dev, static_cast<VkRenderPass>(encode_load_render_pass_), nullptr);
    // The encode sets come from descriptor_pool_ and die with it; encode_set_layout_ is
    // destroyed in shutdown() alongside the other layouts.
#endif
    encode_pipeline_ = nullptr;
    encode_pipeline_layout_ = nullptr;
    encode_render_pass_ = nullptr;
    encode_load_render_pass_ = nullptr;
    encode_ready_ = false;
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_encode_pipeline(lx::pixel_format format) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (encode_ready_ && encode_format_ == format)
        return {};

    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_encode_pipeline");

    wait_all_slots_idle();
    destroy_encode_pipeline();
    encode_format_ = format;

    VkAttachmentDescription color{};
    color.format = vk_scanout_format(format);
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    // The encode pass covers every pixel with a fullscreen triangle, so nothing needs
    // clearing first — DONT_CARE saves a full write of the scanout buffer per frame.
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // GENERAL, not PRESENT_SRC: the consumer is KMS via dmabuf, not a Vulkan swapchain.
    color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // The composite pass writes the intermediate and this pass samples it. Without this
    // the read may be scheduled against a write that has not landed.
    // The region pass also loads the target, ordered after its last write (a frame three
    // back) or read (a capture's copy out of it); both passes carry the one dependency
    // (render pass compatibility wants them identical).
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &pass) != VK_SUCCESS)
        return vk_error("encode vkCreateRenderPass failed");
    encode_render_pass_ = pass;

    // A region's: the target keeps what it holds outside the region (LOAD from GENERAL).
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkRenderPass load_pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &load_pass) != VK_SUCCESS)
        return vk_error("encode load vkCreateRenderPass failed");
    encode_load_render_pass_ = load_pass;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(encode_push);

    auto set_layout = static_cast<VkDescriptorSetLayout>(encode_set_layout_);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push_range;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout) != VK_SUCCESS)
        return vk_error("encode vkCreatePipelineLayout failed");
    encode_pipeline_layout_ = layout;

    VkShaderModule vert = create_shader(dev, k_encode_vert_spv, sizeof(k_encode_vert_spv));
    VkShaderModule frag = create_shader(dev, k_encode_frag_spv, sizeof(k_encode_frag_spv));
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) vkDestroyShaderModule(dev, frag, nullptr);
        return vk_error("encode shader module creation failed");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

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

    // No blending: this pass replaces the scanout buffer outright.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState = &viewport_state;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pColorBlendState = &blend_state;
    gpci.pDynamicState = &dynamic;
    gpci.layout = layout;
    gpci.renderPass = pass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult created =
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    if (created != VK_SUCCESS)
        return vk_error("encode vkCreateGraphicsPipelines failed");
    encode_pipeline_ = pipeline;

    encode_ready_ = true;
    return {};
#else
    (void)format;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_encode_pipeline");
#endif
}

void lx::gfx::vulkan_compositor::destroy_linear_target(linear_target& t) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return;
    if (t.framebuffer)
        vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(t.framebuffer), nullptr);
    if (t.view)
        vkDestroyImageView(dev, static_cast<VkImageView>(t.view), nullptr);
    if (t.image)
        vkDestroyImage(dev, static_cast<VkImage>(t.image), nullptr);
    if (t.memory)
        vkFreeMemory(dev, static_cast<VkDeviceMemory>(t.memory), nullptr);
#endif
    t.framebuffer = nullptr;
    t.view = nullptr;
    t.image = nullptr;
    t.memory = nullptr;
    t.width = 0;
    t.height = 0;
    t.last_used = 0;
    t.content = nullptr;
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_linear_target(unsigned width,
                                                                  unsigned height,
                                                                  const void* previous,
                                                                  bool chained) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev || !render_pass_ || width == 0 || height == 0)
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_linear_target");

    ++linear_frame_;
    const auto idle = [this](const linear_target& t) {
        return t.image && linear_frame_ - t.last_used > k_linear_idle_frames;
    };
    linear_target* found = nullptr;
    linear_target* same = nullptr;
    bool any_idle = false;
    bool any_free = false;
    for (linear_target& t : linear_targets_) {
        if (t.image && t.width == width && t.height == height) {
            if (previous && t.content == previous)
                found = &t; // the chain's own
            else if (!same || (same->content && !t.content) ||
                     (!same->content == !t.content && t.last_used < same->last_used))
                same = &t;
        } else {
            any_free = any_free || !t.image;
            any_idle = any_idle || idle(t);
        }
    }
    // Another chain's (an output of the same size): a chained frame takes a slot of its own
    // while there is one, so both outputs keep continuing.
    if (!found && same && (!chained || !same->content || !any_free))
        found = same;
    if (found && !any_idle) {
        found->last_used = linear_frame_;
        linear_ = found;
        return {};
    }

    // Any in-flight frame may still be sampling what is freed or re-pointed below.
    wait_all_slots_idle();
    // Unused for long: its output is gone or was resized.
    for (linear_target& t : linear_targets_)
        if (&t != found && idle(t))
            destroy_linear_target(t);
    if (found) {
        found->last_used = linear_frame_;
        linear_ = found;
        return {};
    }

    // A size not kept: an empty slot, else the least recently used one.
    linear_target* slot = &linear_targets_[0];
    for (linear_target& t : linear_targets_) {
        if (!t.image) {
            slot = &t;
            break;
        }
        if (t.last_used < slot->last_used)
            slot = &t;
    }
    linear_target& t = *slot;
    destroy_linear_target(t);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = k_linear_format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    // Optimal tiling and device-local: nothing outside the GPU ever reads this. Only the
    // scanout buffer has to be linear and exportable.
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS) {
        linear_ = nullptr;
        return vk_error("linear intermediate vkCreateImage failed");
    }
    t.image = image;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, image, &req);

    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(static_cast<VkPhysicalDevice>(ctx_.physical_device),
                                        &props);
    uint32_t type_index = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const bool allowed = (req.memoryTypeBits & (1u << i)) != 0;
        const bool device_local = (props.memoryTypes[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (allowed && device_local) {
            type_index = i;
            break;
        }
    }
    if (type_index == UINT32_MAX) {
        destroy_linear_target(t);
        linear_ = nullptr;
        return vk_error("no device-local memory type for the linear intermediate");
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
        destroy_linear_target(t);
        linear_ = nullptr;
        return vk_error("linear intermediate vkAllocateMemory failed");
    }
    t.memory = memory;

    if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS) {
        destroy_linear_target(t);
        linear_ = nullptr;
        return vk_error("linear intermediate vkBindImageMemory failed");
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = k_linear_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
        destroy_linear_target(t);
        linear_ = nullptr;
        return vk_error("linear intermediate vkCreateImageView failed");
    }
    t.view = view;

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = static_cast<VkRenderPass>(render_pass_);
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) {
        destroy_linear_target(t);
        linear_ = nullptr;
        return vk_error("linear intermediate vkCreateFramebuffer failed");
    }
    t.framebuffer = fb;
    t.width = width;
    t.height = height;
    t.last_used = linear_frame_;
    linear_ = &t;
    ++linear_allocations_;

    // Point the encode pass's sampler at the new image.
    if (t.encode_set) {
        VkDescriptorImageInfo info{};
        info.sampler = static_cast<VkSampler>(sampler_);
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = static_cast<VkDescriptorSet>(t.encode_set);
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
    return {};
#else
    (void)width;
    (void)height;
    (void)previous;
    (void)chained;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_linear_target");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_framebuffer(const render_target& target) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    // The scanout framebuffer belongs to the encode pass — the composite pass renders into
    // the linear intermediate and never touches the target directly.
    if (!dev || !encode_render_pass_ || !target.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_framebuffer");

    int free_slot = -1;
    for (unsigned i = 0; i < k_max_framebuffers; ++i) {
        if (framebuffers_[i].framebuffer && framebuffers_[i].image == target.vk_image()) {
            framebuffer_ = framebuffers_[i].framebuffer;
            framebuffer_index_ = i;
            return {};
        }
        if (!framebuffers_[i].framebuffer && free_slot < 0)
            free_slot = static_cast<int>(i);
    }

    if (free_slot < 0) {
        // More distinct targets than slots: evict the first rather than grow unbounded.
        vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(framebuffers_[0].framebuffer),
                             nullptr);
        framebuffers_[0] = {};
        free_slot = 0;
    }

    VkImageView attachment = static_cast<VkImageView>(target.vk_image_view());
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = static_cast<VkRenderPass>(encode_render_pass_);
    fbci.attachmentCount = 1;
    fbci.pAttachments = &attachment;
    fbci.width = target.width();
    fbci.height = target.height();
    fbci.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &framebuffer) != VK_SUCCESS)
        return vk_error("vkCreateFramebuffer failed");
    framebuffers_[static_cast<unsigned>(free_slot)] = {framebuffer, target.vk_image()};
    framebuffer_ = framebuffer;
    framebuffer_index_ = static_cast<unsigned>(free_slot);
    return {};
#else
    (void)target;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_framebuffer");
#endif
}

void lx::gfx::vulkan_compositor::plan_frame(const render_target& target,
                                            const damage_region* region,
                                            const damage_region* changed,
                                            const render_target* previous, bool chained) {
    const lx::rect2i bounds{0, 0, static_cast<int>(target.width()),
                            static_cast<int>(target.height())};
    // Rects of `d` inside the target; true when it is the whole target.
    const auto fill = [&](const damage_region& d, lx::rect2i* out, unsigned& n) {
        n = 0;
        if (d.full)
            return true;
        for (unsigned i = 0; i < d.count; ++i) {
            const lx::rect2i r = detail::damage_intersect(d.rects[i], bounds);
            if (detail::damage_rect_empty(r))
                continue;
            if (r.width == bounds.width && r.height == bounds.height)
                return true;
            out[n++] = r;
        }
        return false;
    };
    const bool continues = region && changed && previous && linear_->content &&
                           linear_->content == previous->vk_image();
    pass_full_ = !continues || fill(*changed, pass_rects_, pass_count_);
    if (pass_full_)
        pass_count_ = 0;
    encode_full_ = !region || !framebuffers_[framebuffer_index_].written;
    if (!encode_full_) {
        // What the composite pass changes is encoded too, whatever the caller's region says.
        damage_region encode = *region;
        if (changed)
            encode.add_region(*changed);
        encode_full_ = fill(encode, encode_rects_, encode_count_);
    }
    if (encode_full_)
        encode_count_ = 0;
    frame_chained_ = chained;
    // Until this frame is submitted, nothing continues from what the intermediate holds.
    linear_->content = nullptr;
}

void lx::gfx::vulkan_compositor::finish_frame(const render_target& target) {
    framebuffers_[framebuffer_index_].written = true;
    if (!frame_chained_ || !linear_)
        return;
    // One intermediate per target: an older one naming it (a handle reused) is stale.
    for (linear_target& t : linear_targets_)
        if (t.content == target.vk_image())
            t.content = nullptr;
    linear_->content = target.vk_image();
}

void lx::gfx::vulkan_compositor::begin_composite_pass(void* command_buffer, unsigned width,
                                                      unsigned height, lx::color clear) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    // The attachment holds linear light, so the clear colour has to be decoded too —
    // clearing with the encoded value would make the background the wrong brightness.
    VkClearValue clear_value{};
    clear_value.color = {{lx::to_linear(clear.r, clear.encoding),
                          lx::to_linear(clear.g, clear.encoding),
                          lx::to_linear(clear.b, clear.encoding), clear.a}};
    for (unsigned i = 0; i < 4; ++i)
        pass_clear_[i] = clear_value.color.float32[i];

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.framebuffer = static_cast<VkFramebuffer>(linear_->framebuffer);
    if (pass_full_) {
        rpbi.renderPass = static_cast<VkRenderPass>(render_pass_);
        rpbi.renderArea.extent = {width, height};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear_value;
    } else {
        // Loaded: the rects are cleared one by one as they are drawn (`clear_pass_rect`).
        // No rect (nothing changed): an empty pass over one pixel.
        damage_region area{};
        for (unsigned i = 0; i < pass_count_; ++i)
            area.rects[area.count++] = pass_rects_[i];
        const lx::rect2i box = pass_count_ > 0 ? area.bounds() : lx::rect2i{0, 0, 1, 1};
        rpbi.renderPass = static_cast<VkRenderPass>(load_render_pass_);
        rpbi.renderArea.offset = {box.x, box.y};
        rpbi.renderArea.extent = {static_cast<unsigned>(box.width),
                                  static_cast<unsigned>(box.height)};
    }
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {width, height};
    vkCmdSetScissor(cb, 0, 1, &scissor);
#else
    (void)command_buffer;
    (void)width;
    (void)height;
    (void)clear;
#endif
}

void lx::gfx::vulkan_compositor::clear_pass_rect(void* command_buffer, lx::rect2i rect) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    VkClearAttachment ca{};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    for (unsigned i = 0; i < 4; ++i)
        ca.clearValue.color.float32[i] = pass_clear_[i];
    VkClearRect cr{};
    cr.rect.offset = {rect.x, rect.y};
    cr.rect.extent = {static_cast<unsigned>(rect.width), static_cast<unsigned>(rect.height)};
    cr.layerCount = 1;
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdSetScissor(cb, 0, 1, &cr.rect);
#else
    (void)command_buffer;
    (void)rect;
#endif
}

void lx::gfx::vulkan_compositor::record_encode_pass(void* command_buffer,
                                                    const render_target& target) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    // ── Encode pass ────────────────────────────────────────────────────────────────
    //
    // The composite pass left the intermediate holding blended linear light, already in
    // SHADER_READ_ONLY_OPTIMAL via its final layout. This converts it into the display's
    // transfer function and writes the scanout buffer. A fullscreen triangle, no blending,
    // and the render pass's subpass dependency orders the sample after the write. A region
    // is the same triangle once per rect, scissored to it, over a target that keeps the
    // rest (loaded).
    if (!encode_full_ && encode_count_ == 0)
        return; // nothing of the target to write
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    VkRenderPassBeginInfo encode_rpbi{};
    encode_rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    encode_rpbi.framebuffer = static_cast<VkFramebuffer>(framebuffer_);
    if (encode_full_) {
        encode_rpbi.renderPass = static_cast<VkRenderPass>(encode_render_pass_);
        encode_rpbi.renderArea.extent = {target.width(), target.height()};
    } else {
        damage_region area{};
        for (unsigned i = 0; i < encode_count_; ++i)
            area.rects[area.count++] = encode_rects_[i];
        const lx::rect2i box = area.bounds();
        encode_rpbi.renderPass = static_cast<VkRenderPass>(encode_load_render_pass_);
        encode_rpbi.renderArea.offset = {box.x, box.y};
        encode_rpbi.renderArea.extent = {static_cast<unsigned>(box.width),
                                         static_cast<unsigned>(box.height)};
    }
    vkCmdBeginRenderPass(cb, &encode_rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(target.width());
    viewport.height = static_cast<float>(target.height());
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(encode_pipeline_));
    auto encode_set = static_cast<VkDescriptorSet>(linear_->encode_set);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            static_cast<VkPipelineLayout>(encode_pipeline_layout_), 0, 1,
                            &encode_set, 0, nullptr);
    encode_push epush{};
    epush.transfer = transfer_slot(output_transfer_);
    vkCmdPushConstants(cb, static_cast<VkPipelineLayout>(encode_pipeline_layout_),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(epush), &epush);
    const unsigned passes = encode_full_ ? 1u : encode_count_;
    for (unsigned i = 0; i < passes; ++i) {
        VkRect2D scissor{};
        if (encode_full_) {
            scissor.extent = {target.width(), target.height()};
        } else {
            const lx::rect2i r = encode_rects_[i];
            scissor.offset = {r.x, r.y};
            scissor.extent = {static_cast<unsigned>(r.width), static_cast<unsigned>(r.height)};
        }
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
#else
    (void)command_buffer;
    (void)target;
#endif
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::composite(
    render_target& target, lx::color clear, const blit_command* cmds, unsigned count) {
    damage_region all{};
    all.set_full();
    return composite_frame(target, clear, cmds, count, all, all, nullptr, false);
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::composite_region(
    render_target& target, lx::color clear, const blit_command* cmds, unsigned count,
    const damage_region& region, const damage_region& changed, const render_target* previous) {
    return composite_frame(target, clear, cmds, count, region, changed, previous, true);
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::composite_frame(
    render_target& target, lx::color clear, const blit_command* cmds, unsigned count,
    const damage_region& region, const damage_region& changed, const render_target* previous,
    bool chained) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::composite");
    if (!target.valid()) {
        return lx::invalid_argument("composite into an invalid render target");
    }
    if (region.empty() && changed.empty())
        return composite_stats{};
    // Composite renders into a linear-light intermediate; a second pass encodes that into
    // the scanout buffer. Both are sized to the target and rebuilt only when it changes.
    if (auto linear = ensure_linear_target(target.width(), target.height(),
                                           previous ? previous->vk_image() : nullptr, chained);
        !linear)
        return linear.get_error();
    if (auto encode = ensure_encode_pipeline(target.format()); !encode)
        return encode.get_error();
    if (auto framebuffer = ensure_framebuffer(target); !framebuffer)
        return framebuffer.get_error();

    // A solid draw needs some bound descriptor set (see `k_solid_texture_id`).
    if (auto solid = ensure_solid_texture(cmds, count); !solid)
        return solid.get_error();

    const unsigned slot = submit_slot_;
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (vkWaitForFences(static_cast<VkDevice>(ctx_.device), 1, &fence, VK_TRUE,
                        1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming composite slot");
    vkResetFences(static_cast<VkDevice>(ctx_.device), 1, &fence);

    plan_frame(target, &region, &changed, previous, chained);

    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    record_acquires(cb);
    begin_composite_pass(cb, target.width(), target.height(), clear);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(pipeline_));
    void* bound_pipeline = pipeline_;

    const int max_x = static_cast<int>(target.width());
    const int max_y = static_cast<int>(target.height());
    composite_stats stats{};
    void* bound_set = nullptr;
    // One pass over the draws per rect (the whole target: one), each rect cleared first —
    // so rects that overlap redraw their overlap from the clear, never blend it twice.
    const unsigned passes = pass_full_ ? 1u : pass_count_;
    for (unsigned pass = 0; pass < passes; ++pass) {
        const lx::rect2i area = pass_full_ ? lx::rect2i{0, 0, max_x, max_y} : pass_rects_[pass];
        if (!pass_full_)
            clear_pass_rect(cb, area);
        for (unsigned i = 0; i < count && cmds; ++i) {
            const auto& cmd = cmds[i];
            if (cmd.occluded || draw_invisible(cmd)) {
                ++stats.draws_culled;
                continue;
            }
            // A solid draw samples nothing: any set will do, the frame's current one first.
            const int index =
                find_texture(cmd.solid ? (bound_set ? 0u : k_solid_texture_id) : cmd.texture_id);
            if (index < 0 && !(cmd.solid && bound_set)) {
                ++stats.draws_skipped;
                continue;
            }
            const texture_slot* slot_ptr =
                index >= 0 && !cmd.solid ? &textures_[static_cast<unsigned>(index)] : nullptr;
            void* want_set = index >= 0 ? textures_[static_cast<unsigned>(index)].descriptor_set
                                        : bound_set;
            if (!want_set) {
                ++stats.draws_skipped;
                continue;
            }

            // Clip is a scissor, which costs nothing extra here — the pipeline already
            // declares scissor as dynamic state. An empty clip means unclipped. A damage
            // rect is one more scissor, and a draw whose quad (exactly `dst`) misses it is
            // not recorded.
            int x0 = area.x;
            int y0 = area.y;
            int x1 = area.x + area.width;
            int y1 = area.y + area.height;
            if (cmd.clip.width > 0 && cmd.clip.height > 0) {
                x0 = std::max(x0, cmd.clip.x);
                y0 = std::max(y0, cmd.clip.y);
                x1 = std::min(x1, cmd.clip.x + cmd.clip.width);
                y1 = std::min(y1, cmd.clip.y + cmd.clip.height);
            }
            if (x1 <= x0 || y1 <= y0 ||
                (!pass_full_ &&
                 (std::max(x0, cmd.dst.x) >= std::min(x1, cmd.dst.x + cmd.dst.width) ||
                  std::max(y0, cmd.dst.y) >= std::min(y1, cmd.dst.y + cmd.dst.height)))) {
                ++stats.draws_culled;
                continue; // clipped away entirely
            }
            // Partly covered (the visibility pass): one scissored draw per visible rect.
            VkRect2D parts[k_max_visible_rects]{};
            const unsigned part_count = visible_scissors(cmd, {x0, y0, x1 - x0, y1 - y0}, parts);
            if (part_count == 0) {
                ++stats.draws_culled;
                continue;
            }
            if (want_set != bound_set) {
                auto set = static_cast<VkDescriptorSet>(want_set);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        static_cast<VkPipelineLayout>(pipeline_layout_), 0, 1,
                                        &set, 0, nullptr);
                bound_set = want_set;
            }
            // Bound sets, push constants and dynamic state survive the switch: one layout.
            void* want_pipeline =
                cmd.blend == lx::blend_mode::opaque ? opaque_pipeline_ : pipeline_;
            if (want_pipeline != bound_pipeline) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  static_cast<VkPipeline>(want_pipeline));
                bound_pipeline = want_pipeline;
            }
            const quad_push push = make_quad_push(
                cmd, slot_ptr ? slot_ptr->width : 0u, slot_ptr ? slot_ptr->height : 0u,
                target.width(), target.height(), transfer_slot(cmd.src_transfer));

            vkCmdPushConstants(cb, static_cast<VkPipelineLayout>(pipeline_layout_),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);

            for (unsigned p = 0; p < part_count; ++p) {
                vkCmdSetScissor(cb, 0, 1, &parts[p]);
                vkCmdDraw(cb, 6, 1, 0, 0);
            }
            ++stats.draws_submitted;
        }
    }

    vkCmdEndRenderPass(cb);
    record_encode_pass(cb, target);
    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(true, &stats.out_fence_fd); !submitted)
        return submitted.get_error();
    finish_frame(target);
    return stats;
#else
    (void)target;
    (void)clear;
    (void)cmds;
    (void)count;
    (void)region;
    (void)changed;
    (void)previous;
    (void)chained;
    return lx::not_implemented("lx::gfx::vulkan_compositor::composite");
#endif
}

void lx::gfx::vulkan_compositor::record_acquires(void* command_buffer) {
#if defined(LUMEN_HAS_VULKAN)
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    acquired_count_ = 0;
    // Acquire freshly imported client images. A dma-buf written by another device has no
    // Vulkan release to pair with, so the acquire comes from the foreign queue family and
    // the modifier — not a Vulkan layout — is what guarantees the contents survive.
    for (unsigned i = 0; i < k_max_textures; ++i) {
        auto& slot = textures_[i];
        if (!slot.used || slot.layout_ready || !slot.image || slot.owns_image)
            continue;

        VkImageMemoryBarrier acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        acquire.srcQueueFamilyIndex = ctx_.supports_queue_family_foreign
                                          ? VK_QUEUE_FAMILY_FOREIGN_EXT
                                          : VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = ctx_.supports_queue_family_foreign
                                          ? ctx_.queue_family
                                          : VK_QUEUE_FAMILY_IGNORED;
        acquire.image = static_cast<VkImage>(slot.image);
        acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        acquire.subresourceRange.levelCount = 1;
        acquire.subresourceRange.layerCount = 1;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &acquire);
        slot.layout_ready = true;
        // Undone by `abandon_prepared_blits` when this command buffer is never submitted.
        if (acquired_count_ < k_max_textures)
            acquired_[acquired_count_++] = static_cast<std::uint16_t>(i);
    }
#else
    (void)command_buffer;
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_solid_texture(const blit_command* cmds,
                                                                  unsigned count) {
    if (find_texture(k_solid_texture_id) >= 0)
        return {};
    bool any_solid = false;
    for (unsigned i = 0; i < count && cmds && !any_solid; ++i)
        any_solid = cmds[i].solid;
    if (!any_solid)
        return {};
    static const unsigned char k_white[4] = {255, 255, 255, 255};
    return upload_rgba(k_solid_texture_id, 1, 1, k_white);
}

lx::result<void> lx::gfx::vulkan_compositor::prepare_blits(void* command_buffer,
                                                           const blit_command* cmds,
                                                           unsigned count) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ready() || !command_buffer)
        return lx::not_implemented("lx::gfx::vulkan_compositor::prepare_blits");
    if (auto solid = ensure_solid_texture(cmds, count); !solid)
        return solid.get_error();
    record_acquires(command_buffer);
    return {};
#else
    (void)command_buffer;
    (void)cmds;
    (void)count;
    return lx::not_implemented("lx::gfx::vulkan_compositor::prepare_blits");
#endif
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::record_blits(
    void* command_buffer, void* render_pass, unsigned vk_format, void* pipeline_cache,
    unsigned width, unsigned height, const blit_command* cmds, unsigned count,
    lx::rect2i scissor, bool encoded) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ready() || !command_buffer || !render_pass)
        return lx::not_implemented("lx::gfx::vulkan_compositor::record_blits");
    if (width == 0 || height == 0)
        return lx::invalid_argument("record_blits into an empty attachment");
    auto dev = static_cast<VkDevice>(ctx_.device);

    // The pipeline for this attachment format, made once; this compositor's own linear pass
    // draws with its composite pipeline.
    void* pipeline = render_pass == render_pass_ ? pipeline_ : nullptr;
    void* opaque_pipeline = render_pass == render_pass_ ? opaque_pipeline_ : nullptr;
    for (const pass_pipeline& p : pass_pipelines_)
        if (!pipeline && p.pipeline && p.format == vk_format) {
            pipeline = p.pipeline;
            opaque_pipeline = p.opaque_pipeline;
        }
    // Another command buffer than this compositor's frame: its textures are marked as an
    // external user's until the users are waited for.
    const bool external = command_buffer != linear_cb_;
    if (!pipeline) {
        VkPipeline made = create_composite_pipeline(
            dev, static_cast<VkPipelineLayout>(pipeline_layout_),
            static_cast<VkRenderPass>(render_pass), static_cast<VkPipelineCache>(pipeline_cache));
        VkPipeline made_opaque =
            made ? create_composite_pipeline(dev, static_cast<VkPipelineLayout>(pipeline_layout_),
                                             static_cast<VkRenderPass>(render_pass),
                                             static_cast<VkPipelineCache>(pipeline_cache), false)
                 : VK_NULL_HANDLE;
        if (!made_opaque) {
            if (made)
                vkDestroyPipeline(dev, made, nullptr);
            return vk_error("record_blits pipeline creation failed");
        }
        unsigned slot = 0;
        while (slot < k_pass_pipelines && pass_pipelines_[slot].pipeline)
            ++slot;
        if (slot == k_pass_pipelines) {
            // Evicted: whatever recorded it may still be in flight.
            (void)vkDeviceWaitIdle(dev);
            vkDestroyPipeline(dev, static_cast<VkPipeline>(pass_pipelines_[0].pipeline), nullptr);
            vkDestroyPipeline(dev, static_cast<VkPipeline>(pass_pipelines_[0].opaque_pipeline),
                              nullptr);
            slot = 0;
        }
        pass_pipelines_[slot] = {vk_format, made, made_opaque};
        pipeline = made;
        opaque_pipeline = made_opaque;
    }

    composite_stats stats{};
    auto cb = static_cast<VkCommandBuffer>(command_buffer);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(pipeline));
    void* bound_pipeline = pipeline;
    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cb, 0, 1, &viewport);

    void* bound_set = nullptr;
    for (unsigned i = 0; i < count && cmds; ++i) {
        const blit_command& cmd = cmds[i];
        if (cmd.occluded || draw_invisible(cmd)) {
            ++stats.draws_culled;
            continue;
        }
        // As `composite`: a solid draw samples nothing, any set will do.
        const int index =
            find_texture(cmd.solid ? (bound_set ? 0u : k_solid_texture_id) : cmd.texture_id);
        if (index < 0 && !(cmd.solid && bound_set)) {
            ++stats.draws_skipped;
            continue;
        }
        const texture_slot* slot_ptr =
            index >= 0 && !cmd.solid ? &textures_[static_cast<unsigned>(index)] : nullptr;
        void* want_set = index >= 0 ? textures_[static_cast<unsigned>(index)].descriptor_set
                                    : bound_set;
        if (!want_set) {
            ++stats.draws_skipped;
            continue;
        }
        lx::rect2i clip = scissor;
        if (cmd.clip.width > 0 && cmd.clip.height > 0) {
            const long long x0 = clip.x > cmd.clip.x ? clip.x : cmd.clip.x;
            const long long y0 = clip.y > cmd.clip.y ? clip.y : cmd.clip.y;
            const long long x1 = std::min(static_cast<long long>(clip.x) + clip.width,
                                          static_cast<long long>(cmd.clip.x) + cmd.clip.width);
            const long long y1 = std::min(static_cast<long long>(clip.y) + clip.height,
                                          static_cast<long long>(cmd.clip.y) + cmd.clip.height);
            clip = x1 > x0 && y1 > y0 ? lx::rect2i{static_cast<int>(x0), static_cast<int>(y0),
                                                   static_cast<int>(x1 - x0),
                                                   static_cast<int>(y1 - y0)}
                                      : lx::rect2i{};
        }
        if (clip.width <= 0 || clip.height <= 0 || clip.x < 0 || clip.y < 0) {
            ++stats.draws_culled;
            continue;
        }
        // As `composite`: one scissored draw per visible rect of a partly covered draw.
        VkRect2D parts[k_max_visible_rects]{};
        const unsigned part_count = visible_scissors(cmd, clip, parts);
        if (part_count == 0) {
            ++stats.draws_culled;
            continue;
        }
        if (want_set != bound_set) {
            auto set = static_cast<VkDescriptorSet>(want_set);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    static_cast<VkPipelineLayout>(pipeline_layout_), 0, 1, &set,
                                    0, nullptr);
            bound_set = want_set;
        }
        if (external && index >= 0)
            textures_[static_cast<unsigned>(index)].external_use = true;
        // As `composite`: opaque draws without blending, sets and push ranges kept bound.
        void* want_pipeline = cmd.blend == lx::blend_mode::opaque ? opaque_pipeline : pipeline;
        if (want_pipeline != bound_pipeline) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              static_cast<VkPipeline>(want_pipeline));
            bound_pipeline = want_pipeline;
        }
        // Encoded target: transfer slot 0 ("linear") is the identity — the texel's encoded
        // values blend as they are.
        const quad_push push =
            make_quad_push(cmd, slot_ptr ? slot_ptr->width : 0u, slot_ptr ? slot_ptr->height : 0u,
                           width, height, encoded ? 0 : transfer_slot(cmd.src_transfer));
        vkCmdPushConstants(cb, static_cast<VkPipelineLayout>(pipeline_layout_),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        for (unsigned p = 0; p < part_count; ++p) {
            vkCmdSetScissor(cb, 0, 1, &parts[p]);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
        ++stats.draws_submitted;
    }
    return stats;
#else
    (void)command_buffer;
    (void)render_pass;
    (void)vk_format;
    (void)pipeline_cache;
    (void)width;
    (void)height;
    (void)cmds;
    (void)count;
    (void)scissor;
    (void)encoded;
    return lx::not_implemented("lx::gfx::vulkan_compositor::record_blits");
#endif
}

lx::result<lx::gfx::vulkan_compositor::linear_frame>
lx::gfx::vulkan_compositor::begin_linear_frame(render_target& target, const blit_command* cmds,
                                               unsigned count, const damage_region* region,
                                               const damage_region* changed,
                                               const render_target* previous) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::begin_linear_frame");
    if (!target.valid())
        return lx::invalid_argument("begin_linear_frame on an invalid render target");
    if (linear_cb_)
        abandon_linear_frame(); // a frame begun and never ended: dropped
    // As `composite`: the intermediate, the encode pipeline and the framebuffer, rebuilt only
    // when the target changes; the solid texture before the slot is taken (it is an upload of
    // its own). A region makes the frame part of a chain, as `composite_region`.
    if (auto linear = ensure_linear_target(target.width(), target.height(),
                                           previous ? previous->vk_image() : nullptr,
                                           region != nullptr);
        !linear)
        return linear.get_error();
    if (auto encode = ensure_encode_pipeline(target.format()); !encode)
        return encode.get_error();
    if (auto framebuffer = ensure_framebuffer(target); !framebuffer)
        return framebuffer.get_error();
    if (auto solid = ensure_solid_texture(cmds, count); !solid)
        return solid.get_error();

    auto dev = static_cast<VkDevice>(ctx_.device);
    const unsigned slot = submit_slot_;
    auto fence = static_cast<VkFence>(fences_[slot]);
    // The fence is reset just before the submit: a frame abandoned on the way leaves it
    // signalled, and the next wait does not hang on work never submitted.
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming composite slot");
    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS)
        return vk_error("vkBeginCommandBuffer failed");
    damage_region all{};
    all.set_full();
    plan_frame(target, region ? region : &all, region ? changed : nullptr, previous,
               region != nullptr);
    linear_cb_ = cb;
    linear_target_ = &target;
    linear_pass_open_ = false;
    record_acquires(cb);
    linear_frame f{};
    f.command_buffer = cb;
    f.render_pass = render_pass_;
    f.vk_format = static_cast<unsigned>(k_linear_format);
    f.width = target.width();
    f.height = target.height();
    f.full = pass_full_;
    f.rects = pass_rects_;
    f.rect_count = pass_count_;
    return f;
#else
    (void)target;
    (void)cmds;
    (void)count;
    (void)region;
    (void)changed;
    (void)previous;
    return lx::not_implemented("lx::gfx::vulkan_compositor::begin_linear_frame");
#endif
}

void lx::gfx::vulkan_compositor::begin_linear_pass(lx::color clear) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!linear_cb_ || !linear_target_ || linear_pass_open_)
        return;
    // As `composite`: the attachment holds linear light, so the clear is decoded.
    begin_composite_pass(linear_cb_, linear_target_->width(), linear_target_->height(), clear);
    linear_pass_open_ = true;
#else
    (void)clear;
#endif
}

void lx::gfx::vulkan_compositor::begin_linear_rect(lx::rect2i rect) {
    if (!linear_cb_ || !linear_pass_open_ || pass_full_)
        return;
    clear_pass_rect(linear_cb_, rect);
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::end_linear_frame() {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!linear_cb_ || !linear_target_)
        return lx::invalid_argument("end_linear_frame without begin_linear_frame");
    auto cb = static_cast<VkCommandBuffer>(linear_cb_);
    const render_target& target = *linear_target_;
    if (!linear_pass_open_)
        begin_linear_pass(lx::color::rgb(0.f, 0.f, 0.f));
    vkCmdEndRenderPass(cb);
    linear_pass_open_ = false;

    // The encode pass, as `composite` records it: the intermediate (left in
    // SHADER_READ_ONLY_OPTIMAL by the pass) into the scanout target's transfer function.
    record_encode_pass(cb, target);

    linear_cb_ = nullptr;
    linear_target_ = nullptr;
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        abandon_prepared_blits();
        return vk_error("vkEndCommandBuffer failed");
    }
    auto fence = static_cast<VkFence>(fences_[submit_slot_]);
    vkResetFences(static_cast<VkDevice>(ctx_.device), 1, &fence);
    composite_stats stats{};
    if (auto submitted = submit_current_slot(true, &stats.out_fence_fd); !submitted) {
        // Nothing ran: the acquires go again, and the fence must not stay unsignalled.
        abandon_prepared_blits();
        VkSubmitInfo none{};
        none.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        (void)vkQueueSubmit(static_cast<VkQueue>(ctx_.queue), 0, &none, fence);
        return submitted.get_error();
    }
    acquired_count_ = 0;
    finish_frame(target);
    return stats;
#else
    return lx::not_implemented("lx::gfx::vulkan_compositor::end_linear_frame");
#endif
}

void lx::gfx::vulkan_compositor::abandon_linear_frame() {
#if defined(LUMEN_HAS_VULKAN)
    if (linear_cb_) {
        auto cb = static_cast<VkCommandBuffer>(linear_cb_);
        if (linear_pass_open_)
            vkCmdEndRenderPass(cb);
        (void)vkEndCommandBuffer(cb);
    }
#endif
    linear_cb_ = nullptr;
    linear_target_ = nullptr;
    linear_pass_open_ = false;
    abandon_prepared_blits();
}

lx::result<void> lx::gfx::vulkan_compositor::read_back(const render_target& target,
                                                        unsigned char* rgba,
                                                        unsigned capacity) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready() || !target.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::read_back");
    const unsigned needed = target.width() * target.height() * 4u;
    if (!rgba || capacity < needed) {
        return lx::invalid_argument("read_back destination too small");
    }

    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = needed;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS)
        return vk_error("read_back vkCreateBuffer failed");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev, staging, &reqs);
    const int type_index =
        find_memory_type(phys, reqs.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type_index < 0) {
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("no host-visible memory type for read_back");
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkAllocateMemory failed");
    }
    if (vkBindBufferMemory(dev, staging, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkBindBufferMemory failed");
    }

    const unsigned frame_slot = submit_slot_;
    auto fence = static_cast<VkFence>(fences_[frame_slot]);
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming read_back slot");
    vkResetFences(dev, 1, &fence);

    auto cb = static_cast<VkCommandBuffer>(command_buffers_[frame_slot]);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = static_cast<VkImage>(target.vk_image());
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {target.width(), target.height(), 1};
    vkCmdCopyImageToBuffer(cb, static_cast<VkImage>(target.vk_image()),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);

    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &back);

    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(true); !submitted)
        return submitted.get_error();

    void* mapped = nullptr;
    if (vkMapMemory(dev, memory, 0, needed, 0, &mapped) == VK_SUCCESS) {
        // The target is B8G8R8A8; callers expect tightly packed RGBA.
        const auto* src = static_cast<const unsigned char*>(mapped);
        for (unsigned i = 0; i < needed; i += 4u) {
            rgba[i + 0] = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i + 0];
            rgba[i + 3] = src[i + 3];
        }
        vkUnmapMemory(dev, memory);
    } else {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkMapMemory failed");
    }

    vkFreeMemory(dev, memory, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    return {};
#else
    (void)target;
    (void)rgba;
    (void)capacity;
    return lx::not_implemented("lx::gfx::vulkan_compositor::read_back");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::read_back_async(unsigned slot,
                                                              const render_target& target,
                                                              const lx::rect2i* rects,
                                                              unsigned count) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready() || !target.valid() || slot >= k_readback_slots)
        return lx::not_implemented("lx::gfx::vulkan_compositor::read_back_async");
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);
    auto queue = static_cast<VkQueue>(ctx_.queue);
    readback_slot& rb = readbacks_[slot];
    const unsigned w = target.width();
    const unsigned h = target.height();
    const unsigned needed = w * h * 4u;

    if (rb.submitted) {
        // The caller waited for it already (its read landed); this only reclaims the fence.
        auto fence = static_cast<VkFence>(rb.fence);
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
            return vk_error("read_back_async: the slot's previous read never completed");
        rb.submitted = false;
    }
    if (rb.capacity < needed) {
        // The output grew (or the first frame): the buffer is made once per size.
        if (rb.memory) {
            vkUnmapMemory(dev, static_cast<VkDeviceMemory>(rb.memory));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(rb.memory), nullptr);
        }
        if (rb.buffer)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(rb.buffer), nullptr);
        rb.buffer = rb.memory = nullptr;
        rb.mapped = nullptr;
        rb.capacity = 0;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = needed;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS)
            return vk_error("read_back_async vkCreateBuffer failed");
        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(dev, buffer, &reqs);
        // Cached memory where there is some: the CPU reads every byte of it back.
        int type = find_memory_type(phys, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0)
            type = find_memory_type(phys, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) {
            vkDestroyBuffer(dev, buffer, nullptr);
            return vk_error("no host-visible memory type for read_back_async");
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = static_cast<uint32_t>(type);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(dev, buffer, nullptr);
            return vk_error("read_back_async vkAllocateMemory failed");
        }
        void* mapped = nullptr;
        if (vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            vkFreeMemory(dev, memory, nullptr);
            vkDestroyBuffer(dev, buffer, nullptr);
            return vk_error("read_back_async: buffer bind or map failed");
        }
        rb.buffer = buffer;
        rb.memory = memory;
        rb.mapped = static_cast<unsigned char*>(mapped);
        rb.capacity = needed;
    }
    if (!rb.command_buffer) {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = static_cast<VkCommandPool>(ctx_.command_pool);
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS)
            return vk_error("read_back_async vkAllocateCommandBuffers failed");
        rb.command_buffer = cb;
    }
    if (!rb.fence) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS)
            return vk_error("read_back_async vkCreateFence failed");
        rb.fence = fence;
    }
    auto fence = static_cast<VkFence>(rb.fence);
    vkResetFences(dev, 1, &fence);

    auto cb = static_cast<VkCommandBuffer>(rb.command_buffer);
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    // After the composites that wrote the target (earlier submissions on this queue).
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = static_cast<VkImage>(target.vk_image());
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

    // Only the rects asked for, each at its own place in the buffer.
    constexpr unsigned k_max_regions = 32;
    VkBufferImageCopy regions[k_max_regions]{};
    unsigned nregions = 0;
    const auto add = [&](int x, int y, int rw, int rh) {
        const int x0 = x < 0 ? 0 : x;
        const int y0 = y < 0 ? 0 : y;
        const int x1 = x + rw > static_cast<int>(w) ? static_cast<int>(w) : x + rw;
        const int y1 = y + rh > static_cast<int>(h) ? static_cast<int>(h) : y + rh;
        if (x1 <= x0 || y1 <= y0)
            return;
        VkBufferImageCopy& r = regions[nregions++];
        r.bufferOffset = (static_cast<VkDeviceSize>(y0) * w + static_cast<VkDeviceSize>(x0)) * 4u;
        r.bufferRowLength = w;
        r.bufferImageHeight = h;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageOffset = {x0, y0, 0};
        r.imageExtent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), 1};
    };
    if (count == 0 || count > k_max_regions) {
        add(0, 0, static_cast<int>(w), static_cast<int>(h));
    } else {
        for (unsigned i = 0; i < count; ++i)
            add(rects[i].x, rects[i].y, rects[i].width, rects[i].height);
    }
    if (nregions > 0)
        vkCmdCopyImageToBuffer(cb, static_cast<VkImage>(target.vk_image()),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               static_cast<VkBuffer>(rb.buffer), nregions, regions);

    // Back for the next composite, which must not write the image before this read is done
    // (no CPU wait orders them any more).
    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // And the copy's writes made visible to the host that reads the mapping.
    VkBufferMemoryBarrier to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = static_cast<VkBuffer>(rb.buffer);
    to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &to_host, 1, &back);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        return vk_error("read_back_async vkQueueSubmit failed");
    rb.submitted = true;
    return {};
#else
    (void)slot;
    (void)target;
    (void)rects;
    (void)count;
    return lx::not_implemented("lx::gfx::vulkan_compositor::read_back_async");
#endif
}

const unsigned char* lx::gfx::vulkan_compositor::read_back_wait(
    unsigned slot, unsigned long long timeout_ns) const {
#if defined(LUMEN_HAS_VULKAN)
    if (slot >= k_readback_slots)
        return nullptr;
    const readback_slot& rb = readbacks_[slot];
    if (!rb.submitted || !rb.fence || !rb.mapped)
        return nullptr;
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto fence = static_cast<VkFence>(rb.fence);
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, timeout_ns) != VK_SUCCESS)
        return nullptr;
    return rb.mapped;
#else
    (void)slot;
    (void)timeout_ns;
    return nullptr;
#endif
}

void lx::gfx::vulkan_compositor::read_back_free(unsigned slot) {
#if defined(LUMEN_HAS_VULKAN)
    if (slot >= k_readback_slots)
        return;
    readback_slot& rb = readbacks_[slot];
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev) {
        rb = {};
        return;
    }
    if (rb.submitted && rb.fence) {
        auto fence = static_cast<VkFence>(rb.fence);
        (void)vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull);
    }
    if (rb.memory) {
        vkUnmapMemory(dev, static_cast<VkDeviceMemory>(rb.memory));
        vkFreeMemory(dev, static_cast<VkDeviceMemory>(rb.memory), nullptr);
    }
    if (rb.buffer)
        vkDestroyBuffer(dev, static_cast<VkBuffer>(rb.buffer), nullptr);
    if (rb.command_buffer) {
        auto cb = static_cast<VkCommandBuffer>(rb.command_buffer);
        vkFreeCommandBuffers(dev, static_cast<VkCommandPool>(ctx_.command_pool), 1, &cb);
    }
    if (rb.fence)
        vkDestroyFence(dev, static_cast<VkFence>(rb.fence), nullptr);
    rb = {};
#else
    (void)slot;
#endif
}
