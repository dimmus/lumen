module;

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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
    /// icon, the session lock's surfaces, input-method popups, then the software cursor. Popups
    /// sit above the overlay layer so no panel or window ever covers a menu; the drag icon
    /// above everything a client draws. The four layer-shell containers hold layer surfaces'
    /// tree containers (`:layer_shell`); the lock layer holds lock surfaces'
    /// (wire_session_lock.cpp). While the session is locked every layer below the lock layer
    /// is hidden. Input-method popups (wire_input_method.cpp) are above the lock layer: they
    /// are only ever shown for the focused text input, which while locked is a lock
    /// surface's.
    scene::container_node background_layer_{};
    scene::container_node bottom_layer_{};
    scene::container_node windows_layer_{};
    scene::container_node top_layer_{};
    scene::container_node overlay_shell_layer_{};
    scene::container_node popup_layer_{};
    scene::container_node dnd_icon_layer_{};
    scene::container_node lock_layer_{};
    scene::container_node ime_popup_layer_{};
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

    // ── Session pause/resume (VT switching) — docs/architecture.md "Session pause/resume" ──
    /// UI affinity. The state machine; `apply_session_actions` executes what it returns.
    session_state session_{};
    /// UI affinity. VT and logout bindings, the shell's key grabs, tap observation.
    keybindings keys_{};
    /// The display gate (design §2.3). UI writes (release), render reads (acquire) at tick
    /// start and again right before every commit ioctl: while any bit is set, render
    /// composites nothing and commits nothing, the frame clock gives way to the 1 Hz paused
    /// tick, and nothing is presented. `gate_vt`: the session is paused (VT away, display
    /// lost). `gate_power`: the output is powered off (display power, I4). Only input
    /// suspension stays tied to `gate_vt` — a dark output is woken by input.
    static constexpr unsigned gate_vt = 1u << 0;
    static constexpr unsigned gate_power = 1u << 1;
    std::atomic<unsigned> display_gate_{0};
    /// UI stores the resume generation (release) *before* clearing a gate bit, and render
    /// loads them in the opposite order: seeing "ungated" guarantees seeing the new
    /// generation, so render never commits without running the resume procedure first.
    /// One counter for every resume (VT and power-on), owned by UI (`render_gen_counter_`).
    std::atomic<unsigned> resume_gen_{0};
    /// UI affinity. The last render generation handed out, and which resume each answers:
    /// the session machine's (with its own generation) and the power machine's.
    unsigned render_gen_counter_ = 0;
    unsigned vt_resume_render_gen_ = 0;
    unsigned vt_resume_session_gen_ = 0;
    unsigned power_resume_render_gen_ = 0;
    /// Render affinity. The generation whose resume procedure ran, and a result owed to UI.
    unsigned resume_done_gen_ = 0;
    bool resume_result_pending_ = false;
    /// Render affinity. Commits have failed for lack of DRM master since then (0: they do
    /// not) — reported once if no pause follows within a second.
    lx::runtime::clock_time master_lost_since_ns_ = 0;
    bool master_lost_reported_ = false;
    /// UI affinity. The resume modeset's flip has not arrived yet: a full modeset can take
    /// longer than the 250 ms present watchdog on some drivers, which is not a stall.
    bool awaiting_resume_flip_ = false;
    static constexpr lx::runtime::clock_time k_resume_flip_grace_ns = 1'000'000'000;
    /// The card's device number (what logind's signals name) and how it was opened.
    unsigned drm_major_ = 0;
    unsigned drm_minor_ = 0;
    bool drm_via_logind_ = false;
    bool drm_start_inactive_ = false;
    /// UI affinity. logind signals recorded inside sd_bus_process, handled right after it
    /// (`on_session_events`): the callbacks only record.
    struct session_event {
        enum class kind : unsigned char { paused, resumed, active, vt_failed, lock, unlock, sleep };
        kind what = kind::paused;
        unsigned major = 0;
        unsigned minor = 0;
        session_pause_kind pause = session_pause_kind::force;
        bool active = false;
        const char* error = "";
    };
    /// Never drops an event: an unacknowledged `pause` would hold the seat's switch
    /// forever (logind waits for every device), and nothing in logind's state says which
    /// one was missed. Reserved at startup; only a burst past that allocates, on the UI
    /// thread, at a VT switch — never in a frame.
    std::vector<session_event> session_events_;
    void record_session_event(const session_event& ev);
    /// UI. After every logind dispatch: feeds the recorded signals to the state machine.
    void on_session_events();
    /// UI. Executes a state-machine decision (A.3 pause edge, A.5 resume edge).
    void apply_session_actions(const session_actions& a);
    void enter_display_pause(unsigned quiesce_gen);
    void suspend_input_for_pause();
    void resume_input_after_pause();
    void begin_display_resume(unsigned gen);

    // ── Idle, inhibition and display power — docs/architecture.md "Idle, inhibition and
    //    display power" ──
    /// UI affinity. ext_idle_notifier_v1 notifications and zwp_idle_inhibitor_v1 objects.
    idle_tracker idle_{};
    inhibitor_set inhibitors_{};
    /// UI affinity. Armed at the tracker's next deadline (lazily: activity does not re-arm it).
    std::unique_ptr<runtime::timer_source> idle_timer_{};
    static void idle_timer_callback();
    /// UI. Arms (or disarms) the idle timer at `idle_.next_deadline()`.
    void rearm_idle_timer();
    /// UI. Real input arrived (design I2): idle notifications resume and their timers
    /// restart; a dark output is woken (I5). True when this event woke it — the caller then
    /// consumes a press (and so its release).
    bool on_user_activity();
    /// UI affinity. The output's power state machine (`:display_power`).
    display_power power_{};
    /// UI writes the newest power-off generation (release); the render task compares (acquire)
    /// and does nothing when a later request superseded it.
    std::atomic<unsigned> power_gen_{0};
    /// UI. Executes a power decision (the gate, the render task, the resume, `mode` events).
    void apply_power_actions(const power_actions& a);
    /// UI. zwlr_output_power_v1.set_mode.
    void request_output_power(bool on);
    /// UI → render → UI. The power-off task for `gen`.
    void post_power_off(unsigned gen);
    /// Render. Turns the CRTC off (or, refused, commits a black frame).
    power_off_outcome run_power_off(unsigned gen);
    /// Render. The soft blank: one black frame, cursor off, CRTC left on.
    void commit_black_frame();
    bool power_rejected_logged_ = false;
    // ── Session lock — docs/architecture.md "Session lock" ──
    /// UI affinity. ext-session-lock-v1's state machine (`:session_lock`); the wire unit and
    /// the compositor execute what it returns (`apply_lock_actions`).
    session_lock lock_{};
    /// UI affinity. text-input-v3 ↔ input-method-v2 relay (`:text_input`); the wire units
    /// (wire_text_input.cpp, wire_input_method.cpp) send what it decides, the router routes
    /// keys to the input method's grab while it is active.
    text_input_relay text_input_{};
    /// UI affinity. The first snapshot built with the scene locked: a content flip of it (or
    /// a later one) on the output is what lets `locked` go out (design L1).
    unsigned lock_frame_ = 0;
    /// UI affinity. The 1 s wait for lock surfaces, and the sleep delay's 2 s.
    std::unique_ptr<runtime::timer_source> lock_timer_{};
    static void lock_timer_callback();
    void rearm_lock_timer();
    /// UI affinity. While the system prepares to sleep with a lock asked for: the logind
    /// delay inhibitor is held until `locked` or this deadline (0: none).
    lx::runtime::clock_time sleep_lock_deadline_ns_ = 0;
    static constexpr lx::runtime::clock_time k_sleep_lock_wait_ns = 2'000'000'000;
    void release_sleep_delay(const char* why);
    /// Render → UI. A content commit of ours has reached the CRTC since start: before it, the
    /// console (or nothing) is on screen — none of the session.
    std::atomic<bool> ever_lit_{false};
    /// The state last logged (one info line per transition).
    lock_state logged_lock_state_ = lock_state::unlocked;
    /// UI. Executes a lock decision (the wire half through `session_lock_send`).
    void apply_lock_actions(const lock_actions& a);
    /// UI. Nothing of the session can be on screen right now: the VT is away, the output is
    /// powered off (or blanked black), or nothing of ours was ever committed. A lock that
    /// reaches `locking` then is `locked` at once.
    [[nodiscard]] bool display_dark() const;
    /// UI. `locking` ends when the display is dark (see above). After every edge that can
    /// darken it.
    void advance_lock();
    /// UI. logind's Lock / Unlock / PrepareForSleep.
    void on_logind_lock();
    void on_logind_unlock();
    void on_prepare_for_sleep(bool start);
    /// UI. Ctrl+Alt+F<n> (XF86Switch_VT_<n>).
    void request_vt_switch(unsigned vt);
    /// Render. The resume procedure (A.5) for generation `gen`.
    void run_resume_procedure(unsigned gen);
    /// Render → UI.
    void post_resume_result(unsigned gen, session_resume_outcome outcome);
    void post_flip_dispatch();
    [[nodiscard]] static lx::runtime::clock_time now_ns() {
        return static_cast<lx::runtime::clock_time>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
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
    /// Render affinity. Frame index of the snapshot composited into `composited_slot_`: the
    /// commit's `user_tag`, echoed by its flip event, which presentation feedback is
    /// resolved against (`presentation_flipped`).
    unsigned composited_frame_ = 0;
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
    /// UI affinity. Newest published frame index: frame callbacks go to the surfaces the
    /// visibility pass found on screen in it.
    unsigned last_published_frame_ = 0;
    /// UI affinity. wp_presentation feedback in flight (`p0_.presentation`).
    presentation_feedback_table presentation_feedback_{};
    /// UI affinity. Output 0's geometry as of the last tick; a change repaints everything.
    /// Debug screenshot (`compositor::request_screenshot`, SIGUSR1 in lumen-compositor):
    /// requested from any thread or a signal handler; UI forces a full repaint, render reads
    /// back the frame it composites next and writes a PPM. Never set in normal operation.
    std::atomic<bool> screenshot_requested_{false};
    std::atomic<bool> screenshot_pending_{false};
    unsigned screenshot_count_ = 0;
    void write_screenshot(unsigned slot);
    /// Output 0's logical rect and scale as last applied to the scene projection.
    lx::rect2i last_output_rect_{};
    double last_output_scale_ = 1.0;
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
    /// GL path: shm commits uploaded from the client's memory, no staging (see the class).
    shm_direct_uploads shm_direct_{};

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
    std::unique_ptr<runtime::event_source> logind_source{};

    /// The frame clock runs only while there is something to do. Two consecutive ticks that
    /// find nothing — no damage, no frame callback or buffer waiting on a later frame, no
    /// flip or commit outstanding, nothing the window manager deferred — stop it; any
    /// client request, input event or flip starts it again, aligned to the display. An
    /// idle desktop then wakes once a second for `housekeeping_timer` instead of 60 times.
    static constexpr unsigned k_idle_ticks = 2;
    unsigned quiet_ticks_ = 0;
    bool tick_quiet_ = false;
    /// While the clock is stopped: one tick a second for the deadlines only a tick notices
    /// (liveness pings, unmapping idle staging blocks).
    std::unique_ptr<runtime::timer_source> housekeeping_timer{};
    static constexpr runtime::clock_time k_housekeeping_ns = 1'000'000'000;
    /// UI thread. Starts the frame clock if it is stopped.
    void request_frame();
    /// UI thread, after each tick: stop the clock if this tick was the k-th quiet one.
    void settle_frame_clock();

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
    bool worker_wake_ = false; // guarded by worker_mutex
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
        (void)scene.root().add(&lock_layer_);
        (void)scene.root().add(&ime_popup_layer_);
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
        // libinput closes its devices through the provider, which hands them back to
        // logind: that has to happen while the session object is still alive.
        input_devices_ = lx::input::input_manager{};
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

    // ── logout (UI affinity) ──
    /// Asks every window to close, then stops at `logout_deadline_ns_` or as soon as no
    /// toplevel is left, whichever comes first. Idempotent.
    void begin_logout(const char* source);
    /// tick_ui: stops the loop once the logout may finish.
    void advance_logout(lx::runtime::clock_time now_ns);
    static constexpr lx::runtime::clock_time k_logout_grace_ns = 5'000'000'000ull;
    bool logout_active_ = false;
    lx::runtime::clock_time logout_deadline_ns_ = 0;
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
    static void housekeeping_callback();
    static void after_dispatch_callback();
};

compositor_impl* compositor_impl::active = nullptr;

void compositor_impl::input_ready_callback() {
    if (!active)
        return;
    (void)active->input_devices_.dispatch();
    active->request_frame();
}

void compositor_impl::on_input_key(const lx::input::key_event& event) {
    lx::trace::scoped_span span{"input", "key"};
    auto& seat = input_devices_.default_seat();
    const auto& km = seat.keymap();
    key_input in{};
    in.code = event.evdev_keycode;
    in.pressed = event.pressed;
    in.time_ms = event.time_ms;
    in.sym = km.keysym(in.code);
    in.raw = km.level1_keysym(in.code, km.active_layout());
    in.raw0 = km.level1_keysym(in.code, 0);
    const auto named = km.named();
    in.mods = (named.shift ? key_mod_shift : 0u) | (named.ctrl ? key_mod_ctrl : 0u) |
              (named.alt ? key_mod_alt : 0u) | (named.super ? key_mod_super : 0u);
    // The seat already counts this key on a press (and no longer on a release).
    in.others_held = seat.pressed_count() > (event.pressed ? 1u : 0u) || wm_.buttons_down() > 0;

    // 0. Activity, before any binding or routing, so a consumed key still counts (I2). A
    //    release made by the device going away is not the user. A press that woke the
    //    display does nothing else (I5) — except the compositor's own bindings below.
    const bool woke = !event.device_removed && on_user_activity();
    // 1. The compositor's own bindings — always, also during a grab and with no shell: the
    //    keyboard ways out (Ctrl+Alt+F<n> to a text console, Ctrl+Alt+Backspace to log
    //    out). A consumed press is never forwarded, so the router swallows its release.
    if (const key_decision d =
            keybindings::internal(in, logind_.can_switch_vt(), logind_.vt_number());
        d.consume) {
        keys_.reset();
        if (d.action == key_action::switch_vt)
            request_vt_switch(d.vt);
        else if (d.action == key_action::logout)
            begin_logout("Ctrl+Alt+Backspace");
        return;
    }
    // The waking press: not forwarded, so the router swallows its release too (Enter that
    // wakes the screen does not also confirm a dialog — KWin's DpmsInputEventFilter).
    if (woke && event.pressed) {
        keys_.reset();
        return;
    }
    // 2. Keys the window manager claims (Escape during a grab or drag).
    if (wm_.on_key(event)) {
        keys_.reset();
        return;
    }
    // 3. The shell's grabs: never during a move, resize or drag, never while paused.
    const bool wm_busy = wm_.grab_active() || wm_.drag_active();
    if (!wm_busy && display_gate_.load(std::memory_order_relaxed) == 0) {
        if (const key_decision d = keys_.match_grab(in); d.consume) {
            if (p0_.fire_key_grab)
                p0_.fire_key_grab(d.grab, event.time_ms);
            return;
        }
    }
    // 4. Tap observation: the key still reaches the client; a tap fires after it. A
    //    release made by the device going away (logind revokes the fd at a VT switch
    //    before it signals the pause; an unplug) is not the user letting go of Super.
    if (event.device_removed)
        keys_.reset();
    void* tap = keys_.observe_tap(in);
    input_router_.send_key(event, seat);
    if (tap && !wm_busy && p0_.fire_key_grab)
        p0_.fire_key_grab(tap, event.time_ms);
}

void compositor_impl::request_vt_switch(unsigned vt) {
    if (auto switched = logind_.switch_vt(vt); !switched) {
        lx::trace::logger::global().log_error(switched.get_error(), "compositor.session");
        return;
    }
    char line[64];
    std::snprintf(line, sizeof(line), "VT switch to %u requested", vt);
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.session", line);
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
/// The logind bus. Dispatched when its socket is readable — and also when sd-bus already
/// holds a message, which it reads into its queue during a synchronous call: that one
/// arrives with no readability left to report it.
class logind_bus_source final : public lx::runtime::event_source {
public:
    logind_bus_source(lx::session::logind_session& session, void (*after)(void*), void* user)
        : session_{&session}, after_{after}, user_{user} {}
    bool prepare(lx::runtime::clock_time& timeout) override {
        timeout = session_->dispatch_timeout_ns();
        return ready_ || timeout == 0;
    }
    bool check() override { return ready_ || session_->dispatch_timeout_ns() == 0; }
    bool dispatch() override {
        ready_ = false;
        session_->dispatch();
        // The signal callbacks only recorded; acting on them (D-Bus calls, libinput) is
        // done here, after sd_bus_process has returned.
        if (after_)
            after_(user_);
        return true;
    }
    [[nodiscard]] int poll_fd() const override { return session_->bus_fd(); }
    void on_readable() override { ready_ = true; }

private:
    lx::session::logind_session* session_;
    void (*after_)(void*) = nullptr;
    void* user_ = nullptr;
    bool ready_ = false;
};

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
        const lx::rect2i l = o.logical_rect();
        char line[192];
        std::snprintf(line, sizeof(line),
                      "output %u name=%s mode=%dx%d refresh_mhz=%u scale=%g (%ld/120) "
                      "logical=%dx%d+%d+%d",
                      slot, o.name, o.mode.width, o.mode.height, o.refresh_mhz, o.scale,
                      std::lround(o.scale * 120.0), l.width, l.height, l.x, l.y);
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
    // Before the first content commit the plane joins the modeset only once it has passed
    // its test (a resume); at startup it is stated off there and joins the next commit.
    if ((!content_committed_once_ && !cursor_tested_) || kms_atomic.flip_pending())
        return;
    if (display_gate_.load(std::memory_order_acquire) != 0)
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
    lx::trace::scoped_span span{"input", "pointer_motion"};
    // Activity; motion that wakes the display is delivered all the same (I5).
    (void)on_user_activity();
    auto& seat = input_devices_.default_seat();
    // Output geometry is the compositor's to know — libinput reports deltas and has no
    // idea how large the desktop is — so both halves of the mapping live here. An absolute
    // device (a tablet, or the mouse of any VM console) arrives normalised to a unit box
    // and is scaled into the output; whatever the source, the pointer is then kept on
    // screen. lx.input accumulates only relative motion, which is why absolute devices
    // used to leave the pointer at the origin for the whole session.
    // Layout (logical) coordinates: the pointer moves in logical units at any scale
    // (wlroots, Mutter), and an absolute device covers the output's logical rect.
    if (outputs.count() > 0) {
        const lx::rect2i geometry = outputs.logical_rect(outputs.nth(0));
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
    // A press that wakes the display is consumed, and so is its release (I5).
    if (!event.device_removed && on_user_activity() && event.pressed) {
        keys_.reset();
        wm_.swallow_button_press(event);
        return;
    }
    if (event.pressed)
        keys_.note_other_input(); // Super+click is not a Super tap
    // The device is going away under a held button (the revoke at a VT switch comes before
    // logind's signal; an unplug): a move ends as Escape would and a drag is cancelled, so
    // the release cannot land a window or become a drop.
    if (!event.pressed && event.device_removed)
        wm_.cancel_for_pause();
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
    (void)on_user_activity(); // a scroll that wakes the display is delivered too
    keys_.note_other_input();
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
    // Every device libinput closes — at suspend, on unplug, at exit — goes back to logind:
    // without ReleaseDevice the TakeDevice of a resume fails with "Device already taken".
    provider.close_device = [](int fd, void* user) {
        if (auto* self = static_cast<compositor_impl*>(user))
            self->logind_.release_device_fd(fd);
        ::close(fd);
    };

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
    // Touch is not routed to clients yet; it is still the user, and wakes the display.
    sink.touch = [](const lx::input::touch_event&, void* u) {
        (void)static_cast<compositor_impl*>(u)->on_user_activity();
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
    lx::trace::scoped_span span{"ui", "wayland.pump"};
    (void)active->wayland.dispatch(0);
    // Whatever a client asked for is applied, configured and composited by a tick.
    active->request_frame();
}

void compositor_impl::drm_fd_callback() {
    if (!active || !active->drm_present_)
        return;
    // The moment the flip lands, not at the next poll: the flip handler phase-locks the
    // frame clock to it, and frame callbacks wait for it.
    active->kms_atomic.dispatch_events();
}

void compositor_impl::housekeeping_callback() {
    if (!active)
        return;
    if (active->session_.display_stopped() ||
        (active->display_gate_.load(std::memory_order_relaxed) & gate_power) != 0) {
        // The paused tick, once a second (VT away, or the output powered off): a lost
        // ResumeDevice (or a card logind does not manage) is noticed here, and clients
        // still get their frame callbacks.
        if (active->session_.display_stopped()) {
            const bool session_active =
                !active->logind_.has_control() || active->logind_.is_active();
            const bool master = session_active && active->kms.is_master();
            active->apply_session_actions(active->session_.on_paused_tick(session_active, master));
        }
        if (active->display_gate_.load(std::memory_order_relaxed) != 0)
            active->on_vsync();
        return;
    }
    // The frame clock is stopped: surfaces not on screen get their throttled frame
    // callbacks here, about once a second, without restarting it for them.
    if (frame_callbacks_throttled(active->p0_)) {
        const auto now = now_ns();
        fire_visible_frame_callbacks(active->p0_, static_cast<unsigned>(now / 1'000'000ull),
                                     active->last_published_frame_, now);
    }
    active->request_frame();
    // Nothing woke the clock but the calendar: one quiet tick is enough to stop again.
    active->quiet_ticks_ = k_idle_ticks - 1;
}

void compositor_impl::write_screenshot(unsigned slot) {
    // Render thread, on request only: a full-frame read back and a synchronous write. Not a
    // hot-path operation and never part of a normal frame.
    const auto& target = gl_targets_[slot];
    const unsigned w = target.width();
    const unsigned h = target.height();
    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4u);
    if (auto read = gl_compositor_.read_back(target, rgba.data(), static_cast<unsigned>(rgba.size()));
        !read) {
        lx::trace::logger::global().log_error(read.get_error(), "compositor.render");
        return;
    }
    const char* dir = std::getenv("LUMEN_SCREENSHOT_DIR");
    if (!dir || !*dir)
        dir = std::getenv("XDG_RUNTIME_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/lumen-screenshot-%u.ppm", dir ? dir : "/tmp",
                  ++screenshot_count_);
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                        "screenshot: cannot open the output file");
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    // GL_RGBA read-back yields R, G, B, A per pixel whatever the renderbuffer's storage.
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
        std::fwrite(&rgba[i * 4u], 1, 3, f);
    std::fclose(f);
    char line[600];
    std::snprintf(line, sizeof(line), "screenshot %ux%u written to %s", w, h, path);
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.render", line);
}

void compositor_impl::after_dispatch_callback() {
    if (active && active->screenshot_requested_.exchange(false, std::memory_order_acq_rel)) {
        // Every pixel repainted into the slot that is read back.
        active->scene.note_full_damage();
        active->screenshot_pending_.store(true, std::memory_order_release);
        active->request_frame();
    }
    // Events produced anywhere in this iteration (input, protocol replies, releases) leave
    // in one flush rather than waiting for the next tick.
    if (active)
        active->wayland.flush();
}

void compositor_impl::request_frame() {
    // Paused or dark: the 1 Hz paused tick is the only clock; requests and flips do not
    // start one.
    if (!vsync_timer || vsync_timer->enabled() ||
        display_gate_.load(std::memory_order_relaxed) != 0)
        return;
    const auto now = static_cast<lx::runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    // On the display's phase: the vblank cadence continues while the clock is stopped, so
    // the last flip still predicts the next deadline.
    vsync_timer->schedule_at(repaint_.next_start(now));
    if (housekeeping_timer)
        housekeeping_timer->set_enabled(false);
    quiet_ticks_ = 0;
    // Time spent stopped is neither a frame interval nor a stalled present.
    last_vsync_time = std::chrono::steady_clock::now();
    last_callback_ns_ = 0;
}

void compositor_impl::settle_frame_clock() {
    if (!vsync_timer || display_gate_.load(std::memory_order_relaxed) != 0)
        return;
    if (!tick_quiet_) {
        quiet_ticks_ = 0;
        return;
    }
    if (++quiet_ticks_ < k_idle_ticks)
        return;
    vsync_timer->set_enabled(false);
    if (housekeeping_timer)
        housekeeping_timer->schedule_at(
            static_cast<lx::runtime::clock_time>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()) +
            k_housekeeping_ns);
    lx::trace::timeline_instant("ui", "clock.idle");
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
    p0_.presentation = &presentation_feedback_;
    p0_.idle = &idle_;
    p0_.inhibitors = &inhibitors_;
    p0_.idle_user = this;
    p0_.idle_changed = [](void* u) {
        auto* self = static_cast<compositor_impl*>(u);
        self->rearm_idle_timer();
        self->request_frame(); // a tick evaluates inhibition with the newest frame
    };
    // Display power needs a display: without KMS (headless, nested) the objects are failed.
    if (drm_present_) {
        p0_.set_output_power = [](void* u, unsigned, bool on) {
            static_cast<compositor_impl*>(u)->request_output_power(on);
        };
        p0_.output_power_on = [](void* u, unsigned) {
            return static_cast<compositor_impl*>(u)->power_.reported_on();
        };
    }
    p0_.text_input = &text_input_;
    p0_.ime_popup_layer = &ime_popup_layer_;
    {
        wm_ime_ops ops{};
        ops.user = this;
        ops.hit = [](void* u, double x, double y, wm_hit& out) {
            auto* self = static_cast<compositor_impl*>(u);
            void* res = nullptr;
            lx::surface_id surface{};
            double sx = 0.0;
            double sy = 0.0;
            if (!input_popup_hit(self->p0_, x, y, res, surface, sx, sy))
                return false;
            out = {};
            out.surface_resource = res;
            out.surface = surface;
            out.root = surface;
            out.sx = sx;
            out.sy = sy;
            out.owner = hit_owner::ime_popup;
            return true;
        };
        wm_.set_ime_ops(ops);
    }
    p0_.lock = &lock_;
    p0_.lock_layer = &lock_layer_;
    p0_.lock_user = this;
    p0_.lock_apply = [](void* u, const lock_actions& a) {
        static_cast<compositor_impl*>(u)->apply_lock_actions(a);
    };
    {
        wm_lock_ops ops{};
        ops.user = this;
        ops.hit = [](void* u, double x, double y, wm_hit& out) {
            auto* self = static_cast<compositor_impl*>(u);
            void* res = nullptr;
            lx::surface_id surface{};
            lx::surface_id root{};
            double sx = 0.0;
            double sy = 0.0;
            if (!session_lock_hit(self->p0_, x, y, res, surface, root, sx, sy))
                return false;
            out = {};
            out.surface_resource = res;
            out.surface = surface;
            out.root = root;
            out.sx = sx;
            out.sy = sy;
            out.owner = hit_owner::lock;
            return true;
        };
        ops.keyboard = [](void* u, lx::surface_id& surface) -> void* {
            auto* self = static_cast<compositor_impl*>(u);
            // Only once the lock layer is shown: before that, nothing has the keyboard.
            if (!self->lock_.scene_locked())
                return nullptr;
            surface = self->lock_.keyboard_surface();
            return self->lock_.keyboard_resource();
        };
        ops.clicked = [](void* u, lx::surface_id surface) {
            auto* self = static_cast<compositor_impl*>(u);
            self->apply_lock_actions(self->lock_.clicked(surface));
        };
        wm_.set_lock_ops(ops);
    }
    p0_.outputs = &outputs;
    // Only when frames are actually composited in software. The headless backend keeps a
    // full copy of every registered texture; handed it on a GPU path, the commit path
    // copied every shm buffer into a table nothing read and nothing pruned — the bulk of
    // the compositor's heap growth (measured with heaptrack: 60 MB over a 30-resize run).
    p0_.headless = headless_present_ ? &headless_ : nullptr;
    p0_.dmabuf = &graphics.dmabuf();
    p0_.textures = &texture_updates_;
    p0_.request_logout = [](void* self, const char* source) {
        static_cast<compositor_impl*>(self)->begin_logout(source);
    };
    p0_.logout_user = this;
    p0_.keys = &keys_;
    keys_.set_layout_query(
        [](unsigned keysym, void* u) {
            auto& km = static_cast<compositor_impl*>(u)->input_devices_.default_seat().keymap();
            return km.layout_has_keysym(keysym, km.active_layout());
        },
        this);
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
        // Where the driver takes BGRA rows at any stride, the client's buffer is uploaded
        // as it is: no staging copy and no swizzle on the UI thread. What is still staged
        // (a full table, compositor images) keeps the same channel order, so a texture
        // never changes format between uploads.
        if (gl_compositor_.supports_client_upload()) {
            p0_.shm_direct = &shm_direct_;
            p0_.shm_native_format = true;
            lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx",
                                            "shm: uploaded from client memory (no staging)");
        }
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

void compositor_impl::begin_logout(const char* source) {
    if (logout_active_)
        return;
    logout_active_ = true;
    const auto now = static_cast<lx::runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    logout_deadline_ns_ = now + k_logout_grace_ns;
    // The shell learns first, so it reads the disconnect that follows as a normal end.
    if (p0_.notify_session_ending)
        p0_.notify_session_ending();
    // Every window, mapped or not, gets xdg_toplevel.close: a client decides how to exit
    // (an editor may save), and nothing is killed. Clients still connected at the
    // deadline are disconnected by the compositor's own teardown.
    unsigned asked = 0;
    toplevels.for_each([&](const toplevel_record& rec) {
        wm_.request_close(rec.id);
        ++asked;
    });
    char line[128];
    std::snprintf(line, sizeof(line), "logout requested by %s: closing %u window%s", source,
                  asked, asked == 1 ? "" : "s");
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.session", line);
}

void compositor_impl::advance_logout(lx::runtime::clock_time now_ns) {
    if (!logout_active_)
        return;
    const unsigned left = toplevels.count();
    if (left > 0 && now_ns < logout_deadline_ns_)
        return;
    char line[128];
    std::snprintf(line, sizeof(line), "logout: stopping (%u window%s still open)", left,
                  left == 1 ? "" : "s");
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.session", line);
    logout_active_ = false;
    loop.quit(0);
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
    lx::trace::scoped_span span{"render", "texture_update"};
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
            const bool bgra =
                update.format == static_cast<lx::fourcc>(lx::pixel_format::argb8888) ||
                update.format == static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
            const bool opaque =
                update.format == static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
            if (update.shm_direct) {
                // The client's own rows, read under its buffer's access guard.
                shm_direct_.upload(update.shm_direct,
                                   [&](const unsigned char* pixels, unsigned stride) {
                                       applied = pixels ? gl_compositor_.upload_client_region(
                                                              update.texture_id, update.width,
                                                              update.height, update.region,
                                                              pixels, stride, opaque)
                                                        : lx::result<void>{};
                                   });
            } else if (bgra) {
                applied = gl_compositor_.upload_client_region(
                    update.texture_id, update.width, update.height, update.region,
                    update.pixels, update.stride, opaque);
            } else {
                applied = gl_compositor_.upload_rgba_region(update.texture_id, update.width,
                                                            update.height, update.region,
                                                            update.pixels);
            }
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
    // A flip of a commit made before the pause, drained after it: the slot it showed is
    // noted, its timing dropped. It must not restart the frame clock or pace callbacks.
    if (impl->display_gate_.load(std::memory_order_relaxed) != 0) {
        impl->committed_submit_us_.store(0, std::memory_order_relaxed);
        (void)impl->committed_input_us_.exchange(0, std::memory_order_acq_rel);
        if (const int fd = impl->committed_fence_fd_.exchange(-1, std::memory_order_acq_rel);
            fd >= 0)
            ::close(fd);
        // Its presentation feedback was discarded at the pause edge (nothing presented
        // while the session is away — wlroots reports such flips `presented = false`).
        if (fb.vsync)
            impl->on_scanout_flip(impl->pending_flip_slot_.load(std::memory_order_acquire));
        // A power-off waiting for this flip (the CRTC takes one commit at a time) goes now.
        if (impl->power_.waiting_for_flip() && !impl->kms_atomic.flip_pending())
            impl->apply_power_actions(impl->power_.on_flip_drained());
        return;
    }
    // The flip landed, so clients may draw again regardless of how complete the feedback is
    // — unless it only moved the pointer: no client content was presented by it.
    if (impl->last_commit_content_.load(std::memory_order_acquire))
        impl->present_completed_ = true;
    impl->awaiting_resume_flip_ = false;

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
    lx::trace::timeline_instant("kms", "flip");
    if (fb.hw_clock)
        impl->note_present_latency(fb.actual_present_ns, submit_us, fence_us);

    if (!fb.vsync)
        return;
    const unsigned slot = impl->pending_flip_slot_.load(std::memory_order_acquire);
    impl->on_scanout_flip(slot);

    // wp_presentation: the composite of frame `tag` is on screen. A kernel flip event is
    // vsync'd and hardware-signalled; its time is the hardware's (hw_clock) when the driver
    // timestamps vblanks, with the CRTC's vblank counter as MSC (wlroots' handle_page_flip).
    // A driver without vblank support samples the time when it sends the event and has no
    // counter (seq 0, as the spec asks): no hw_clock. A blocking modeset's completion is
    // timed here, in software: vsync only.
    if (fb.content && fb.tag != 0) {
        present_info info{};
        info.timestamp_ns = static_cast<unsigned long long>(fb.actual_present_ns);
        info.seq = fb.vblank_clock ? fb.seq : 0;
        info.flags = k_present_vsync;
        if (fb.hw_clock)
            info.flags |= k_present_hw_completion;
        if (fb.vblank_clock)
            info.flags |= k_present_hw_clock;
        if (impl->outputs.count() > 0) {
            const unsigned mhz = impl->outputs.state(impl->outputs.nth(0)).refresh_mhz;
            if (mhz > 0)
                info.refresh_ns = static_cast<unsigned>(
                    (1'000'000'000'000ull + mhz / 2) / mhz);
        }
        presentation_flipped(impl->p0_, fb.tag, info);
        // ext-session-lock (L1): a locked frame is on the output — `locked` may go out.
        if (impl->lock_.state() == lock_state::locking &&
            static_cast<int>(fb.tag - impl->lock_frame_) >= 0)
            impl->apply_lock_actions(impl->lock_.locked_frame_shown());
    }
}

// ── Session pause/resume (VT switching) ────────────────────────────────────

void compositor_impl::record_session_event(const session_event& ev) {
    session_events_.push_back(ev);
}

namespace {
const char* pause_kind_name(session_pause_kind k) {
    switch (k) {
    case session_pause_kind::pause: return "pause";
    case session_pause_kind::force: return "force";
    case session_pause_kind::gone: return "gone";
    }
    return "?";
}
} // namespace

void compositor_impl::on_session_events() {
    const std::size_t n = session_events_.size();
    const bool debug = lx::trace::logger::global().min_level() <= lx::trace::level::debug;
    for (std::size_t i = 0; i < n; ++i) {
        const session_event ev = session_events_[i];
        char line[128];
        switch (ev.what) {
        case session_event::kind::paused:
            if (debug) {
                std::snprintf(line, sizeof(line), "PauseDevice %u:%u type=%s", ev.major, ev.minor,
                              pause_kind_name(ev.pause));
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.session",
                                                line);
            }
            // evdev `pause`s (seats without VTs) are acknowledged after the batch, below.
            if (session_.is_drm(ev.major, ev.minor))
                apply_session_actions(session_.on_device_paused(ev.major, ev.minor, ev.pause));
            break;
        case session_event::kind::resumed:
            if (debug) {
                std::snprintf(line, sizeof(line), "ResumeDevice %u:%u", ev.major, ev.minor);
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.session",
                                                line);
            }
            // evdev: we released every input device at the pause, so this is a leftover of
            // a race; the fd is sd-bus's and libinput re-takes its devices itself.
            if (session_.is_drm(ev.major, ev.minor))
                apply_session_actions(session_.on_device_resumed(ev.major, ev.minor));
            break;
        case session_event::kind::active:
            if (debug) {
                std::snprintf(line, sizeof(line), "Active=%d", ev.active ? 1 : 0);
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.session",
                                                line);
            }
            apply_session_actions(
                session_.on_active_changed(ev.active, ev.active && kms.is_master()));
            break;
        case session_event::kind::vt_failed:
            std::snprintf(line, sizeof(line), "VT switch failed: %s", ev.error);
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.session", line);
            break;
        case session_event::kind::lock:
            on_logind_lock();
            break;
        case session_event::kind::unlock:
            on_logind_unlock();
            break;
        case session_event::kind::sleep:
            on_prepare_for_sleep(ev.active);
            break;
        }
    }
    // evdev `pause`s (seats without VTs): every one acknowledged, if still held — a DRM
    // pause in the same batch suspended libinput first, which released them all. logind
    // completes the switch only once no device of the session is active.
    for (std::size_t i = 0; i < n; ++i) {
        const session_event& ev = session_events_[i];
        if (ev.what == session_event::kind::paused && ev.pause == session_pause_kind::pause &&
            !session_.is_drm(ev.major, ev.minor) && logind_.holds_device(ev.major, ev.minor))
            logind_.complete_pause(ev.major, ev.minor);
    }
    // Only this batch: a signal recorded meanwhile (none today — the handlers above make
    // synchronous calls at most, which queue but do not dispatch) waits for the next one.
    session_events_.erase(session_events_.begin(),
                          session_events_.begin() + static_cast<std::ptrdiff_t>(n));
}

void compositor_impl::apply_session_actions(const session_actions& a) {
    if (a.empty())
        return;
    char line[128];
    if (a.has(session_actions::stop_display))
        enter_display_pause(a.quiesce_gen);
    if (a.has(session_actions::suspend_input))
        suspend_input_for_pause();
    if (a.has(session_actions::stop_display))
        wm_.set_suspend_reason(window_manager::suspend_vt, true);
    if (a.has(session_actions::log_paused)) {
        if (a.by_active)
            std::snprintf(line, sizeof(line), "session paused (active=false)");
        else
            std::snprintf(line, sizeof(line), "session paused (drm=%u:%u kind=%s)", drm_major_,
                          drm_minor_, pause_kind_name(a.kind));
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.session", line);
    }
    if (a.has(session_actions::log_display_lost))
        lx::trace::logger::global().log(lx::trace::level::error, "compositor.session",
                                        "DRM device removed — no display");
    if (a.has(session_actions::log_resume_rejected))
        lx::trace::logger::global().log(
            lx::trace::level::error, "compositor.session",
            "resume modeset rejected (the mode is no longer offered?) — the display stays off "
            "until the session is switched to again");
    if (a.has(session_actions::begin_resume))
        begin_display_resume(a.resume_gen);
    if (a.has(session_actions::resume_input))
        resume_input_after_pause();
    if (a.has(session_actions::complete_pause)) {
        logind_.complete_pause(a.ack_major, a.ack_minor);
        std::snprintf(line, sizeof(line), "PauseDeviceComplete %u:%u", a.ack_major, a.ack_minor);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.session", line);
    }
    if (a.has(session_actions::log_resumed))
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.session",
                                        "session resumed");
    // A lock under way when the VT went away is locked now (nothing of it is visible).
    advance_lock();
    wayland.flush();
}

void compositor_impl::enter_display_pause(unsigned quiesce_gen) {
    // 1. Gate the render thread: from its next check on, nothing is composited or
    //    committed. A commit already past the check fails with EACCES (logind dropped
    //    master before signalling), which render then classifies as paused.
    display_gate_.fetch_or(gate_vt, std::memory_order_release);
    // 2. Render runs this after its current tick; the reply times a `pause` acknowledgement.
    auto& ex = runtime::executor::global();
    if (threads_started.load(std::memory_order_acquire)) {
        compositor_impl* self = this;
        (void)ex.post(runtime::affinity::render, [self, quiesce_gen] {
            compositor_impl* s = self;
            const unsigned g = quiesce_gen;
            (void)runtime::executor::global().post(runtime::affinity::ui, [s, g] {
                s->apply_session_actions(s->session_.on_render_quiesced(g));
            });
        });
    } else {
        apply_session_actions(session_.on_render_quiesced(quiesce_gen));
    }
    // 3. The frame clock stops; the housekeeping timer becomes the 1 Hz paused tick. Nothing
    //    in flight will be presented: its feedback is discarded now (the flips still to
    //    drain are dropped by the paused branch of the flip handler).
    presentation_discard(p0_, false);
    if (vsync_timer)
        vsync_timer->set_enabled(false);
    if (housekeeping_timer)
        housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
    // 4. End what a release must not complete: a move/resize ends as Escape would, a drag
    //    is cancelled. A half-typed tap is no tap.
    wm_.cancel_for_pause();
    keys_.reset();
}

void compositor_impl::suspend_input_for_pause() {
    auto& seat = input_devices_.default_seat();
    // 5. libinput closes every device (back to logind with ReleaseDevice) and emits a
    //    release for every key and button still down; those were drained through the
    //    normal path just now, so clients saw them in order.
    input_devices_.suspend();
    // Whatever libinput did not cover (injected input, a device that vanished uncleanly):
    // released here, so no client is left with a stuck key or button.
    const auto time_ms = static_cast<unsigned>(now_ns() / 1'000'000ull);
    input_router_.release_forwarded_keys(time_ms, seat);
    wm_.release_held_buttons(time_ms);
    if (seat.keymap().reset_depressed())
        input_router_.send_modifiers(seat.keymap().modifiers(), seat);
    seat.clear_pressed();
    // 6. The focused client is told it lost the keyboard; the window manager's decision is
    //    kept for the resume.
    input_router_.suspend_keyboard_focus(seat);
}

void compositor_impl::resume_input_after_pause() {
    auto& seat = input_devices_.default_seat();
    if (input_devices_.suspended()) {
        // Re-enumerates the seat and re-takes every device through logind; a device that
        // fails stays closed (libinput logs it).
        if (auto resumed = input_devices_.resume(); !resumed)
            lx::trace::logger::global().log_error(resumed.get_error(), "compositor.input");
        // The DEVICE_ADDED events are queued now; deliver them (and the LED updates).
        (void)input_devices_.dispatch();
    }
    // The window manager decides again first (a session lock may have started while away):
    // the surface the resume gives the keys to is its decision now, never the one from
    // before the switch.
    wm_.resolve_keyboard_focus();
    // `enter` with an empty key array and the modifiers (only locks survive): a full resync.
    input_router_.resume_keyboard_focus(seat);
}

void compositor_impl::begin_display_resume(unsigned gen) {
    // A flip completed before the pause is consumed under the old generation first, which
    // keeps the order obvious in the log (the cookies make it safe either way).
    if (drm_present_)
        kms_atomic.dispatch_events();
    // The VT coming back is activity (I2), and it brings a dark output back on: this
    // resume's own modeset turns the CRTC on, so the power gate just goes.
    apply_power_actions(power_.on_vt_resumed());
    const unsigned render_gen = ++render_gen_counter_;
    vt_resume_render_gen_ = render_gen;
    vt_resume_session_gen_ = gen;
    resume_gen_.store(render_gen, std::memory_order_release);
    display_gate_.fetch_and(~gate_vt, std::memory_order_release);
    resume_input_after_pause();
    wm_.set_suspend_reason(window_manager::suspend_vt, false);
    if (idle_.note_activity(now_ns(), idle_events(p0_)))
        rearm_idle_timer();
    // Another master drew on the CRTC: every pixel is repainted.
    scene.note_full_damage();
    if (housekeeping_timer)
        housekeeping_timer->set_enabled(false);
    last_callback_ns_ = 0;
    present_completed_ = false;
    awaiting_resume_flip_ = true;
    request_frame();
}

// ── Idle, inhibition and display power ─────────────────────────────────────

void compositor_impl::idle_timer_callback() {
    if (!active)
        return;
    active->idle_.on_timer(static_cast<unsigned long long>(now_ns()), idle_events(active->p0_));
    active->rearm_idle_timer();
}

void compositor_impl::rearm_idle_timer() {
    if (!idle_timer_)
        return;
    const unsigned long long deadline = idle_.next_deadline();
    if (deadline == 0)
        idle_timer_->set_enabled(false);
    else
        idle_timer_->schedule_at(static_cast<lx::runtime::clock_time>(deadline));
}

bool compositor_impl::on_user_activity() {
    // Per input event: one clock read, one store, and a branch on the idle count.
    if (idle_.note_activity(static_cast<unsigned long long>(now_ns()), idle_events(p0_)))
        rearm_idle_timer();
    if (!power_.wants_wake())
        return false;
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.power",
                                    "display woken by input");
    apply_power_actions(
        power_.wake((display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0));
    return true;
}

void compositor_impl::request_output_power(bool on) {
    const bool vt_away = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
    apply_power_actions(on ? power_.request_on(vt_away) : power_.request_off(vt_away));
}

void compositor_impl::apply_power_actions(const power_actions& a) {
    if (a.empty())
        return;
    if (a.has(power_actions::gate)) {
        // The display stops as at a VT pause (A.3 step 3), but input stays live: it is what
        // wakes the output. Keyboard focus is kept.
        power_gen_.store(power_.generation(), std::memory_order_release);
        display_gate_.fetch_or(gate_power, std::memory_order_release);
        presentation_discard(p0_, false);
        if (vsync_timer)
            vsync_timer->set_enabled(false);
        if (housekeeping_timer)
            housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
        awaiting_resume_flip_ = false;
        wm_.set_suspend_reason(window_manager::suspend_power, true);
        lx::trace::timeline_instant("ui", "power.gate");
    }
    if (a.has(power_actions::post_off))
        post_power_off(a.gen);
    if (a.has(power_actions::ungate)) {
        // Whatever off task is still in flight is stale from here on.
        power_gen_.store(power_.generation(), std::memory_order_release);
        wm_.set_suspend_reason(window_manager::suspend_power, false);
        if (a.resume) {
            // The CRTC is down: the resume procedure (full repaint, a full modeset with
            // ACTIVE=1) brings it back. The generation is stored before the gate opens.
            const unsigned render_gen = ++render_gen_counter_;
            power_resume_render_gen_ = render_gen;
            resume_gen_.store(render_gen, std::memory_order_release);
        }
        display_gate_.fetch_and(~gate_power, std::memory_order_release);
        if ((display_gate_.load(std::memory_order_relaxed) & gate_vt) == 0) {
            scene.note_full_damage();
            if (housekeeping_timer)
                housekeeping_timer->set_enabled(false);
            last_callback_ns_ = 0;
            present_completed_ = false;
            awaiting_resume_flip_ = a.resume;
            request_frame();
        }
        lx::trace::timeline_instant("ui", "power.ungate");
    }
    if (a.has(power_actions::wait_flip) && !kms_atomic.flip_pending())
        apply_power_actions(power_.on_flip_drained());
    if (a.has(power_actions::send_mode) && outputs.count() > 0) {
        const int slot = outputs.slot_of(outputs.nth(0));
        if (slot >= 0)
            notify_output_power(p0_, static_cast<unsigned>(slot), a.mode_on);
    }
    if (a.has(power_actions::log_off))
        lx::trace::logger::global().log(
            lx::trace::level::info, "compositor.power",
            (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0
                ? "display power off (session away: nothing to commit)"
                : "display power off (CRTC ACTIVE=0, cursor plane off)");
    if (a.has(power_actions::log_soft_blank))
        lx::trace::logger::global().log(
            lx::trace::level::warn, "compositor.power",
            "display power off refused by the driver — showing black instead (no power saving)");
    if (a.has(power_actions::log_on))
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.power",
                                        "display power on");
    if (a.has(power_actions::log_on_failed))
        lx::trace::logger::global().log(
            lx::trace::level::error, "compositor.power",
            "display power on: the modeset was refused (the mode is no longer offered?) — the "
            "output stays dark until the next input");
    // A lock under way when the output went dark is locked now.
    advance_lock();
    wayland.flush();
}

void compositor_impl::post_power_off(unsigned gen) {
    power_gen_.store(gen, std::memory_order_release);
    if (!threads_started.load(std::memory_order_acquire) || !drm_present_) {
        apply_power_actions(power_.on_off_result(gen, power_off_outcome::deferred));
        return;
    }
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::render, [self, gen] {
        const power_off_outcome outcome = self->run_power_off(gen);
        (void)runtime::executor::global().post(runtime::affinity::ui, [self, gen, outcome] {
            self->apply_power_actions(self->power_.on_off_result(gen, outcome));
        });
    });
}

power_off_outcome compositor_impl::run_power_off(unsigned gen) {
    // Render thread, after a tick: the gate is up, so nothing else commits meanwhile.
    if (power_gen_.load(std::memory_order_acquire) != gen)
        return power_off_outcome::stale;
    if ((display_gate_.load(std::memory_order_acquire) & gate_vt) != 0)
        return power_off_outcome::deferred;
    if (kms_atomic.flip_pending())
        return power_off_outcome::retry_after_flip;
    // Nothing of ours is lit: never modeset yet, or the CRTC is still off from the previous
    // power-off (a resume whose modeset has not happened).
    if (!content_committed_once_)
        return power_off_outcome::deferred;
    lx::trace::scoped_span span{"render", "kms.power_off"};
    auto off = kms_atomic.commit_power_off(cursor_plane_.valid() ? &cursor_plane_ : nullptr);
    if (off) {
        // What the plane shows is unknown to it now: the power-on modeset states it in full.
        if (cursor_plane_.valid())
            cursor_plane_.invalidate();
        return power_off_outcome::done;
    }
    if (kms_atomic.last_failure_lost_master())
        return power_off_outcome::deferred; // a VT switch raced the request
    if (!power_rejected_logged_) {
        power_rejected_logged_ = true;
        lx::trace::logger::global().log_error(off.get_error(), "compositor.power");
    }
    commit_black_frame();
    return power_off_outcome::rejected;
}

void compositor_impl::commit_black_frame() {
    if (scanout_slot_count_ == 0 || kms_atomic.flip_pending())
        return;
    const unsigned slot = pick_back_slot();
    gfx::damage_region region{};
    region.set_full();
    region.clip(scanout_rect_);
    bool drawn = false;
    int fence = -1;
    const lx::color black = lx::color::rgb(0.f, 0.f, 0.f);
    if (gl_present_) {
        if (auto c = gl_compositor_.composite_region(gl_targets_[slot], black, nullptr, 0, region);
            c) {
            drawn = true;
            fence = c.value().out_fence_fd;
        }
    } else if (cpu_present_) {
        const auto& fb = dumb_fbs_[slot];
        gfx::pixel_surface target{};
        target.pixels = fb.pixels();
        target.width = fb.width();
        target.height = fb.height();
        target.stride = fb.stride();
        target.format = fb.format();
        target.write_combining = true;
        drawn = static_cast<bool>(
            cpu_compositor_.composite_region(target, black, nullptr, 0, region));
    } else if (vulkan_present_) {
        if (auto c = vk_compositor_.composite(vk_targets_[slot], black, nullptr, 0); c) {
            drawn = true;
            fence = c.value().out_fence_fd;
        }
    }
    if (!drawn)
        return;
    drm::atomic_commit_request req{};
    req.connector = 0;
    req.request_page_flip = true;
    req.in_fence_fd = fence;
    req.damage.full_frame = true;
    req.framebuffer_id = cpu_present_ ? dumb_fbs_[slot].id() : scanout_fbs_[slot].id();
    if (cursor_plane_.valid()) {
        cursor_plane_.set_visible(false);
        req.cursor = &cursor_plane_;
    }
    // Its flip lands while gated: it rotates the slot and answers nothing.
    pending_flip_slot_.store(slot, std::memory_order_release);
    last_commit_content_.store(false, std::memory_order_release);
    if (auto committed = kms_atomic.commit(req); committed && kms_atomic.completion_needs_dispatch())
        post_flip_dispatch();
    if (fence >= 0)
        ::close(fence);
    // Every slot is stale against what a power-on shows (the resume repaints it all anyway).
    for (unsigned i = 0; i < k_scanout_slots; ++i)
        slot_age_damage_[i].set_full();
    kms_pending_damage_.set_full();
}

// ── Session lock (ext-session-lock-v1) ──────────────────────────────────────

void compositor_impl::lock_timer_callback() {
    if (!active)
        return;
    const auto now = now_ns();
    active->apply_lock_actions(active->lock_.on_timer(static_cast<unsigned long long>(now)));
    if (active->sleep_lock_deadline_ns_ != 0 && now >= active->sleep_lock_deadline_ns_)
        active->release_sleep_delay("no lock within 2 s");
    active->rearm_lock_timer();
}

void compositor_impl::rearm_lock_timer() {
    if (!lock_timer_)
        return;
    lx::runtime::clock_time next = static_cast<lx::runtime::clock_time>(lock_.deadline());
    if (sleep_lock_deadline_ns_ != 0 && (next == 0 || sleep_lock_deadline_ns_ < next))
        next = sleep_lock_deadline_ns_;
    if (next == 0)
        lock_timer_->set_enabled(false);
    else
        lock_timer_->schedule_at(next);
}

void compositor_impl::release_sleep_delay(const char* why) {
    if (sleep_lock_deadline_ns_ == 0)
        return;
    sleep_lock_deadline_ns_ = 0;
    logind_.release_sleep_inhibitor();
    char line[96];
    std::snprintf(line, sizeof(line), "sleep may proceed (%s)", why);
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.lock", line);
}

bool compositor_impl::display_dark() const {
    const unsigned gate = display_gate_.load(std::memory_order_relaxed);
    if ((gate & gate_vt) != 0)
        return true; // another VT (or nothing) is on the CRTC
    // Powered off, or the black frame of a refused power-off — not while the off task is
    // still pending (the last frame is on screen until it runs).
    if (power_.phase() == power_phase::off || power_.phase() == power_phase::soft_blank)
        return true;
    // KMS, and nothing of ours ever reached the CRTC: the console is on screen.
    return drm_present_ && !ever_lit_.load(std::memory_order_acquire);
}

void compositor_impl::advance_lock() {
    if (lock_.state() == lock_state::locking && display_dark())
        apply_lock_actions(lock_.locked_frame_shown());
}

void compositor_impl::apply_lock_actions(const lock_actions& a) {
    if (a.empty())
        return;
    const lx::color black = lx::color::rgb(0.f, 0.f, 0.f);
    if (a.has(lock_actions::input_lock)) {
        // L1: input leaves normal clients at the request, before anything is hidden. The
        // lock layer stays hidden until the scene locks (lock surfaces are not shown before).
        lock_layer_.set_visible(false);
        keys_.set_locked(true);
        wm_.set_session_locked(true);
    }
    if (a.has(lock_actions::scene_lock)) {
        for (scene::container_node* layer :
             {&background_layer_, &bottom_layer_, &windows_layer_, &top_layer_,
              &overlay_shell_layer_, &popup_layer_, &dnd_icon_layer_})
            layer->set_visible(false);
        lock_layer_.set_visible(true);
        // The first snapshot built from here on is a locked frame.
        lock_frame_ = p0_.building_frame;
        wm_.set_suspend_reason(window_manager::suspend_lock, true);
        scene.note_full_damage();
        request_frame();
    }
    // The wire half: finished, locked, and lock surfaces dropped when the lock ended.
    session_lock_send(p0_, a);
    if (a.has(lock_actions::scene_unlock)) {
        for (scene::container_node* layer :
             {&background_layer_, &bottom_layer_, &windows_layer_, &top_layer_,
              &overlay_shell_layer_, &popup_layer_, &dnd_icon_layer_})
            layer->set_visible(true);
        lock_layer_.set_visible(false);
        wm_.set_suspend_reason(window_manager::suspend_lock, false);
        scene.note_full_damage();
        request_frame();
    }
    if (a.has(lock_actions::input_unlock)) {
        keys_.set_locked(false);
        wm_.set_session_locked(false);
    }
    if (a.has(lock_actions::refocus))
        wm_.lock_focus_changed();
    // The solid colour: dark red once the locker is gone (sway), so whoever sits down sees
    // why the screen is locked and empty; black otherwise.
    const lx::color clear = lock_.state() == lock_state::abandoned
                                ? lx::color::rgb(0x3f / 255.f, 0.f, 0.f)
                                : black;
    const lx::color old = scene.clear_color();
    if (old.r != clear.r || old.g != clear.g || old.b != clear.b) {
        scene.set_clear_color(clear);
        scene.note_full_damage();
        request_frame();
    }
    if (a.has(lock_actions::locked_hint) && logind_.has_control())
        logind_.set_locked_hint(a.hint);
    if (a.has(lock_actions::notify_shell) && p0_.notify_lock_state)
        p0_.notify_lock_state(shell_lock_state(lock_.state()));
    if (a.has(lock_actions::activity)) {
        // An unlock is the user (I2) — but not a wake: unlocking while blanked stays dark
        // until input (the unlock itself was input, which already woke it).
        if (idle_.note_activity(static_cast<unsigned long long>(now_ns()), idle_events(p0_)))
            rearm_idle_timer();
    }
    if (a.has(lock_actions::rearm_timer))
        rearm_lock_timer();
    if (lock_.state() != logged_lock_state_) {
        const lock_state from = logged_lock_state_;
        logged_lock_state_ = lock_.state();
        char line[128];
        std::snprintf(line, sizeof(line), "session lock: %s -> %s", lock_state_name(from),
                      lock_state_name(logged_lock_state_));
        lx::trace::logger::global().log(logged_lock_state_ == lock_state::abandoned
                                            ? lx::trace::level::warn
                                            : lx::trace::level::info,
                                        "compositor.lock", line);
        if (logged_lock_state_ == lock_state::abandoned)
            lx::trace::logger::global().log(
                lx::trace::level::warn, "compositor.lock",
                "session lock abandoned (the locker exited) — staying locked until a new "
                "locker takes over or logind unlocks");
    }
    // `locked` is what before-sleep locking waits for.
    if ((lock_.state() == lock_state::locked || lock_.state() == lock_state::abandoned ||
         lock_.state() == lock_state::unlocked) &&
        sleep_lock_deadline_ns_ != 0)
        release_sleep_delay(lock_.state() == lock_state::unlocked ? "lock ended" : "locked");
    // A lock reaching `locking` while nothing of the session is visible is locked at once.
    advance_lock();
    wayland.flush();
}

void compositor_impl::on_logind_lock() {
    // loginctl lock-session, a lid policy: the shell starts its locker (Lumen ships none).
    if (lock_.state() != lock_state::unlocked) {
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.lock",
                                        "logind Lock: already locked");
        return;
    }
    const bool told = p0_.notify_lock_requested && p0_.notify_lock_requested(0);
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.lock",
                                    told ? "logind Lock: shell asked to lock"
                                         : "logind Lock: no shell to start a locker — ignored");
}

void compositor_impl::on_logind_unlock() {
    // loginctl unlock-session (polkit-gated by logind): the secure way back from a lock whose
    // locker is gone (L2). A live locker is sent `finished`.
    if (lock_.state() == lock_state::unlocked)
        return;
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.lock",
                                    "logind Unlock: unlocking the session");
    apply_lock_actions(lock_.force_unlock());
}

void compositor_impl::on_prepare_for_sleep(bool start) {
    if (!start) {
        // Resumed from sleep: the user is back (I2), and the next sleep gets its delay again.
        (void)on_user_activity();
        if (logind_.has_control())
            logind_.take_sleep_inhibitor();
        return;
    }
    if (lock_.state() == lock_state::locked || lock_.state() == lock_state::abandoned) {
        release_sleep_delay("already locked");
        logind_.release_sleep_inhibitor();
        return;
    }
    bool wait = lock_.state() != lock_state::unlocked; // a lock is under way: wait for it
    if (!wait)
        wait = p0_.notify_lock_requested && p0_.notify_lock_requested(1);
    if (!wait || !logind_.holds_sleep_inhibitor()) {
        logind_.release_sleep_inhibitor();
        return;
    }
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.lock",
                                    "system going to sleep: holding it until the session is "
                                    "locked (at most 2 s)");
    sleep_lock_deadline_ns_ = now_ns() + k_sleep_lock_wait_ns;
    rearm_lock_timer();
}

void compositor_impl::run_resume_procedure(unsigned gen) {
    discard_composited_fence();
    composited_slot_ = -1;
    frame_outstanding_.store(false, std::memory_order_release);
    kms_atomic.prepare_resume();
    if (cursor_plane_.valid())
        cursor_plane_.invalidate();
    content_committed_once_ = false;
    for (unsigned slot = 0; slot < k_scanout_slots; ++slot)
        slot_age_damage_[slot].set_full();
    kms_pending_damage_.set_full();
    missed_damage_.set_full();
    resume_done_gen_ = gen;
    resume_result_pending_ = true;
}

void compositor_impl::post_resume_result(unsigned gen, session_resume_outcome outcome) {
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::ui, [self, gen, outcome] {
        // `gen` is the render generation: it answers the VT resume, a power-on, or both
        // (whichever asked for it last).
        if (gen == self->vt_resume_render_gen_)
            self->apply_session_actions(
                self->session_.on_resume_result(self->vt_resume_session_gen_, outcome));
        if (gen == self->power_resume_render_gen_)
            self->apply_power_actions(self->power_.on_resume_result(
                outcome == session_resume_outcome::ok ? power_on_outcome::ok
                : outcome == session_resume_outcome::rejected
                    ? power_on_outcome::rejected
                    : power_on_outcome::device_unavailable));
    });
}

void compositor_impl::post_flip_dispatch() {
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::ui,
                                           [self] { self->kms_atomic.dispatch_events(); });
}

void compositor_impl::start_threads() {
    if (threads_started.exchange(true)) return;

    running.store(true, std::memory_order_release);
    last_vsync_time = std::chrono::steady_clock::now();

    // Work posted to the render or worker strand wakes that thread. Neither polls, and
    // the render thread otherwise runs only when a tick hands it a snapshot.
    auto& ex = runtime::executor::global();
    ex.render_strand().set_waker(
        [](void* self) {
            auto* impl = static_cast<compositor_impl*>(self);
            {
                std::lock_guard lock{impl->render_mutex};
                impl->render_pending = true;
            }
            impl->render_cv.notify_one();
        },
        this);
    ex.worker_strand().set_waker(
        [](void* self) {
            auto* impl = static_cast<compositor_impl*>(self);
            {
                std::lock_guard lock{impl->worker_mutex};
                impl->worker_wake_ = true;
            }
            impl->worker_cv.notify_one();
        },
        this);

    render_thread = std::thread([this] { render_loop(); });

    if (cfg.worker_thread_count > 0)
        worker_thread = std::thread([this] { worker_loop(); });
}

void compositor_impl::stop_threads() {
    if (!threads_started.exchange(false)) return;

    running.store(false, std::memory_order_release);
    {
        // Under each mutex: a waiter between its predicate check and its wait would
        // otherwise miss the notify and sleep forever, now that nothing times out.
        std::lock_guard lock{render_mutex};
    }
    render_cv.notify_all();
    {
        std::lock_guard lock{worker_mutex};
    }
    worker_cv.notify_all();

    if (render_thread.joinable()) render_thread.join();
    if (worker_thread.joinable()) worker_thread.join();
    runtime::executor::global().render_strand().set_waker(nullptr, nullptr);
    runtime::executor::global().worker_strand().set_waker(nullptr, nullptr);
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
    settle_frame_clock();
}

void compositor_impl::render_loop() {
    scheduler::frame_scheduler::affinity_scope scope{scheduler::thread_affinity::render};
    lx::trace::name_thread("lx-render");

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
    lx::trace::name_thread("lx-worker");

    while (running.load(std::memory_order_acquire)) {
        runtime::executor::global().drain(runtime::affinity::worker);
        // Woken by a post (the strand's waker), not on a period: an idle worker sleeps.
        std::unique_lock lock{worker_mutex};
        worker_cv.wait(lock, [this] {
            return worker_wake_ || !running.load(std::memory_order_acquire);
        });
        worker_wake_ = false;
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
    // LUMEN_TRACE_FILE=path records a Perfetto-readable timeline of this run.
    lx::trace::timeline_start_from_env();
    lx::trace::name_thread("lx-ui");
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
        bool inactive = false;
        if (auto taken = impl_->logind_.take_device_path("/dev/dri/card0", &inactive); taken) {
            impl_->drm_via_logind_ = impl_->logind_.has_control();
            impl_->drm_start_inactive_ = impl_->drm_via_logind_ && inactive;
            opened = drm::kms_device::open_fd(std::move(taken).value());
        } else {
            opened = drm::kms_device::open("/dev/dri/card0");
        }
        if (opened) {
            impl_->kms = std::move(opened).value();
            impl_->graphics.syncobj().set_drm_fd(impl_->kms.card_fd());
            impl_->graphics.syncobj().set_device(impl_->graphics.vk_device());
            // Presenting needs DRM master, not merely an open card. Claiming the
            // page-flip path without it produces a commit failure on every frame — unless
            // logind gave us the card for a session that is merely not in front yet: then
            // master arrives with ResumeDevice, and the session starts paused.
            impl_->drm_present_ = impl_->kms.card_fd() >= 0 &&
                                  (impl_->kms.is_master() || impl_->drm_start_inactive_);
            if (impl_->kms.card_fd() >= 0) {
                struct stat st {};
                if (::fstat(impl_->kms.card_fd(), &st) == 0 && S_ISCHR(st.st_mode)) {
                    impl_->drm_major_ = major(st.st_rdev);
                    impl_->drm_minor_ = minor(st.st_rdev);
                }
            }
            if (impl_->drm_present_ && impl_->drm_start_inactive_) {
                lx::trace::logger::global().log(lx::trace::level::info, "compositor.drm",
                                                "KMS device open, session inactive — scanout "
                                                "starts when the session is switched to");
            } else if (impl_->drm_present_) {
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

    // Every connector takes the configured scale (n/120) when it first appears.
    impl_->outputs.set_default_scale(impl_->cfg.output_scale);
    (void)impl_->hotplug_.start(impl_->outputs, impl_->kms);
    if (impl_->outputs.count() > 0) {
        const output_state o0 = impl_->outputs.state(impl_->outputs.nth(0));
        const lx::rect2i g = o0.mode_rect();
        const lx::rect2i l = o0.logical_rect();
        unsigned refresh_mhz = 0;
        if (auto mode = impl_->kms.active_mode(0); mode)
            refresh_mhz = mode.value().refresh_millihz;
        else // the virtual output (headless, nested)
            refresh_mhz = o0.refresh_mhz;
        char line[160];
        std::snprintf(line, sizeof(line),
                      "output 0 mode=%dx%d refresh_mhz=%u scale=%g (%ld/120) logical=%dx%d",
                      g.width, g.height, refresh_mhz, o0.scale, std::lround(o0.scale * 120.0),
                      l.width, l.height);
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
        // Themes are loaded at the output's pixel size: XCURSOR_SIZE (or 24) is a logical
        // size, so a 1.25 output shows 30 px images — sharp, not a 24 px image stretched.
        {
            unsigned base = 24;
            if (const char* env = std::getenv("XCURSOR_SIZE")) {
                const long v = std::strtol(env, nullptr, 10);
                if (v > 0 && v <= 256)
                    base = static_cast<unsigned>(v);
            }
            const double scale = impl->outputs.count() > 0
                                     ? impl->outputs.scale(impl->outputs.nth(0))
                                     : impl->outputs.default_scale();
            const long px = std::lround(static_cast<double>(base) * scale);
            impl->cursor_theme_.load(nullptr, px > 0 ? static_cast<unsigned>(px) : base);
        }
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
        const lx::rect2i g = impl_->outputs.logical_rect(impl_->outputs.nth(0));
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
    impl_->housekeeping_timer = std::make_unique<runtime::timer_source>(
        detail::compositor_impl::k_housekeeping_ns,
        &detail::compositor_impl::housekeeping_callback, true);
    impl_->housekeeping_timer->set_enabled(false); // only while the frame clock is stopped
    impl_->loop.add_source(impl_->housekeeping_timer.get());
    // ext-idle-notify: one timer for every notification, armed at the next deadline only.
    impl_->idle_timer_ = std::make_unique<runtime::timer_source>(
        0, &detail::compositor_impl::idle_timer_callback, false);
    impl_->idle_timer_->set_enabled(false);
    impl_->loop.add_source(impl_->idle_timer_.get());
    impl_->rearm_idle_timer();
    // ext-session-lock: the 1 s wait for lock surfaces and the 2 s sleep delay.
    impl_->lock_timer_ = std::make_unique<runtime::timer_source>(
        0, &detail::compositor_impl::lock_timer_callback, false);
    impl_->lock_timer_->set_enabled(false);
    impl_->loop.add_source(impl_->lock_timer_.get());

    // Every fd the UI thread serves is waited on, not polled: the loop sleeps until one is
    // readable or a timer is due.
    if (const int wfd = impl_->wayland.event_fd(); wfd >= 0) {
        impl_->wayland_fd_source = std::make_unique<runtime::fd_source>(
            wfd, &detail::compositor_impl::wayland_fd_callback);
        impl_->loop.add_source(impl_->wayland_fd_source.get());
    }
    // The card reports readable only while a DRM event (a flip) is queued, and
    // drmHandleEvent consumes it — so the flip handler runs the moment the flip lands.
    if (impl_->drm_present_ && impl_->kms.card_fd() >= 0) {
        impl_->drm_fd_source = std::make_unique<runtime::fd_source>(
            impl_->kms.card_fd(), &detail::compositor_impl::drm_fd_callback);
        impl_->loop.add_source(impl_->drm_fd_source.get());
    }
    if (impl_->logind_.bus_fd() >= 0) {
        // Signals are recorded by these callbacks and acted on after each dispatch. A VT
        // switch brings one PauseDevice/ResumeDevice per taken device plus Active.
        impl_->session_events_.reserve(64);
        lx::session::session_listener listener{};
        listener.user = impl_;
        listener.device_paused = [](unsigned maj, unsigned min, lx::session::device_pause k,
                                    void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::paused;
            ev.major = maj;
            ev.minor = min;
            ev.pause = k == lx::session::device_pause::pause  ? session_pause_kind::pause
                       : k == lx::session::device_pause::gone ? session_pause_kind::gone
                                                              : session_pause_kind::force;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.device_resumed = [](unsigned maj, unsigned min, int, void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::resumed;
            ev.major = maj;
            ev.minor = min;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.active_changed = [](bool on, void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::active;
            ev.active = on;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.vt_switch_failed = [](const char* name, void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::vt_failed;
            // D-Bus error names in the reply are freed with it: keep only the known ones.
            ev.error = std::strstr(name, "AccessDenied")    ? "access denied"
                       : std::strstr(name, "NotSupported")  ? "not supported"
                       : std::strstr(name, "InvalidArgs")   ? "invalid VT"
                       : std::strstr(name, "NoSuchSeat")    ? "no such seat"
                                                            : "logind error";
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.lock_requested = [](void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::lock;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.unlock_requested = [](void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::unlock;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        listener.prepare_for_sleep = [](bool start, void* u) {
            detail::compositor_impl::session_event ev{};
            ev.what = detail::compositor_impl::session_event::kind::sleep;
            ev.active = start;
            static_cast<detail::compositor_impl*>(u)->record_session_event(ev);
        };
        if (auto listening = impl_->logind_.set_listener(listener); !listening)
            lx::trace::logger::global().log_error(listening.get_error(), "compositor.session");
        // Before-sleep locking: sleep waits (at most 2 s of ours) for the lock a shell starts
        // on lock_requested(sleep). Only the session's display server holds it.
        if (impl_->logind_.has_control())
            impl_->logind_.take_sleep_inhibitor();
        impl_->logind_source = std::make_unique<detail::logind_bus_source>(
            impl_->logind_,
            [](void* u) { static_cast<detail::compositor_impl*>(u)->on_session_events(); },
            impl_);
        impl_->loop.add_source(impl_->logind_source.get());
    }
    impl_->loop.set_after_dispatch(&detail::compositor_impl::after_dispatch_callback);

    impl_->start_threads();

    impl_->memory_.set_pressure_handler(&detail::compositor_impl::on_memory_pressure, impl_);
    impl_->presentation_.bind_page_flip(impl_->kms_atomic);
    impl_->presentation_.set_handler(&detail::compositor_impl::presentation_flip_handler, impl_);

    // The pause/resume machine runs only where there is a display to lose. Starting while
    // the session is in the background pauses right away (input is held until the switch).
    {
        char line[160];
        std::snprintf(line, sizeof(line), "session %s vt=%u can_switch_vt=%d active=%d drm=%u:%u",
                      impl_->logind_.session_id()[0] ? impl_->logind_.session_id() : "-",
                      impl_->logind_.vt_number(), impl_->logind_.can_switch_vt() ? 1 : 0,
                      impl_->logind_.is_active() ? 1 : 0, impl_->drm_major_, impl_->drm_minor_);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.session", line);
        impl_->apply_session_actions(impl_->session_.init(
            impl_->drm_present_, impl_->drm_major_, impl_->drm_minor_, impl_->drm_start_inactive_));
    }

    lx::trace::logger::global().log(lx::trace::level::info, "compositor",
                                    "threads: ui(main)+render+worker; vsync timer armed");

    return {};
}

int compositor::run() {
    if (!impl_) return 1;

    runtime::set_current_affinity(runtime::affinity::ui);
    const int code = impl_->loop.run();
    impl_->stop_threads();
    // Every recording thread has stopped: the timeline is complete.
    (void)lx::trace::timeline_flush();
    // Whatever the last window collected; a short session still reports its numbers.
    impl_->log_latency_summary();
    detail::compositor_impl::active = nullptr;
    return code;
}

void compositor::request_stop() {
    if (!impl_) return;
    impl_->loop.quit(0);
}

void compositor::request_screenshot() {
    if (!impl_)
        return;
    impl_->screenshot_requested_.store(true, std::memory_order_release);
    impl_->loop.wake();
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

void compositor::inject_logind_unlock() {
    if (impl_)
        impl_->on_logind_unlock();
}

lock_state compositor::session_lock_state() const {
    return impl_ ? impl_->lock_.state() : lock_state::unlocked;
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
input_router& compositor::input() { return impl_->input_router_; }
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
    lx::trace::scoped_span span{"ui", "tick_ui"};

    const auto t0 = std::chrono::steady_clock::now();
    // Protocol handlers date texture retirements against this, so it has to be current
    // before any Wayland dispatch below. What is committed from here on belongs to this
    // tick's snapshot at the earliest.
    impl_->p0_.frame_index = tick.frame_index;
    impl_->p0_.building_frame = tick.frame_index;
    impl_->budget_.begin_frame(tick.frame_index);
    impl_->p0_.now_ns = static_cast<unsigned long long>(tick.timestamp_ns);
    impl_->advance_logout(tick.timestamp_ns);
    // Staging: free retired CPU-path storage the render thread has let go of, unmap GL
    // blocks idle past the threshold, and report what is left against the budget.
    impl_->shm_store_.collect();
    impl_->shm_pool_.collect(impl_->p0_.now_ns, detail::compositor_impl::k_staging_idle_ns);
    {
        const unsigned long long staged = impl_->shm_store_.bytes_held() +
                                          impl_->shm_pool_.bytes_mapped();
        lx::trace::timeline_counter("staging_bytes", static_cast<long long>(staged));
        const unsigned clamped =
            staged > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<unsigned>(staged);
        impl_->memory_.report_usage(lx::runtime::memory_pool::staging, clamped, 0);
    }
    impl_->memory_.tick(tick.frame_index);
    impl_->memory_.reset_frame_arenas(lx::runtime::memory_pool::ui);

    impl_->hotplug_.poll();

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
        const output_state o0 = impl_->outputs.state(impl_->outputs.nth(0));
        const lx::rect2i g = o0.logical_rect();
        const lx::rect2i& last = impl_->last_output_rect_;
        if (g.x != last.x || g.y != last.y || g.width != last.width || g.height != last.height ||
            o0.scale != impl_->last_output_scale_) {
            impl_->last_output_rect_ = g;
            impl_->last_output_scale_ = o0.scale;
            // Layout → output pixels, applied where draws and damage are made. Every pixel
            // moves with it.
            const scene::output_projection projection{{g.x, g.y}, o0.scale, o0.mode};
            scene().set_projection(projection);
            impl_->cursor_.set_projection(projection);
            impl_->decorations_.set_scale(o0.scale);
            scene().note_full_damage();
            // Layer surfaces are arranged against the output, and the usable area with them.
            impl_->wm_.on_output_changed();
        }
    }
    if (!impl_->initial_damage_noted_) {
        impl_->initial_damage_noted_ = true;
        scene().note_full_damage();
    }

    {
        // DRM flips and the logind bus have sources of their own and are handled the
        // moment they are readable; requests that arrived since the loop last looked are
        // taken now, so they make this frame.
        lx::trace::scoped_span dispatch_span{"ui", "dispatch"};
        (void)wayland().dispatch(0);
    }

    // wl_surface.frame means "the content you committed has been shown". Answering it once
    // per vsync tick instead makes clients render frames that are composited over and never
    // reach the display, so on the KMS path the callbacks wait for an actual flip. Without
    // a present path (headless, nested) the tick is the only clock there is.
    if (impl_->last_callback_ns_ == 0)
        impl_->last_callback_ns_ = tick.timestamp_ns;
    const bool stalled =
        tick.timestamp_ns - impl_->last_callback_ns_ >=
        (impl_->awaiting_resume_flip_ ? detail::compositor_impl::k_resume_flip_grace_ns
                                      : detail::compositor_impl::k_callback_stall_ns);
    // Paused: this is the 1 Hz paused tick, and every callback is answered by it — a
    // client blocked in eglSwapBuffers keeps running, and one that honours `suspended`
    // stops drawing on its own (niri's 995 ms throttle, for the same reason).
    const bool paused = impl_->display_gate_.load(std::memory_order_relaxed) != 0;
    if (paused) {
        impl_->awaiting_resume_flip_ = false;
        impl_->last_callback_ns_ = tick.timestamp_ns;
        fire_frame_callbacks(impl_->p0_, static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull));
        // Nothing reaches the screen while stopped: queued presentation feedback is
        // discarded once a second, so a client pacing by it (mpv) falls back to its timer.
        presentation_discard(impl_->p0_, true);
        // A lock under way while nothing is shown is locked now.
        impl_->advance_lock();
    } else if (!impl_->drm_present_ || impl_->present_completed_ || stalled) {
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
        // Surfaces on screen in the last published frame now; the others (covered, off the
        // output, unmapped) at most once per 995 ms.
        fire_visible_frame_callbacks(impl_->p0_, frame_time_ms, impl_->last_published_frame_,
                                     static_cast<unsigned long long>(tick.timestamp_ns));
    }

    // Window management for this frame — configures coalesced since the last tick go out
    // here, once, rather than once per request that changed them.
    {
        lx::trace::scoped_span wm_span{"ui", "wm.on_tick"};
        impl_->wm_.on_tick(tick.timestamp_ns);
    }
    // Frames: the atlas on first use and changed titles, rasterized once per tick however
    // many set_title requests arrived — in this tick's snapshot, with the geometry they fit.
    impl_->decorations_.on_tick();
    // The pointer image, after the window manager (a grab sets the override) and before the
    // scene is published (the software cursor is a scene node).
    if (impl_->cursor_hw_failed_.load(std::memory_order_acquire))
        impl_->cursor_.on_hardware_failed();
    impl_->cursor_.on_tick();

    scheduler().on_vsync(tick);
    {
        lx::trace::scoped_span scene_span{"ui", "scene.update"};
        scene().update(tick.delta_seconds);
    }

    const bool nothing_changed = scene().pending_damage().empty();
    const bool published = scene().commit_frame(tick.frame_index);
    if (published) {
        // Published: commits dispatched between now and the next tick are the next
        // snapshot's. A dropped publish keeps this frame, whose damage carries forward.
        impl_->p0_.building_frame = tick.frame_index + 1;
        impl_->buffer_lifecycle_.on_frame_submitted(tick.frame_index);
        if (!nothing_changed)
            impl_->last_damaged_frame_ = tick.frame_index;
        impl_->last_published_frame_ = tick.frame_index;
        // wp_presentation: queued feedback of the surfaces this frame shows is sampled
        // into it (the visibility pass just ran). Not while stopped: nothing is shown.
        if (!paused) {
            presentation_published(impl_->p0_, tick.frame_index);
            if (!impl_->drm_present_) {
                // No display to flip (headless, nested): the tick is the only presentation
                // clock there is, so the frame counts as presented when it is published —
                // in software, so no flags (wlroots' headless backend does the same).
                present_info info{};
                info.timestamp_ns = static_cast<unsigned long long>(tick.timestamp_ns);
                if (impl_->outputs.count() > 0) {
                    const unsigned mhz =
                        impl_->outputs.state(impl_->outputs.nth(0)).refresh_mhz;
                    if (mhz > 0)
                        info.refresh_ns =
                            static_cast<unsigned>((1'000'000'000'000ull + mhz / 2) / mhz);
                }
                presentation_flipped(impl_->p0_, tick.frame_index, info);
                // Likewise a locked frame counts as shown once published.
                if (impl_->lock_.state() == lock_state::locking &&
                    static_cast<int>(tick.frame_index - impl_->lock_frame_) >= 0)
                    impl_->apply_lock_actions(impl_->lock_.locked_frame_shown());
            }
        }
    } else {
        // The render thread still holds every other slot. Visible on the timeline as an
        // instant, so a stutter can be told apart from a frame that had nothing new.
        lx::trace::timeline_instant("ui", "publish_dropped");
        lx::trace::logger::global().log(lx::trace::level::trace, "compositor",
                                        "frame dropped (backpressure)");
    }

    // Idle inhibitors (I3): honoured while their surface was on screen in the newest
    // published frame, on a running display. Only looked at while one exists, or while
    // inhibition still has to end — an empty set costs nothing.
    if (impl_->inhibitors_.count() > 0 || impl_->idle_.inhibited()) {
        const bool inhibited =
            !paused && idle_inhibitor_visible(impl_->p0_, impl_->last_published_frame_);
        if (impl_->idle_.set_inhibited(inhibited,
                                       static_cast<unsigned long long>(tick.timestamp_ns))) {
            impl_->rearm_idle_timer();
            if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.idle",
                                                inhibited ? "idle inhibited=1" : "idle inhibited=0");
        }
    }

    // An idle tick produces no flip, and frame callbacks otherwise wait for one — a client
    // that commits a frame request without new content would then stall until the
    // watchdog. Once every damaged frame has been composited and committed and no flip is
    // queued, nothing is waiting on the display: answer now (wlroots' "frame done without
    // a commit"). The reads are ordered against render's writes: last_rendered_frame_ is
    // published after frame_outstanding_ is raised, and frame_outstanding_ is lowered only
    // after the commit has set flip_pending.
    if (impl_->drm_present_ && !paused && nothing_changed &&
        impl_->last_rendered_frame_.load(std::memory_order_acquire) >= impl_->last_damaged_frame_ &&
        !impl_->frame_outstanding_.load(std::memory_order_acquire) &&
        !impl_->kms_atomic.flip_pending()) {
        impl_->last_callback_ns_ = tick.timestamp_ns;
        fire_visible_frame_callbacks(impl_->p0_,
                                     static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull),
                                     impl_->last_published_frame_,
                                     static_cast<unsigned long long>(tick.timestamp_ns));
    }

    // Release client buffers the render thread has finished sampling. Done here because
    // sending wl_buffer.release requires the UI thread's ownership of Wayland dispatch.
    const long long rendered = impl_->last_rendered_frame_.load(std::memory_order_acquire);
    (void)impl_->buffer_lifecycle_.retire_completed_frames(rendered);
    // Likewise shm buffers the render thread has uploaded from client memory.
    impl_->shm_direct_.retire();

    // Whether the next tick has anything to do. Each term is something only a later tick
    // finishes: damage to composite, a frame the render thread has not reached, a flip or
    // commit outstanding, a frame callback or buffer waiting on one, deferred WM work.
    impl_->tick_quiet_ =
        published && nothing_changed && rendered >= impl_->last_damaged_frame_ &&
        !impl_->frame_outstanding_.load(std::memory_order_acquire) &&
        !(impl_->drm_present_ && impl_->kms_atomic.flip_pending()) &&
        !frame_callbacks_pending(impl_->p0_) &&
        !impl_->buffer_lifecycle_.has_unreleased_frames() && impl_->shm_direct_.idle() &&
        !impl_->wm_.needs_tick() && !impl_->logout_active_; // logout ends on a tick

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
    lx::trace::scoped_span span{"render", "tick_render"};

    const auto t0 = std::chrono::steady_clock::now();

    // The pause gate, in this order (see `resume_gen_`). A resume runs its procedure before
    // anything else this tick: the KMS state, the cursor plane and every slot's history are
    // whatever the last master left.
    const bool paused = impl_->display_gate_.load(std::memory_order_acquire) != 0;
    const unsigned resume_gen = impl_->resume_gen_.load(std::memory_order_acquire);
    if (impl_->drm_present_ && !paused && resume_gen != impl_->resume_done_gen_)
        impl_->run_resume_procedure(resume_gen);

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
    if (lx::trace::timeline_enabled()) {
        const lx::rect2i bounds = snapshot.damage();
        lx::trace::timeline_counter("damage_px",
                                    static_cast<long long>(bounds.width) * bounds.height);
    }
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
            out[i].scaled = cmd.scaled;
            out[i].occluded = cmd.occluded;
        }
        return out;
    };

    {
        lx::trace::scoped_span drain_span{"render", "drain_textures"};
        impl_->drain_texture_updates(idx);
    }

    if (paused && impl_->drm_present_) {
        // Client buffers keep being applied and released (a client committing once a second
        // never runs out), so the scene is current at resume — but nothing is composited
        // or committed: master is gone. The damage is dropped; resume repaints everything.
        impl_->last_rendered_frame_.store(static_cast<long long>(idx), std::memory_order_release);
        impl_->apply_due_forgets(idx);
        scene().release_render_frame(snapshot);
        return;
    }

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
            lx::trace::scoped_span composite_span{"render", "composite"};
            auto composited = impl_->gl_compositor_.composite_region(
                impl_->gl_targets_[scanout_back], snapshot.clear_color(), blits, blit_count,
                region);
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
                // A requested debug screenshot of a fully repainted frame.
                if (impl_->screenshot_pending_.load(std::memory_order_relaxed) &&
                    region.count == 1 && region.rects[0].width == impl_->scanout_rect_.width &&
                    region.rects[0].height == impl_->scanout_rect_.height &&
                    impl_->screenshot_pending_.exchange(false, std::memory_order_acq_rel))
                    impl_->write_screenshot(scanout_back);
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
                target, snapshot.clear_color(), blits, blit_count, region);
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
                impl_->vk_targets_[scanout_back], snapshot.clear_color(), blits, blit_count);
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
    if (composited_this_tick) {
        impl_->composited_slot_ = static_cast<int>(scanout_back);
        impl_->composited_frame_ = idx;
    }

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
        // The flip event names the frame it shows (presentation feedback).
        req.user_tag = impl_->composited_frame_;
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

        // The pointer rides the frame: both change on the same vblank. The first commit
        // (a modeset) states the plane in full: on resume where the pointer is (the plane
        // passed its test before), at startup off — it joins once tested.
        if (impl_->cursor_plane_.valid() &&
            (impl_->content_committed_once_
                 ? impl_->cursor_plane_.dirty()
                 : (impl_->cursor_tested_ || !impl_->cursor_plane_.visible()))) {
            req.cursor = &impl_->cursor_plane_;
            cursor_rode = true;
        }

        impl_->pending_flip_slot_.store(present_slot, std::memory_order_release);
        impl_->last_commit_content_.store(true, std::memory_order_release);
        impl_->publish_commit_timing(impl_->composited_fence_fd_);
        lx::trace::scoped_span commit_span{"render", "kms.commit"};
        // Checked again right before the ioctl: a pause that landed during the composite
        // keeps the frame (resume discards it) and commits nothing.
        const bool paused_now = impl_->display_gate_.load(std::memory_order_acquire) != 0;
        const auto committed =
            paused_now ? lx::result<void>{lx::make_error(
                             lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                             "session paused")}
                       : impl_->kms_atomic.commit(req);
        // A commit that raced the pause is expected, not a failure of the present path:
        // logind drops master *before* it signals, so EACCES usually arrives first and the
        // pause a moment later. Master lost with no pause following is reported below.
        const bool lost_to_pause =
            !committed && (paused_now || impl_->display_gate_.load(std::memory_order_acquire) != 0 ||
                           impl_->kms_atomic.last_failure_lost_master());
        if (!committed && lost_to_pause && !paused_now) {
            const auto now = detail::compositor_impl::now_ns();
            if (impl_->master_lost_since_ns_ == 0)
                impl_->master_lost_since_ns_ = now;
            else if (now - impl_->master_lost_since_ns_ > 1'000'000'000 &&
                     !impl_->master_lost_reported_) {
                impl_->master_lost_reported_ = true;
                lx::trace::logger::global().log_error(committed.get_error(), "compositor.drm");
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.drm",
                                                "DRM master lost and the session did not pause "
                                                "(no logind?) — retrying every frame");
            }
        } else if (committed) {
            impl_->master_lost_since_ns_ = 0;
            impl_->master_lost_reported_ = false;
        }
        if (!committed)
            impl_->retract_commit_timing();
        if (committed && impl_->kms_atomic.completion_needs_dispatch())
            impl_->post_flip_dispatch();
        if (impl_->resume_result_pending_ && !paused_now) {
            impl_->resume_result_pending_ = false;
            impl_->post_resume_result(
                impl_->resume_done_gen_,
                committed ? session_resume_outcome::ok
                : !lost_to_pause && impl_->kms_atomic.last_failure_rejected_state()
                    ? session_resume_outcome::rejected
                    : session_resume_outcome::device_unavailable);
        }
        if (!committed && cursor_rode && !lost_to_pause) {
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
            impl_->ever_lit_.store(true, std::memory_order_release);
            impl_->composited_slot_ = -1;
            // The kernel has taken its own reference to the fence.
            impl_->discard_composited_fence();
            impl_->kms_pending_damage_.clear();
            commit_kind = "content";
            impl_->note_committed_input(true);
        } else {
            // Keep the slot and its fence: the commit is retried next tick without
            // compositing the frame again (or, paused, discarded by the resume).
            commit_kind = lost_to_pause ? "paused" : "failed";
        }
        // Lowered after the commit raised flip_pending (see the idle-callback check).
        impl_->frame_outstanding_.store(!committed, std::memory_order_release);
        // Log on transition only: a failing commit every frame must not flood the hot
        // path, but silently discarding it is how a dead present path stayed invisible.
        if (!committed && lost_to_pause) {
            // Expected; nothing to report.
        } else if (!committed && !impl_->drm_commit_failing_) {
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
               !impl_->kms_atomic.flip_pending() &&
               impl_->display_gate_.load(std::memory_order_acquire) == 0) {
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
        } else if (impl_->display_gate_.load(std::memory_order_acquire) != 0) {
            impl_->retract_commit_timing();
            commit_kind = "paused";
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
