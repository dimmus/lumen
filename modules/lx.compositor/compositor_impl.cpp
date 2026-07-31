module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

import lx.foundation;
import lx.sync;
import lx.trace;
import lx.runtime;
import lx.scheduler;
import lx.gfx;
import lx.drm;
import lx.wayland.server;
import lx.scene;
import lx.input;
import lx.session;

module lx.compositor;

namespace lx::compositor::detail {

struct compositor_impl {
    compositor* owner = nullptr;
    config cfg{};

    wayland::server wayland{};
    surface_manager surfaces{};
    toplevel_manager toplevels{};
    shell_bridge shell_bridge_;
    cursor_manager cursor_{};
    scene::scene_graph scene{};
    gfx::device graphics{};
    drm::kms_device kms{};
    drm::kms_atomic_commit kms_atomic{kms};
    gfx::dmabuf_import_cache import_cache;
    buffer_lifecycle_tracker buffer_lifecycle_;
    runtime::memory_budget_coordinator memory_{};
    output_manager outputs{};
    seat_manager seats{};
    scheduler::frame_scheduler scheduler_{};
    scheduler::presentation_tracker presentation_{};
    scheduler::budget_tracker budget_{};
    protocol_managers protocols_{};
    hotplug_monitor hotplug_{};
    gfx::device_recovery device_recovery_{graphics};
    session::logind_session logind_{};
    p0_protocol_context p0_{};
    runtime::event_loop loop{};
    gfx::headless_backend headless_{};
    gfx::frame_renderer frame_renderer_{};
    gfx::pipeline_cache pipeline_cache_{};
    gfx::vulkan_compositor vk_compositor_{};
    static constexpr unsigned k_scanout_slots = 2;
    gfx::render_target vk_targets_[k_scanout_slots]{};
    drm::kms_framebuffer scanout_fbs_[k_scanout_slots]{};
    unsigned scanout_slot_count_ = 0;
    std::atomic<unsigned> scanout_front_{0};
    std::atomic<unsigned> pending_flip_slot_{0};
    /// Written by render after compositing, read by UI to retire client buffers. -1 until
    /// the first frame completes.
    std::atomic<long long> last_rendered_frame_{-1};
    texture_update_queue texture_updates_{};
    shm_staging_ring shm_staging_{};
    bool headless_present_ = true;
    bool drm_present_ = false;
    bool vulkan_present_ = false;
    bool drm_commit_failing_ = false;

    /// Over-budget frames are reported as an aggregate every k_budget_report_frames
    /// ticks — the hot path must not emit a warn per frame.
    static constexpr unsigned k_budget_report_frames = 600;
    unsigned ui_over_budget_ = 0;
    unsigned ui_budget_window_ = 0;
    unsigned render_over_budget_ = 0;
    unsigned render_budget_window_ = 0;

    std::unique_ptr<runtime::timer_source> vsync_timer{};
    std::unique_ptr<runtime::fd_source> wayland_fd_source{};
    std::unique_ptr<runtime::fd_source> drm_fd_source{};

    std::thread render_thread{};
    std::thread worker_thread{};

    std::mutex render_mutex{};
    std::condition_variable render_cv{};
    bool render_pending = false;

    std::mutex worker_mutex{};
    std::condition_variable worker_cv{};

    std::atomic<bool> running{false};
    std::atomic<bool> threads_started{false};
    std::atomic<unsigned> frame_index{0};

    scheduler::frame_tick last_tick{};
    std::chrono::steady_clock::time_point last_vsync_time{};

    explicit compositor_impl(compositor* o, config c)
        : owner{o}, cfg{c}, shell_bridge_{wayland},
          import_cache{graphics.dmabuf(), c.import_cache},
          buffer_lifecycle_{import_cache},
          memory_{c.memory}, budget_{c.hot_path} {
        scene.snapshots().set_config(c.snapshot);
        surfaces.set_import_cache(&import_cache);
        surfaces.set_scene(&scene);
        frame_renderer_.set_pipeline_cache(&pipeline_cache_);
    }

    static void on_memory_pressure(lx::runtime::memory_pressure level, void* self);
    void handle_memory_pressure(lx::runtime::memory_pressure level);

    void start_threads();
    void stop_threads();
    void on_vsync();
    void render_loop();
    void worker_loop();
    void register_p0_globals();

    /// UI thread: rotate scanout front buffer after a successful page flip.
    void on_scanout_flip(unsigned slot);
    static void presentation_flip_handler(scheduler::presentation_feedback fb, void* user_data);

    /// Render-affinity only: apply queued client-buffer updates before compositing.
    void drain_texture_updates();
    /// Render-affinity only: bring up the GPU composite target and its scanout FB.
    [[nodiscard]] lx::result<void> setup_vulkan_present();

    static compositor_impl* active;
    static void vsync_callback();
    static void wayland_fd_callback();
    static void drm_fd_callback();
};

compositor_impl* compositor_impl::active = nullptr;

void compositor_impl::on_memory_pressure(lx::runtime::memory_pressure level, void* self) {
    if (self) static_cast<compositor_impl*>(self)->handle_memory_pressure(level);
}

void compositor_impl::handle_memory_pressure(lx::runtime::memory_pressure level) {
    import_cache.on_memory_pressure(level);
    buffer_lifecycle_.on_memory_pressure(level);
    if (level == lx::runtime::memory_pressure::critical) {
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.memory",
                                        "critical pressure — evicting caches");
    }
}

void compositor_impl::vsync_callback() {
    if (active) active->on_vsync();
}

void compositor_impl::wayland_fd_callback() {
    if (!active)
        return;
    (void)active->wayland.dispatch(0);
    active->wayland.flush();
    active->logind_.dispatch();
}

void compositor_impl::drm_fd_callback() {
    if (!active || !active->drm_present_)
        return;
    active->kms_atomic.dispatch_events();
}

void compositor_impl::register_p0_globals() {
    p0_.server = &wayland;
    p0_.surfaces = &surfaces;
    p0_.toplevels = &toplevels;
    p0_.outputs = &outputs;
    p0_.headless = &headless_;
    p0_.dmabuf = &graphics.dmabuf();
    p0_.textures = &texture_updates_;
    p0_.lifecycle = &buffer_lifecycle_;
    p0_.shm_staging = &shm_staging_;
    buffer_lifecycle_.set_release_callback(&release_wl_buffer_resource, nullptr);
    protocols_.set_surface_manager(&surfaces);
    if (auto installed = install_p0_protocols(p0_); !installed) {
        lx::trace::logger::global().log_error(installed.get_error(), "compositor");
    }
    // Standard protocol state tables — globals are not advertised until bind handlers exist
    // (see reassessment gate after the dispatch seam lands).
    (void)protocols_.install(wayland);
}

void compositor_impl::drain_texture_updates() {
    // Always drain, even without a Vulkan target: leaving entries queued would stall the
    // staging ring and starve the UI thread of slots.
    texture_update update{};
    while (texture_updates_.try_pop(update)) {
        lx::result<void> applied{};
        switch (update.kind) {
        case texture_update::op::dmabuf: {
            gfx::imported_image image{};
            image.image_id = update.texture_id;
            image.width = update.width;
            image.height = update.height;
            image.format = update.format;
            image.vk_image = update.vk_image;
            image.vk_memory = update.vk_memory;
            if (vulkan_present_)
                applied = vk_compositor_.bind_imported(image);
            break;
        }
        case texture_update::op::shm:
            if (vulkan_present_)
                applied = vk_compositor_.upload_rgba(update.texture_id, update.width,
                                                     update.height, update.pixels);
            // The staging slot is free the moment the upload has been recorded.
            shm_staging_.publish_consumed(update.shm_seq);
            break;
        case texture_update::op::forget:
            if (vulkan_present_)
                vk_compositor_.forget_texture(update.texture_id);
            break;
        case texture_update::op::none:
            break;
        }
        if (!applied)
            lx::trace::logger::global().log_error(applied.get_error(), "compositor.render");
    }
}

lx::result<void> compositor_impl::setup_vulkan_present() {
    if (!graphics.info().supports_dmabuf_import)
        return lx::not_implemented("lx::compositor::setup_vulkan_present");

    if (auto initialized = vk_compositor_.initialize(graphics.context()); !initialized)
        return initialized.get_error();

    // Size the composite target to the active mode when there is a real output, so the
    // exported dmabuf can be scanned out without scaling.
    unsigned width = 1920;
    unsigned height = 1080;
    if (drm_present_) {
        if (auto active = kms.active_mode(0); active) {
            width = active.value().width;
            height = active.value().height;
        }
    }
    if (width == 0 || height == 0)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no usable mode for composite target");

    const unsigned slots = drm_present_ ? k_scanout_slots : 1u;
    scanout_slot_count_ = 0;
    for (unsigned slot = 0; slot < slots; ++slot) {
        auto created = gfx::render_target::create(graphics.context(), width, height);
        if (!created) {
            if (slot == 0)
                return created.get_error();
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                            "second scanout slot unavailable — single-buffer");
            break;
        }
        vk_targets_[slot] = std::move(created).value();

        if (drm_present_) {
            auto exported = vk_targets_[slot].export_dmabuf();
            if (!exported) {
                if (slot == 0)
                    return exported.get_error();
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                                "second scanout FB import failed — single-buffer");
                break;
            }
            const auto dmabuf = std::move(exported).value();
            auto framebuffer = drm::kms_framebuffer::import_dmabuf(
                kms, dmabuf.fd.get(), dmabuf.width, dmabuf.height, dmabuf.stride, dmabuf.offset,
                static_cast<lx::fourcc>(lx::pixel_format::xrgb8888), dmabuf.modifier);
            if (!framebuffer) {
                if (slot == 0)
                    return framebuffer.get_error();
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                                "second scanout FB import failed — single-buffer");
                break;
            }
            scanout_fbs_[slot] = std::move(framebuffer).value();
        }
        ++scanout_slot_count_;
    }

    if (drm_present_ && scanout_slot_count_ > 0) {
        kms_atomic.set_framebuffer(scanout_fbs_[0].id());
        scanout_front_.store(0, std::memory_order_release);
        pending_flip_slot_.store(0, std::memory_order_release);
    }

    vulkan_present_ = true;
    headless_present_ = false;
    return {};
}

void compositor_impl::on_scanout_flip(unsigned slot) {
    if (slot < scanout_slot_count_)
        scanout_front_.store(slot, std::memory_order_release);
}

void compositor_impl::presentation_flip_handler(scheduler::presentation_feedback fb,
                                                void* user_data) {
    if (!fb.hw_clock || !fb.vsync || !user_data)
        return;
    auto* impl = static_cast<compositor_impl*>(user_data);
    const unsigned slot = impl->pending_flip_slot_.load(std::memory_order_acquire);
    impl->on_scanout_flip(slot);
}

void compositor_impl::start_threads() {
    if (threads_started.exchange(true)) return;

    running.store(true, std::memory_order_release);
    last_vsync_time = std::chrono::steady_clock::now();

    render_thread = std::thread([this] { render_loop(); });

    if (cfg.worker_thread_count > 0)
        worker_thread = std::thread([this] { worker_loop(); });
}

void compositor_impl::stop_threads() {
    if (!threads_started.exchange(false)) return;

    running.store(false, std::memory_order_release);
    render_cv.notify_all();
    worker_cv.notify_all();

    if (render_thread.joinable()) render_thread.join();
    if (worker_thread.joinable()) worker_thread.join();
}

void compositor_impl::on_vsync() {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    const auto now = std::chrono::steady_clock::now();
    const double delta = std::chrono::duration<double>(now - last_vsync_time).count();
    last_vsync_time = now;

    scheduler::frame_tick tick{};
    tick.timestamp_ns = static_cast<runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    tick.delta_seconds = delta > 0.0 ? delta : (1.0 / cfg.target_fps);
    tick.frame_index = frame_index.fetch_add(1, std::memory_order_relaxed) + 1;
    last_tick = tick;

    if (owner) owner->tick_ui(tick);

    {
        std::lock_guard lock{render_mutex};
        render_pending = true;
    }
    render_cv.notify_one();
    worker_cv.notify_one();
}

void compositor_impl::render_loop() {
    scheduler::frame_scheduler::affinity_scope scope{scheduler::thread_affinity::render};

    while (running.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock{render_mutex};
            render_cv.wait(lock, [this] {
                return render_pending || !running.load(std::memory_order_acquire);
            });
            if (!running.load(std::memory_order_acquire)) break;
            render_pending = false;
        }

        if (owner) owner->tick_render();
        runtime::executor::global().drain(runtime::affinity::render);
    }
}

void compositor_impl::worker_loop() {
    scheduler::frame_scheduler::affinity_scope scope{scheduler::thread_affinity::worker};

    while (running.load(std::memory_order_acquire)) {
        runtime::executor::global().drain(runtime::affinity::worker);
        std::unique_lock lock{worker_mutex};
        worker_cv.wait_for(lock, std::chrono::milliseconds(16), [this] {
            return !running.load(std::memory_order_acquire);
        });
    }
}

} // namespace lx::compositor::detail

namespace lx::compositor {

void seat_manager::set_focus(lx::surface_id) {}

lx::input::seat& seat_manager::seat() {
    static lx::input::seat s;
    return s;
}

compositor::compositor(config cfg) {
    impl_ = new detail::compositor_impl{this, cfg};
}

compositor::~compositor() {
    if (impl_) {
        impl_->stop_threads();
        delete impl_;
        impl_ = nullptr;
    }
}

lx::result<void> compositor::start() {
    if (!impl_) {
        const auto err = lx::make_error(lx::error_domain::invalid_argument, 0, "null impl");
        lx::trace::logger::global().log_error(err, "compositor");
        return err;
    }

    impl_->scheduler_.set_target_fps(impl_->cfg.target_fps);

    // Graphics: Vulkan when available, otherwise headless-capable device.
    // Failure must not abort compositor start — fall back to default soft-import device.
    if (auto dev = gfx::device_selector::select_best(); dev) {
        impl_->graphics = std::move(dev).value();
        impl_->headless_present_ = !impl_->graphics.info().supports_dmabuf_import;
        lx::trace::logger::global().log(
            lx::trace::level::info, "compositor.gfx",
            impl_->graphics.info().device_name);
    } else {
        lx::trace::logger::global().log_error(dev.get_error(), "compositor.gfx");
        impl_->headless_present_ = true;
    }

    if (impl_->headless_present_) {
        if (auto hr = impl_->headless_.create(64, 64); !hr) {
            lx::trace::logger::global().log_error(hr.get_error(), "compositor.gfx");
        }
    }

    if (auto session = session::logind_session::open(); session)
        impl_->logind_ = std::move(session).value();

    // Prefer logind TakeDevice for the card node; fall back to direct open.
    {
        lx::result<drm::kms_device> opened =
            lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                           "kms not opened");
        if (auto taken = impl_->logind_.take_device_path("/dev/dri/card0"); taken) {
            opened = drm::kms_device::open_fd(std::move(taken).value());
        } else {
            opened = drm::kms_device::open("/dev/dri/card0");
        }
        if (opened) {
            impl_->kms = std::move(opened).value();
            impl_->graphics.syncobj().set_drm_fd(impl_->kms.card_fd());
            impl_->graphics.syncobj().set_device(impl_->graphics.vk_device());
            // Presenting needs DRM master, not merely an open card. Claiming the
            // page-flip path without it produces a commit failure on every frame.
            impl_->drm_present_ = impl_->kms.card_fd() >= 0 && impl_->kms.is_master();
            if (impl_->drm_present_) {
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.drm",
                                                "KMS device open — page-flip present path");
            } else if (impl_->kms.card_fd() >= 0) {
                lx::trace::logger::global().log(
                    lx::trace::level::warn, "compositor.drm",
                    "card open without DRM master — scanout disabled (nested session)");
            }
        }
    }

    // GPU composite path. Requires the KMS mode, so it comes after the card is open.
    // Failure is not fatal: the compositor degrades to the software blit rather than
    // refusing to start.
    if (auto vulkan = impl_->setup_vulkan_present(); vulkan) {
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                        "Vulkan composite path active");
    } else {
        lx::trace::logger::global().log_error(vulkan.get_error(), "compositor.gfx");
        impl_->headless_present_ = true;
        if (impl_->headless_.width() == 0) {
            if (auto created = impl_->headless_.create(64, 64); !created)
                lx::trace::logger::global().log_error(created.get_error(), "compositor.gfx");
        }
    }

    (void)impl_->hotplug_.start(impl_->outputs, impl_->kms);

    if (auto bound = impl_->wayland.bind(impl_->cfg.socket_name); !bound) {
        lx::trace::logger::global().log_error(bound.get_error(), "compositor");
        return bound.get_error();
    }

    impl_->register_p0_globals();

    if (auto installed = impl_->shell_bridge_.install(impl_->cfg.shell_binary_path); !installed) {
        lx::trace::logger::global().log_error(installed.get_error(), "compositor");
        return installed.get_error();
    }

    detail::compositor_impl::active = impl_;

    const runtime::clock_time interval_ns = static_cast<runtime::clock_time>(
        1'000'000'000.0 / impl_->cfg.target_fps);

    impl_->vsync_timer = std::make_unique<runtime::timer_source>(
        interval_ns, &detail::compositor_impl::vsync_callback, true);
    impl_->loop.add_source(impl_->vsync_timer.get());

    if (const int wfd = impl_->wayland.event_fd(); wfd >= 0) {
        // Poll Wayland at 1ms cadence and always pump when the source dispatches —
        // relying solely on epoll readability of the display fd is brittle when other
        // sources busy-wake the loop.
        impl_->wayland_fd_source = std::make_unique<runtime::fd_source>(
            wfd, &detail::compositor_impl::wayland_fd_callback, 1'000'000);
        impl_->loop.add_source(impl_->wayland_fd_source.get());
    }
    // DRM page-flip events are drained from tick_ui (see dispatch_events call), not via
    // a dedicated fd_source — DRM fds often report POLLIN continuously and would busy-spin.

    impl_->start_threads();

    impl_->memory_.set_pressure_handler(&detail::compositor_impl::on_memory_pressure, impl_);
    impl_->presentation_.bind_page_flip(impl_->kms_atomic);
    impl_->presentation_.set_handler(&detail::compositor_impl::presentation_flip_handler, impl_);

    lx::trace::logger::global().log(lx::trace::level::info, "compositor",
                                    "threads: ui(main)+render+worker; vsync timer armed");

    return {};
}

int compositor::run() {
    if (!impl_) return 1;

    runtime::set_current_affinity(runtime::affinity::ui);
    const int code = impl_->loop.run();
    impl_->stop_threads();
    detail::compositor_impl::active = nullptr;
    return code;
}

void compositor::request_stop() {
    if (!impl_) return;
    impl_->loop.quit(0);
}

config compositor::configuration() const { return impl_ ? impl_->cfg : config{}; }
wayland::server& compositor::wayland() { return impl_->wayland; }
surface_manager& compositor::surfaces() { return impl_->surfaces; }
toplevel_manager& compositor::toplevels() { return impl_->toplevels; }
shell_bridge& compositor::shell_bridge() { return impl_->shell_bridge_; }
cursor_manager& compositor::cursor() { return impl_->cursor_; }
scene::scene_graph& compositor::scene() { return impl_->scene; }
gfx::device& compositor::graphics() { return impl_->graphics; }
gfx::dmabuf_import_cache& compositor::import_cache() { return impl_->import_cache; }
output_manager& compositor::outputs() { return impl_->outputs; }
seat_manager& compositor::seats() { return impl_->seats; }
scheduler::frame_scheduler& compositor::scheduler() { return impl_->scheduler_; }
scheduler::presentation_tracker& compositor::presentation() { return impl_->presentation_; }
scheduler::budget_tracker& compositor::budget() { return impl_->budget_; }
drm::kms_atomic_commit& compositor::kms_atomic() { return impl_->kms_atomic; }
buffer_lifecycle_tracker& compositor::buffer_lifecycle() { return impl_->buffer_lifecycle_; }
runtime::memory_budget_coordinator& compositor::memory() { return impl_->memory_; }

runtime::executor& compositor::executor() {
    return runtime::executor::global();
}

void compositor::tick_ui(scheduler::frame_tick tick) {
    runtime::assert_affinity(runtime::affinity::ui);

    const auto t0 = std::chrono::steady_clock::now();
    impl_->budget_.begin_frame(tick.frame_index);
    impl_->memory_.tick(tick.frame_index);
    impl_->memory_.reset_frame_arenas(lx::runtime::memory_pool::ui);

    impl_->hotplug_.poll();

    for (unsigned i = 0; i < impl_->outputs.count(); ++i) {
        const float scale = impl_->protocols_.fractional_scale(i);
        if (scale > 0.f)
            impl_->outputs.set_scale(impl_->outputs.nth(i), scale);
    }

    (void)wayland().dispatch(0);
    if (impl_->drm_present_)
        impl_->kms_atomic.dispatch_events();
    impl_->logind_.dispatch();

    // Deliver wl_surface.frame callbacks after the previous present path ran.
    unsigned frame_time_ms = static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull);
    const auto last_present = impl_->presentation_.last_feedback();
    if (last_present.hw_clock && last_present.actual_present_ns > 0)
        frame_time_ms = static_cast<unsigned>(last_present.actual_present_ns / 1'000'000ull);
    fire_frame_callbacks(impl_->p0_, frame_time_ms);

    scheduler().on_vsync(tick);
    scene().update(tick.delta_seconds);

    lx::rect2i damage{};
    scene().collect_damage(damage);

    if (scene().commit_frame(tick.frame_index)) {
        impl_->buffer_lifecycle_.on_frame_submitted(tick.frame_index);
    } else {
        lx::trace::logger::global().log(lx::trace::level::trace, "compositor",
                                        "frame dropped (backpressure)");
    }

    // Release client buffers the render thread has finished sampling. Done here because
    // sending wl_buffer.release requires the UI thread's ownership of Wayland dispatch.
    (void)impl_->buffer_lifecycle_.retire_completed_frames(
        impl_->last_rendered_frame_.load(std::memory_order_acquire));

    const auto us = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    impl_->budget_.end_tick_ui(us);
    if (impl_->budget_.tick_ui_over_budget())
        ++impl_->ui_over_budget_;
    if (++impl_->ui_budget_window_ >= detail::compositor_impl::k_budget_report_frames) {
        if (impl_->ui_over_budget_ > 0)
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor",
                                            "tick_ui over budget in recent frames");
        impl_->ui_over_budget_ = 0;
        impl_->ui_budget_window_ = 0;
    }
}

void compositor::tick_render() {
    runtime::assert_affinity(runtime::affinity::render);

    const auto t0 = std::chrono::steady_clock::now();

    const auto& snapshot = scene().acquire_render_snapshot();
    const unsigned idx = snapshot.frame_index();
    const auto& draws = snapshot.draws();

    import_cache().tick(idx);
    impl_->memory_.reset_frame_arenas(lx::runtime::memory_pool::render);

    if (impl_->device_recovery_.needs_recovery()) {
        if (auto recovered = impl_->device_recovery_.handle_device_lost(); recovered) {
            impl_->import_cache = gfx::dmabuf_import_cache{impl_->graphics.dmabuf(),
                                                           impl_->cfg.import_cache};
            impl_->pipeline_cache_ = gfx::pipeline_cache{};
            impl_->frame_renderer_.set_pipeline_cache(&impl_->pipeline_cache_);
        }
    }

    static constexpr unsigned k_max_draws = 4096;
    lx::gfx::blit_command blits[k_max_draws];
    const unsigned blit_count = draws.size() < k_max_draws ? draws.size() : k_max_draws;
    for (unsigned i = 0; i < blit_count; ++i) {
        const auto& cmd = draws.data()[i];
        blits[i].texture_id = cmd.texture.id();
        blits[i].dst = cmd.dst;
        blits[i].opacity = cmd.opacity;
        blits[i].blend = cmd.blend;
    }

    const unsigned scanout_front = impl_->scanout_front_.load(std::memory_order_acquire);
    const unsigned scanout_back =
        impl_->scanout_slot_count_ > 1 ? (1u - scanout_front) : 0u;

    impl_->drain_texture_updates();

    if (impl_->vulkan_present_ && impl_->scanout_slot_count_ > 0) {
        auto composited = impl_->vk_compositor_.composite(
            impl_->vk_targets_[scanout_back], lx::color::rgb(0.f, 0.f, 0.f), blits, blit_count);
        if (!composited) {
            lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
        } else if (composited.value().draws_skipped > 0) {
            // A draw with no registered texture means commit and render disagree. Say so
            // instead of silently rendering a placeholder.
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                            "draw referenced an unregistered texture");
        }
    } else if (impl_->headless_present_ && impl_->headless_.width() > 0) {
        // Software fallback for no-GPU hosts and CI.
        (void)impl_->headless_.begin_frame();
        (void)impl_->headless_.clear(lx::color::rgb(0.f, 0.f, 0.f));
        if (blit_count > 0)
            (void)impl_->frame_renderer_.draw_to_headless(blits, blit_count, impl_->headless_);
        (void)impl_->headless_.end_frame_and_dump(nullptr);
    }

    // The composite above waits for GPU completion, so the client buffers sampled by this
    // snapshot are free. Publish the frame index; the UI thread owns Wayland dispatch and
    // sends the wl_buffer.release events.
    impl_->last_rendered_frame_.store(static_cast<long long>(idx), std::memory_order_release);

    const auto submit_ns = static_cast<runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    presentation().on_present_submitted(idx, submit_ns);

    if (impl_->drm_present_) {
        drm::atomic_commit_request req{};
        req.connector = 0;
        req.request_page_flip = true;
        if (impl_->scanout_slot_count_ > 0 && impl_->scanout_fbs_[scanout_back].id())
            req.framebuffer_id = impl_->scanout_fbs_[scanout_back].id();

        const lx::rect2i snap_damage = snapshot.damage();
        if (snap_damage.width > 0 && snap_damage.height > 0) {
            req.damage.full_frame = false;
            req.damage.count = 1;
            req.damage.rects[0] = snap_damage;
            impl_->kms_atomic.build_damage_blob(req.damage);
        } else {
            req.damage.full_frame = true;
        }

        impl_->pending_flip_slot_.store(scanout_back, std::memory_order_release);
        const auto committed = impl_->kms_atomic.commit(req);
        // Log on transition only: a failing commit every frame must not flood the hot
        // path, but silently discarding it is how a dead present path stayed invisible.
        if (!committed && !impl_->drm_commit_failing_) {
            impl_->drm_commit_failing_ = true;
            lx::trace::logger::global().log_error(committed.get_error(), "compositor.drm");
        } else if (committed && impl_->drm_commit_failing_) {
            impl_->drm_commit_failing_ = false;
            lx::trace::logger::global().log(lx::trace::level::info, "compositor.drm",
                                            "atomic commit recovered");
        }
    }

    if (draws.size() > 0) {
        lx::trace::logger::global().log(lx::trace::level::trace, "compositor.render", "draw");
    }

    scene().release_render_frame(snapshot);

    const auto us = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    impl_->budget_.end_tick_render(us);
    if (impl_->budget_.tick_render_over_budget())
        ++impl_->render_over_budget_;
    if (++impl_->render_budget_window_ >= detail::compositor_impl::k_budget_report_frames) {
        if (impl_->render_over_budget_ > 0)
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                            "tick_render over budget in recent frames");
        impl_->render_over_budget_ = 0;
        impl_->render_budget_window_ = 0;
    }
}

unsigned compositor::frame_index() const {
    return impl_ ? impl_->frame_index.load(std::memory_order_acquire) : 0;
}

bool compositor::is_running() const {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

} // namespace lx::compositor
