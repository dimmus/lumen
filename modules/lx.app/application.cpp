module;

// Bodies of lx.app's application: connection, loop sources, the timerfd, the worker thread,
// theme loading and hot reload, seat routing.

#include <errno.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

module lx.app;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.text;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::app {

namespace {

constexpr lx::trace::category log_app{"app"};

[[nodiscard]] std::uint64_t mono_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

// ── the timerfd: armed only at a deadline something needs ──

struct application::deadline_source final : public lx::runtime::event_source {
    explicit deadline_source(application& a) : app{a} {
        fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    }
    ~deadline_source() override {
        if (fd >= 0)
            ::close(fd);
    }
    bool prepare(lx::runtime::clock_time& timeout) override {
        timeout = -1;
        return ready;
    }
    bool check() override { return ready; }
    bool dispatch() override {
        ready = false;
        std::uint64_t n = 0;
        if (::read(fd, &n, sizeof(n)) == static_cast<ssize_t>(sizeof(n))) {
            app.stats_.timer_expirations += n;
            armed = 0; // a one-shot: fired
        }
        app.on_deadline();
        return true;
    }
    [[nodiscard]] int poll_fd() const override { return fd; }
    void on_readable() override { ready = true; }

    /// Absolute CLOCK_MONOTONIC `deadline` (0 disarms). A syscall only when it moves.
    void arm(std::uint64_t deadline) {
        if (fd < 0 || deadline == armed)
            return;
        itimerspec spec{};
        if (deadline != 0) {
            spec.it_value.tv_sec = static_cast<time_t>(deadline / 1'000'000'000ull);
            spec.it_value.tv_nsec = static_cast<long>(deadline % 1'000'000'000ull);
            ++app.stats_.timer_arms;
        } else {
            ++app.stats_.timer_disarms;
        }
        (void)::timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, nullptr);
        armed = deadline;
    }

    application& app;
    int fd = -1;
    std::uint64_t armed = 0;
    bool ready = false;
};

// ── the worker thread: drains the executor's worker strand, woken by its waker ──

struct application::worker_thread {
    explicit worker_thread(application& a) : app{a} {
        lx::runtime::strand& s = lx::runtime::executor::global().worker_strand();
        s.set_waker(&worker_thread::wake, this);
        thread = std::thread([this] { loop(); });
    }
    ~worker_thread() {
        {
            std::lock_guard<std::mutex> hold{mutex};
            running = false;
            pending = true;
        }
        cv.notify_one();
        if (thread.joinable())
            thread.join();
        lx::runtime::executor::global().worker_strand().set_waker(nullptr, nullptr);
    }
    static void wake(void* self) {
        auto* w = static_cast<worker_thread*>(self);
        {
            std::lock_guard<std::mutex> hold{w->mutex};
            w->pending = true;
        }
        w->cv.notify_one();
    }
    void loop() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::worker);
        lx::trace::name_thread("lx-app-worker");
        for (;;) {
            {
                std::unique_lock<std::mutex> hold{mutex};
                cv.wait(hold, [this] { return pending; });
                pending = false;
                if (!running)
                    return;
            }
            // The worker's own counter: app_stats is the UI thread's (read in `stats()`).
            wakes.fetch_add(1, std::memory_order_relaxed);
            lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
        }
    }

    application& app;
    std::mutex mutex{};
    std::condition_variable cv{};
    bool pending = false;
    bool running = true;
    std::atomic<std::uint64_t> wakes{0};
    std::thread thread{};
};

application::application(int argc, char* argv[]) : argc_{argc}, argv_{argv} {
    timeline_.exec_ns = mono_ns();
    if (const char* e = std::getenv("LUMEN_EXEC_NS"); e && *e) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(e, &end, 10);
        if (end && *end == '\0' && v > 0) {
            timeline_.exec_ns = v;
            timeline_.exec_from_env = true;
        }
    }
}

application::~application() {
    if (window_)
        window_->close();
    if (watcher_source_)
        loop_.remove_source(watcher_source_.get());
    watcher_source_.reset();
    if (timer_)
        loop_.remove_source(timer_.get());
    timer_.reset();
    if (display_source_)
        display_source_->detach();
    display_source_.reset();
    clipboard_.stop();
    stop_worker();
    if (lx::wayland::seat* s = display_.default_seat())
        s->set_handler(nullptr);
    display_.disconnect();
}

void application::stop_worker() { worker_.reset(); }

lx::runtime::executor* application::host_worker() {
    return worker_ ? &lx::runtime::executor::global() : nullptr;
}

lx::result<void> application::start() {
    if (started_)
        return {};
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    timeline_.start_ns = mono_ns();
    // The faces from the last run's fontconfig answers (`$XDG_CACHE_HOME/lumen/fonts-<app>`),
    // as lumen-shell does: the first frame's text does not wait for fontconfig to start
    // (≈ 20 ms of a cold start here, the larger part of G7's budget).
    lx::text::face_cache::shared().persist_matches();
    lx::wayland::connect_options co{};
    co.name = settings_.socket;
    if (auto c = display_.connect(co); !c) {
        log_app.error(c.get_error());
        return c;
    }
    timeline_.connect_ns = mono_ns();
    if (lx::wayland::seat* s = display_.default_seat())
        s->set_handler(this);
    // Without it a copy stays in this process (logged); a failure is not the app's end.
    (void)clipboard_.start(display_, loop_, this);

    // The worker: icon rasters and theme reloads (the only work posted to it here).
    bool watch = false;
#if !defined(NDEBUG)
    watch = true;
#endif
    if (settings_.watch_theme >= 0)
        watch = settings_.watch_theme == 1;
    if (const char* e = std::getenv("LUMEN_THEME_WATCH"); e && *e)
        watch = e[0] == '1';
    if (settings_.async_icons || watch)
        worker_ = std::make_unique<worker_thread>(*this);

    load_theme();
    if (watch) {
        // The file the active theme came from, by name or path.
        const char* name = settings_.theme ? settings_.theme : std::getenv("LUMEN_THEME");
        if (!name || !*name)
            name = "one-dark";
        char path[768]{};
        if (std::strchr(name, '/'))
            (void)std::snprintf(path, sizeof(path), "%s", name);
        else if (const char* dir = lx::ui::theme_dir(); dir && *dir)
            (void)std::snprintf(path, sizeof(path), "%s/%s.lxtheme", dir, name);
        if (path[0]) {
            if (auto w = watcher_.watch(path, &lx::runtime::executor::global()); w) {
                watcher_source_ = std::make_unique<lx::runtime::fd_source>(
                    watcher_.fd(), lx::runtime::task{[this] {
                        if (watcher_.poll())
                            ++stats_.theme_reloads;
                    }});
                loop_.add_source(watcher_source_.get());
                log_app.info("theme hot reload: watching {}", path);
            } else {
                log_app.warn("theme hot reload unavailable: {}", lx::format_error(w.get_error()));
            }
        }
    }

    display_source_ = std::make_unique<lx::wayland::display_source>(
        display_, loop_, lx::runtime::task{[this] {
            connection_lost_ = true;
            log_app.error("the Wayland connection is gone");
            loop_.quit(1);
        }});
    display_source_->attach();
    timer_ = std::make_unique<deadline_source>(*this);
    loop_.add_source(timer_.get());
    loop_.set_after_dispatch(lx::runtime::task{[this] { after_dispatch(); }});
    started_ = true;
    return {};
}

void application::load_theme() {
    const char* name = settings_.theme ? settings_.theme : std::getenv("LUMEN_THEME");
    if (!name || !*name)
        name = "one-dark";
    const bool is_path = std::strchr(name, '/') != nullptr;
    auto loaded = is_path ? lx::ui::theme::load(name) : lx::ui::load_theme_file(name);
    if (loaded) {
        lx::ui::set_active_theme(std::move(loaded).value());
        return;
    }
    // The copy compiled into lx.ui (One Dark / One Light from the same Zed file).
    const bool light = std::strstr(name, "light") != nullptr;
    lx::ui::set_active_builtin_theme(light ? lx::ui::appearance::light : lx::ui::appearance::dark);
    log_app.warn("theme '{}' not found ({}): the built-in {}", name,
                 lx::format_error(loaded.get_error()), light ? "One Light" : "One Dark");
}

int application::run() {
    if (!started_)
        if (auto s = start(); !s)
            return 1;
    const int code = loop_.run();
    return connection_lost_ ? 1 : code;
}

void application::quit(int code) { loop_.quit(code); }

app_stats application::stats() const {
    app_stats out = stats_;
    out.worker_wakes = worker_ ? worker_->wakes.load(std::memory_order_relaxed) : 0;
    return out;
}

std::uint64_t application::armed_deadline_ns() const { return timer_ ? timer_->armed : 0; }

unsigned long long application::display_reads() const {
    return display_source_ ? display_source_->reads() : 0;
}

void application::after_dispatch() {
    // Once per loop iteration: the timer follows the earliest deadline anything needs, and
    // is disarmed when nothing does (the idle rule).
    if (!timer_)
        return;
    const std::uint64_t now = mono_ns();
    timer_->arm(window_ ? window_->next_deadline_ns(now) : 0);
}

void application::on_deadline() {
    if (window_)
        window_->on_deadline(mono_ns());
}

lx::ui::ui_root* application::host_root_of(const lx::wayland::surface* s) {
    return window_ && s && &window_->surface() == s ? &window_->root() : nullptr;
}

lx::wayland::surface* application::host_drag_origin() {
    // The window the pointer is in: a drag starts from a press there.
    lx::wayland::seat* s = display_.default_seat();
    if (!window_ || !s || s->pointer_focus() != &window_->surface())
        return nullptr;
    return &window_->surface();
}

void application::host_close_requested(window& w) {
    if (close_fn_)
        close_fn_(close_user_, w);
    else
        quit(0);
}

lx::result<void> application::host_attach(window& w) {
    if (window_ && window_ != &w)
        return lx::invalid_argument("lx::app::window::open: the application has a window open "
                                    "(lx.ui keeps one tree per UI thread)");
    window_ = &w;
    return {};
}

void application::host_detach(window& w) {
    if (window_ != &w)
        return;
    window_ = nullptr;
    // The keyboard focus named the window's surface: a surface reopened at the same address
    // must not inherit it before its own enter.
    keyboard_target_ = nullptr;
}

// ── seat routing ──

void application::on_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) {
    // By target only: a null target is a surface this process does not know (destroyed, or
    // another library's) — never the window's.
    if (window_ && f.target == &window_->surface())
        window_->route_pointer(s, f);
}

void application::on_keyboard_focus(lx::wayland::seat& s, lx::wayland::surface* target,
                                    std::uint32_t serial, bool entered) {
    (void)serial;
    keyboard_target_ = entered ? target : nullptr;
    if (window_ && target == &window_->surface())
        window_->route_keyboard_focus(s, entered);
}

void application::on_key(lx::wayland::seat& s, const lx::wayland::key_event& e) {
    if (window_ && keyboard_target_ == &window_->surface())
        window_->route_key(s, e);
}

void application::on_modifiers(lx::wayland::seat& s, std::uint32_t modifiers) {
    if (window_)
        window_->route_modifiers(s, modifiers);
}

void application::on_repeat_info(lx::wayland::seat& s, std::int32_t rate, std::int32_t delay_ms) {
    if (window_)
        window_->route_repeat_info(s, rate, delay_ms);
}

void application::on_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) {
    if (window_ && (!e.target || e.target == &window_->surface()))
        window_->route_touch(s, e);
}

} // namespace lx::app
