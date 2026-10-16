module;

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

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
    /// UI affinity. Subsurface trees and each root's scene container — see `:surface_tree`.
    surface_tree tree_{};
    toplevel_manager toplevels{};
    shell_bridge shell_bridge_;
    cursor_manager cursor_{};
    scene::scene_graph scene{};
    /// Paint order under the scene root, bottom→top (sway's): layer-shell background and
    /// bottom, the windows, layer-shell top and overlay, every popup, the drag-and-drop
    /// icon, then the software cursor. Popups sit above the overlay layer so no panel or
    /// window ever covers a menu; the drag icon above everything a client draws.
    /// The four layer-shell containers hold layer surfaces' tree containers (`:layer_shell`).
    scene::container_node background_layer_{};
    scene::container_node bottom_layer_{};
    scene::container_node windows_layer_{};
    scene::container_node top_layer_{};
    scene::container_node overlay_shell_layer_{};
    scene::container_node popup_layer_{};
    scene::container_node dnd_icon_layer_{};
    scene::container_node cursor_layer_{};
    /// UI affinity. The drag-and-drop grab and its icon — see `:dnd`.
    dnd_grab dnd_{};
    /// UI affinity. xdg_popup placement, grab and dismissal — see `:popup`.
    popup_manager popups_{};
    /// UI affinity. wlr-layer-shell surfaces, arrangement, exclusive zones — see
    /// `:layer_shell`.
    layer_shell layers_{};
    /// UI affinity. Server-side frames (xdg-decoration) — see `:decoration`.
    decoration_manager decorations_{};
    /// UI affinity. Window geometry, configure flow and focus — see `:wm`.
    window_manager wm_{};
    gfx::device graphics{};
    drm::kms_device kms{};
    drm::kms_atomic_commit kms_atomic{kms};
    /// Render affinity after `start`. Declared after `kms_atomic` so it is destroyed first:
    /// removing its framebuffers takes the pointer off the CRTC before `kms_atomic` hands
    /// the CRTC back to the console.
    drm::kms_cursor_plane cursor_plane_{};
    gfx::dmabuf_import_cache import_cache;
    buffer_lifecycle_tracker buffer_lifecycle_;
    runtime::memory_budget_coordinator memory_{};
    output_manager outputs{};
    seat_manager seats{};
    input_router input_router_{};
    /// One seat, owned by the device layer. It has to be one: the libinput feed maintains
    /// the xkb state and the held-key set on it, and the router reads both when it sends
    /// `enter`. With two, the router's copy has no compiled keymap and every focus change
    /// reports no modifiers and no held keys — which looks like a protocol bug and is not.
    /// `input_manager` is default-constructible, so this is valid even when no device ever
    /// opens, and its address is stable across the move-assignment in `start_input`.
    lx::input::input_manager input_devices_{};
    bool input_ready_ = false;
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

    // ── damage tracking (render affinity unless noted) ──
    /// Extent of the scanout target; damage is clipped to it.
    lx::rect2i scanout_rect_{};
    /// Per scanout slot: damage the slot has *missed* since it was last composited into.
    /// With three slots a slot is two or three frames old when it is reused, so compositing
    /// only the newest frame's damage into it would leave the intervening frames' changes
    /// as ghost trails behind a moving window. Starts full: nothing has been drawn yet.
    gfx::damage_region slot_age_damage_[k_scanout_slots]{};
    /// Damage since the last successful KMS commit. FB_DAMAGE_CLIPS describe how the new
    /// framebuffer differs from the one the plane showed before it, which spans every frame
    /// composited since that commit — not just the newest. Starts full.
    gfx::damage_region kms_pending_damage_{};
    /// Damage a failed composite could not draw, retried on the next tick.
    gfx::damage_region missed_damage_{};
    /// Frame index of the snapshot whose damage was last consumed; a re-acquired snapshot
    /// has nothing new to say.
    unsigned last_seen_frame_ = ~0u;
    /// Set by render while it holds a composited frame that is not committed yet; read by
    /// UI before it answers frame callbacks on an idle tick.
    std::atomic<bool> frame_outstanding_{false};
    /// UI affinity. Newest frame index published with non-empty damage.
    long long last_damaged_frame_ = -1;
    /// UI affinity. Output 0's geometry as of the last tick; a change repaints everything.
    lx::rect2i last_output_rect_{};
    /// UI affinity. `outputs.generation()` clients were last told about.
    unsigned output_generation_ = 0;
    /// UI affinity. xdg_wm_base.ping/pong: who is not responding.
    liveness_tracker liveness_{};
    activation_tokens activation_{};
    bool initial_damage_noted_ = false;
    /// Written by render after compositing, read by UI to retire client buffers. -1 until
    /// the first frame completes.
    std::atomic<long long> last_rendered_frame_{-1};
    texture_update_queue texture_updates_{};
    /// Render affinity. Updates popped from `texture_updates_` that belong to a snapshot
    /// newer than the one being composited (`texture_update::visible_from_frame`), oldest
    /// first. Everything behind the first such update waits too, so FIFO order holds across
    /// a forget and a re-registration of the same id.
    static constexpr unsigned k_deferred_updates = 256;
    texture_update deferred_updates_[k_deferred_updates]{};
    unsigned deferred_head_ = 0;
    unsigned deferred_count_ = 0;
    bool warned_deferred_full_ = false;
    shm_staging_ring shm_staging_{};
    shm_pixel_store shm_store_{};
    /// GL path: transient staging, a block per in-flight update (see shm_upload_pool).
    shm_upload_pool shm_pool_{};
    /// Staging blocks unused this long are unmapped; long enough to ride out a pause in
    /// an animation, short enough that an idle desktop holds none.
    static constexpr unsigned long long k_staging_idle_ns = 1'000'000'000ull;
    bool reported_partial_fresh_ = false;

    // ── cursor ──
    /// UI affinity; loaded once in `start`.
    xcursor_theme cursor_theme_{};
    /// UI → render. Pointer position/visibility, and plane-sized images.
    cursor_frame_mailbox cursor_frames_{};
    cursor_image_mailbox cursor_images_{};
    /// Render affinity. The newest cursor frame taken from the mailbox.
    cursor_frame cursor_cur_{};
    /// Render affinity. Generation of the image staged into the plane's back buffer.
    unsigned cursor_staged_gen_ = 0;
    /// Render affinity. The plane passed a TEST_ONLY commit with an image on it.
    bool cursor_tested_ = false;
    /// Render affinity. A content commit has gone through: the CRTC is ours (the first one
    /// is the modeset, which the cursor does not join).
    bool content_committed_once_ = false;
    /// Render → UI. The plane is unusable; UI switches the cursor to software.
    std::atomic<bool> cursor_hw_failed_{false};
    /// Render → UI. The commit whose flip is pending carried a frame (false: cursor only).
    /// A cursor-only flip is not a content present: it must not answer frame callbacks.
    std::atomic<bool> last_commit_content_{true};

    // ── Input-to-present latency (ADR-005 cross-cutting). libinput µs and the KMS flip
    //    timestamp are both CLOCK_MONOTONIC, so the difference is what the user feels, up
    //    to the scanout of the line the pointer is on. ──
    /// Render affinity. Newest input time already attributed to a commit.
    unsigned long long latency_reported_input_us_ = 0;
    /// Render affinity. Pointer state of the last commit: a commit samples latency only
    /// when what the pointer shows changed, so a later unrelated present (a client mapping
    /// after the pointer went idle) is not timed against stale input.
    int latency_cursor_x_ = -2147483647 - 1;
    int latency_cursor_y_ = 0;
    unsigned latency_cursor_gen_ = 0;
    bool latency_cursor_visible_ = false;
    /// Render → UI. `input_us << 1 | content` of the commit whose flip is pending; 0 when
    /// none. Written only when 0 (compare-exchange), so a commit racing the flip handler
    /// drops its sample instead of being timed against the previous commit's flip.
    std::atomic<unsigned long long> committed_input_us_{0};
    /// UI affinity. Fixed sample window for the periodic summary; no allocation.
    static constexpr unsigned k_latency_window = 600;
    unsigned long long latency_samples_[k_latency_window]{};
    unsigned latency_sample_count_ = 0;
    /// Render affinity. A commit carrying this tick's cursor frame succeeded.
    void note_committed_input(bool content);
    /// UI affinity. Logs one present and, every `k_latency_window`, a summary.
    void note_present_latency(unsigned long long present_ns, unsigned long long submit_us,
                              unsigned long long fence_us);
    void log_latency_summary();
    /// `output N name=… mode=WxH refresh_mhz=…` (info) for every output: at start and on
    /// every change clients are told about.
    void log_outputs() const;
    /// Render → UI. When the commit whose flip is pending was issued, and a dup of the
    /// sync_file its plane waited on (-1 when there was none). With the flip timestamp
    /// they split a present into compositor, GPU and display time, which is what tells a
    /// slow tail on a VM host from one in Lumen. Published before the commit ioctl, since
    /// its flip can be drained before the ioctl returns; taken by the flip handler. Only
    /// filled while debug logging is on: a dup per commit is not free.
    std::atomic<unsigned long long> committed_submit_us_{0};
    std::atomic<int> committed_fence_fd_{-1};
    /// Render affinity. Publishes the timing above for a commit about to be issued.
    void publish_commit_timing(int fence_fd);
    /// Render affinity. Takes it back after the commit failed.
    void retract_commit_timing();

    /// Render affinity. Prepares the plane for this tick's commit (newest position, a new
    /// image staged into the back buffer) — only when no flip is pending, which is also the
    /// only time a commit can happen.
    void prepare_cursor_plane();
    /// Render affinity. Marks the plane unusable after a rejected test, once.
    void fail_cursor_plane(const char* why);

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
    std::unique_ptr<runtime::fd_source> input_fd_source{};

    /// UI thread. Brings up libinput and routes its events to focused clients.
    void start_input();
    /// Render affinity. A draw whose texture was never registered means the commit path and
    /// the render path disagree, and the screen stays black. Reporting only that it
    /// happened is not actionable — this reports the ids involved, once, because it
    /// otherwise fires every frame.
    void report_unregistered_draw(const gfx::blit_command* blits, unsigned count);
    bool reported_unregistered_ = false;

    /// Render affinity. Traces the first few texture registrations and retirements.
    /// "Content appears, then goes black" is a lifecycle question — which id was drawn,
    /// which was retired, and whether replacements kept arriving — and none of that is
    /// answerable from a single warning at the point the draw fails.
    static constexpr unsigned k_texture_trace_limit = 16;
    unsigned texture_trace_count_ = 0;
    void trace_texture_op(const char* op, unsigned texture_id, bool ok);
    static void input_ready_callback();
    void on_input_key(const lx::input::key_event& event);
    void on_input_pointer_motion(const lx::input::pointer_motion_event& event);
    void on_input_pointer_button(const lx::input::pointer_button_event& event);
    void on_input_pointer_axis(const lx::input::pointer_axis_event& event);
    void on_input_modifiers(const lx::input::modifier_state& mods);

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
        (void)scene.root().add(&background_layer_);
        (void)scene.root().add(&bottom_layer_);
        (void)scene.root().add(&windows_layer_);
        (void)scene.root().add(&top_layer_);
        (void)scene.root().add(&overlay_shell_layer_);
        (void)scene.root().add(&popup_layer_);
        (void)scene.root().add(&dnd_icon_layer_);
        (void)scene.root().add(&cursor_layer_);
        surfaces.set_import_cache(&import_cache);
        surfaces.set_scene(&scene);
        tree_.bind(surfaces, scene);
        shell_bridge_.set_toplevels(&toplevels);
        for (auto& slot : slot_age_damage_)
            slot.set_full();
        kms_pending_damage_.set_full();
        frame_renderer_.set_pipeline_cache(&pipeline_cache_);
        seats.bind(this);
    }

    ~compositor_impl() {
        delete[] blits_;
        // A deferred dma-buf update still owns the fd the UI thread duplicated for it.
        for (unsigned i = 0; i < deferred_count_; ++i) {
            texture_update& u = deferred_updates_[(deferred_head_ + i) % k_deferred_updates];
            if (u.dmabuf_fd >= 0)
                ::close(u.dmabuf_fd);
        }
        if (const int fd = committed_fence_fd_.exchange(-1); fd >= 0)
            ::close(fd);
    }

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

    /// Render-affinity only: apply queued client-buffer updates before compositing the
    /// snapshot of `frame` — only those that snapshot may show (see `deferred_updates_`).
    void drain_texture_updates(unsigned frame);
    /// Render-affinity only: one update, applied now.
    void apply_texture_update(texture_update& update);
    /// Render-affinity only: bring up the GPU composite target and its scanout FB.
    [[nodiscard]] lx::result<void> setup_vulkan_present();
    /// Creates the Vulkan device if it does not exist yet (start-up defers it when Vulkan
    /// will not composite). Idempotent; failure leaves the soft-import default in place.
    void ensure_graphics_device();
    bool graphics_created_ = false;
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

void compositor_impl::input_ready_callback() {
    if (active)
        (void)active->input_devices_.dispatch();
}

void compositor_impl::on_input_key(const lx::input::key_event& event) {
    // Keys the window manager claims (Escape during a grab) never reach the client.
    if (wm_.on_key(event))
        return;
    input_router_.send_key(event, input_devices_.default_seat());
}

void compositor_impl::note_committed_input(bool content) {
    const cursor_frame& f = cursor_cur_;
    const bool changed = f.x != latency_cursor_x_ || f.y != latency_cursor_y_ ||
                         f.image_generation != latency_cursor_gen_ ||
                         f.visible != latency_cursor_visible_;
    latency_cursor_x_ = f.x;
    latency_cursor_y_ = f.y;
    latency_cursor_gen_ = f.image_generation;
    latency_cursor_visible_ = f.visible;
    if (!changed)
        return;
    const unsigned long long in = f.input_time_us;
    if (in == 0 || in <= latency_reported_input_us_)
        return;
    latency_reported_input_us_ = in;
    unsigned long long expected = 0;
    (void)committed_input_us_.compare_exchange_strong(expected, (in << 1) | (content ? 1u : 0u),
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed);
}

namespace {
/// CLOCK_MONOTONIC microseconds at which every fence in a sync_file signalled, or 0 when
/// that is unknown (not signalled, an error, or more fences than this reads).
unsigned long long sync_file_signal_us(int fd) {
#if defined(__linux__)
    sync_fence_info fences[4]{};
    sync_file_info info{};
    info.num_fences = 4;
    info.sync_fence_info = reinterpret_cast<__u64>(fences);
    if (::ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1)
        return 0;
    unsigned long long latest = 0;
    for (unsigned i = 0; i < info.num_fences && i < 4; ++i) {
        if (fences[i].status != 1)
            return 0;
        if (fences[i].timestamp_ns > latest)
            latest = fences[i].timestamp_ns;
    }
    return latest / 1000ull;
#else
    (void)fd;
    return 0;
#endif
}

unsigned long long monotonic_us() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

void compositor_impl::publish_commit_timing(int fence_fd) {
    if (lx::trace::logger::global().min_level() > lx::trace::level::debug)
        return;
    const int dup_fd = fence_fd >= 0 ? ::fcntl(fence_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (const int old = committed_fence_fd_.exchange(dup_fd, std::memory_order_acq_rel); old >= 0)
        ::close(old);
    committed_submit_us_.store(monotonic_us(), std::memory_order_release);
}

void compositor_impl::retract_commit_timing() {
    committed_submit_us_.store(0, std::memory_order_release);
    if (const int old = committed_fence_fd_.exchange(-1, std::memory_order_acq_rel); old >= 0)
        ::close(old);
}

void compositor_impl::note_present_latency(unsigned long long present_ns,
                                           unsigned long long submit_us,
                                           unsigned long long fence_us) {
    const unsigned long long packed = committed_input_us_.exchange(0, std::memory_order_acquire);
    if (packed == 0 || present_ns == 0)
        return;
    const unsigned long long input_us = packed >> 1;
    const unsigned long long present_us = present_ns / 1000ull;
    if (present_us < input_us)
        return; // clocks disagree (a driver without monotonic timestamps): no sample
    const unsigned long long latency_us = present_us - input_us;
    if (lx::trace::logger::global().min_level() > lx::trace::level::debug)
        return;
    // A timing submitted after this flip landed belongs to a newer commit, picked up because
    // the flip handler raced it: report nothing rather than the wrong commit's breakdown.
    if (submit_us > present_us || submit_us < input_us) {
        submit_us = 0;
        fence_us = 0;
    }
    char line[224];
    std::snprintf(line, sizeof(line),
                  "present kind=%s input_us=%llu present_us=%llu latency_us=%llu "
                  "submit_us=%llu fence_us=%llu",
                  (packed & 1u) ? "content" : "cursor", input_us, present_us, latency_us,
                  submit_us, fence_us);
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.latency", line);
    latency_samples_[latency_sample_count_++] = latency_us;
    if (latency_sample_count_ == k_latency_window)
        log_latency_summary();
}

void compositor_impl::log_outputs() const {
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        if (!outputs.slot_live(slot))
            continue;
        const output_state& o = outputs.slot_state(slot);
        char line[128];
        std::snprintf(line, sizeof(line), "output %u name=%s mode=%dx%d refresh_mhz=%u", slot,
                      o.name, o.geometry.width, o.geometry.height, o.refresh_mhz);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.output", line);
    }
}

void compositor_impl::log_latency_summary() {
    const unsigned n = latency_sample_count_;
    latency_sample_count_ = 0;
    if (n == 0)
        return;
    // Exact percentiles over the fixed window, in place: nth_element allocates nothing.
    unsigned long long* b = latency_samples_;
    const unsigned i50 = (n - 1) / 2;
    const unsigned i95 = (n - 1) * 95 / 100;
    std::nth_element(b, b + i95, b + n);
    const unsigned long long p95 = b[i95];
    std::nth_element(b, b + i50, b + i95 + 1);
    const unsigned long long p50 = b[i50];
    const unsigned long long max = *std::max_element(b + i95, b + n);
    char line[128];
    std::snprintf(line, sizeof(line), "latency summary n=%u p50_us=%llu p95_us=%llu max_us=%llu",
                  n, p50, p95, max);
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.latency", line);
}

void compositor_impl::on_input_modifiers(const lx::input::modifier_state& mods) {
    input_router_.send_modifiers(mods, input_devices_.default_seat());
    // During a drag the modifiers choose the user's action.
    wm_.on_modifiers();
}

void compositor_impl::prepare_cursor_plane() {
    if (!cursor_plane_.valid())
        return;
    if (!content_committed_once_ || kms_atomic.flip_pending())
        return;

    const bool failed = cursor_hw_failed_.load(std::memory_order_relaxed);
    bool want = !failed && cursor_cur_.visible && cursor_cur_.hardware;
    if (want && cursor_cur_.image_generation != cursor_staged_gen_) {
        // The image mailbox may already hold a newer one than this frame references; the
        // newest is always right to show.
        (void)cursor_images_.take();
        const cursor_plane_image& img = cursor_images_.front();
        if (img.generation != 0 && img.generation != cursor_staged_gen_) {
            if (auto staged = cursor_plane_.stage_image(img.pixels.data(), img.width,
                                                        img.height, img.width, img.hot_x,
                                                        img.hot_y);
                staged) {
                cursor_staged_gen_ = img.generation;
            } else {
                fail_cursor_plane("image does not fit the plane");
                want = false;
            }
        }
        if (cursor_staged_gen_ == 0)
            want = false; // nothing staged yet: do not show an empty buffer
    }
    cursor_plane_.set_visible(want);
    cursor_plane_.set_position(cursor_cur_.x, cursor_cur_.y);

    // Validate once, before the plane's state rides a content commit: a rejection there
    // would fail the frame too. TEST_ONLY applies nothing and raises no event.
    if (want && !cursor_tested_ && cursor_plane_.dirty()) {
        drm::atomic_commit_request test{};
        test.connector = 0;
        test.update_primary = false;
        test.cursor = &cursor_plane_;
        test.request_page_flip = false;
        test.test_only = true;
        if (auto ok = kms_atomic.commit(test); ok) {
            cursor_tested_ = true;
        } else if (kms_atomic.last_failure_rejected_state()) {
            lx::trace::logger::global().log_error(ok.get_error(), "compositor.cursor");
            fail_cursor_plane("TEST_ONLY commit rejected");
            cursor_plane_.set_visible(false);
        }
        // Otherwise the device was unavailable (DRM master on another VT): that says
        // nothing about the plane, so the test runs again on a later tick.
    }
}

void compositor_impl::fail_cursor_plane(const char* why) {
    if (cursor_hw_failed_.exchange(true, std::memory_order_acq_rel))
        return;
    char line[128];
    std::snprintf(line, sizeof(line), "cursor plane rejected: %s — falling back to software",
                  why);
    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.cursor", line);
}

void compositor_impl::on_input_pointer_motion(const lx::input::pointer_motion_event& event) {
    auto& seat = input_devices_.default_seat();
    // Output geometry is the compositor's to know — libinput reports deltas and has no
    // idea how large the desktop is — so both halves of the mapping live here. An absolute
    // device (a tablet, or the mouse of any VM console) arrives normalised to a unit box
    // and is scaled into the output; whatever the source, the pointer is then kept on
    // screen. lx.input accumulates only relative motion, which is why absolute devices
    // used to leave the pointer at the origin for the whole session.
    if (outputs.count() > 0) {
        const lx::rect2i geometry = outputs.geometry(outputs.nth(0));
        if (event.absolute) {
            seat.set_pointer_position(geometry.x + event.x * geometry.width,
                                      geometry.y + event.y * geometry.height);
        }
        seat.constrain_pointer(geometry);
    }
    // O(1): records the position and, outside a grab, publishes it for render's next
    // commit. No composite is involved while the cursor plane is in use.
    // During an interactive grab or a drag the pointer is published at the tick instead,
    // so it and the dragged window (or the drag icon) reach the display in the same commit.
    cursor_.on_motion(seat.pointer_x(), seat.pointer_y(),
                      event.time_us ? event.time_us
                                    : static_cast<unsigned long long>(event.time_ms) * 1000ull,
                      wm_.grab_active() || wm_.drag_active());
    // Hit-testing, enter/leave and the implicit grab are the window manager's; it sends
    // the motion in the focused surface's local coordinates.
    wm_.on_pointer_motion(event.time_ms);
}

void compositor_impl::on_input_pointer_button(const lx::input::pointer_button_event& event) {
    const bool grab_before = wm_.grab_active() || wm_.drag_active();
    wm_.on_pointer_button(event);
    // A button that starts or ends a grab (or ends a drag) changes the pointer image without
    // moving it: re-publish the same position stamped with this event, so latency is
    // measured from the button and not from the last motion.
    const bool grab_after = wm_.grab_active() || wm_.drag_active();
    if (event.time_us != 0 && grab_before != grab_after) {
        auto& seat = input_devices_.default_seat();
        cursor_.on_motion(seat.pointer_x(), seat.pointer_y(), event.time_us, grab_after);
    }
}

void compositor_impl::on_input_pointer_axis(const lx::input::pointer_axis_event& event) {
    wm_.on_pointer_axis(event);
}

void compositor_impl::trace_texture_op(const char* op, unsigned texture_id, bool ok) {
    if (texture_trace_count_ >= k_texture_trace_limit)
        return;
    ++texture_trace_count_;
    char line[128];
    std::snprintf(line, sizeof(line), "texture %s id=%u%s%s", op, texture_id,
                  ok ? "" : " FAILED",
                  texture_trace_count_ == k_texture_trace_limit ? " (trace ends here)" : "");
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.texture", line);
}

void compositor_impl::report_unregistered_draw(const gfx::blit_command* blits,
                                               unsigned count) {
    if (reported_unregistered_)
        return;
    reported_unregistered_ = true;

    char detail[256];
    unsigned written = 0;
    written += static_cast<unsigned>(std::snprintf(
        detail + written, sizeof(detail) - written,
        "draw references an unregistered texture — nothing will be drawn. ids:"));
    for (unsigned i = 0; i < count && i < 4 && written + 16 < sizeof(detail); ++i) {
        written += static_cast<unsigned>(std::snprintf(detail + written,
                                                       sizeof(detail) - written, " %u",
                                                       blits[i].texture_id));
    }
    std::snprintf(detail + written, sizeof(detail) - written,
                  " (%u draws; backend=%s). Reported once.", count,
                  gl_present_    ? "gl"
                  : cpu_present_ ? "cpu"
                  : vulkan_present_ ? "vulkan"
                                    : "headless");
    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render", detail);
}

void compositor_impl::start_input() {
    // Device nodes are opened through logind when there is a session; without one libinput
    // falls back to opening them directly, which works for root or a member of the input
    // group and fails cleanly otherwise.
    lx::input::device_provider provider{};
    provider.user = this;
    provider.open_device = [](const char* path, int flags, void* user) -> int {
        auto* self = static_cast<compositor_impl*>(user);
        if (self) {
            if (auto taken = self->logind_.take_device_path(path); taken)
                return std::move(taken).value().release();
        }
        const int fd = ::open(path, flags | O_CLOEXEC);
        return fd < 0 ? -errno : fd;
    };
    provider.close_device = [](int fd, void*) { ::close(fd); };

    // Compile a keymap regardless of whether devices open. Clients are told the layout at
    // bind time, and a compositor that never found a keyboard still has to answer that.
    if (auto compiled = input_devices_.default_seat().keymap().compile(); !compiled)
        lx::trace::logger::global().log_error(compiled.get_error(), "compositor.input");

    auto opened = lx::input::input_manager::open(provider);
    if (!opened) {
        // Not fatal. A compositor with no seat still composites — it is useful headless and
        // in CI — but say so plainly, because "nothing happens when I type" is otherwise a
        // long debugging session.
        lx::trace::logger::global().log_error(opened.get_error(), "compositor.input");
        return;
    }
    input_devices_ = std::move(opened).value();

    lx::input::event_sink sink{};
    sink.user = this;
    sink.key = [](const lx::input::key_event& e, void* u) {
        static_cast<compositor_impl*>(u)->on_input_key(e);
    };
    sink.modifiers = [](const lx::input::modifier_state& m, void* u) {
        static_cast<compositor_impl*>(u)->on_input_modifiers(m);
    };
    sink.pointer_motion = [](const lx::input::pointer_motion_event& e, void* u) {
        static_cast<compositor_impl*>(u)->on_input_pointer_motion(e);
    };
    sink.pointer_button = [](const lx::input::pointer_button_event& e, void* u) {
        static_cast<compositor_impl*>(u)->on_input_pointer_button(e);
    };
    sink.pointer_axis = [](const lx::input::pointer_axis_event& e, void* u) {
        static_cast<compositor_impl*>(u)->on_input_pointer_axis(e);
    };
    input_devices_.set_sink(sink);

    // Share the keymap the device layer compiled, so the state the compositor tracks and
    // the state clients are told about are the same one.
    if (const int fd = input_devices_.fd(); fd >= 0) {
        input_fd_source = std::make_unique<runtime::fd_source>(
            fd, &compositor_impl::input_ready_callback);
        loop.add_source(input_fd_source.get());
        input_ready_ = true;
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.input",
                                        "libinput seat opened");
    }
}

void compositor_impl::on_memory_pressure(lx::runtime::memory_pressure level, void* self) {
    if (self) static_cast<compositor_impl*>(self)->handle_memory_pressure(level);
}

void compositor_impl::handle_memory_pressure(lx::runtime::memory_pressure level) {
    import_cache.on_memory_pressure(level);
    buffer_lifecycle_.on_memory_pressure(level);
    // Cached staging blocks are pure spare capacity: drop them at the first sign of
    // pressure. (Retired CPU-path storage is already freed every tick.)
    if (level != lx::runtime::memory_pressure::normal)
        shm_pool_.trim();
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
    // A flip is drained within this 1 ms pump rather than at the next tick: the flip
    // handler phase-locks the frame clock, and seeing the flip a tick late is what let the
    // clock drift ahead of the display. Only while a flip is outstanding, so an idle loop
    // makes no extra syscall; the fd is non-blocking.
    if (active->drm_present_ && active->kms_atomic.flip_pending())
        active->kms_atomic.dispatch_events();
    active->wayland.flush();
    active->logind_.dispatch();
}

void compositor_impl::drm_fd_callback() {
    if (!active || !active->drm_present_)
        return;
    active->kms_atomic.dispatch_events();
}

/// dev_t clients should allocate dma-bufs on (zwp_linux_dmabuf_feedback_v1.main_device):
/// the render node of the KMS card when it has one, else the card itself; 0 without DRM.
/// Startup-only sysfs I/O.
[[nodiscard]] static unsigned long long dmabuf_main_device(int card_fd) {
#if defined(__linux__)
    struct stat st {};
    if (card_fd < 0 || ::fstat(card_fd, &st) != 0 || !S_ISCHR(st.st_mode))
        return 0;
    char dir[96];
    std::snprintf(dir, sizeof(dir), "/sys/dev/char/%u:%u/device/drm", major(st.st_rdev),
                  minor(st.st_rdev));
    if (DIR* d = ::opendir(dir)) {
        unsigned long long found = 0;
        while (const dirent* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "renderD", 7) != 0)
                continue;
            char node[64];
            std::snprintf(node, sizeof(node), "/dev/dri/%s", e->d_name);
            struct stat rs {};
            if (::stat(node, &rs) == 0 && S_ISCHR(rs.st_mode)) {
                found = rs.st_rdev;
                break;
            }
        }
        ::closedir(d);
        if (found)
            return found;
    }
    return st.st_rdev;
#else
    (void)card_fd;
    return 0;
#endif
}

void compositor_impl::register_p0_globals() {
    p0_.server = &wayland;
    if (drm_present_)
        p0_.dmabuf_main_device = dmabuf_main_device(kms.card_fd());
    p0_.surfaces = &surfaces;
    p0_.input = &input_router_;
    p0_.seat = &input_devices_.default_seat();
    p0_.shell = &shell_bridge_;
    p0_.toplevels = &toplevels;
    p0_.tree = &tree_;
    wm_.bind(toplevels, surfaces, tree_, scene, windows_layer_, input_router_,
             input_devices_.default_seat(), shell_bridge_, outputs);
    popups_.bind(toplevels, surfaces, tree_, popup_layer_);
    wm_.set_popups(&popups_);
    p0_.popups = &popups_;
    {
        scene::container_node* const containers[k_layer_levels] = {
            &background_layer_, &bottom_layer_, &top_layer_, &overlay_shell_layer_};
        layers_.bind(surfaces, tree_, containers);
    }
    wm_.set_layers(&layers_);
    p0_.layers = &layers_;
    decorations_.bind(tree_, scene);
    wm_.set_decorations(&decorations_);
    p0_.decorations = &decorations_;
    p0_.server_side_decorations = cfg.server_side_decorations;
    p0_.wm = &wm_;
    wm_.set_cursor(&cursor_);
    p0_.cursor = &cursor_;
    dnd_.bind(tree_, dnd_icon_layer_, &cursor_);
    wm_.set_dnd(&dnd_);
    p0_.dnd = &dnd_;
    liveness_.set_timeout_ns(static_cast<runtime::clock_time>(cfg.ping_timeout_ms) * 1'000'000);
    wm_.set_liveness(&liveness_);
    p0_.liveness = &liveness_;
    p0_.activation = &activation_;
    p0_.outputs = &outputs;
    // Only when frames are actually composited in software. The headless backend keeps a
    // full copy of every registered texture; handed it on a GPU path, the commit path
    // copied every shm buffer into a table nothing read and nothing pruned — the bulk of
    // the compositor's heap growth (measured with heaptrack: 60 MB over a 30-resize run).
    p0_.headless = headless_present_ ? &headless_ : nullptr;
    p0_.dmabuf = &graphics.dmabuf();
    p0_.textures = &texture_updates_;
    p0_.lifecycle = &buffer_lifecycle_;
    // Exactly one staging strategy: the CPU path samples staged rows in place during the
    // composite, which a shared FIFO cannot keep alive that long (see shm_pixel_store).
    // GL copies the pixels out during the drain, but still stages per texture: a global
    // 8-slot ring starves as soon as one client commits more surfaces than that in a frame
    // (a CSD client's subsurfaces alone do), and the dropped update never shows. GL uploads
    // RGBA, so it wants the conversion. Vulkan keeps the ring.
    p0_.shm_pool = nullptr;
    if (cpu_present_) {
        p0_.shm_store = &shm_store_;
        p0_.shm_staging = nullptr;
        // Scanout is XR24, so keeping the client's channel order means an unscaled opaque
        // surface reaches the display with no conversion pass at all.
        p0_.shm_native_format = true;
    } else if (gl_present_) {
        // GL reads staged pixels once, during the drain, so staging only has to outlive
        // the trip to the render thread: transient blocks sized by the damage they carry,
        // not a persistent window-sized copy per texture.
        p0_.shm_pool = &shm_pool_;
        p0_.shm_store = nullptr;
        p0_.shm_staging = nullptr;
        p0_.shm_native_format = false;
    } else {
        p0_.shm_staging = &shm_staging_;
        p0_.shm_store = nullptr;
        p0_.shm_native_format = false;
    }
    // GL imports client dma-bufs on the render thread, where its context lives (an EGLImage
    // per commit); the UI-side Vulkan import cache is only for the Vulkan path.
    surfaces.set_render_thread_import(gl_present_);
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

void compositor_impl::ensure_graphics_device() {
    if (graphics_created_)
        return;
    graphics_created_ = true;
    if (auto dev = gfx::device_selector::select_best(); dev) {
        graphics = std::move(dev).value();
        headless_present_ = !graphics.info().supports_dmabuf_import;
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                        graphics.info().device_name);
        if (kms.card_fd() >= 0) {
            graphics.syncobj().set_drm_fd(kms.card_fd());
            graphics.syncobj().set_device(graphics.vk_device());
        }
    } else {
        lx::trace::logger::global().log_error(dev.get_error(), "compositor.gfx");
        headless_present_ = true;
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
        trace_texture_op("retire-applied", pending.texture_id, true);
        apply_forget(pending.texture_id);
        pending = {};
    }
}

void compositor_impl::drain_texture_updates(unsigned frame) {
    // Always drain, even without a Vulkan target: leaving entries queued would stall the
    // staging ring and starve the UI thread of slots.
    //
    // Signed difference, not `<`: frame indices only advance, so this survives the wrap.
    const auto too_new = [frame](const texture_update& u) {
        return static_cast<int>(u.visible_from_frame - frame) > 0;
    };

    // Held-back updates first, in order, up to the first one this snapshot may not show.
    while (deferred_count_ > 0) {
        texture_update& held = deferred_updates_[deferred_head_];
        if (too_new(held))
            break;
        apply_texture_update(held);
        held = {};
        deferred_head_ = (deferred_head_ + 1) % k_deferred_updates;
        --deferred_count_;
    }

    texture_update update{};
    while (texture_updates_.try_pop(update)) {
        if (deferred_count_ == 0 && !too_new(update)) {
            apply_texture_update(update);
            continue;
        }
        if (deferred_count_ == k_deferred_updates) {
            // Out of room: applying early beats dropping. The oldest goes first, so order
            // still holds; the cost is a frame that may show new pixels a frame early.
            if (!warned_deferred_full_) {
                warned_deferred_full_ = true;
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                "texture update deferral full — applying "
                                                "ahead of its snapshot. Reported once.");
            }
            texture_update& oldest = deferred_updates_[deferred_head_];
            apply_texture_update(oldest);
            oldest = {};
            deferred_head_ = (deferred_head_ + 1) % k_deferred_updates;
            --deferred_count_;
        }
        deferred_updates_[(deferred_head_ + deferred_count_) % k_deferred_updates] = update;
        ++deferred_count_;
    }
}

void compositor_impl::apply_texture_update(texture_update& update) {
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
            applied = gl_compositor_.upload_rgba_region(update.texture_id, update.width,
                                                        update.height, update.region,
                                                        update.pixels);
            // glTexImage2D/glTexSubImage2D copy out of client memory before returning,
            // so the staging block is free again right away.
            shm_pool_.release(update.shm_pool_token);
            if (!reported_partial_fresh_ && gl_compositor_.partial_into_fresh() > 0) {
                reported_partial_fresh_ = true;
                lx::trace::logger::global().log(
                    lx::trace::level::warn, "compositor.render",
                    "partial shm upload into a texture with no prior content — staging "
                    "bookkeeping disagrees with the GL slot (reported once)");
            }
        } else if (vulkan_present_) {
            applied = vk_compositor_.upload_rgba(update.texture_id, update.width,
                                                 update.height, update.pixels);
            // The ring slot is free the moment the upload has been recorded.
            shm_staging_.publish_consumed(update.shm_seq);
        } else {
            shm_staging_.publish_consumed(update.shm_seq);
            shm_store_.release(update.shm_token);
            shm_pool_.release(update.shm_pool_token);
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

    if (update.kind == texture_update::op::shm)
        trace_texture_op("upload", update.texture_id, static_cast<bool>(applied));
    else if (update.kind == texture_update::op::dmabuf)
        trace_texture_op("import", update.texture_id, static_cast<bool>(applied));
    else if (update.kind == texture_update::op::forget)
        trace_texture_op("retire-queued", update.texture_id, true);
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
    scanout_rect_ = {0, 0, static_cast<int>(width), static_cast<int>(height)};
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

    scanout_rect_ = {0, 0, static_cast<int>(width), static_cast<int>(height)};
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
    scanout_rect_ = {0, 0, static_cast<int>(width), static_cast<int>(height)};
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
    // The flip landed, so clients may draw again regardless of how complete the feedback is
    // — unless it only moved the pointer: no client content was presented by it.
    if (impl->last_commit_content_.load(std::memory_order_acquire))
        impl->present_completed_ = true;

    // Phase-lock the frame clock to the display, and aim each repaint at its deadline
    // rather than at the interval boundary. A timer free-running at target_fps drifts
    // against the real vblank cadence, so ticks land while a flip is still queued and their
    // composite is wasted — the visible rate then sits well under the refresh rate for
    // reasons that look like slow rendering.
    //
    // The start time comes from the measured repaint cost, so the idle wait sits before the
    // work instead of after it and the frame carries input from as late as it can. A frame
    // that gets more expensive starts earlier on its own.
    //
    // The phase comes from the kernel's flip timestamp when it is on our clock (KMS reports
    // CLOCK_MONOTONIC, which is what steady_clock reads on Linux) and plausibly recent;
    // otherwise from `now`, which is within the 1 ms loop pump of the flip because flips
    // are drained there (wayland_fd_callback). Taking `now` at the next tick instead put the
    // phase up to a whole period late, so ticks ran at period − lead: faster than the
    // display, every few frames composited behind a still-pending flip ("deferred").
    if (impl->vsync_timer) {
        const auto now = static_cast<lx::runtime::clock_time>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        impl->repaint_.observe_repaint(impl->budget_.last_tick_ui_us(),
                                       impl->render_cost_us_.load(std::memory_order_relaxed));
        auto flip_ns = now;
        if (fb.hw_clock && fb.actual_present_ns > 0 && fb.actual_present_ns <= now &&
            now - fb.actual_present_ns < impl->repaint_.refresh_period())
            flip_ns = fb.actual_present_ns;
        impl->repaint_.on_present(flip_ns);
        impl->vsync_timer->schedule_at(impl->repaint_.next_start(now));
    }

    // Taken on every flip, sampled or not, so the dup'd fence is always closed.
    const unsigned long long submit_us =
        impl->committed_submit_us_.exchange(0, std::memory_order_acq_rel);
    unsigned long long fence_us = 0;
    if (const int fd = impl->committed_fence_fd_.exchange(-1, std::memory_order_acq_rel);
        fd >= 0) {
        fence_us = sync_file_signal_us(fd);
        ::close(fd);
    }
    if (fb.hw_clock)
        impl->note_present_latency(fb.actual_present_ns, submit_us, fence_us);

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

void seat_manager::set_focus(lx::surface_id surface) {
    if (impl_)
        impl_->input_devices_.default_seat().set_keyboard_focus(surface);
}

lx::input::seat& seat_manager::seat() {
    // Falls back to a shared instance only when unbound, so callers always get something
    // valid — but an unbound manager means focus changes go nowhere, which is worth not
    // hiding behind a silent default.
    if (!impl_) {
        static lx::input::seat orphan;
        return orphan;
    }
    return impl_->input_devices_.default_seat();
}

void seat_manager::bind(detail::compositor_impl* impl) { impl_ = impl; }

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

    // Graphics. A Vulkan device is not free: where the only Vulkan driver is a software
    // rasterizer (lavapipe), creating one initialises LLVM and its JIT — about 28 MB of heap
    // (heaptrack) for a device the GL path never composites with. So probe with an instance
    // only, and create the device now only when Vulkan may composite; the fallbacks below
    // create it on demand. Failure must not abort compositor start.
    {
        const auto want = impl_->cfg.present;
        const bool vulkan_may_composite =
            want == present_backend::vulkan || want == present_backend::cpu ||
            (want == present_backend::automatic && gfx::device_selector::has_hardware_vulkan());
        if (vulkan_may_composite) {
            impl_->ensure_graphics_device();
        } else {
            impl_->headless_present_ = true;
            lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                            "no hardware Vulkan — Vulkan device not created");
        }
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
            // The CPU path imports client dma-bufs through the Vulkan importer.
            impl_->ensure_graphics_device();
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
            impl_->ensure_graphics_device();
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

    // GL composites, and imports client dma-bufs, through EGL. A Vulkan device exists here
    // only if start-up expected to composite with it (hardware Vulkan, or an explicit
    // request) and GL won anyway; kept, it would hold its driver for nothing. Nothing on
    // the GL path reaches it: dma-buf feedback takes its device from the KMS card, and the
    // UI-side import cache serves Vulkan only.
    if (impl_->gl_present_ && impl_->graphics.vk_device()) {
        impl_->graphics = gfx::device{};
        impl_->graphics.syncobj().set_drm_fd(impl_->kms.card_fd());
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                        "Vulkan device released (unused on the GL path)");
    }
#if defined(__GLIBC__)
    // Start-up frees a lot (instance probing, a released device, driver set-up), and glibc
    // keeps freed heap pages resident until told otherwise. Once, here — never per frame.
    (void)::malloc_trim(0);
#endif

    (void)impl_->hotplug_.start(impl_->outputs, impl_->kms);
    if (impl_->outputs.count() > 0) {
        const lx::rect2i g = impl_->outputs.geometry(impl_->outputs.nth(0));
        unsigned refresh_mhz = 0;
        if (auto mode = impl_->kms.active_mode(0); mode)
            refresh_mhz = mode.value().refresh_millihz;
        else // the virtual output (headless, nested)
            refresh_mhz = impl_->outputs.state(impl_->outputs.nth(0)).refresh_mhz;
        char line[96];
        std::snprintf(line, sizeof(line), "output 0 mode=%dx%d refresh_mhz=%u", g.width, g.height,
                      refresh_mhz);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.output", line);
    }
    impl_->log_outputs();
    impl_->output_generation_ = impl_->outputs.generation();

    if (auto bound = impl_->wayland.bind(impl_->cfg.socket_name); !bound) {
        lx::trace::logger::global().log_error(bound.get_error(), "compositor");
        return bound.get_error();
    }

    // Cursor: theme first (it decides how large the plane's buffers must be), then the
    // plane, then the manager that feeds both. Startup-only file I/O.
    {
        auto* impl = impl_;
        impl->cursor_theme_.load(nullptr, 0);
        char line[160];
        std::snprintf(line, sizeof(line), "cursor theme=%s size=%u from_theme=%u builtin=%u",
                      impl->cursor_theme_.name(), impl->cursor_theme_.size(),
                      impl->cursor_theme_.from_theme(), impl->cursor_theme_.builtin());
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.cursor", line);

        const char* mode = std::getenv("LUMEN_CURSOR");
        const bool force_software = mode && std::strcmp(mode, "software") == 0;
        const char* why_software = nullptr;
        if (force_software) {
            why_software = "LUMEN_CURSOR=software";
        } else if (!impl->drm_present_) {
            why_software = "no KMS scanout";
        } else {
            const unsigned need = impl->cursor_theme_.max_dimension();
            auto plane = drm::kms_cursor_plane::create(impl->kms.card_fd(),
                                                       impl->kms.crtc_id_for(0),
                                                       impl->kms.crtc_index(), need, need);
            if (plane) {
                impl->cursor_plane_ = std::move(plane).value();
            } else {
                why_software = plane.get_error().message;
            }
        }
        if (impl->cursor_plane_.valid()) {
            const auto caps = impl->cursor_plane_.caps();
            std::snprintf(line, sizeof(line), "hardware cursor plane=%u size=%ux%u hotspot=%d",
                          caps.plane_id, caps.width, caps.height, caps.hotspot ? 1 : 0);
        } else {
            std::snprintf(line, sizeof(line), "software cursor: %s",
                          why_software ? why_software : "no cursor plane");
        }
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.cursor", line);

        cursor_manager::setup setup{};
        setup.theme = &impl->cursor_theme_;
        setup.frames = &impl->cursor_frames_;
        setup.images = &impl->cursor_images_;
        setup.router = &impl->input_router_;
        setup.scene = &impl->scene;
        setup.overlay = &impl->cursor_layer_;
        setup.hardware = impl->cursor_plane_.valid();
        if (setup.hardware) {
            const auto caps = impl->cursor_plane_.caps();
            setup.plane_size = {static_cast<int>(caps.width), static_cast<int>(caps.height)};
        }
        impl->cursor_.init(setup);
    }

    impl_->register_p0_globals();
    impl_->start_input();

    // The pointer starts in the middle of output 0, where it is easy to find.
    if (impl_->outputs.count() > 0) {
        const lx::rect2i g = impl_->outputs.geometry(impl_->outputs.nth(0));
        auto& seat = impl_->input_devices_.default_seat();
        seat.set_pointer_position(g.x + g.width / 2, g.y + g.height / 2);
        impl_->cursor_.on_motion(seat.pointer_x(), seat.pointer_y(), 0, false);
    }

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
    // DRM page-flip events are drained by the Wayland pump above while a flip is pending
    // (and again in tick_ui), not via a dedicated fd_source — DRM fds often report POLLIN
    // continuously and would busy-spin.

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
    // Whatever the last window collected; a short session still reports its numbers.
    impl_->log_latency_summary();
    detail::compositor_impl::active = nullptr;
    return code;
}

void compositor::request_stop() {
    if (!impl_) return;
    impl_->loop.quit(0);
}

void compositor::inject_key(const lx::input::key_event& event) {
    if (!impl_)
        return;
    auto& seat = impl_->input_devices_.default_seat();
    const bool mods_changed = seat.keymap().update_key(event.evdev_keycode, event.pressed);
    seat.note_key(event.evdev_keycode, event.pressed);
    impl_->on_input_key(event);
    if (mods_changed)
        impl_->on_input_modifiers(seat.keymap().modifiers());
}

void compositor::inject_pointer_motion(const lx::input::pointer_motion_event& event) {
    if (!impl_)
        return;
    lx::input::pointer_motion_event e = event;
    e.absolute = false;
    impl_->input_devices_.default_seat().set_pointer_position(event.x, event.y);
    impl_->on_input_pointer_motion(e);
}

void compositor::inject_pointer_button(const lx::input::pointer_button_event& event) {
    if (impl_)
        impl_->on_input_pointer_button(event);
}

config compositor::configuration() const { return impl_ ? impl_->cfg : config{}; }
wayland::server& compositor::wayland() { return impl_->wayland; }
surface_manager& compositor::surfaces() { return impl_->surfaces; }
toplevel_manager& compositor::toplevels() { return impl_->toplevels; }
window_manager& compositor::windows() { return impl_->wm_; }
shell_bridge& compositor::shell_bridge() { return impl_->shell_bridge_; }
cursor_manager& compositor::cursor() { return impl_->cursor_; }
scene::scene_graph& compositor::scene() { return impl_->scene; }
gfx::device& compositor::graphics() { return impl_->graphics; }
gfx::dmabuf_import_cache& compositor::import_cache() { return impl_->import_cache; }
output_manager& compositor::outputs() { return impl_->outputs; }
liveness_tracker& compositor::liveness() { return impl_->liveness_; }
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
    // before any Wayland dispatch below. What is committed from here on belongs to this
    // tick's snapshot at the earliest.
    impl_->p0_.frame_index = tick.frame_index;
    impl_->p0_.building_frame = tick.frame_index;
    impl_->budget_.begin_frame(tick.frame_index);
    impl_->p0_.now_ns = static_cast<unsigned long long>(tick.timestamp_ns);
    // Staging: free retired CPU-path storage the render thread has let go of, unmap GL
    // blocks idle past the threshold, and report what is left against the budget.
    impl_->shm_store_.collect();
    impl_->shm_pool_.collect(impl_->p0_.now_ns, detail::compositor_impl::k_staging_idle_ns);
    {
        const unsigned long long staged = impl_->shm_store_.bytes_held() +
                                          impl_->shm_pool_.bytes_mapped();
        const unsigned clamped =
            staged > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<unsigned>(staged);
        impl_->memory_.report_usage(lx::runtime::memory_pool::staging, clamped, 0);
    }
    impl_->memory_.tick(tick.frame_index);
    impl_->memory_.reset_frame_arenas(lx::runtime::memory_pool::ui);

    impl_->hotplug_.poll();

    for (unsigned i = 0; i < impl_->outputs.count(); ++i) {
        const float scale = impl_->protocols_.fractional_scale(i);
        if (scale > 0.f)
            impl_->outputs.set_scale(impl_->outputs.nth(i), scale);
    }

    // An output appeared, went, or changed mode, position or scale: clients are told (each
    // wl_output and xdg_output, then done) and every surface's membership is recomputed.
    // Compared by generation: the hotplug poll refreshes every tick.
    if (impl_->outputs.generation() != impl_->output_generation_) {
        impl_->output_generation_ = impl_->outputs.generation();
        impl_->log_outputs();
        notify_outputs_changed(impl_->p0_);
    }

    // Nothing has been drawn yet on the first tick, and a mode change invalidates every
    // pixel; anything else is repainted only where it changed.
    if (impl_->outputs.count() > 0) {
        const lx::rect2i g = impl_->outputs.geometry(impl_->outputs.nth(0));
        const lx::rect2i& last = impl_->last_output_rect_;
        if (g.x != last.x || g.y != last.y || g.width != last.width || g.height != last.height) {
            impl_->last_output_rect_ = g;
            scene().note_full_damage();
            // Layer surfaces are arranged against the output, and the usable area with them.
            impl_->wm_.on_output_changed();
        }
    }
    if (!impl_->initial_damage_noted_) {
        impl_->initial_damage_noted_ = true;
        scene().note_full_damage();
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

    // Window management for this frame — configures coalesced since the last tick go out
    // here, once, rather than once per request that changed them.
    impl_->wm_.on_tick(tick.timestamp_ns);
    // Frames: the atlas on first use and changed titles, rasterized once per tick however
    // many set_title requests arrived — in this tick's snapshot, with the geometry they fit.
    impl_->decorations_.on_tick();
    // The pointer image, after the window manager (a grab sets the override) and before the
    // scene is published (the software cursor is a scene node).
    if (impl_->cursor_hw_failed_.load(std::memory_order_acquire))
        impl_->cursor_.on_hardware_failed();
    impl_->cursor_.on_tick();

    scheduler().on_vsync(tick);
    scene().update(tick.delta_seconds);

    const bool nothing_changed = scene().pending_damage().empty();
    if (scene().commit_frame(tick.frame_index)) {
        // Published: commits dispatched between now and the next tick are the next
        // snapshot's. A dropped publish keeps this frame, whose damage carries forward.
        impl_->p0_.building_frame = tick.frame_index + 1;
        impl_->buffer_lifecycle_.on_frame_submitted(tick.frame_index);
        if (!nothing_changed)
            impl_->last_damaged_frame_ = tick.frame_index;
    } else {
        lx::trace::logger::global().log(lx::trace::level::trace, "compositor",
                                        "frame dropped (backpressure)");
    }

    // An idle tick produces no flip, and frame callbacks otherwise wait for one — a client
    // that commits a frame request without new content would then stall until the
    // watchdog. Once every damaged frame has been composited and committed and no flip is
    // queued, nothing is waiting on the display: answer now (wlroots' "frame done without
    // a commit"). The reads are ordered against render's writes: last_rendered_frame_ is
    // published after frame_outstanding_ is raised, and frame_outstanding_ is lowered only
    // after the commit has set flip_pending.
    if (impl_->drm_present_ && nothing_changed &&
        impl_->last_rendered_frame_.load(std::memory_order_acquire) >= impl_->last_damaged_frame_ &&
        !impl_->frame_outstanding_.load(std::memory_order_acquire) &&
        !impl_->kms_atomic.flip_pending()) {
        impl_->last_callback_ns_ = tick.timestamp_ns;
        fire_frame_callbacks(impl_->p0_,
                             static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull));
    }

    // Release client buffers the render thread has finished sampling. Done here because
    // sending wl_buffer.release requires the UI thread's ownership of Wayland dispatch.
    (void)impl_->buffer_lifecycle_.retire_completed_frames(
        impl_->last_rendered_frame_.load(std::memory_order_acquire));

    // Configures and events produced by this tick leave now, not at the next poll.
    wayland().flush();

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

    // What changed since the last frame this thread saw. A re-acquired snapshot (the UI
    // published nothing new) contributes nothing; a skipped one was folded into this one
    // by `snapshot_buffer::publish`.
    gfx::damage_region frame_damage = impl_->missed_damage_;
    impl_->missed_damage_.clear();
    if (idx != impl_->last_seen_frame_) {
        frame_damage.add_region(snapshot.damage_rects());
        impl_->last_seen_frame_ = idx;
    }

    // Translated only when something is drawn: an idle tick touches no draw data at all.
    const unsigned blit_count = draws.size();
    const auto build_blits = [&]() -> lx::gfx::blit_command* {
        lx::gfx::blit_command* out = impl_->blit_storage(blit_count);
        for (unsigned i = 0; i < blit_count; ++i) {
            const auto& cmd = draws.data()[i];
            out[i].texture_id = cmd.texture.id();
            out[i].dst = cmd.dst;
            out[i].src = cmd.src;
            out[i].clip = cmd.clip;
            out[i].tint = cmd.tint;
            out[i].opacity = cmd.opacity;
            out[i].blend = cmd.blend;
            out[i].buffer_xform = cmd.buffer_xform;
            out[i].src_space = cmd.src_space;
        }
        return out;
    };

    impl_->drain_texture_updates(idx);

    // A CRTC takes one flip at a time, so a frame arriving while one is queued cannot be
    // shown yet — but with three scanout slots it no longer has to be thrown away either.
    // It composites into the slot that is neither on screen nor queued, and the commit is
    // what waits. The frame clock is phase-locked to the flips, so this is rare.
    const bool flip_in_flight = impl_->drm_present_ && impl_->kms_atomic.flip_pending();
    const bool scanout_path = (impl_->gl_present_ || impl_->cpu_present_ ||
                               impl_->vulkan_present_) &&
                              impl_->scanout_slot_count_ > 0;
    bool composited_this_tick = false;
    unsigned scanout_back = 0;
    gfx::damage_region region{};

    if (scanout_path) {
        frame_damage.clip(impl_->scanout_rect_);
    }

    if (scanout_path && !frame_damage.empty()) {
        // Buffer age: the slot about to be drawn into also missed every frame composited
        // since it was last used, so its region is this frame's damage plus that.
        scanout_back = impl_->pick_back_slot();
        region = frame_damage;
        region.add_region(impl_->slot_age_damage_[scanout_back]);
        region.clip(impl_->scanout_rect_);
        lx::gfx::blit_command* blits = build_blits();

        if (impl_->gl_present_) {
            auto composited = impl_->gl_compositor_.composite_region(
                impl_->gl_targets_[scanout_back], lx::color::rgb(0.f, 0.f, 0.f), blits,
                blit_count, region);
            if (!composited) {
                lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
            } else {
                composited_this_tick = true;
                // This frame supersedes any uncommitted one, so its fence replaces that
                // frame's — which is dropped rather than leaked.
                impl_->discard_composited_fence();
                impl_->composited_fence_fd_ = composited.value().out_fence_fd;
                if (composited.value().draws_skipped > 0)
                    impl_->report_unregistered_draw(blits, blit_count);
            }
        } else if (impl_->cpu_present_) {
            const auto& fb = impl_->dumb_fbs_[scanout_back];
            gfx::pixel_surface target{};
            target.pixels = fb.pixels();
            target.width = fb.width();
            target.height = fb.height();
            target.stride = fb.stride();
            target.format = fb.format();
            target.write_combining = true;

            auto composited = impl_->cpu_compositor_.composite_region(
                target, lx::color::rgb(0.f, 0.f, 0.f), blits, blit_count, region);
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
        } else {
            // Vulkan repaints the whole target, which is correct for any buffer age.
            auto composited = impl_->vk_compositor_.composite(
                impl_->vk_targets_[scanout_back], lx::color::rgb(0.f, 0.f, 0.f), blits,
                blit_count);
            if (!composited) {
                lx::trace::logger::global().log_error(composited.get_error(), "compositor.render");
            } else {
                composited_this_tick = true;
                // Same contract as the GL path: this frame supersedes any uncommitted one,
                // so its fence replaces that frame's rather than leaking alongside it. The
                // commit waits on the GPU instead of the render thread doing so.
                impl_->discard_composited_fence();
                impl_->composited_fence_fd_ = composited.value().out_fence_fd;
                if (composited.value().draws_skipped > 0)
                    impl_->report_unregistered_draw(blits, blit_count);
            }
        }

        if (composited_this_tick) {
            for (unsigned slot = 0; slot < impl_->scanout_slot_count_; ++slot) {
                if (slot != scanout_back)
                    impl_->slot_age_damage_[slot].add_region(frame_damage);
            }
            impl_->slot_age_damage_[scanout_back].clear();
            impl_->kms_pending_damage_.add_region(frame_damage);
            impl_->frame_outstanding_.store(true, std::memory_order_release);
        } else {
            // Not drawn: try again next tick rather than lose it.
            impl_->missed_damage_.add_region(frame_damage);
        }
    } else if (!scanout_path && impl_->headless_present_ && impl_->headless_.width() > 0) {
        // Software fallback for no-GPU hosts and CI.
        (void)impl_->headless_.begin_frame();
        (void)impl_->headless_.clear(lx::color::rgb(0.f, 0.f, 0.f));
        if (blit_count > 0)
            (void)impl_->frame_renderer_.draw_to_headless(build_blits(), blit_count,
                                                          impl_->headless_);
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

    const char* commit_kind = "none";
    if (impl_->drm_present_ && impl_->composited_slot_ >= 0 && flip_in_flight)
        commit_kind = "deferred";

    // As late as possible: the cursor position used is the newest the UI has published.
    // Taken whether or not a plane exists — its input time is what latency is measured from.
    if (impl_->cursor_frames_.take())
        impl_->cursor_cur_ = impl_->cursor_frames_.front();
    if (impl_->drm_present_)
        impl_->prepare_cursor_plane();
    bool cursor_rode = false;

    if (impl_->drm_present_ && !flip_in_flight && impl_->composited_slot_ >= 0) {
        const auto present_slot = static_cast<unsigned>(impl_->composited_slot_);

        const auto submit_ns = static_cast<runtime::clock_time>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        presentation().on_present_submitted(idx, submit_ns);

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

        // Every rect since the last commit, as separate clips: the plane's previous
        // framebuffer is older than this frame's damage alone describes.
        const gfx::damage_region& pending = impl_->kms_pending_damage_;
        if (pending.full || pending.count == 0) {
            req.damage.full_frame = true;
        } else {
            req.damage.full_frame = false;
            req.damage.count = pending.count;
            for (unsigned i = 0; i < pending.count; ++i)
                req.damage.rects[i] = pending.rects[i];
            impl_->kms_atomic.build_damage_blob(req.damage, req.connector);
        }

        // The pointer rides the frame: both change on the same vblank.
        if (impl_->content_committed_once_ && impl_->cursor_plane_.valid() &&
            impl_->cursor_plane_.dirty()) {
            req.cursor = &impl_->cursor_plane_;
            cursor_rode = true;
        }

        impl_->pending_flip_slot_.store(present_slot, std::memory_order_release);
        impl_->last_commit_content_.store(true, std::memory_order_release);
        impl_->publish_commit_timing(impl_->composited_fence_fd_);
        const auto committed = impl_->kms_atomic.commit(req);
        if (!committed)
            impl_->retract_commit_timing();
        if (!committed && cursor_rode) {
            // Do not let the plane take frames down with it: if the cursor state alone is
            // rejected, the plane is unusable and the frame retries without it.
            drm::atomic_commit_request test{};
            test.update_primary = false;
            test.cursor = &impl_->cursor_plane_;
            test.request_page_flip = false;
            test.test_only = true;
            // Blame the plane only for a state the kernel refused: losing DRM master
            // (a VT switch) fails TEST_ONLY too, and must not cost the session its
            // hardware cursor.
            if (!impl_->kms_atomic.commit(test) &&
                impl_->kms_atomic.last_failure_rejected_state()) {
                impl_->fail_cursor_plane("commit rejected the cursor state");
                impl_->cursor_plane_.set_visible(false);
            }
        }
        if (committed) {
            impl_->content_committed_once_ = true;
            impl_->composited_slot_ = -1;
            // The kernel has taken its own reference to the fence.
            impl_->discard_composited_fence();
            impl_->kms_pending_damage_.clear();
            commit_kind = "content";
            impl_->note_committed_input(true);
        } else {
            // Keep the slot and its fence: the commit is retried next tick without
            // compositing the frame again.
            commit_kind = "failed";
        }
        // Lowered after the commit raised flip_pending (see the idle-callback check).
        impl_->frame_outstanding_.store(!committed, std::memory_order_release);
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
    } else if (impl_->drm_present_ && !composited_this_tick && impl_->composited_slot_ < 0 &&
               impl_->content_committed_once_ && impl_->cursor_plane_.valid() &&
               impl_->cursor_plane_.dirty() && impl_->cursor_plane_.involves_crtc() &&
               !impl_->kms_atomic.flip_pending()) {
        // Nothing to composite, but the pointer moved: a commit of the cursor plane alone.
        // The scene is not touched — this is what keeps pointer motion off the GPU.
        drm::atomic_commit_request req{};
        req.connector = 0;
        req.update_primary = false;
        req.cursor = &impl_->cursor_plane_;
        req.request_page_flip = true;
        // Its flip re-arms the frame clock but presents no client content, and must not
        // rotate the scanout slots.
        impl_->pending_flip_slot_.store(impl_->scanout_front_.load(std::memory_order_acquire),
                                        std::memory_order_release);
        impl_->last_commit_content_.store(false, std::memory_order_release);
        impl_->publish_commit_timing(-1);
        if (auto committed = impl_->kms_atomic.commit(req); committed) {
            commit_kind = "cursor";
            impl_->note_committed_input(false);
        } else {
            impl_->retract_commit_timing();
            commit_kind = "cursor-failed";
            // EBUSY (a flip raced in) and EACCES (DRM master on another VT) are transient;
            // only a state the plane itself refuses, confirmed by TEST_ONLY, disables it.
            drm::atomic_commit_request test{};
            test.update_primary = false;
            test.cursor = &impl_->cursor_plane_;
            test.request_page_flip = false;
            test.test_only = true;
            if (!impl_->kms_atomic.commit(test) &&
                impl_->kms_atomic.last_failure_rejected_state()) {
                lx::trace::logger::global().log_error(committed.get_error(), "compositor.cursor");
                impl_->fail_cursor_plane("cursor-only commit rejected");
                impl_->cursor_plane_.set_visible(false);
            }
        }
    }

    // One line per tick that did something; an idle tick is silent. Checked before
    // formatting so the frame path pays nothing when debug is off.
    if ((composited_this_tick || commit_kind[0] != 'n') &&
        lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        const auto so_far_us = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        char line[160];
        std::snprintf(line, sizeof(line),
                      "frame=%u composite=%d rects=%u area=%llu commit=%s render_us=%u", idx,
                      composited_this_tick ? 1 : 0, region.count, region.area(), commit_kind,
                      so_far_us);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.render", line);
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
