module;

// lx.app:window — a toplevel window drawn with the ADR-010 engine: an lx.ui.host surface with
// the xdg_toplevel role (and a server-side decoration request). The surface does the rest —
// the ui_root, the client pixel path of §4.10 on the application's shared client_gpu, the
// frame loop of §4.9, configure, scale and idle (lx.ui.host:surface describes them); the
// window adds the toplevel's options and its close request.
//
// An application may open several windows: each has its own ui_root and ui_context (damage,
// overlays, timers, reducers), its own swapchain and frame loop; they share the application's
// client_gpu, renderer, atlas, glyph and icon caches (lx.ui.host:context).
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
import lx.ui.host;

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
    /// A render node's dev_t to choose the tier on instead of the compositor's main device
    /// (a headless compositor names none: the bench's automatic-tier start-up row). 0: the
    /// compositor's.
    std::uint64_t device_override = 0;
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
    /// The context the window's surface joins (the application's: one per UI thread).
    [[nodiscard]] virtual lx::ui::host::context& host_context() = 0;
    /// The window opens (refused: the open fails with the error) / is closing (a no-op for a
    /// window never admitted).
    [[nodiscard]] virtual lx::result<void> host_attach(window& w) = 0;
    virtual void host_detach(window& w) = 0;

protected:
    ~window_host() = default;
};

struct window_options {
    const char* title = "Lumen";
    const char* app_id = "org.lumen.app";
    /// Logical px, used when the compositor's first configure leaves the size to the client.
    lx::size2i size{640, 480};
    /// xdg_toplevel.set_min_size, and the floor of a size the client picks. Never above a
    /// maximized, tiled, fullscreen or resizing configure's size (xdg-shell).
    lx::size2i min_size{};
    /// The UI. Required.
    lx::ui::describe_fn describe = nullptr;
    bool server_side_decorations = true;
    /// An opaque window (XRGB buffers, an opaque region): the compositor blends nothing.
    bool opaque = true;
    /// The UI arena (descriptors, per frame).
    std::size_t arena_bytes = 512 * 1024;
    /// wp_presentation feedback for every frame (the frame ring's presented times,
    /// `stats().presented`). Off: only until the first frame is presented (the timeline's
    /// `first_presented_ns`) — each request is a proxy, its events and a delete_id.
    bool presentation_feedback = false;
};

/// One committed frame's record, the window's statistics and timeline: the surface's
/// (lx.ui.host:surface).
using frame_record = lx::ui::host::frame_record;
using lx::ui::host::k_frame_ring;
using window_stats = lx::ui::host::surface_stats;
using window_timeline = lx::ui::host::surface_timeline;

class window final {
public:
    window();
    ~window();
    window(const window&) = delete;
    window& operator=(const window&) = delete;

    /// Creates the surface and the toplevel on the host's context, and the first commit;
    /// the context's pixel tier is picked with the first window. The first frame is drawn
    /// when the first configure arrives. Fails (invalid_argument) when the host refuses it
    /// (its window table is full) or without a describe function.
    [[nodiscard]] lx::result<void> open(window_host& host, const window_options& options);
    void close();
    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] lx::ui::ui_root& root() { return surface_.root(); }
    [[nodiscard]] input_router& input() { return surface_.input(); }
    [[nodiscard]] lx::wayland::surface& surface() { return surface_.wl(); }
    [[nodiscard]] lx::wayland::toplevel& toplevel() { return role_.toplevel(); }
    /// The lx.ui.host surface the window is.
    [[nodiscard]] lx::ui::host::surface& host_surface() { return surface_; }
    [[nodiscard]] lx::gfx::client_tier tier() const { return surface_.tier(); }
    [[nodiscard]] const lx::gfx::client_gpu& gpu() const { return surface_.gpu(); }
    [[nodiscard]] const lx::gfx::client_swapchain& swapchain() const { return surface_.swapchain(); }
    [[nodiscard]] lx::size2i logical_size() const { return surface_.logical_size(); }
    [[nodiscard]] lx::size2i buffer_size() const { return surface_.buffer_size(); }
    [[nodiscard]] float scale() const { return surface_.scale(); }
    /// wl_surface.buffer_scale in use (1 with a viewport).
    [[nodiscard]] int buffer_scale() const { return surface_.buffer_scale(); }
    /// The viewport's destination (0 × 0: no viewport).
    [[nodiscard]] lx::size2i viewport_destination() const { return surface_.viewport_destination(); }
    [[nodiscard]] const window_stats& stats() const { return surface_.stats(); }
    /// The last frame's damage_buffer rects (device px).
    [[nodiscard]] const lx::gfx::damage_region& last_damage() const { return surface_.last_damage(); }
    [[nodiscard]] const window_timeline& timeline() const { return surface_.timeline(); }
    /// The frame ring, oldest first through `frame_count()` entries (at most `k_frame_ring`).
    [[nodiscard]] const frame_record& frame_at(unsigned i) const { return surface_.frame_at(i); }
    [[nodiscard]] unsigned frame_count() const { return surface_.frame_count(); }
    /// The last frame's draw list (device px).
    [[nodiscard]] const lx::scene::draw_list& last_draws() const { return surface_.last_draws(); }
    /// The last presented image's pixels on the CPU tier (XRGB8888, `stride` bytes), else null.
    [[nodiscard]] const lx::gfx::pixel_surface* cpu_pixels() const { return surface_.cpu_pixels(); }
    /// The application's atlas (shared by its windows).
    [[nodiscard]] lx::gfx::atlas& atlas() { return surface_.atlas(); }

    /// Draw every frame (whole window damaged) while true — `lumen-hello --frames`.
    void set_continuous(bool on) { surface_.set_continuous(on); }
    /// `window_options::presentation_feedback`, from the next frame.
    void set_presentation_feedback(bool on) { surface_.set_presentation_feedback(on); }
    /// The earliest deadline the application must wake for (a held key's repeat, a UI timer
    /// still ahead of `now_ns`); 0: none.
    [[nodiscard]] std::uint64_t next_deadline_ns(std::uint64_t now_ns) const {
        return surface_.next_deadline_ns(now_ns);
    }
    /// The application's timer fired.
    void on_deadline(std::uint64_t now_ns) { surface_.on_deadline(now_ns); }

    // ── seat events for this window (the application's context routes them) ──
    void route_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) { surface_.route_pointer(s, f); }
    void route_keyboard_focus(lx::wayland::seat& s, bool entered) { surface_.route_keyboard_focus(s, entered); }
    void route_key(lx::wayland::seat& s, const lx::wayland::key_event& e) { surface_.route_key(s, e); }
    void route_modifiers(lx::wayland::seat& s, std::uint32_t m) { surface_.route_modifiers(s, m); }
    void route_repeat_info(lx::wayland::seat& s, std::int32_t r, std::int32_t d) { surface_.route_repeat_info(s, r, d); }
    void route_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) { surface_.route_touch(s, e); }

    // ── the frame source ──
    [[nodiscard]] bool frame_due() const { return surface_.frame_due(); }
    void run_frame() { surface_.run_frame(); }

private:
    static void close_requested(void* self);

    window_host* host_ = nullptr;
    bool open_ = false;
    bool attached_ = false;
    lx::ui::host::surface surface_{};
    lx::ui::host::toplevel_role role_{};
    /// The surface's frame ring, kept in the window (where it always was).
    frame_record ring_[k_frame_ring]{};
};

} // namespace lx::app
