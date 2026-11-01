module;

// Bodies of lx.app's application: the connection, the display source, the lx.ui.host
// context (theme, worker, timer, seat routing), the clipboard and the windows it holds.

#include <stdlib.h>

module lx.app;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
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
    // Windows still open close first (each detaches itself), then the context they shared.
    while (window_count_ > 0)
        windows_[window_count_ - 1]->close();
    context_.stop();
    if (display_source_)
        display_source_->detach();
    display_source_.reset();
    clipboard_.stop();
    display_.disconnect();
}

lx::runtime::executor* application::host_worker() { return context_.worker(); }

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
    // The context: the theme, the worker (icon rasters, theme reloads), the timer, the seat
    // routing and the caches every window shares.
    lx::ui::host::context_options ho{};
    ho.backend = settings_.backend;
    ho.allow_software_gpu = settings_.allow_software_gpu;
    ho.device_override = settings_.device_override;
    ho.theme = settings_.theme;
    ho.watch_theme = settings_.watch_theme;
    ho.async_icons = settings_.async_icons;
    if (auto c = context_.start(display_, loop_, ho); !c) {
        log_app.error(c.get_error());
        return c;
    }
    // Without it a copy stays in this process (logged); a failure is not the app's end.
    (void)clipboard_.start(display_, loop_, this);

    display_source_ = std::make_unique<lx::wayland::display_source>(
        display_, loop_, lx::runtime::task{[this] {
            connection_lost_ = true;
            log_app.error("the Wayland connection is gone");
            loop_.quit(1);
        }});
    display_source_->attach();
    started_ = true;
    return {};
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
    const lx::ui::host::context_stats c = context_.stats();
    app_stats out{};
    out.timer_arms = c.timer_arms;
    out.timer_disarms = c.timer_disarms;
    out.timer_expirations = c.timer_expirations;
    out.theme_reloads = c.theme_reloads;
    out.worker_wakes = c.worker_wakes;
    return out;
}

std::uint64_t application::armed_deadline_ns() const { return context_.armed_deadline_ns(); }

unsigned long long application::display_reads() const {
    return display_source_ ? display_source_->reads() : 0;
}

lx::ui::ui_root* application::host_root_of(const lx::wayland::surface* s) {
    lx::ui::host::surface* hs = context_.surface_of(s);
    return hs ? &hs->root() : nullptr;
}

lx::wayland::surface* application::host_drag_origin() {
    // The window the pointer is in: a drag starts from a press there.
    lx::ui::host::surface* hs = context_.pointer_focus();
    return hs ? &hs->wl() : nullptr;
}

void application::host_close_requested(window& w) {
    if (close_fn_)
        close_fn_(close_user_, w);
    else
        quit(0);
}

lx::result<void> application::host_attach(window& w) {
    for (unsigned i = 0; i < window_count_; ++i)
        if (windows_[i] == &w)
            return {};
    if (window_count_ == k_max_windows)
        return lx::invalid_argument("lx::app::window::open: the application has k_max_windows "
                                    "windows open");
    windows_[window_count_++] = &w;
    return {};
}

void application::host_detach(window& w) {
    for (unsigned i = 0; i < window_count_; ++i)
        if (windows_[i] == &w) {
            for (unsigned j = i + 1; j < window_count_; ++j)
                windows_[j - 1] = windows_[j];
            windows_[--window_count_] = nullptr;
            return;
        }
}

} // namespace lx::app
