module;

// Bodies of lx.ui.host:context: the shared pixel path and caches, the theme and its reload,
// the worker thread, the timerfd, seat and text-input routing (moved from lx.app's
// application and window in Phase 5).

#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

module lx.ui.host;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.text;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::ui::host {

namespace {

constexpr lx::trace::category log_host{"ui.host"};

[[nodiscard]] std::uint64_t mono_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

// ── the timerfd: armed only at a deadline something needs ──

struct context::deadline_source final : public lx::runtime::event_source {
    explicit deadline_source(context& c) : ctx{c} {
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
            ctx.stats_.timer_expirations += n;
            armed = 0; // a one-shot: fired
        }
        ctx.on_deadline();
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
            ++ctx.stats_.timer_arms;
        } else {
            ++ctx.stats_.timer_disarms;
        }
        (void)::timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, nullptr);
        armed = deadline;
    }

    context& ctx;
    int fd = -1;
    std::uint64_t armed = 0;
    bool ready = false;
};

// ── the worker thread: drains the executor's worker strand, woken by its waker ──

struct context::worker_thread {
    worker_thread() {
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
            // The worker's own counter: context_stats is the UI thread's (read in `stats()`).
            wakes.fetch_add(1, std::memory_order_relaxed);
            lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
        }
    }

    std::mutex mutex{};
    std::condition_variable cv{};
    bool pending = false;
    bool running = true;
    std::atomic<std::uint64_t> wakes{0};
    std::thread thread{};
};

context::context() = default;

context::~context() { stop(); }

lx::result<void> context::start(lx::wayland::display& display, lx::runtime::event_loop& loop,
                                const context_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (started_)
        return lx::invalid_argument("lx::ui::host::context::start: already started");
    if (!display.connected())
        return lx::make_error(lx::wayland_err::connect_failed,
                              "lx::ui::host::context::start: the display is not connected");
    display_ = &display;
    loop_ = &loop;
    options_ = options;
    stats_ = {};

    // The worker: icon rasters and theme reloads (the only work posted to it here).
    bool watch = false;
#ifndef NDEBUG
    watch = true;
#endif
    if (options.watch_theme >= 0)
        watch = options.watch_theme == 1;
    if (const char* e = std::getenv("LUMEN_THEME_WATCH"); e && *e)
        watch = e[0] == '1';
    if (!options.load_theme)
        watch = false;
    if (options.async_icons || watch)
        worker_ = std::make_unique<worker_thread>();

    if (options.load_theme)
        load_theme();
    if (watch) {
        // The file the active theme came from, by name or path.
        const char* name = options.theme ? options.theme : std::getenv("LUMEN_THEME");
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
                loop_->add_source(watcher_source_.get());
                log_host.info("theme hot reload: watching {}", path);
            } else {
                log_host.warn("theme hot reload unavailable: {}", lx::format_error(w.get_error()));
            }
        }
    }

    // The seat: its events routed by target surface; one text input for it.
    seat_ = display.default_seat();
    if (seat_) {
        seat_->set_handler(this);
        if (display.has(lx::wayland::global_kind::text_input)) {
            if (auto made = text_input_.create(display, *seat_); made)
                text_input_.set_handler(this);
            else
                log_host.warn("zwp_text_input_v3 unavailable: {}", lx::format_error(made.get_error()));
        }
    }

    timer_ = std::make_unique<deadline_source>(*this);
    loop_->add_source(timer_.get());
    loop_->set_after_dispatch(lx::runtime::task{[this] { after_dispatch(); }});
    started_ = true;
    return {};
}

void context::stop() {
    if (!started_)
        return;
    // Every surface still attached goes first (its swapchain on the shared gpu).
    while (count_ > 0)
        surfaces_[count_ - 1]->destroy();
    loop_->set_after_dispatch(lx::runtime::task{});
    if (watcher_source_)
        loop_->remove_source(watcher_source_.get());
    watcher_source_.reset();
    if (timer_)
        loop_->remove_source(timer_.get());
    timer_.reset();
    text_input_.destroy();
    // The seat as the display has it now (one removed since `start` is gone).
    if (lx::wayland::seat* seat = display_->default_seat(); seat && seat == seat_)
        seat->set_handler(nullptr);
    seat_ = nullptr;
    keyboard_target_ = touch_target_ = nullptr;
    press_serial_ = 0;
    if (vk_)
        vk_->wait_idle();
    vk_.reset();
    gl_.reset();
    gpu_.destroy();
    gpu_made_ = false;
    if (atlas_)
        lx::ui::set_ui_resources({});
    icons_.reset();
    glyphs_.reset();
    atlas_.reset();
    worker_.reset();
    formats_.clear();
    tranches_.clear();
    feedback_ = {};
    have_modifiers_ = have_repeat_ = false;
    started_ = false;
}

void context::load_theme() {
    const char* name = options_.theme ? options_.theme : std::getenv("LUMEN_THEME");
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
    log_host.warn("theme '{}' not found ({}): the built-in {}", name,
                  lx::format_error(loaded.get_error()), light ? "One Light" : "One Dark");
}

lx::runtime::executor* context::worker() {
    return worker_ ? &lx::runtime::executor::global() : nullptr;
}

context_stats context::stats() const {
    context_stats out = stats_;
    out.worker_wakes = worker_ ? worker_->wakes.load(std::memory_order_relaxed) : 0;
    return out;
}

std::uint64_t context::armed_deadline_ns() const { return timer_ ? timer_->armed : 0; }

// ── the shared pixel path (ADR-010 §4.10) ──

lx::result<void> context::ensure_gpu(bool opaque) {
    if (gpu_made_)
        return {};
    // The compositor's dma-buf feedback (default; the main device and its tranches), copied
    // into the plain values lx.gfx takes (kept: each surface picks its format from them).
    formats_.clear();
    tranches_.clear();
    feedback_ = {};
    if (lx::wayland::dmabuf_feedback* fb = display_->default_feedback(); fb && fb->received()) {
        const lx::wayland::dmabuf_feedback_snapshot& s = fb->snapshot();
        std::size_t total = 0;
        for (const auto& t : s.list())
            total += t.count;
        formats_.reserve(total);
        for (const auto& t : s.list()) {
            const std::size_t first = formats_.size();
            for (const auto& f : t.entries())
                formats_.push_back({f.format, f.modifier});
            tranches_.push_back({t.target_device, t.scanout(),
                                 std::span<const lx::gfx::client_format>{formats_.data() + first, t.count}});
        }
        feedback_ = {s.main_device, tranches_};
    }
    lx::gfx::client_gpu_options o{};
    o.force = options_.backend;
    o.allow_software = options_.allow_software_gpu;
    o.device_override = options_.device_override;
    auto made = gpu_.create(feedback_, o);
    if (!made && options_.backend != lx::gfx::client_tier::none) {
        // A forced tier this host cannot run: the automatic choice, as GTK falls back from
        // an unavailable GSK_RENDERER.
        log_host.warn("{} tier unavailable ({}), choosing automatically",
                      lx::gfx::client_tier_name(options_.backend), lx::format_error(made.get_error()));
        o.force = lx::gfx::client_tier::none;
        o.read_environment = false;
        made = gpu_.create(feedback_, o);
    }
    if (!made)
        return made.get_error();
    if (gpu_.tier() != lx::gfx::client_tier::cpu) {
        auto chosen = lx::gfx::choose_client_format(feedback_, {.opaque = opaque}, 0);
        if (!chosen) {
            log_host.warn("no dma-buf format for the {} tier ({}): wl_shm",
                          lx::gfx::client_tier_name(gpu_.tier()), lx::format_error(chosen.get_error()));
            gpu_.destroy();
            if (auto failed = gpu_.create(feedback_, {.force = lx::gfx::client_tier::cpu}); !failed)
                return failed.get_error();
        }
    }
    gpu_made_ = true;
    return {};
}

lx::result<lx::gfx::format_choice> context::format_for(bool opaque) const {
    if (!gpu_made_ || gpu_.tier() == lx::gfx::client_tier::cpu)
        return lx::gfx::format_choice{};
    return lx::gfx::choose_client_format(feedback_, {.opaque = opaque}, 0);
}

lx::result<void> context::ensure_renderer() {
    if (gpu_.tier() == lx::gfx::client_tier::gl && !gl_) {
        auto gl = std::make_unique<lx::gfx::ui_gl_renderer>();
        // No program built ahead: the first draw builds what its stream needs (a hello
        // window: quads and glyphs), each program ≈ 1.8 MiB of driver memory on SVGA3D
        // and its compile the larger part of a cold first frame.
        lx::gfx::ui_gl_options go{};
        go.prebuild_kinds = 0;
        if (auto failed = gl->initialize(go); !failed)
            return failed.get_error();
        gl->set_owner_affinity(lx::runtime::affinity::ui);
        gl_ = std::move(gl);
    } else if (gpu_.tier() == lx::gfx::client_tier::vulkan && !vk_) {
        auto vk = std::make_unique<lx::gfx::ui_vk_renderer>();
        lx::gfx::ui_vk_options vo{};
        vo.format = lx::gfx::k_ui_vk_bgra8_unorm;
        if (auto failed = vk->initialize(gpu_.vulkan_context(), vo); !failed)
            return failed.get_error();
        vk->set_owner_affinity(lx::runtime::affinity::ui);
        vk_ = std::move(vk);
    }
    return {};
}

// ── surfaces ──

void context::ensure_caches() {
    if (atlas_)
        return;
    // The caches every surface of the context draws with: one atlas (each renderer uploads
    // it), the glyphs and icons in it, installed as lx.ui's resources. Made with the first
    // surface (≈ 1.8 MiB resident): a context that never shows one pays nothing for them.
    atlas_ = std::make_unique<lx::gfx::atlas>();
    glyphs_ = std::make_unique<lx::text::glyph_cache>(*atlas_);
    icons_ = std::make_unique<lx::ui::icon_cache>(*atlas_);
    if (options_.async_icons)
        icons_->set_async(worker());
    lx::ui::ui_resources resources{};
    resources.text = {{}, glyphs_.get(), nullptr};
    resources.icons = icons_.get();
    lx::ui::set_ui_resources(resources);
}

bool context::attach(surface& s) {
    for (unsigned i = 0; i < count_; ++i)
        if (surfaces_[i] == &s)
            return true;
    if (count_ == k_max_surfaces)
        return false;
    ensure_caches();
    surfaces_[count_++] = &s;
    // The seat's state as it is: a surface made after the modifiers or repeat info came.
    if (lx::wayland::seat* seat = display_->default_seat()) {
        if (have_repeat_)
            s.route_repeat_info(*seat, repeat_rate_, repeat_delay_ms_);
        if (have_modifiers_)
            s.route_modifiers(*seat, modifiers_);
    }
    return true;
}

void context::detach(surface& s) {
    for (unsigned i = 0; i < count_; ++i)
        if (surfaces_[i] == &s) {
            // Order kept: the deadline and routing scans see surfaces as they were made.
            for (unsigned j = i + 1; j < count_; ++j)
                surfaces_[j - 1] = surfaces_[j];
            surfaces_[--count_] = nullptr;
            break;
        }
    // The focus named the surface: one reopened at the same address must not inherit it
    // before its own enter.
    if (keyboard_target_ && keyboard_target_ == &s.wl())
        keyboard_target_ = nullptr;
    if (touch_target_ && touch_target_ == &s.wl())
        touch_target_ = nullptr;
}

surface* context::surface_of(const lx::wayland::surface* wl) const {
    if (!wl)
        return nullptr;
    for (unsigned i = 0; i < count_; ++i)
        if (&surfaces_[i]->wl() == wl)
            return surfaces_[i];
    return nullptr;
}

surface* context::keyboard_focus() const { return surface_of(keyboard_target_); }

surface* context::pointer_focus() const {
    lx::wayland::seat* seat = display_ ? display_->default_seat() : nullptr;
    return seat ? surface_of(seat->pointer_focus()) : nullptr;
}

lx::wayland::text_input* context::text_input() {
    return text_input_.valid() ? &text_input_ : nullptr;
}

// ── loop hooks ──

void context::after_dispatch() {
    // Once per loop iteration: the timer follows the earliest deadline any surface needs,
    // and is disarmed when none does (the idle rule).
    if (!timer_)
        return;
    const std::uint64_t now = mono_ns();
    std::uint64_t earliest = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (const std::uint64_t d = surfaces_[i]->next_deadline_ns(now); d != 0 && (earliest == 0 || d < earliest))
            earliest = d;
    timer_->arm(earliest);
}

void context::on_deadline() {
    const std::uint64_t now = mono_ns();
    for (unsigned i = 0; i < count_; ++i)
        surfaces_[i]->on_deadline(now);
}

// ── seat routing ──

void context::on_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) {
    // By target only: a null target is a surface this process does not know (destroyed, or
    // another library's) — never one of ours.
    if (surface* to = surface_of(f.target)) {
        if ((f.mask & lx::wayland::pointer_frame::has_button) && f.pressed)
            press_serial_ = f.serial;
        to->route_pointer(s, f);
    } else if (unrouted_ && f.target) {
        // Not one of the hosted surfaces: the caller's own canvas view owns it.
        unrouted_->on_unrouted_pointer(s, f);
    }
}

void context::on_keyboard_focus(lx::wayland::seat& s, lx::wayland::surface* target,
                                std::uint32_t serial, bool entered) {
    if (!entered) {
        if (surface* from = surface_of(target))
            from->route_keyboard_focus(s, false);
        else if (unrouted_ && target)
            unrouted_->on_unrouted_keyboard_focus(s, target, serial, false);
        if (keyboard_target_ == target)
            keyboard_target_ = nullptr;
        return;
    }
    keyboard_target_ = target;
    if (surface* to = surface_of(target)) {
        to->route_keyboard_focus(s, true);
        // The seat's modifiers as they are at the enter (wl_keyboard sends modifiers after
        // it; a surface that joined since has them already).
        if (have_modifiers_)
            to->route_modifiers(s, modifiers_);
    } else if (unrouted_ && target) {
        unrouted_->on_unrouted_keyboard_focus(s, target, serial, true);
    }
}

void context::on_key(lx::wayland::seat& s, const lx::wayland::key_event& e) {
    if (surface* to = surface_of(keyboard_target_)) {
        if (e.state == lx::wayland::key_state::pressed)
            press_serial_ = e.serial;
        to->route_key(s, e);
    } else if (unrouted_ && keyboard_target_) {
        // The keyboard is on a canvas view of the caller's.
        unrouted_->on_unrouted_key(s, e);
    }
}

void context::on_modifiers(lx::wayland::seat& s, std::uint32_t modifiers) {
    // One seat state: a shift-click on a surface without the keyboard focus is a shift-click.
    modifiers_ = modifiers;
    have_modifiers_ = true;
    for (unsigned i = 0; i < count_; ++i)
        surfaces_[i]->route_modifiers(s, modifiers);
}

void context::on_repeat_info(lx::wayland::seat& s, std::int32_t rate, std::int32_t delay_ms) {
    repeat_rate_ = rate;
    repeat_delay_ms_ = delay_ms;
    have_repeat_ = true;
    for (unsigned i = 0; i < count_; ++i)
        surfaces_[i]->route_repeat_info(s, rate, delay_ms);
}

void context::on_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) {
    // The down names the surface; later events of the sequence may name none.
    if (e.type == lx::wayland::touch_event::kind::down && e.target) {
        touch_target_ = e.target;
        press_serial_ = e.serial;
    }
    if (surface* to = surface_of(e.target ? e.target : touch_target_))
        to->route_touch(s, e);
}

void context::on_focus(lx::wayland::text_input& t, lx::wayland::surface* target, bool entered) {
    if (surface* to = surface_of(target))
        to->input().on_focus(t, target, entered);
}

void context::on_done(lx::wayland::text_input& t, const lx::wayland::text_input_done& d) {
    if (surface* to = surface_of(t.focus()))
        to->input().on_done(t, d);
}

} // namespace lx::ui::host
