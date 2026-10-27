module;

// lx.app — the application runtime of ADR-010 Phase 4: one Wayland connection
// (lx.wayland.client), one lx::runtime::event_loop on the UI thread, and the windows drawn
// with the engine (`:window`, `:input`).
//
// The loop waits in epoll on: the display (`wayland::display_source`: prepare_read, flush,
// read or cancel — GTK's GSource and calloop's WaylandSource); a timerfd armed only while
// something needs a wake-up — a held key's repeat or a UI timer (a tooltip's delay, a caret's
// blink) — at that deadline, never on a period; the swapchain's release eventfd under
// explicit sync; and the theme file's inotify descriptor when hot reload is on. Each window is
// an event source of its own that is due when its frame loop wants a frame. An idle
// application has nothing armed and blocks with no timeout (G1). Work posted from the worker
// thread (icon rasters, a theme reload) lands on the loop's UI strand.
//
// The worker thread drains `lx::runtime::executor::global()`'s worker strand, woken by the
// strand's waker (no polling); it starts with the application when async icons or theme
// reload need it.

export module lx.app;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.ui;
import lx.wayland.client;

export import :input;
export import :window;

export namespace lx::app {

class clipboard {
public:
    void set_text(const char* text);
    [[nodiscard]] const char* text() const;
};

/// What the application's loop did (tests: G1's idle window counts these).
struct app_stats {
    std::uint64_t timer_arms = 0;        ///< the timerfd armed at a new deadline
    std::uint64_t timer_disarms = 0;     ///< disarmed (nothing to wake for)
    std::uint64_t timer_expirations = 0; ///< expirations read from the timerfd
    std::uint64_t theme_reloads = 0;     ///< theme file changes seen
    std::uint64_t worker_wakes = 0;      ///< times the worker thread woke for posted work
};

/// The start-up instants (CLOCK_MONOTONIC ns, §8.2 "Start-up"; 0: not reached).
struct app_timeline {
    /// The process's `exec`: `LUMEN_EXEC_NS` when the launcher set it (a bench taking its
    /// own clock right before execve), else the application's construction.
    std::uint64_t exec_ns = 0;
    bool exec_from_env = false;
    /// `start` began, and the connection's globals were bound.
    std::uint64_t start_ns = 0;
    std::uint64_t connect_ns = 0;
};

class application final : public window_host, public lx::wayland::seat_handler {
public:
    application(int argc, char* argv[]);
    ~application();
    application(const application&) = delete;
    application& operator=(const application&) = delete;

    /// Connects (`settings::socket`), loads the theme, starts the worker thread and puts the
    /// display on the loop. Called by `run` when not yet done; call it first to open windows
    /// before running. UI affinity (it sets the calling thread's).
    [[nodiscard]] lx::result<void> start();
    /// Runs the loop until `quit` (or the connection dies: exit code 1).
    [[nodiscard]] int run();
    void quit(int code = 0);
    /// Runs the loop until `done()` holds or `timeout_ms` passes (tests, and start-up
    /// sequences that wait for one event). True when `done()` held.
    template <typename F>
    bool run_until(F done, int timeout_ms);

    [[nodiscard]] lx::runtime::event_loop& event_loop() { return loop_; }
    [[nodiscard]] settings& config() { return settings_; }
    [[nodiscard]] clipboard& clipboard_service() { return clipboard_; }
    [[nodiscard]] lx::wayland::display& display() { return display_; }
    [[nodiscard]] const app_stats& stats() const { return stats_; }
    [[nodiscard]] const app_timeline& timeline() const { return timeline_; }
    /// The timerfd's armed deadline (0: disarmed).
    [[nodiscard]] std::uint64_t armed_deadline_ns() const;
    /// The display source's socket reads (diagnostics).
    [[nodiscard]] unsigned long long display_reads() const;
    [[nodiscard]] int argc() const { return argc_; }
    [[nodiscard]] char** argv() const { return argv_; }
    /// What a toplevel's close request does (default: `quit(0)`).
    void set_close_handler(void (*fn)(void* user, window& w), void* user) {
        close_fn_ = fn;
        close_user_ = user;
    }

    // window_host
    [[nodiscard]] lx::wayland::display& host_display() override { return display_; }
    [[nodiscard]] lx::runtime::event_loop& host_loop() override { return loop_; }
    [[nodiscard]] const settings& host_settings() const override { return settings_; }
    [[nodiscard]] lx::runtime::executor* host_worker() override;
    void host_close_requested(window& w) override;
    void host_attach(window& w) override;
    void host_detach(window& w) override;

    // seat_handler: routed to the window whose surface the event names.
    void on_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) override;
    void on_keyboard_focus(lx::wayland::seat& s, lx::wayland::surface* target, std::uint32_t serial,
                           bool entered) override;
    void on_key(lx::wayland::seat& s, const lx::wayland::key_event& e) override;
    void on_modifiers(lx::wayland::seat& s, std::uint32_t modifiers) override;
    void on_repeat_info(lx::wayland::seat& s, std::int32_t rate, std::int32_t delay_ms) override;
    void on_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) override;

private:
    struct deadline_source;
    struct worker_thread;

    void after_dispatch();
    void on_deadline();
    void load_theme();
    void stop_worker();

    lx::runtime::event_loop loop_{};
    settings settings_{};
    clipboard clipboard_{};
    lx::wayland::display display_{};
    std::unique_ptr<lx::wayland::display_source> display_source_{};
    std::unique_ptr<deadline_source> timer_{};
    std::unique_ptr<worker_thread> worker_{};
    lx::ui::theme_watcher watcher_{};
    std::unique_ptr<lx::runtime::fd_source> watcher_source_{};
    window* window_ = nullptr;
    lx::wayland::surface* keyboard_target_ = nullptr;
    void (*close_fn_)(void*, window&) = nullptr;
    void* close_user_ = nullptr;
    app_stats stats_{};
    app_timeline timeline_{};
    int argc_ = 0;
    char** argv_ = nullptr;
    bool started_ = false;
    bool connection_lost_ = false;
};

template <typename F>
bool application::run_until(F done, int timeout_ms) {
    if (done())
        return true;
    struct ctx {
        application* app;
        F* fn;
        bool met = false;
    } c{this, &done};
    // A 1 ms poll timer and a deadline: test plumbing, not the idle path.
    lx::runtime::timer_source poll{1'000'000, lx::runtime::task{[p = &c] {
                                       if ((*p->fn)()) {
                                           p->met = true;
                                           p->app->loop_.quit(0);
                                       }
                                   }}};
    lx::runtime::timer_source limit{0, lx::runtime::task{[p = &c] { p->app->loop_.quit(0); }},
                                    false};
    limit.schedule_at(loop_.now() + static_cast<long long>(timeout_ms) * 1'000'000);
    loop_.add_source(&poll);
    loop_.add_source(&limit);
    (void)loop_.run();
    loop_.remove_source(&limit);
    loop_.remove_source(&poll);
    return c.met || done();
}

} // namespace lx::app

// Convenience alias — must be exported for importers of lx.app.
export namespace lx {
using app::application;
}
