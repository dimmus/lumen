module;

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

import lx.foundation;

export module lx.gfx:cpu_renderer;

import :renderer;

export namespace lx::gfx {

/// A CPU-addressable image: a client's shm pixels, or a scanout buffer the CPU may write.
struct pixel_surface {
    unsigned char* pixels = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    lx::fourcc format = 0;
    /// True when writes go to a write-combining mapping (a DRM dumb buffer). Such memory
    /// must be written sequentially and never read back.
    bool write_combining = false;

    [[nodiscard]] bool valid() const {
        return pixels != nullptr && width > 0 && height > 0 && stride >= width * 4u;
    }
};

struct cpu_composite_stats {
    unsigned draws_submitted = 0;
    /// Referenced a texture that was never registered.
    unsigned draws_skipped = 0;
    /// Fully hidden behind an opaque draw above them — never touched.
    unsigned draws_culled = 0;
    /// Source and destination formats have no conversion in common.
    unsigned draws_unsupported = 0;
    unsigned long long bytes_written = 0;
    bool cleared = false;
};

/// Runs a row-band callback across a fixed set of threads plus the calling thread.
///
/// Compositing a frame is a pure scatter over disjoint output rows, so bands never need to
/// synchronise with each other. The pool is persistent because a fullscreen frame at 60 Hz
/// cannot afford thread creation, and it is a private pool rather than the shared executor
/// because the render thread blocks until the frame is complete — borrowing worker slots
/// that also serve latency-sensitive posts would couple the two.
class row_band_pool {
public:
    using band_fn = void (*)(void* user, unsigned band, unsigned band_count);

    row_band_pool() = default;
    ~row_band_pool();

    row_band_pool(const row_band_pool&) = delete;
    row_band_pool& operator=(const row_band_pool&) = delete;

    /// `workers` counts threads *in addition to* the caller. 0 keeps everything inline.
    void resize(unsigned workers);
    [[nodiscard]] unsigned workers() const { return worker_count_; }

    void run(unsigned bands, band_fn fn, void* user);

private:
    void shutdown();
    void worker_main();

    static constexpr unsigned k_max_workers = 15;

    std::thread threads_[k_max_workers]{};
    unsigned worker_count_ = 0;

    std::mutex mutex_{};
    std::condition_variable start_cv_{};
    std::condition_variable done_cv_{};
    band_fn fn_ = nullptr;
    void* user_ = nullptr;
    unsigned band_count_ = 0;
    std::atomic<unsigned> next_band_{0};
    unsigned active_ = 0;
    unsigned long long generation_ = 0;
    bool quit_ = false;
};

/// Software compositor that writes straight into a scanout surface.
///
/// This is the counterpart to `vulkan_compositor` for hosts whose only Vulkan device is a
/// software rasterizer. There, the GPU path costs a staging copy, a tiling copy and a
/// fullscreen fragment shader to produce pixels the CPU already had in cache; this writes
/// the destination exactly once, only over the damaged region, and skips draws that an
/// opaque draw above them completely covers.
///
/// Textures are **borrowed**: `register_texture` stores the caller's pointer. The caller
/// owns the storage and must keep it alive until `forget_texture`, and must not rewrite it
/// while a composite is in flight.
class cpu_compositor {
public:
    static constexpr unsigned k_max_textures = 256;

    cpu_compositor() = default;

    cpu_compositor(const cpu_compositor&) = delete;
    cpu_compositor& operator=(const cpu_compositor&) = delete;

    /// Extra threads used for row bands. 0 composites entirely on the calling thread.
    void set_worker_threads(unsigned workers) { bands_.resize(workers); }
    [[nodiscard]] unsigned worker_threads() const { return bands_.workers(); }

    [[nodiscard]] lx::result<void> register_texture(unsigned texture_id, const pixel_surface& src);
    void forget_texture(unsigned texture_id);
    void forget_all();
    [[nodiscard]] bool has_texture(unsigned texture_id) const { return find(texture_id) >= 0; }

    /// Composites `cmds` (paint order, back to front) into `dst`, restricted to `damage`.
    /// An empty `damage` means the whole surface.
    [[nodiscard]] lx::result<cpu_composite_stats> composite(const pixel_surface& dst,
                                                            lx::color clear,
                                                            const blit_command* cmds,
                                                            unsigned count, lx::rect2i damage);

private:
    struct texture_slot {
        unsigned id = 0;
        pixel_surface surface{};
        bool used = false;
    };

    struct band_job;

    [[nodiscard]] int find(unsigned texture_id) const;
    static void run_band(void* user, unsigned band, unsigned band_count);

    texture_slot textures_[k_max_textures]{};
    row_band_pool bands_{};
};

} // namespace lx::gfx


// ── row_band_pool ────────────────────────────────────────────────────────────

lx::gfx::row_band_pool::~row_band_pool() { shutdown(); }

void lx::gfx::row_band_pool::shutdown() {
    if (worker_count_ == 0)
        return;
    {
        std::lock_guard lock{mutex_};
        quit_ = true;
    }
    start_cv_.notify_all();
    for (unsigned i = 0; i < worker_count_; ++i) {
        if (threads_[i].joinable())
            threads_[i].join();
        threads_[i] = std::thread{};
    }
    worker_count_ = 0;
    quit_ = false;
}

void lx::gfx::row_band_pool::resize(unsigned workers) {
    if (workers > k_max_workers)
        workers = k_max_workers;
    if (workers == worker_count_)
        return;
    shutdown();
    for (unsigned i = 0; i < workers; ++i)
        threads_[i] = std::thread{[this] { worker_main(); }};
    worker_count_ = workers;
}

void lx::gfx::row_band_pool::worker_main() {
    unsigned long long seen = 0;
    for (;;) {
        band_fn fn = nullptr;
        void* user = nullptr;
        unsigned bands = 0;
        {
            std::unique_lock lock{mutex_};
            start_cv_.wait(lock, [this, seen] { return quit_ || generation_ != seen; });
            if (quit_)
                return;
            seen = generation_;
            fn = fn_;
            user = user_;
            bands = band_count_;
        }

        for (;;) {
            const unsigned band = next_band_.fetch_add(1, std::memory_order_relaxed);
            if (band >= bands)
                break;
            fn(user, band, bands);
        }

        {
            std::lock_guard lock{mutex_};
            if (--active_ == 0)
                done_cv_.notify_one();
        }
    }
}

void lx::gfx::row_band_pool::run(unsigned bands, band_fn fn, void* user) {
    if (bands == 0 || !fn)
        return;
    if (worker_count_ == 0 || bands == 1) {
        for (unsigned band = 0; band < bands; ++band)
            fn(user, band, bands);
        return;
    }

    {
        std::lock_guard lock{mutex_};
        fn_ = fn;
        user_ = user;
        band_count_ = bands;
        next_band_.store(0, std::memory_order_relaxed);
        active_ = worker_count_;
        ++generation_;
    }
    start_cv_.notify_all();

    // The caller is a band worker too — otherwise the render thread would idle through the
    // frame it is waiting on.
    for (;;) {
        const unsigned band = next_band_.fetch_add(1, std::memory_order_relaxed);
        if (band >= bands)
            break;
        fn(user, band, bands);
    }

    std::unique_lock lock{mutex_};
    done_cv_.wait(lock, [this] { return active_ == 0; });
}

// ── pixel format helpers ─────────────────────────────────────────────────────

namespace lx::gfx::detail {

/// How to get a source pixel into the destination's byte order. Every format handled here
/// is 32-bpp with alpha (or the ignored X channel) in memory byte 3, so a destination
/// alpha fill is always the top byte of the little-endian word.
enum class convert_op : unsigned char { copy, swap_rb, unsupported };

[[nodiscard]] inline bool is_bgra_order(lx::fourcc f) {
    return f == static_cast<lx::fourcc>(lx::pixel_format::argb8888) ||
           f == static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
}

[[nodiscard]] inline bool is_rgba_order(lx::fourcc f) {
    return f == static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
}

[[nodiscard]] inline bool has_alpha_channel(lx::fourcc f) {
    return f == static_cast<lx::fourcc>(lx::pixel_format::argb8888) ||
           f == static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
}

[[nodiscard]] inline convert_op pick_convert(lx::fourcc src, lx::fourcc dst) {
    if (src == dst)
        return convert_op::copy;
    if (is_bgra_order(src) && is_bgra_order(dst))
        return convert_op::copy;
    if (is_rgba_order(src) && is_rgba_order(dst))
        return convert_op::copy;
    if ((is_bgra_order(src) && is_rgba_order(dst)) || (is_rgba_order(src) && is_bgra_order(dst)))
        return convert_op::swap_rb;
    return convert_op::unsupported;
}

[[nodiscard]] inline std::uint32_t swap_rb_word(std::uint32_t p) {
    return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

/// Premultiplied source over destination. `src` and `dst` are already in the destination's
/// channel order, so the arithmetic is per-byte and order-agnostic.
[[nodiscard]] inline std::uint32_t blend_over(std::uint32_t src, std::uint32_t dst,
                                              unsigned opacity_255) {
    if (opacity_255 < 255u) {
        const std::uint32_t lo = ((src & 0x00FF00FFu) * opacity_255 >> 8) & 0x00FF00FFu;
        const std::uint32_t hi = ((src >> 8 & 0x00FF00FFu) * opacity_255) & 0xFF00FF00u;
        src = lo | hi;
    }
    const unsigned inv = 255u - (src >> 24);
    if (inv == 0)
        return src;
    const std::uint32_t lo = ((dst & 0x00FF00FFu) * inv >> 8) & 0x00FF00FFu;
    const std::uint32_t hi = ((dst >> 8 & 0x00FF00FFu) * inv) & 0xFF00FF00u;
    return src + (lo | hi);
}

[[nodiscard]] inline int clamp_low(int v, int lo) { return v < lo ? lo : v; }
[[nodiscard]] inline int clamp_high(int v, int hi) { return v > hi ? hi : v; }

/// Intersection of two rects; the result may be empty.
[[nodiscard]] inline lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = clamp_low(a.x, b.x);
    const int y0 = clamp_low(a.y, b.y);
    const int x1 = clamp_high(a.x + a.width, b.x + b.width);
    const int y1 = clamp_high(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

[[nodiscard]] inline bool covers(lx::rect2i outer, lx::rect2i inner) {
    return outer.x <= inner.x && outer.y <= inner.y &&
           outer.x + outer.width >= inner.x + inner.width &&
           outer.y + outer.height >= inner.y + inner.height;
}

/// Source sub-rectangle to sample, clamped into the texture. An empty `src` means the
/// whole texture, which is both a plain surface and a `wp_viewport` with no source set.
[[nodiscard]] inline lx::rect2i effective_src(lx::rect2i src, unsigned tex_w, unsigned tex_h) {
    const lx::rect2i whole{0, 0, static_cast<int>(tex_w), static_cast<int>(tex_h)};
    if (src.width <= 0 || src.height <= 0)
        return whole;
    return intersect(src, whole);
}

/// `dst` clipped to the draw's own clip rect. An empty clip means unclipped.
[[nodiscard]] inline lx::rect2i clipped_dst(lx::rect2i dst, lx::rect2i clip) {
    if (clip.width <= 0 || clip.height <= 0)
        return dst;
    return intersect(dst, clip);
}

/// True when the tint is the identity multiplier and can be skipped entirely.
[[nodiscard]] inline bool tint_is_identity(const lx::color& t) {
    return t.r >= 1.f && t.g >= 1.f && t.b >= 1.f && t.a >= 1.f;
}

[[nodiscard]] inline unsigned to_255(float v) {
    const float scaled = v * 255.f + 0.5f;
    return static_cast<unsigned>(scaled < 0.f ? 0.f : (scaled > 255.f ? 255.f : scaled));
}

/// Per-channel multiply of a premultiplied pixel. `r_255`/`b_255` are passed in the word's
/// own channel order, so the caller resolves BGRA vs RGBA once per draw rather than per
/// pixel.
[[nodiscard]] inline std::uint32_t apply_tint(std::uint32_t p, unsigned c0_255, unsigned g_255,
                                              unsigned c2_255, unsigned a_255) {
    const unsigned c0 = ((p & 0xFFu) * c0_255) >> 8;
    const unsigned g = (((p >> 8) & 0xFFu) * g_255) >> 8;
    const unsigned c2 = (((p >> 16) & 0xFFu) * c2_255) >> 8;
    const unsigned a = (((p >> 24) & 0xFFu) * a_255) >> 8;
    return (a << 24) | (c2 << 16) | (g << 8) | c0;
}

} // namespace lx::gfx::detail

// ── cpu_compositor ───────────────────────────────────────────────────────────

int lx::gfx::cpu_compositor::find(unsigned texture_id) const {
    if (texture_id == 0)
        return -1;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used && textures_[i].id == texture_id)
            return static_cast<int>(i);
    }
    return -1;
}

lx::result<void> lx::gfx::cpu_compositor::register_texture(unsigned texture_id,
                                                           const pixel_surface& src) {
    if (texture_id == 0 || !src.valid()) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "cpu_compositor::register_texture: invalid surface");
    }
    if (const int existing = find(texture_id); existing >= 0) {
        textures_[static_cast<unsigned>(existing)].surface = src;
        return {};
    }
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used)
            continue;
        textures_[i].used = true;
        textures_[i].id = texture_id;
        textures_[i].surface = src;
        return {};
    }
    return lx::make_error(lx::error_domain::invalid_argument, 0,
                          "cpu_compositor: texture table full");
}

void lx::gfx::cpu_compositor::forget_texture(unsigned texture_id) {
    if (const int index = find(texture_id); index >= 0)
        textures_[static_cast<unsigned>(index)] = {};
}

void lx::gfx::cpu_compositor::forget_all() {
    for (auto& slot : textures_)
        slot = {};
}

struct lx::gfx::cpu_compositor::band_job {
    const cpu_compositor* self = nullptr;
    const pixel_surface* dst = nullptr;
    const blit_command* cmds = nullptr;
    unsigned first_cmd = 0;
    unsigned count = 0;
    lx::rect2i clip{};
    std::uint32_t clear_word = 0;
    bool do_clear = false;
    std::atomic<unsigned long long> bytes{0};
};

void lx::gfx::cpu_compositor::run_band(void* user, unsigned band, unsigned band_count) {
    using namespace lx::gfx::detail;
    auto& job = *static_cast<band_job*>(user);
    const pixel_surface& dst = *job.dst;

    const int rows = job.clip.height;
    const int first_row = job.clip.y + static_cast<int>(static_cast<long long>(rows) * band /
                                                        band_count);
    const int last_row = job.clip.y + static_cast<int>(static_cast<long long>(rows) *
                                                       (band + 1) / band_count);
    if (first_row >= last_row)
        return;

    const lx::rect2i band_rect{job.clip.x, first_row, job.clip.width, last_row - first_row};
    unsigned long long bytes = 0;

    if (job.do_clear) {
        for (int y = first_row; y < last_row; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(dst.pixels +
                                                         static_cast<std::size_t>(y) * dst.stride) +
                        job.clip.x;
            for (int x = 0; x < job.clip.width; ++x)
                row[x] = job.clear_word;
        }
        bytes += static_cast<unsigned long long>(band_rect.height) * band_rect.width * 4ull;
    }

    for (unsigned i = job.first_cmd; i < job.count; ++i) {
        const auto& cmd = job.cmds[i];
        const int index = job.self->find(cmd.texture_id);
        if (index < 0)
            continue;
        const pixel_surface& src = job.self->textures_[static_cast<unsigned>(index)].surface;
        if (!src.valid() || cmd.dst.width <= 0 || cmd.dst.height <= 0)
            continue;

        const convert_op conv = pick_convert(src.format, dst.format);
        if (conv == convert_op::unsupported)
            continue;

        // Rotated and flipped buffers need a transposed walk this blitter does not do;
        // composite() counts them as unsupported rather than drawing them upright.
        if (cmd.buffer_xform != lx::buffer_transform::normal)
            continue;

        const lx::rect2i area = intersect(clipped_dst(cmd.dst, cmd.clip), band_rect);
        if (area.width <= 0 || area.height <= 0)
            continue;

        const lx::rect2i sub = effective_src(cmd.src, src.width, src.height);
        if (sub.width <= 0 || sub.height <= 0)
            continue;

        // Tint alpha folds into opacity; the color channels stay a per-pixel multiply.
        const float effective_opacity = cmd.opacity * cmd.tint.a;
        const unsigned opacity_255 =
            effective_opacity >= 1.f ? 255u
                                     : (effective_opacity <= 0.f ? 0u : to_255(effective_opacity));
        if (opacity_255 == 0)
            continue;

        const bool tinted = !(cmd.tint.r >= 1.f && cmd.tint.g >= 1.f && cmd.tint.b >= 1.f);
        // Destination word order decides which channel the tint's red lands in.
        const unsigned tint_c0 = to_255(is_rgba_order(dst.format) ? cmd.tint.r : cmd.tint.b);
        const unsigned tint_g = to_255(cmd.tint.g);
        const unsigned tint_c2 = to_255(is_rgba_order(dst.format) ? cmd.tint.b : cmd.tint.r);

        // Alpha only has to be respected when the source carries it, the draw asks for
        // blending, and the draw is not fully opaque. Worth avoiding for more than the
        // arithmetic: blending reads the destination back, and a scanout mapping is
        // write-combining — reads from it are uncached and an order of magnitude slower
        // than the writes. Opaque draws (and the occlusion cull above) never read it.
        const bool needs_blend = cmd.blend != lx::blend_mode::opaque &&
                                 (has_alpha_channel(src.format) || opacity_255 < 255u);
        const bool fill_alpha = has_alpha_channel(dst.format) && !has_alpha_channel(src.format);

        // 16.16 fixed point: one multiply per row/column setup, one add per pixel step.
        // The ratio is the *source sub-rect* against dst, which is what makes viewporter
        // cropping and fractional scale fall out of the same walk — a cropped source over
        // the same dst simply steps faster.
        const std::uint32_t x_step =
            static_cast<std::uint32_t>((static_cast<unsigned long long>(sub.width) << 16) /
                                       static_cast<unsigned>(cmd.dst.width));
        const std::uint32_t y_step =
            static_cast<std::uint32_t>((static_cast<unsigned long long>(sub.height) << 16) /
                                       static_cast<unsigned>(cmd.dst.height));
        const bool unscaled = sub.width == cmd.dst.width && sub.height == cmd.dst.height;

        const std::uint32_t x_start =
            static_cast<std::uint32_t>(area.x - cmd.dst.x) * x_step;
        const std::uint32_t y_start =
            static_cast<std::uint32_t>(area.y - cmd.dst.y) * y_step;

        // The whole point of this backend: an unscaled opaque draw in a compatible format
        // is a row memcpy — the destination is written once and nothing is read back.
        const bool memcpy_rows =
            unscaled && !needs_blend && !fill_alpha && !tinted && conv == convert_op::copy;

        // Sub-rect origin is added on top of the scaled offset, so cropping is a shift of
        // where sampling starts rather than a second pass over the source.
        const unsigned sub_x = static_cast<unsigned>(sub.x);
        const unsigned sub_y = static_cast<unsigned>(sub.y);
        const unsigned sub_x_end = sub_x + static_cast<unsigned>(sub.width);
        const unsigned sub_y_end = sub_y + static_cast<unsigned>(sub.height);

        for (int row = 0; row < area.height; ++row) {
            const unsigned sy =
                sub_y + (unscaled ? static_cast<unsigned>(area.y - cmd.dst.y + row)
                                  : ((y_start + static_cast<std::uint32_t>(row) * y_step) >> 16));
            if (sy >= sub_y_end || sy >= src.height)
                break;

            const auto* src_row = reinterpret_cast<const std::uint32_t*>(
                src.pixels + static_cast<std::size_t>(sy) * src.stride);
            auto* dst_row = reinterpret_cast<std::uint32_t*>(
                                dst.pixels +
                                static_cast<std::size_t>(area.y + row) * dst.stride) +
                            area.x;

            if (memcpy_rows) {
                std::memcpy(dst_row, src_row + sub_x + (area.x - cmd.dst.x),
                            static_cast<std::size_t>(area.width) * 4u);
                continue;
            }

            std::uint32_t sx = x_start;
            for (int x = 0; x < area.width; ++x) {
                const unsigned src_x =
                    sub_x + (unscaled ? static_cast<unsigned>(area.x - cmd.dst.x + x)
                                      : (sx >> 16));
                sx += x_step;
                if (src_x >= sub_x_end || src_x >= src.width)
                    break;

                std::uint32_t pixel = src_row[src_x];
                if (conv == convert_op::swap_rb)
                    pixel = swap_rb_word(pixel);
                if (fill_alpha)
                    pixel |= 0xFF000000u;
                if (tinted)
                    pixel = apply_tint(pixel, tint_c0, tint_g, tint_c2, 255u);

                dst_row[x] = needs_blend ? blend_over(pixel, dst_row[x], opacity_255) : pixel;
            }
        }
        bytes += static_cast<unsigned long long>(area.height) * area.width * 4ull;
    }

    job.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

lx::result<lx::gfx::cpu_composite_stats> lx::gfx::cpu_compositor::composite(
    const pixel_surface& dst, lx::color clear, const blit_command* cmds, unsigned count,
    lx::rect2i damage) {
    using namespace lx::gfx::detail;

    if (!dst.valid()) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "cpu_compositor::composite: invalid destination");
    }
    if (!is_bgra_order(dst.format) && !is_rgba_order(dst.format)) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "cpu_compositor::composite: unsupported destination format");
    }

    const lx::rect2i full{0, 0, static_cast<int>(dst.width), static_cast<int>(dst.height)};
    lx::rect2i clip = (damage.width > 0 && damage.height > 0) ? intersect(damage, full) : full;

    cpu_composite_stats stats{};
    if (clip.width <= 0 || clip.height <= 0)
        return stats;

    // Occlusion cull: find the topmost draw that is opaque and covers the whole clip.
    // Everything below it is invisible, and the clear underneath is pure waste — for a
    // fullscreen client this turns the frame into a single pass over the destination.
    unsigned first_cmd = 0;
    bool needs_clear = true;
    for (unsigned i = count; i-- > 0 && cmds;) {
        const auto& cmd = cmds[i];
        const int index = find(cmd.texture_id);
        if (index < 0)
            continue;
        const pixel_surface& src = textures_[static_cast<unsigned>(index)].surface;
        if (!src.valid() || pick_convert(src.format, dst.format) == convert_op::unsupported)
            continue;
        if (cmd.buffer_xform != lx::buffer_transform::normal)
            continue;
        // A tint that darkens or fades still covers, but only if it is fully opaque and
        // the draw is actually drawn — otherwise what is underneath shows through.
        const bool opaque = cmd.opacity >= 1.f && cmd.tint.a >= 1.f &&
                            (cmd.blend == lx::blend_mode::opaque || !has_alpha_channel(src.format));
        // The clip, not just dst, bounds what this draw actually covers.
        if (opaque && covers(clipped_dst(cmd.dst, cmd.clip), clip)) {
            first_cmd = i;
            needs_clear = false;
            break;
        }
    }

    for (unsigned i = 0; i < count && cmds; ++i) {
        const auto& cmd = cmds[i];
        const int index = find(cmd.texture_id);
        if (index < 0) {
            ++stats.draws_skipped;
            continue;
        }
        if (i < first_cmd) {
            ++stats.draws_culled;
            continue;
        }
        const pixel_surface& src = textures_[static_cast<unsigned>(index)].surface;
        if (pick_convert(src.format, dst.format) == convert_op::unsupported) {
            ++stats.draws_unsupported;
            continue;
        }
        // Say so rather than silently drawing a rotated buffer upright — a wrong frame is
        // worse than a missing one, because nothing upstream can tell it happened.
        if (cmd.buffer_xform != lx::buffer_transform::normal) {
            ++stats.draws_unsupported;
            continue;
        }
        ++stats.draws_submitted;
    }

    band_job job{};
    job.self = this;
    job.dst = &dst;
    job.cmds = cmds;
    job.first_cmd = first_cmd;
    job.count = cmds ? count : 0;
    job.clip = clip;
    job.do_clear = needs_clear;
    job.clear_word = [&] {
        const auto to_byte = [](float v) {
            const float scaled = v * 255.f + 0.5f;
            return static_cast<std::uint32_t>(scaled < 0.f ? 0.f : (scaled > 255.f ? 255.f
                                                                                   : scaled));
        };
        const std::uint32_t r = to_byte(clear.r);
        const std::uint32_t g = to_byte(clear.g);
        const std::uint32_t b = to_byte(clear.b);
        const std::uint32_t a = to_byte(clear.a);
        return is_rgba_order(dst.format) ? (a << 24) | (b << 16) | (g << 8) | r
                                         : (a << 24) | (r << 16) | (g << 8) | b;
    }();
    stats.cleared = needs_clear;

    // One band per worker, but never so few rows per band that the dispatch costs more
    // than the work.
    static constexpr int k_min_rows_per_band = 64;
    unsigned bands = bands_.workers() + 1u;
    const unsigned by_rows =
        static_cast<unsigned>(clip.height / k_min_rows_per_band) + 1u;
    if (by_rows < bands)
        bands = by_rows;

    bands_.run(bands, &cpu_compositor::run_band, &job);

    stats.bytes_written = job.bytes.load(std::memory_order_relaxed);
    return stats;
}
