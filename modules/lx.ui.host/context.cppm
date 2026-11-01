module;

// lx.ui.host:context — what one UI thread's surfaces share (ADR-010 §7 Phase 5; GTK's
// GdkDisplay with its GL context and glyph cache, Qt's QWaylandDisplay).
//
// Once per context, for every surface of it:
// - the pixel path: ONE `lx::gfx::client_gpu` (its tier chosen once from the compositor's
//   dma-buf feedback, on the first surface's `show`), ONE renderer of that tier (built with
//   the first swapchain), ONE atlas with its glyph and icon caches (made with the first
//   surface), installed as lx.ui's resources. A renderer re-uploads every atlas page when it sees a different atlas, so
//   sharing the renderer means sharing the atlas;
// - the theme (`context_options::theme`), its hot reload, and the worker thread icon rasters
//   and theme loads go to (none: synchronous);
// - ONE timerfd, armed at the earliest deadline any surface needs (a held key's repeat, a UI
//   timer, a configure held for a frame callback) and disarmed when none does (G1);
// - the seat router: pointer and touch by the event's target surface, keys to the surface
//   with the keyboard focus, modifiers and repeat info to every surface (one seat state,
//   replayed to a surface created later), and ONE zwp_text_input_v3 per seat, its enter and
//   done routed to the surface it names.
//
// The connection and the loop are the caller's (lx.app's application, the shell): the
// context borrows both and must be stopped before either goes. `start` sets the loop's
// after-dispatch hook and the default seat's handler.
//
// UI affinity throughout.

export module lx.ui.host:context;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.text;
import lx.ui;
import lx.wayland.client;

import :input;
import :surface;

export namespace lx::ui::host {

struct context_options {
    /// The pixel path: `none` picks as the compositor does (hardware Vulkan → hardware GL →
    /// CPU, §4.10); `LUMEN_UI_BACKEND` overrides it (`client_gpu`). A forced tier the host
    /// cannot run falls back to the automatic choice.
    lx::gfx::client_tier backend = lx::gfx::client_tier::none;
    /// Let a forced tier run on a software device (lavapipe, llvmpipe): tests only.
    bool allow_software_gpu = false;
    /// A render node's dev_t to choose the tier on instead of the compositor's main device.
    std::uint64_t device_override = 0;
    /// "one-dark" (null: `LUMEN_THEME`, else One Dark), "one-light", or a `.lxtheme` path.
    const char* theme = nullptr;
    /// Load the theme at `start` (off: the caller's, or whatever is active).
    bool load_theme = true;
    /// Reload the theme file when it changes: -1 debug builds only (`LUMEN_THEME_WATCH=1` /
    /// `0` overrides), 0 never, 1 always.
    int watch_theme = -1;
    /// Rasterise icon misses on the worker thread (else on the UI thread, synchronously).
    /// The worker thread runs only when this or the theme watch needs it.
    bool async_icons = true;
};

/// What the context's loop hooks did (tests: G1's idle window counts these).
struct context_stats {
    std::uint64_t timer_arms = 0;        ///< the timerfd armed at a new deadline
    std::uint64_t timer_disarms = 0;     ///< disarmed (nothing to wake for)
    std::uint64_t timer_expirations = 0; ///< expirations read from the timerfd
    std::uint64_t theme_reloads = 0;     ///< theme file changes seen
    std::uint64_t worker_wakes = 0;      ///< times the worker thread woke for posted work
};

/// Seat input for a surface the context does not host (the caller's own canvas views — the
/// shell's launcher, notification stack, switcher and menus draw without an lx.ui root):
/// every pointer frame whose target is such a surface, the keyboard focus entering or
/// leaving one, and the keys pressed while one has the focus. Installed with
/// `set_unrouted_input_handler`; the context still tracks the keyboard target so the keys
/// keep arriving while it stays there. UI affinity.
class unrouted_input_handler {
public:
    virtual ~unrouted_input_handler() = default;
    virtual void on_unrouted_pointer(lx::wayland::seat& s,
                                     const lx::wayland::pointer_frame& f) = 0;
    virtual void on_unrouted_keyboard_focus(lx::wayland::seat& s, lx::wayland::surface* target,
                                            std::uint32_t serial, bool entered) = 0;
    virtual void on_unrouted_key(lx::wayland::seat& s, const lx::wayland::key_event& e) = 0;
};

class context final : public lx::wayland::seat_handler,
                      public lx::wayland::text_input_handler {
public:
    /// Surfaces one context drives at most (a shell's panels, menus and notifications).
    static constexpr unsigned k_max_surfaces = 32;

    context();
    ~context() override;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    /// On a connected display, on the calling thread (UI affinity): the theme, the worker,
    /// the timer, the seat routing and the text input (the caches come with the first
    /// surface).
    [[nodiscard]] lx::result<void> start(lx::wayland::display& display,
                                         lx::runtime::event_loop& loop,
                                         const context_options& options = {});
    /// Destroys every surface still attached, then everything `start` made. Idempotent.
    void stop();
    [[nodiscard]] bool started() const { return started_; }

    [[nodiscard]] lx::wayland::display& display() const { return *display_; }
    [[nodiscard]] lx::runtime::event_loop& loop() const { return *loop_; }
    [[nodiscard]] const context_options& options() const { return options_; }
    /// The worker executor icon rasters go to (null: synchronous).
    [[nodiscard]] lx::runtime::executor* worker();
    [[nodiscard]] context_stats stats() const;
    /// The timerfd's armed deadline (0: disarmed).
    [[nodiscard]] std::uint64_t armed_deadline_ns() const;

    // ── the shared pixel path ──

    /// The client_gpu: made on first call (dma-buf feedback → tier; a forced tier this host
    /// cannot run → automatic; a GPU tier with no dma-buf format for `opaque` → cpu).
    [[nodiscard]] lx::result<void> ensure_gpu(bool opaque);
    [[nodiscard]] lx::gfx::client_gpu& gpu() { return gpu_; }
    [[nodiscard]] const lx::gfx::client_gpu& gpu() const { return gpu_; }
    [[nodiscard]] bool gpu_ready() const { return gpu_made_; }
    /// The dma-buf format for an opaque or translucent swapchain of the GPU tier (none on
    /// cpu).
    [[nodiscard]] lx::result<lx::gfx::format_choice> format_for(bool opaque) const;
    /// The tier's renderer, built on first call (GL: with a context current; no program built
    /// ahead, each ≈ 1.8 MiB of driver memory on SVGA3D).
    [[nodiscard]] lx::result<void> ensure_renderer();
    [[nodiscard]] lx::gfx::ui_cpu_renderer& cpu_renderer() { return cpu_; }
    [[nodiscard]] lx::gfx::ui_gl_renderer* gl_renderer() { return gl_.get(); }
    [[nodiscard]] lx::gfx::ui_vk_renderer* vk_renderer() { return vk_.get(); }
    [[nodiscard]] lx::gfx::atlas& atlas() { return *atlas_; }
    [[nodiscard]] lx::text::glyph_cache& glyphs() { return *glyphs_; }
    [[nodiscard]] lx::ui::icon_cache& icons() { return *icons_; }
    /// The atlas frame a surface's draw stamps its tiles with (monotonic over every surface
    /// of the context: a tile any surface drew recently is not stale).
    [[nodiscard]] std::uint64_t next_atlas_frame() { return ++atlas_frame_; }

    // ── surfaces ──

    /// A surface joins (`surface::create`): seat events naming it, deadlines and the seat's
    /// modifiers reach it. False with the table full.
    [[nodiscard]] bool attach(surface& s);
    void detach(surface& s);
    [[nodiscard]] unsigned surface_count() const { return count_; }
    [[nodiscard]] surface* surface_at(unsigned i) const { return i < count_ ? surfaces_[i] : nullptr; }
    /// The context's surface on `wl` (null: not one of this context's).
    [[nodiscard]] surface* surface_of(const lx::wayland::surface* wl) const;
    /// The surface with the keyboard focus (null: none of ours).
    [[nodiscard]] surface* keyboard_focus() const;
    /// The surface the pointer is over (null: none of ours).
    [[nodiscard]] surface* pointer_focus() const;
    /// The seat's text input (null: no zwp_text_input_manager_v3 or no seat).
    [[nodiscard]] lx::wayland::text_input* text_input();
    /// The serial of the last button press, key press or touch down on one of the context's
    /// surfaces: what an xdg_popup grab or an interactive move takes (0: none yet).
    [[nodiscard]] std::uint32_t last_press_serial() const { return press_serial_; }

    /// Seat input for surfaces the context does not host (the caller's canvas views); see
    /// `unrouted_input_handler`. Null (the default): such frames are dropped.
    void set_unrouted_input_handler(unrouted_input_handler* h) { unrouted_ = h; }

    // seat_handler: routed to the surface the event names.
    void on_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) override;
    void on_keyboard_focus(lx::wayland::seat& s, lx::wayland::surface* target, std::uint32_t serial,
                           bool entered) override;
    void on_key(lx::wayland::seat& s, const lx::wayland::key_event& e) override;
    void on_modifiers(lx::wayland::seat& s, std::uint32_t modifiers) override;
    void on_repeat_info(lx::wayland::seat& s, std::int32_t rate, std::int32_t delay_ms) override;
    void on_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) override;
    // text_input_handler: routed to the surface the enter named.
    void on_focus(lx::wayland::text_input& t, lx::wayland::surface* target, bool entered) override;
    void on_done(lx::wayland::text_input& t, const lx::wayland::text_input_done& d) override;

private:
    struct deadline_source;
    struct worker_thread;

    void after_dispatch();
    void on_deadline();
    void load_theme();
    void ensure_caches();

    lx::wayland::display* display_ = nullptr;
    lx::runtime::event_loop* loop_ = nullptr;
    context_options options_{};
    bool started_ = false;

    // the pixel path
    lx::gfx::client_gpu gpu_{};
    bool gpu_made_ = false;
    std::vector<lx::gfx::client_format> formats_{};
    std::vector<lx::gfx::client_tranche> tranches_{};
    lx::gfx::client_feedback feedback_{};
    lx::gfx::ui_cpu_renderer cpu_{};
    std::unique_ptr<lx::gfx::ui_gl_renderer> gl_{};
    std::unique_ptr<lx::gfx::ui_vk_renderer> vk_{};
    std::unique_ptr<lx::gfx::atlas> atlas_{};
    std::unique_ptr<lx::text::glyph_cache> glyphs_{};
    std::unique_ptr<lx::ui::icon_cache> icons_{};
    std::uint64_t atlas_frame_ = 0;

    // loop hooks
    std::unique_ptr<deadline_source> timer_{};
    std::unique_ptr<worker_thread> worker_{};
    lx::ui::theme_watcher watcher_{};
    std::unique_ptr<lx::runtime::fd_source> watcher_source_{};
    context_stats stats_{};

    // surfaces and the seat
    surface* surfaces_[k_max_surfaces]{};
    unsigned count_ = 0;
    lx::wayland::seat* seat_ = nullptr;
    lx::wayland::text_input text_input_{};
    lx::wayland::surface* keyboard_target_ = nullptr;
    lx::wayland::surface* touch_target_ = nullptr;
    unrouted_input_handler* unrouted_ = nullptr;
    std::uint32_t press_serial_ = 0;
    /// The seat's modifier and repeat state, replayed to a surface that joins later.
    std::uint32_t modifiers_ = 0;
    bool have_modifiers_ = false;
    std::int32_t repeat_rate_ = 0;
    std::int32_t repeat_delay_ms_ = 0;
    bool have_repeat_ = false;
};

} // namespace lx::ui::host
