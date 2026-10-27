module;

// lx.app:window — a toplevel window drawn with the ADR-010 engine: xdg_toplevel (with a
// server-side decoration request), an lx.ui `ui_root`, the client pixel path of §4.10 (the
// `lx::gfx::client_gpu` tier and its `client_swapchain`, whose images become wl_buffers here)
// and the frame loop of §4.9.
//
// Frame loop. Input, a reducer, a timer or a configure leaves work in lx.ui's ledger; the
// window's frame source sees it in the same loop iteration. With no frame callback
// outstanding the frame is drawn at once (no round trip to the compositor first: input that
// arrives while the window is idle is on screen at the next refresh); with one outstanding it
// waits for it, so a window never draws faster than the compositor presents. A frame:
// acquire a free image (none free: wait for a release, then draw once — never a busy loop),
// take its age as repaint (`ui_root::add_repaint`), tick the root with `emit_scope::damage`,
// render inside the repaint region (each rect scissored), present it (sync exported), ack the
// configure it answers, attach, damage_buffer × what changed (≤ 16 rects), set the syncobj
// points, request the next frame callback and presentation feedback, commit. The ledger is
// cleared only by that commit: a frame that fails puts its damage back. A configure (and a
// preferred-scale change) is answered at once, whatever callback is outstanding: the commit
// right after its ack has its size (frame-perfect resize).
//
// Idle (G1): an empty ledger, nothing animating, no timer due, no key held → no frame callback
// is requested after the last frame's, nothing is submitted, no timer is armed, and the loop
// blocks in epoll with no timeout.
//
// Scale. With wp_fractional_scale_v1 and wp_viewporter the buffer is ceil(logical × scale)
// device px and the viewport's destination the logical size (buffer_scale 1); without them the
// integer wl_surface.preferred_buffer_scale (v6), else the outputs' scale, else 1, as
// buffer_scale. The UI always lays out in logical px; `ui_root::set_scale` projects at
// emission and rasterises text and icons at device resolution.
//
// UI affinity. A steady frame allocates nothing (G5); allocation happens at open, at a
// swapchain (re)creation and when the tree's shape changes.

export module lx.app:window;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;
import lx.wayland.client;

import :input;

export namespace lx::app {

/// How an application runs (read once at `application::start`).
struct settings {
    /// The compositor socket (null: WAYLAND_SOCKET / WAYLAND_DISPLAY, libwayland's order).
    const char* socket = nullptr;
    /// A compiled theme: "one-dark" (the default), "one-light", or a path to a `.lxtheme`.
    /// Null: `LUMEN_THEME`, else One Dark.
    const char* theme = nullptr;
    /// Reload the theme file when it changes (`lx::ui::theme_watcher`): -1 debug builds only
    /// (`LUMEN_THEME_WATCH=1` / `0` overrides), 0 never, 1 always.
    int watch_theme = -1;
    /// The window's pixel path: `none` picks as the compositor does (hardware Vulkan →
    /// hardware GL → CPU, §4.10); `LUMEN_UI_BACKEND` overrides it (`client_gpu`).
    lx::gfx::client_tier backend = lx::gfx::client_tier::none;
    /// Let a forced tier run on a software device (lavapipe, llvmpipe): tests only.
    bool allow_software_gpu = false;
    /// Rasterise icon misses on the worker thread (else on the UI thread, synchronously).
    bool async_icons = true;
};

class window;

/// What a window needs from its application (implemented by `application`).
class window_host {
public:
    [[nodiscard]] virtual lx::wayland::display& host_display() = 0;
    [[nodiscard]] virtual lx::runtime::event_loop& host_loop() = 0;
    [[nodiscard]] virtual const settings& host_settings() const = 0;
    /// The worker executor icon rasters and theme loads go to (null: synchronous).
    [[nodiscard]] virtual lx::runtime::executor* host_worker() = 0;
    /// The window asks to be closed (xdg_toplevel.close): the application decides.
    virtual void host_close_requested(window& w) = 0;
    /// The window opened / is closing: seat events and deadlines are routed to it.
    virtual void host_attach(window& w) = 0;
    virtual void host_detach(window& w) = 0;

protected:
    ~window_host() = default;
};

struct window_options {
    const char* title = "Lumen";
    const char* app_id = "org.lumen.app";
    /// Logical px, used when the compositor's first configure leaves the size to the client.
    lx::size2i size{640, 480};
    lx::size2i min_size{};
    /// The UI. Required.
    lx::ui::describe_fn describe = nullptr;
    bool server_side_decorations = true;
    /// An opaque window (XRGB buffers, an opaque region): the compositor blends nothing.
    bool opaque = true;
    /// The UI arena (descriptors, per frame).
    std::size_t arena_bytes = 512 * 1024;
};

/// One committed frame's wp_presentation feedback, for the bench (§8.3 S9). CLOCK_MONOTONIC
/// ns (the compositor's presentation clock when it is that, which Lumen's is).
struct frame_record {
    std::uint64_t commit_id = 0;
    std::uint64_t commit_ns = 0;
    /// When the frame started (its event or callback seen): commit − start is the UI
    /// thread's frame time.
    std::uint64_t start_ns = 0;
    std::uint64_t presented_ns = 0;
    std::uint64_t sequence = 0;
    std::uint32_t refresh_ns = 0;
    std::uint32_t flags = 0;
    std::uint16_t damage_rects = 0;
    bool presented = false;
    bool discarded = false;
};
inline constexpr unsigned k_frame_ring = 256;

struct window_stats {
    std::uint64_t frame_requests = 0;  ///< wl_surface.frame requested
    std::uint64_t frame_callbacks = 0; ///< wl_callback.done received
    std::uint64_t frames = 0;          ///< frames drawn and committed
    std::uint64_t commits = 0;         ///< wl_surface.commit (frames, and acks without a frame)
    std::uint64_t empty_ticks = 0;     ///< ticks that produced nothing (the image went back)
    std::uint64_t no_image = 0;        ///< frames that waited for a release
    std::uint64_t configures = 0;
    std::uint64_t acks = 0;
    std::uint64_t reallocations = 0;   ///< swapchain allocations (creation and new sizes)
    std::uint64_t scale_changes = 0;
    std::uint64_t failures = 0;        ///< frames that failed (their damage put back)
    std::uint64_t presented = 0;
    std::uint64_t discarded = 0;
    /// The last frame: what changed (device rects sent as damage_buffer), what it repainted.
    unsigned last_damage_rects = 0;
    unsigned last_repaint_rects = 0;
    std::uint64_t last_damage_px = 0;
    lx::rect2i last_damage_bounds{};
};

/// When the window got where (CLOCK_MONOTONIC ns; 0: not yet).
struct window_timeline {
    std::uint64_t opened_ns = 0;
    std::uint64_t first_configure_ns = 0;
    std::uint64_t first_commit_ns = 0;
    std::uint64_t first_presented_ns = 0;
};

class window final : public lx::wayland::toplevel_handler,
                     public lx::wayland::surface_handler,
                     public lx::wayland::buffer_handler,
                     public lx::gfx::swapchain_listener {
public:
    window();
    ~window() override;
    window(const window&) = delete;
    window& operator=(const window&) = delete;

    /// Creates the surface and the toplevel, picks the pixel tier and adds the frame source
    /// to the host's loop. The first frame is drawn when the first configure arrives.
    [[nodiscard]] lx::result<void> open(window_host& host, const window_options& options);
    void close();
    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] lx::ui::ui_root& root() { return *root_; }
    [[nodiscard]] input_router& input() { return input_; }
    [[nodiscard]] lx::wayland::surface& surface() { return surface_; }
    [[nodiscard]] lx::wayland::toplevel& toplevel() { return toplevel_; }
    [[nodiscard]] lx::gfx::client_tier tier() const { return gpu_.tier(); }
    [[nodiscard]] const lx::gfx::client_gpu& gpu() const { return gpu_; }
    [[nodiscard]] const lx::gfx::client_swapchain& swapchain() const { return swapchain_; }
    [[nodiscard]] lx::size2i logical_size() const { return logical_; }
    [[nodiscard]] lx::size2i buffer_size() const { return buffer_; }
    [[nodiscard]] float scale() const { return scale_; }
    /// wl_surface.buffer_scale in use (1 with a viewport).
    [[nodiscard]] int buffer_scale() const { return buffer_scale_; }
    /// The viewport's destination (0 × 0: no viewport).
    [[nodiscard]] lx::size2i viewport_destination() const { return viewport_dst_; }
    [[nodiscard]] const window_stats& stats() const { return stats_; }
    /// The last frame's damage_buffer rects (device px).
    [[nodiscard]] const lx::gfx::damage_region& last_damage() const { return last_damage_; }
    [[nodiscard]] const window_timeline& timeline() const { return timeline_; }
    /// The frame ring, oldest first through `frame_count()` entries (at most `k_frame_ring`).
    [[nodiscard]] const frame_record& frame_at(unsigned i) const;
    [[nodiscard]] unsigned frame_count() const;
    /// The last frame's draw list (device px).
    [[nodiscard]] const lx::scene::draw_list& last_draws() const { return *draws_; }
    /// The last presented image's pixels on the CPU tier (XRGB8888, `stride` bytes), else null.
    [[nodiscard]] const lx::gfx::pixel_surface* cpu_pixels() const;
    [[nodiscard]] lx::gfx::atlas& atlas() { return *atlas_; }

    /// Draw every frame (whole window damaged) while true — `lumen-hello --frames`.
    void set_continuous(bool on) { continuous_ = on; }
    /// The earliest deadline the application must wake for (a held key's repeat, a UI timer
    /// still ahead of `now_ns`); 0: none.
    [[nodiscard]] std::uint64_t next_deadline_ns(std::uint64_t now_ns) const;
    /// The application's timer fired.
    void on_deadline(std::uint64_t now_ns);

    // ── seat events for this window (the application routes them) ──
    void route_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) { input_.on_pointer(s, f); }
    void route_keyboard_focus(lx::wayland::seat& s, bool entered) { input_.on_keyboard_focus(s, entered); }
    void route_key(lx::wayland::seat& s, const lx::wayland::key_event& e) { input_.on_key(s, e); }
    void route_modifiers(lx::wayland::seat& s, std::uint32_t m) { input_.on_modifiers(s, m); }
    void route_repeat_info(lx::wayland::seat& s, std::int32_t r, std::int32_t d) { input_.on_repeat_info(s, r, d); }
    void route_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) { input_.on_touch(s, e); }

    // ── the frame source (lx::runtime::event_source over the window) ──
    [[nodiscard]] bool frame_due() const;
    void run_frame();

    // toplevel_handler
    void on_configure(lx::wayland::toplevel& t, const lx::wayland::toplevel_configure& c) override;
    void on_close(lx::wayland::toplevel& t) override;
    // surface_handler
    void on_frame(lx::wayland::surface& s, std::uint32_t time_ms) override;
    void on_preferred_scale(lx::wayland::surface& s, std::uint32_t scale120) override;
    void on_presentation(lx::wayland::surface& s, const lx::wayland::presentation_event& e) override;
    // buffer_handler
    void on_release(lx::wayland::buffer& b) override;
    void on_failed(lx::wayland::buffer& b) override;
    // swapchain_listener
    void on_image_created(const lx::gfx::swapchain_image& image) override;
    void on_image_destroyed(std::uint32_t id) override;

private:
    struct frame_source;
    struct buffer_slot {
        std::uint32_t id = 0;
        lx::wayland::buffer buffer{};
        lx::wayland::syncobj_timeline timeline{};
    };
    static constexpr unsigned k_buffer_slots =
        lx::gfx::k_swapchain_images + lx::gfx::k_swapchain_retired;

    [[nodiscard]] lx::result<void> make_pixel_path();
    /// Applies a pending configure and scale: bounds, scale, viewport, swapchain size.
    [[nodiscard]] lx::result<void> apply_geometry();
    [[nodiscard]] bool draw();
    void ack_pending();
    void restore_damage();
    [[nodiscard]] lx::result<void> render(lx::gfx::swapchain_image& image,
                                          const lx::gfx::damage_region& repaint);
    [[nodiscard]] buffer_slot* slot_of(std::uint32_t id);
    void collect_releases();
    [[nodiscard]] static std::uint64_t now_ns();

    window_host* host_ = nullptr;
    window_options options_{};
    bool open_ = false;
    bool broken_ = false;

    lx::wayland::surface surface_{};
    lx::wayland::toplevel toplevel_{};
    input_router input_{};

    std::unique_ptr<lx::runtime::memory_arena> arena_{};
    std::unique_ptr<lx::ui::ui_root> root_{};
    std::unique_ptr<lx::scene::draw_list> draws_{};
    std::unique_ptr<lx::gfx::atlas> atlas_{};
    std::unique_ptr<lx::text::glyph_cache> glyphs_{};
    std::unique_ptr<lx::ui::icon_cache> icons_{};
    std::uint64_t atlas_frame_ = 0;

    lx::gfx::client_gpu gpu_{};
    lx::gfx::format_choice format_{};
    lx::gfx::client_swapchain swapchain_{};
    lx::gfx::ui_cpu_renderer cpu_{};
    std::unique_ptr<lx::gfx::ui_gl_renderer> gl_{};
    std::unique_ptr<lx::gfx::ui_vk_renderer> vk_{};
    std::unique_ptr<frame_source> source_{};
    std::unique_ptr<lx::runtime::fd_source> release_source_{};
    buffer_slot buffers_[k_buffer_slots]{};
    const lx::gfx::swapchain_image* last_image_ = nullptr;

    // configure and scale
    lx::wayland::toplevel_configure pending_cfg_{};
    bool configure_pending_ = false;
    bool configured_ = false;
    bool scale_pending_ = false;
    bool fractional_ = false;
    bool viewport_ = false;
    lx::size2i logical_{};
    lx::size2i buffer_{};
    lx::size2i viewport_dst_{};
    float scale_ = 1.f;
    int buffer_scale_ = 1;

    bool waiting_release_ = false;
    bool continuous_ = false;
    /// The wp_linux_drm_syncobj_surface_v1 exists (explicit sync settled on).
    bool syncobj_ = false;
    std::uint64_t frame_start_ns_ = 0;

    frame_record ring_[k_frame_ring]{};
    std::uint64_t ring_next_ = 0;
    window_stats stats_{};
    lx::gfx::damage_region last_damage_{};
    window_timeline timeline_{};
};

} // namespace lx::app
