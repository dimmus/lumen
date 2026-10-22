module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#if defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#define LUMEN_SHELL_FRACTIONAL 1
#endif
#endif
#include <stdlib.h>

import lx.foundation;
import lx.text;
import lx.trace;

export module lx.shell:shm_canvas;

import std;

export namespace lx::shell {

// ── shm canvas: what the panel and the launcher draw with ─────────────────────
//
// Two XRGB8888 buffers in one wl_shm_pool on a sealed memfd, with busy tracking (a buffer
// the compositor holds is never drawn into) and frame-callback pacing (at most one commit
// per frame callback). Painting helpers: solid and rounded rects, and one line of text
// through lx.text, clipped to its own box. No Wayland type is exported.
//
// Callers lay out and paint in logical (surface) coordinates. With fractional scale
// attached (`attach_scale`) the buffers are round(size × scale) pixels — the compositor's
// preferred scale from wp_fractional_scale_v1 — every rect is rounded edge by edge onto
// them, text is shaped at the scaled font size, and a wp_viewport destination shows the
// buffer at the logical size: sharp at 1.25, as GTK 4 and foot are.

inline constexpr std::uint32_t k_canvas_bg = 0xFF1E1E22u;

class shm_canvas {
public:
    shm_canvas() = default;
    ~shm_canvas();
    shm_canvas(const shm_canvas&) = delete;
    shm_canvas& operator=(const shm_canvas&) = delete;

    /// Called when a buffer comes back or a frame callback fires: whoever waits to draw
    /// may now.
    using wake_fn = void (*)(void* user);
    void set_wake(wake_fn fn, void* user) {
        wake_ = fn;
        wake_user_ = user;
    }

    /// (Re)allocates both buffers for a `width`×`height` (logical) surface at the current
    /// scale (`shm` is a wl_shm*).
    [[nodiscard]] bool allocate(void* shm, int width, int height, const char* name);
    /// Frees the buffers (not the scale objects: the surface keeps them).
    void free();
    [[nodiscard]] bool allocated() const { return pool_ != nullptr; }
    /// Logical size.
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

    /// Fractional scale for `surface` (wl_surface*): `viewporter` is a wp_viewporter*,
    /// `fractional` a wp_fractional_scale_manager_v1*; either null keeps scale 1. Once per
    /// surface, before its first buffer; `release_scale` before the surface is destroyed.
    void attach_scale(void* viewporter, void* fractional, void* surface);
    void release_scale();
    /// The preferred scale changed since the last call: reallocate at the same logical size
    /// and repaint everything.
    [[nodiscard]] bool take_rescale() {
        const bool r = rescale_;
        rescale_ = false;
        return r;
    }
    /// wp_fractional_scale_v1.preferred_scale, in 120ths (120 without it).
    [[nodiscard]] unsigned scale120() const { return scale120_; }
    /// Bytes held by the pool (0 when freed).
    [[nodiscard]] std::size_t bytes() const { return pool_size_; }

    /// A buffer that may be drawn into, preferring the one committed last (it already
    /// shows most of the surface); -1 when the compositor holds both.
    [[nodiscard]] int acquire() const;
    [[nodiscard]] std::uint32_t* pixels(int buffer) const { return buffers_[buffer].pixels; }
    /// Attaches `buffer`, damages `rects`, asks for a frame callback and commits.
    void commit(void* surface, int buffer, const lx::rect2i* rects, unsigned n);
    /// A frame callback is outstanding: the next draw waits for it.
    [[nodiscard]] bool frame_pending() const { return frame_ != nullptr; }

    // ── Painting into a buffer ──
    void fill(int buffer, lx::rect2i r, std::uint32_t color);
    /// A rounded rect over `under`: corners blend by analytic pixel coverage.
    void fill_rounded(int buffer, lx::rect2i r, std::uint32_t color, std::uint32_t under,
                      int radius = 4);
    /// Draws a premultiplied ARGB8888 image of `size`² pixels over the buffer, its top-left
    /// at the pixel corner of `logical`'s origin (sources over: src + dst × (1 − αsrc)).
    /// `size` should be `pixel_rect(logical)`'s width — the caller resolves the icon at the
    /// pixel size it lands on, so nothing is scaled here.
    void blit(int buffer, lx::rect2i logical, const std::uint32_t* pixels, int size);
    /// A logical rect on this canvas's pixel grid (the scale applied).
    [[nodiscard]] lx::rect2i pixel_rect(lx::rect2i logical) const { return px(logical); }
    enum class align : unsigned char { left, right };
    /// One line of `text` in the box, vertically centred, ellipsized to fit. False when
    /// text cannot be shaped (warned once).
    bool text(int buffer, lx::rect2i box, const char* utf8, lx::text::raster_color color,
              lx::text::font_desc font, align a = align::left);
    /// Width `text` would take (logical), or -1.
    [[nodiscard]] int text_width(const char* utf8, lx::text::font_desc font);

    struct events;

private:
    friend struct events;
    struct buffer {
        void* wl = nullptr; // wl_buffer*
        std::uint32_t* pixels = nullptr;
        bool busy = false;
    };

    buffer buffers_[2]{};
    int last_ = -1;
    void* pool_ = nullptr; // wl_shm_pool*
    void* pool_data_ = nullptr;
    std::size_t pool_size_ = 0;
    void* frame_ = nullptr; // wl_callback*
    int width_ = 0;  // logical
    int height_ = 0;
    int pw_ = 0;     // pixels
    int ph_ = 0;
    void* viewport_ = nullptr;   // wp_viewport*
    void* fractional_ = nullptr; // wp_fractional_scale_v1*
    unsigned scale120_ = 120;
    bool rescale_ = false;
    bool viewport_dirty_ = false;
    [[nodiscard]] double scale() const { return scale120_ / 120.0; }
    /// A logical rect on the pixel grid, edge by edge (adjacent rects share edges).
    [[nodiscard]] lx::rect2i px(lx::rect2i r) const;
    bool font_warned_ = false;
    wake_fn wake_ = nullptr;
    void* wake_user_ = nullptr;
    lx::text::shaped_line line_{};
};

} // namespace lx::shell

namespace {

constexpr const char* k_canvas_category = "shell.panel";

[[nodiscard]] std::uint32_t mix(std::uint32_t over, std::uint32_t under, unsigned a /*0..255*/) {
    const unsigned ia = 255u - a;
    const unsigned r = (((over >> 16) & 0xFFu) * a + ((under >> 16) & 0xFFu) * ia + 127u) / 255u;
    const unsigned g = (((over >> 8) & 0xFFu) * a + ((under >> 8) & 0xFFu) * ia + 127u) / 255u;
    const unsigned b = ((over & 0xFFu) * a + (under & 0xFFu) * ia + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

} // namespace

lx::rect2i lx::shell::shm_canvas::px(lx::rect2i r) const {
    if (scale120_ == 120)
        return r;
    const double s = scale();
    const long x0 = std::lround(r.x * s), y0 = std::lround(r.y * s);
    const long x1 = std::lround((static_cast<double>(r.x) + r.width) * s);
    const long y1 = std::lround((static_cast<double>(r.y) + r.height) * s);
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

void lx::shell::shm_canvas::fill(int bi, lx::rect2i logical, std::uint32_t c) {
    std::uint32_t* px = buffers_[bi].pixels;
    if (!px)
        return;
    const lx::rect2i r = this->px(logical);
    // Clipped to the buffer.
    const int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    const int x1 = r.x + r.width > pw_ ? pw_ : r.x + r.width;
    const int y1 = r.y + r.height > ph_ ? ph_ : r.y + r.height;
    for (int y = y0; y < y1; ++y) {
        std::uint32_t* row = px + static_cast<std::ptrdiff_t>(y) * pw_;
        for (int x = x0; x < x1; ++x)
            row[x] = c;
    }
}

void lx::shell::shm_canvas::fill_rounded(int bi, lx::rect2i logical, std::uint32_t c,
                                         std::uint32_t under, int logical_radius) {
    fill(bi, logical, c);
    std::uint32_t* px = buffers_[bi].pixels;
    const lx::rect2i r = this->px(logical);
    const int radius = static_cast<int>(std::lround(logical_radius * scale()));
    if (!px || r.x < 0 || r.y < 0 || r.x + r.width > pw_ || r.y + r.height > ph_)
        return;
    const int rad = r.width >= 2 * radius && r.height >= 2 * radius ? radius : 0;
    for (int cy = 0; cy < rad; ++cy) {
        for (int cx = 0; cx < rad; ++cx) {
            // Distance from the corner circle's centre to this pixel's centre.
            const float dx = static_cast<float>(rad) - (static_cast<float>(cx) + 0.5f);
            const float dy = static_cast<float>(rad) - (static_cast<float>(cy) + 0.5f);
            float cov = static_cast<float>(rad) - std::sqrt(dx * dx + dy * dy) + 0.5f;
            cov = cov < 0.f ? 0.f : (cov > 1.f ? 1.f : cov);
            const unsigned a = static_cast<unsigned>(cov * 255.f + 0.5f);
            const std::uint32_t v = mix(c, under, a);
            const int xl = r.x + cx, xr = r.x + r.width - 1 - cx;
            const int yt = r.y + cy, yb = r.y + r.height - 1 - cy;
            px[static_cast<std::ptrdiff_t>(yt) * pw_ + xl] = v;
            px[static_cast<std::ptrdiff_t>(yt) * pw_ + xr] = v;
            px[static_cast<std::ptrdiff_t>(yb) * pw_ + xl] = v;
            px[static_cast<std::ptrdiff_t>(yb) * pw_ + xr] = v;
        }
    }
}

void lx::shell::shm_canvas::blit(int bi, lx::rect2i logical, const std::uint32_t* src, int size) {
    std::uint32_t* dst = buffers_[bi].pixels;
    if (!dst || !src || size <= 0)
        return;
    const lx::rect2i r = this->px(logical);
    for (int y = 0; y < size; ++y) {
        const int ty = r.y + y;
        if (ty < 0 || ty >= ph_)
            continue;
        std::uint32_t* row = dst + static_cast<std::ptrdiff_t>(ty) * pw_;
        const std::uint32_t* in = src + static_cast<std::ptrdiff_t>(y) * size;
        for (int x = 0; x < size; ++x) {
            const int tx = r.x + x;
            if (tx < 0 || tx >= pw_)
                continue;
            const std::uint32_t s = in[x];
            const unsigned a = s >> 24;
            if (a == 0)
                continue;
            if (a == 255) {
                row[tx] = s;
                continue;
            }
            const std::uint32_t d = row[tx];
            const unsigned ia = 255u - a;
            const unsigned rr = ((s >> 16) & 0xFFu) + (((d >> 16) & 0xFFu) * ia + 127u) / 255u;
            const unsigned gg = ((s >> 8) & 0xFFu) + (((d >> 8) & 0xFFu) * ia + 127u) / 255u;
            const unsigned bb = (s & 0xFFu) + ((d & 0xFFu) * ia + 127u) / 255u;
            row[tx] = 0xFF000000u | ((rr > 255 ? 255 : rr) << 16) | ((gg > 255 ? 255 : gg) << 8) |
                      (bb > 255 ? 255 : bb);
        }
    }
}

bool lx::shell::shm_canvas::text(int bi, lx::rect2i logical_box, const char* utf8,
                                 lx::text::raster_color color, lx::text::font_desc font,
                                 align a) {
    std::uint32_t* px = buffers_[bi].pixels;
    const lx::rect2i box = this->px(logical_box);
    if (!px || box.width <= 0 || box.height <= 0 || box.x < 0 || box.y < 0 ||
        box.x + box.width > pw_ || box.y + box.height > ph_)
        return false;
    // Shaped at the output's pixel density: the glyphs are rasterized for the pixels they
    // land on, never scaled afterwards.
    font.size_pt = static_cast<float>(font.size_pt * scale());
    if (auto shaped = lx::text::shape_line(utf8, font, line_); !shaped) {
        if (!font_warned_) {
            font_warned_ = true;
            lx::trace::logger::global().log_error(shaped.get_error(), k_canvas_category);
            lx::trace::logger::global().log(lx::trace::level::warn, k_canvas_category,
                                            "no text rendering: labels are not drawn");
        }
        return false;
    }
    const lx::text::line_fit fit = lx::text::fit_line(line_, box.width);
    const int x = a == align::right ? box.width - fit.width_px : 0;
    // The target is the label's own box, so nothing can spill into a neighbour.
    lx::text::raster_target target{px + static_cast<std::ptrdiff_t>(box.y) * pw_ + box.x,
                                   box.width, box.height, pw_};
    const int baseline = (box.height - (line_.ascent_px + line_.descent_px)) / 2 + line_.ascent_px;
    (void)lx::text::render_line(line_, fit, color, target, {x, baseline});
    return true;
}

int lx::shell::shm_canvas::text_width(const char* utf8, lx::text::font_desc font) {
    if (!lx::text::shape_line(utf8, font, line_))
        return -1;
    return line_.width_px;
}

int lx::shell::shm_canvas::acquire() const {
    if (!pool_)
        return -1;
    if (last_ >= 0 && !buffers_[last_].busy)
        return last_;
    if (!buffers_[0].busy)
        return 0;
    if (!buffers_[1].busy)
        return 1;
    return -1;
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::shm_canvas::events {
    static void frame_done(void* data, wl_callback* cb, uint32_t) {
        auto& c = *static_cast<shm_canvas*>(data);
        wl_callback_destroy(cb);
        if (c.frame_ == cb)
            c.frame_ = nullptr;
        if (c.wake_)
            c.wake_(c.wake_user_);
    }
    static void release(void* data, wl_buffer* wb) {
        auto& c = *static_cast<shm_canvas*>(data);
        for (auto& b : c.buffers_)
            if (b.wl == wb)
                b.busy = false;
        if (c.wake_)
            c.wake_(c.wake_user_);
    }
    static void preferred(shm_canvas& c, unsigned scale120) {
        if (scale120 == 0 || scale120 == c.scale120_)
            return;
        c.scale120_ = scale120;
        c.rescale_ = true;
        if (c.wake_)
            c.wake_(c.wake_user_);
    }
};

namespace {

const wl_callback_listener k_canvas_frame_listener = {.done = lx::shell::shm_canvas::events::frame_done};
const wl_buffer_listener k_canvas_buffer_listener = {.release = lx::shell::shm_canvas::events::release};

} // namespace

bool lx::shell::shm_canvas::allocate(void* shm, int logical_w, int logical_h, const char* name) {
    free();
    if (!shm || logical_w <= 0 || logical_h <= 0)
        return false;
    // round(size × scale): the compositor draws a viewport destination of the logical size
    // over exactly this many pixels, 1:1.
    const int width = static_cast<int>(std::lround(logical_w * scale()));
    const int height = static_cast<int>(std::lround(logical_h * scale()));
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    const std::size_t one = stride * static_cast<std::size_t>(height);
    const std::size_t size = one * 2u;
    const int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return false;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return false;
    }
    // The compositor maps this pool too: it must not be able to shrink under it.
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    auto* pool = wl_shm_create_pool(static_cast<wl_shm*>(shm), fd, static_cast<int32_t>(size));
    close(fd);
    pool_ = pool;
    pool_data_ = data;
    pool_size_ = size;
    width_ = logical_w;
    height_ = logical_h;
    pw_ = width;
    ph_ = height;
    viewport_dirty_ = true;
    for (int i = 0; i < 2; ++i) {
        buffer& b = buffers_[i];
        auto* wb = wl_shm_pool_create_buffer(pool, static_cast<int32_t>(one) * i, width, height,
                                             static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(wb, &k_canvas_buffer_listener, this);
        b.wl = wb;
        b.pixels = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(data) + one * i);
        b.busy = false;
    }
    last_ = -1;
    return true;
}

void lx::shell::shm_canvas::free() {
    for (auto& b : buffers_) {
        if (b.wl)
            wl_buffer_destroy(static_cast<wl_buffer*>(b.wl));
        b = {};
    }
    if (pool_)
        wl_shm_pool_destroy(static_cast<wl_shm_pool*>(pool_));
    pool_ = nullptr;
    if (pool_data_)
        munmap(pool_data_, pool_size_);
    pool_data_ = nullptr;
    pool_size_ = 0;
    last_ = -1;
    width_ = height_ = pw_ = ph_ = 0;
    if (frame_)
        wl_callback_destroy(static_cast<wl_callback*>(frame_));
    frame_ = nullptr;
}

void lx::shell::shm_canvas::commit(void* surface_ptr, int bi, const lx::rect2i* rects, unsigned n) {
    auto* surface = static_cast<wl_surface*>(surface_ptr);
#if defined(LUMEN_SHELL_FRACTIONAL)
    if (viewport_ && viewport_dirty_) {
        // Sticky: once per allocation. At 120 it is the buffer's own size (a no-op).
        wp_viewport_set_destination(static_cast<wp_viewport*>(viewport_), width_, height_);
        viewport_dirty_ = false;
    }
#endif
    wl_surface_attach(surface, static_cast<wl_buffer*>(buffers_[bi].wl), 0, 0);
    // Damage in buffer pixels, rounded out (antialiased edges reach the next pixel).
    const double s = scale();
    for (unsigned i = 0; i < n; ++i) {
        const lx::rect2i& r = rects[i];
        if (scale120_ == 120) {
            wl_surface_damage_buffer(surface, r.x, r.y, r.width, r.height);
            continue;
        }
        const int x0 = static_cast<int>(std::floor(r.x * s)) - 1;
        const int y0 = static_cast<int>(std::floor(r.y * s)) - 1;
        const int x1 = static_cast<int>(std::ceil((static_cast<double>(r.x) + r.width) * s)) + 1;
        const int y1 = static_cast<int>(std::ceil((static_cast<double>(r.y) + r.height) * s)) + 1;
        wl_surface_damage_buffer(surface, x0, y0, x1 - x0, y1 - y0);
    }
    if (!frame_) {
        auto* cb = wl_surface_frame(surface);
        wl_callback_add_listener(cb, &k_canvas_frame_listener, this);
        frame_ = cb;
    }
    wl_surface_commit(surface);
    buffers_[bi].busy = true;
    last_ = bi;
}

#if defined(LUMEN_SHELL_FRACTIONAL)
namespace {
void on_preferred_scale(void* data, wp_fractional_scale_v1*, uint32_t scale120) {
    auto& c = *static_cast<lx::shell::shm_canvas*>(data);
    lx::shell::shm_canvas::events::preferred(c, scale120);
}
const wp_fractional_scale_v1_listener k_canvas_fractional_listener = {
    .preferred_scale = on_preferred_scale};
} // namespace
#endif

void lx::shell::shm_canvas::attach_scale(void* viewporter, void* fractional, void* surface) {
    release_scale();
#if defined(LUMEN_SHELL_FRACTIONAL)
    if (!viewporter || !fractional || !surface)
        return;
    viewport_ = wp_viewporter_get_viewport(static_cast<wp_viewporter*>(viewporter),
                                           static_cast<wl_surface*>(surface));
    auto* f = wp_fractional_scale_manager_v1_get_fractional_scale(
        static_cast<wp_fractional_scale_manager_v1*>(fractional), static_cast<wl_surface*>(surface));
    wp_fractional_scale_v1_add_listener(f, &k_canvas_fractional_listener, this);
    fractional_ = f;
    viewport_dirty_ = true;
#else
    (void)viewporter;
    (void)fractional;
    (void)surface;
#endif
}

void lx::shell::shm_canvas::release_scale() {
#if defined(LUMEN_SHELL_FRACTIONAL)
    if (fractional_)
        wp_fractional_scale_v1_destroy(static_cast<wp_fractional_scale_v1*>(fractional_));
    if (viewport_)
        wp_viewport_destroy(static_cast<wp_viewport*>(viewport_));
#endif
    fractional_ = nullptr;
    viewport_ = nullptr;
    // The next surface starts from 1 until its compositor says otherwise.
    if (scale120_ != 120)
        rescale_ = true;
    scale120_ = 120;
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::shm_canvas::events {};
bool lx::shell::shm_canvas::allocate(void*, int, int, const char*) { return false; }
void lx::shell::shm_canvas::free() {}
void lx::shell::shm_canvas::commit(void*, int, const lx::rect2i*, unsigned) {}
void lx::shell::shm_canvas::attach_scale(void*, void*, void*) {}
void lx::shell::shm_canvas::release_scale() {}

#endif

lx::shell::shm_canvas::~shm_canvas() {
    free();
    release_scale();
}
