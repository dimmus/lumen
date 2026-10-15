module;

#include <atomic>
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

/// Pointer state, UI → render. Plain data: crossing threads is a 32-byte copy.
struct cursor_frame {
    /// Pointer (hotspot) position, output pixels.
    int x = 0;
    int y = 0;
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

using cursor_frame_mailbox = latest_value<cursor_frame>;
using cursor_image_mailbox = latest_value<cursor_plane_image>;

/// Stages a premultiplied ARGB image as compositor texture `texture_id`, padded into a
/// `canvas`-square texture (a fixed size keeps every backend on its in-place update path).
/// Installed by the protocol layer, which owns the staging and the texture queue.
using cursor_stage_fn = bool (*)(void* user, unsigned texture_id, const std::uint32_t* argb,
                                 unsigned width, unsigned height, unsigned stride_px,
                                 unsigned canvas);

/// The software cursor: a scene node drawn after every window. Used only when there is
/// no usable cursor plane, the plane rejected the image, or `LUMEN_CURSOR=software`.
class cursor_node final : public scene::scene_node {
public:
    ~cursor_node() override;
    [[nodiscard]] scene::node_kind kind() const override;
    void emit_draws(scene::draw_list& out) const override;

    void show(lx::rect2i dst);
    void hide();
    [[nodiscard]] bool shown() const { return shown_; }
    [[nodiscard]] lx::rect2i rect() const { return dst_; }

private:
    lx::rect2i dst_{};
    bool shown_ = false;
};

/// What the pointer looks like and where it is. UI affinity; render reads it only through
/// the two mailboxes.
///
/// Precedence, resolved when an input to it changes (never per frame):
///   1. the compositor's override (an interactive move/resize sets one), else
///   2. the pointer-focus client's request — a shape, a surface, or hidden, else
///   3. `default`.
/// A pointer-focus change resets the client request to `default`: clients set their cursor
/// on every `enter`, and a shape left over from the previous window must not persist.
class cursor_manager {
public:
    struct setup {
        const xcursor_theme* theme = nullptr;
        cursor_frame_mailbox* frames = nullptr;
        cursor_image_mailbox* images = nullptr;
        const input_router* router = nullptr;
        scene::scene_graph* scene = nullptr;
        scene::container_node* overlay = nullptr;
        /// A cursor plane exists; images up to `plane_size` go to it.
        bool hardware = false;
        lx::size2i plane_size{};
    };

    void init(const setup& s);
    void set_stage_fn(cursor_stage_fn fn, void* user);

    // Client requests. The protocol layer has already checked that the client holds
    // pointer focus and quoted the enter serial.
    void set_client_shape(cursor_shape shape);
    /// `surface` empty: hide the pointer.
    void set_client_surface(lx::surface_id surface, int hot_x, int hot_y);
    /// A cursor-role surface committed. `argb` null = no content. XRGB content (`opaque`) is
    /// given full alpha. `dx`/`dy` is the attach offset, which moves the hotspot by −d.
    void on_client_surface_commit(lx::surface_id surface, const std::uint32_t* argb,
                                  unsigned width, unsigned height, unsigned stride_px,
                                  bool opaque, int dx, int dy);
    void on_surface_destroyed(lx::surface_id surface);

    // Compositor.
    void set_override(cursor_shape shape);
    void clear_override();
    /// Per input event, O(1), no allocation. Outside a grab the new position is published
    /// at once, so render picks up the newest one when it builds its commit; inside a grab
    /// it waits for `on_tick`, so pointer and dragged window land in the same commit.
    void on_motion(double x, double y, unsigned long long time_us, bool in_grab);
    /// Once per frame, before the scene is published.
    void on_tick();
    /// Render reported the plane unusable: software from now on.
    void on_hardware_failed();

    [[nodiscard]] bool hardware_active() const { return hardware_ && !hw_failed_; }
    [[nodiscard]] cursor_shape shape() const { return shown_shape_; }

private:
    enum class client_mode : unsigned char { none, shape, surface, hidden };

    struct surface_image {
        lx::surface_id surface{};
        std::vector<std::uint32_t> pixels{};
        unsigned width = 0;
        unsigned height = 0;
        bool has_content = false;
        unsigned long long stamp = 0;
    };

    void sync_focus();
    void resolve();
    void publish_frame();
    void update_software(bool want);
    [[nodiscard]] surface_image* find_surface(lx::surface_id surface);

    static constexpr unsigned k_surface_cache = 4;

    const xcursor_theme* theme_ = nullptr;
    cursor_frame_mailbox* frames_ = nullptr;
    cursor_image_mailbox* images_ = nullptr;
    const input_router* router_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    cursor_stage_fn stage_fn_ = nullptr;
    void* stage_user_ = nullptr;
    cursor_node node_{};

    bool hardware_ = false;
    bool hw_failed_ = false;
    lx::size2i plane_size_{};
    unsigned canvas_ = 64;

    cursor_shape override_ = cursor_shape::none;
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

    // Resolved.
    cursor_image_view view_{};
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

void lx::compositor::cursor_node::emit_draws(scene::draw_list& out) const {
    if (!shown_)
        return;
    scene::draw_command cmd{};
    cmd.texture = lx::texture_id{k_cursor_texture_id};
    cmd.dst = dst_;
    cmd.src = {0, 0, dst_.width, dst_.height};
    cmd.opacity = 1.f;
    cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    // Set explicitly: the surface path calls anything at full opacity opaque, and a cursor
    // is mostly transparent.
    cmd.blend = lx::blend_mode::premultiplied;
    cmd.sort_key = lx::draw_sort_key::make(0, out.size());
    out.push(cmd);
}

void lx::compositor::cursor_node::show(lx::rect2i dst) {
    dst_ = dst;
    shown_ = true;
    set_transform({dst.x, dst.y});
}

void lx::compositor::cursor_node::hide() {
    shown_ = false;
    mark_dirty({});
}

// ── cursor_manager ─────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool cursor_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] bool rects_equal(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] int floor_px(double v) {
    const int i = static_cast<int>(v);
    return static_cast<double>(i) > v ? i - 1 : i;
}

} // namespace

void lx::compositor::cursor_manager::init(const setup& s) {
    theme_ = s.theme;
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
            images_->slot(i).pixels.assign(px, 0u);
    }
    // The software cursor's texture is one fixed square, so every image change is an
    // in-place update on each backend: the smallest power of two the theme fits in.
    unsigned need = theme_ ? theme_->max_dimension() : 32u;
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

void lx::compositor::cursor_manager::set_stage_fn(cursor_stage_fn fn, void* user) {
    stage_fn_ = fn;
    stage_user_ = user;
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
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::on_client_surface_commit(lx::surface_id surface,
                                                              const std::uint32_t* argb,
                                                              unsigned width, unsigned height,
                                                              unsigned stride_px, bool opaque,
                                                              int dx, int dy) {
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
        // Grows to the largest image the surface has used, then stays: no steady-state
        // allocation.
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
}

void lx::compositor::cursor_manager::set_override(cursor_shape shape) {
    if (override_ == shape)
        return;
    override_ = shape;
    needs_resolve_ = true;
}

void lx::compositor::cursor_manager::clear_override() { set_override(cursor_shape::none); }

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
    if (!theme_)
        return;

    cursor_image_view v{};
    bool show = true;
    cursor_shape shape = cursor_shape::default_;
    const char* source = "default";
    if (override_ != cursor_shape::none) {
        shape = override_;
        v = theme_->image(shape);
        source = "override";
    } else {
        switch (mode_) {
        case client_mode::shape:
            shape = client_shape_;
            v = theme_->image(shape);
            source = "client-shape";
            break;
        case client_mode::surface: {
            shape = cursor_shape::none;
            source = "client-surface";
            const surface_image* img = find_surface(client_surface_);
            if (img && img->has_content) {
                v.pixels = img->pixels.data();
                v.width = img->width;
                v.height = img->height;
                v.stride_px = img->width;
                v.hot_x = client_hot_x_;
                v.hot_y = client_hot_y_;
            } else {
                show = false; // a surface without content draws nothing
            }
            break;
        }
        case client_mode::hidden:
            shape = cursor_shape::none;
            source = "hidden";
            show = false;
            break;
        case client_mode::none:
            v = theme_->image(cursor_shape::default_);
            break;
        }
    }
    if (show && v.empty())
        show = false;

    const bool changed = force_image_ || show != visible_ || v.pixels != view_.pixels ||
                         v.width != view_.width || v.height != view_.height ||
                         v.hot_x != view_.hot_x || v.hot_y != view_.hot_y;
    force_image_ = false;
    view_ = v;
    visible_ = show;
    shown_shape_ = shape;

    // Hardware unless the plane is gone or cannot hold this image.
    const bool fits = static_cast<int>(v.width) <= plane_size_.width &&
                      static_cast<int>(v.height) <= plane_size_.height;
    frame_hardware_ = hardware_ && !hw_failed_ && fits;

    if (changed) {
        ++image_generation_;
        if (show && frame_hardware_ && images_) {
            cursor_plane_image& slot = images_->back();
            for (unsigned y = 0; y < v.height; ++y)
                std::memcpy(slot.pixels.data() + static_cast<std::size_t>(y) * v.width,
                            v.pixels + static_cast<std::size_t>(y) * v.stride_px,
                            static_cast<std::size_t>(v.width) * 4u);
            slot.width = v.width;
            slot.height = v.height;
            slot.hot_x = v.hot_x;
            slot.hot_y = v.hot_y;
            slot.generation = image_generation_;
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
    f.x = floor_px(x_);
    f.y = floor_px(y_);
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
            scene_->note_damage(node_.rect());
            node_.hide();
        }
        return;
    }
    if (sw_staged_generation_ != image_generation_) {
        // The canvas only grows: a client cursor larger than the theme's (a HiDPI client's
        // 48 px image over a 24 px theme) must not be cropped, hotspot and all. Growing
        // reallocates the texture once; later changes are in-place again.
        const unsigned need = view_.width > view_.height ? view_.width : view_.height;
        while (canvas_ < need && canvas_ < k_cursor_max_px)
            canvas_ *= 2;
        if (!stage_fn_ || !stage_fn_(stage_user_, k_cursor_texture_id, view_.pixels,
                                     view_.width, view_.height, view_.stride_px, canvas_)) {
            // Staging full this frame; keep the previous image and retry next tick.
            return;
        }
        sw_staged_generation_ = image_generation_;
        if (node_.shown())
            scene_->note_damage(node_.rect()); // same rect, new pixels
    }
    const int w = static_cast<int>(view_.width < canvas_ ? view_.width : canvas_);
    const int h = static_cast<int>(view_.height < canvas_ ? view_.height : canvas_);
    const lx::rect2i dst{floor_px(x_) - view_.hot_x, floor_px(y_) - view_.hot_y, w, h};
    if (node_.shown() && rects_equal(dst, node_.rect()))
        return;
    // Old and new as separate rects: a pointer that jumped far is two small repaints.
    if (node_.shown())
        scene_->note_damage(node_.rect());
    scene_->note_damage(dst);
    node_.show(dst);
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
