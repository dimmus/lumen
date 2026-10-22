module;

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

import lx.foundation;
import lx.scene;
import lx.trace;

export module lx.compositor:cursor;

import :xcursor;
import :input_router;

export namespace lx::compositor {

/// Texture id of the software cursor, outside the shm range (0x4000'0000 upward).
inline constexpr unsigned k_cursor_texture_id = 0x7FFF'FF00u;
/// Largest cursor image accepted from a client or a theme.
inline constexpr unsigned k_cursor_max_px = 256;

/// Single-producer single-consumer "latest value" triple buffer: the writer never waits,
/// the reader always gets the newest complete value, and intermediate values are dropped.
/// Lock-free and allocation-free after construction — the shape of a pointer position.
template <class T>
class latest_value {
public:
    /// Producer. The slot to fill before `publish`.
    [[nodiscard]] T& back() { return slots_[back_]; }
    void publish() {
        back_ = middle_.exchange(back_ | k_fresh, std::memory_order_acq_rel) & k_index;
    }
    /// Consumer. True when a value newer than the last `take` was published; it is then
    /// `front()`, which stays the consumer's own until the next successful `take`.
    [[nodiscard]] bool take() {
        if ((middle_.load(std::memory_order_acquire) & k_fresh) == 0)
            return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & k_index;
        return true;
    }
    [[nodiscard]] const T& front() const { return slots_[front_]; }
    /// Either side, before the other thread runs (one-time sizing).
    [[nodiscard]] T& slot(unsigned i) { return slots_[i]; }

private:
    static constexpr unsigned k_fresh = 0x80u;
    static constexpr unsigned k_index = 0x3u;
    T slots_[3]{};
    std::atomic<unsigned> middle_{1};
    unsigned back_ = 0;
    unsigned front_ = 2;
};

/// Pointer state, UI → render. Plain data: crossing threads is a 40-byte copy.
struct cursor_frame {
    /// Pointer (hotspot) position, layout (logical) coordinates: each output's pipeline
    /// places its plane from it with its own projection (ADR 008 §A.7).
    double x = 0.0;
    double y = 0.0;
    /// Input event that produced this frame, CLOCK_MONOTONIC µs (latency
    /// instrumentation); 0 when the frame was not input-driven (a client set its cursor, a
    /// window under a still pointer unmapped), so such changes are not timed against
    /// whatever input happened last.
    unsigned long long input_time_us = 0;
    /// Generation of the image this frame shows; render stages the plane when it changes.
    unsigned image_generation = 0;
    bool visible = false;
    /// False: the plane must be off and the scene's cursor node draws the pointer.
    bool hardware = false;
};

/// A plane-sized premultiplied ARGB image, UI → render.
struct cursor_plane_image {
    std::vector<std::uint32_t> pixels{};
    unsigned width = 0;
    unsigned height = 0;
    int hot_x = 0;
    int hot_y = 0;
    unsigned generation = 0;
};

/// Distinct output scales a cursor image set covers (outputs with more distinct scales
/// share the nearest).
inline constexpr unsigned k_cursor_scales = 4;

/// The pointer image at every distinct output scale (ADR 008 §A.7): an output's pipeline
/// stages the one of its scale, so the pointer is sharp on a 1.0 and a 1.5 output alike.
/// Prepared when the image or the set of scales changes, never per motion.
struct cursor_image_set {
    cursor_plane_image images[k_cursor_scales]{};
    /// Scale of `images[i]`, in 120ths.
    unsigned scale120[k_cursor_scales]{};
    unsigned count = 0;
    /// Bumped with every new set.
    unsigned generation = 0;

    /// The image for an output at `s120`: its own scale's, else the nearest. Null when empty.
    [[nodiscard]] const cursor_plane_image* for_scale(unsigned s120) const {
        const cursor_plane_image* best = nullptr;
        unsigned best_d = ~0u;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned d = scale120[i] > s120 ? scale120[i] - s120 : s120 - scale120[i];
            if (d < best_d) {
                best_d = d;
                best = &images[i];
            }
        }
        return best;
    }
};

using cursor_frame_mailbox = latest_value<cursor_frame>;
using cursor_image_mailbox = latest_value<cursor_image_set>;

/// Compositor-owned textures (the software cursor, server-side frames), staged through the
/// same queue as client shm content. Installed by the protocol layer, which owns the staging
/// and the texture queue; no Wayland or backend type crosses it.
struct compositor_texture_ops {
    /// Stages a premultiplied ARGB image (`width` x `height`, `stride_px` words per row) as
    /// texture `texture_id`, padded with transparent pixels to `canvas_w` x `canvas_h`
    /// (a fixed canvas keeps every backend on its in-place update path). The update becomes
    /// visible with the snapshot being built. False when staging is full this frame: retry.
    bool (*stage)(void* user, unsigned texture_id, const std::uint32_t* argb, unsigned width,
                  unsigned height, unsigned stride_px, unsigned canvas_w,
                  unsigned canvas_h) = nullptr;
    /// Dated retirement: dropped once no snapshot that may still draw it is in flight.
    void (*retire)(void* user, unsigned texture_id) = nullptr;
    void* user = nullptr;
};

/// The software cursor: a scene node drawn after every window. Used only when there is
/// no usable cursor plane, the plane rejected the image, or `LUMEN_CURSOR=software`.
class cursor_node final : public scene::scene_node {
public:
    ~cursor_node() override;
    [[nodiscard]] scene::node_kind kind() const override;
    using scene::scene_node::emit_draws;
    /// Placed per output view: the hotspot's pixel there, the image (rasterized for
    /// `image_scale`) resampled to the view's scale where it differs.
    void emit_draws(scene::draw_list& out, const scene::emit_context& ctx) const override;

    /// The hotspot at layout (`x`, `y`); a `w`x`h` image with its hotspot at (`hot_x`,
    /// `hot_y`), in pixels at `image_scale`.
    void show(double x, double y, unsigned w, unsigned h, int hot_x, int hot_y,
              double image_scale);
    void hide();
    [[nodiscard]] bool shown() const { return shown_; }
    /// The layout rect the image covers (a pixel wider on every side for rounding).
    [[nodiscard]] lx::rect2i logical_rect() const;

private:
    double x_ = 0.0;
    double y_ = 0.0;
    unsigned w_ = 0;
    unsigned h_ = 0;
    int hot_x_ = 0;
    int hot_y_ = 0;
    double image_scale_ = 1.0;
    bool shown_ = false;
};

/// What the pointer looks like and where it is. UI affinity; render reads it only through
/// the two mailboxes.
///
/// Precedence, resolved when an input to it changes (never per frame):
///   1. the compositor's override (an interactive move/resize sets one), else
///   2. the compositor's hover shape (the pointer is over a server-side frame), else
///   3. the pointer-focus client's request — a shape, a surface, or hidden, else
///   4. `default`.
/// A pointer-focus change resets the client request to `default`: clients set their cursor
/// on every `enter`, and a shape left over from the previous window must not persist.
class cursor_manager {
public:
    struct setup {
        /// One theme per distinct output scale, at that scale's pixel size, and the scale
        /// (120ths) each is for.
        const xcursor_theme* themes[k_cursor_scales]{};
        unsigned theme_scale120[k_cursor_scales]{};
        unsigned theme_count = 0;
        cursor_frame_mailbox* frames = nullptr;
        cursor_image_mailbox* images = nullptr;
        const input_router* router = nullptr;
        scene::scene_graph* scene = nullptr;
        scene::container_node* overlay = nullptr;
        /// Every output has a cursor plane; images up to `plane_size` go to them.
        bool hardware = false;
        lx::size2i plane_size{};
    };

    void init(const setup& s);
    /// The themes changed (an output configuration brought a new scale, ADR 008 §C.4): the
    /// image is resolved again at the next tick for every scale. Same shape as `setup`.
    void set_themes(const xcursor_theme* const* themes, const unsigned* scale120, unsigned count);
    void set_texture_ops(compositor_texture_ops ops) { tex_ops_ = ops; }

    // Client requests. The protocol layer has already checked that the client holds
    // pointer focus and quoted the enter serial.
    void set_client_shape(cursor_shape shape);
    /// `surface` empty: hide the pointer.
    void set_client_surface(lx::surface_id surface, int hot_x, int hot_y);
    /// A cursor-role surface committed. `argb` null = no content. XRGB content (`opaque`) is
    /// given full alpha. `dx`/`dy` is the attach offset (logical), which moves the hotspot
    /// by −d. `logical_w`×`logical_h` is the surface's size after its buffer scale and
    /// viewport (0: the buffer's): the image is resampled once per output scale, when it is
    /// resolved, to that size in that scale's pixels where the buffer differs (a scale-2
    /// cursor on a 1.25 output), so planes and the software node always show it at its
    /// logical size.
    void on_client_surface_commit(lx::surface_id surface, const std::uint32_t* argb,
                                  unsigned width, unsigned height, unsigned stride_px,
                                  bool opaque, int dx, int dy, int logical_w = 0,
                                  int logical_h = 0);
    void on_surface_destroyed(lx::surface_id surface);
    /// Called when the client cursor surface being shown changes (either may be null): the
    /// protocol layer sends it wl_surface.enter/leave for the output under the pointer — a
    /// client picks its cursor buffer scale from that (GTK does).
    using surface_hook = void (*)(void* user, lx::surface_id old_surface,
                                  lx::surface_id new_surface);
    void set_surface_hook(surface_hook fn, void* user) {
        surface_hook_ = fn;
        surface_hook_user_ = user;
    }
    /// The client surface the pointer shows, null when it shows a shape or nothing.
    [[nodiscard]] lx::surface_id shown_surface() const {
        return mode_ == client_mode::surface ? client_surface_ : lx::surface_id{};
    }

    // Compositor.
    void set_override(cursor_shape shape);
    void clear_override();
    /// The pointer is over something the compositor draws (a server-side frame): its shape
    /// wins over the client's, below a grab's override.
    void set_hover_shape(cursor_shape shape);
    void clear_hover_shape();
    /// Per input event, O(1), no allocation. Outside a grab the new position is published
    /// at once, so render picks up the newest one when it builds its commit; inside a grab
    /// it waits for `on_tick`, so pointer and dragged window land in the same commit.
    void on_motion(double x, double y, unsigned long long time_us, bool in_grab);
    /// Once per frame, before the scene is published.
    void on_tick();
    /// Render reported a plane unusable: software from now on.
    void on_hardware_failed();

    [[nodiscard]] bool hardware_active() const { return hardware_ && !hw_failed_; }
    /// The pointer as last resolved, for screen capture's cursor sessions (UI; the view stays
    /// valid until the next tick): its image at the theme scale nearest `scale120`, and that
    /// image's generation. False while no pointer is shown.
    [[nodiscard]] bool image_for_scale(unsigned scale120, cursor_image_view& out,
                                       unsigned& generation) const {
        if (!visible_ || theme_count_ == 0)
            return false;
        unsigned best = 0;
        unsigned best_d = ~0u;
        for (unsigned k = 0; k < theme_count_; ++k) {
            const unsigned d = theme_scale120_[k] > scale120 ? theme_scale120_[k] - scale120
                                                             : scale120 - theme_scale120_[k];
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        if (views_[best].empty())
            return false;
        out = views_[best];
        generation = image_generation_;
        return true;
    }
    /// The pointer's hotspot, layout coordinates.
    [[nodiscard]] double x() const { return x_; }
    [[nodiscard]] double y() const { return y_; }
    [[nodiscard]] cursor_shape shape() const { return shown_shape_; }

private:
    enum class client_mode : unsigned char { none, shape, surface, hidden };

    struct surface_image {
        lx::surface_id surface{};
        /// The client's buffer as committed (premultiplied, packed), and its logical size.
        std::vector<std::uint32_t> pixels{};
        unsigned width = 0;
        unsigned height = 0;
        int logical_w = 0;
        int logical_h = 0;
        bool has_content = false;
        unsigned long long stamp = 0;
    };

    void sync_focus();
    /// Fires the surface hook when `shown_surface()` changed since it last did.
    void notify_surface();
    void resolve();
    void publish_frame();
    void update_software(bool want);
    [[nodiscard]] surface_image* find_surface(lx::surface_id surface);

    static constexpr unsigned k_surface_cache = 4;

    const xcursor_theme* themes_[k_cursor_scales]{};
    unsigned theme_scale120_[k_cursor_scales]{};
    unsigned theme_count_ = 0;
    /// The theme of the largest scale: what the software cursor draws (resampled down on
    /// smaller outputs).
    unsigned sw_index_ = 0;
    /// A client surface's image at each scale (resampled when its logical size in that
    /// scale's pixels differs from its buffer). Grows to the largest seen, then stays.
    std::vector<std::uint32_t> scaled_[k_cursor_scales]{};
    cursor_frame_mailbox* frames_ = nullptr;
    cursor_image_mailbox* images_ = nullptr;
    const input_router* router_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    compositor_texture_ops tex_ops_{};
    cursor_node node_{};

    bool hardware_ = false;
    bool hw_failed_ = false;
    lx::size2i plane_size_{};
    unsigned canvas_ = 64;

    cursor_shape override_ = cursor_shape::none;
    cursor_shape hover_ = cursor_shape::none;
    client_mode mode_ = client_mode::none;
    cursor_shape client_shape_ = cursor_shape::default_;
    lx::surface_id client_surface_{};
    int client_hot_x_ = 0;
    int client_hot_y_ = 0;
    unsigned focus_generation_ = 0;
    surface_image cache_[k_surface_cache]{};
    unsigned long long cache_stamp_ = 0;
    bool needs_resolve_ = true;
    bool force_image_ = false;
    surface_hook surface_hook_ = nullptr;
    void* surface_hook_user_ = nullptr;
    lx::surface_id notified_surface_{};

    // Resolved, per scale.
    cursor_image_view views_[k_cursor_scales]{};
    bool visible_ = false;
    bool frame_hardware_ = false;
    unsigned image_generation_ = 0;
    cursor_shape shown_shape_ = cursor_shape::default_;

    double x_ = 0.0;
    double y_ = 0.0;
    unsigned long long input_time_us_ = 0;
    /// `input_time_us_` as last published: a frame carries an input time only once.
    unsigned long long published_input_us_ = 0;
    bool frame_pending_ = true;
    unsigned sw_staged_generation_ = 0;
};

} // namespace lx::compositor


// ── cursor_node ────────────────────────────────────────────────────────────

lx::compositor::cursor_node::~cursor_node() = default;

lx::scene::node_kind lx::compositor::cursor_node::kind() const {
    // Not a container, so `scene_graph::emit_node` asks it for draws.
    return scene::node_kind::surface;
}

void lx::compositor::cursor_node::emit_draws(scene::draw_list& out,
                                            const scene::emit_context& ctx) const {
    if (!shown_ || w_ == 0 || h_ == 0)
        return;
    const scene::output_projection& p = ctx.projection;
    const double vs = p.scale > 0.0 ? p.scale : 1.0;
    const double k = vs / (image_scale_ > 0.0 ? image_scale_ : 1.0);
    // The hotspot's pixel on this output, as the plane is placed.
    const long px = static_cast<long>(std::floor((x_ - p.origin.x) * vs));
    const long py = static_cast<long>(std::floor((y_ - p.origin.y) * vs));
    scene::draw_command cmd{};
    cmd.texture = lx::texture_id{k_cursor_texture_id};
    cmd.dst = {static_cast<int>(px - std::lround(hot_x_ * k)),
               static_cast<int>(py - std::lround(hot_y_ * k)),
               static_cast<int>(std::lround(w_ * k)), static_cast<int>(std::lround(h_ * k))};
    if (p.size.width > 0 && p.size.height > 0 &&
        (cmd.dst.x >= p.size.width || cmd.dst.y >= p.size.height ||
         cmd.dst.x + cmd.dst.width <= 0 || cmd.dst.y + cmd.dst.height <= 0))
        return; // not on this output
    cmd.src = lx::rect2f{0.f, 0.f, static_cast<float>(w_), static_cast<float>(h_)};
    cmd.scaled = k != 1.0;
    cmd.opacity = 1.f;
    cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    // Set explicitly: the surface path calls anything at full opacity opaque, and a cursor
    // is mostly transparent.
    cmd.blend = lx::blend_mode::premultiplied;
    cmd.sort_key = lx::draw_sort_key::make(0, out.size());
    out.push(cmd);
}

void lx::compositor::cursor_node::show(double x, double y, unsigned w, unsigned h, int hot_x,
                                       int hot_y, double image_scale) {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
    hot_x_ = hot_x;
    hot_y_ = hot_y;
    image_scale_ = image_scale > 0.0 ? image_scale : 1.0;
    shown_ = true;
    mark_dirty({});
}

void lx::compositor::cursor_node::hide() {
    shown_ = false;
    mark_dirty({});
}

lx::rect2i lx::compositor::cursor_node::logical_rect() const {
    const double s = image_scale_ > 0.0 ? image_scale_ : 1.0;
    const double x0 = x_ - hot_x_ / s;
    const double y0 = y_ - hot_y_ / s;
    const int lx0 = static_cast<int>(std::floor(x0)) - 1;
    const int ly0 = static_cast<int>(std::floor(y0)) - 1;
    const int lx1 = static_cast<int>(std::ceil(x0 + w_ / s)) + 1;
    const int ly1 = static_cast<int>(std::ceil(y0 + h_ / s)) + 1;
    return {lx0, ly0, lx1 - lx0, ly1 - ly0};
}

// ── cursor_manager ─────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool cursor_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] bool rects_equal(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Bilinear resample of a premultiplied ARGB image (`sw`×`sh`, `stride` words a row) into
/// `out` (`dw`×`dh`, tightly packed): pixel centres mapped onto each other, taps clamped at
/// the edges. Once per cursor image change, never per frame.
void resample_bilinear(const std::uint32_t* src, unsigned sw, unsigned sh, unsigned stride,
                       std::uint32_t* out, unsigned dw, unsigned dh) {
    const double kx = static_cast<double>(sw) / dw;
    const double ky = static_cast<double>(sh) / dh;
    for (unsigned y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * ky - 0.5;
        if (fy < 0.0)
            fy = 0.0;
        const unsigned y0 = static_cast<unsigned>(fy);
        const unsigned y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const unsigned wy = static_cast<unsigned>((fy - y0) * 256.0);
        for (unsigned x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * kx - 0.5;
            if (fx < 0.0)
                fx = 0.0;
            const unsigned x0 = static_cast<unsigned>(fx);
            const unsigned x1 = x0 + 1 < sw ? x0 + 1 : x0;
            const unsigned wx = static_cast<unsigned>((fx - x0) * 256.0);
            const std::uint32_t p00 = src[static_cast<std::size_t>(y0) * stride + x0];
            const std::uint32_t p10 = src[static_cast<std::size_t>(y0) * stride + x1];
            const std::uint32_t p01 = src[static_cast<std::size_t>(y1) * stride + x0];
            const std::uint32_t p11 = src[static_cast<std::size_t>(y1) * stride + x1];
            std::uint32_t pixel = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                const unsigned c = (((p00 >> shift) & 0xFFu) * (256u - wx) * (256u - wy) +
                                    ((p10 >> shift) & 0xFFu) * wx * (256u - wy) +
                                    ((p01 >> shift) & 0xFFu) * (256u - wx) * wy +
                                    ((p11 >> shift) & 0xFFu) * wx * wy + 32768u) >>
                                   16;
                pixel |= (c > 255u ? 255u : c) << shift;
            }
            out[static_cast<std::size_t>(y) * dw + x] = pixel;
        }
    }
}

} // namespace

void lx::compositor::cursor_manager::init(const setup& s) {
    theme_count_ = s.theme_count < k_cursor_scales ? s.theme_count : k_cursor_scales;
    sw_index_ = 0;
    for (unsigned i = 0; i < theme_count_; ++i) {
        themes_[i] = s.themes[i];
        theme_scale120_[i] = s.theme_scale120[i] ? s.theme_scale120[i] : 120u;
        if (theme_scale120_[i] > theme_scale120_[sw_index_])
            sw_index_ = i;
    }
    frames_ = s.frames;
    images_ = s.images;
    router_ = s.router;
    scene_ = s.scene;
    hardware_ = s.hardware && s.plane_size.width > 0 && s.plane_size.height > 0 && s.images;
    plane_size_ = s.plane_size;
    if (hardware_) {
        // Allocated once: render owns whichever slot it has taken, so every slot must be
        // plane-sized before the render thread starts.
        const std::size_t px = static_cast<std::size_t>(plane_size_.width) *
                               static_cast<std::size_t>(plane_size_.height);
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned k = 0; k < k_cursor_scales; ++k)
                images_->slot(i).images[k].pixels.assign(px, 0u);
    }
    // The software cursor's texture is one fixed square, so every image change is an
    // in-place update on each backend: the smallest power of two the theme fits in.
    unsigned need = theme_count_ > 0 && themes_[sw_index_] ? themes_[sw_index_]->max_dimension()
                                                           : 32u;
    canvas_ = 32;
    while (canvas_ < need && canvas_ < k_cursor_max_px)
        canvas_ *= 2;
    for (auto& c : cache_)
        c = {};
    if (s.overlay)
        (void)s.overlay->add(&node_);
    if (router_)
        focus_generation_ = router_->pointer_focus_generation();
    needs_resolve_ = true;
    frame_pending_ = true;
}

void lx::compositor::cursor_manager::set_themes(const xcursor_theme* const* themes,
                                                const unsigned* scale120, unsigned count) {
    theme_count_ = count < k_cursor_scales ? count : k_cursor_scales;
    sw_index_ = 0;
    for (unsigned i = 0; i < theme_count_; ++i) {
        themes_[i] = themes[i];
        theme_scale120_[i] = scale120[i] ? scale120[i] : 120u;
        if (theme_scale120_[i] > theme_scale120_[sw_index_])
            sw_index_ = i;
        views_[i] = {};
    }
    force_image_ = true;
    needs_resolve_ = true;
    frame_pending_ = true;
}

void lx::compositor::cursor_manager::sync_focus() {
    if (!router_)
        return;
    const unsigned g = router_->pointer_focus_generation();
    if (g == focus_generation_)
        return;
    focus_generation_ = g;
    mode_ = client_mode::none;
    client_surface_ = {};
    needs_resolve_ = true;
    notify_surface();
}

void lx::compositor::cursor_manager::notify_surface() {
    const lx::surface_id now = shown_surface();
    if (now == notified_surface_)
        return;
    const lx::surface_id old = notified_surface_;
    notified_surface_ = now;
    if (surface_hook_)
        surface_hook_(surface_hook_user_, old, now);
}

lx::compositor::cursor_manager::surface_image*
lx::compositor::cursor_manager::find_surface(lx::surface_id surface) {
    if (!surface)
        return nullptr;
    for (auto& c : cache_)
        if (c.surface == surface)
            return &c;
    return nullptr;
}

void lx::compositor::cursor_manager::set_client_shape(cursor_shape shape) {
    sync_focus();
    mode_ = client_mode::shape;
    client_shape_ = shape;
    needs_resolve_ = true;
    notify_surface();
}

void lx::compositor::cursor_manager::set_client_surface(lx::surface_id surface, int hot_x,
                                                        int hot_y) {
    sync_focus();
    if (!surface) {
        mode_ = client_mode::hidden;
        client_surface_ = {};
    } else {
        mode_ = client_mode::surface;
        client_surface_ = surface;
        client_hot_x_ = hot_x;
        client_hot_y_ = hot_y;
        force_image_ = true;
    }
    notify_surface();
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::on_client_surface_commit(lx::surface_id surface,
                                                              const std::uint32_t* argb,
                                                              unsigned width, unsigned height,
                                                              unsigned stride_px, bool opaque,
                                                              int dx, int dy, int logical_w,
                                                              int logical_h) {
    if (!surface)
        return;
    surface_image* slot = find_surface(surface);
    if (!slot && (!argb || width == 0 || height == 0)) {
        // Nothing cached and nothing to cache: "no content" needs no slot.
        if (surface == client_surface_)
            needs_resolve_ = true;
        return;
    }
    if (!slot) {
        // Least recently committed entry. A client normally has one cursor surface; the
        // cache only covers ones that commit before set_cursor names them.
        // Never the surface currently shown: the resolved image points into its pixels.
        slot = nullptr;
        for (auto& c : cache_) {
            if (!c.surface) {
                slot = &c;
                break;
            }
            if (c.surface == client_surface_)
                continue;
            if (!slot || c.stamp < slot->stamp)
                slot = &c;
        }
        if (!slot)
            return;
        slot->surface = surface;
        slot->has_content = false;
    }
    slot->stamp = ++cache_stamp_;
    if (!argb || width == 0 || height == 0) {
        slot->has_content = false;
    } else if (width > k_cursor_max_px || height > k_cursor_max_px || stride_px < width) {
        static bool reported = false;
        if (!reported && cursor_debug()) {
            reported = true;
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.cursor",
                                            "client cursor larger than 256x256 ignored");
        }
        return;
    } else {
        // Kept as the client drew it; each output scale's image is made from it when the
        // cursor is resolved. Grows to the largest buffer the surface has used, then stays:
        // no steady-state allocation.
        const std::size_t n = static_cast<std::size_t>(width) * height;
        if (slot->pixels.size() < n)
            slot->pixels.resize(n);
        for (unsigned y = 0; y < height; ++y) {
            const std::uint32_t* src = argb + static_cast<std::size_t>(y) * stride_px;
            std::uint32_t* dst = slot->pixels.data() + static_cast<std::size_t>(y) * width;
            if (opaque) {
                for (unsigned x = 0; x < width; ++x)
                    dst[x] = src[x] | 0xFF000000u;
            } else {
                std::memcpy(dst, src, static_cast<std::size_t>(width) * 4u);
            }
        }
        slot->width = width;
        slot->height = height;
        slot->logical_w = logical_w > 0 ? logical_w : static_cast<int>(width);
        slot->logical_h = logical_h > 0 ? logical_h : static_cast<int>(height);
        slot->has_content = true;
    }
    if (surface == client_surface_ && mode_ == client_mode::surface) {
        client_hot_x_ -= dx;
        client_hot_y_ -= dy;
        force_image_ = true;
        needs_resolve_ = true;
    }
}

void lx::compositor::cursor_manager::on_surface_destroyed(lx::surface_id surface) {
    if (surface_image* slot = find_surface(surface))
        *slot = {};
    if (surface && surface == client_surface_) {
        // The pointer keeps no image of a surface that no longer exists.
        mode_ = client_mode::hidden;
        client_surface_ = {};
        needs_resolve_ = true;
    }
    notify_surface();
}

void lx::compositor::cursor_manager::set_override(cursor_shape shape) {
    if (override_ == shape)
        return;
    override_ = shape;
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::clear_override() { set_override(cursor_shape::none); }

void lx::compositor::cursor_manager::set_hover_shape(cursor_shape shape) {
    if (hover_ == shape)
        return;
    hover_ = shape;
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::clear_hover_shape() { set_hover_shape(cursor_shape::none); }

void lx::compositor::cursor_manager::on_motion(double x, double y, unsigned long long time_us,
                                               bool in_grab) {
    x_ = x;
    y_ = y;
    input_time_us_ = time_us;
    if (in_grab || needs_resolve_ || !frames_) {
        frame_pending_ = true;
        return;
    }
    publish_frame();
}

void lx::compositor::cursor_manager::on_hardware_failed() {
    if (hw_failed_)
        return;
    hw_failed_ = true;
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.cursor",
                                    "software cursor: the cursor plane rejected the image");
    force_image_ = true;
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::resolve() {
    needs_resolve_ = false;
    if (theme_count_ == 0)
        return;

    bool show = true;
    cursor_shape shape = cursor_shape::default_;
    const char* source = "default";
    const surface_image* client = nullptr;
    if (override_ != cursor_shape::none) {
        shape = override_;
        source = "override";
    } else if (hover_ != cursor_shape::none) {
        shape = hover_;
        source = "hover";
    } else {
        switch (mode_) {
        case client_mode::shape:
            shape = client_shape_;
            source = "client-shape";
            break;
        case client_mode::surface: {
            shape = cursor_shape::none;
            source = "client-surface";
            const surface_image* img = find_surface(client_surface_);
            if (img && img->has_content)
                client = img;
            else
                show = false; // a surface without content draws nothing
            break;
        }
        case client_mode::hidden:
            shape = cursor_shape::none;
            source = "hidden";
            show = false;
            break;
        case client_mode::none:
            shape = cursor_shape::default_;
            break;
        }
    }

    // One view per scale: the theme loaded at that scale's size, or the client's image at
    // its logical size in that scale's pixels (resampled only where they differ).
    cursor_image_view v[k_cursor_scales]{};
    for (unsigned k = 0; k < theme_count_; ++k) {
        const double sc = theme_scale120_[k] / 120.0;
        if (!show)
            continue;
        if (!client) {
            if (themes_[k])
                v[k] = themes_[k]->image(shape);
            continue;
        }
        const long pw = std::lround(client->logical_w * sc);
        const long ph = std::lround(client->logical_h * sc);
        const unsigned out_w = pw < 1 ? 1u : (pw > static_cast<long>(k_cursor_max_px)
                                                  ? k_cursor_max_px
                                                  : static_cast<unsigned>(pw));
        const unsigned out_h = ph < 1 ? 1u : (ph > static_cast<long>(k_cursor_max_px)
                                                  ? k_cursor_max_px
                                                  : static_cast<unsigned>(ph));
        if (out_w == client->width && out_h == client->height) {
            v[k].pixels = client->pixels.data();
        } else {
            const std::size_t n = static_cast<std::size_t>(out_w) * out_h;
            if (scaled_[k].size() < n)
                scaled_[k].resize(n);
            resample_bilinear(client->pixels.data(), client->width, client->height,
                              client->width, scaled_[k].data(), out_w, out_h);
            v[k].pixels = scaled_[k].data();
        }
        v[k].width = out_w;
        v[k].height = out_h;
        v[k].stride_px = out_w;
        // set_cursor's hotspot is surface-local (logical); the image is in pixels.
        v[k].hot_x = static_cast<int>(std::lround(client_hot_x_ * sc));
        v[k].hot_y = static_cast<int>(std::lround(client_hot_y_ * sc));
    }
    if (show && v[sw_index_].empty())
        show = false;

    bool changed = force_image_ || show != visible_;
    for (unsigned k = 0; k < theme_count_; ++k)
        changed = changed || v[k].pixels != views_[k].pixels || v[k].width != views_[k].width ||
                  v[k].height != views_[k].height || v[k].hot_x != views_[k].hot_x ||
                  v[k].hot_y != views_[k].hot_y;
    // A client image resampled into the same buffer changes without its pointer changing.
    changed = changed || client != nullptr;
    force_image_ = false;
    for (unsigned k = 0; k < theme_count_; ++k)
        views_[k] = v[k];
    visible_ = show;
    shown_shape_ = shape;

    // Hardware unless a plane is gone or cannot hold an image.
    bool fits = true;
    for (unsigned k = 0; k < theme_count_; ++k)
        fits = fits && static_cast<int>(v[k].width) <= plane_size_.width &&
               static_cast<int>(v[k].height) <= plane_size_.height;
    frame_hardware_ = hardware_ && !hw_failed_ && fits;

    if (changed) {
        ++image_generation_;
        if (show && frame_hardware_ && images_) {
            cursor_image_set& set = images_->back();
            for (unsigned k = 0; k < theme_count_; ++k) {
                cursor_plane_image& img = set.images[k];
                const cursor_image_view& iv = v[k];
                for (unsigned y = 0; y < iv.height; ++y)
                    std::memcpy(img.pixels.data() + static_cast<std::size_t>(y) * iv.width,
                                iv.pixels + static_cast<std::size_t>(y) * iv.stride_px,
                                static_cast<std::size_t>(iv.width) * 4u);
                img.width = iv.width;
                img.height = iv.height;
                img.hot_x = iv.hot_x;
                img.hot_y = iv.hot_y;
                img.generation = image_generation_;
                set.scale120[k] = theme_scale120_[k];
            }
            set.count = theme_count_;
            set.generation = image_generation_;
            images_->publish();
        }
        if (cursor_debug()) {
            char line[96];
            std::snprintf(line, sizeof(line), "cursor shape=%u source=%s hardware=%d",
                          static_cast<unsigned>(shape), source, frame_hardware_ ? 1 : 0);
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.cursor", line);
        }
    }
    frame_pending_ = true;
}

void lx::compositor::cursor_manager::publish_frame() {
    frame_pending_ = false;
    if (!frames_)
        return;
    cursor_frame& f = frames_->back();
    // Layout coordinates: each pipeline projects them onto its own output.
    f.x = x_;
    f.y = y_;
    f.input_time_us = input_time_us_ != published_input_us_ ? input_time_us_ : 0;
    published_input_us_ = input_time_us_;
    f.image_generation = image_generation_;
    f.visible = visible_;
    f.hardware = frame_hardware_;
    frames_->publish();
}

void lx::compositor::cursor_manager::update_software(bool want) {
    if (!scene_)
        return;
    if (!want) {
        if (node_.shown()) {
            scene_->note_damage(node_.logical_rect());
            node_.hide();
        }
        return;
    }
    const cursor_image_view& view = views_[sw_index_];
    if (sw_staged_generation_ != image_generation_) {
        // The canvas only grows: a client cursor larger than the theme's (a HiDPI client's
        // 48 px image over a 24 px theme) must not be cropped, hotspot and all. Growing
        // reallocates the texture once; later changes are in-place again.
        const unsigned need = view.width > view.height ? view.width : view.height;
        while (canvas_ < need && canvas_ < k_cursor_max_px)
            canvas_ *= 2;
        if (!tex_ops_.stage ||
            !tex_ops_.stage(tex_ops_.user, k_cursor_texture_id, view.pixels, view.width,
                            view.height, view.stride_px, canvas_, canvas_)) {
            // Staging full this frame; keep the previous image and retry next tick.
            return;
        }
        sw_staged_generation_ = image_generation_;
        if (node_.shown())
            scene_->note_damage(node_.logical_rect()); // same rect, new pixels
    }
    const unsigned w = view.width < canvas_ ? view.width : canvas_;
    const unsigned h = view.height < canvas_ ? view.height : canvas_;
    const lx::rect2i before = node_.logical_rect();
    const bool was_shown = node_.shown();
    node_.show(x_, y_, w, h, view.hot_x, view.hot_y, theme_scale120_[sw_index_] / 120.0);
    const lx::rect2i after = node_.logical_rect();
    if (was_shown && rects_equal(before, after))
        return;
    // Old and new as separate rects: a pointer that jumped far is two small repaints, and
    // each reaches every output it touches (the damage fans out).
    if (was_shown)
        scene_->note_damage(before);
    scene_->note_damage(after);
}

void lx::compositor::cursor_manager::on_tick() {
    lx::trace::scoped_span span{"compositor.cursor", "on_tick"};
    sync_focus();
    if (needs_resolve_)
        resolve();
    if (frame_pending_)
        publish_frame();
    update_software(visible_ && !frame_hardware_);
}
