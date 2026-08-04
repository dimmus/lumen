module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
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
    /// Render affinity. Scene draw commands translated for the present backend, reused
    /// every frame. A member and not a local: sized by the cap rather than by occupancy it
    /// was a six-figure stack allocation touched once per frame, which evicts the working
    /// set for no benefit.
    gfx::blit_command* blits_ = nullptr;
    unsigned blit_capacity_ = 0;
    gfx::vulkan_compositor vk_compositor_{};
    gfx::cpu_compositor cpu_compositor_{};
    gfx::egl_device egl_{};
    gfx::gl_compositor gl_compositor_{};
    gfx::gl_scanout_target gl_targets_[3]{};
    /// Three, not two: with two, the slot the display is scanning and the slot a queued
    /// flip is about to scan leave nothing writable, so every frame overlapping a flip had
    /// to be dropped. A third slot means the composite always has somewhere to go.
    static constexpr unsigned k_scanout_slots = 3;
    gfx::render_target vk_targets_[k_scanout_slots]{};
    drm::kms_framebuffer scanout_fbs_[k_scanout_slots]{};
    /// CPU-writable scanout for the `cpu` present backend; parallel to `scanout_fbs_`.
    drm::kms_dumb_framebuffer dumb_fbs_[k_scanout_slots]{};
    unsigned scanout_slot_count_ = 0;
    std::atomic<unsigned> scanout_front_{0};
    std::atomic<unsigned> pending_flip_slot_{0};
    /// Render affinity. Newest composited slot not yet handed to the CRTC, or -1. A frame
    /// composited while a flip was queued waits here instead of being thrown away; if
    /// another frame lands first it simply takes its place (mailbox — show the newest).
    int composited_slot_ = -1;
    /// Render affinity. sync_file for the composite that produced `composited_slot_`, or
    /// -1. Travels with the slot because the commit can be deferred a tick, and the fence
    /// belongs to the frame in that slot rather than to whichever tick commits it. Owned
    /// here: superseding a slot or committing it closes the FD.
    int composited_fence_fd_ = -1;

    /// Render affinity. Drops a pending fence without committing it.
    void discard_composited_fence();
    /// Written by render after compositing, read by UI to retire client buffers. -1 until
    /// the first frame completes.
    std::atomic<long long> last_rendered_frame_{-1};
    texture_update_queue texture_updates_{};
    shm_staging_ring shm_staging_{};
    shm_pixel_store shm_store_{};

    /// Render affinity. The `shm_pixel_store` slot each texture is currently sampled from.
    /// A texture holds at most one: staging a new commit supersedes the previous slot, and
    /// the composite that read it has already finished by the time the drain sees the
    /// replacement — both run on this thread, in that order.
    static constexpr unsigned k_held_tokens = 64;
    struct held_shm_slot {
        unsigned texture_id = 0;
        unsigned long long token = 0;
    };
    held_shm_slot held_shm_[k_held_tokens]{};

    /// Render affinity. Takes over `texture_id`'s held slot, releasing the one it replaces.
    void hold_shm_slot(unsigned texture_id, unsigned long long token);
    void release_shm_slot(unsigned texture_id);

    /// Render affinity. Texture retirements waiting for the snapshots that still draw them.
    static constexpr unsigned k_max_deferred_forgets = 64;
    struct deferred_forget {
        unsigned texture_id = 0;
        unsigned retire_after_frame = 0;
        bool used = false;
    };
    deferred_forget deferred_forgets_[k_max_deferred_forgets]{};

    void queue_forget(unsigned texture_id, unsigned retire_after_frame);
    void apply_forget(unsigned texture_id);
    /// Applies every retirement whose frame has now been composited.
    void apply_due_forgets(unsigned composited_frame);
    bool headless_present_ = true;
    bool drm_present_ = false;
    bool vulkan_present_ = false;
    bool cpu_present_ = false;
    bool gl_present_ = false;
    bool drm_commit_failing_ = false;

    /// Frame clock. A free-running timer drifts against the display: ticks land while a
    /// flip is still queued and their work is wasted, which is how a 60 Hz output settles
    /// at a fraction of its refresh. Each completed flip re-arms the timer just ahead of
    /// the next vblank, so the compositor runs at the display's cadence and in phase.
    lx::runtime::clock_time refresh_period_ns_ = 16'666'667;
    /// UI affinity. Decides when each repaint starts so it lands just before the display
    /// needs it, instead of at the interval boundary — see `scheduler::repaint_scheduler`.
    /// Touched only from the UI thread, which is why the render cost reaches it through
    /// `render_cost_us_` rather than being fed in directly from the render thread.
    scheduler::repaint_scheduler repaint_{};
    /// Written by render at the end of each tick, read by UI when it re-arms the frame
    /// clock. Relaxed: it is a cost estimate, and a one-frame-stale value only shifts the
    /// next start by the difference between two adjacent frames.
    std::atomic<unsigned> render_cost_us_{0};
    /// Composite must finish before the vblank it targets; waking exactly on it would miss.
    static constexpr lx::runtime::clock_time k_frame_deadline_margin_ns = 2'000'000;

    /// Frame-callback pacing. UI affinity: set by the page-flip handler (drained from
    /// tick_ui) and consumed there. `last_callback_ns_` backs the stall watchdog below.
    bool present_completed_ = false;
    lx::runtime::clock_time last_callback_ns_ = 0;
    /// If the present path stops reporting flips, clients would never be told to draw
    /// again. Past this gap, fall back to tick pacing so a broken output cannot wedge them.
    static constexpr lx::runtime::clock_time k_callback_stall_ns = 250'000'000;

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

    ~compositor_impl() { delete[] blits_; }

    compositor_impl(const compositor_impl&) = delete;
    compositor_impl& operator=(const compositor_impl&) = delete;

    /// Render affinity. Grows to fit and never shrinks; returns null if `need` is 0.
    [[nodiscard]] gfx::blit_command* blit_storage(unsigned need) {
        if (need == 0)
            return nullptr;
        if (need > blit_capacity_) {
            delete[] blits_;
            blits_ = new gfx::blit_command[need];
            blit_capacity_ = need;
        }
        return blits_;
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
    /// Bring up CPU-writable dumb-buffer scanout. Requires DRM master.
    [[nodiscard]] lx::result<void> setup_cpu_present();
    /// Render-affinity only: bring up EGL/GBM and the GL scanout targets.
    [[nodiscard]] lx::result<void> setup_gl_present();
    /// The scanout slot that is neither on screen nor queued for a flip.
    [[nodiscard]] unsigned pick_back_slot() const;

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
    // Exactly one staging strategy: the CPU path samples staged rows in place during the
    // composite, which a shared FIFO cannot keep alive that long (see shm_pixel_store).
    // GL copies the pixels out during the drain, like Vulkan, so it uses the ring — and it
    // uploads RGBA, so it wants the same conversion.
    if (cpu_present_) {
        p0_.shm_store = &shm_store_;
        p0_.shm_staging = nullptr;
        // Scanout is XR24, so keeping the client's channel order means an unscaled opaque
        // surface reaches the display with no conversion pass at all.
        p0_.shm_native_format = true;
    } else {
        p0_.shm_staging = &shm_staging_;
        p0_.shm_store = nullptr;
        p0_.shm_native_format = false;
    }
    buffer_lifecycle_.set_release_callback(&release_wl_buffer_resource, nullptr);
    protocols_.set_surface_manager(&surfaces);
    if (auto installed = install_p0_protocols(p0_); !installed) {
        lx::trace::logger::global().log_error(installed.get_error(), "compositor");
    }
    // Standard protocol state tables — globals are not advertised until bind handlers exist
    // (see reassessment gate after the dispatch seam lands).
    (void)protocols_.install(wayland);
}

void compositor_impl::hold_shm_slot(unsigned texture_id, unsigned long long token) {
    if (texture_id == 0)
        return;
    held_shm_slot* free_slot = nullptr;
    for (auto& held : held_shm_) {
        if (held.texture_id == texture_id) {
            shm_store_.release(held.token);
            held.token = token;
            return;
        }
        if (held.texture_id == 0 && !free_slot)
            free_slot = &held;
    }
    if (free_slot) {
        free_slot->texture_id = texture_id;
        free_slot->token = token;
        return;
    }
    // No room to track it: releasing now is wrong (the composite is about to read it), so
    // the slot leaks until the texture is forgotten. Say so rather than corrupt the frame.
    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                    "shm slot table full — staging slot not tracked");
}

void compositor_impl::release_shm_slot(unsigned texture_id) {
    for (auto& held : held_shm_) {
        if (held.texture_id != texture_id)
            continue;
        shm_store_.release(held.token);
        held = {};
        return;
    }
}

void compositor_impl::discard_composited_fence() {
    if (composited_fence_fd_ >= 0) {
        ::close(composited_fence_fd_);
        composited_fence_fd_ = -1;
    }
}

void compositor_impl::apply_forget(unsigned texture_id) {
    if (cpu_present_) {
        cpu_compositor_.forget_texture(texture_id);
        release_shm_slot(texture_id);
    }
    if (gl_present_)
        gl_compositor_.forget_texture(texture_id);
    if (vulkan_present_)
        vk_compositor_.forget_texture(texture_id);
}

void compositor_impl::queue_forget(unsigned texture_id, unsigned retire_after_frame) {
    for (auto& pending : deferred_forgets_) {
        if (pending.used)
            continue;
        pending.used = true;
        pending.texture_id = texture_id;
        pending.retire_after_frame = retire_after_frame;
        return;
    }
    // Out of room. Retiring now risks a snapshot that still draws it, so say so instead of
    // doing it silently — an unregistered-texture warning downstream would be the symptom.
    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                    "deferred texture retirement table full — retiring early");
    apply_forget(texture_id);
}

void compositor_impl::apply_due_forgets(unsigned composited_frame) {
    for (auto& pending : deferred_forgets_) {
        if (!pending.used)
            continue;
        // Signed difference rather than `<`: frame indices only advance, so their
        // difference stays small and this keeps working across the counter's wrap.
        if (static_cast<int>(composited_frame - pending.retire_after_frame) < 0)
            continue;
        apply_forget(pending.texture_id);
        pending = {};
    }
}

void compositor_impl::drain_texture_updates() {
    // Always drain, even without a Vulkan target: leaving entries queued would stall the
    // staging ring and starve the UI thread of slots.
    texture_update update{};
    while (texture_updates_.try_pop(update)) {
        lx::result<void> applied{};
        switch (update.kind) {
        case texture_update::op::dmabuf: {
            // Owns the FD the UI thread duplicated, so it is closed however this branch
            // exits — including when no backend wants it.
            gfx::dmabuf_desc desc{};
            if (update.dmabuf_fd >= 0) {
                desc.width = update.width;
                desc.height = update.height;
                desc.format = update.format;
                desc.plane_count = 1;
                desc.planes[0].fd.reset(update.dmabuf_fd);
                desc.planes[0].offset = update.dmabuf_offset;
                desc.planes[0].stride = update.dmabuf_stride;
                desc.planes[0].modifier = update.dmabuf_modifier;
            }

            if (vulkan_present_) {
                gfx::imported_image image{};
                image.image_id = update.texture_id;
                image.width = update.width;
                image.height = update.height;
                image.format = update.format;
                image.vk_image = update.vk_image;
                image.vk_memory = update.vk_memory;
                applied = vk_compositor_.bind_imported(image);
            } else if (gl_present_ && desc.plane_count > 0) {
                // The whole point of the GL path: the client's buffer is sampled where it
                // already lives, so a window costs a textured quad and no upload at all.
                applied = gl_compositor_.import_dmabuf(update.texture_id, desc);
            }
            break;
        }
        case texture_update::op::shm:
            if (cpu_present_) {
                // No upload: the CPU compositor samples the staged rows in place, so the
                // pixels are read exactly once, during the composite itself.
                gfx::pixel_surface src{};
                src.pixels = const_cast<unsigned char*>(update.pixels);
                src.width = update.width;
                src.height = update.height;
                src.stride = update.stride ? update.stride : update.width * 4u;
                src.format = update.format;
                applied = cpu_compositor_.register_texture(update.texture_id, src);
                if (applied)
                    hold_shm_slot(update.texture_id, update.shm_token);
                else
                    shm_store_.release(update.shm_token);
            } else if (gl_present_) {
                applied = gl_compositor_.upload_rgba(update.texture_id, update.width,
                                                     update.height, update.pixels, update.stride);
                shm_staging_.publish_consumed(update.shm_seq);
            } else if (vulkan_present_) {
                applied = vk_compositor_.upload_rgba(update.texture_id, update.width,
                                                     update.height, update.pixels);
                // The ring slot is free the moment the upload has been recorded.
                shm_staging_.publish_consumed(update.shm_seq);
            } else {
                shm_staging_.publish_consumed(update.shm_seq);
                shm_store_.release(update.shm_token);
            }
            break;
        case texture_update::op::forget:
            // Deferred, not applied: a snapshot published before the buffer was destroyed
            // may still be waiting to composite, and it draws this texture by id. Dropping
            // it now costs a warning on the Vulkan path — and on the CPU path it frees the
            // staging slot the draw is still reading from.
            queue_forget(update.texture_id, update.retire_after_frame);
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

lx::result<void> compositor_impl::setup_cpu_present() {
    if (!drm_present_) {
        // Without DRM master there is no scanout buffer to write into, and compositing
        // into memory nobody reads is not a present path.
        return lx::not_implemented("lx::compositor::setup_cpu_present");
    }

    unsigned width = 0;
    unsigned height = 0;
    if (auto active = kms.active_mode(0); active) {
        width = active.value().width;
        height = active.value().height;
    }
    if (width == 0 || height == 0)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no usable mode for CPU scanout target");

    // XR24 is what the primary plane takes everywhere, and it is the byte order wl_shm
    // clients already use — so an unscaled opaque surface reaches the display as a plain
    // row copy with no conversion at all.
    const auto format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);

    scanout_slot_count_ = 0;
    for (unsigned slot = 0; slot < k_scanout_slots; ++slot) {
        auto created = drm::kms_dumb_framebuffer::create(kms, width, height, format);
        if (!created) {
            if (slot < 2) // fewer than two buffers cannot page-flip without tearing
                return created.get_error();
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                            "third scanout slot unavailable — double-buffered");
            break;
        }
        dumb_fbs_[slot] = std::move(created).value();
        ++scanout_slot_count_;
    }

    cpu_compositor_.set_worker_threads(cfg.composite_thread_count);

    kms_atomic.set_framebuffer(dumb_fbs_[0].id());
    scanout_front_.store(0, std::memory_order_release);
    pending_flip_slot_.store(0, std::memory_order_release);

    cpu_present_ = true;
    vulkan_present_ = false;
    headless_present_ = false;
    return {};
}

lx::result<void> compositor_impl::setup_gl_present() {
    if (!drm_present_)
        return lx::not_implemented("lx::compositor::setup_gl_present");

    unsigned width = 0;
    unsigned height = 0;
    if (auto active = kms.active_mode(0); active) {
        width = active.value().width;
        height = active.value().height;
    }
    if (width == 0 || height == 0)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no usable mode for GL scanout target");

    // The card node, not a render node: the scanout buffers have to be allocated on the
    // device that will scan them out, and this fd is the one holding DRM master.
    auto device = gfx::egl_device::create(kms.card_fd());
    if (!device)
        return device.get_error();
    egl_ = std::move(device).value();

    // Falling through to GL on llvmpipe would be strictly worse than compositing on the
    // CPU directly — same rasterizer, plus an upload and a driver stack.
    if (egl_.is_software_renderer()) {
        return lx::make_error(lx::error_domain::gl, static_cast<int>(lx::gl_err::no_context),
                              "GL is a software rasterizer — CPU composite is cheaper");
    }

    if (auto initialized = gl_compositor_.initialize(egl_); !initialized)
        return initialized.get_error();

    const auto format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    scanout_slot_count_ = 0;
    for (unsigned slot = 0; slot < k_scanout_slots; ++slot) {
        auto created = gfx::gl_scanout_target::create(egl_, width, height, format);
        if (!created) {
            if (slot < 2)
                return created.get_error();
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                            "third scanout slot unavailable — double-buffered");
            break;
        }
        gl_targets_[slot] = std::move(created).value();

        auto exported = gl_targets_[slot].export_dmabuf();
        if (!exported) {
            if (slot < 2)
                return exported.get_error();
            break;
        }
        const auto dmabuf = std::move(exported).value();
        auto framebuffer = drm::kms_framebuffer::import_dmabuf(
            kms, dmabuf.fd.get(), dmabuf.width, dmabuf.height, dmabuf.stride, dmabuf.offset,
            format, dmabuf.modifier);
        if (!framebuffer) {
            if (slot < 2)
                return framebuffer.get_error();
            break;
        }
        scanout_fbs_[slot] = std::move(framebuffer).value();
        ++scanout_slot_count_;
    }

    kms_atomic.set_framebuffer(scanout_fbs_[0].id());
    scanout_front_.store(0, std::memory_order_release);
    pending_flip_slot_.store(0, std::memory_order_release);

    gl_present_ = true;
    vulkan_present_ = false;
    cpu_present_ = false;
    headless_present_ = false;
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx", egl_.renderer());
    return {};
}

unsigned compositor_impl::pick_back_slot() const {
    if (scanout_slot_count_ <= 1)
        return 0;
    const unsigned front = scanout_front_.load(std::memory_order_acquire);
    const unsigned pending = kms_atomic.flip_pending()
                                 ? pending_flip_slot_.load(std::memory_order_acquire)
                                 : front;
    for (unsigned slot = 0; slot < scanout_slot_count_; ++slot) {
        if (slot != front && slot != pending)
            return slot;
    }
    return (front + 1u) % scanout_slot_count_;
}

void compositor_impl::on_scanout_flip(unsigned slot) {
    if (slot < scanout_slot_count_)
        scanout_front_.store(slot, std::memory_order_release);
}

void compositor_impl::presentation_flip_handler(scheduler::presentation_feedback fb,
                                                void* user_data) {
    if (!user_data)
        return;
    auto* impl = static_cast<compositor_impl*>(user_data);
    // The flip landed, so clients may draw again regardless of how complete the feedback is.
    impl->present_completed_ = true;

    // Phase-lock the frame clock to the display, and aim each repaint at its deadline
    // rather than at the interval boundary. A timer free-running at target_fps drifts
    // against the real vblank cadence, so ticks land while a flip is still queued and their
    // composite is wasted — the visible rate then sits well under the refresh rate for
    // reasons that look like slow rendering.
    //
    // The start time comes from the measured repaint cost, so the idle wait sits before the
    // work instead of after it and the frame carries input from as late as it can. A frame
    // that gets more expensive starts earlier on its own. Scheduling from `now` rather than
    // the flip timestamp keeps this free of assumptions about which clock the driver
    // reports.
    if (impl->vsync_timer) {
        const auto now = static_cast<lx::runtime::clock_time>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        impl->repaint_.observe_repaint(impl->budget_.last_tick_ui_us(),
                                       impl->render_cost_us_.load(std::memory_order_relaxed));
        impl->repaint_.on_present(now);
        impl->vsync_timer->schedule_at(impl->repaint_.next_start(now));
    }

    if (!fb.hw_clock || !fb.vsync)
        return;
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

    // The GL context was built on the UI thread during start() and released there; claim it
    // here, because every GL call from now on happens on this thread.
    if (gl_present_) {
        if (auto current = egl_.make_current(); !current) {
            lx::trace::logger::global().log_error(current.get_error(), "compositor.gfx");
            gl_present_ = false;
            headless_present_ = true;
        }
    }

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

    // A frame composited but never committed still holds its fence; this thread owns it.
    discard_composited_fence();
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

    // Present path. Requires the KMS mode, so it comes after the card is open. Failure is
    // not fatal: the compositor degrades rather than refusing to start.
    //
    // The choice that matters is Vulkan vs CPU. A software Vulkan device (llvmpipe under a
    // VM, SwiftShader) still runs the whole GPU path, but every frame then pays a staging
    // upload, a tiling copy and a fullscreen fragment shader to produce pixels the CPU
    // already had — several passes over the frame to do the work of one. Compositing
    // straight into a dumb buffer is strictly less work on such a host.
    // Preference order under `automatic`, all of it about where the pixels actually get
    // rasterized:
    //
    //   hardware Vulkan  — best: zero-copy dma-buf import and a GPU composite
    //   hardware GL      — same idea through EGL/GLES; the accelerated path on drivers Mesa
    //                      ships no Vulkan driver for (vmwgfx/SVGA3D and friends)
    //   CPU              — when the only "GPU" is a software rasterizer, going through a
    //                      graphics API costs an upload and a driver stack to run the same
    //                      instructions the CPU compositor runs directly
    {
        const auto requested = impl_->cfg.present;
        const bool automatic = requested == present_backend::automatic;
        const bool software_vulkan = impl_->graphics.info().is_software_rasterizer;

        lx::result<void> present = lx::not_implemented("lx::compositor::present_path");

        if ((requested == present_backend::vulkan || (automatic && !software_vulkan)) &&
            impl_->drm_present_) {
            present = impl_->setup_vulkan_present();
            if (present) {
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                                "Vulkan composite path active");
            } else if (!automatic) {
                lx::trace::logger::global().log_error(present.get_error(), "compositor.gfx");
            }
        }

        if (!present && (requested == present_backend::gl || automatic)) {
            present = impl_->setup_gl_present();
            if (present) {
                // The context has to move to the render thread, which claims it in
                // render_loop; it cannot be current on two threads at once.
                impl_->egl_.release_current();
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                                "GL composite path active (EGL/GLES on GBM)");
            } else if (requested == present_backend::gl) {
                lx::trace::logger::global().log_error(present.get_error(), "compositor.gfx");
            }
        }

        if (!present && (requested == present_backend::cpu || automatic)) {
            present = impl_->setup_cpu_present();
            if (present) {
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                                "CPU composite path active");
            } else if (requested == present_backend::cpu) {
                lx::trace::logger::global().log_error(present.get_error(), "compositor.gfx");
            }
        }

        // Last resort when nothing above came up — commonly a nested session with no DRM
        // master, where there is no scanout buffer to composite into at all.
        if (!present && requested == present_backend::automatic && !impl_->drm_present_) {
            present = impl_->setup_vulkan_present();
            if (present) {
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                                "Vulkan composite path active (no scanout)");
            }
        }

        if (!present) {
            lx::trace::logger::global().log_error(present.get_error(), "compositor.gfx");
            impl_->headless_present_ = true;
            impl_->cpu_present_ = false;
            impl_->vulkan_present_ = false;
            impl_->gl_present_ = false;
            if (impl_->headless_.width() == 0) {
                if (auto created = impl_->headless_.create(64, 64); !created)
                    lx::trace::logger::global().log_error(created.get_error(), "compositor.gfx");
            }
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

    // The frame clock follows the display, not a configured target: a 60 Hz timer against
    // a 75 Hz panel (or the reverse) beats against the flips and wastes composites. The
    // configured fps is the fallback for when there is no mode to read.
    runtime::clock_time interval_ns = static_cast<runtime::clock_time>(
        1'000'000'000.0 / impl_->cfg.target_fps);
    if (impl_->drm_present_) {
        if (auto active = impl_->kms.active_mode(0); active && active.value().refresh_millihz > 0) {
            interval_ns = static_cast<runtime::clock_time>(1'000'000'000'000.0 /
                                                           active.value().refresh_millihz);
        }
    }
    impl_->refresh_period_ns_ = interval_ns;
    impl_->repaint_.set_refresh_period(interval_ns);

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
    // Protocol handlers date texture retirements against this, so it has to be current
    // before any Wayland dispatch below.
    impl_->p0_.frame_index = tick.frame_index;
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

    // wl_surface.frame means "the content you committed has been shown". Answering it once
    // per vsync tick instead makes clients render frames that are composited over and never
    // reach the display, so on the KMS path the callbacks wait for an actual flip. Without
    // a present path (headless, nested) the tick is the only clock there is.
    if (impl_->last_callback_ns_ == 0)
        impl_->last_callback_ns_ = tick.timestamp_ns;
    const bool stalled =
        tick.timestamp_ns - impl_->last_callback_ns_ >= detail::compositor_impl::k_callback_stall_ns;
    if (!impl_->drm_present_ || impl_->present_completed_ || stalled) {
        if (stalled && !impl_->present_completed_) {
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor",
                                            "no present in 250ms — pacing frame callbacks by tick");
        }
        impl_->present_completed_ = false;
        impl_->last_callback_ns_ = tick.timestamp_ns;

        unsigned frame_time_ms = static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull);
        const auto last_present = impl_->presentation_.last_feedback();
        if (last_present.hw_clock && last_present.actual_present_ns > 0)
            frame_time_ms = static_cast<unsigned>(last_present.actual_present_ns / 1'000'000ull);
        fire_frame_callbacks(impl_->p0_, frame_time_ms);
    }

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

    const unsigned blit_count = draws.size();
    lx::gfx::blit_command* blits = impl_->blit_storage(blit_count);
    for (unsigned i = 0; i < blit_count; ++i) {
        const auto& cmd = draws.data()[i];
        blits[i].texture_id = cmd.texture.id();
        blits[i].dst = cmd.dst;
        blits[i].src = cmd.src;
        blits[i].clip = cmd.clip;
        blits[i].tint = cmd.tint;
        blits[i].opacity = cmd.opacity;
        blits[i].blend = cmd.blend;
        blits[i].buffer_xform = cmd.buffer_xform;
        blits[i].src_space = cmd.src_space;
    }

    impl_->drain_texture_updates();

    // A CRTC takes one flip at a time, so a frame arriving while one is queued cannot be
    // shown yet — but with three scanout slots it no longer has to be thrown away either.
    // It composites into the slot that is neither on screen nor queued, and the commit is
    // what waits. The frame clock is phase-locked to the flips, so this is rare.
    const unsigned scanout_back = impl_->pick_back_slot();
    const bool flip_in_flight = impl_->drm_present_ && impl_->kms_atomic.flip_pending();
    bool composited_this_tick = false;

    if (impl_->gl_present_ && impl_->scanout_slot_count_ > 0) {
        auto composited = impl_->gl_compositor_.composite(
            impl_->gl_targets_[scanout_back], lx::color::rgb(0.f, 0.f, 0.f), blits, blit_count,
            snapshot.damage());
        if (!composited) {
            lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
        } else {
            composited_this_tick = true;
            // This frame supersedes any uncommitted one, so its fence replaces that
            // frame's — which is dropped rather than leaked.
            impl_->discard_composited_fence();
            impl_->composited_fence_fd_ = composited.value().out_fence_fd;
            if (composited.value().draws_skipped > 0) {
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                "draw referenced an unregistered texture");
            }
        }
    } else if (impl_->cpu_present_ && impl_->scanout_slot_count_ > 0) {
        const auto& fb = impl_->dumb_fbs_[scanout_back];
        gfx::pixel_surface target{};
        target.pixels = fb.pixels();
        target.width = fb.width();
        target.height = fb.height();
        target.stride = fb.stride();
        target.format = fb.format();
        target.write_combining = true;

        auto composited = impl_->cpu_compositor_.composite(
            target, lx::color::rgb(0.f, 0.f, 0.f), blits, blit_count, snapshot.damage());
        if (!composited) {
            lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
        } else {
            composited_this_tick = true;
            const auto stats = composited.value();
            if (stats.draws_skipped > 0) {
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                "draw referenced an unregistered texture");
            }
            if (stats.draws_unsupported > 0) {
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                "draw format has no conversion to the scanout "
                                                "format — surface not shown");
            }
        }
    } else if (impl_->vulkan_present_ && impl_->scanout_slot_count_ > 0) {
        auto composited = impl_->vk_compositor_.composite(
            impl_->vk_targets_[scanout_back], lx::color::rgb(0.f, 0.f, 0.f), blits, blit_count);
        if (!composited) {
            lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
        } else {
            composited_this_tick = true;
            if (composited.value().draws_skipped > 0) {
                // A draw with no registered texture means commit and render disagree. Say
                // so instead of silently rendering a placeholder.
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                "draw referenced an unregistered texture");
            }
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
    // snapshot are free — as are those of a dropped frame, which was never sampled at all.
    // Publish the frame index; the UI thread owns Wayland dispatch and sends the
    // wl_buffer.release events.
    impl_->last_rendered_frame_.store(static_cast<long long>(idx), std::memory_order_release);

    // This snapshot has been drawn, so any texture whose retirement was waiting on a frame
    // this recent is now genuinely unreferenced.
    impl_->apply_due_forgets(idx);

    // Newest wins: a frame that could not be shown yet is superseded by this one rather
    // than queued behind it, so what reaches the display is always the latest content.
    if (composited_this_tick)
        impl_->composited_slot_ = static_cast<int>(scanout_back);

    const auto submit_ns = static_cast<runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    // Nothing is submitted while a flip is queued: recording it would measure the
    // in-flight flip's latency from the wrong timestamp.
    if (!flip_in_flight)
        presentation().on_present_submitted(idx, submit_ns);

    if (impl_->drm_present_ && !flip_in_flight && impl_->composited_slot_ >= 0) {
        const auto present_slot = static_cast<unsigned>(impl_->composited_slot_);
        impl_->composited_slot_ = -1;

        drm::atomic_commit_request req{};
        req.connector = 0;
        req.request_page_flip = true;
        // Borrowed by the commit; closed below once it has returned. When the GL backend
        // produced no fence it already blocked, so an absent fence is still correct.
        req.in_fence_fd = impl_->composited_fence_fd_;
        if (impl_->cpu_present_ && impl_->dumb_fbs_[present_slot].id())
            req.framebuffer_id = impl_->dumb_fbs_[present_slot].id();
        else if (impl_->scanout_slot_count_ > 0 && impl_->scanout_fbs_[present_slot].id())
            req.framebuffer_id = impl_->scanout_fbs_[present_slot].id();

        const lx::rect2i snap_damage = snapshot.damage();
        if (snap_damage.width > 0 && snap_damage.height > 0) {
            req.damage.full_frame = false;
            req.damage.count = 1;
            req.damage.rects[0] = snap_damage;
            impl_->kms_atomic.build_damage_blob(req.damage);
        } else {
            req.damage.full_frame = true;
        }

        impl_->pending_flip_slot_.store(present_slot, std::memory_order_release);
        const auto committed = impl_->kms_atomic.commit(req);
        // The kernel has taken its own reference by now, so the compositor's copy goes
        // regardless of whether the commit succeeded.
        impl_->discard_composited_fence();
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
    // Publish for the frame clock. Both phases decide when the next repaint has to start:
    // the frame is not presentable until render is done, so the deadline applies to their
    // sum — but the scheduler itself belongs to the UI thread.
    impl_->render_cost_us_.store(us, std::memory_order_relaxed);
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
