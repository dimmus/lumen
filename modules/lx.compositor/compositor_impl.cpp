module;

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
import lx.health;
import lx.config;

module lx.compositor;

import std;

namespace lx::compositor::detail {

struct compositor_impl;

/// One output's half of the render path (ADR 008 M4): its CRTC pipeline and cursor plane,
/// its scanout slots with their buffer-age damage, the composited frame waiting for its
/// commit, and the flip state. Render affinity; UI reads only the atomics. Created per
/// output at start (and by output plans later), never per frame.
struct output_pipeline {
    static constexpr unsigned k_slots = 3;
    compositor_impl* impl = nullptr;
    unsigned slot = 0;
    lx::output_id id{};
    /// Null for a virtual output (no KMS): it "presents" when its frame is published.
    std::unique_ptr<drm::kms_pipeline> kms{};
    /// Render affinity after `start`. Declared after `kms` so it is destroyed first:
    /// removing its framebuffers takes the pointer off the CRTC before `kms` hands the CRTC
    /// back to the console.
    drm::kms_cursor_plane cursor{};
    scheduler::presentation_tracker presentation{};
    /// Where layout coordinates reach this output's pixels (render's copy).
    lx::point2i origin{};
    double scale = 1.0;
    /// wl_output.transform (render's copy): the cursor plane's position and image turn with it.
    lx::buffer_transform transform = lx::buffer_transform::normal;
    lx::size2i mode{};
    /// Three, not two: with two, the slot the display is scanning and the slot a queued
    /// flip is about to scan leave nothing writable, so every frame overlapping a flip had
    /// to be dropped. A third slot means the composite always has somewhere to go.
    gfx::gl_scanout_target gl[k_slots]{};
    gfx::render_target vk[k_slots]{};
    drm::kms_framebuffer fb[k_slots]{};
    /// CPU-writable scanout for the `cpu` present backend; parallel to `fb`.
    drm::kms_dumb_framebuffer dumb[k_slots]{};
    unsigned slot_count = 0;
    lx::rect2i scanout_rect{};
    std::atomic<unsigned> front{0};
    std::atomic<unsigned> pending_flip_slot{0};
    /// Newest composited slot not yet handed to the CRTC, or -1. A frame composited while a
    /// flip was queued waits here; another frame landing first takes its place (mailbox).
    int composited_slot = -1;
    /// Frame index of the snapshot composited into `composited_slot`: the commit's
    /// `user_tag`, echoed by its flip event (presentation feedback).
    unsigned composited_frame = 0;
    /// sync_file of the composite in `composited_slot`, or -1; owned here.
    int composited_fence = -1;
    /// Screen capture: the slot holding the newest composited frame (-1: none since the
    /// targets were made), its frame index and when it was composited. It stays intact until
    /// the next composite of this pipeline, so a capture can copy it without a new frame.
    int capture_front = -1;
    unsigned capture_front_frame = 0;
    unsigned long long capture_front_ns = 0;
    /// Per scanout slot: damage the slot missed since it was last composited into (buffer
    /// age). Starts full.
    gfx::damage_region slot_age[k_slots]{};
    /// Damage since the last successful KMS commit (FB_DAMAGE_CLIPS). Starts full.
    gfx::damage_region kms_pending{};
    /// Damage a failed composite could not draw, retried on the next tick.
    gfx::damage_region missed{};
    /// Render clock: when this output last composited (slot catch-up waits for idle).
    lx::runtime::clock_time last_composite_ns = 0;
    /// Frame index of the snapshot whose damage was last consumed.
    unsigned last_seen_frame = ~0u;
    /// Newest snapshot of this view acquired and composited (or skipped): -1 before any.
    std::atomic<long long> rendered_frame{-1};
    /// A composited frame is not committed yet (read by UI before it answers frame
    /// callbacks on an idle tick).
    std::atomic<bool> frame_outstanding{false};
    /// The plane passed a TEST_ONLY commit with an image on it.
    bool cursor_tested = false;
    /// A content commit has gone through since the last modeset need (the CRTC is ours).
    bool content_committed_once = false;
    unsigned cursor_staged_gen = 0;
    /// Render → UI. The commit whose flip is pending carried a frame (false: cursor only).
    std::atomic<bool> last_commit_content{true};
    /// Render → UI. A content commit of ours reached this CRTC since start.
    std::atomic<bool> ever_lit{false};
    /// Render affinity. The resume generation whose procedure ran on this pipeline, and a
    /// result owed to UI.
    unsigned resume_done_gen = 0;
    bool resume_result_pending = false;
    /// Written by render after each composite of this output, read by UI for its clock.
    std::atomic<unsigned> render_cost_us{0};
    bool drm_commit_failing = false;
    lx::runtime::clock_time master_lost_since_ns = 0;
    bool master_lost_reported = false;
    /// Waiting for the combined modeset (every pipeline needing one has a frame) since.
    lx::runtime::clock_time modeset_wait_since_ns = 0;
    /// The view's publish count when render last acquired it (the low-water mark).
    unsigned consumed_publishes = 0;
    // ── output plans (ADR 008 §A.4, §B.3), render affinity ──
    /// A mode change the UI switched the output to: applied right before the first snapshot
    /// of frame `switch_from_frame` or later is composited (older ones are the old size).
    bool switch_pending = false;
    unsigned switch_from_frame = 0;
    drm::kms_mode switch_mode{};
    /// Targets allocated and checked by TEST_ONLY for a mode change, waiting for the switch;
    /// dropped when no switch came within `k_staged_keep_ns` (the change was superseded).
    std::unique_ptr<output_pipeline> staged{};
    lx::runtime::clock_time staged_since_ns = 0;
    static constexpr lx::runtime::clock_time k_staged_keep_ns = 5'000'000'000;
    /// The previous mode's targets: one of them is on screen until the modeset's flip.
    std::unique_ptr<output_pipeline> graveyard{};
    /// The output went: its CRTC is turned off once no flip is pending, then the pipeline is
    /// handed to UI to be destroyed (`retire`).
    bool retiring = false;
    unsigned retire_gen = 0;
    bool retire_connector_gone = false;
    /// The first frame built for this pipeline's output: the view's snapshots of an output
    /// that had the slot before (the views are per slot and outlive outputs) are never
    /// composited here.
    unsigned first_frame = 0;
    bool has_first_frame = false;
    /// An output configuration (ADR 008 §C.3) waits for this pipeline's next modeset: its
    /// outcome goes to UI (`on_modeset_result`) with this ticket. 0: nobody waits. A mode
    /// switch's ticket moves here when the switch is applied (`switch_ticket`).
    unsigned apply_ticket = 0;
    unsigned switch_ticket = 0;
    /// Adaptive sync asked for while a mode switch is pending (-1: none): programmed by the
    /// switch's modeset, so both changes are one modeset in the new mode (and the switch,
    /// not a modeset still in the old mode, answers the configuration).
    int switch_vrr = -1;
    /// The cursor image turned for this output's transform (render; grows, never shrinks).
    std::vector<std::uint32_t> cursor_turned{};
    // ── input-to-present latency (debug) ──
    std::atomic<unsigned long long> committed_input_us{0};
    std::atomic<unsigned long long> committed_submit_us{0};
    std::atomic<int> committed_fence_fd{-1};

    output_pipeline() {
        for (auto& d : slot_age)
            d.set_full();
        kms_pending.set_full();
    }
    ~output_pipeline() {
        if (composited_fence >= 0)
            ::close(composited_fence);
        if (const int fd = committed_fence_fd.exchange(-1); fd >= 0)
            ::close(fd);
    }
    output_pipeline(const output_pipeline&) = delete;
    output_pipeline& operator=(const output_pipeline&) = delete;

    [[nodiscard]] bool scanout() const { return slot_count > 0; }
    void discard_fence() {
        if (composited_fence >= 0) {
            ::close(composited_fence);
            composited_fence = -1;
        }
    }
    /// The scanout slot that is neither on screen nor queued for a flip.
    [[nodiscard]] unsigned back_slot() const {
        if (slot_count <= 1)
            return 0;
        const unsigned f = front.load(std::memory_order_acquire);
        const unsigned pending = kms && kms->flip_pending()
                                     ? pending_flip_slot.load(std::memory_order_acquire)
                                     : f;
        for (unsigned i = 0; i < slot_count; ++i)
            if (i != f && i != pending)
                return i;
        return (f + 1u) % slot_count;
    }
    [[nodiscard]] unsigned framebuffer(unsigned i, bool cpu) const {
        return cpu ? dumb[i].id() : fb[i].id();
    }
};

/// UI's per-output state (ADR 008 M6, M14, M15). UI affinity.
struct output_ui {
    bool live = false;
    lx::output_id id{};
    /// UI's handle on the output's render pipeline (ADR 008 §B.3): set when render installed
    /// it, cleared before UI asks render to retire it — render owns `pipes_`, UI only ever
    /// reads through this, so neither thread sees a pipeline the other has let go of.
    output_pipeline* pipe = nullptr;
    /// A content flip landed since frame callbacks were last answered for this output.
    bool present_completed = false;
    /// The resume modeset's flip has not arrived yet (a full modeset can outlast the 250 ms
    /// present watchdog on some drivers, which is not a stall).
    bool awaiting_resume_flip = false;
    /// Frame callbacks paced by this output were last answered (the stall watchdog).
    lx::runtime::clock_time last_callback_ns = 0;
    /// Newest frame index published to this view with non-empty damage, and the newest
    /// published.
    long long last_damaged_frame = -1;
    unsigned last_published_frame = 0;
    bool published_once = false;
    /// The view's projection as last applied (a change repaints the view).
    lx::rect2i logical{};
    double scale = 0.0;
    lx::size2i mode{};
    unsigned transform = 0;
    /// The refresh its frame clock runs at (mHz; 0: 60 Hz assumed).
    unsigned refresh_mhz = 0;
    /// The output's power state machine (`:display_power`), one per output (M15).
    display_power power{};
    /// The render generation a power-on asked for on this output.
    unsigned power_resume_render_gen = 0;
    /// The first frame built with the scene locked (M14).
    unsigned lock_frame = 0;
    /// The flip handler's last feedback (frame callback timestamps).
    scheduler::presentation_feedback last_feedback{};
    /// Until when this output's first frame waits for a client's first surface (0: not
    /// waiting yet). See `hold_first_frames`.
    lx::runtime::clock_time first_frame_hold_until = 0;
    /// Frame callbacks of the last flip held until just before the next repaint
    /// (`fire_flip_callbacks`); 0 while none are held.
    lx::runtime::clock_time callback_release_ns = 0;
    /// How long clients take to answer this output's frame callbacks: release to the first
    /// commit that damaged the output, over the last `k_response_window` answers.
    static constexpr unsigned k_response_window = 8;
    bool awaiting_response = false;
    lx::runtime::clock_time response_ns[k_response_window]{};
    unsigned response_count = 0;
    unsigned response_next = 0;
    /// Health (`lx.health`): content frames presented, those that landed a refresh period
    /// or more after the vblank their repaint aimed at, and the last 128 of them. The aim
    /// is noted when a tick serves the output (`aim_frame` its frame index).
    unsigned long long presented = 0;
    unsigned long long missed = 0;
    lx::health::miss_ring recent{};
    lx::runtime::clock_time aim_ns = 0;
    unsigned aim_frame = 0;
};

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
    /// The edge-snap preview while a window is dragged to an edge (design-session-and-wm
    /// K.4): above the windows, under a panel.
    scene::container_node snap_preview_layer_{};
    scene::container_node top_layer_{};
    /// Fullscreen windows over their outputs' black backdrops (M11): above a panel, under
    /// the overlay layer and popups (sway).
    scene::container_node fullscreen_layer_{};
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
    scheduler::budget_tracker budget_{};
    protocol_managers protocols_{};
    hotplug_controller hotplug_{};
    gfx::device_recovery device_recovery_{graphics};
    session::logind_session logind_{};
    p0_protocol_context p0_{};

    // ── Session pause/resume (VT switching) — docs/architecture.md "Session pause/resume" ──
    /// UI affinity. The state machine; `apply_session_actions` executes what it returns.
    session_state session_{};
    /// UI affinity. VT and logout bindings, the shell's key grabs, tap observation.
    keybindings keys_{};
    /// UI affinity. xdg-toplevel-icon-v1's icon memfds and xdg-foreign's handles
    /// (design-app-compat-capture B.2, B.4); the protocol context points at them.
    toplevel_icon_table icons_{};
    foreign_registry foreign_{};
    /// The display gate (design §2.3). UI writes (release), render reads (acquire) at tick
    /// start and again right before every commit ioctl: while it is set, render composites
    /// nothing and commits nothing on any output, the frame clocks give way to the 1 Hz
    /// paused tick, and nothing is presented. `gate_vt`: the session is paused (VT away,
    /// display lost). Display power gates one output at a time (`gate_power_mask_`, M15).
    /// Only input suspension stays tied to `gate_vt` — a dark output is woken by input.
    static constexpr unsigned gate_vt = 1u << 0;
    std::atomic<unsigned> display_gate_{0};
    /// Bit per output slot powered off (zwlr_output_power_v1, idle blanking): render skips
    /// that pipeline; the others go on.
    std::atomic<std::uint32_t> gate_power_mask_{0};
    /// Per output: UI stores the resume generation (release) *before* clearing a gate bit,
    /// and render loads them in the opposite order: seeing "ungated" guarantees seeing the
    /// new generation, so render never commits without running the resume procedure first.
    /// One counter for every resume (VT and power-on), owned by UI (`render_gen_counter_`).
    std::atomic<unsigned> resume_gen_[output_manager::k_max]{};
    /// UI affinity. The last render generation handed out, and which resume each answers:
    /// the session machine's (with its own generation) and each output's power machine.
    unsigned render_gen_counter_ = 0;
    unsigned vt_resume_render_gen_ = 0;
    unsigned vt_resume_session_gen_ = 0;
    /// UI affinity. Outputs whose VT-resume result is still owed, and the worst one so far:
    /// the session machine hears once, when every output answered.
    std::uint32_t vt_resume_pending_ = 0;
    session_resume_outcome vt_resume_outcome_ = session_resume_outcome::ok;
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
    /// restart; every dark output is woken (I5). True when this event woke one — the caller
    /// then consumes a press (and so its release).
    bool on_user_activity();
    /// UI writes the newest power-off generation per output (release); the render task
    /// compares (acquire) and does nothing when a later request superseded it.
    std::atomic<unsigned> power_gen_[output_manager::k_max]{};
    /// UI. Executes output `slot`'s power decision (its gate bit, the render task, the
    /// resume, `mode` events).
    void apply_power_actions(unsigned slot, const power_actions& a);
    /// UI. zwlr_output_power_v1.set_mode on output `slot`.
    void request_output_power(unsigned slot, bool on);
    /// UI → render → UI. The power-off task of output `slot` for `gen`.
    void post_power_off(unsigned slot, unsigned gen);
    /// Render. Turns the output's CRTC off (or, refused, commits a black frame).
    power_off_outcome run_power_off(unsigned slot, unsigned gen);
    /// Render. The soft blank of one output: one black frame, cursor off, CRTC left on.
    void commit_black_frame(output_pipeline& p);
    bool power_rejected_logged_ = false;
    // ── Session lock — docs/architecture.md "Session lock" ──
    /// UI affinity. ext-session-lock-v1's state machine (`:session_lock`); the wire unit and
    /// the compositor execute what it returns (`apply_lock_actions`).
    session_lock lock_{};
    /// UI affinity. text-input-v3 ↔ input-method-v2 relay (`:text_input`); the wire units
    /// (wire_text_input.cpp, wire_input_method.cpp) send what it decides, the router routes
    /// keys to the input method's grab while it is active.
    text_input_relay text_input_{};
    /// UI affinity. Which outputs have shown a locked frame since the scene locked (M14):
    /// `locked` goes out once every live output has, or is dark.
    lock_frame_tracker lock_shown_{};
    /// UI affinity. The 1 s wait for lock surfaces, and the sleep delay's 2 s.
    std::unique_ptr<runtime::timer_source> lock_timer_{};
    static void lock_timer_callback();
    void rearm_lock_timer();
    /// UI affinity. While the system prepares to sleep with a lock asked for: the logind
    /// delay inhibitor is held until `locked` or this deadline (0: none).
    lx::runtime::clock_time sleep_lock_deadline_ns_ = 0;
    static constexpr lx::runtime::clock_time k_sleep_lock_wait_ns = 2'000'000'000;
    void release_sleep_delay(const char* why);
    /// The state last logged (one info line per transition).
    lock_state logged_lock_state_ = lock_state::unlocked;
    /// UI. Executes a lock decision (the wire half through `session_lock_send`).
    void apply_lock_actions(const lock_actions& a);
    /// UI. Nothing of the session can be on output `slot` right now: the VT is away, it is
    /// powered off (or blanked black), or nothing of ours was ever committed to it.
    [[nodiscard]] bool output_dark(unsigned slot) const;
    /// UI. Every live output is dark (`output_dark`): a lock reaching `locking` is `locked`
    /// at once.
    [[nodiscard]] bool display_dark() const;
    /// UI. Output `slot` showed a locked frame (or went dark): `locking` ends once every live
    /// output has.
    void note_lock_shown(unsigned slot);
    /// UI. `locking` ends when every output is dark or showed a locked frame. After every
    /// edge that can darken one.
    void advance_lock();
    /// UI. logind's Lock / Unlock / PrepareForSleep.
    void on_logind_lock();
    void on_logind_unlock();
    void on_prepare_for_sleep(bool start);
    /// UI. Ctrl+Alt+F<n> (XF86Switch_VT_<n>).
    void request_vt_switch(unsigned vt);
    /// Render. The resume procedure (A.5) of one pipeline for generation `gen`.
    void run_resume_procedure(output_pipeline& p, unsigned gen);
    /// Render → UI.
    void post_resume_result(unsigned slot, unsigned gen, session_resume_outcome outcome);
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
    /// Render affinity (created at start, before the render thread; at runtime by output
    /// ops). One per output slot with a pipeline; null for a free slot. UI reaches a
    /// pipeline only through `outs_[slot].pipe`.
    std::unique_ptr<output_pipeline> pipes_[output_manager::k_max]{};

    // ── Hotplug: output plans UI → render → UI (ADR 008 §A.4, Part B) ──
    /// What UI asks render to do with an output's pipeline. Applied at the start of the next
    /// `tick_render`; the answer comes back as a UI task.
    struct output_op {
        enum class kind : unsigned char { add, test_mode, switch_mode, retire, set_vrr };
        static constexpr unsigned k_modes = 8;
        kind what = kind::add;
        unsigned slot = 0;
        unsigned gen = 0;
        /// add: a KMS output (else a composite-only pipeline: virtual outputs, no display).
        bool kms = false;
        /// add: give it a cursor plane (the hardware cursor is in use).
        bool cursor_plane = false;
        /// retire: its connector no longer exists (only the CRTC is turned off).
        bool connector_gone = false;
        unsigned connector_id = 0;
        unsigned possible_crtcs = 0;
        unsigned routed_crtc_id = 0;
        /// add / test_mode: the modes to try, in order; switch_mode: [0].
        drm::kms_mode modes[k_modes]{};
        unsigned mode_count = 0;
        /// add: the size of a composite-only pipeline.
        lx::size2i size{};
        lx::point2i origin{};
        double scale = 1.0;
        /// switch_mode: the first frame built at the new mode; add: the first frame built for
        /// the new output.
        unsigned from_frame = 0;
        /// add / switch_mode / set_vrr: an output configuration waits for the modeset that
        /// makes it real (`output_pipeline::apply_ticket`); 0: nobody waits.
        unsigned ticket = 0;
        /// add / set_vrr: adaptive sync; add: the output's transform.
        bool vrr = false;
        lx::buffer_transform transform = lx::buffer_transform::normal;
    };
    /// A TEST_ONLY of a whole output configuration (ADR 008 §C.3 step 3): every pipeline it
    /// changes — a new mode (with real framebuffers of that size, staged for the switch),
    /// adaptive sync, disabled — and every output it enables (a CRTC allocation among the
    /// CRTCs left, checked by the same test). Outputs it leaves alone are in the kernel's
    /// current state the test runs against.
    struct config_test_entry {
        /// The output's slot; -1 for an output being enabled.
        int slot = -1;
        bool enable = true;
        unsigned connector_id = 0;
        unsigned possible_crtcs = 0;
        unsigned routed_crtc_id = 0;
        drm::kms_mode mode{};
        bool mode_change = false;
        /// -1: unchanged; 0/1: adaptive sync off/on.
        int vrr = -1;
    };
    struct config_test {
        unsigned gen = 0;
        unsigned count = 0;
        config_test_entry entries[output_manager::k_max]{};
    };
    /// One at a time (one job runs at a time): UI fills it under `ops_mutex_`, render takes it.
    config_test config_test_{};
    bool config_test_pending_ = false;
    config_test config_test_work_{};
    /// Render. Runs the test; the answer goes to UI (`on_config_tested`).
    void render_test_config(const config_test& t);
    static constexpr unsigned k_max_ops = 32;
    std::mutex ops_mutex_{};
    std::inplace_vector<output_op, k_max_ops> ops_{}; // guarded by ops_mutex_
    /// Render affinity: the ops taken from the queue, run outside the lock.
    std::inplace_vector<output_op, k_max_ops> ops_work_{};
    std::atomic<bool> ops_pending_{false};
    /// Render affinity. Ops held back until every retiring KMS pipeline has turned its CRTC
    /// off (a KMS add, and every later op of the same slot, in order): an add's CRTC
    /// allocation and TEST_ONLY then see the state its configuration was tested in — the
    /// outputs it disables already off — not one with them still lit (shared PLLs, MST
    /// bandwidth). Bounded by the queue; run by `run_deferred_output_ops`.
    std::inplace_vector<output_op, k_max_ops> ops_deferred_{};
    std::uint32_t ops_deferred_slots_ = 0;
    lx::runtime::clock_time ops_deferred_since_ns_ = 0;
    /// A retiring pipeline's flip that never lands does not hold an add back longer.
    static constexpr lx::runtime::clock_time k_ops_defer_max_ns = 1'000'000'000;
    /// UI. Queues `op` for render and wakes it.
    void post_output_op(const output_op& op);
    /// Render, at the start of `tick_render`. Retires first (each one that is the first op
    /// of its slot in the batch), then the rest in order.
    void run_output_ops();
    /// Render. One op.
    void run_output_op(const output_op& op);
    /// Render. Runs `op` now, or holds it back (see `ops_deferred_`).
    void run_or_defer_output_op(const output_op& op);
    /// Render, in `tick_render` once retiring pipelines were given their turn.
    void run_deferred_output_ops();
    [[nodiscard]] bool kms_retire_in_flight() const;
    void render_add_output(const output_op& op);
    void render_test_mode(const output_op& op);
    /// Render. A pending mode switch of `p`, before compositing frame `frame`; false when it
    /// has to wait (the previous switch's targets are still on screen).
    bool apply_mode_switch(output_pipeline& p, unsigned frame);
    /// Render. A retiring pipeline whose flip has drained: CRTC off, targets released,
    /// handed to UI.
    void finish_retire(unsigned slot);
    /// Render → UI: pipelines in transit to be destroyed on UI (the thread draining KMS
    /// events, which may still hold them). Deleted by `on_retired`, or at exit.
    std::atomic<output_pipeline*> in_transit_[output_manager::k_max]{};
    void wake_render();
    /// UI. The answers.
    void on_add_result(unsigned slot, unsigned gen, bool ok, unsigned mode_index,
                       output_pipeline* pipe);
    void on_mode_tested(unsigned slot, unsigned gen, bool ok, unsigned mode_index);
    void on_retired(unsigned slot, unsigned gen);

    /// UI. Where each slot is in its life: an output being added (its pipeline is being
    /// made), live, or retiring (its pipeline is being taken down). A slot is reused only
    /// once free (ADR 008 M1).
    enum class slot_phase : unsigned char { free, adding, live, retiring };
    slot_phase slot_phase_[output_manager::k_max]{};
    unsigned slot_gen_[output_manager::k_max]{};
    [[nodiscard]] std::uint32_t reserved_slots() const {
        std::uint32_t m = 0;
        for (unsigned s = 0; s < output_manager::k_max; ++s)
            if (slot_phase_[s] == slot_phase::adding || slot_phase_[s] == slot_phase::retiring)
                m |= 1u << s;
        return m;
    }
    /// UI. What an add is waiting for: the connector's probe (the strings the output's
    /// description points into), the modes tried, whether a later scan cancelled it, and
    /// whether it is HEADLESS-1 (already live, only its pipeline is coming).
    struct pending_output {
        drm::connector_probe probe{};
        drm::kms_mode modes[output_op::k_modes]{};
        unsigned mode_count = 0;
        bool cancelled = false;
        bool headless = false;
        bool kms = false;
        /// A newer probe saw this connector change while its add was under way: probed
        /// again once the output is up, so it ends at what is connected now.
        bool stale = false;
        /// What it comes up with when a configuration chose it (a client's apply, the
        /// persisted monitor and setup — ADR 008 §C.5): 0 scale is the default one.
        double scale = 0.0;
        unsigned transform = 0;
        bool adaptive_sync = false;
        bool has_position = false;
        lx::point2i position{};
        bool follow_preferred = true;
        unsigned ticket = 0;
        /// Its head was advertised already (a disabled monitor being enabled): it stays a
        /// head while it comes up. A newly plugged monitor's head appears once it is live.
        bool head_known = false;
    };
    std::unique_ptr<pending_output[]> pending_{};
    /// UI. Connectors whose add failed (no CRTC, no mode passed TEST_ONLY), with the mode
    /// list they failed with: the same scan again is not retried.
    struct failed_connector {
        unsigned connector_id = 0;
        unsigned modes_hash = 0;
    };
    std::inplace_vector<failed_connector, output_manager::k_max> failed_{};
    /// UI. HEADLESS-1 standing in for a display with nothing connected (-1: none): removed
    /// once a real output is added, added before the last one is removed.
    int fallback_headless_slot_ = -1;
    /// The card was removed: no more KMS (render reads it before touching the device).
    std::atomic<bool> card_gone_{false};
    /// UI. The VT resume waits (≤ 250 ms) for its connector probe, so its modeset uses what
    /// is connected now (a resize while away) — ADR 008 §A.14.
    bool resume_probe_wait_ = false;
    unsigned resume_probe_gen_ = 0;
    /// The hotplug controller can probe (KMS present): the resume waits for its probe.
    bool hotplug_ready_ = false;
    std::unique_ptr<runtime::timer_source> hotplug_timer_{};
    static constexpr lx::runtime::clock_time k_resume_probe_wait_ns = 250'000'000;
    static void hotplug_timer_callback();
    /// Render-read after start: the cursor plane size the start-up themes need.
    unsigned cursor_plane_px_ = 0;
    /// UI. A finished probe (the hotplug controller's `scan_ready`).
    void apply_connector_scan(const drm::connector_scan& scan, bool forced);
    /// What an output made from a connector comes up with (ADR 008 §C.5): the modes to try,
    /// and the scale, transform, adaptive sync and position a configuration chose.
    struct connect_choice {
        bool enabled = true;
        drm::kms_mode modes[output_op::k_modes]{};
        unsigned mode_count = 0;
        double scale = 0.0;
        unsigned transform = 0;
        bool adaptive_sync = false;
        bool has_position = false;
        lx::point2i position{};
        bool follow_preferred = true;
        unsigned ticket = 0;
    };
    /// UI. Brings the output of connector `probe` up (null `choice`: the defaults). Returns
    /// the slot it is being added in, -1 when it cannot be.
    int add_output_from(const drm::connector_probe& probe, const connect_choice* choice = nullptr);
    void change_output(unsigned slot, const drm::connector_probe& probe, bool forced);
    /// UI. The output in `slot` goes: unplugged, or (`why` set) disabled by a configuration
    /// while its connector stays connected.
    void remove_output(unsigned slot, bool connector_gone, const char* why = nullptr);
    /// UI. The output in `slot` takes `mode` now (logical state, the render switch).
    /// `follow`: it keeps following its connector's preferred mode (H3); `custom`: a mode the
    /// connector does not list; `ticket`: an output configuration waits for its modeset.
    void switch_output_mode(unsigned slot, const drm::kms_mode& mode, const char* why);
    void switch_output_mode(unsigned slot, const drm::kms_mode& mode, const char* why, bool follow,
                            bool custom, unsigned ticket);
    /// UI. Adds HEADLESS-1 (no connected display), its pipeline following.
    void add_fallback_headless();
    void on_card_removed();
    /// UI. Output state changed: views, clocks, clients told now (not at the next tick),
    /// the pointer kept on the outputs.
    void outputs_changed_now();
    void reclamp_pointer();
    /// UI. Moves the pointer to the global point as motion would — kept on the outputs, the
    /// cursor and pointer focus follow, the focus gets `wl_pointer.motion` — but with no
    /// relative motion (pointer-constraints: a lock's cursor hint on unlock, a confinement
    /// brought inside its new region).
    void warp_pointer(double x, double y);
    /// UI. The VT resume, after its probe (or the 250 ms wait).
    void finish_display_resume(unsigned gen);

    // ── Output configuration: wlr-output-management and persistence (ADR 008 Part C) ──
    /// UI. The persisted configuration (`outputs.toml`); the model is kept in memory even
    /// when persistence is off (setups still decide which monitors are enabled).
    output_config_file outcfg_{};
    std::unique_ptr<runtime::timer_source> outcfg_timer_{};
    static void outcfg_timer_callback();
    /// UI. The newest connector scan applied (a copy: the hotplug controller reuses its
    /// buffer): the heads of connectors that are not outputs, and every head's mode list.
    std::unique_ptr<drm::connector_scan> last_scan_{};
    /// UI. Connectors connected but disabled — by a client's configuration or the setup —
    /// and the set of connected connectors those decisions were made for.
    std::inplace_vector<unsigned, output_manager::k_max> disabled_connectors_{};
    std::inplace_vector<unsigned, output_manager::k_max> connected_set_{};
    [[nodiscard]] bool connector_disabled(unsigned connector_id) const;
    void set_connector_disabled(unsigned connector_id, bool disabled);
    /// UI. Where a disabled monitor was placed and the mode it had, when chosen explicitly:
    /// enabling it again without them brings them back (wlroots keeps a head's last state).
    struct kept_position {
        unsigned connector_id = 0;
        bool has_position = false;
        lx::point2i position{};
        bool has_mode = false;
        drm::kms_mode mode{};
    };
    std::inplace_vector<kept_position, output_manager::k_max> kept_positions_{};
    void keep_position(unsigned connector_id, const output_state& o);
    [[nodiscard]] const kept_position* kept_position_of(unsigned connector_id) const;
    /// UI. Virtual outputs as heads (HEADLESS-n, `--virtual-outputs`): their key, and what a
    /// disabled one comes back as.
    struct virtual_head {
        unsigned key = 0;
        char name[32]{};
        bool disabled = false;
        lx::size2i mode{};
        unsigned refresh_mhz = 60000;
        double scale = 1.0;
        unsigned transform = 0;
        lx::point2i position{};
        bool auto_position = true;
    };
    std::inplace_vector<virtual_head, output_manager::k_max> virtual_heads_{};
    unsigned next_virtual_key_ = 1;
    [[nodiscard]] unsigned virtual_key(const char* name);
    /// UI. The heads as zwlr_output_manager_v1 shows them, rebuilt and sent (only what
    /// changed) once per loop iteration after anything about outputs or connectors moved.
    std::unique_ptr<mgmt::head_set> heads_scratch_{};
    bool heads_dirty_ = true;
    void mark_heads_dirty() { heads_dirty_ = true; }
    void build_heads(mgmt::head_set& out);
    void flush_output_heads();
    /// UI. The identifiers of the connected desktop connectors of `scan` (duplicates told
    /// apart as `output_manager` does), with each one's index in the scan.
    unsigned connected_identifiers(const drm::connector_scan& scan,
                                   char (*ids)[monitor_config::k_id], unsigned* entries) const;
    /// UI. What connector `e` (identifier `id`) comes up with: the monitor's persisted mode,
    /// scale, transform and adaptive sync, and the setup's enabled flag and position.
    void choose_for_connector(const drm::connector_probe& e, const char* id,
                              const setup_config* setup, connect_choice& out) const;
    /// UI, at start: the outputs of the start-up scan as the persisted configuration says.
    void configure_outputs_at_start(const drm::connector_scan& scan);
    /// UI, after a scan's removals and changes: new connectors come up, and when the set of
    /// connected monitors changed, the setup decides which are enabled and where.
    void apply_connected_setup(const drm::connector_scan& scan);
    /// UI. The current state into the persisted configuration (after a client's apply).
    void record_output_config();
    /// The kms mode a head target asks for on connector `e` (null: a virtual output).
    [[nodiscard]] drm::kms_mode target_kms_mode(const mgmt::head_target& t,
                                                const drm::connector_probe* e) const;
    /// The live slot of head `key`, -1 when it is not an output now.
    [[nodiscard]] int slot_of_head(unsigned key) const;
    [[nodiscard]] unsigned max_enabled_heads() const;
    /// The apply or test running (one at a time, §C.3 step 7).
    struct mgmt_run {
        bool active = false;
        mgmt::job job{};
        mgmt::target target{};
        mgmt::target previous{};
        enum class phase : unsigned char { testing, committing } stage = phase::testing;
        unsigned test_gen = 0;
        unsigned ticket = 0;
        /// Slots whose modeset is awaited, and whether one failed.
        std::uint32_t waiting = 0;
        bool failed = false;
        lx::runtime::clock_time deadline_ns = 0;
    };
    std::unique_ptr<mgmt_run> mgmt_{};
    unsigned config_test_gen_ = 0;
    unsigned apply_ticket_counter_ = 0;
    static constexpr lx::runtime::clock_time k_mgmt_deadline_ns = 3'000'000'000;
    /// UI. Starts the next queued job when none runs and no output is mid-way.
    void run_next_mgmt_job();
    /// UI. Fills `out` with what the kernel must be asked about; false when nothing.
    bool plan_config_test(const mgmt::target& t, config_test& out) const;
    void on_config_tested(unsigned gen, bool ok, unsigned tests);
    /// UI. Makes the outputs what `t` says (C.3 step 4): disables, modes, adaptive sync,
    /// transforms, scales, positions, enables. `waiting` receives the slots whose modeset
    /// will report to `ticket` (0: a revert, nobody waits).
    void switch_to_target(const mgmt::target& t, unsigned ticket, std::uint32_t& waiting);
    void on_modeset_result(unsigned slot, unsigned ticket, bool ok);
    void complete_apply(const char* why);
    void finish_mgmt_job(mgmt_result result, const char* why);
    /// Render affinity. The KMS state every output's pipeline was last modeset with (the
    /// combined modeset waits for every pipeline that needs one, M5).
    static constexpr lx::runtime::clock_time k_modeset_wait_ns = 100'000'000;
    /// Creates the scanout targets of `p` at its mode (GL, CPU or Vulkan per the present
    /// backend) — the per-output half of the old `setup_*_present`.
    [[nodiscard]] lx::result<void> create_targets(output_pipeline& p, bool scanout);

    // ── per-output state (UI affinity unless noted) ──
    output_ui outs_[output_manager::k_max]{};
    /// One frame clock per output over one shared timer (ADR 008 M6).
    scheduler::frame_clocks clocks_{};
    /// UI → render: bit per output whose view published since render last looked.
    std::atomic<std::uint32_t> render_mask_{0};
    /// UI → render: bumped after each resume UI posts (`resume_gen_`), work only a render
    /// tick notices.
    std::atomic<unsigned long long> render_posts_{1};
    /// Render → UI, at the end of each render tick: the `render_posts_` it started from when
    /// nothing of render's own is left for a later tick (no flip or frame waiting, no
    /// leftover damage, no targets kept around, no deferred op, texture retirement, capture
    /// job or resume), else 0. Equal to `render_posts_`: the idle housekeeping tick leaves the
    /// render thread asleep. A post after the tick began no longer matches, so is not missed.
    std::atomic<unsigned long long> render_quiet_at_{0};
    /// UI affinity. The outputs `on_vsync` is serving (`tick_ui`); 0 outside a vsync tick
    /// (a test driving `tick_ui` directly serves every live output).
    std::uint32_t due_mask_ = 0;
    bool in_vsync_ = false;
    /// UI affinity. Served outputs whose tick found nothing to do (`settle_frame_clocks`).
    std::uint32_t tick_quiet_mask_ = 0;
    /// UI affinity. Newest frame index published to any view.
    long long last_published_any_ = -1;
    /// UI affinity. `outputs.layout_generation()` the views and clocks were synced to.
    unsigned applied_layout_generation_ = ~0u;
    /// UI affinity. wp_presentation feedback in flight (`p0_.presentation`).
    presentation_feedback_table presentation_feedback_{};
    /// Debug screenshot (`compositor::request_screenshot`, SIGUSR1 in lumen-compositor):
    /// requested from any thread or a signal handler; UI forces a full repaint, render reads
    /// back the frame it composites next and writes a PPM. Never set in normal operation.
    std::atomic<bool> screenshot_requested_{false};
    /// Render affinity: outputs still to write a screenshot of (bit per slot).
    std::atomic<std::uint32_t> screenshot_pending_{0};
    unsigned screenshot_count_ = 0;
    void write_screenshot(const output_pipeline& p, unsigned slot);
    /// UI affinity. `outputs.generation()` clients were last told about.
    unsigned output_generation_ = 0;
    /// UI affinity. xdg_wm_base.ping/pong: who is not responding.
    liveness_tracker liveness_{};
    activation_tokens activation_{};
    bool initial_damage_noted_ = false;
    /// The render low-water mark (ADR 008 §A.4): the oldest frame an output can still
    /// sample — the newest rendered frame of any output whose view has an unconsumed
    /// snapshot or missed damage, else the newest frame rendered at all. Written by render
    /// after each tick; UI retires client buffers against it, render forgets textures by it.
    /// -1 until the first frame completes.
    std::atomic<long long> render_low_water_{-1};
    /// Render affinity. Computes the mark from every pipeline.
    [[nodiscard]] long long compute_low_water() const;
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
    shm_pixel_store shm_store_{};
    /// GL and Vulkan paths: transient staging, a block per in-flight update (see
    /// shm_upload_pool).
    shm_upload_pool shm_pool_{};
    /// Staging blocks unused this long are unmapped; long enough to ride out a pause in
    /// an animation, short enough that an idle desktop holds none.
    static constexpr unsigned long long k_staging_idle_ns = 1'000'000'000ull;
    bool reported_partial_fresh_ = false;
    /// GL path: shm commits uploaded from the client's memory, no staging (see the class).
    shm_direct_uploads shm_direct_{};

    // ── cursor ──
    /// UI affinity; one theme per distinct output scale (≤ 4), loaded once in `start` at the
    /// scale's pixel size (ADR 008 §A.7).
    xcursor_theme cursor_themes_[k_cursor_scales]{};
    /// The theme at the configured default output scale, read on a helper thread beside the
    /// present path's set-up (`begin_cursor_theme`): the cursor set-up adopts it for that
    /// scale instead of reading the files itself.
    std::thread cursor_theme_thread_{};
    xcursor_theme prefetched_theme_{};
    unsigned prefetched_scale120_ = 0;
    void begin_cursor_theme();
    /// config.toml cursor.size, else XCURSOR_SIZE, else 24: the logical cursor size.
    [[nodiscard]] unsigned cursor_base_px() const;
    /// config.toml cursor.theme, else null ($XCURSOR_THEME, else "default").
    [[nodiscard]] const char* cursor_theme_arg() const {
        return cursor_theme_name_[0] ? cursor_theme_name_ : nullptr;
    }
    unsigned cursor_theme_count_ = 0;
    /// The scale (120ths) each theme was loaded for, and XCURSOR_SIZE (logical px).
    unsigned cursor_theme_scale120_[k_cursor_scales]{};
    unsigned cursor_base_px_ = 24;
    /// UI. A scale no theme is loaded for came with an output configuration or a hotplug:
    /// its theme is loaded (a free slot, or one of a scale no output uses any more), so the
    /// pointer is as sharp there as anywhere (file I/O on a configuration change only).
    void sync_cursor_themes();
    /// UI → render. Pointer position/visibility, and plane-sized images per scale.
    cursor_frame_mailbox cursor_frames_{};
    cursor_image_mailbox cursor_images_{};
    /// Render affinity. The newest cursor frame taken from the mailbox.
    cursor_frame cursor_cur_{};
    /// Render → UI. A plane is unusable; UI switches the cursor to software everywhere.
    std::atomic<bool> cursor_hw_failed_{false};

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
    /// UI affinity. Fixed sample window for the periodic summary; no allocation.
    static constexpr unsigned k_latency_window = 600;
    std::inplace_vector<unsigned long long, k_latency_window> latency_samples_{};
    /// Render affinity. A commit of `p` carrying this tick's cursor frame succeeded: its
    /// pipeline's `committed_input_us` gets `input_us << 1 | content` when free (0), so a
    /// commit racing the flip handler drops its sample instead of being timed against the
    /// previous commit's flip.
    void note_committed_input(output_pipeline& p, bool content);
    /// UI affinity. Logs one present and, every `k_latency_window`, a summary.
    void note_present_latency(output_pipeline& p, unsigned long long present_ns,
                              unsigned long long submit_us, unsigned long long fence_us);
    void log_latency_summary();
    /// `output N name=… mode=WxH refresh_mhz=…` (info) for every output: at start and on
    /// every change clients are told about.
    void log_outputs() const;
    /// Render affinity. When the commit of `p` whose flip is pending was issued, and a dup
    /// of the sync_file its plane waited on (-1 when there was none). With the flip
    /// timestamp they split a present into compositor, GPU and display time, which is what
    /// tells a slow tail on a VM host from one in Lumen. Published before the commit ioctl,
    /// since its flip can be drained before the ioctl returns; taken by the flip handler.
    /// Only filled while debug logging is on: a dup per commit is not free.
    void publish_commit_timing(output_pipeline& p, int fence_fd);
    /// Render affinity. Takes it back after the commit failed.
    void retract_commit_timing(output_pipeline& p);

    /// Render affinity. Prepares output `p`'s plane for this tick's commit (newest
    /// position on this output, the image of its scale staged into the back buffer) —
    /// only when no flip is pending, which is also the only time a commit can happen.
    void prepare_cursor_plane(output_pipeline& p);
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
    /// As many as there can be live textures (the backends' tables): a client that drops
    /// all of its buffers at once (weston-simple-timing holds ~110) retires them together.
    static constexpr unsigned k_max_deferred_forgets = 256;
    struct deferred_forget {
        unsigned texture_id = 0;
        unsigned retire_after_frame = 0;
        bool used = false;
    };
    deferred_forget deferred_forgets_[k_max_deferred_forgets]{};

    void queue_forget(unsigned texture_id, unsigned retire_after_frame);
    void apply_forget(unsigned texture_id);
    /// Applies every retirement whose frame no output can still sample (the low-water mark).
    void apply_due_forgets(long long low_water);
    bool headless_present_ = true;
    bool drm_present_ = false;
    bool vulkan_present_ = false;
    bool cpu_present_ = false;
    bool gl_present_ = false;

    /// If the present path stops reporting flips, clients would never be told to draw
    /// again. Past this gap, fall back to tick pacing so a broken output cannot wedge them.
    static constexpr lx::runtime::clock_time k_callback_stall_ns = 250'000'000;

    /// Over-budget frames are reported as an aggregate every k_budget_report_frames
    /// ticks — the hot path must not emit a warn per frame.
    static constexpr unsigned k_budget_report_frames = 600;
    unsigned ui_over_budget_ = 0;
    unsigned ui_over_budget_max_us_ = 0;
    unsigned ui_budget_window_ = 0;
    unsigned render_over_budget_ = 0;
    unsigned render_budget_window_ = 0;

    std::unique_ptr<runtime::timer_source> vsync_timer{};
    std::unique_ptr<runtime::fd_source> wayland_fd_source{};
    std::unique_ptr<runtime::fd_source> drm_fd_source{};
    std::unique_ptr<runtime::fd_source> input_fd_source{};
    /// The content-update queue's readiness poller (fence waits; one source for all).
    std::unique_ptr<runtime::fd_source> content_fd_source{};
    /// Explicit sync: the newest composite fence of a frame, for release points (render
    /// publishes, UI dups; only while some client uses linux-drm-syncobj-v1).
    gfx::release_fence_mailbox release_fences_{};
    // ── Screen capture (design-app-compat-capture Part D) ──
    /// UI ↔ render jobs and completions, the client shm render writes into, what the wire is
    /// told about outputs and the render path, and the render thread's executor.
    capture_channel capture_channel_{};
    capture_shm_guards capture_guards_{};
    capture_host capture_host_{};
    capture_renderer capture_renderer_{};
    /// Render → UI: a task to take the completions is queued.
    std::atomic<bool> capture_ui_posted_{false};
    void notify_capture_completions();
    /// UI. What capture sees of the outputs, refreshed before every capture tick.
    void refresh_capture_host();
    /// Render. The pointer as output `p` shows it (paint_cursors, the software cursor test).
    [[nodiscard]] capture_cursor capture_cursor_for(const output_pipeline& p) const;
    /// Render, after the commits: copies that need no new composite, window captures,
    /// curtains; completions handed to UI.
    void run_capture_render();
    std::unique_ptr<runtime::event_source> logind_source{};

    /// The frame clocks run only while there is something to do. An output whose two
    /// consecutive ticks find nothing for it — no damage, no frame of it the render thread
    /// has not reached, no flip or commit outstanding on it — stops (unless something only
    /// a tick finishes is pending anywhere: a frame callback, a buffer, deferred WM work);
    /// any client request, input event or flip starts it again, aligned to its display.
    /// With every clock stopped, an idle desktop wakes once a second for
    /// `housekeeping_timer` instead of 60 times per output.
    /// While every clock is stopped: one tick a second for the deadlines only a tick notices
    /// (liveness pings, unmapping idle staging blocks).
    std::unique_ptr<runtime::timer_source> housekeeping_timer{};
    static constexpr runtime::clock_time k_housekeeping_ns = 1'000'000'000;
    /// UI thread. Starts the frame clocks of `mask` (default: every live output) that are
    /// stopped and not dark, and re-arms the shared timer.
    void request_frame(std::uint32_t mask = ~0u);
    /// UI. Output `slot` flipped a frame with client content: its frame callbacks, now or
    /// held until just before its next repaint.
    void fire_flip_callbacks(unsigned slot);
    /// UI. Answers the frame callbacks output `slot` paces (the flip's, held or not).
    void release_flip_callbacks(unsigned slot);
    /// UI. How far ahead of output `slot`'s next repaint its clients need their frame
    /// callbacks: the slowest recent answer plus a margin; 0 before any answer was measured.
    [[nodiscard]] lx::runtime::clock_time client_lead_ns(unsigned slot) const;
    /// UI. Clients committed damage to the outputs of `fresh`: their answer time.
    void note_client_response(std::uint32_t fresh);
    /// UI. Releases held frame callbacks that are due and re-arms `callback_timer_`.
    void release_due_callbacks();
    void rearm_callback_timer();
    std::unique_ptr<runtime::timer_source> callback_timer_{};
    static void callback_timer_callback();
    /// Margin added to the clients' measured answer time.
    static constexpr runtime::clock_time k_client_margin_ns = 1'500'000;
    /// Holding callbacks for less than this is not worth a timer wake-up.
    static constexpr runtime::clock_time k_callback_hold_min_ns = 500'000;
    /// UI thread: arms the shared timer at the earliest running clock, or the housekeeping
    /// timer when none runs.
    void rearm_frame_timer();
    /// UI thread, after each tick: per served output, stop its clock if this was its k-th
    /// quiet tick.
    void settle_frame_clocks(std::uint32_t served, std::uint32_t quiet);
    /// UI thread: the per-output state follows the output manager (views, clocks, the
    /// lock's outputs) — at start and whenever the layout moved.
    void sync_output_views();

    /// UI thread, in a tick: the outputs of `due` whose first frame waits for a client's
    /// first surface; their clocks are stopped and `first_frame_timer_` ends the wait.
    [[nodiscard]] std::uint32_t hold_first_frames(std::uint32_t due, runtime::clock_time now);
    std::unique_ptr<runtime::timer_source> first_frame_timer_{};
    static void first_frame_timer_callback();
    /// UI thread, GL context current, scanout targets created: draws once through every
    /// client texture path so the driver builds its shader and surface variants now, not
    /// on the first client frame.
    void warm_gl_paths();
    /// UI thread. Brings up libinput and routes its events to focused clients:
    /// `begin_input` opens the seat's devices on a helper thread — mostly logind
    /// round trips, so they overlap the present path's set-up — and `finish_input` joins it
    /// and installs the event routing. Nothing else may use `logind_` or `input_devices_`
    /// between the two.
    void begin_input();
    void finish_input();
    std::thread input_open_thread_{};
    std::optional<lx::result<lx::input::input_manager>> input_opened_{};
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
    void on_input_gesture(const lx::input::gesture_event& event);
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
    /// Render ticks run (`compositor::render_and_wait`).
    std::atomic<unsigned> render_ticks_{0};
    std::atomic<unsigned> frame_index{0};

    scheduler::frame_tick last_tick{};
    std::chrono::steady_clock::time_point last_vsync_time{};

    explicit compositor_impl(compositor* o, config c)
        : owner{o}, cfg{c}, shell_bridge_{wayland},
          import_cache{graphics.dmabuf(), c.import_cache},
          buffer_lifecycle_{import_cache},
          memory_{c.memory}, budget_{c.hot_path} {
        scene.set_snapshot_config(c.snapshot);
        (void)scene.root().add(&background_layer_);
        (void)scene.root().add(&bottom_layer_);
        (void)scene.root().add(&windows_layer_);
        (void)scene.root().add(&snap_preview_layer_);
        (void)scene.root().add(&top_layer_);
        (void)scene.root().add(&fullscreen_layer_);
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
        frame_renderer_.set_pipeline_cache(&pipeline_cache_);
        seats.bind(this);
    }

    ~compositor_impl() {
        // Clients first, while everything their objects' destructors reach (the scene, the
        // window manager, the tree) is still alive: members are destroyed in reverse order,
        // and the server is declared first.
        wayland.destroy_clients();
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
        // Pipelines before the device they restore the console's CRTCs through.
        for (auto& p : pipes_)
            p.reset();
        // Retired pipelines whose hand-over to UI never ran (the loop stopped first).
        for (auto& t : in_transit_)
            delete t.exchange(nullptr);
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
    /// One tick of the frame clocks: the outputs due now (`frame_clocks::take_due`) plus
    /// `forced` (the housekeeping and paused ticks serve outputs whose clock is stopped).
    /// `housekeeping`: the idle once-a-second tick, which wakes render only for work.
    void on_vsync(std::uint32_t forced = 0, bool housekeeping = false);
    /// UI affinity. Whether the render thread has anything to do: it said at the end of its
    /// last tick that nothing of its own was left (`render_quiet_at_`), and nothing new has
    /// been handed to it since.
    [[nodiscard]] bool render_has_work() const;
    /// Render affinity, end of `tick_render`: see `render_quiet_at_`.
    [[nodiscard]] bool render_is_quiet() const;
    void render_loop();
    void worker_loop();
    void register_p0_globals();

    /// A flip of one output's pipeline (UI thread; `user_data` is the `output_pipeline`).
    static void presentation_flip_handler(scheduler::presentation_feedback fb, void* user_data);
    void on_output_flip(output_pipeline& p, const scheduler::presentation_feedback& fb);

    /// Render-affinity only: apply queued client-buffer updates before compositing the
    /// snapshot of `frame` — only those that snapshot may show (see `deferred_updates_`).
    void drain_texture_updates(unsigned frame);
    /// Render-affinity only: one update, applied now.
    void apply_texture_update(texture_update& update);
    /// Bring up the Vulkan composite (targets per output come from `create_targets`).
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

    // ── the session, the control socket, health (design-session-and-wm Part 1; UI) ──
    /// io.lumen.Compositor on <socket>.varlink, and the private sd-event loop the user bus
    /// shares: one fd in the UI loop (`control_source_`).
    control_server control_{};
    std::unique_ptr<lx::runtime::event_source> control_source_{};
    /// The environment exported to the user manager in `--session` mode.
    session::user_environment user_env_{};
    bool ready_sent_ = false;
    lx::runtime::clock_time started_ns_ = 0;
    /// The systemd watchdog (WatchdogSec=): pinged from wake-ups that happen anyway, at most
    /// every quarter period, and only while the render thread is alive (its heartbeat moved,
    /// or it is idle). 0: off.
    lx::runtime::clock_time watchdog_period_ns_ = 0;
    lx::runtime::clock_time watchdog_next_ns_ = 0;
    unsigned watchdog_render_ticks_ = 0;
    bool watchdog_withheld_ = false;
    /// While `lumen-ctl monitor` subscribes: the health level is re-evaluated this often
    /// (only then; nobody watching costs nothing).
    static constexpr lx::runtime::clock_time k_health_poll_ns = 2'000'000'000ll;
    lx::runtime::clock_time next_health_poll_ns_ = 0;
    /// Render: inside `tick_render` (the heartbeat's "busy" half).
    std::atomic<bool> render_in_tick_{false};
    /// SIGHUP arrived (request_reload); handled after the next dispatch.
    std::atomic<bool> reload_requested_{false};
    // ── The configuration file (design-session-and-wm Part C) ──
    /// The snapshot in use, its bytes, the generation's diagnostics, the inotify watch.
    config_state config_{};
    std::unique_ptr<runtime::fd_source> config_watch_source_{};
    /// One timer: the 100 ms debounce after the watch saw a change.
    std::unique_ptr<runtime::timer_source> config_timer_{};
    static constexpr lx::runtime::clock_time k_config_debounce_ns = 100'000'000ll;
    /// A generation is being applied (its keymap or cursor job on the worker); a reload
    /// asked for meanwhile runs after it (`config_again_`, forced when any asker was).
    bool config_busy_ = false;
    bool config_again_ = false;
    bool config_again_force_ = false;
    unsigned config_jobs_ = 0;
    /// SIGHUP's reload: READY=1 goes out when the generation is applied.
    bool config_sighup_ = false;
    /// Reload calls of the control socket waiting for the generation.
    std::inplace_vector<unsigned, 8> config_tokens_{};
    /// A SIGHUP and Reload calls that came while a generation was busy: they wait for the
    /// queued reload's generation (they must see the files as they are now, not the bytes
    /// the busy generation read).
    bool config_sighup_next_ = false;
    std::inplace_vector<unsigned, 8> config_tokens_next_{};
    /// The generation the shell's notification state was last told about (zlm v7).
    unsigned config_bytes_generation_ = 0;
    /// A keymap compiled on the worker for a reload.
    struct keymap_job {
        lx::config::keyboard_settings settings{};
        lx::input::keyboard_keymap keymap{};
        char error[192]{};
        bool ok = false;
        unsigned compile_us = 0;
    };
    std::unique_ptr<keymap_job> keymap_job_{};
    /// Cursor themes read on the worker for a reload (one per scale in use).
    struct cursor_job {
        char theme[64]{};
        unsigned base_px = 24;
        unsigned count = 0;
        unsigned scale120[k_cursor_scales]{};
        xcursor_theme themes[k_cursor_scales]{};
    };
    std::unique_ptr<cursor_job> cursor_job_{};
    /// Why the configured keymap does not compile; empty while it is the one in use. Each
    /// generation reports it (and an accepted one compiles it again: a layout installed
    /// since then is picked up), so the problem stays visible until it is fixed.
    char keymap_error_[192]{};
    /// The keymap in use is the system default (the configured one failed at start).
    bool keymap_default_ = false;
    /// Adds the configured keymap's compile error to this generation's diagnostics.
    void report_keymap_error();
    /// The matcher's table (config bindings minus unavailable ones).
    std::inplace_vector<binding_entry, keybindings::k_max_bindings> binding_table_{};
    /// A binding with `repeat` held: it fires again at the keyboard's repeat rate.
    std::unique_ptr<runtime::timer_source> binding_repeat_timer_{};
    const binding_entry* repeat_binding_ = nullptr;
    binding_entry repeat_entry_{};
    unsigned repeat_code_ = 0;
    /// The repeating key is a held action's (`key` again at the repeat rate) or a keyboard
    /// move/resize's (the window manager's step again), not `repeat_binding_`.
    enum class key_repeat : unsigned char { binding, hold, wm };
    key_repeat repeat_kind_ = key_repeat::binding;
    key_input repeat_in_{};
    lx::input::key_event repeat_event_{};
    /// Starts the repeat of `kind` for the key of `event` at the keyboard's repeat delay.
    void start_key_repeat(key_repeat kind, const lx::input::key_event& event, const key_input& in);
    /// A held action (design-session-and-wm K.1): `key` with the binding it would run.
    void send_hold_key(const key_input& in, unsigned time_ms);
    /// Ends the hold without the user letting go (lock, VT switch, a grab): `cancelled`.
    void cancel_hold(const char* why);
    /// Cursor theme name from the configuration ("" = $XCURSOR_THEME).
    char cursor_theme_name_[64]{};

    enum class reload_cause : unsigned char { watch, sighup, control, action, start };
    /// Before the input and the cursor are set up: locate, read, parse (generation 1).
    void load_initial_config();
    /// After the protocols exist: bindings, focus, repeat, the device hook.
    void apply_initial_config();
    void request_config_reload(reload_cause cause, unsigned control_token);
    void run_config_reload(bool force);
    void start_keymap_job();
    void finish_keymap_job();
    void start_cursor_job();
    void finish_cursor_job();
    void config_job_done();
    /// The generation is applied (or rejected): log, tell the shell, the control socket,
    /// systemd; run a queued reload.
    void finish_config_generation();
    void install_bindings();
    void apply_focus_policy();
    /// config.toml [windows]: edge snapping, its preview, drag-to-restore (Part K).
    void apply_window_config();
    void apply_repeat();
    /// zlm v7 config + diagnostics + done to `shell_resource` (null: every v7 shell).
    void send_config_to_shell(void* shell_resource, bool with_bytes);
    void export_cursor_environment();
    /// Executes a binding's or lumen-ctl's action. `input`: a key or button triggered it
    /// (the shell gets an input serial). `message` explains an invalid_argument.
    control_action_result run_action(lx::config::action_id action, unsigned argument,
                                     unsigned time_ms, bool input, const char* source,
                                     const char*& message);
    void stop_binding_repeat();
    static void config_watch_callback();
    static void config_timer_callback();
    static void binding_repeat_callback();
    [[nodiscard]] wm_button_binding on_button_binding(unsigned button, unsigned time_ms);
    /// One timer for the session's bounded holds: the zone hold (5 s after the shell went)
    /// and the placement hold (750 ms after READY while no shell has bound).
    std::unique_ptr<runtime::timer_source> session_timer_{};
    static constexpr lx::runtime::clock_time k_zone_hold_ns = 5'000'000'000ll;
    static constexpr lx::runtime::clock_time k_placement_hold_ns = 750'000'000ll;
    lx::runtime::clock_time zone_hold_until_ = 0;
    lx::runtime::clock_time placement_hold_until_ = 0;
    /// Windows that mapped during the placement hold, fitted into the usable area once the
    /// shell's zones are known.
    std::inplace_vector<lx::toplevel_id, 16> placement_held_{};
    /// The health level last streamed to subscribers.
    lx::health::level streamed_health_ = lx::health::level::unknown;

    /// `start`: the control socket, the environment export and READY.
    void start_session();
    void on_environment_exported(bool ok);
    void send_ready(const char* why);
    /// `run`, after the loop: the environment unset, the control socket removed.
    void end_session();
    /// After each dispatch: the watchdog ping when due (cheap when not).
    void maybe_ping_watchdog(lx::runtime::clock_time now);
    /// Until the control source must run without its fd (the next watchdog ping); -1 none.
    [[nodiscard]] lx::runtime::clock_time control_timeout_ns(lx::runtime::clock_time now) const;
    void handle_reload_request();
    /// The control host: GetHealth asks the render thread, then answers.
    void begin_health(unsigned token);
    void answer_health(unsigned token, bool render_answered, unsigned render_ms,
                       unsigned cache_used, unsigned cache_capacity, unsigned render_us,
                       unsigned long long render_over);
    void fill_control_window(const toplevel_record& rec, control_window& out) const;
    /// Every workspace for the control socket (ListWorkspaces); returns how many.
    unsigned fill_control_workspaces(control_workspace* out, unsigned cap) const;
    /// The workspaces changed (the window manager's observer): `workspace` /
    /// `workspace-removed` events for what differs from the last ones streamed.
    void publish_workspaces();
    /// What the control stream last said of each workspace (while it has subscribers).
    control_workspace published_ws_[k_max_workspaces]{};
    unsigned published_ws_count_ = 0;
    /// config.toml [workspaces] to the window manager.
    void apply_workspace_config();
    [[nodiscard]] control_action_result run_control_action(const char* action,
                                                          const char* argument,
                                                          const char*& message);
    void on_shell_bound(void* client_native);
    void rearm_session_timer();
    static void session_timer_callback();
    /// The placement hold ended: held windows are fitted into their usable area.
    void end_placement_hold(const char* why);
    /// Bring up CPU-writable dumb-buffer scanout. Requires DRM master.
    [[nodiscard]] lx::result<void> setup_cpu_present();
    /// Bring up EGL/GBM for the GL composite (targets per output from `create_targets`).
    [[nodiscard]] lx::result<void> setup_gl_present();
    /// UI, at start: the CRTC allocation (`drm::assign_crtcs`, each candidate checked by a
    /// TEST_ONLY of the full state with the real framebuffers) and one pipeline per output.
    [[nodiscard]] lx::result<void> create_pipelines();
    /// UI, at start: the pipelines' cursor planes.
    void create_cursor_planes(unsigned need_px);
    /// Render. Composites output `p` from `snapshot` (its view's, acquired) and releases
    /// it; true when it drew.
    bool composite_output(output_pipeline& p, unsigned slot,
                          const scene::immutable_frame_snapshot& snapshot, unsigned& rects,
                          unsigned long long& area);
    /// Render. Commits `p`'s composited frame (and its cursor), or a cursor-only commit.
    const char* commit_output(output_pipeline& p, unsigned slot, bool composited);
    /// Render, once `p` has composited nothing for `k_catch_up_idle_ns` with nothing in
    /// flight (GL): every slot off screen whose age covers at least a quarter of the output
    /// is brought up to date from the one on screen by a copy, so the busy frame that next
    /// draws into it — the third slot is first drawn into by a frame composited while a flip
    /// is pending — repaints only its own damage rather than the whole output. Not between
    /// the frames of an animation: those repaint the same region anyway.
    void catch_up_slots(output_pipeline& p);
    /// Render. `p` is in a state a catch-up may run in, and some slot's age is large enough;
    /// `front` receives the slot on screen. Owed catch-ups keep render from reporting quiet,
    /// so the idle housekeeping tick runs it.
    [[nodiscard]] bool catch_up_owed(const output_pipeline& p, unsigned& front) const;
    static constexpr lx::runtime::clock_time k_catch_up_idle_ns = 200'000'000;
    /// Render. A copy failed once: no more catch-ups (the frames repaint by buffer age).
    bool catch_up_failed_ = false;
    /// Render. Every pipeline that needs a modeset and has a frame: one combined commit (M5).
    /// `kinds[slot]` receives what each pipeline committed.
    void commit_pending_modesets(const char** kinds);

    static compositor_impl* active;
    static void vsync_callback();
    static void wayland_fd_callback();
    static void content_fd_callback();
    /// commit-timing-v1: clocks of outputs with a queued target that stopped are armed for
    /// the tick presenting it.
    void arm_commit_timers();
    /// tearing-control-v1: the output policy's verdict for the outputs just published.
    void evaluate_tearing(std::uint32_t published);
    /// Per output: the last `allow_async` verdict (what an async-capable commit path reads).
    std::uint32_t async_allowed_mask_ = 0;
    bool async_unsupported_logged_ = false;
    /// Render: hands a dup of this composite's fence to explicit-sync release points (only
    /// while some client uses linux-drm-syncobj-v1).
    void publish_release_fence(int fence_fd, unsigned frame) {
        if (fence_fd < 0 || !release_fences_.wanted())
            return;
        const int dup = ::fcntl(fence_fd, F_DUPFD_CLOEXEC, 0);
        if (dup >= 0)
            release_fences_.publish(dup, frame);
    }
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
    // A repeating binding (or held-action key, or keyboard-move arrow) stops when its key
    // goes up or another key goes down.
    if (repeat_code_ && (event.pressed || event.evdev_keycode == repeat_code_))
        stop_binding_repeat();

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
    // Super+Escape, when it acts: the focused surface's shortcuts inhibitor is toggled
    // (keyboard-shortcuts-inhibit) and an active pointer lock or confinement is released
    // (pointer-constraints) — one key gives a VM viewer's or a game's captured keyboard and
    // pointer back. Never inhibitable, like the bindings above; a surface with neither gets
    // the key.
    if (keybindings::escape_hatch(in)) {
        const bool toggled = shortcuts_inhibit_toggle(p0_);
        const bool released = pointer_constraints_release(p0_);
        if (toggled || released) {
            keys_.reset();
            return;
        }
    }
    // The waking press: not forwarded, so the router swallows its release too (Enter that
    // wakes the screen does not also confirm a dialog — KWin's DpmsInputEventFilter).
    if (woke && event.pressed) {
        keys_.reset();
        return;
    }
    // 2. Keys the window manager claims (Escape during a grab or drag; every press during a
    //    keyboard move or resize, whose arrows repeat).
    if (wm_.on_key(event)) {
        keys_.reset();
        if (event.pressed && wm_.grab_active() && wm_.grab().keyboard)
            start_key_repeat(key_repeat::wm, event, in);
        return;
    }
    // 2b. A held action (zlm v7, design-session-and-wm K.1): while the binding's modifier is
    //     down every key press goes to the shell's hold instead of the focused client — which
    //     keeps the keyboard focus and sees the modifiers as they are. Letting go of the
    //     modifier ends it; that release still reaches the client, which saw the press.
    if (keys_.hold_active()) {
        switch (keys_.on_hold_key(in)) {
        case keybindings::hold_step::divert:
            send_hold_key(in, event.time_ms);
            start_key_repeat(key_repeat::hold, event, in);
            return;
        case keybindings::hold_step::released:
            if (void* hold = keys_.end_hold(); hold && p0_.send_hold_released)
                p0_.send_hold_released(hold, event.time_ms);
            if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.bindings",
                                                "held action released");
            break;
        case keybindings::hold_step::none: break;
        }
    }
    // 3. The configured bindings (config.toml [bindings]), then the shell's legacy grabs
    //    (zlm v4): never during a move, resize or drag, never while paused; while locked only
    //    `allow-when-locked` bindings; while the focused surface inhibits shortcuts, only
    //    `allow-when-inhibited` bindings and `not_inhibitable` grabs.
    const bool wm_busy = wm_.grab_active() || wm_.drag_active();
    const bool inhibited = shortcuts_inhibited(p0_);
    if (!wm_busy && display_gate_.load(std::memory_order_relaxed) == 0) {
        if (const key_decision d = keys_.match_binding(in, inhibited); d.consume) {
            // A copy: the action may replace the table (session.reload-config).
            const binding_entry b = *d.binding;
            // A held action (Alt+Tab): the shell gets a hold for as long as the binding's
            // modifier stays down (K.1). With no v7 shell it runs as an ordinary action.
            if (const auto& info = lx::config::action(b.action);
                info.held && keys_.shell_actions() && p0_.send_held_action) {
                if (void* hold = p0_.send_held_action(info.name, "", event.time_ms)) {
                    keys_.begin_hold(b.keys.mods, hold);
                    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                        lx::trace::logger::global().logf(lx::trace::level::debug,
                                                         "compositor.bindings",
                                                         "held action {} begins (modifier 0x{:x})",
                                                         info.name, keys_.hold_mods());
                    return;
                }
            }
            const char* message = "";
            (void)run_action(b.action, b.argument, event.time_ms, true, "binding", message);
            const auto& kb = config_.current().keyboard;
            if ((b.options & lx::config::bo_repeat) && binding_repeat_timer_ &&
                kb.repeat_rate > 0) {
                repeat_entry_ = b;
                repeat_binding_ = &repeat_entry_;
                repeat_kind_ = key_repeat::binding;
                repeat_code_ = event.evdev_keycode;
                binding_repeat_timer_->schedule_at(
                    now_ns() + static_cast<runtime::clock_time>(kb.repeat_delay) * 1'000'000ll);
                binding_repeat_timer_->set_enabled(true);
            }
            return;
        }
        if (const key_decision d = keys_.match_grab(in, inhibited); d.consume) {
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
    void* tap = keys_.observe_tap(in, inhibited);
    input_router_.send_key(event, seat);
    if (!tap || wm_busy)
        return;
    if (const binding_entry* b = keys_.tap_binding(tap)) {
        const char* message = "";
        (void)run_action(b->action, b->argument, event.time_ms, true, "binding", message);
    } else if (p0_.fire_key_grab) {
        p0_.fire_key_grab(tap, event.time_ms);
    }
}

void compositor_impl::request_vt_switch(unsigned vt) {
    if (auto switched = logind_.switch_vt(vt); !switched) {
        lx::trace::logger::global().log_error(switched.get_error(), "compositor.session");
        return;
    }
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                     "VT switch to {:d} requested", vt);
}

void compositor_impl::note_committed_input(output_pipeline& p, bool content) {
    const cursor_frame& f = cursor_cur_;
    const int fx = static_cast<int>(std::floor(f.x * 256.0));
    const int fy = static_cast<int>(std::floor(f.y * 256.0));
    const bool changed = fx != latency_cursor_x_ || fy != latency_cursor_y_ ||
                         f.image_generation != latency_cursor_gen_ ||
                         f.visible != latency_cursor_visible_;
    latency_cursor_x_ = fx;
    latency_cursor_y_ = fy;
    latency_cursor_gen_ = f.image_generation;
    latency_cursor_visible_ = f.visible;
    if (!changed)
        return;
    const unsigned long long in = f.input_time_us;
    if (in == 0 || in <= latency_reported_input_us_)
        return;
    latency_reported_input_us_ = in;
    unsigned long long expected = 0;
    (void)p.committed_input_us.compare_exchange_strong(expected, (in << 1) | (content ? 1u : 0u),
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

/// The control socket's sd-event loop (and the user bus attached to it) as one source of
/// the UI loop: its epoll fd, plus a deadline only when the watchdog's next ping is due and
/// nothing else woke the loop first (on an idle desktop the 1 Hz housekeeping tick does).
class control_source final : public lx::runtime::event_source {
public:
    explicit control_source(compositor_impl& impl) : impl_{&impl} {}
    bool prepare(lx::runtime::clock_time& timeout) override {
        if (ready_ || impl_->control_.kicked()) {
            timeout = 0;
            return true;
        }
        timeout = impl_->control_timeout_ns(compositor_impl::now_ns());
        return timeout == 0;
    }
    bool check() override { return ready_ || impl_->control_.kicked(); }
    bool dispatch() override {
        ready_ = false;
        impl_->control_.dispatch();
        return true;
    }
    [[nodiscard]] int poll_fd() const override { return impl_->control_.fd(); }
    void on_readable() override { ready_ = true; }

private:
    compositor_impl* impl_;
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

void compositor_impl::publish_commit_timing(output_pipeline& p, int fence_fd) {
    if (lx::trace::logger::global().min_level() > lx::trace::level::debug)
        return;
    const int dup_fd = fence_fd >= 0 ? ::fcntl(fence_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (const int old = p.committed_fence_fd.exchange(dup_fd, std::memory_order_acq_rel); old >= 0)
        ::close(old);
    p.committed_submit_us.store(monotonic_us(), std::memory_order_release);
}

void compositor_impl::retract_commit_timing(output_pipeline& p) {
    p.committed_submit_us.store(0, std::memory_order_release);
    if (const int old = p.committed_fence_fd.exchange(-1, std::memory_order_acq_rel); old >= 0)
        ::close(old);
}

void compositor_impl::note_present_latency(output_pipeline& p, unsigned long long present_ns,
                                           unsigned long long submit_us,
                                           unsigned long long fence_us) {
    const unsigned long long packed = p.committed_input_us.exchange(0, std::memory_order_acquire);
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
    lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.latency",
                                     "present kind={} input_us={} present_us={} latency_us={} "
                                     "submit_us={} fence_us={} output={}",
                                     (packed & 1u) ? "content" : "cursor", input_us, present_us,
                                     latency_us, submit_us, fence_us, p.slot);
    latency_samples_.push_back(latency_us);
    if (latency_samples_.size() == k_latency_window)
        log_latency_summary();
}

void compositor_impl::log_outputs() const {
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        if (!outputs.slot_live(slot))
            continue;
        const output_state& o = outputs.slot_state(slot);
        const lx::rect2i l = o.logical_rect();
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.output",
                                         "output {} name={} mode={}x{} refresh_mhz={} scale={:g} "
                                         "({}/120) logical={}x{}+{}+{} identifier=\"{}\"",
                                         slot, o.name, o.mode.width, o.mode.height, o.refresh_mhz,
                                         o.scale, std::lround(o.scale * 120.0), l.width, l.height,
                                         l.x, l.y, o.identifier);
    }
}

void compositor_impl::sync_output_views() {
    // UI. Views, clocks and per-output state follow the output manager: a live output gets
    // its view (projection, full damage on change), its frame clock at its refresh, and its
    // render pipeline's projection; a gone one loses them.
    bool changed = false;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        output_ui& o = outs_[s];
        if (!outputs.slot_live(s)) {
            if (!o.live)
                continue;
            scene.remove_output_view(s);
            clocks_.set_live(s, false, 0);
            o = output_ui{};
            changed = true;
            continue;
        }
        const output_state& st = outputs.slot_state(s);
        const lx::rect2i l = st.logical_rect();
        const bool fresh = !o.live || o.id != st.id;
        if (fresh) {
            // The pipeline handle is set by whoever made the pipeline (start, an add's
            // answer) and survives the reset.
            output_pipeline* const pipe = o.pipe;
            o = output_ui{};
            o.pipe = pipe;
            o.live = true;
            o.id = st.id;
            const unsigned mhz = st.refresh_mhz > 0 ? st.refresh_mhz : 60000u;
            clocks_.set_live(s, true,
                             static_cast<lx::runtime::clock_time>(1'000'000'000'000.0 / mhz));
            clocks_.set_vrr(s, st.adaptive_sync);
            changed = true;
            o.refresh_mhz = st.refresh_mhz;
        } else if (st.refresh_mhz != o.refresh_mhz) {
            // A new mode (a refresh-only change too: 2560x1440@60 to @144): its refresh is
            // the clock's period now.
            o.refresh_mhz = st.refresh_mhz;
            const unsigned mhz = st.refresh_mhz > 0 ? st.refresh_mhz : 60000u;
            clocks_.clock(s).set_refresh_period(
                static_cast<lx::runtime::clock_time>(1'000'000'000'000.0 / mhz));
        }
        if (fresh || l.x != o.logical.x || l.y != o.logical.y || l.width != o.logical.width ||
            l.height != o.logical.height || st.scale != o.scale ||
            st.mode.width != o.mode.width || st.mode.height != o.mode.height ||
            st.transform != o.transform) {
            o.logical = l;
            o.scale = st.scale;
            o.mode = st.mode;
            o.transform = st.transform;
            // Layout → this output's pixels, applied where draws and damage are made. Every
            // pixel of the view moves with it (`set_output_view` notes full damage).
            const auto transform = static_cast<lx::buffer_transform>(st.transform & 7u);
            scene::output_view_config v{};
            v.logical = l;
            v.projection = {{l.x, l.y}, st.scale, st.mode, transform};
            scene.set_output_view(s, v);
            changed = true;
            // Render's copy of the projection (the cursor plane's position on this output).
            if (output_pipeline* p = o.pipe) {
                const lx::point2i origin{l.x, l.y};
                const double scale = st.scale;
                if (threads_started.load(std::memory_order_acquire)) {
                    // By slot, looked up on render, never the pointer: the output may be
                    // retired, and its pipeline freed on UI, before render drains this task
                    // (a dock unplug re-arranges the second monitor, then removes it in the
                    // same scan). The task is drained no later than the render iteration
                    // that runs the retire, and a slot is reused only after that, so the
                    // pipeline found is this output's or none.
                    compositor_impl* self = this;
                    (void)runtime::executor::global().post(
                        runtime::affinity::render, [self, s, origin, scale, transform] {
                            output_pipeline* rp = self->pipes_[s].get();
                            if (!rp || rp->retiring)
                                return;
                            rp->origin = origin;
                            rp->scale = scale;
                            if (rp->transform != transform) {
                                rp->transform = transform;
                                rp->cursor_staged_gen = 0;
                            }
                        });
                } else {
                    p->origin = origin;
                    p->scale = scale;
                    p->transform = transform;
                }
            }
        }
    }
    applied_layout_generation_ = outputs.layout_generation();
    if (!changed)
        return;
    // Layer surfaces are arranged against their outputs, and the usable areas with them;
    // window decorations follow their outputs' scales.
    wm_.on_output_changed();
    request_frame();
}

void compositor_impl::log_latency_summary() {
    const unsigned n = static_cast<unsigned>(latency_samples_.size());
    if (n == 0)
        return;
    // Exact percentiles over the fixed window, in place: nth_element allocates nothing.
    unsigned long long* b = latency_samples_.data();
    const unsigned i50 = (n - 1) / 2;
    const unsigned i95 = (n - 1) * 95 / 100;
    std::nth_element(b, b + i95, b + n);
    const unsigned long long p95 = b[i95];
    std::nth_element(b, b + i50, b + i95 + 1);
    const unsigned long long p50 = b[i50];
    const unsigned long long max = *std::max_element(b + i95, b + n);
    latency_samples_.clear();
    lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.latency",
                                     "latency summary n={} p50_us={} p95_us={} max_us={}", n, p50,
                                     p95, max);
}

void compositor_impl::on_input_modifiers(const lx::input::modifier_state& mods) {
    input_router_.send_modifiers(mods, input_devices_.default_seat());
    // Caps Lock and Num Lock light their LEDs (only when the lit set changed).
    input_devices_.update_leds();
    // During a drag the modifiers choose the user's action.
    wm_.on_modifiers();
}

void compositor_impl::prepare_cursor_plane(output_pipeline& p) {
    if (!p.kms || !p.cursor.valid())
        return;
    // Before the first content commit the plane joins the modeset only once it has passed
    // its test (a resume); at startup it is stated off there and joins the next commit.
    if ((!p.content_committed_once && !p.cursor_tested) || p.kms->flip_pending())
        return;
    if (display_gate_.load(std::memory_order_acquire) != 0 ||
        (gate_power_mask_.load(std::memory_order_acquire) & (1u << p.slot)) != 0)
        return;

    const bool failed = cursor_hw_failed_.load(std::memory_order_relaxed);
    bool want = !failed && cursor_cur_.visible && cursor_cur_.hardware;
    const unsigned scale120 = static_cast<unsigned>(std::lround(p.scale * 120.0));
    const cursor_image_set& set = cursor_images_.front();
    const cursor_plane_image* img = set.for_scale(scale120);
    // A turned output (ADR 008 §C.4) shows the image turned the same way, its hotspot with it:
    // turned once per image change, never per motion.
    const lx::buffer_transform t = p.transform;
    const bool turned = t != lx::buffer_transform::normal;
    unsigned img_w = img ? img->width : 0;
    unsigned img_h = img ? img->height : 0;
    int hot_x = img ? img->hot_x : 0;
    int hot_y = img ? img->hot_y : 0;
    if (img && turned) {
        if ((static_cast<unsigned>(t) & 1u) != 0) {
            img_w = img->height;
            img_h = img->width;
        }
        const lx::rect2i h = gfx::transform_rect({img->hot_x, img->hot_y, 1, 1}, t,
                                                 static_cast<int>(img->width),
                                                 static_cast<int>(img->height));
        hot_x = h.x;
        hot_y = h.y;
    }
    if (want && img && set.generation != 0 && set.generation != p.cursor_staged_gen) {
        const std::uint32_t* pixels = img->pixels.data();
        if (turned) {
            const std::size_t px_count = static_cast<std::size_t>(img->width) * img->height;
            if (p.cursor_turned.size() < px_count)
                p.cursor_turned.resize(px_count);
            for (unsigned y = 0; y < img->height; ++y)
                for (unsigned x = 0; x < img->width; ++x) {
                    const lx::rect2i d = gfx::transform_rect(
                        {static_cast<int>(x), static_cast<int>(y), 1, 1}, t,
                        static_cast<int>(img->width), static_cast<int>(img->height));
                    p.cursor_turned[static_cast<std::size_t>(d.y) * img_w + static_cast<unsigned>(d.x)] =
                        img->pixels[static_cast<std::size_t>(y) * img->width + x];
                }
            pixels = p.cursor_turned.data();
        }
        if (auto staged = p.cursor.stage_image(pixels, img_w, img_h, img_w, hot_x, hot_y); staged) {
            p.cursor_staged_gen = set.generation;
        } else {
            fail_cursor_plane("image does not fit the plane");
            want = false;
        }
    }
    if (p.cursor_staged_gen == 0 || !img)
        want = false; // nothing staged yet: do not show an empty buffer
    // The hotspot's pixel on this output; the plane is on only where its image reaches the
    // output — a pointer straddling two outputs shows a half on each (wlroots).
    int px = static_cast<int>(std::floor((cursor_cur_.x - p.origin.x) * p.scale));
    int py = static_cast<int>(std::floor((cursor_cur_.y - p.origin.y) * p.scale));
    if (turned) {
        const bool swap = (static_cast<unsigned>(t) & 1u) != 0;
        const int cw = swap ? p.mode.height : p.mode.width;
        const int ch = swap ? p.mode.width : p.mode.height;
        const lx::rect2i d = gfx::transform_rect({px, py, 1, 1}, t, cw, ch);
        px = d.x;
        py = d.y;
    }
    if (want && img) {
        const int x0 = px - hot_x;
        const int y0 = py - hot_y;
        want = x0 < p.mode.width && y0 < p.mode.height &&
               x0 + static_cast<int>(img_w) > 0 && y0 + static_cast<int>(img_h) > 0;
    }
    p.cursor.set_visible(want);
    p.cursor.set_position(px, py);

    // Validate once, before the plane's state rides a content commit: a rejection there
    // would fail the frame too. TEST_ONLY applies nothing and raises no event.
    if (want && !p.cursor_tested && p.cursor.dirty()) {
        drm::atomic_commit_request test{};
        test.update_primary = false;
        test.cursor = &p.cursor;
        test.request_page_flip = false;
        test.test_only = true;
        if (auto ok = p.kms->commit(test); ok) {
            p.cursor_tested = true;
        } else if (p.kms->last_failure_rejected_state()) {
            lx::trace::logger::global().log_error(ok.get_error(), "compositor.cursor");
            fail_cursor_plane("TEST_ONLY commit rejected");
            p.cursor.set_visible(false);
        }
        // Otherwise the device was unavailable (DRM master on another VT): that says
        // nothing about the plane, so the test runs again on a later tick.
    }
}

void compositor_impl::fail_cursor_plane(const char* why) {
    if (cursor_hw_failed_.exchange(true, std::memory_order_acq_rel))
        return;
    lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.cursor",
                                     "cursor plane rejected: {} — falling back to software", why);
}

void compositor_impl::on_input_pointer_motion(const lx::input::pointer_motion_event& event) {
    lx::trace::scoped_span span{"input", "pointer_motion"};
    // Activity; motion that wakes the display is delivered all the same (I5).
    (void)on_user_activity();
    auto& seat = input_devices_.default_seat();
    // Where the pointer was before this event: lx.input has already added a relative
    // device's delta to the seat position; an absolute device's position is set below.
    const double from_x = event.absolute ? seat.pointer_x() : seat.pointer_x() - event.dx;
    const double from_y = event.absolute ? seat.pointer_y() : seat.pointer_y() - event.dy;
    // relative-pointer: the device's own motion, to the pointer focus's client, before the
    // pointer moves (or does not: a lock still yields it). An absolute device's delta is the
    // change of its position in layout units, accelerated and unaccelerated alike (sway).
    double rdx = event.dx;
    double rdy = event.dy;
    double rdxu = event.dx_unaccel;
    double rdyu = event.dy_unaccel;
    if (event.absolute) {
        rdx = rdy = 0.0;
        if (event.has_prev && outputs.count() > 0) {
            const lx::rect2i bounds = layout_bounds(outputs);
            rdx = (event.x - event.prev_x) * bounds.width;
            rdy = (event.y - event.prev_y) * bounds.height;
        }
        rdxu = rdx;
        rdyu = rdy;
    }
    const bool relative_sent =
        (rdx != 0.0 || rdy != 0.0 || rdxu != 0.0 || rdyu != 0.0) &&
        relative_pointer_motion(p0_,
                                event.time_us ? event.time_us
                                              : static_cast<unsigned long long>(event.time_ms) * 1000ull,
                                rdx, rdy, rdxu, rdyu);
    // Output geometry is the compositor's to know — libinput reports deltas and has no
    // idea how large the desktop is — so both halves of the mapping live here. An absolute
    // device (a tablet, or the mouse of any VM console) arrives normalised to a unit box
    // and is scaled into the output; whatever the source, the pointer is then kept on
    // screen. lx.input accumulates only relative motion, which is why absolute devices
    // used to leave the pointer at the origin for the whole session.
    // Layout (logical) coordinates: the pointer moves in logical units at any scale
    // (wlroots, Mutter). An absolute device covers the layout's bounding box (the VMware
    // mouse spans the host window, which is the bounding box of the guest topology), and
    // the pointer stays on the union of the outputs: the nearest point of the nearest
    // output when it would leave them or fall into a gap (ADR 008 M9).
    if (outputs.count() > 0 && event.absolute) {
        const lx::rect2i bounds = layout_bounds(outputs);
        seat.set_pointer_position(bounds.x + event.x * bounds.width,
                                  bounds.y + event.y * bounds.height);
    }
    // pointer-constraints: a lock keeps the pointer where it was — no cursor move, no
    // hit-test, no `wl_pointer.motion`; the relative motion above still went out, and its
    // frame ends here. A confinement clips the motion into its region.
    {
        double x = seat.pointer_x();
        double y = seat.pointer_y();
        const pointer_constraint_kind k = pointer_constraint_motion(p0_, from_x, from_y, x, y);
        if (k == pointer_constraint_kind::locked) {
            seat.set_pointer_position(from_x, from_y);
            if (relative_sent)
                input_router_.send_pointer_frame();
            return;
        }
        if (k == pointer_constraint_kind::confined)
            seat.set_pointer_position(x, y);
    }
    if (outputs.count() > 0) {
        double x = seat.pointer_x();
        double y = seat.pointer_y();
        if (closest_point(outputs, x, y) && (x != seat.pointer_x() || y != seat.pointer_y()))
            seat.set_pointer_position(x, y);
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
    // A constraint may hold now that the pointer is inside its region.
    pointer_constraints_update(p0_);
}

void compositor_impl::warp_pointer(double x, double y) {
    auto& seat = input_devices_.default_seat();
    if (outputs.count() > 0)
        (void)closest_point(outputs, x, y);
    seat.set_pointer_position(x, y);
    cursor_.on_motion(x, y, 0, wm_.grab_active() || wm_.drag_active());
    wm_.on_pointer_motion(static_cast<unsigned>(now_ns() / 1'000'000ull));
}

void compositor_impl::on_input_gesture(const lx::input::gesture_event& event) {
    (void)on_user_activity();
    keys_.note_other_input();
    // pointer-gestures: to the pointer focus at begin, latched until the end (the wire
    // decides; a compositor gesture would claim it there).
    pointer_gesture(p0_, event);
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
    // A press on a window during a held action (the switcher is up) is the user going
    // elsewhere: the hold is cancelled, as GNOME's switcher closes on an outside click. A
    // press on the shell's own surfaces (the switcher) is the shell's to judge.
    if (event.pressed && keys_.hold_active() && wm_.pointer_focus())
        cancel_hold("click elsewhere");
    // A button that starts or ends a grab (or ends a drag) changes the pointer image without
    // moving it: re-publish the same position stamped with this event, so latency is
    // measured from the button and not from the last motion.
    const bool grab_after = wm_.grab_active() || wm_.drag_active();
    if (event.time_us != 0 && grab_before != grab_after) {
        auto& seat = input_devices_.default_seat();
        cursor_.on_motion(seat.pointer_x(), seat.pointer_y(), event.time_us, grab_after);
    }
    // A press may have started a grab or opened a popup grab (a constraint yields), or
    // focused the window under the pointer (one may hold now).
    pointer_constraints_update(p0_);
}

void compositor_impl::on_input_pointer_axis(const lx::input::pointer_axis_event& event) {
    (void)on_user_activity(); // a scroll that wakes the display is delivered too
    keys_.note_other_input();
    if (event.scroll_factor == 1.0) {
        wm_.on_pointer_axis(event);
        return;
    }
    // config.toml scroll-factor (per device): the values scale; a wheel's discrete steps
    // scale and round, and a step that rounds to none is sent as continuous scrolling only
    // (axis_discrete must be a whole number of notches).
    lx::input::pointer_axis_event scaled = event;
    const double f = event.scroll_factor;
    scaled.vertical *= f;
    scaled.horizontal *= f;
    scaled.vertical_discrete = std::round(event.vertical_discrete * f);
    scaled.horizontal_discrete = std::round(event.horizontal_discrete * f);
    wm_.on_pointer_axis(scaled);
}

void compositor_impl::trace_texture_op(const char* op, unsigned texture_id, bool ok) {
    if (texture_trace_count_ >= k_texture_trace_limit)
        return;
    ++texture_trace_count_;
    lx::trace::logger::global().logf(
        lx::trace::level::info, "compositor.texture", "texture {} id={}{}{}", op, texture_id,
        ok ? "" : " FAILED",
        texture_trace_count_ == k_texture_trace_limit ? " (trace ends here)" : "");
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

void compositor_impl::begin_input() {
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

    // The configured layout (config.toml [input.keyboard]); the snapshot does not change
    // before the loop runs, so the thread may read it.
    const lx::input::keymap_config keymap = keymap_config_of(config_.current().keyboard);
    const bool num_lock = config_.current().keyboard.numlock;
    const bool configured = keymap.layout || keymap.variant || keymap.options || keymap.model ||
                            keymap.rules;
    input_open_thread_ = std::thread([this, provider, keymap, num_lock, configured] {
        // Compile a keymap regardless of whether devices open. Clients are told the layout
        // at bind time, and a compositor that never found a keyboard still has to answer it.
        lx::input::keyboard_keymap km;
        if (auto compiled = km.compile(keymap, keymap_error_, sizeof(keymap_error_));
            !compiled) {
            if (!configured)
                lx::trace::logger::global().log_error(compiled.get_error(), "compositor.input");
            // A configured layout that does not compile: the system default (reported with
            // the configuration's first generation).
            else {
                if (!keymap_error_[0])
                    std::snprintf(keymap_error_, sizeof(keymap_error_),
                                  "xkbcommon gave no reason");
                if (auto fallback = km.compile(); !fallback)
                    lx::trace::logger::global().log_error(fallback.get_error(),
                                                          "compositor.input");
            }
        } else {
            keymap_error_[0] = '\0';
        }
        if (num_lock)
            (void)km.set_num_lock(true);
        input_opened_.emplace(
            lx::input::input_manager::open(provider, "seat0", keymap, num_lock, &km));
        // Not taken (no seat): the keymap the device-less compositor still answers with.
        if (km.valid())
            input_devices_.default_seat().keymap() = std::move(km);
    });
}

void compositor_impl::finish_input() {
    if (input_open_thread_.joinable())
        input_open_thread_.join();
    if (!input_opened_)
        return;
    auto opened = std::move(*input_opened_);
    input_opened_.reset();
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
    sink.gesture = [](const lx::input::gesture_event& e, void* u) {
        static_cast<compositor_impl*>(u)->on_input_gesture(e);
    };
    // Each device once, as it appears (start, hotplug, VT resume): what it can do decides
    // which protocols it can drive — gestures need a touchpad, relative pointers a mouse.
    sink.device_added = [](const char* name, bool keyboard, bool pointer, bool touch, bool gesture,
                           void*) {
        lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.input",
                                         "input device \"{}\":{}{}{}{}", name ? name : "?",
                                         keyboard ? " keyboard" : "", pointer ? " pointer" : "",
                                         touch ? " touch" : "", gesture ? " gesture" : "");
    };
    // Touch is not routed to clients yet; it is still the user, and wakes the display.
    sink.touch = [](const lx::input::touch_event&, void* u) {
        (void)static_cast<compositor_impl*>(u)->on_user_activity();
    };
    input_devices_.set_sink(sink);
    // Pointer devices get the configuration's libinput settings as they appear (and again
    // on a reload): class sections, then a [[input.device]] of that name.
    input_devices_.set_device_config(
        [](void* u, const char* name, lx::input::device_class kind, lx::input::pointer_config& out) {
            out = pointer_config_of(static_cast<compositor_impl*>(u)->config_.current(), name, kind);
            return true;
        },
        [](void*, const char* name, lx::input::device_class kind,
           const lx::input::apply_report& r) {
            lx::trace::logger::global().logf(lx::trace::level::info, "compositor.input",
                                             "input device \"{}\" ({}): {}", name ? name : "?",
                                             lx::input::device_class_name(kind),
                                             r.applied[0] ? r.applied : "nothing to set");
            if (r.unsupported_count &&
                lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.input",
                                                 "input device \"{}\" does not support: {}",
                                                 name ? name : "?", r.unsupported);
        },
        this);

    // Share the keymap the device layer compiled, so the state the compositor tracks and
    // the state clients are told about are the same one.
    if (const int fd = input_devices_.fd(); fd >= 0) {
        input_fd_source = std::make_unique<runtime::fd_source>(
            fd, &compositor_impl::input_ready_callback);
        loop.add_source(input_fd_source.get());
        input_ready_ = true;
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.input",
                                        "libinput seat opened");
        // The seat's devices were added while it was assigned (on the opening thread):
        // their events wait in libinput's queue, which the fd does not signal. Taken now,
        // not at the first key press — the keyboard LEDs and the device count (health)
        // are right from the start.
        (void)input_devices_.dispatch();
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
    const std::uint32_t damaged_before = active->scene.pending_damage_mask();
    // Unflushed: the loop's after-dispatch hook flushes once the iteration is done.
    (void)active->wayland.dispatch(0, false);
    // Whatever a client asked for is applied, configured and composited by a tick — of the
    // outputs it damaged when it damaged any (each on its own phase), else of every output
    // (a frame callback or a configure answer is served by whichever ticks).
    // A window capture's view damaged (its window committed while covered) ticks the
    // window's output.
    const std::uint32_t output_damage = active->scene.pending_damage_mask() & 0xFFFFu;
    const std::uint32_t damaged = output_damage | capture_wanted_outputs(active->p0_);
    // Output damage this dispatch added answers held frame callbacks (capture views do not).
    if (const std::uint32_t fresh = output_damage & ~damaged_before; fresh != 0)
        active->note_client_response(fresh);
    active->request_frame(damaged != 0 ? damaged : ~0u);
}

void compositor_impl::content_fd_callback() {
    if (!active)
        return;
    // A fence a queued content update waited on signalled: it applies now and is composited
    // by the next tick of the outputs it damaged.
    content_updates_poll(active->p0_);
    const std::uint32_t damaged = (active->scene.pending_damage_mask() & 0xFFFFu) |
                                  capture_wanted_outputs(active->p0_);
    if (damaged != 0)
        active->request_frame(damaged);
}

void compositor_impl::arm_commit_timers() {
    if (!vsync_timer || display_gate_.load(std::memory_order_relaxed) != 0 ||
        content_updates_queued(p0_) == 0)
        return;
    const auto now = now_ns();
    bool armed = false;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!clocks_.live(s) || (clocks_.running_mask() & (1u << s)))
            continue;
        if (const unsigned long long t = content_updates_next_target(p0_, s); t != 0)
            armed = clocks_.request_at(s, static_cast<lx::runtime::clock_time>(t),
                                       static_cast<lx::runtime::clock_time>(k_cu_time_slack_ns),
                                       now) ||
                    armed;
    }
    if (armed)
        rearm_frame_timer();
}

void compositor_impl::evaluate_tearing(std::uint32_t published) {
    const bool locked_or_paused = lock_.state() != lock_state::unlocked ||
                                  display_gate_.load(std::memory_order_relaxed) != 0;
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        const std::uint32_t bit = 1u << slot;
        if (!(published & bit))
            continue;
        async_flip_state st{};
        st.card_capable = p0_.async_flip_capable;
        // No async commit has been TEST_ONLY-verified on any pipeline: the atomic-async
        // property set is not wired yet (no hardware here exposes the cap to validate it
        // against), so this stays false and the policy answers vsync everywhere.
        st.pipeline_tested = false;
        st.locked_or_paused = locked_or_paused;
        toplevels.for_each([&](const toplevel_record& rec) {
            if (st.fullscreen_top || !rec.mapped || rec.state != lx::toplevel_state::fullscreen ||
                rec.fullscreen_output != static_cast<int>(slot))
                return;
            st.fullscreen_top = true;
            st.wants_async = surface_wants_async(p0_, rec.surface);
        });
        if (allow_async(st))
            async_allowed_mask_ |= bit;
        else
            async_allowed_mask_ &= ~bit;
        if (st.wants_async && !st.card_capable && !async_unsupported_logged_ &&
            lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
            async_unsupported_logged_ = true;
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.gfx",
                                            "tearing hint async on a fullscreen window: async "
                                            "page flip unsupported by the card, presenting "
                                            "with vsync");
        }
    }
}

void compositor_impl::drm_fd_callback() {
    if (!active || !active->drm_present_)
        return;
    // The moment a flip lands, not at the next poll: the flip handler phase-locks that
    // output's frame clock to it, and frame callbacks wait for it. One drain serves every
    // pipeline: the device routes each event to the pipeline whose CRTC it names.
    active->kms.dispatch_events();
}

void compositor_impl::housekeeping_callback() {
    if (!active)
        return;
    const std::uint32_t live = active->outputs.live_mask();
    const std::uint32_t dark = active->gate_power_mask_.load(std::memory_order_relaxed);
    if (active->session_.display_stopped() || (live != 0 && (dark & live) == live)) {
        // The paused tick, once a second (VT away, or every output powered off): a lost
        // ResumeDevice (or a card logind does not manage) is noticed here, and clients
        // still get their frame callbacks.
        if (active->session_.display_stopped()) {
            const bool session_active =
                !active->logind_.has_control() || active->logind_.is_active();
            const bool master = session_active && active->kms.is_master();
            active->apply_session_actions(active->session_.on_paused_tick(session_active, master));
        }
        active->on_vsync(0, true);
        if (active->clocks_.running_mask() == 0 && active->housekeeping_timer)
            active->housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
        return;
    }
    // Every frame clock is stopped: surfaces not on screen get their throttled frame
    // callbacks here, about once a second, without restarting a clock for them.
    if (frame_callbacks_throttled(active->p0_)) {
        const auto now = now_ns();
        fire_visible_frame_callbacks(active->p0_, static_cast<unsigned>(now / 1'000'000ull),
                                     now);
    }
    // One tick of every lit output for the deadlines only a tick notices; nothing woke the
    // clocks but the calendar, so one quiet tick is enough to stop them again.
    active->on_vsync(0, true);
    if (active->clocks_.running_mask() == 0 && active->housekeeping_timer)
        active->housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
}

void compositor_impl::write_screenshot(const output_pipeline& p, unsigned slot) {
    // Render thread, on request only: a full-frame read back and a synchronous write. Not a
    // hot-path operation and never part of a normal frame.
    const auto& target = p.gl[slot];
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
    // lumen-screenshot-<n>-<output>.ppm: one file per output of the same request.
    const output_state& o = outputs.slot_state(p.slot);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/lumen-screenshot-%u-%s.ppm", dir ? dir : "/tmp",
                  screenshot_count_, outputs.slot_live(p.slot) ? o.name : "gone");
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
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.render",
                                     "screenshot {}x{} written to {} (output {})", w, h, path,
                                     p.slot);
}

void compositor_impl::refresh_capture_host() {
    // UI. What screen capture sees of each output (design-app-compat-capture CC7–CC9): its
    // mode and projection, whether its display is stopped, and where the lock starts on it.
    const bool vt = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
    const std::uint32_t dark = gate_power_mask_.load(std::memory_order_relaxed);
    capture_host_.paused = vt;
    const bool locked = lock_.scene_locked();
    for (unsigned s = 0; s < output_manager::k_max && s < 16; ++s) {
        capture_output_info& c = capture_host_.outputs[s];
        const output_ui& o = outs_[s];
        if (!outputs.slot_live(s) || !o.live) {
            c = {};
            continue;
        }
        const output_state& st = outputs.slot_state(s);
        c.live = true;
        c.id = st.id;
        c.mode = st.mode;
        c.scale = st.scale;
        c.origin = {st.logical_rect().x, st.logical_rect().y};
        c.transform = static_cast<lx::buffer_transform>(st.transform & 7u);
        c.held = vt || (dark & (1u << s)) != 0;
        c.lock_frame = o.lock_frame;
        c.lock_shown = locked && o.published_once &&
                       static_cast<int>(o.last_published_frame - o.lock_frame) >= 0;
    }
}

capture_cursor compositor_impl::capture_cursor_for(const output_pipeline& p) const {
    // Render. The pointer image of this output's scale, turned with it, where the plane
    // would put it (the same math as `prepare_cursor_plane`).
    capture_cursor c{};
    c.visible = cursor_cur_.visible;
    c.in_frame = cursor_cur_.visible &&
                 (!cursor_cur_.hardware || cursor_hw_failed_.load(std::memory_order_relaxed) ||
                  !p.cursor.valid());
    if (!c.visible || c.in_frame)
        return c;
    const unsigned scale120 = static_cast<unsigned>(std::lround(p.scale * 120.0));
    const cursor_image_set& set = cursor_images_.front();
    const cursor_plane_image* img = set.for_scale(scale120);
    if (!img || img->width == 0 || img->pixels.empty()) {
        c.visible = false;
        return c;
    }
    const lx::buffer_transform t = p.transform;
    unsigned w = img->width;
    unsigned h = img->height;
    int hot_x = img->hot_x;
    int hot_y = img->hot_y;
    c.pixels = img->pixels.data();
    c.stride_px = img->width;
    if (t != lx::buffer_transform::normal) {
        // The plane's turned copy, when it holds this image (it is turned once per change).
        if ((static_cast<unsigned>(t) & 1u) != 0) {
            w = img->height;
            h = img->width;
        }
        const lx::rect2i hs = gfx::transform_rect({img->hot_x, img->hot_y, 1, 1}, t,
                                                  static_cast<int>(img->width),
                                                  static_cast<int>(img->height));
        hot_x = hs.x;
        hot_y = hs.y;
        if (p.cursor_staged_gen == set.generation && !p.cursor_turned.empty()) {
            c.pixels = p.cursor_turned.data();
            c.stride_px = w;
        } else {
            c.visible = false; // not turned yet: painted from the next frame on
            return c;
        }
    }
    c.width = w;
    c.height = h;
    c.generation = set.generation;
    c.rect = capture_cursor_rect(cursor_cur_.x, cursor_cur_.y, p.origin, p.scale, t, p.mode, w, h,
                                 hot_x, hot_y);
    return c;
}

void compositor_impl::run_capture_render() {
    // Render. After the commits: output copies that need no new composite (a first frame, a
    // pointer move for paint_cursors) from each output's newest composited frame, window
    // captures from their capture views, curtains; then UI hears of the completions.
    if (capture_channel_.sessions.load(std::memory_order_acquire) == 0 &&
        !capture_renderer_.has_pending()) {
        return;
    }
    const bool vt_paused = (display_gate_.load(std::memory_order_acquire) & gate_vt) != 0;
    const std::uint32_t dark = gate_power_mask_.load(std::memory_order_acquire);
    capture_renderer_.run_curtains(capture_channel_);
    for (auto& up : pipes_) {
        if (!up || up->retiring || up->capture_front < 0)
            continue;
        output_pipeline& p = *up;
        // A stopped display holds its sessions (CC8): nothing new can be shown.
        if (vt_paused || (dark & (1u << p.slot)))
            continue;
        const capture_cursor cursor = capture_cursor_for(p);
        if (!capture_renderer_.wants_front(p.slot, cursor))
            continue;
        capture_frame_source src{};
        src.frame = p.capture_front_frame;
        src.time_ns = p.capture_front_ns;
        src.size = p.mode;
        src.cursor = cursor;
        const scene::immutable_frame_snapshot* snap = nullptr;
        if (cpu_present_ || (gl_present_ && cursor.in_frame)) {
            // The frame's draws: the CPU path composites them into the client's memory, GL
            // needs them to leave the software pointer out.
            snap = &scene.acquire_render_snapshot(p.slot);
            drain_texture_updates(snap->frame_index());
            const auto& draws = snap->draws();
            gfx::blit_command* blits = blit_storage(draws.size());
            for (unsigned i = 0; i < draws.size(); ++i) {
                const auto& cmd = draws.data()[i];
                blits[i] = {};
                blits[i].texture_id = cmd.texture.id();
                blits[i].dst = cmd.dst;
                blits[i].src = cmd.src;
                blits[i].clip = cmd.clip;
                blits[i].tint = cmd.tint;
                blits[i].opacity = cmd.opacity;
                blits[i].blend = cmd.blend;
                blits[i].buffer_xform = cmd.buffer_xform;
                blits[i].src_space = cmd.src_space;
                blits[i].scaled = cmd.scaled;
                blits[i].occluded = cmd.occluded;
                blits[i].solid = cmd.solid;
                blits[i].solid_color = cmd.solid_color;
            }
            src.blits = blits;
            src.blit_count = draws.size();
            src.clear = snap->clear_color();
        }
        if (gl_present_)
            src.gl_framebuffer = p.gl[p.capture_front].framebuffer();
        else if (vulkan_present_)
            src.vk_target = &p.vk[p.capture_front];
        capture_renderer_.output_front(p.slot, src, capture_channel_);
        if (snap)
            scene.release_render_frame(p.slot, *snap);
    }
    // Window captures: each job's capture view, composited into its client's buffer.
    capture_toplevel_job jobs[capture_channel::k_jobs];
    const unsigned n = capture_renderer_.toplevel_jobs(jobs, capture_channel::k_jobs);
    for (unsigned i = 0; i < n; ++i) {
        bool seen = false;
        for (unsigned k = 0; k < i; ++k)
            seen = seen || jobs[k].view == jobs[i].view;
        if (seen || jobs[i].view >= scene::k_max_views)
            continue;
        const scene::immutable_frame_snapshot& snap = scene.acquire_render_snapshot(jobs[i].view);
        drain_texture_updates(snap.frame_index());
        const auto& draws = snap.draws();
        gfx::blit_command* blits = blit_storage(draws.size());
        for (unsigned d = 0; d < draws.size(); ++d) {
            const auto& cmd = draws.data()[d];
            blits[d] = {};
            blits[d].texture_id = cmd.texture.id();
            blits[d].dst = cmd.dst;
            blits[d].src = cmd.src;
            blits[d].clip = cmd.clip;
            blits[d].tint = cmd.tint;
            blits[d].opacity = cmd.opacity;
            blits[d].blend = cmd.blend;
            blits[d].buffer_xform = cmd.buffer_xform;
            blits[d].src_space = cmd.src_space;
            blits[d].scaled = cmd.scaled;
            blits[d].occluded = cmd.occluded;
            blits[d].solid = cmd.solid;
            blits[d].solid_color = cmd.solid_color;
        }
        capture_frame_source src{};
        src.frame = snap.frame_index();
        src.time_ns = static_cast<unsigned long long>(now_ns());
        src.blits = blits;
        src.blit_count = draws.size();
        src.clear = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
        // The pointer over the window, at the view's projection.
        if (jobs[i].paint_cursors && cursor_cur_.visible) {
            const unsigned scale120 = static_cast<unsigned>(std::lround(jobs[i].scale * 120.0));
            if (const cursor_plane_image* img = cursor_images_.front().for_scale(scale120);
                img && img->width > 0 && !img->pixels.empty()) {
                src.cursor.visible = true;
                src.cursor.pixels = img->pixels.data();
                src.cursor.width = img->width;
                src.cursor.height = img->height;
                src.cursor.stride_px = img->width;
                src.cursor.generation = cursor_images_.front().generation;
                src.cursor.rect = capture_cursor_rect(
                    cursor_cur_.x, cursor_cur_.y, jobs[i].origin, jobs[i].scale,
                    lx::buffer_transform::normal, jobs[i].size, img->width, img->height,
                    img->hot_x, img->hot_y);
            }
        }
        capture_renderer_.toplevel_composited(jobs[i].view, snap.damage_rects(), src,
                                              capture_channel_);
        scene.release_render_frame(jobs[i].view, snap);
    }
    capture_renderer_.flush(capture_channel_);
    if (capture_channel_.has_completions())
        notify_capture_completions();
    // Capture composites read client textures after this tick's output composites: their
    // fence is the newest one explicit-sync release points must follow.
    if (const int fence = capture_renderer_.take_sample_fence(release_fences_.wanted());
        fence >= 0) {
        const long long done = capture_renderer_.view_frame_done();
        release_fences_.publish(fence, done >= 0 ? static_cast<unsigned>(done) : 0u);
    }
}

void compositor_impl::notify_capture_completions() {
    // Render or a worker read: UI answers the clients without waiting for its next tick.
    if (capture_ui_posted_.exchange(true, std::memory_order_acq_rel))
        return;
    compositor_impl* self = this;
    if (!runtime::executor::global().post(runtime::affinity::ui, [self] {
            self->capture_ui_posted_.store(false, std::memory_order_release);
            capture_complete(self->p0_);
            self->wayland.flush();
        }))
        capture_ui_posted_.store(false, std::memory_order_release); // the tick drains
}

void compositor_impl::after_dispatch_callback() {
    if (active && active->screenshot_requested_.exchange(false, std::memory_order_acq_rel)) {
        // Every pixel of every output repainted into the slot that is read back.
        ++active->screenshot_count_;
        active->scene.note_full_damage();
        active->screenshot_pending_.store(active->outputs.live_mask(), std::memory_order_release);
        active->request_frame();
    }
    // Output heads that changed in this iteration (hotplug, an apply, a mode switch): one
    // round of events and one `done` for all of it.
    if (active)
        active->flush_output_heads();
    // Pointer constraints decided after what this iteration dispatched (a region commit, a
    // window that mapped or moved): nothing to do while no constraint exists.
    if (active)
        pointer_constraints_update(active->p0_);
    // Workspace changes not flushed by the operation that made them (a flag compare).
    if (active)
        active->wm_.flush_workspaces();
    // A move, resize or drag that began during a held action takes the keyboard back from it.
    if (active && active->keys_.hold_active() &&
        (active->wm_.grab_active() || active->wm_.drag_active()))
        active->cancel_hold("interactive grab");
    if (active) {
        // SIGHUP (systemctl --user reload), the watchdog, and the placement hold's early end
        // (the shell reserved its zones): flags and clock compares when there is nothing to do.
        active->handle_reload_request();
        const auto now = now_ns();
        active->maybe_ping_watchdog(now);
        if (active->placement_hold_until_ != 0 && active->shell_bridge_.has_bridge()) {
            bool reserved = active->outputs.live_mask() != 0;
            for (unsigned slot = 0; slot < output_manager::k_max && reserved; ++slot) {
                if (!active->outputs.slot_live(slot))
                    continue;
                const lx::rect2i full = active->outputs.slot_state(slot).logical_rect();
                const lx::rect2i u = active->layers_.usable(slot);
                reserved = active->layers_.arranged(slot) &&
                           (u.x != full.x || u.y != full.y || u.width != full.width ||
                            u.height != full.height);
            }
            if (reserved)
                active->end_placement_hold("the shell reserved its zones");
        }
    }
    // Events produced anywhere in this iteration (input, protocol replies, releases) leave
    // in one flush rather than waiting for the next tick.
    if (active)
        active->wayland.flush();
}

void compositor_impl::request_frame(std::uint32_t mask) {
    // Paused: the 1 Hz paused tick is the only clock; requests and flips do not start one.
    // A dark output (powered off) is never started (`frame_clocks::set_dark`).
    if (!vsync_timer || display_gate_.load(std::memory_order_relaxed) != 0)
        return;
    const std::uint32_t started = clocks_.request(mask, now_ns());
    if (started == 0)
        return;
    // Time spent stopped is neither a frame interval nor a stalled present.
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (started & (1u << s))
            outs_[s].last_callback_ns = 0;
    last_vsync_time = std::chrono::steady_clock::now();
    rearm_frame_timer();
}

void compositor_impl::rearm_frame_timer() {
    if (!vsync_timer)
        return;
    const lx::runtime::clock_time next = clocks_.next_wakeup();
    if (next != 0) {
        vsync_timer->schedule_at(next);
        if (housekeeping_timer)
            housekeeping_timer->set_enabled(false);
        return;
    }
    vsync_timer->set_enabled(false);
    if (housekeeping_timer && !housekeeping_timer->enabled())
        housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
}

void compositor_impl::settle_frame_clocks(std::uint32_t served, std::uint32_t quiet) {
    if (!vsync_timer || display_gate_.load(std::memory_order_relaxed) != 0)
        return;
    bool stopped_any = false;
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (served & (1u << s))
            stopped_any = clocks_.settle(s, (quiet & (1u << s)) != 0) || stopped_any;
    rearm_frame_timer();
    // A queued commit-timing target keeps no clock running: the stopped clock is armed for
    // the one tick that presents it.
    arm_commit_timers();
    if (stopped_any && clocks_.running_mask() == 0)
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
    // Workspaces changed (design Part W): ext-workspace follows, then the control stream.
    wm_.set_workspace_observer({[](void* u) {
                                    auto* self = static_cast<compositor_impl*>(u);
                                    workspaces_changed(self->p0_);
                                    self->publish_workspaces();
                                },
                                this});
    wm_.set_fullscreen_layer(&fullscreen_layer_);
    wm_.set_snap_preview_layer(&snap_preview_layer_);
    wm_.set_warp([](void* u, double x, double y) { static_cast<compositor_impl*>(u)->warp_pointer(x, y); },
                 this);
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
        p0_.set_output_power = [](void* u, unsigned slot, bool on) {
            static_cast<compositor_impl*>(u)->request_output_power(slot, on);
        };
        p0_.output_power_on = [](void* u, unsigned slot) {
            auto* self = static_cast<compositor_impl*>(u);
            return slot < output_manager::k_max ? self->outs_[slot].power.reported_on() : true;
        };
    }
    p0_.text_input = &text_input_;
    p0_.ime_popup_layer = &ime_popup_layer_;
    // wlr-output-management: a queued apply/test runs when nothing else does.
    p0_.output_mgmt_user = this;
    p0_.output_mgmt_kick = [](void* u) { static_cast<compositor_impl*>(u)->run_next_mgmt_job(); };
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
    // GL and Vulkan copy the pixels out during the drain, so their staging is a transient
    // block per update (`shm_upload_pool`), mapped for it and unmapped after a second idle.
    // A global ring of window-sized vectors that never shrank held seven 1080p frames
    // (57 MiB) after the only client had gone.
    p0_.shm_pool = nullptr;
    p0_.shm_regions = false;
    if (cpu_present_) {
        p0_.shm_store = &shm_store_;
        // Scanout is XR24, so keeping the client's channel order means an unscaled opaque
        // surface reaches the display with no conversion pass at all.
        p0_.shm_native_format = true;
    } else if (gl_present_) {
        // GL reads staged pixels once, during the drain, so staging only has to outlive
        // the trip to the render thread: transient blocks sized by the damage they carry,
        // not a persistent window-sized copy per texture.
        p0_.shm_pool = &shm_pool_;
        p0_.shm_regions = true;
        p0_.shm_store = nullptr;
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
        // Vulkan uploads whole RGBA images.
        p0_.shm_pool = &shm_pool_;
        p0_.shm_store = nullptr;
        p0_.shm_native_format = false;
    }
    // GL imports client dma-bufs on the render thread, where its context lives (an EGLImage
    // per commit); the UI-side Vulkan import cache is only for the Vulkan path.
    surfaces.set_render_thread_import(gl_present_);
    buffer_lifecycle_.set_release_callback(&release_wl_buffer_resource, nullptr);
    buffer_lifecycle_.set_sync_release_callback(&release_sync_point, nullptr);
    protocols_.set_surface_manager(&surfaces);
    // The content-update queue (fifo-v1, commit-timing-v1, fences): when content applied
    // now is predicted to present — this tick's deadline while a tick builds the output's
    // frame, else the first vblank whose tick has not started — and clocks armed for targets.
    p0_.cu_user = this;
    p0_.cu_predict = [](void* u, unsigned slot) -> unsigned long long {
        auto* self = static_cast<compositor_impl*>(u);
        if (slot >= output_manager::k_max || !self->clocks_.live(slot))
            return 0;
        const scheduler::repaint_scheduler& c = self->clocks_.clock(slot);
        const auto now = now_ns();
        if (self->p0_.cu_in_tick && (self->due_mask_ & (1u << slot)))
            return static_cast<unsigned long long>(c.next_deadline(now));
        return static_cast<unsigned long long>(
            c.next_deadline(now + c.estimate_ns() + scheduler::repaint_scheduler::k_margin_ns));
    };
    p0_.cu_request_at = [](void* u, unsigned slot, unsigned long long target) {
        auto* self = static_cast<compositor_impl*>(u);
        if (!self->vsync_timer || self->display_gate_.load(std::memory_order_relaxed) != 0)
            return;
        if (self->clocks_.request_at(slot, static_cast<lx::runtime::clock_time>(target),
                                     static_cast<lx::runtime::clock_time>(k_cu_time_slack_ns),
                                     now_ns()))
            self->rearm_frame_timer();
    };
    // linux-drm-syncobj-v1 needs timeline syncobjs and the eventfd ioctl on the card, and a
    // composite that exports its completion as a sync_file (GL with
    // EGL_ANDROID_native_fence_sync, Vulkan); the CPU composite has no GPU fence to attach.
    p0_.release_fences = &release_fences_;
    p0_.icons = &icons_;
    p0_.foreign = &foreign_;
    // Screen capture (design-app-compat-capture Part D): on the composite path that runs —
    // GL copies the composited frame (one blit, or a read-back for shm) and renders windows'
    // capture views, CPU composites into the client's memory, Vulkan reads its target back
    // (outputs only). Without a composite path (headless) nothing is offered.
    capture_host_.backend = gl_present_       ? capture_backend::gl
                            : cpu_present_    ? capture_backend::cpu
                            : vulkan_present_ ? capture_backend::vulkan
                                              : capture_backend::none;
    capture_host_.dmabuf = gl_present_ && egl_.supports_dmabuf_import();
    capture_host_.dmabuf_device = p0_.dmabuf_main_device;
    capture_host_.channel = &capture_channel_;
    capture_host_.guards = &capture_guards_;
    capture_host_.scene = &scene;
    capture_host_.user = this;
    capture_host_.wake_render = [](void* u) { static_cast<compositor_impl*>(u)->wake_render(); };
    capture_host_.request_frame = [](void* u, std::uint32_t mask) {
        static_cast<compositor_impl*>(u)->request_frame(mask);
    };
    capture_host_.predict = [](void* u, unsigned slot, unsigned long long composited) {
        // The first vblank after the composite: the flip that shows it (CC6).
        auto* self = static_cast<compositor_impl*>(u);
        if (slot >= output_manager::k_max || !self->clocks_.live(slot))
            return 0ull;
        return static_cast<unsigned long long>(self->clocks_.clock(slot).next_deadline(
            static_cast<lx::runtime::clock_time>(composited)));
    };
    capture_guards_.bind({}); // libwayland's access calls are bound by install_p0_protocols
    if (gl_present_)
        capture_renderer_.bind_gl(&gl_compositor_, &egl_);
    else if (cpu_present_)
        capture_renderer_.bind_cpu(&cpu_compositor_);
    else if (vulkan_present_)
        capture_renderer_.bind_vulkan(&vk_compositor_);
    capture_renderer_.bind_guards(&capture_guards_);
    if (cfg.worker_thread_count > 0)
        capture_renderer_.bind_worker(
            [](void (*fn)(void*), void* arg) {
                compositor_impl* self = compositor_impl::active;
                if (!self || !self->threads_started.load(std::memory_order_acquire))
                    return false;
                return runtime::executor::global().post(runtime::affinity::worker,
                                                        runtime::task{fn, arg});
            },
            [](void* u) { static_cast<compositor_impl*>(u)->notify_capture_completions(); },
            this);
    p0_.capture = capture_host_.backend != capture_backend::none ? &capture_host_ : nullptr;
    refresh_capture_host();
    p0_.warp_user = this;
    p0_.warp_pointer = [](void* u, double x, double y) {
        static_cast<compositor_impl*>(u)->warp_pointer(x, y);
    };
    if (drm_present_) {
        const bool fence_out = (gl_present_ && egl_.supports_native_fence()) || vulkan_present_;
        p0_.syncobj_drm_fd = kms.card_fd();
        p0_.syncobj_supported = fence_out && gfx::drm_timeline_supported(kms.card_fd());
        p0_.async_flip_capable = kms.supports_async_flip();
        lx::trace::logger::global().logf(
            lx::trace::level::info, "compositor.gfx",
            "explicit sync (linux-drm-syncobj-v1): {}; async page "
            "flips (tearing-control-v1): {}",
            p0_.syncobj_supported ? "offered"
            : fence_out           ? "not offered (no timeline syncobjs on this card)"
                                  : "not offered (composite exports no fence)",
            p0_.async_flip_capable ? "available" : "unsupported by the card");
    }
    if (auto installed = install_p0_protocols(p0_); !installed) {
        lx::trace::logger::global().log_error(installed.get_error(), "compositor");
    }
    // The heads every zwlr_output_manager_v1 is told about from its first bind.
    mark_heads_dirty();
    flush_output_heads();
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
    // systemd shows the unit deactivating from here on; the shell's exit is an ending, not a
    // crash, so its zones are not held.
    session::service_notifier::stopping("Logging out");
    layers_.set_zone_hold_client(nullptr);
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
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                     "logout requested by {}: closing {} window{}", source, asked,
                                     asked == 1 ? "" : "s");
}

void compositor_impl::advance_logout(lx::runtime::clock_time now_ns) {
    if (!logout_active_)
        return;
    const unsigned left = toplevels.count();
    if (left > 0 && now_ns < logout_deadline_ns_)
        return;
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                     "logout: stopping ({} window{} still open)", left,
                                     left == 1 ? "" : "s");
    logout_active_ = false;
    loop.quit(0);
}

// ── The session, the control socket, health (design-session-and-wm Part 1) ────────

namespace {

const char* backend_name(bool gl, bool cpu, bool vulkan) {
    return gl ? "gl" : cpu ? "cpu" : vulkan ? "vulkan" : "headless";
}

const char* state_name(lx::toplevel_state s) {
    switch (s) {
    case lx::toplevel_state::normal: return "normal";
    case lx::toplevel_state::maximized: return "maximized";
    case lx::toplevel_state::fullscreen: return "fullscreen";
    case lx::toplevel_state::minimized: return "minimized";
    case lx::toplevel_state::tiled: return "tiled";
    case lx::toplevel_state::modal: return "modal";
    }
    return "normal";
}

/// `list` (a colon-separated XDG_CURRENT_DESKTOP) names Lumen.
bool names_lumen(const char* list) {
    if (!list)
        return false;
    std::string_view rest{list};
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        if (rest.substr(0, colon) == "Lumen")
            return true;
        if (colon == std::string_view::npos)
            break;
        rest.remove_prefix(colon + 1);
    }
    return false;
}

/// GetHealth requests the compositor makes itself (the health stream of `lumen-ctl
/// monitor`): answered into the stream only.
constexpr unsigned k_internal_health_token = 0xFFu;

} // namespace

void compositor_impl::start_session() {
    started_ns_ = now_ns();
    watchdog_period_ns_ =
        static_cast<lx::runtime::clock_time>(session::service_notifier::watchdog_usec()) * 1000;
    if (watchdog_period_ns_ > 0)
        watchdog_next_ns_ = started_ns_ + watchdog_period_ns_ / 4;

    if (cfg.control_socket || cfg.session_mode || watchdog_period_ns_ > 0) {
        control_host host{};
        host.user = this;
        host.version = [](void* u, control_version& out) {
            auto* self = static_cast<compositor_impl*>(u);
            out.version = "0.3.0";
            out.zlm_version = 7;
            out.backend = backend_name(self->gl_present_, self->cpu_present_,
                                       self->vulkan_present_);
        };
        host.health = [](void* u, unsigned token) {
            static_cast<compositor_impl*>(u)->begin_health(token);
        };
        host.health_timeout = [](void* u, unsigned token) {
            static_cast<compositor_impl*>(u)->answer_health(
                token, false, control_server::health_timeout_ms, 0, 0, 0, 0);
        };
        host.outputs = [](void* u, control_output* out, unsigned cap) {
            auto* self = static_cast<compositor_impl*>(u);
            unsigned n = 0;
            for (unsigned slot = 0; slot < output_manager::k_max && n < cap; ++slot) {
                if (!self->outputs.slot_live(slot))
                    continue;
                const output_state& o = self->outputs.slot_state(slot);
                control_output& c = out[n++];
                c = {};
                std::snprintf(c.name, sizeof(c.name), "%s", o.name);
                std::snprintf(c.description, sizeof(c.description), "%s", o.description);
                std::snprintf(c.identifier, sizeof(c.identifier), "%s", o.identifier);
                const lx::rect2i l = o.logical_rect();
                c.x = l.x;
                c.y = l.y;
                c.width = l.width;
                c.height = l.height;
                c.mode_width = o.mode.width;
                c.mode_height = o.mode.height;
                c.refresh_mhz = o.refresh_mhz;
                c.scale = o.scale;
                static constexpr const char* k_transforms[] = {
                    "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180",
                    "flipped-270"};
                c.transform = o.transform < 8 ? k_transforms[o.transform] : "normal";
                if (const auto* w = self->wm_.workspaces().find(self->wm_.workspaces().active(slot)))
                    std::snprintf(c.active_workspace, sizeof(c.active_workspace), "%s", w->name);
            }
            return n;
        };
        host.workspaces = [](void* u, control_workspace* out, unsigned cap) {
            return static_cast<compositor_impl*>(u)->fill_control_workspaces(out, cap);
        };
        host.windows = [](void* u, control_window* out, unsigned cap) {
            auto* self = static_cast<compositor_impl*>(u);
            unsigned n = 0;
            // Bottom to top: the stacking order, as a task list would read it reversed.
            self->toplevels.for_each_bottom_up([&](const toplevel_record& rec) {
                if (n < cap)
                    self->fill_control_window(rec, out[n++]);
            });
            return n;
        };
        host.focused = [](void* u, control_window& out) {
            auto* self = static_cast<compositor_impl*>(u);
            const toplevel_record* rec = self->toplevels.find(self->wm_.keyboard_focus());
            if (!rec || !rec->mapped)
                return false;
            self->fill_control_window(*rec, out);
            return true;
        };
        host.reload = [](void* u, unsigned token) {
            static_cast<compositor_impl*>(u)->request_config_reload(reload_cause::control, token);
        };
        host.run_action = [](void* u, const char* action, const char* argument,
                             const char*& message) {
            return static_cast<compositor_impl*>(u)->run_control_action(action, argument,
                                                                         message);
        };
        host.logout = [](void* u, const char* source) {
            static_cast<compositor_impl*>(u)->begin_logout(source);
        };
        if (auto opened = control_.open(host); !opened) {
            lx::trace::logger::global().log_error(opened.get_error(), "compositor.session");
        } else {
            if (cfg.control_socket) {
                const char* runtime = std::getenv("XDG_RUNTIME_DIR");
                char path[256];
                if (cfg.socket_name && cfg.socket_name[0] == '/')
                    std::snprintf(path, sizeof(path), "%s.varlink", cfg.socket_name);
                else
                    std::snprintf(path, sizeof(path), "%s/%s.varlink",
                                  runtime && *runtime ? runtime : "/tmp", cfg.socket_name);
                if (auto listening = control_.listen(path); !listening)
                    lx::trace::logger::global().log_error(listening.get_error(),
                                                          "compositor.session");
                else
                    lx::trace::logger::global().logf(lx::trace::level::info,
                                                     "compositor.session",
                                                     "control socket {} ({})", path,
                                                     k_control_interface);
            }
            control_source_ = std::make_unique<control_source>(*this);
            loop.add_source(control_source_.get());
        }
    }

    // The control socket's event stream follows windows and focus; the placement hold
    // notes the windows that map while it lasts.
    toplevel_observer o{};
    o.user = this;
    o.mapped = [](void* u, toplevel_record& rec) {
        auto* self = static_cast<compositor_impl*>(u);
        if (self->placement_hold_until_ != 0 && self->placement_held_.size() < 16)
            self->placement_held_.push_back(rec.id);
        if (self->control_.subscribers() == 0)
            return;
        control_window w{};
        self->fill_control_window(rec, w);
        self->control_.publish_window("window-opened", w);
    };
    o.unmapped = [](void* u, toplevel_record& rec) {
        auto* self = static_cast<compositor_impl*>(u);
        if (self->control_.subscribers() == 0)
            return;
        control_window w{};
        self->fill_control_window(rec, w);
        self->control_.publish_window("window-closed", w);
    };
    o.changed = [](void* u, const toplevel_record& rec) {
        auto* self = static_cast<compositor_impl*>(u);
        if (self->control_.subscribers() == 0)
            return;
        control_window w{};
        self->fill_control_window(rec, w);
        self->control_.publish_window("window-changed", w);
    };
    o.focused = [](void* u, const toplevel_record* rec) {
        auto* self = static_cast<compositor_impl*>(u);
        if (self->control_.subscribers() == 0)
            return;
        control_window w{};
        if (rec)
            self->fill_control_window(*rec, w);
        self->control_.publish_focus(rec ? &w : nullptr);
    };
    shell_bridge_.set_control_observer(o);
    shell_bridge_.set_bound_handler(
        [](void* u, void* client) { static_cast<compositor_impl*>(u)->on_shell_bound(client); },
        this);
    layers_.set_zone_hold_started(
        [](void* u) {
            auto* self = static_cast<compositor_impl*>(u);
            self->zone_hold_until_ = now_ns() + k_zone_hold_ns;
            self->rearm_session_timer();
        },
        this);
    session_timer_ = std::make_unique<runtime::timer_source>(
        0, &compositor_impl::session_timer_callback, false);
    session_timer_->set_enabled(false);
    loop.add_source(session_timer_.get());

    // A tty login's session says "tty"; the display server that took it over says what it
    // is now. logind puts the old type back when control is released.
    if (logind_.has_control() && std::strcmp(logind_.session_type(), "tty") == 0)
        logind_.set_type("wayland");

    if (!cfg.session_mode) {
        send_ready(nullptr);
        return;
    }
    // The session environment, before READY: units ordered after graphical-session.target
    // (the shell, XDG autostart, portal backends) must find WAYLAND_DISPLAY in the manager.
    // Both calls are asynchronous; READY follows their replies (or 2 s).
    bool sent = false;
    if (!control_.event()) {
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.session",
                                        "no event loop for the user bus: environment not "
                                        "exported");
    } else if (auto opened = user_env_.open(control_.event()); !opened) {
        lx::trace::logger::global().log_error(opened.get_error(), "compositor.session");
    } else {
        char display[96];
        char socket[300];
        char desktop[160];
        std::snprintf(display, sizeof(display), "WAYLAND_DISPLAY=%s", cfg.socket_name);
        std::snprintf(socket, sizeof(socket), "LUMEN_SOCKET=%s",
                      control_.path()[0] ? control_.path() : "");
        const char* current = std::getenv("XDG_CURRENT_DESKTOP");
        std::snprintf(desktop, sizeof(desktop), "XDG_CURRENT_DESKTOP=%s",
                      names_lumen(current) ? current : "Lumen");
        std::array<std::string_view, 7> vars{
            std::string_view{display}, std::string_view{desktop},
            std::string_view{"XDG_SESSION_TYPE=wayland"},
            std::string_view{"XDG_SESSION_DESKTOP=lumen"}};
        std::size_t count = 4;
        if (control_.path()[0])
            vars[count++] = socket;
        // The configured cursor (config.toml [cursor]) for applications that draw their own.
        char xtheme[96];
        char xsize[40];
        const auto& cur = config_.current().cursor;
        if (cur.theme[0]) {
            std::snprintf(xtheme, sizeof(xtheme), "XCURSOR_THEME=%s", cur.theme);
            vars[count++] = xtheme;
        }
        if (cur.size > 0) {
            std::snprintf(xsize, sizeof(xsize), "XCURSOR_SIZE=%d", cur.size);
            vars[count++] = xsize;
        }
        if (auto exported = user_env_.export_async(
                std::span<const std::string_view>{vars.data(), count}, 2000,
                [](void* u, bool ok) {
                    static_cast<compositor_impl*>(u)->on_environment_exported(ok);
                },
                this);
            !exported) {
            lx::trace::logger::global().log_error(exported.get_error(), "compositor.session");
        } else {
            sent = true;
            control_.dispatch(); // the calls leave now, not at the loop's first wake-up
        }
    }
    if (!sent)
        send_ready("environment not exported");
}

void compositor_impl::on_environment_exported(bool ok) {
    lx::trace::timeline_instant("startup", "env exported");
    if (ok)
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                         "session environment exported: WAYLAND_DISPLAY={} "
                                         "XDG_SESSION_TYPE=wayland",
                                         cfg.socket_name);
    else
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.session",
                                        "the user manager refused the session environment "
                                        "(or did not answer in 2 s)");
    send_ready(ok ? nullptr : "environment export failed");
}

void compositor_impl::send_ready(const char* why) {
    if (ready_sent_)
        return;
    ready_sent_ = true;
    char status[160];
    const unsigned n = outputs.count();
    std::snprintf(status, sizeof(status), "Serving %s (%u output%s)%s%s", cfg.socket_name, n,
                  n == 1 ? "" : "s", why ? "; " : "", why ? why : "");
    session::service_notifier::ready(status);
    lx::trace::timeline_instant("startup", "ready");
    if (session::service_notifier::listening())
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                         "ready: {}", status);
    // Windows that map before the shell's panel has reserved its strip are fitted into the
    // usable area once it has (or after 750 ms): only in a session, where the shell starts
    // after READY in parallel with autostart applications.
    if (cfg.session_mode && !shell_bridge_.has_bridge()) {
        placement_hold_until_ = now_ns() + k_placement_hold_ns;
        rearm_session_timer();
    }
}

void compositor_impl::end_session() {
    if (control_source_) {
        loop.remove_source(control_source_.get());
        control_source_.reset();
    }
    // An export still in flight (a stop during start-up) is unset too: close() would flush
    // the SetEnvironment, and the UnsetEnvironment sent after it on the same connection is
    // what the manager applies last.
    if (user_env_.exported() || user_env_.export_pending()) {
        user_env_.unset_now(1000);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.session",
                                        "session environment unset");
    }
    user_env_.close();
    control_.close();
    if (session_timer_) {
        loop.remove_source(session_timer_.get());
        session_timer_.reset();
    }
    for (auto* src : {static_cast<runtime::event_source*>(config_watch_source_.get()),
                      static_cast<runtime::event_source*>(config_timer_.get()),
                      static_cast<runtime::event_source*>(binding_repeat_timer_.get())})
        if (src)
            loop.remove_source(src);
    config_watch_source_.reset();
    config_timer_.reset();
    binding_repeat_timer_.reset();
    config_.watch().close();
}

void compositor_impl::maybe_ping_watchdog(lx::runtime::clock_time now) {
    if (control_.subscribers() > 0 && now >= next_health_poll_ns_) {
        next_health_poll_ns_ = now + k_health_poll_ns;
        begin_health(k_internal_health_token);
    }
    if (watchdog_period_ns_ <= 0 || now < watchdog_next_ns_)
        return;
    // The render thread is alive when its heartbeat moved since the last ping, or when it
    // waits for work. Stuck inside a tick, it withholds the ping; systemd's watchdog then
    // ends the session instead of leaving a frozen screen.
    const unsigned ticks = render_ticks_.load(std::memory_order_acquire);
    const bool alive = !threads_started.load(std::memory_order_acquire) ||
                       ticks != watchdog_render_ticks_ ||
                       !render_in_tick_.load(std::memory_order_acquire);
    if (!alive) {
        if (!watchdog_withheld_) {
            watchdog_withheld_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.session",
                                            "render thread stuck in a tick: watchdog ping "
                                            "withheld");
        }
        watchdog_next_ns_ = now + 250'000'000ll;
        return;
    }
    watchdog_withheld_ = false;
    watchdog_render_ticks_ = ticks;
    session::service_notifier::watchdog_ping();
    watchdog_next_ns_ = now + watchdog_period_ns_ / 4;
}

lx::runtime::clock_time compositor_impl::control_timeout_ns(lx::runtime::clock_time now) const {
    lx::runtime::clock_time due = -1;
    if (watchdog_period_ns_ > 0)
        due = watchdog_next_ns_;
    if (control_.subscribers() > 0 && (due < 0 || next_health_poll_ns_ < due))
        due = next_health_poll_ns_;
    if (due < 0)
        return -1;
    return due > now ? due - now : 0;
}

void compositor_impl::handle_reload_request() {
    if (!reload_requested_.exchange(false, std::memory_order_acq_rel))
        return;
    // systemd's reload protocol (Type=notify-reload): RELOADING=1 now, READY=1 once the
    // generation is applied (finish_config_generation).
    session::service_notifier::reloading("Reloading the configuration");
    lx::trace::logger::global().log(lx::trace::level::info, "config",
                                    "reload (SIGHUP): reading the configuration again");
    request_config_reload(reload_cause::sighup, ~0u);
}

void compositor_impl::begin_health(unsigned token) {
    if (!threads_started.load(std::memory_order_acquire)) {
        answer_health(token, true, 0, import_cache.size(), import_cache.config().max_entries,
                      budget_.last_tick_render_us(), budget_.render_over_total());
        return;
    }
    // The render thread's own figures, read on that thread: the round trip is also the proof
    // that it runs (an answer missing after 500 ms makes "render" critical).
    compositor_impl* self = this;
    const lx::runtime::clock_time t0 = now_ns();
    const bool posted = runtime::executor::global().post(
        runtime::affinity::render, [self, token, t0] {
            const unsigned used = self->import_cache.size();
            const unsigned cap = self->import_cache.config().max_entries;
            const unsigned render_us = self->budget_.last_tick_render_us();
            const unsigned long long over = self->budget_.render_over_total();
            (void)runtime::executor::global().post(
                runtime::affinity::ui, [self, token, t0, used, cap, render_us, over] {
                    if (compositor_impl::active != self)
                        return;
                    const auto ms = static_cast<unsigned>((now_ns() - t0) / 1'000'000);
                    self->answer_health(token, true, ms, used, cap, render_us, over);
                });
        });
    if (!posted)
        answer_health(token, false, 0, 0, 0, 0, 0);
}

void compositor_impl::answer_health(unsigned token, bool render_answered, unsigned render_ms,
                                    unsigned cache_used, unsigned cache_capacity,
                                    unsigned render_us, unsigned long long render_over) {
    lx::health::health_inputs in{};
    const auto now = now_ns();
    in.uptime_ms = started_ns_ > 0 ? static_cast<unsigned long long>((now - started_ns_) / 1'000'000)
                                   : 0;
    in.backend = backend_name(gl_present_, cpu_present_, vulkan_present_);
    in.compositor_pid = static_cast<int>(::getpid());
    in.wayland_serving = true;
    in.socket_name = cfg.socket_name;
    const bool want_virtual = cfg.virtual_outputs && *cfg.virtual_outputs;
    in.display = drm_present_          ? lx::health::display_path::kms
                 : want_virtual        ? lx::health::display_path::virtual_outputs
                 : kms.card_fd() >= 0  ? lx::health::display_path::kms_without_master
                                       : lx::health::display_path::none;
    in.display_paused = session_.display_stopped();
    in.render_answered = render_answered;
    in.render_answer_ms = render_ms;
    in.input_open = input_devices_.fd() >= 0;
    in.input_devices = input_devices_.device_count();
    in.logind_control = logind_.has_control();
    in.session_id = logind_.session_id();
    in.shell_connected = shell_bridge_.has_bridge();
    in.shell_pid = shell_bridge_.bridge_pid();
    in.shell_binds = shell_bridge_.binds();
    in.session_mode = cfg.session_mode;
    in.environment_exported = user_env_.exported();
    in.ready_sent = ready_sent_;
    in.watchdog = watchdog_period_ns_ > 0;
    switch (memory_.current_pressure()) {
    case lx::runtime::memory_pressure::normal: in.memory_pressure = 0; break;
    case lx::runtime::memory_pressure::moderate: in.memory_pressure = 1; break;
    case lx::runtime::memory_pressure::critical: in.memory_pressure = 2; break;
    }
    in.rss_kib = lx::health::read_rss_kib();
    in.import_cache_used = cache_used;
    in.import_cache_capacity = cache_capacity;
    in.render_figures = render_answered;
    in.staging_bytes = shm_store_.bytes_held() + shm_pool_.bytes_mapped();
    in.staging_budget = memory_.config().staging_budget_bytes;
    in.last_tick_ui_us = budget_.last_tick_ui_us();
    in.last_tick_render_us = render_us;
    in.ui_over_budget = budget_.ui_over_total();
    in.render_over_budget = render_over;
    for (unsigned slot = 0; slot < output_manager::k_max && in.outputs.size() < in.outputs.capacity();
         ++slot) {
        if (!outputs.slot_live(slot))
            continue;
        const output_state& s = outputs.slot_state(slot);
        const output_ui& o = outs_[slot];
        lx::health::output_input oi{};
        oi.name.set(s.name);
        oi.refresh_mhz = s.refresh_mhz;
        oi.flip_paced = o.pipe && o.pipe->kms;
        oi.presented = o.presented;
        oi.missed = o.missed;
        oi.recent = o.recent;
        in.outputs.push_back(oi);
    }
    in.config_generation = config_.generation();
    in.config_errors = config_.diagnostics().errors();
    in.config_warnings = config_.diagnostics().warnings();
    const lx::health::health_report report = lx::health::evaluate(in);
    if (token != k_internal_health_token)
        control_.finish_health(token, report);
    if (report.overall != streamed_health_) {
        streamed_health_ = report.overall;
        control_.publish_health(report.overall);
    }
}

void compositor_impl::fill_control_window(const toplevel_record& rec, control_window& out) const {
    out = {};
    out.id = static_cast<unsigned>(rec.id.id());
    std::snprintf(out.app_id, sizeof(out.app_id), "%s", rec.app_id ? rec.app_id : "");
    std::snprintf(out.title, sizeof(out.title), "%s", rec.title ? rec.title : "");
    if (auto* conn = const_cast<wayland::server&>(wayland).find_client(rec.client.id()))
        out.pid = static_cast<int>(conn->credentials().pid);
    if (rec.output >= 0 && outputs.slot_live(static_cast<unsigned>(rec.output)))
        std::snprintf(out.output, sizeof(out.output), "%s",
                      outputs.slot_state(static_cast<unsigned>(rec.output)).name);
    // Snapped to a half or a quarter (design-session-and-wm K.3): "tiled", as the shell sees it.
    out.state = rec.snap && rec.state == lx::toplevel_state::normal ? "tiled" : state_name(rec.state);
    out.focused = rec.id == wm_.keyboard_focus();
    out.minimized = rec.minimized;
    out.sticky = rec.sticky;
    out.workspace = static_cast<unsigned>(rec.workspace.id());
    out.x = rec.geometry.x;
    out.y = rec.geometry.y;
    out.width = rec.geometry.width;
    out.height = rec.geometry.height;
}

unsigned compositor_impl::fill_control_workspaces(control_workspace* out, unsigned cap) const {
    unsigned n = 0;
    wm_.workspaces().for_each([&](const workspace_record& w) {
        if (n >= cap)
            return;
        control_workspace& c = out[n++];
        c = {};
        c.id = static_cast<unsigned>(w.id.id());
        if (w.output >= 0 && outputs.slot_live(static_cast<unsigned>(w.output)))
            std::snprintf(c.output, sizeof(c.output), "%s",
                          outputs.slot_state(static_cast<unsigned>(w.output)).name);
        c.index = w.index + 1;
        std::snprintf(c.name, sizeof(c.name), "%s", w.name);
        c.active = w.active;
        c.urgent = w.urgent;
        c.windows = w.windows;
    });
    return n;
}

void compositor_impl::publish_workspaces() {
    if (control_.subscribers() == 0) {
        published_ws_count_ = 0; // the next subscriber's stream starts from scratch
        return;
    }
    control_workspace now[k_max_workspaces];
    const unsigned n = fill_control_workspaces(now, k_max_workspaces);
    const auto same = [](const control_workspace& a, const control_workspace& b) {
        return a.id == b.id && a.index == b.index && a.active == b.active &&
               a.urgent == b.urgent && a.windows == b.windows &&
               std::strcmp(a.output, b.output) == 0 && std::strcmp(a.name, b.name) == 0;
    };
    for (unsigned i = 0; i < published_ws_count_; ++i) {
        bool still = false;
        for (unsigned j = 0; j < n && !still; ++j)
            still = now[j].id == published_ws_[i].id;
        if (!still)
            control_.publish_workspace("workspace-removed", published_ws_[i]);
    }
    for (unsigned j = 0; j < n; ++j) {
        bool known = false;
        for (unsigned i = 0; i < published_ws_count_ && !known; ++i)
            known = same(now[j], published_ws_[i]);
        if (!known)
            control_.publish_workspace("workspace", now[j]);
    }
    for (unsigned j = 0; j < n; ++j)
        published_ws_[j] = now[j];
    published_ws_count_ = n;
}

void compositor_impl::apply_workspace_config() {
    const lx::config::workspace_settings& w = config_.current().workspaces;
    workspace_config c{};
    c.dynamic = w.mode == lx::config::workspace_mode::dynamic;
    c.count = w.count > 0 ? static_cast<unsigned>(w.count) : 1u;
    c.name_count = w.name_count < k_workspaces_per_output ? w.name_count : k_workspaces_per_output;
    for (unsigned i = 0; i < c.name_count; ++i)
        std::snprintf(c.names[i], sizeof(c.names[i]), "%s", w.names[i]);
    c.switch_on_activate = w.switch_on_activate;
    c.move_follows = w.move_follows;
    wm_.set_workspace_config(c);
}

control_action_result compositor_impl::run_control_action(const char* action,
                                                          const char* argument,
                                                          const char*& message) {
    namespace lxc = lx::config;
    const lxc::action_info* a = lxc::find_action(action ? action : "");
    if (!a || a->id == lxc::action_id::none)
        return control_action_result::unknown_action;
    if (!a->available) {
        message = "this action is not available in this version of Lumen yet";
        return control_action_result::invalid_argument;
    }
    if (a->pointer) {
        message = "this action needs a pointer button (bind it to one, e.g. Super+BTN_LEFT)";
        return control_action_result::invalid_argument;
    }
    unsigned arg = 0;
    if (a->arg == lxc::action_arg::index) {
        char* end = nullptr;
        const long n = argument && *argument ? std::strtol(argument, &end, 10) : 0;
        if (!end || *end || n < 1 || n > 99) {
            message = "this action needs a number 1..99";
            return control_action_result::invalid_argument;
        }
        arg = static_cast<unsigned>(n);
    } else if (argument && *argument) {
        message = "this action takes no argument";
        return control_action_result::invalid_argument;
    }
    // Lock-safe actions only, while locked (the bindings' rule, design C.3).
    if (!a->lock_safe && lock_.state() != lock_state::unlocked)
        return control_action_result::session_locked;
    return run_action(a->id, arg, 0, false, "lumen-ctl", message);
}

control_action_result compositor_impl::run_action(lx::config::action_id action, unsigned argument,
                                                  unsigned time_ms, bool input, const char* source,
                                                  const char*& message) {
    namespace lxc = lx::config;
    using enum lxc::action_id;
    const lxc::action_info& info = lxc::action(action);
    if (info.owner == lxc::action_owner::shell) {
        char arg[16] = "";
        if (argument)
            std::snprintf(arg, sizeof(arg), "%u", argument);
        if (!p0_.send_shell_action || !p0_.send_shell_action(info.name, arg, time_ms, input)) {
            // switcher.next with no shell: the compositor's quick switch (design C.3), so
            // Alt+Tab still works while the shell restarts.
            if (action == switcher_next && lock_.state() == lock_state::unlocked) {
                (void)wm_.quick_switch();
                return control_action_result::ok;
            }
            if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.bindings",
                                                 "{}: no shell speaks zlm v7; dropped", info.name);
            return control_action_result::shell_not_connected;
        }
        return control_action_result::ok;
    }
    switch (action) {
    case session_logout: begin_logout(source); return control_action_result::ok;
    case session_reload_config:
        request_config_reload(reload_cause::action, ~0u);
        return control_action_result::ok;
    case keyboard_next_layout:
    case keyboard_previous_layout: {
        auto& seat = input_devices_.default_seat();
        if (seat.keymap().cycle_layout(action == keyboard_next_layout ? 1 : -1))
            on_input_modifiers(seat.keymap().modifiers());
        return control_action_result::ok;
    }
    // Workspace switches need no focused window (W.1: the focused output, else the
    // pointer's).
    case workspace_switch: (void)wm_.switch_to_index(argument); return control_action_result::ok;
    case workspace_next: (void)wm_.switch_relative(1); return control_action_result::ok;
    case workspace_previous: (void)wm_.switch_relative(-1); return control_action_result::ok;
    default: break;
    }
    const lx::toplevel_id id = wm_.keyboard_focus();
    const toplevel_record* rec = toplevels.find(id);
    if (!rec || !rec->mapped) {
        message = "no window has the keyboard focus";
        return control_action_result::invalid_argument;
    }
    switch (action) {
    case window_close: wm_.request_close(id); break;
    case window_maximize_toggle:
        wm_.set_maximized(id, rec->state != lx::toplevel_state::maximized, source);
        break;
    case window_fullscreen_toggle:
        wm_.set_fullscreen(id, rec->state != lx::toplevel_state::fullscreen, -1, source);
        break;
    case window_move_to_output_left: (void)wm_.move_to_output(id, -1, 0); break;
    case window_move_to_output_right: (void)wm_.move_to_output(id, 1, 0); break;
    case window_move_to_output_up: (void)wm_.move_to_output(id, 0, -1); break;
    case window_move_to_output_down: (void)wm_.move_to_output(id, 0, 1); break;
    case window_minimize: wm_.set_minimized(id, true, source); break;
    case window_sticky_toggle: wm_.set_sticky(id, !rec->sticky); break;
    // Snapping and keyboard move/resize (design-session-and-wm Part K).
    case window_snap_left: (void)wm_.snap_by_key(id, snap_key::left, source); break;
    case window_snap_right: (void)wm_.snap_by_key(id, snap_key::right, source); break;
    case window_snap_up: (void)wm_.snap_by_key(id, snap_key::up, source); break;
    case window_snap_down: (void)wm_.snap_by_key(id, snap_key::down, source); break;
    case window_move_keyboard:
    case window_resize_keyboard:
        if (keys_.hold_active())
            cancel_hold("keyboard grab");
        if (!wm_.begin_keyboard_grab(id, action == window_move_keyboard ? grab_mode::move
                                                                          : grab_mode::resize)) {
            message = "the window cannot be moved or resized now (fullscreen, maximized for a "
                      "resize, or another grab)";
            return control_action_result::invalid_argument;
        }
        break;
    case workspace_move_window: (void)wm_.move_to_index(id, argument); break;
    case workspace_move_window_next: (void)wm_.move_relative(id, 1); break;
    case workspace_move_window_previous: (void)wm_.move_relative(id, -1); break;
    default:
        message = "this action is not available in this version of Lumen yet";
        return control_action_result::invalid_argument;
    }
    return control_action_result::ok;
}

// ── The configuration file (design-session-and-wm Part C) ──────────────────────────────

void compositor_impl::load_initial_config() {
    config_.init();
    (void)config_.reload(true);
    std::snprintf(cursor_theme_name_, sizeof(cursor_theme_name_), "%s",
                  config_.current().cursor.theme);
}

void compositor_impl::apply_initial_config() {
    // A configured layout that did not compile at start: part of generation 1's report.
    if (keymap_error_[0]) {
        keymap_default_ = true;
        report_keymap_error();
    }
    apply_repeat();
    install_bindings();
    apply_focus_policy();
    apply_window_config();
    // Workspaces of the outputs there are (later ones get theirs as they come).
    apply_workspace_config();
    wm_.sync_workspace_outputs(true);
    wm_.set_button_binding(
        [](void* u, unsigned button, unsigned time_ms) {
            return static_cast<compositor_impl*>(u)->on_button_binding(button, time_ms);
        },
        this);
    p0_.shell_config_user = this;
    p0_.shell_config_on_bind = [](void* u, void* res) {
        static_cast<compositor_impl*>(u)->send_config_to_shell(res, true);
    };
    config_timer_ = std::make_unique<runtime::timer_source>(
        0, &compositor_impl::config_timer_callback, false);
    config_timer_->set_enabled(false);
    loop.add_source(config_timer_.get());
    binding_repeat_timer_ = std::make_unique<runtime::timer_source>(
        0, &compositor_impl::binding_repeat_callback, false);
    binding_repeat_timer_->set_enabled(false);
    loop.add_source(binding_repeat_timer_.get());
    if (const int fd = config_.watch().fd(); fd >= 0) {
        config_watch_source_ =
            std::make_unique<runtime::fd_source>(fd, &compositor_impl::config_watch_callback);
        loop.add_source(config_watch_source_.get());
    }
    config_bytes_generation_ = config_.generation();
    // Generation 1 is reported like any other (the log; the shell asks for it on bind).
    finish_config_generation();
}

void compositor_impl::config_watch_callback() {
    if (!active || !active->config_.watch().drain() || !active->config_timer_)
        return;
    // An editor's save is several events (a temporary file, a rename): one reload after they
    // settle.
    active->config_timer_->schedule_at(now_ns() + k_config_debounce_ns);
}

void compositor_impl::config_timer_callback() {
    if (!active)
        return;
    active->config_timer_->set_enabled(false);
    active->request_config_reload(reload_cause::watch, ~0u);
}

void compositor_impl::request_config_reload(reload_cause cause, unsigned control_token) {
    // Answered by the generation this request starts: the busy one's (its bytes were read
    // before this request) is not it.
    auto& tokens = config_busy_ ? config_tokens_next_ : config_tokens_;
    if (control_token != ~0u && !tokens.try_push_back(control_token))
        control_.finish_reload(control_token, config_.generation(), false, {});
    if (cause == reload_cause::sighup)
        (config_busy_ ? config_sighup_next_ : config_sighup_) = true;
    const bool force = cause != reload_cause::watch;
    if (config_busy_) {
        config_again_ = true;
        config_again_force_ = config_again_force_ || force;
        return;
    }
    run_config_reload(force);
}

void compositor_impl::run_config_reload(bool force) {
    lx::trace::scoped_span span{"ui", "config_reload"};
    const config_state::result r = config_.reload(force);
    if (r.what == config_state::outcome::unchanged)
        return;
    config_busy_ = true;
    if (r.what == config_state::outcome::accepted) {
        if (r.bytes_changed)
            config_bytes_generation_ = config_.generation();
        if (r.changes & cfg_repeat)
            apply_repeat();
        if (r.changes & cfg_pointer)
            input_devices_.reapply_device_config();
        if (r.changes & cfg_focus)
            apply_focus_policy();
        if (r.changes & cfg_windows)
            apply_window_config();
        if (r.changes & cfg_workspaces)
            apply_workspace_config();
        if (r.changes & cfg_bindings)
            install_bindings();
        // Unchanged but still failing: compiled again, which reports it again (or applies it
        // when the layout became available).
        if ((r.changes & cfg_keymap) || keymap_error_[0])
            start_keymap_job();
        if (r.changes & cfg_cursor) {
            std::snprintf(cursor_theme_name_, sizeof(cursor_theme_name_), "%s",
                          config_.current().cursor.theme);
            start_cursor_job();
        }
    } else if (keymap_error_[0]) {
        // Rejected: the configuration in use stays, and so does its keymap problem.
        report_keymap_error();
    }
    if (config_jobs_ == 0)
        finish_config_generation();
}

void compositor_impl::report_keymap_error() {
    namespace lxc = lx::config;
    const auto& k = config_.current().keyboard;
    config_.diagnostics().addf(lxc::severity::error, lxc::k_no_layer, 0, "input.keyboard",
                               "layout \"{}\" variant \"{}\" options \"{}\" does not compile ({}); "
                               "{}",
                               k.layout, k.variant, k.options, keymap_error_,
                               keymap_default_ ? "using the system default"
                                               : "keeping the previous keymap");
}

namespace {
/// Runs `fn(arg)` on the worker; false when there is none (the caller runs it inline).
bool config_post_worker(compositor_impl& self, void (*fn)(void*), void* arg) {
    if (!self.threads_started.load(std::memory_order_acquire) || self.cfg.worker_thread_count == 0)
        return false;
    return runtime::executor::global().post(runtime::affinity::worker, runtime::task{fn, arg});
}
} // namespace

void compositor_impl::start_keymap_job() {
    keymap_job_ = std::make_unique<keymap_job>();
    keymap_job_->settings = config_.current().keyboard;
    ++config_jobs_;
    // A keymap compile is ~10 ms of xkbcommon parsing: never on the UI thread's frame path.
    const auto work = [](void* u) {
        auto* self = static_cast<compositor_impl*>(u);
        keymap_job& j = *self->keymap_job_;
        const auto t0 = std::chrono::steady_clock::now();
        j.ok = static_cast<bool>(
            j.keymap.compile(keymap_config_of(j.settings), j.error, sizeof(j.error)));
        j.compile_us = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - t0)
                                                 .count());
        if (lx::runtime::current_affinity() == lx::runtime::affinity::worker)
            (void)runtime::executor::global().post(runtime::affinity::ui,
                                                   [self] { self->finish_keymap_job(); });
        else
            self->finish_keymap_job();
    };
    if (!config_post_worker(*this, work, this))
        work(this);
}

void compositor_impl::finish_keymap_job() {
    std::unique_ptr<keymap_job> job = std::move(keymap_job_);
    auto& seat = input_devices_.default_seat();
    const auto& k = job->settings;
    if (job->ok) {
        // The new keymap takes the old one's locks and held keys; clients get it at once
        // (and the focus its modifiers), the IM grab too.
        job->keymap.adopt_state(seat.keymap(), seat.pressed_keys(), seat.pressed_count());
        seat.keymap() = std::move(job->keymap);
        if (p0_.invalidate_seat_keymap)
            p0_.invalidate_seat_keymap();
        input_router_.seat_keymap_changed(seat);
        input_devices_.update_leds();
        stop_binding_repeat();
        keymap_error_[0] = '\0';
        keymap_default_ = false;
        const auto& km = seat.keymap();
        lx::trace::logger::global().logf(lx::trace::level::info, "config",
                                         "keymap: layout \"{}\" variant \"{}\" options \"{}\" "
                                         "({} layouts, active {}; compiled in {} us)",
                                         k.layout, k.variant, k.options, km.layout_count(),
                                         km.layout_name(km.active_layout()), job->compile_us);
    } else {
        std::snprintf(keymap_error_, sizeof(keymap_error_), "%s",
                      job->error[0] ? job->error : "xkbcommon gave no reason");
        report_keymap_error();
    }
    config_job_done();
}

void compositor_impl::start_cursor_job() {
    cursor_job_ = std::make_unique<cursor_job>();
    cursor_job& j = *cursor_job_;
    std::snprintf(j.theme, sizeof(j.theme), "%s", cursor_theme_name_);
    j.base_px = cursor_base_px();
    for (unsigned s = 0; s < output_manager::k_max && j.count < k_cursor_scales; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const auto s120 = static_cast<unsigned>(std::lround(outputs.slot_state(s).scale * 120.0));
        if (!std::ranges::contains(std::span{j.scale120, j.count}, s120))
            j.scale120[j.count++] = s120;
    }
    if (j.count == 0)
        j.scale120[j.count++] = static_cast<unsigned>(std::lround(outputs.default_scale() * 120.0));
    ++config_jobs_;
    // Dozens of file opens along the theme's Inherits= chain: on the worker.
    const auto work = [](void* u) {
        auto* self = static_cast<compositor_impl*>(u);
        cursor_job& c = *self->cursor_job_;
        for (unsigned i = 0; i < c.count; ++i) {
            const long px = std::lround(static_cast<double>(c.base_px) * c.scale120[i] / 120.0);
            c.themes[i].load(c.theme[0] ? c.theme : nullptr,
                             px > 0 ? static_cast<unsigned>(px) : c.base_px);
        }
        if (lx::runtime::current_affinity() == lx::runtime::affinity::worker)
            (void)runtime::executor::global().post(runtime::affinity::ui,
                                                   [self] { self->finish_cursor_job(); });
        else
            self->finish_cursor_job();
    };
    if (!config_post_worker(*this, work, this))
        work(this);
}

void compositor_impl::finish_cursor_job() {
    namespace lxc = lx::config;
    std::unique_ptr<cursor_job> job = std::move(cursor_job_);
    if (cursor_theme_count_ > 0) {
        for (unsigned i = 0; i < job->count; ++i) {
            cursor_themes_[i] = std::move(job->themes[i]);
            cursor_theme_scale120_[i] = job->scale120[i];
        }
        cursor_theme_count_ = job->count;
        cursor_base_px_ = job->base_px;
        const xcursor_theme* themes[k_cursor_scales]{};
        for (unsigned i = 0; i < cursor_theme_count_; ++i)
            themes[i] = &cursor_themes_[i];
        cursor_.set_themes(themes, cursor_theme_scale120_, cursor_theme_count_);
        request_frame();
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.cursor",
                                         "cursor theme={} size={} (configuration) from_theme={}",
                                         cursor_themes_[0].name(), cursor_themes_[0].size(),
                                         cursor_themes_[0].from_theme());
        if (cursor_theme_name_[0] && !cursor_themes_[0].from_theme())
            config_.diagnostics().addf(lxc::severity::warning, lxc::k_no_layer, 0, "cursor.theme",
                                       "cursor theme \"{}\" was not found; using {}",
                                       cursor_theme_name_, cursor_themes_[0].name());
    }
    export_cursor_environment();
    config_job_done();
}

void compositor_impl::config_job_done() {
    if (config_jobs_ > 0 && --config_jobs_ == 0)
        finish_config_generation();
}

void compositor_impl::export_cursor_environment() {
    // Applications started from now on draw the same cursor (those that draw their own read
    // XCURSOR_*); cursor-shape clients follow at once anyway.
    if (!cfg.session_mode || !user_env_.is_open())
        return;
    const auto& c = config_.current().cursor;
    char theme[96];
    char size[40];
    std::snprintf(theme, sizeof(theme), "XCURSOR_THEME=%s", c.theme);
    std::snprintf(size, sizeof(size), "XCURSOR_SIZE=%u", cursor_base_px());
    std::array<std::string_view, 2> vars{};
    std::size_t n = 0;
    if (c.theme[0])
        vars[n++] = theme;
    if (c.size > 0)
        vars[n++] = size;
    if (n == 0)
        return;
    if (auto r = user_env_.export_async(std::span<const std::string_view>{vars.data(), n}, 2000,
                                        [](void*, bool) {}, nullptr);
        !r)
        lx::trace::logger::global().log_error(r.get_error(), "config");
    else
        control_.dispatch();
}

void compositor_impl::apply_repeat() {
    const auto& k = config_.current().keyboard;
    p0_.repeat_rate = k.repeat_rate;
    p0_.repeat_delay = k.repeat_delay;
    input_router_.send_repeat_info(k.repeat_rate, k.repeat_delay);
}

void compositor_impl::install_bindings() {
    binding_table(config_.current(), binding_table_);
    keys_.set_bindings(binding_table_);
    stop_binding_repeat();
}

void compositor_impl::apply_window_config() {
    const auto& w = config_.current().windows;
    wm_.set_window_config({.edge_snap = w.edge_snap,
                           .edge_snap_distance = w.edge_snap_distance,
                           .edge_snap_corner = w.edge_snap_corner,
                           .snap_preview = w.snap_preview,
                           .drag_restore = w.drag_restore});
}

void compositor_impl::apply_focus_policy() {
    const auto& f = config_.current().focus;
    wm_.set_focus_policy({.sloppy = f.mode == lx::config::focus_mode::sloppy,
                          .raise_on_click = f.raise_on_click,
                          .raise_on_sloppy = f.raise_on_sloppy});
}

void compositor_impl::finish_config_generation() {
    namespace lxc = lx::config;
    config_busy_ = false;
    const auto& d = config_.diagnostics();
    // Logged once per generation: each problem, then one summary line.
    for (const lxc::diagnostic& x : d.items()) {
        char line[1024]; // a path (up to 512) and the message
        lxc::format_diagnostic(x, config_.path_of(x.layer), true, line, sizeof(line));
        lx::trace::logger::global().log(
            x.sev == lxc::severity::error ? lx::trace::level::warn : lx::trace::level::info,
            "config", line);
    }
    if (config_.accepted_last())
        lx::trace::logger::global().logf(lx::trace::level::info, "config",
                                         "config: generation {} from {} ({} errors, {} "
                                         "warnings; {} bindings)",
                                         config_.generation(), config_.describe_source(),
                                         d.errors(), d.warnings(), keys_.binding_count());
    else
        lx::trace::logger::global().logf(lx::trace::level::warn, "config",
                                         "config: generation {} from {} rejected ({} errors); "
                                         "the configuration in use stays",
                                         config_.generation(), config_.describe_source(),
                                         d.errors());
    // The shell: the new bytes when they changed, and the report.
    if (p0_.send_shell_config)
        send_config_to_shell(nullptr, config_.accepted_last() &&
                                          config_bytes_generation_ == config_.generation());
    if (config_sighup_) {
        config_sighup_ = false;
        char status[128];
        std::snprintf(status, sizeof(status), "Serving %s (configuration generation %u)",
                      cfg.socket_name, config_.generation());
        session::service_notifier::ready(status);
    }
    if (!config_tokens_.empty()) {
        // The Reload replies: the generation, whether it applied, and its problems.
        std::vector<control_diagnostic> out;
        out.reserve(d.items().size());
        for (const lxc::diagnostic& x : d.items()) {
            control_diagnostic c{};
            c.severity = lxc::severity_name(x.sev);
            const char* path = config_.path_of(x.layer);
            c.file = path ? path : "";
            c.line = x.line;
            c.key = x.key;
            c.message = x.message;
            out.push_back(c);
        }
        for (const unsigned token : config_tokens_)
            control_.finish_reload(token, config_.generation(), config_.accepted_last(), out);
        config_tokens_.clear();
    }
    control_.publish_config(config_.generation());
    if (config_again_) {
        // A SIGHUP or a Reload call among the queued requests makes it forced, so it always
        // produces a generation that answers them.
        const bool force = config_again_force_;
        config_again_ = false;
        config_again_force_ = false;
        for (const unsigned token : config_tokens_next_)
            (void)config_tokens_.try_push_back(token);
        config_tokens_next_.clear();
        config_sighup_ = std::exchange(config_sighup_next_, false);
        run_config_reload(force);
    }
}

void compositor_impl::send_config_to_shell(void* shell_resource, bool with_bytes) {
    namespace lxc = lx::config;
    if (!p0_.send_shell_config)
        return;
    const auto& d = config_.diagnostics();
    std::vector<std::array<char, 256>> texts(d.items().size());
    std::vector<shell_config_line> lines(d.items().size());
    for (std::size_t i = 0; i < d.items().size(); ++i) {
        const lxc::diagnostic& x = d.items()[i];
        if (x.key[0])
            std::snprintf(texts[i].data(), texts[i].size(), "%s: %s", x.key, x.message);
        else
            std::snprintf(texts[i].data(), texts[i].size(), "%s", x.message);
        const char* path = config_.path_of(x.layer);
        lines[i] = {x.sev == lxc::severity::error ? 0u : 1u, path ? path : "", x.line,
                    texts[i].data()};
    }
    shell_config_report r{};
    r.generation = config_.generation();
    r.applied = config_.accepted_last();
    r.errors = d.errors();
    r.warnings = d.warnings();
    r.lines = lines.data();
    r.line_count = static_cast<unsigned>(lines.size());
    lx::unique_fd fd{};
    if (with_bytes) {
        // A sealed memfd of the accepted bytes: the shell maps it read-only and parses what
        // the compositor parsed.
        const std::string& bytes = config_.accepted_text().bytes;
        const int raw = ::memfd_create("lumen-config", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (raw >= 0) {
            fd = lx::unique_fd{raw};
            std::size_t off = 0;
            while (off < bytes.size()) {
                const ssize_t n = ::write(raw, bytes.data() + off, bytes.size() - off);
                if (n <= 0)
                    break;
                off += static_cast<std::size_t>(n);
            }
            (void)::fcntl(raw, F_ADD_SEALS,
                          F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
            if (off == bytes.size()) {
                r.fd = raw;
                r.size = static_cast<unsigned>(bytes.size());
            }
        }
    }
    p0_.send_shell_config(shell_resource, r);
}

void compositor_impl::stop_binding_repeat() {
    repeat_binding_ = nullptr;
    repeat_code_ = 0;
    repeat_kind_ = key_repeat::binding;
    if (binding_repeat_timer_)
        binding_repeat_timer_->set_enabled(false);
}

void compositor_impl::binding_repeat_callback() {
    if (!active || !active->repeat_code_)
        return;
    compositor_impl& self = *active;
    const auto time_ms = static_cast<unsigned>(now_ns() / 1'000'000);
    switch (self.repeat_kind_) {
    case key_repeat::binding: {
        if (!self.repeat_binding_) {
            self.stop_binding_repeat();
            return;
        }
        // The rules the key press was matched under, again: a lock (or a shortcuts inhibitor)
        // that came while the key is held stops a binding not allowed under it.
        if (!self.keys_.allows(*self.repeat_binding_, shortcuts_inhibited(self.p0_))) {
            self.stop_binding_repeat();
            return;
        }
        const binding_entry b = *self.repeat_binding_;
        const char* message = "";
        (void)self.run_action(b.action, b.argument, time_ms, true, "binding", message);
        if (!self.repeat_binding_) {
            self.stop_binding_repeat();
            return;
        }
        break;
    }
    case key_repeat::hold:
        // The hold may have ended meanwhile (the shell destroyed it, a lock).
        if (!self.keys_.hold_active()) {
            self.stop_binding_repeat();
            return;
        }
        self.send_hold_key(self.repeat_in_, time_ms);
        break;
    case key_repeat::wm:
        if (!self.wm_.grab_active() || !self.wm_.grab().keyboard) {
            self.stop_binding_repeat();
            return;
        }
        self.repeat_event_.time_ms = time_ms;
        (void)self.wm_.on_key(self.repeat_event_);
        break;
    }
    const int rate = self.config_.current().keyboard.repeat_rate;
    if (rate <= 0) {
        self.stop_binding_repeat();
        return;
    }
    self.binding_repeat_timer_->schedule_at(now_ns() + 1'000'000'000ll / rate);
}

void compositor_impl::start_key_repeat(key_repeat kind, const lx::input::key_event& event,
                                       const key_input& in) {
    const auto& kb = config_.current().keyboard;
    if (!binding_repeat_timer_ || kb.repeat_rate <= 0 || !event.pressed)
        return;
    repeat_kind_ = kind;
    repeat_binding_ = nullptr;
    repeat_code_ = event.evdev_keycode;
    repeat_in_ = in;
    repeat_event_ = event;
    binding_repeat_timer_->schedule_at(now_ns() +
                                       static_cast<runtime::clock_time>(kb.repeat_delay) * 1'000'000ll);
    binding_repeat_timer_->set_enabled(true);
}

void compositor_impl::send_hold_key(const key_input& in, unsigned time_ms) {
    if (!p0_.send_hold_key || !keys_.hold_active())
        return;
    const binding_entry* b = keys_.peek_binding(in, shortcuts_inhibited(p0_));
    const char* binding = b ? lx::config::action(b->action).name : "";
    p0_.send_hold_key(keys_.hold_owner(), in.raw ? in.raw : in.raw0, in.mods, binding, time_ms);
}

void compositor_impl::cancel_hold(const char* why) {
    void* hold = keys_.end_hold();
    if (!hold)
        return;
    if (repeat_kind_ == key_repeat::hold)
        stop_binding_repeat();
    if (p0_.send_hold_cancelled)
        p0_.send_hold_cancelled(hold);
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
        lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.bindings",
                                         "held action cancelled ({})", why);
}

wm_button_binding compositor_impl::on_button_binding(unsigned button, unsigned time_ms) {
    namespace lxc = lx::config;
    const auto named = input_devices_.default_seat().keymap().named();
    const unsigned mods = (named.shift ? key_mod_shift : 0u) | (named.ctrl ? key_mod_ctrl : 0u) |
                          (named.alt ? key_mod_alt : 0u) | (named.super ? key_mod_super : 0u);
    const binding_entry* b = keys_.match_button(button, mods, shortcuts_inhibited(p0_));
    if (!b)
        return wm_button_binding::none;
    if (b->action == lxc::action_id::window_move_interactive)
        return wm_button_binding::move;
    if (b->action == lxc::action_id::window_resize_interactive)
        return wm_button_binding::resize;
    const binding_entry copy = *b;
    const char* message = "";
    (void)run_action(copy.action, copy.argument, time_ms, true, "binding", message);
    return wm_button_binding::consumed;
}

void compositor_impl::on_shell_bound(void* client_native) {
    // The new shell's zones are the ones held if it goes without a logout.
    layers_.set_zone_hold_client(logout_active_ ? nullptr : client_native);
}

void compositor_impl::rearm_session_timer() {
    if (!session_timer_)
        return;
    lx::runtime::clock_time next = 0;
    for (const auto t : {zone_hold_until_, placement_hold_until_})
        if (t != 0 && (next == 0 || t < next))
            next = t;
    if (next == 0)
        session_timer_->set_enabled(false);
    else
        session_timer_->schedule_at(next);
}

void compositor_impl::session_timer_callback() {
    if (!active)
        return;
    const auto now = now_ns();
    if (active->zone_hold_until_ != 0 && now >= active->zone_hold_until_) {
        active->zone_hold_until_ = 0;
        active->layers_.release_zone_hold();
    }
    if (active->placement_hold_until_ != 0 && now >= active->placement_hold_until_)
        active->end_placement_hold("750 ms passed");
    active->rearm_session_timer();
}

void compositor_impl::end_placement_hold(const char* why) {
    if (placement_hold_until_ == 0)
        return;
    placement_hold_until_ = 0;
    unsigned moved = 0;
    for (const lx::toplevel_id id : placement_held_)
        if (wm_.fit_into_usable(id))
            ++moved;
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.session",
                                     "placement hold ended ({}): {} window{} mapped meanwhile, "
                                     "{} moved into the usable area",
                                     why, placement_held_.size(),
                                     placement_held_.size() == 1 ? "" : "s", moved);
    placement_held_.clear();
    rearm_session_timer();
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

void compositor_impl::apply_due_forgets(long long low_water) {
    if (low_water < 0)
        return;
    const auto mark = static_cast<unsigned>(low_water);
    for (auto& pending : deferred_forgets_) {
        if (!pending.used)
            continue;
        // Signed difference rather than `<`: frame indices only advance, so their
        // difference stays small and this keeps working across the counter's wrap.
        if (static_cast<int>(mark - pending.retire_after_frame) < 0)
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
            // The rects of the region, when it has several (empty: the region as one).
            const std::span<const lx::rect2i> parts{update.rects.rects, update.rects.count};
            if (update.shm_direct) {
                // The client's own rows, read under its buffer's access guard.
                shm_direct_.upload(update.shm_direct,
                                   [&](const unsigned char* pixels, unsigned stride) {
                                       applied = pixels ? gl_compositor_.upload_client_regions(
                                                              update.texture_id, update.width,
                                                              update.height, update.region,
                                                              parts, pixels, stride, opaque)
                                                        : lx::result<void>{};
                                   });
            } else if (bgra) {
                applied = gl_compositor_.upload_client_regions(
                    update.texture_id, update.width, update.height, update.region, parts,
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
            // The upload copied the pixels into the renderer's own staging.
            shm_pool_.release(update.shm_pool_token);
        } else {
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
    vulkan_present_ = true;
    headless_present_ = false;
    return {};
}

lx::result<void> compositor_impl::setup_cpu_present() {
    // Without DRM master there is no scanout buffer to write into, and compositing into
    // memory nobody reads is not a present path — except virtual outputs asked to composite
    // on the CPU (`--virtual-outputs` with `--present cpu`): their frames are composited on
    // demand, into screen captures' buffers (headless capture, CI).
    const bool virtual_cpu = cfg.present == present_backend::cpu && cfg.virtual_outputs &&
                             *cfg.virtual_outputs;
    if (!drm_present_ && !virtual_cpu)
        return lx::not_implemented("lx::compositor::setup_cpu_present");
    cpu_compositor_.set_worker_threads(cfg.composite_thread_count);
    cpu_present_ = true;
    vulkan_present_ = false;
    headless_present_ = false;
    return {};
}

lx::result<void> compositor_impl::setup_gl_present() {
    if (!drm_present_)
        return lx::not_implemented("lx::compositor::setup_gl_present");

    // The card node, not a render node: the scanout buffers have to be allocated on the
    // device that will scan them out, and this fd is the one holding DRM master.
    auto device = gfx::egl_device::create(kms.card_fd());
    if (!device)
        return device.get_error();
    egl_ = std::move(device).value();
    lx::trace::timeline_instant("startup", "egl ready");

    // Falling through to GL on llvmpipe would be strictly worse than compositing on the
    // CPU directly — same rasterizer, plus an upload and a driver stack.
    if (egl_.is_software_renderer()) {
        return lx::make_error(lx::gl_err::no_context,
                              "GL is a software rasterizer — CPU composite is cheaper");
    }

    if (auto initialized = gl_compositor_.initialize(egl_); !initialized)
        return initialized.get_error();
    lx::trace::timeline_instant("startup", "gl program ready");

    gl_present_ = true;
    vulkan_present_ = false;
    cpu_present_ = false;
    headless_present_ = false;
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.gfx", egl_.renderer());
    return {};
}

lx::result<void> compositor_impl::create_targets(output_pipeline& p, bool scanout) {
    // Sized to the output's mode, so the scanout buffer is shown without scaling.
    const unsigned width = static_cast<unsigned>(p.mode.width);
    const unsigned height = static_cast<unsigned>(p.mode.height);
    if (width == 0 || height == 0)
        return lx::make_error(lx::drm_err::mode_invalid, "no usable mode for the scanout targets");
    const auto format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    const bool kms_out = drm_present_ && scanout;
    p.scanout_rect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    p.slot_count = 0;
    const unsigned slots = kms_out ? output_pipeline::k_slots : 1u;
    for (unsigned i = 0; i < slots; ++i) {
        if (gl_present_) {
            auto created = gfx::gl_scanout_target::create(egl_, width, height, format);
            if (!created) {
                if (i < 2)
                    return created.get_error();
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                                "third scanout slot unavailable — double-buffered");
                break;
            }
            p.gl[i] = std::move(created).value();
            auto exported = p.gl[i].export_dmabuf();
            if (!exported) {
                if (i < 2)
                    return exported.get_error();
                break;
            }
            const auto dmabuf = std::move(exported).value();
            auto framebuffer = drm::kms_framebuffer::import_dmabuf(
                kms, dmabuf.fd.get(), dmabuf.width, dmabuf.height, dmabuf.stride, dmabuf.offset,
                format, dmabuf.modifier);
            if (!framebuffer) {
                if (i < 2)
                    return framebuffer.get_error();
                break;
            }
            p.fb[i] = std::move(framebuffer).value();
        } else if (cpu_present_) {
            // A virtual output on the CPU path has no scanout: its frames are composited only
            // into screen captures' buffers (`composite_output`).
            if (!kms_out)
                break;
            // XR24 is what the primary plane takes everywhere, and it is the byte order
            // wl_shm clients already use — so an unscaled opaque surface reaches the display
            // as a plain row copy with no conversion at all.
            auto created = drm::kms_dumb_framebuffer::create(kms, width, height, format);
            if (!created) {
                if (i < 2) // fewer than two buffers cannot page-flip without tearing
                    return created.get_error();
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                                "third scanout slot unavailable — double-buffered");
                break;
            }
            p.dumb[i] = std::move(created).value();
        } else if (vulkan_present_) {
            auto created = gfx::render_target::create(graphics.context(), width, height);
            if (!created) {
                if (i == 0)
                    return created.get_error();
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.gfx",
                                                "second scanout slot unavailable — single-buffer");
                break;
            }
            p.vk[i] = std::move(created).value();
            if (kms_out) {
                auto exported = p.vk[i].export_dmabuf();
                if (!exported) {
                    if (i == 0)
                        return exported.get_error();
                    break;
                }
                const auto dmabuf = std::move(exported).value();
                auto framebuffer = drm::kms_framebuffer::import_dmabuf(
                    kms, dmabuf.fd.get(), dmabuf.width, dmabuf.height, dmabuf.stride,
                    dmabuf.offset, format, dmabuf.modifier);
                if (!framebuffer) {
                    if (i == 0)
                        return framebuffer.get_error();
                    break;
                }
                p.fb[i] = std::move(framebuffer).value();
            }
        } else {
            break; // headless: nothing to composite into
        }
        ++p.slot_count;
    }
    if (p.kms && p.slot_count > 0)
        p.kms->set_framebuffer(p.framebuffer(0, cpu_present_));
    p.front.store(0, std::memory_order_release);
    p.pending_flip_slot.store(0, std::memory_order_release);
    return {};
}

namespace {

/// The allocator's test: a TEST_ONLY of the whole candidate state with each output's real
/// first framebuffer (ADR 008 M7). Temporary pipelines carry the candidate routing.
struct crtc_test_ctx {
    compositor_impl* impl = nullptr;
    drm::kms_device* kms = nullptr;
    const unsigned* slots = nullptr;
    unsigned count = 0;
    const drm::crtc_alloc_input* in = nullptr;
    const drm::kms_plane_info* planes = nullptr;
    unsigned framebuffers[output_manager::k_max]{};
    const lx::drm::kms_mode* modes[output_manager::k_max]{};
    unsigned connectors[output_manager::k_max]{};
    /// Adaptive sync asked of every candidate (an output a configuration enables with it).
    bool vrr = false;
};

[[nodiscard]] drm::kms_pipeline_desc desc_for(const drm::kms_device& kms,
                                              const drm::crtc_alloc_result& r, unsigned i,
                                              unsigned connector_id) {
    drm::kms_pipeline_desc d{};
    d.crtc_index = static_cast<unsigned>(r.crtc[i]);
    for (unsigned c = 0; c < kms.crtc_count(); ++c)
        if (kms.crtc(c).index == d.crtc_index)
            d.crtc_id = kms.crtc(c).id;
    d.primary_plane_id = kms.plane(static_cast<unsigned>(r.primary_plane[i])).id;
    d.connector_id = connector_id;
    return d;
}

bool crtc_test(const drm::crtc_alloc_result& r, void* user) {
    auto* t = static_cast<crtc_test_ctx*>(user);
    std::unique_ptr<drm::kms_pipeline> pipes[output_manager::k_max];
    drm::modeset_entry entries[output_manager::k_max]{};
    for (unsigned i = 0; i < t->count; ++i) {
        // Unregistered: no event can reach a candidate, and no other thread sees it.
        pipes[i] = std::make_unique<drm::kms_pipeline>(
            *t->kms, desc_for(*t->kms, r, i, t->connectors[i]), false);
        pipes[i]->set_mode(*t->modes[i]);
        entries[i].pipe = pipes[i].get();
        entries[i].framebuffer_id = t->framebuffers[i];
        entries[i].request_page_flip = false;
        entries[i].vrr = t->vrr ? 1 : 0;
    }
    return static_cast<bool>(drm::commit_modeset(*t->kms, entries, t->count, true));
}

} // namespace

namespace {
void release_targets(output_pipeline& p);
} // namespace

std::uint32_t compositor_impl::hold_first_frames(std::uint32_t due, runtime::clock_time now) {
    // An output's first frame is a modeset, and on a virtual GPU (vmwgfx) the next flip after
    // it lands 15–50 ms later. A client connected at start-up commits its first buffer a
    // millisecond or two after the first tick sends its configure, so showing the empty
    // desktop first cost that client a whole extra flip. While clients are connected and none
    // has mapped a surface, the first frame waits — for at most one refresh period, after
    // which the output lights as before. The tick itself still runs (it sends configures);
    // the output is only left out of it. A map restarts the stopped clock at once.
    if (!drm_present_ || toplevels.top() != lx::toplevel_id{} || layers_.mapped_count() > 0)
        return 0;
    // A client whose connection libwayland has not accepted yet counts: at the first tick
    // the clients that connected during start-up are still in the socket's backlog, which
    // makes the Wayland event-loop fd readable.
    if (!wayland.has_clients()) {
        pollfd pending{wayland.event_fd(), POLLIN, 0};
        if (pending.fd < 0 || ::poll(&pending, 1, 0) <= 0)
            return 0;
    }
    std::uint32_t held = 0;
    runtime::clock_time until = 0;
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        output_ui& o = outs_[slot];
        if (!(due & (1u << slot)) || o.published_once || !o.pipe || !o.pipe->kms)
            continue;
        if (o.first_frame_hold_until == 0)
            o.first_frame_hold_until = now + clocks_.clock(slot).refresh_period();
        if (now >= o.first_frame_hold_until)
            continue;
        held |= 1u << slot;
        clocks_.stop(slot);
        if (until == 0 || o.first_frame_hold_until < until)
            until = o.first_frame_hold_until;
    }
    if (held != 0 && first_frame_timer_)
        first_frame_timer_->schedule_at(until);
    return held;
}

void compositor_impl::first_frame_timer_callback() {
    if (active)
        active->request_frame();
}

unsigned compositor_impl::cursor_base_px() const {
    if (const int size = config_.current().cursor.size; size > 0)
        return static_cast<unsigned>(size);
    unsigned base = 24;
    if (const char* env = std::getenv("XCURSOR_SIZE")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0 && v <= 256)
            base = static_cast<unsigned>(v);
    }
    return base;
}

void compositor_impl::begin_cursor_theme() {
    // Reading a theme is dozens of opens across the search path and its Inherits= chain
    // (7–9 ms on the critical path before the first client was served). The outputs' scales
    // are settled only after the present path, so the theme for the configured default scale
    // — what every output uses unless configured otherwise — is read now, beside it.
    const double scale = cfg.output_scale > 0.0 ? cfg.output_scale : 1.0;
    prefetched_scale120_ = static_cast<unsigned>(std::lround(scale * 120.0));
    const unsigned base = cursor_base_px();
    const long px = std::lround(static_cast<double>(base) * prefetched_scale120_ / 120.0);
    const unsigned size = px > 0 ? static_cast<unsigned>(px) : base;
    cursor_theme_thread_ =
        std::thread([this, size] { prefetched_theme_.load(cursor_theme_arg(), size); });
}

void compositor_impl::warm_gl_paths() {
    // A virtual GPU (vmwgfx/SVGA3D) translates shaders for its host on first use, per texture
    // format, sampler and target, and creates host surfaces lazily: a startup trace showed
    // the first client frame's flip landing 14–25 ms after its commit, against about 5 ms
    // once warm. One tiny draw per client path — shm BGRA opaque and with alpha, resampled,
    // and the RGBA path of compositor-drawn pixels — into a scanout slot that has not been
    // shown makes the driver do that work now, while start-up goes on: the GL work is
    // flushed, not waited for. The slot starts with full damage, so it is repainted whole
    // before its first scanout.
    output_pipeline* pipe = nullptr;
    for (auto& p : pipes_)
        if (p && p->gl[output_pipeline::k_slots - 1].valid()) {
            pipe = p.get();
            break;
        }
    if (!pipe)
        return;
    constexpr unsigned k_bgra_opaque = 0xfffffff0u;
    constexpr unsigned k_bgra_alpha = 0xfffffff1u;
    constexpr unsigned k_rgba = 0xfffffff2u;
    const unsigned char px[16] = {0x40, 0x40, 0x40, 0xff, 0x40, 0x40, 0x40, 0x80,
                                  0x40, 0x40, 0x40, 0xff, 0x40, 0x40, 0x40, 0x80};
    unsigned ids[3]{};
    unsigned n = 0;
    if (gl_compositor_.supports_client_upload()) {
        if (gl_compositor_.upload_client_region(k_bgra_opaque, 2, 2, {}, px, 8, true))
            ids[n++] = k_bgra_opaque;
        if (gl_compositor_.upload_client_region(k_bgra_alpha, 2, 2, {}, px, 8, false))
            ids[n++] = k_bgra_alpha;
    }
    if (gl_compositor_.upload_rgba(k_rgba, 2, 2, px, 8))
        ids[n++] = k_rgba;
    gfx::blit_command cmds[4]{};
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) {
        cmds[count].texture_id = ids[i];
        cmds[count].dst = {static_cast<int>(i) * 2, 0, 2, 2};
        ++count;
    }
    if (n > 1) { // resampled: the linear-filter variant
        cmds[count].texture_id = ids[1];
        cmds[count].dst = {0, 2, 3, 3};
        cmds[count].scaled = true;
        ++count;
    }
    if (count > 0) {
        if (auto drawn = gl_compositor_.composite(pipe->gl[output_pipeline::k_slots - 1],
                                                  lx::color::rgb(0.f, 0.f, 0.f, 1.f), cmds,
                                                  count, lx::rect2i{0, 0, 8, 5});
            drawn && drawn.value().out_fence_fd >= 0)
            ::close(drawn.value().out_fence_fd);
    }
    for (unsigned i = 0; i < n; ++i)
        gl_compositor_.forget_texture(ids[i]);
    lx::trace::timeline_instant("startup", "gl paths warmed");
}

lx::result<void> compositor_impl::create_pipelines() {
    // One pipeline per live output. Without KMS (headless, nested) the pipelines have no
    // CRTC and only composite (Vulkan without scanout) or nothing at all.
    unsigned slots[output_manager::k_max];
    unsigned n = 0;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const output_state& o = outputs.slot_state(s);
        auto p = std::make_unique<output_pipeline>();
        p->impl = this;
        p->slot = s;
        p->id = o.id;
        p->origin = o.position;
        p->scale = o.scale;
        p->transform = static_cast<lx::buffer_transform>(o.transform & 7u);
        p->mode = o.mode;
        outs_[s].pipe = p.get();
        pipes_[s] = std::move(p);
        slot_phase_[s] = slot_phase::live;
        if (drm_present_ && !o.virtual_output && o.connector_id != 0)
            slots[n++] = s;
    }
    if (!drm_present_ || n == 0) {
        for (auto& p : pipes_)
            if (p && (vulkan_present_ || gl_present_ || cpu_present_))
                if (auto made = create_targets(*p, false); !made)
                    lx::trace::logger::global().log_error(made.get_error(), "compositor.gfx");
        return {};
    }

    // Scanout targets first (they depend on the mode only), so the allocator's TEST_ONLY
    // runs with the real framebuffers.
    crtc_test_ctx t{};
    t.impl = this;
    t.kms = &kms;
    t.count = n;
    drm::crtc_alloc_input in{};
    in.connector_count = n;
    in.crtc_count = kms.crtc_count();
    for (unsigned i = 0; i < n; ++i) {
        const output_state& o = outputs.slot_state(slots[i]);
        output_pipeline& p = *pipes_[slots[i]];
        if (auto made = create_targets(p, true); !made)
            return made.get_error();
        t.framebuffers[i] = p.framebuffer(0, cpu_present_);
        t.modes[i] = &o.kms_mode;
        t.connectors[i] = o.connector_id;
        in.connectors[i].possible_crtcs = o.possible_crtcs;
        in.connectors[i].routed_crtc = o.routed_crtc_id ? kms.crtc_index_of(o.routed_crtc_id) : -1;
    }
    in.plane_count = kms.plane_count() < drm::k_alloc_max_planes ? kms.plane_count()
                                                                  : drm::k_alloc_max_planes;
    for (unsigned k = 0; k < in.plane_count; ++k) {
        const drm::kms_plane_info pl = kms.plane(k);
        in.planes[k].kind = pl.kind;
        in.planes[k].possible_crtcs = pl.possible_crtcs;
        in.planes[k].bound_crtc = pl.crtc_id ? kms.crtc_index_of(pl.crtc_id) : -1;
        in.planes[k].usable = pl.kind == drm::plane_kind::cursor ? pl.argb8888 : pl.xrgb8888;
    }
    drm::crtc_alloc_result r{};
    const bool master = kms.is_master();
    bool assigned = drm::assign_crtcs(in, r, master ? &crtc_test : nullptr, &t);
    if (!assigned && master) {
        // A persisted mode the monitor no longer takes (or a custom mode the link cannot
        // carry): those outputs fall back to their preferred mode, and the test runs again
        // (§C.3's fallback search, at start).
        unsigned changed = 0;
        for (unsigned i = 0; i < n; ++i) {
            const output_state& o = outputs.slot_state(slots[i]);
            if (o.follow_preferred || !o.preferred_mode.valid() ||
                o.preferred_mode.same_timings(o.kms_mode))
                continue;
            lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.outputcfg",
                                             "output {} {}: its configured mode {}x{} failed "
                                             "TEST_ONLY — preferred mode instead",
                                             slots[i], o.name, o.mode.width, o.mode.height);
            outputs.set_mode(slots[i], o.preferred_mode, true, false);
            output_pipeline& p = *pipes_[slots[i]];
            release_targets(p);
            p.mode = outputs.slot_state(slots[i]).mode;
            if (auto made = create_targets(p, true); !made)
                return made.get_error();
            t.framebuffers[i] = p.framebuffer(0, cpu_present_);
            t.modes[i] = &outputs.slot_state(slots[i]).kms_mode;
            ++changed;
        }
        if (changed > 0) {
            r = drm::crtc_alloc_result{};
            assigned = drm::assign_crtcs(in, r, &crtc_test, &t);
        }
    }
    // Without master yet (a session starting in the background) TEST_ONLY cannot run; the
    // structural assignment is taken and the first modeset decides.
    if (!assigned) {
        lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.drm",
                                         "CRTC allocation failed for {} output{} (steps={} "
                                         "tests={}) — trying without the test",
                                         n, n == 1 ? "" : "s", r.steps, r.tests);
        if (!drm::assign_crtcs(in, r, nullptr, nullptr))
            return lx::make_error(lx::drm_err::no_crtc,
                                  "no CRTC assignment for the connected outputs");
    }
    for (unsigned i = 0; i < n; ++i) {
        output_pipeline& p = *pipes_[slots[i]];
        const output_state& o = outputs.slot_state(slots[i]);
        const drm::kms_pipeline_desc d = desc_for(kms, r, i, o.connector_id);
        p.kms = std::make_unique<drm::kms_pipeline>(kms, d);
        p.kms->set_mode(o.kms_mode);
        p.kms->set_vrr(o.adaptive_sync);
        p.kms->set_framebuffer(p.framebuffer(0, cpu_present_));
        p.presentation.bind_page_flip(*p.kms);
        p.presentation.set_handler(&compositor_impl::presentation_flip_handler, &p);
        lx::trace::logger::global().logf(
            lx::trace::level::info, "compositor.drm",
            "output {} pipeline crtc={} (index {}) primary={} "
            "connector={} mode={}x{}@{} cursor_plane={}",
            p.slot, d.crtc_id, d.crtc_index, d.primary_plane_id, d.connector_id, o.kms_mode.width,
            o.kms_mode.height, o.kms_mode.refresh_mhz,
            r.cursor_plane[i] >= 0
                ? static_cast<int>(kms.plane(static_cast<unsigned>(r.cursor_plane[i])).id)
                : -1);
    }
    {
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.drm",
                                         "CRTC allocation: {} output{}, {} TEST_ONLY, {} steps", n,
                                         n == 1 ? "" : "s", r.tests, r.steps);
    }
    return {};
}

void compositor_impl::create_cursor_planes(unsigned need_px) {
    for (auto& up : pipes_) {
        if (!up || !up->kms)
            continue;
        output_pipeline& p = *up;
        const drm::kms_pipeline_desc& d = p.kms->desc();
        // The cursor plane the allocation picked for this CRTC: the one usable on it that
        // no other pipeline holds.
        unsigned want = 0;
        for (unsigned k = 0; k < kms.plane_count() && !want; ++k) {
            const drm::kms_plane_info pl = kms.plane(k);
            if (pl.kind != drm::plane_kind::cursor || !(pl.possible_crtcs & (1u << d.crtc_index)))
                continue;
            bool taken = false;
            for (const auto& other : pipes_)
                taken = taken || (other && other.get() != &p && other->cursor.valid() &&
                                  other->cursor.caps().plane_id == pl.id);
            if (!taken)
                want = pl.id;
        }
        if (!want)
            continue;
        if (auto plane = drm::kms_cursor_plane::create(kms.card_fd(), d.crtc_id, d.crtc_index,
                                                       need_px, need_px, want);
            plane)
            p.cursor = std::move(plane).value();
    }
}

void compositor_impl::presentation_flip_handler(scheduler::presentation_feedback fb,
                                                void* user_data) {
    auto* p = static_cast<output_pipeline*>(user_data);
    if (!p || !p->impl)
        return;
    p->impl->on_output_flip(*p, fb);
}

void compositor_impl::on_output_flip(output_pipeline& p, const scheduler::presentation_feedback& fb) {
    const unsigned slot = p.slot;
    output_ui& o = outs_[slot];
    if (o.pipe != &p) {
        // A pipeline UI let go of (its output went): its last flip landed, so render can turn
        // its CRTC off and hand it back now. Nothing of it is presented or paced any more.
        if (fb.vsync) {
            const unsigned flipped = p.pending_flip_slot.load(std::memory_order_acquire);
            if (flipped < p.slot_count)
                p.front.store(flipped, std::memory_order_release);
        }
        wake_render();
        return;
    }
    const bool gated = display_gate_.load(std::memory_order_relaxed) != 0 ||
                       (gate_power_mask_.load(std::memory_order_relaxed) & (1u << slot)) != 0;
    // A flip of a commit made before the pause (or the power-off), drained after it: the
    // slot it showed is noted, its timing dropped. It must not restart the frame clock or
    // pace callbacks.
    if (gated) {
        p.committed_submit_us.store(0, std::memory_order_relaxed);
        (void)p.committed_input_us.exchange(0, std::memory_order_acq_rel);
        if (const int fd = p.committed_fence_fd.exchange(-1, std::memory_order_acq_rel); fd >= 0)
            ::close(fd);
        // Its presentation feedback was discarded at the pause edge (nothing presented
        // while the session is away — wlroots reports such flips `presented = false`).
        if (fb.vsync) {
            const unsigned flipped = p.pending_flip_slot.load(std::memory_order_acquire);
            if (flipped < p.slot_count)
                p.front.store(flipped, std::memory_order_release);
        }
        // A power-off waiting for this flip (the CRTC takes one commit at a time) goes now.
        if (o.power.waiting_for_flip() && !(p.kms && p.kms->flip_pending()))
            apply_power_actions(slot, o.power.on_flip_drained());
        return;
    }
    // The flip landed, so clients may draw again regardless of how complete the feedback is
    // — unless it only moved the pointer: no client content was presented by it.
    if (p.last_commit_content.load(std::memory_order_acquire))
        o.present_completed = true;
    o.awaiting_resume_flip = false;
    o.last_feedback = fb;

    // Phase-lock this output's frame clock to its display, and aim each repaint at its
    // deadline rather than at the interval boundary (`repaint_scheduler`). The phase comes
    // from the kernel's flip timestamp when it is on our clock and plausibly recent;
    // otherwise from `now`, which is within the 1 ms loop pump of the flip because flips
    // are drained there.
    if (vsync_timer) {
        const auto now = now_ns();
        scheduler::repaint_scheduler& clock = clocks_.clock(slot);
        clock.observe_repaint(budget_.last_tick_ui_us(),
                              p.render_cost_us.load(std::memory_order_relaxed));
        auto flip_ns = now;
        if (fb.hw_clock && fb.actual_present_ns > 0 && fb.actual_present_ns <= now &&
            now - fb.actual_present_ns < clock.refresh_period())
            flip_ns = fb.actual_present_ns;
        clocks_.on_present(slot, flip_ns, now);
        rearm_frame_timer();
    }

    // Taken on every flip, sampled or not, so the dup'd fence is always closed.
    const unsigned long long submit_us = p.committed_submit_us.exchange(0, std::memory_order_acq_rel);
    unsigned long long fence_us = 0;
    if (const int fd = p.committed_fence_fd.exchange(-1, std::memory_order_acq_rel); fd >= 0) {
        fence_us = sync_file_signal_us(fd);
        ::close(fd);
    }
    lx::trace::timeline_instant("kms", "flip");
    if (fb.hw_clock)
        note_present_latency(p, fb.actual_present_ns, submit_us, fence_us);

    if (!fb.vsync) {
        fire_flip_callbacks(slot);
        return;
    }
    const unsigned flipped = p.pending_flip_slot.load(std::memory_order_acquire);
    if (flipped < p.slot_count)
        p.front.store(flipped, std::memory_order_release);

    // wp_presentation: the composite of frame `tag` is on this output now. A kernel flip
    // event is vsync'd and hardware-signalled; its time is the hardware's (hw_clock) when
    // the driver timestamps vblanks, with the CRTC's vblank counter as MSC. A driver
    // without vblank support samples the time when it sends the event and has no counter
    // (seq 0, as the spec asks): no hw_clock. A blocking modeset's completion is timed
    // here, in software: vsync only. Only feedback this output paces is presented (M8).
    if (fb.content && fb.tag != 0) {
        // Health: presented, and missed when it landed half a period or more past its aim
        // (a whole vblank late). Only the frame the aim was noted for is judged.
        ++o.presented;
        if (o.aim_frame == fb.tag && o.aim_ns > 0) {
            const auto period = clocks_.clock(slot).refresh_period();
            const auto landed = fb.hw_clock && fb.actual_present_ns > 0
                                    ? static_cast<lx::runtime::clock_time>(fb.actual_present_ns)
                                    : now_ns();
            const bool late = period > 0 && landed - o.aim_ns > period / 2;
            if (late)
                ++o.missed;
            o.recent.record(late);
            o.aim_ns = 0;
        }
        present_info info{};
        info.timestamp_ns = static_cast<unsigned long long>(fb.actual_present_ns);
        info.seq = fb.vblank_clock ? fb.seq : 0;
        info.flags = k_present_vsync;
        if (fb.hw_clock)
            info.flags |= k_present_hw_completion;
        if (fb.vblank_clock)
            info.flags |= k_present_hw_clock;
        if (outputs.slot_live(slot)) {
            const unsigned mhz = outputs.slot_state(slot).refresh_mhz;
            if (mhz > 0)
                info.refresh_ns = static_cast<unsigned>((1'000'000'000'000ull + mhz / 2) / mhz);
        }
        presentation_flipped(p0_, fb.tag, slot, info);
        // ext-session-lock (M14): a locked frame is on this output.
        if (lock_.state() == lock_state::locking && static_cast<int>(fb.tag - o.lock_frame) >= 0)
            note_lock_shown(slot);
    }
    fire_flip_callbacks(slot);
}

void compositor_impl::fire_flip_callbacks(unsigned slot) {
    // The frame callbacks this output paces are answered after its flip: what their surfaces
    // committed is on screen now. Answered at the flip itself, a client that draws at once
    // commits a whole period before the repaint that shows it, so commit-to-present is a
    // full refresh (wlroots, at the output's frame event). Answered at the repaint start,
    // the reply misses that repaint: a frame every two periods. So they are held until the
    // repaint start minus what this output's clients have recently needed to answer, plus a
    // margin (sway's per-view max_render_time, measured rather than configured): the commit
    // lands just before the repaint. Until an answer was measured they go out at once; a
    // client that answers late raises the lead for the next frame, and one slower than the
    // whole period is answered at the flip as before.
    output_ui& o = outs_[slot];
    if (!o.present_completed)
        return;
    const lx::runtime::clock_time lead = client_lead_ns(slot);
    if (lead > 0 && (clocks_.running_mask() & (1u << slot)) != 0) {
        const lx::runtime::clock_time release = clocks_.start_of(slot) - lead;
        if (release > now_ns() + k_callback_hold_min_ns) {
            o.callback_release_ns = release;
            rearm_callback_timer();
            return;
        }
    }
    release_flip_callbacks(slot);
}

void compositor_impl::release_flip_callbacks(unsigned slot) {
    output_ui& o = outs_[slot];
    const bool held = o.callback_release_ns != 0;
    o.callback_release_ns = 0;
    if (held)
        rearm_callback_timer();
    if (!o.present_completed)
        return;
    const auto now = now_ns();
    o.present_completed = false;
    o.awaiting_response = true;
    o.last_callback_ns = now;
    unsigned ms = static_cast<unsigned>(static_cast<unsigned long long>(now) / 1'000'000ull);
    if (o.last_feedback.hw_clock && o.last_feedback.actual_present_ns > 0)
        ms = static_cast<unsigned>(o.last_feedback.actual_present_ns / 1'000'000ull);
    fire_output_frame_callbacks(p0_, slot, ms, static_cast<unsigned long long>(now));
}

lx::runtime::clock_time compositor_impl::client_lead_ns(unsigned slot) const {
    const output_ui& o = outs_[slot];
    if (o.response_count == 0)
        return 0;
    lx::runtime::clock_time peak = 0;
    for (unsigned i = 0; i < o.response_count; ++i)
        if (o.response_ns[i] > peak)
            peak = o.response_ns[i];
    const lx::runtime::clock_time lead = peak + k_client_margin_ns;
    return lead < clocks_.clock(slot).refresh_period() ? lead : 0;
}

void compositor_impl::note_client_response(std::uint32_t fresh) {
    const auto now = now_ns();
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        output_ui& o = outs_[slot];
        if (!(fresh & (1u << slot)) || !o.awaiting_response || o.last_callback_ns == 0)
            continue;
        o.awaiting_response = false;
        const lx::runtime::clock_time dt = now - o.last_callback_ns;
        // Longer than a period is not an answer to that callback but later, unrelated work.
        if (dt <= 0 || dt >= clocks_.clock(slot).refresh_period())
            continue;
        o.response_ns[o.response_next] = dt;
        o.response_next = (o.response_next + 1) % output_ui::k_response_window;
        if (o.response_count < output_ui::k_response_window)
            ++o.response_count;
    }
}

void compositor_impl::release_due_callbacks() {
    const bool paused = display_gate_.load(std::memory_order_relaxed) != 0;
    const std::uint32_t dark = clocks_.dark_mask();
    const auto now = now_ns();
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        output_ui& o = outs_[slot];
        if (o.callback_release_ns == 0)
            continue;
        // Paused or dark: the paused tick and the power state answer them instead.
        if (paused || !o.live || (dark & (1u << slot))) {
            o.callback_release_ns = 0;
            continue;
        }
        if (o.callback_release_ns <= now + k_callback_hold_min_ns)
            release_flip_callbacks(slot);
    }
    rearm_callback_timer();
}

void compositor_impl::rearm_callback_timer() {
    if (!callback_timer_)
        return;
    lx::runtime::clock_time next = 0;
    for (const auto& o : outs_)
        if (o.callback_release_ns != 0 && (next == 0 || o.callback_release_ns < next))
            next = o.callback_release_ns;
    if (next != 0)
        callback_timer_->schedule_at(next);
    else
        callback_timer_->set_enabled(false);
}

void compositor_impl::callback_timer_callback() {
    if (active)
        active->release_due_callbacks();
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
    // Uevents while away are owed to the resume, which probes everything (not master now:
    // the kernel would only answer from its cache).
    hotplug_.set_paused(true);
    resume_probe_wait_ = false;
    if (hotplug_timer_)
        hotplug_timer_->set_enabled(false);
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
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        clocks_.stop(s);
    if (vsync_timer)
        vsync_timer->set_enabled(false);
    if (housekeeping_timer)
        housekeeping_timer->schedule_at(now_ns() + k_housekeeping_ns);
    // 4. End what a release must not complete: a move/resize ends as Escape would, a drag
    //    is cancelled. A half-typed tap is no tap.
    wm_.cancel_for_pause();
    keys_.reset();
    cancel_hold("session paused");
}

void compositor_impl::suspend_input_for_pause() {
    auto& seat = input_devices_.default_seat();
    p0_.session_paused = true;
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
    // 7. Pointer constraints end while the session is away (a persistent one holds again
    //    after the resume).
    pointer_constraints_update(p0_);
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
    p0_.session_paused = false;
    input_router_.resume_keyboard_focus(seat);
    pointer_constraints_update(p0_);
}

void compositor_impl::begin_display_resume(unsigned gen) {
    // What is connected now comes first (ADR 008 §A.14): a monitor plugged in, unplugged, or
    // a VM window resized while the session was away — the resume modeset uses it, rather
    // than failing on a mode that is gone and leaving the display off. Only the master's
    // probe is a forced one, so it could not run while away; it waits ≤ 250 ms.
    hotplug_.set_paused(false);
    if (hotplug_ready_ && drm_present_ && !card_gone_.load(std::memory_order_acquire)) {
        resume_probe_wait_ = true;
        resume_probe_gen_ = gen;
        if (hotplug_timer_)
            hotplug_timer_->schedule_at(now_ns() + k_resume_probe_wait_ns);
        hotplug_.request_full(true);
        return; // `scan_ready` (or the timer) finishes the resume
    }
    finish_display_resume(gen);
}

void compositor_impl::finish_display_resume(unsigned gen) {
    if (hotplug_timer_)
        hotplug_timer_->set_enabled(false);
    // A flip completed before the pause is consumed under the old generation first, which
    // keeps the order obvious in the log (the cookies make it safe either way).
    if (drm_present_)
        kms.dispatch_events();
    // The VT coming back is activity (I2), and it brings every dark output back on: this
    // resume's own modeset turns each CRTC on, so the power gates just go.
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (outs_[s].live)
            apply_power_actions(s, outs_[s].power.on_vt_resumed());
    const unsigned render_gen = ++render_gen_counter_;
    vt_resume_render_gen_ = render_gen;
    vt_resume_session_gen_ = gen;
    vt_resume_outcome_ = session_resume_outcome::ok;
    vt_resume_pending_ = 0;
    // Every output with a CRTC answers this resume (its result comes from its first commit,
    // the combined modeset of all of them); with none, the resume is answered at once.
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outs_[s].live)
            continue;
        resume_gen_[s].store(render_gen, std::memory_order_release);
        render_posts_.fetch_add(1, std::memory_order_acq_rel);
        if (outs_[s].pipe && outs_[s].pipe->kms)
            vt_resume_pending_ |= 1u << s;
    }
    display_gate_.fetch_and(~gate_vt, std::memory_order_release);
    resume_input_after_pause();
    wm_.set_suspend_reason(window_manager::suspend_vt, false);
    if (idle_.note_activity(now_ns(), idle_events(p0_)))
        rearm_idle_timer();
    // Another master drew on the CRTCs: every pixel is repainted.
    scene.note_full_damage();
    if (housekeeping_timer)
        housekeeping_timer->set_enabled(false);
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        outs_[s].last_callback_ns = 0;
        outs_[s].present_completed = false;
        outs_[s].awaiting_resume_flip = outs_[s].live;
    }
    if (vt_resume_pending_ == 0 && drm_present_)
        apply_session_actions(session_.on_resume_result(gen, session_resume_outcome::ok));
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
    // Per input event: one clock read, one store, and a branch per output on its power.
    if (idle_.note_activity(static_cast<unsigned long long>(now_ns()), idle_events(p0_)))
        rearm_idle_timer();
    if (gate_power_mask_.load(std::memory_order_relaxed) == 0) {
        bool any = false;
        for (unsigned s = 0; s < output_manager::k_max && !any; ++s)
            any = outs_[s].live && outs_[s].power.wants_wake();
        if (!any)
            return false;
    }
    // I5: input wakes every output the compositor powered off.
    const bool vt_away = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
    bool woke = false;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outs_[s].live || !outs_[s].power.wants_wake())
            continue;
        woke = true;
        apply_power_actions(s, outs_[s].power.wake(vt_away));
    }
    if (woke)
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.power",
                                        "display woken by input");
    return woke;
}

void compositor_impl::request_output_power(unsigned slot, bool on) {
    if (slot >= output_manager::k_max || !outs_[slot].live)
        return;
    const bool vt_away = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
    apply_power_actions(slot, on ? outs_[slot].power.request_on(vt_away)
                                 : outs_[slot].power.request_off(vt_away));
}

void compositor_impl::apply_power_actions(unsigned slot, const power_actions& a) {
    if (a.empty() || slot >= output_manager::k_max)
        return;
    output_ui& o = outs_[slot];
    const std::uint32_t bit = 1u << slot;
    char line[160];
    const char* name = outputs.slot_live(slot) ? outputs.slot_state(slot).name : "?";
    if (a.has(power_actions::gate)) {
        // This output stops as at a VT pause (A.3 step 3), but input stays live: it is what
        // wakes it. The other outputs go on. Keyboard focus is kept.
        power_gen_[slot].store(o.power.generation(), std::memory_order_release);
        gate_power_mask_.fetch_or(bit, std::memory_order_release);
        presentation_discard_output(p0_, slot);
        clocks_.set_dark(slot, true);
        rearm_frame_timer();
        o.awaiting_resume_flip = false;
        // Windows are suspended only once every output is dark (a window on one blanked
        // monitor and one lit monitor is not, §A.6).
        const std::uint32_t live = outputs.live_mask();
        if ((gate_power_mask_.load(std::memory_order_relaxed) & live) == live)
            wm_.set_suspend_reason(window_manager::suspend_power, true);
        lx::trace::timeline_instant("ui", "power.gate");
    }
    if (a.has(power_actions::post_off))
        post_power_off(slot, a.gen);
    if (a.has(power_actions::ungate)) {
        // Whatever off task is still in flight is stale from here on.
        power_gen_[slot].store(o.power.generation(), std::memory_order_release);
        wm_.set_suspend_reason(window_manager::suspend_power, false);
        if (a.resume) {
            // The CRTC is down: the resume procedure (full repaint, a full modeset with
            // ACTIVE=1) brings it back. The generation is stored before the gate opens.
            const unsigned render_gen = ++render_gen_counter_;
            o.power_resume_render_gen = render_gen;
            resume_gen_[slot].store(render_gen, std::memory_order_release);
            render_posts_.fetch_add(1, std::memory_order_acq_rel);
        }
        gate_power_mask_.fetch_and(~bit, std::memory_order_release);
        clocks_.set_dark(slot, false);
        if ((display_gate_.load(std::memory_order_relaxed) & gate_vt) == 0) {
            scene.note_full_damage_output(slot);
            o.last_callback_ns = 0;
            o.present_completed = false;
            o.awaiting_resume_flip = a.resume;
            request_frame(bit);
        }
        lx::trace::timeline_instant("ui", "power.ungate");
    }
    if (a.has(power_actions::wait_flip) &&
        !(o.pipe && o.pipe->kms && o.pipe->kms->flip_pending()))
        apply_power_actions(slot, o.power.on_flip_drained());
    if (a.has(power_actions::send_mode))
        notify_output_power(p0_, slot, a.mode_on);
    if (a.has(power_actions::log_off)) {
        std::snprintf(line, sizeof(line), "%s (output %u %s)",
                      (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0
                          ? "display power off (session away: nothing to commit)"
                          : "display power off (CRTC ACTIVE=0, cursor plane off)",
                      slot, name);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.power", line);
    }
    if (a.has(power_actions::log_soft_blank)) {
        std::snprintf(line, sizeof(line),
                      "display power off refused by the driver — showing black instead (no power "
                      "saving) (output %u %s)",
                      slot, name);
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.power", line);
    }
    if (a.has(power_actions::log_on)) {
        std::snprintf(line, sizeof(line), "display power on (output %u %s)", slot, name);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.power", line);
    }
    if (a.has(power_actions::log_on_failed)) {
        std::snprintf(line, sizeof(line),
                      "display power on: the modeset was refused (the mode is no longer "
                      "offered?) — output %u %s stays dark until the next input",
                      slot, name);
        lx::trace::logger::global().log(lx::trace::level::error, "compositor.power", line);
    }
    // A lock under way when the output went dark is locked now if nothing else shows.
    if (a.has(power_actions::log_off) || a.has(power_actions::log_soft_blank))
        note_lock_shown(slot);
    advance_lock();
    wayland.flush();
}

void compositor_impl::post_power_off(unsigned slot, unsigned gen) {
    power_gen_[slot].store(gen, std::memory_order_release);
    if (!threads_started.load(std::memory_order_acquire) || !drm_present_ || !outs_[slot].pipe ||
        !outs_[slot].pipe->kms) {
        apply_power_actions(slot, outs_[slot].power.on_off_result(gen, power_off_outcome::deferred));
        return;
    }
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::render, [self, slot, gen] {
        const power_off_outcome outcome = self->run_power_off(slot, gen);
        (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, gen, outcome] {
            self->apply_power_actions(slot, self->outs_[slot].power.on_off_result(gen, outcome));
        });
    });
}

power_off_outcome compositor_impl::run_power_off(unsigned slot, unsigned gen) {
    // Render thread, after a tick: the output's gate is up, so nothing else commits on it.
    if (power_gen_[slot].load(std::memory_order_acquire) != gen)
        return power_off_outcome::stale;
    if ((display_gate_.load(std::memory_order_acquire) & gate_vt) != 0)
        return power_off_outcome::deferred;
    output_pipeline* p = pipes_[slot].get();
    if (!p || !p->kms)
        return power_off_outcome::deferred;
    if (p->retiring)
        return power_off_outcome::stale;
    if (p->kms->flip_pending())
        return power_off_outcome::retry_after_flip;
    // Nothing of ours is lit: never modeset yet, or the CRTC is still off from the previous
    // power-off (a resume whose modeset has not happened).
    if (!p->content_committed_once)
        return power_off_outcome::deferred;
    lx::trace::scoped_span span{"render", "kms.power_off"};
    auto off = p->kms->commit_power_off(p->cursor.valid() ? &p->cursor : nullptr);
    if (off) {
        // What the plane shows is unknown to it now: the power-on modeset states it in full.
        if (p->cursor.valid())
            p->cursor.invalidate();
        return power_off_outcome::done;
    }
    if (p->kms->last_failure_lost_master())
        return power_off_outcome::deferred; // a VT switch raced the request
    if (!power_rejected_logged_) {
        power_rejected_logged_ = true;
        lx::trace::logger::global().log_error(off.get_error(), "compositor.power");
    }
    commit_black_frame(*p);
    return power_off_outcome::rejected;
}

void compositor_impl::commit_black_frame(output_pipeline& p) {
    if (p.slot_count == 0 || !p.kms || p.kms->flip_pending())
        return;
    const unsigned slot = p.back_slot();
    gfx::damage_region region{};
    region.set_full();
    region.clip(p.scanout_rect);
    bool drawn = false;
    int fence = -1;
    const lx::color black = lx::color::rgb(0.f, 0.f, 0.f);
    if (gl_present_) {
        if (auto c = gl_compositor_.composite_region(p.gl[slot], black, nullptr, 0, region); c) {
            drawn = true;
            fence = c.value().out_fence_fd;
        }
    } else if (cpu_present_) {
        const auto& fb = p.dumb[slot];
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
        if (auto c = vk_compositor_.composite(p.vk[slot], black, nullptr, 0); c) {
            drawn = true;
            fence = c.value().out_fence_fd;
        }
    }
    if (!drawn)
        return;
    drm::atomic_commit_request req{};
    req.request_page_flip = true;
    req.in_fence_fd = fence;
    req.damage.full_frame = true;
    req.framebuffer_id = p.framebuffer(slot, cpu_present_);
    if (p.cursor.valid()) {
        p.cursor.set_visible(false);
        req.cursor = &p.cursor;
    }
    // Its flip lands while gated: it rotates the slot and answers nothing.
    p.pending_flip_slot.store(slot, std::memory_order_release);
    p.last_commit_content.store(false, std::memory_order_release);
    if (auto committed = p.kms->commit(req); committed && p.kms->completion_needs_dispatch())
        post_flip_dispatch();
    if (fence >= 0)
        ::close(fence);
    // Every slot is stale against what a power-on shows (the resume repaints it all anyway).
    for (unsigned i = 0; i < output_pipeline::k_slots; ++i)
        p.slot_age[i].set_full();
    p.kms_pending.set_full();
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
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.lock",
                                     "sleep may proceed ({})", why);
}

bool compositor_impl::output_dark(unsigned slot) const {
    if ((display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0)
        return true; // another VT (or nothing) is on the CRTCs
    if (slot >= output_manager::k_max || !outs_[slot].live)
        return true;
    // Powered off, or the black frame of a refused power-off — not while the off task is
    // still pending (the last frame is on screen until it runs).
    const power_phase ph = outs_[slot].power.phase();
    if (ph == power_phase::off || ph == power_phase::soft_blank)
        return true;
    // KMS, and nothing of ours ever reached this CRTC: the console is on screen.
    const output_pipeline* p = outs_[slot].pipe;
    return drm_present_ && p && p->kms && !p->ever_lit.load(std::memory_order_acquire);
}

bool compositor_impl::display_dark() const {
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (outs_[s].live && !output_dark(s))
            return false;
    return true;
}

void compositor_impl::note_lock_shown(unsigned slot) {
    if (lock_.state() != lock_state::locking)
        return;
    lock_shown_.shown(slot);
    advance_lock();
}

void compositor_impl::advance_lock() {
    if (lock_.state() != lock_state::locking)
        return;
    // ext-session-lock: `locked` once a locked frame is on every live output (M14) — an
    // output that is dark counts, nothing of the session being visible on it.
    std::uint32_t dark = 0;
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (outs_[s].live && output_dark(s))
            dark |= 1u << s;
    if (lock_shown_.complete(outputs.live_mask(), dark))
        apply_lock_actions(lock_.locked_frame_shown());
}

void compositor_impl::apply_lock_actions(const lock_actions& a) {
    // Screen capture follows the lock from the request on (CC7): jobs already with render
    // are cancelled, every session's next frame is the curtain.
    capture_lock_check(p0_);
    if (a.empty())
        return;
    const lx::color black = lx::color::rgb(0.f, 0.f, 0.f);
    if (a.has(lock_actions::input_lock)) {
        // L1: input leaves normal clients at the request, before anything is hidden. The
        // lock layer stays hidden until the scene locks (lock surfaces are not shown before).
        lock_layer_.set_visible(false);
        cancel_hold("session locked");
        keys_.set_locked(true);
        // A repeating binding stops here unless it is allowed when locked (its timer checks
        // too; this keeps the next tick from being scheduled at all).
        if (repeat_kind_ == key_repeat::binding && repeat_binding_ &&
            !keys_.allows(*repeat_binding_))
            stop_binding_repeat();
        wm_.set_session_locked(true);
    }
    if (a.has(lock_actions::scene_lock)) {
        for (scene::container_node* layer :
             {&background_layer_, &bottom_layer_, &windows_layer_, &snap_preview_layer_,
              &top_layer_, &fullscreen_layer_, &overlay_shell_layer_, &popup_layer_,
              &dnd_icon_layer_})
            layer->set_visible(false);
        lock_layer_.set_visible(true);
        // The first snapshot built from here on is a locked frame, on every output.
        for (auto& o : outs_)
            o.lock_frame = p0_.building_frame;
        lock_shown_.begin();
        wm_.set_suspend_reason(window_manager::suspend_lock, true);
        scene.note_full_damage();
        request_frame();
    }
    // The wire half: finished, locked, and lock surfaces dropped when the lock ended.
    session_lock_send(p0_, a);
    if (a.has(lock_actions::scene_unlock)) {
        for (scene::container_node* layer :
             {&background_layer_, &bottom_layer_, &windows_layer_, &snap_preview_layer_,
              &top_layer_, &fullscreen_layer_, &overlay_shell_layer_, &popup_layer_,
              &dnd_icon_layer_})
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
    // A pointer constraint ends at the lock request and may hold again after the unlock.
    if (a.has(lock_actions::input_lock) || a.has(lock_actions::input_unlock))
        pointer_constraints_update(p0_);
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
        lx::trace::logger::global().logf(
            logged_lock_state_ == lock_state::abandoned ? lx::trace::level::warn
                                                        : lx::trace::level::info,
            "compositor.lock", "session lock: {} -> {}", lock_state_name(from),
            lock_state_name(logged_lock_state_));
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

void compositor_impl::run_resume_procedure(output_pipeline& p, unsigned gen) {
    p.discard_fence();
    p.composited_slot = -1;
    p.frame_outstanding.store(false, std::memory_order_release);
    if (p.kms)
        p.kms->prepare_resume();
    if (p.cursor.valid())
        p.cursor.invalidate();
    p.content_committed_once = false;
    for (unsigned i = 0; i < output_pipeline::k_slots; ++i)
        p.slot_age[i].set_full();
    p.kms_pending.set_full();
    p.missed.set_full();
    p.resume_done_gen = gen;
    p.resume_result_pending = p.kms != nullptr;
    p.modeset_wait_since_ns = 0;
}

void compositor_impl::post_resume_result(unsigned slot, unsigned gen,
                                         session_resume_outcome outcome) {
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, gen, outcome] {
        // `gen` is the render generation: it answers the VT resume, this output's power-on,
        // or both (whichever asked for it last).
        if (gen == self->vt_resume_render_gen_ && (self->vt_resume_pending_ & (1u << slot))) {
            self->vt_resume_pending_ &= ~(1u << slot);
            // The worst outcome over every output: one refused modeset is a refused resume.
            if (outcome == session_resume_outcome::rejected ||
                (outcome == session_resume_outcome::device_unavailable &&
                 self->vt_resume_outcome_ == session_resume_outcome::ok))
                self->vt_resume_outcome_ = outcome;
            if (self->vt_resume_pending_ == 0)
                self->apply_session_actions(self->session_.on_resume_result(
                    self->vt_resume_session_gen_, self->vt_resume_outcome_));
        }
        if (slot < output_manager::k_max && gen == self->outs_[slot].power_resume_render_gen)
            self->apply_power_actions(
                slot, self->outs_[slot].power.on_resume_result(
                          outcome == session_resume_outcome::ok ? power_on_outcome::ok
                          : outcome == session_resume_outcome::rejected
                              ? power_on_outcome::rejected
                              : power_on_outcome::device_unavailable));
    });
}

void compositor_impl::post_flip_dispatch() {
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::ui,
                                           [self] { self->kms.dispatch_events(); });
}

// ── Hotplug: output plans UI → render → UI (ADR 008 §A.4, Part B) ─────────────

void compositor_impl::wake_render() {
    {
        std::lock_guard lock{render_mutex};
        render_pending = true;
    }
    render_cv.notify_one();
}

void compositor_impl::post_output_op(const output_op& op) {
    {
        std::lock_guard lock{ops_mutex_};
        if (ops_.size() >= k_max_ops) {
            // Bounded by the slots (one op in flight per slot and a few behind it): not
            // reachable by any sequence of uevents, which coalesce before they get here.
            lx::trace::logger::global().log(lx::trace::level::error, "compositor.hotplug",
                                            "output op queue full — op dropped");
            return;
        }
        ops_.push_back(op);
    }
    ops_pending_.store(true, std::memory_order_release);
    if (threads_started.load(std::memory_order_acquire))
        wake_render();
    else
        run_output_ops(); // before the render thread exists, UI stands in for it
}

void compositor_impl::run_output_ops() {
    if (!ops_pending_.exchange(false, std::memory_order_acq_rel))
        return;
    unsigned n = 0;
    bool test = false;
    {
        std::lock_guard lock{ops_mutex_};
        ops_work_ = ops_;
        ops_.clear();
        n = static_cast<unsigned>(ops_work_.size());
        if (config_test_pending_) {
            config_test_work_ = config_test_;
            config_test_pending_ = false;
            test = true;
        }
    }
    // Retires first: the CRTCs, planes and bandwidth they hold are free before any add in the
    // same batch allocates and tests (an output configuration that disables one output and
    // enables another posts the add first — §C.3). Only a retire that is the first op of its
    // slot moves ahead: one cancelling an add of the same batch stays behind it.
    bool done[k_max_ops]{};
    for (unsigned i = 0; i < n; ++i) {
        const output_op& op = ops_work_[i];
        if (op.what != output_op::kind::retire || op.slot >= output_manager::k_max ||
            (ops_deferred_slots_ & (1u << op.slot)))
            continue;
        bool first = true;
        for (unsigned k = 0; k < i && first; ++k)
            first = ops_work_[k].slot != op.slot;
        if (!first)
            continue;
        run_output_op(op);
        done[i] = true;
    }
    for (unsigned i = 0; i < n; ++i)
        if (!done[i])
            run_or_defer_output_op(ops_work_[i]);
    if (test)
        render_test_config(config_test_work_);
}

bool compositor_impl::kms_retire_in_flight() const {
    for (const auto& up : pipes_)
        if (up && up->retiring && up->kms)
            return true;
    return false;
}

void compositor_impl::run_or_defer_output_op(const output_op& op) {
    if (op.slot >= output_manager::k_max)
        return;
    const std::uint32_t bit = 1u << op.slot;
    const bool hold =
        (ops_deferred_slots_ & bit) ||
        (op.what == output_op::kind::add && op.kms && kms_retire_in_flight() &&
         (ops_deferred_.empty() || now_ns() - ops_deferred_since_ns_ < k_ops_defer_max_ns));
    if (!hold || ops_deferred_.size() >= k_max_ops) {
        run_output_op(op);
        return;
    }
    if (ops_deferred_.empty())
        ops_deferred_since_ns_ = now_ns();
    ops_deferred_.push_back(op);
    ops_deferred_slots_ |= bit;
}

void compositor_impl::run_deferred_output_ops() {
    if (ops_deferred_.empty())
        return;
    if (kms_retire_in_flight() && now_ns() - ops_deferred_since_ns_ < k_ops_defer_max_ns)
        return;
    const std::inplace_vector<output_op, k_max_ops> held = ops_deferred_;
    ops_deferred_.clear();
    ops_deferred_slots_ = 0;
    // Still a retire in flight: the wait timed out — they run now, in order, regardless.
    const bool timed_out = kms_retire_in_flight();
    for (const output_op& op : held) {
        if (timed_out)
            run_output_op(op);
        else
            run_or_defer_output_op(op);
    }
}

void compositor_impl::run_output_op(const output_op& op) {
    if (op.slot >= output_manager::k_max)
        return;
    output_pipeline* p = pipes_[op.slot].get();
    switch (op.what) {
    case output_op::kind::add:
        render_add_output(op);
        break;
    case output_op::kind::test_mode:
        render_test_mode(op);
        break;
    case output_op::kind::switch_mode:
        if (p && !p->retiring && op.mode_count > 0) {
            p->switch_pending = true;
            p->switch_mode = op.modes[0];
            p->switch_from_frame = op.from_frame;
            p->switch_ticket = op.ticket;
            // The same configuration's adaptive-sync change queued before it: the
            // switch's modeset answers for both.
            if (op.ticket && p->apply_ticket == op.ticket)
                p->apply_ticket = 0;
        } else if (op.ticket) {
            compositor_impl* self = this;
            const unsigned slot = op.slot;
            const unsigned ticket = op.ticket;
            (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, ticket] {
                self->on_modeset_result(slot, ticket, false);
            });
        }
        break;
    case output_op::kind::set_vrr:
        if (p && !p->retiring && p->kms) {
            if (p->switch_pending) {
                // A mode switch is on its way: VRR goes with its modeset, which carries
                // the configuration's ticket (a modeset still in the old mode must not
                // answer it).
                p->switch_vrr = op.vrr ? 1 : 0;
                if (op.ticket && !p->switch_ticket)
                    p->switch_ticket = op.ticket;
            } else {
                p->kms->set_vrr(op.vrr);
                p->apply_ticket = op.ticket;
            }
        } else if (op.ticket) {
            compositor_impl* self = this;
            const unsigned slot = op.slot;
            const unsigned ticket = op.ticket;
            const bool ok = p != nullptr; // composite-only: nothing to program
            (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, ticket, ok] {
                self->on_modeset_result(slot, ticket, ok);
            });
        }
        break;
    case output_op::kind::retire:
        if (!p) {
            compositor_impl* self = this;
            const unsigned slot = op.slot;
            const unsigned gen = op.gen;
            (void)runtime::executor::global().post(
                runtime::affinity::ui, [self, slot, gen] { self->on_retired(slot, gen); });
        } else {
            p->retiring = true;
            p->retire_gen = op.gen;
            p->retire_connector_gone = op.connector_gone;
            // At once when nothing is in flight on its CRTC: an output configuration that
            // disables this output and enables another may need its CRTC for the next op.
            finish_retire(op.slot);
        }
        break;
    }
}

namespace {
void release_targets(output_pipeline& p) {
    for (unsigned i = 0; i < output_pipeline::k_slots; ++i) {
        p.gl[i] = gfx::gl_scanout_target{};
        p.vk[i] = gfx::render_target{};
        p.fb[i] = drm::kms_framebuffer{};
        p.dumb[i] = drm::kms_dumb_framebuffer{};
    }
    p.slot_count = 0;
}
} // namespace

void compositor_impl::render_add_output(const output_op& op) {
    const unsigned slot = op.slot;
    compositor_impl* self = this;
    const unsigned gen = op.gen;
    const auto answer = [self, slot, gen](bool ok, unsigned index, output_pipeline* pipe) {
        (void)runtime::executor::global().post(runtime::affinity::ui,
                                               [self, slot, gen, ok, index, pipe] {
                                                   self->on_add_result(slot, gen, ok, index, pipe);
                                               });
    };
    if (pipes_[slot]) {
        // The slot is reserved until its last pipeline is retired: not reachable.
        answer(false, 0, nullptr);
        return;
    }
    auto p = std::make_unique<output_pipeline>();
    p->impl = this;
    p->slot = slot;
    p->origin = op.origin;
    p->scale = op.scale;
    // A new output's frames have nothing to do with the slot's previous one.
    capture_renderer_.output_reset(slot);
    // A resume or power-on asked for before this pipeline existed is not its business.
    p->resume_done_gen = resume_gen_[slot].load(std::memory_order_acquire);
    p->first_frame = op.from_frame;
    p->has_first_frame = true;
    p->transform = op.transform;
    p->apply_ticket = op.ticket;
    bool ok = true;
    unsigned chosen = 0;
    const bool kms_out = op.kms && drm_present_ && !card_gone_.load(std::memory_order_acquire);
    if (kms_out) {
        ok = false;
        // The CRTC allocation for the new output alone (ADR 008 M7): every CRTC and plane
        // another pipeline holds is out of reach, and a TEST_ONLY of the new pipeline with
        // its real first framebuffer decides — the kernel tests it against the state the
        // other CRTCs are in, so this is the full state's test.
        unsigned used_crtcs = 0;
        std::inplace_vector<unsigned, 2 * output_manager::k_max> used_planes;
        for (const auto& up : pipes_) {
            if (!up || !up->kms)
                continue;
            used_crtcs |= 1u << up->kms->desc().crtc_index;
            used_planes.push_back(up->kms->desc().primary_plane_id);
            if (up->cursor.valid())
                used_planes.push_back(up->cursor.caps().plane_id);
        }
        drm::crtc_alloc_input in{};
        in.connector_count = 1;
        in.crtc_count = kms.crtc_count();
        in.connectors[0].possible_crtcs = op.possible_crtcs & ~used_crtcs;
        const int routed = op.routed_crtc_id ? kms.crtc_index_of(op.routed_crtc_id) : -1;
        in.connectors[0].routed_crtc = routed >= 0 && !(used_crtcs & (1u << routed)) ? routed : -1;
        in.plane_count = kms.plane_count() < drm::k_alloc_max_planes ? kms.plane_count()
                                                                      : drm::k_alloc_max_planes;
        for (unsigned k = 0; k < in.plane_count; ++k) {
            const drm::kms_plane_info pl = kms.plane(k);
            const bool taken = std::ranges::contains(used_planes, pl.id);
            in.planes[k].kind = pl.kind;
            in.planes[k].possible_crtcs = pl.possible_crtcs;
            in.planes[k].bound_crtc = pl.crtc_id ? kms.crtc_index_of(pl.crtc_id) : -1;
            in.planes[k].usable =
                !taken && (pl.kind == drm::plane_kind::cursor ? pl.argb8888 : pl.xrgb8888);
        }
        const bool master = kms.is_master();
        lx::size2i made{};
        unsigned tests = 0;
        // Adaptive sync asked for and refused in every mode: the modes again without it
        // (sway's `search_valid_config` order: modes first, then VRR off).
        bool vrr = op.vrr;
        for (unsigned j = 0; j < op.mode_count * (op.vrr ? 2u : 1u) && !ok; ++j) {
            const unsigned i = j % op.mode_count;
            vrr = op.vrr && j < op.mode_count;
            const drm::kms_mode& m = op.modes[i];
            const lx::size2i size{static_cast<int>(m.width), static_cast<int>(m.height)};
            if (size.width != made.width || size.height != made.height) {
                release_targets(*p);
                p->mode = size;
                if (auto targets = create_targets(*p, true); !targets) {
                    lx::trace::logger::global().log_error(targets.get_error(), "compositor.gfx");
                    made = {};
                    continue;
                }
                made = size;
            }
            crtc_test_ctx t{};
            t.impl = this;
            t.kms = &kms;
            t.count = 1;
            t.framebuffers[0] = p->framebuffer(0, cpu_present_);
            t.modes[0] = &m;
            t.connectors[0] = op.connector_id;
            t.vrr = vrr;
            drm::crtc_alloc_result r{};
            const bool assigned = drm::assign_crtcs(in, r, master ? &crtc_test : nullptr, &t);
            tests += r.tests;
            if (!assigned)
                continue;
            const drm::kms_pipeline_desc d = desc_for(kms, r, 0, op.connector_id);
            p->kms = std::make_unique<drm::kms_pipeline>(kms, d);
            p->kms->set_mode(m);
            p->kms->set_vrr(vrr);
            p->kms->set_framebuffer(p->framebuffer(0, cpu_present_));
            p->presentation.bind_page_flip(*p->kms);
            p->presentation.set_handler(&compositor_impl::presentation_flip_handler, p.get());
            int cursor_plane = -1;
            if (op.cursor_plane) {
                const int k = r.cursor_plane[0];
                if (k >= 0) {
                    if (auto plane = drm::kms_cursor_plane::create(
                            kms.card_fd(), d.crtc_id, d.crtc_index, cursor_plane_px_,
                            cursor_plane_px_, kms.plane(static_cast<unsigned>(k)).id);
                        plane) {
                        p->cursor = std::move(plane).value();
                        cursor_plane = static_cast<int>(kms.plane(static_cast<unsigned>(k)).id);
                    }
                }
                // A cursor drawn by a plane on some outputs and by the scene on others would
                // show twice where they meet: software everywhere (Part 1's rule).
                if (!p->cursor.valid())
                    fail_cursor_plane("a hotplugged output has no cursor plane");
            }
            chosen = i;
            ok = true;
            lx::trace::logger::global().logf(lx::trace::level::info, "compositor.drm",
                                             "output {} pipeline crtc={} (index {}) primary={} "
                                             "connector={} mode={}x{}@{} cursor_plane={:d} "
                                             "tests={}{}{}",
                                             slot, d.crtc_id, d.crtc_index, d.primary_plane_id,
                                             d.connector_id, m.width, m.height, m.refresh_mhz,
                                             cursor_plane, tests, i > 0 ? " (fallback mode)" : "",
                                             op.vrr ? (vrr ? " vrr" : " (vrr refused: off)") : "");
        }
        if (!ok)
            release_targets(*p);
    } else {
        p->mode = op.size;
        if (vulkan_present_ || gl_present_ || cpu_present_)
            if (auto targets = create_targets(*p, false); !targets)
                lx::trace::logger::global().log_error(targets.get_error(), "compositor.gfx");
    }
    output_pipeline* raw = p.get();
    if (ok)
        pipes_[slot] = std::move(p);
    answer(ok, chosen, ok ? raw : nullptr);
}

void compositor_impl::render_test_mode(const output_op& op) {
    const unsigned slot = op.slot;
    compositor_impl* self = this;
    const unsigned gen = op.gen;
    const auto answer = [self, slot, gen](bool ok, unsigned index) {
        (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, gen, ok, index] {
            self->on_mode_tested(slot, gen, ok, index);
        });
    };
    output_pipeline* p = pipes_[slot].get();
    if (!p || p->retiring || card_gone_.load(std::memory_order_acquire)) {
        answer(false, 0);
        return;
    }
    if (!p->kms) {
        answer(true, 0); // composite-only: nothing the kernel has to accept
        return;
    }
    // TEST_ONLY of the pipeline in each candidate mode with a real framebuffer of that size
    // (a test with real buffers, as sway's swapchain prepare): what passes is kept staged for
    // the switch. The pipeline itself keeps flipping in its current mode meanwhile.
    for (unsigned i = 0; i < op.mode_count; ++i) {
        const drm::kms_mode& m = op.modes[i];
        const lx::size2i size{static_cast<int>(m.width), static_cast<int>(m.height)};
        std::unique_ptr<output_pipeline> staged;
        if (p->staged && p->staged->mode.width == size.width && p->staged->mode.height == size.height)
            staged = std::move(p->staged);
        else {
            staged = std::make_unique<output_pipeline>();
            staged->mode = size;
            if (auto targets = create_targets(*staged, true); !targets) {
                lx::trace::logger::global().log_error(targets.get_error(), "compositor.gfx");
                continue;
            }
        }
        drm::modeset_entry e{};
        e.pipe = p->kms.get();
        e.mode = &m;
        e.framebuffer_id = staged->framebuffer(0, cpu_present_);
        e.request_page_flip = false;
        if (drm::commit_modeset(kms, &e, 1, true)) {
            p->staged = std::move(staged);
            p->staged_since_ns = now_ns();
            answer(true, i);
            return;
        }
        p->staged = std::move(staged); // reused when the next candidate has its size
    }
    p->staged.reset();
    answer(false, 0);
}

namespace {
/// A whole output configuration's TEST_ONLY (`render_test_config`): the entries of pipelines
/// that exist, and the outputs being enabled, whose CRTCs a candidate assignment names.
struct config_test_ctx {
    drm::kms_device* kms = nullptr;
    const drm::modeset_entry* fixed = nullptr;
    unsigned fixed_count = 0;
    unsigned count = 0;
    unsigned framebuffers[output_manager::k_max]{};
    const drm::kms_mode* modes[output_manager::k_max]{};
    unsigned connectors[output_manager::k_max]{};
    int vrr[output_manager::k_max]{};
};

bool config_crtc_test(const drm::crtc_alloc_result& r, void* user) {
    auto* t = static_cast<config_test_ctx*>(user);
    std::unique_ptr<drm::kms_pipeline> pipes[output_manager::k_max];
    std::inplace_vector<drm::modeset_entry, 2 * output_manager::k_max> entries;
    for (unsigned i = 0; i < t->fixed_count; ++i)
        entries.push_back(t->fixed[i]);
    for (unsigned i = 0; i < t->count; ++i) {
        // Unregistered: no event can reach a candidate, and no other thread sees it.
        pipes[i] = std::make_unique<drm::kms_pipeline>(
            *t->kms, desc_for(*t->kms, r, i, t->connectors[i]), false);
        pipes[i]->set_mode(*t->modes[i]);
        drm::modeset_entry& e = entries.emplace_back();
        e.pipe = pipes[i].get();
        e.framebuffer_id = t->framebuffers[i];
        e.request_page_flip = false;
        e.vrr = t->vrr[i] > 0 ? 1 : 0;
    }
    return static_cast<bool>(
        drm::commit_modeset(*t->kms, entries.data(), static_cast<unsigned>(entries.size()), true));
}
} // namespace

void compositor_impl::render_test_config(const config_test& t) {
    compositor_impl* self = this;
    const unsigned gen = t.gen;
    const auto answer = [self, gen](bool ok, unsigned tests) {
        (void)runtime::executor::global().post(runtime::affinity::ui, [self, gen, ok, tests] {
            self->on_config_tested(gen, ok, tests);
        });
    };
    const bool vt_paused = (display_gate_.load(std::memory_order_acquire) & gate_vt) != 0;
    if (!drm_present_ || card_gone_.load(std::memory_order_acquire) || vt_paused ||
        !kms.is_master()) {
        // Nothing can be asked of the kernel (the VT is away): the configuration cannot be
        // checked, so it is not taken (wlroots' commits fail the same way then).
        answer(t.count == 0, 0);
        return;
    }
    drm::modeset_entry fixed[output_manager::k_max]{};
    unsigned nf = 0;
    unsigned released_crtcs = 0; // CRTCs of outputs this configuration disables
    // Staged targets: a mode change's go to its pipeline for the switch; an enabled output's
    // are dropped after the test (its add makes its own).
    std::inplace_vector<std::unique_ptr<output_pipeline>, output_manager::k_max> fresh;
    const auto stage = [&](lx::size2i size, std::unique_ptr<output_pipeline>& out) {
        out = std::make_unique<output_pipeline>();
        out->mode = size;
        if (auto targets = create_targets(*out, true); !targets) {
            lx::trace::logger::global().log_error(targets.get_error(), "compositor.gfx");
            out.reset();
            return false;
        }
        return true;
    };
    for (unsigned i = 0; i < t.count; ++i) {
        const config_test_entry& c = t.entries[i];
        if (c.slot < 0)
            continue;
        output_pipeline* p = pipes_[c.slot].get();
        if (!p || !p->kms || p->retiring)
            continue;
        drm::modeset_entry& e = fixed[nf];
        e.pipe = p->kms.get();
        e.request_page_flip = false;
        if (!c.enable) {
            e.enable = false;
            e.cursor = p->cursor.valid() ? &p->cursor : nullptr;
            released_crtcs |= 1u << p->kms->desc().crtc_index;
            ++nf;
            continue;
        }
        if (c.mode_change) {
            const lx::size2i size{static_cast<int>(c.mode.width), static_cast<int>(c.mode.height)};
            if (!p->staged || p->staged->mode.width != size.width ||
                p->staged->mode.height != size.height) {
                if (!stage(size, p->staged)) {
                    answer(false, 0);
                    return;
                }
            }
            p->staged_since_ns = now_ns();
            e.mode = &c.mode;
            e.framebuffer_id = p->staged->framebuffer(0, cpu_present_);
        } else {
            e.framebuffer_id = p->framebuffer(p->front.load(std::memory_order_acquire), cpu_present_);
        }
        e.vrr = c.vrr;
        ++nf;
    }
    // Outputs being enabled: a CRTC each among those nothing else keeps, the whole state
    // checked by every candidate's test (KWin's checkCrtcAssignment).
    config_test_ctx ctx{};
    ctx.kms = &kms;
    ctx.fixed = fixed;
    ctx.fixed_count = nf;
    drm::crtc_alloc_input in{};
    unsigned used_crtcs = 0;
    std::inplace_vector<unsigned, 2 * output_manager::k_max> used_planes;
    for (const auto& up : pipes_) {
        if (!up || !up->kms || up->retiring)
            continue;
        if (released_crtcs & (1u << up->kms->desc().crtc_index))
            continue;
        used_crtcs |= 1u << up->kms->desc().crtc_index;
        used_planes.push_back(up->kms->desc().primary_plane_id);
        if (up->cursor.valid())
            used_planes.push_back(up->cursor.caps().plane_id);
    }
    // A released CRTC is not offered to another output in the same test: the disable and the
    // enable would state the same CRTC twice in one commit. Swapping a CRTC between outputs
    // takes two configurations (rare: it needs more enabled outputs than CRTCs).
    used_crtcs |= released_crtcs;
    for (unsigned i = 0; i < t.count; ++i) {
        const config_test_entry& c = t.entries[i];
        if (c.slot >= 0 || !c.enable || ctx.count >= output_manager::k_max)
            continue;
        const unsigned k = ctx.count++;
        in.connectors[k].possible_crtcs = c.possible_crtcs & ~used_crtcs;
        const int routed = c.routed_crtc_id ? kms.crtc_index_of(c.routed_crtc_id) : -1;
        in.connectors[k].routed_crtc = routed >= 0 && !(used_crtcs & (1u << routed)) ? routed : -1;
        const lx::size2i size{static_cast<int>(c.mode.width), static_cast<int>(c.mode.height)};
        if (!stage(size, fresh.emplace_back())) {
            answer(false, 0);
            return;
        }
        ctx.framebuffers[k] = fresh.back()->framebuffer(0, cpu_present_);
        ctx.modes[k] = &c.mode;
        ctx.connectors[k] = c.connector_id;
        ctx.vrr[k] = c.vrr;
    }
    bool ok = false;
    unsigned tests = 0;
    if (ctx.count == 0) {
        ok = nf == 0 || static_cast<bool>(drm::commit_modeset(kms, fixed, nf, true));
        tests = nf > 0 ? 1u : 0u;
    } else {
        in.connector_count = ctx.count;
        in.crtc_count = kms.crtc_count();
        in.plane_count = kms.plane_count() < drm::k_alloc_max_planes ? kms.plane_count()
                                                                      : drm::k_alloc_max_planes;
        for (unsigned k = 0; k < in.plane_count; ++k) {
            const drm::kms_plane_info pl = kms.plane(k);
            const bool taken = std::ranges::contains(used_planes, pl.id);
            in.planes[k].kind = pl.kind;
            in.planes[k].possible_crtcs = pl.possible_crtcs;
            in.planes[k].bound_crtc = pl.crtc_id ? kms.crtc_index_of(pl.crtc_id) : -1;
            in.planes[k].usable =
                !taken && (pl.kind == drm::plane_kind::cursor ? pl.argb8888 : pl.xrgb8888);
        }
        drm::crtc_alloc_result r{};
        ok = drm::assign_crtcs(in, r, &config_crtc_test, &ctx);
        tests = r.tests;
    }
    answer(ok, tests);
}

bool compositor_impl::apply_mode_switch(output_pipeline& p, unsigned frame) {
    if (!p.switch_pending || static_cast<int>(frame - p.switch_from_frame) < 0)
        return true;
    const lx::size2i size{static_cast<int>(p.switch_mode.width),
                          static_cast<int>(p.switch_mode.height)};
    const bool scanout = p.kms != nullptr;
    if (size.width != p.mode.width || size.height != p.mode.height) {
        // What is on screen: the current targets once a content commit of them went out,
        // else the previous switch's (the graveyard) until this pipeline's modeset flipped.
        const bool current_shown = !scanout || p.content_committed_once;
        if (p.graveyard && current_shown) {
            if (scanout && p.kms->flip_pending())
                return false; // the graveyard may still be scanned out: next tick
            p.graveyard.reset();
        }
        std::unique_ptr<output_pipeline> fresh;
        if (p.staged && p.staged->mode.width == size.width && p.staged->mode.height == size.height) {
            fresh = std::move(p.staged);
        } else {
            fresh = std::make_unique<output_pipeline>();
            fresh->mode = size;
            if (auto targets = create_targets(*fresh, scanout); !targets) {
                lx::trace::logger::global().log_error(targets.get_error(), "compositor.gfx");
                p.switch_pending = false;
                // Adaptive sync asked for with it still applies, by itself.
                if (p.switch_vrr >= 0 && p.kms)
                    p.kms->set_vrr(p.switch_vrr != 0);
                p.switch_vrr = -1;
                // An output configuration waiting for this switch has failed (and reverts).
                if (p.switch_ticket) {
                    compositor_impl* self = this;
                    const unsigned slot = p.slot;
                    const unsigned ticket = p.switch_ticket;
                    p.switch_ticket = 0;
                    (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, ticket] {
                        self->on_modeset_result(slot, ticket, false);
                    });
                }
                return true;
            }
        }
        for (unsigned i = 0; i < output_pipeline::k_slots; ++i) {
            std::swap(p.gl[i], fresh->gl[i]);
            std::swap(p.vk[i], fresh->vk[i]);
            std::swap(p.fb[i], fresh->fb[i]);
            std::swap(p.dumb[i], fresh->dumb[i]);
        }
        std::swap(p.slot_count, fresh->slot_count);
        fresh->mode = p.mode;
        // The old targets stay until the new mode's first flip when one of them may be on
        // screen (removing a framebuffer the CRTC scans out turns the CRTC off); else now.
        if (scanout && current_shown)
            p.graveyard = std::move(fresh);
        p.mode = size;
        p.scanout_rect = {0, 0, size.width, size.height};
        p.front.store(0, std::memory_order_release);
        p.pending_flip_slot.store(0, std::memory_order_release);
    }
    p.staged.reset();
    if (p.kms) {
        p.kms->set_mode(p.switch_mode);
        if (p.switch_vrr >= 0)
            p.kms->set_vrr(p.switch_vrr != 0);
        if (p.slot_count > 0)
            p.kms->set_framebuffer(p.framebuffer(0, cpu_present_));
    }
    p.switch_vrr = -1;
    // Nothing composited for the old mode is committed; every slot is repainted.
    p.discard_fence();
    p.composited_slot = -1;
    // Captures wait for the first frame of the new mode (their buffers no longer fit).
    p.capture_front = -1;
    capture_renderer_.output_reset(p.slot);
    p.frame_outstanding.store(false, std::memory_order_release);
    for (unsigned i = 0; i < output_pipeline::k_slots; ++i)
        p.slot_age[i].set_full();
    p.kms_pending.set_full();
    p.missed.clear();
    p.content_committed_once = false;
    p.modeset_wait_since_ns = 0;
    if (p.cursor.valid())
        p.cursor.invalidate();
    p.switch_pending = false;
    // The switch's modeset is the one an output configuration waits for.
    if (p.switch_ticket) {
        p.apply_ticket = p.switch_ticket;
        p.switch_ticket = 0;
    }
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.drm",
                                         "output {} switched to {}x{}@{} from frame {}", p.slot,
                                         p.switch_mode.width, p.switch_mode.height,
                                         p.switch_mode.refresh_mhz, frame);
    }
    return true;
}

void compositor_impl::finish_retire(unsigned slot) {
    output_pipeline* p = pipes_[slot].get();
    if (!p || !p->retiring)
        return;
    // A CRTC takes one commit at a time: its last flip drains first (the flip handler wakes
    // render when it lands).
    if (p->kms && p->kms->flip_pending())
        return;
    const bool vt_paused = (display_gate_.load(std::memory_order_acquire) & gate_vt) != 0;
    if (p->kms && drm_present_ && !vt_paused && !card_gone_.load(std::memory_order_acquire) &&
        p->ever_lit.load(std::memory_order_acquire)) {
        // CRTC off, connector unrouted, planes detached — otherwise the CRTC keeps scanning
        // out a frame of ours on a monitor the session no longer drives (wlroots MR 3967).
        drm::modeset_entry e{};
        e.pipe = p->kms.get();
        e.enable = false;
        e.request_page_flip = false;
        e.connector_gone = p->retire_connector_gone;
        e.cursor = p->cursor.valid() ? &p->cursor : nullptr;
        lx::trace::scoped_span span{"render", "kms.disable"};
        if (auto off = drm::commit_modeset(kms, &e, 1, false); !off)
            lx::trace::logger::global().log_error(off.get_error(), "compositor.drm");
    }
    if (p->kms) {
        // Nothing to give back to the console, and no event will be routed to it again.
        p->kms->forget_saved_state();
        p->kms->leave_registry();
    }
    p->discard_fence();
    p->staged.reset();
    p->graveyard.reset();
    release_targets(*p);
    p->cursor = drm::kms_cursor_plane{};
    const unsigned gen = p->retire_gen;
    // Destroyed on UI: the thread draining KMS events may hold it until its dispatch returns.
    output_pipeline* raw = pipes_[slot].release();
    delete in_transit_[slot].exchange(raw, std::memory_order_acq_rel);
    compositor_impl* self = this;
    (void)runtime::executor::global().post(runtime::affinity::ui,
                                           [self, slot, gen] { self->on_retired(slot, gen); });
}

void compositor_impl::on_retired(unsigned slot, unsigned gen) {
    if (slot >= output_manager::k_max)
        return;
    delete in_transit_[slot].exchange(nullptr, std::memory_order_acq_rel);
    if (slot_phase_[slot] == slot_phase::retiring && gen == slot_gen_[slot]) {
        slot_phase_[slot] = slot_phase::free;
        run_next_mgmt_job(); // a job may have waited for this slot
    }
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.hotplug",
                                         "output {} pipeline retired", slot);
    }
}

namespace {
[[nodiscard]] const drm::connector_probe* scan_entry(const drm::connector_scan& scan,
                                                     unsigned connector_id) {
    for (unsigned i = 0; i < scan.count; ++i)
        if (scan.entries[i].connector_id == connector_id)
            return &scan.entries[i];
    return nullptr;
}
} // namespace

void compositor_impl::apply_connector_scan(const drm::connector_scan& scan, bool forced) {
    if (card_gone_.load(std::memory_order_acquire))
        return;
    lx::trace::scoped_span span{"ui", "hotplug.apply"};
    if (!pending_)
        pending_ = std::make_unique<pending_output[]>(output_manager::k_max);
    // The heads of connectors that are not outputs, and every head's mode list.
    if (!last_scan_)
        last_scan_ = std::make_unique<drm::connector_scan>();
    // memcpy, not `=`: a POD, and Clang 23 loses the implicit copy assignment across the
    // module boundary (the copy constructor survives).
    std::memcpy(static_cast<void*>(last_scan_.get()), &scan, sizeof(scan));
    mark_heads_dirty();
    // Adds the scan no longer backs (unplugged again before their pipeline was up) are
    // cancelled: the pipeline, made or not, is retired.
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (slot_phase_[s] != slot_phase::adding || pending_[s].headless)
            continue;
        const drm::connector_probe* e = scan_entry(scan, pending_[s].probe.connector_id);
        if (e && desktop_connector(*e))
            continue;
        slot_phase_[s] = slot_phase::retiring;
        output_op op{};
        op.what = output_op::kind::retire;
        op.slot = s;
        op.gen = ++slot_gen_[s];
        op.connector_gone = e == nullptr;
        post_output_op(op);
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.hotplug",
                                         "output {} add cancelled ({} gone again)", s,
                                         pending_[s].probe.name);
    }
    // A connector that failed is tried again once it changed (or was unplugged), and at every
    // resume (a failure may have been the VT switch racing the test).
    if (forced)
        failed_.clear();
    for (std::size_t i = 0; i < failed_.size();) {
        const drm::connector_probe* e = scan_entry(scan, failed_[i].connector_id);
        if (e && desktop_connector(*e) && mode_list_hash(*e) == failed_[i].modes_hash) {
            ++i;
            continue;
        }
        failed_[i] = failed_.back();
        failed_.pop_back();
    }

    output_changes ch{};
    diff_scan(outputs, scan, ch);
    // A disabled connector (by a configuration or the setup) is not an output to add.
    {
        unsigned w = 0;
        for (unsigned i = 0; i < ch.count; ++i) {
            const output_change& c = ch.items[i];
            if (c.what == output_change::kind::added && c.entry >= 0 &&
                connector_disabled(scan.entries[c.entry].connector_id))
                continue;
            ch.items[w++] = c;
        }
        ch.count = w;
    }
    if (ch.empty()) {
        // An identical scan (our own probe's detect, `udevadm trigger`, a same-layout
        // update): nothing moves, nobody is told (§B.7).
        if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.hotplug",
                                            "hotplug: no change");
        // A disabled monitor unplugged or plugged in changes no output, but the set of
        // connected monitors, whose setup may say otherwise.
        apply_connected_setup(scan);
        return;
    }
    // Removals first (their slots and CRTCs are needed by what comes), then changes, then
    // the new outputs (wlroots creates outputs only after the old ones are destroyed).
    for (unsigned i = 0; i < ch.count; ++i) {
        const output_change& c = ch.items[i];
        if (c.what != output_change::kind::removed || c.slot < 0)
            continue;
        const unsigned slot = static_cast<unsigned>(c.slot);
        if (!outputs.slot_live(slot))
            continue;
        remove_output(slot, scan_entry(scan, outputs.slot_state(slot).connector_id) == nullptr);
    }
    for (unsigned i = 0; i < ch.count; ++i) {
        const output_change& c = ch.items[i];
        if (c.what != output_change::kind::changed || c.slot < 0 || c.entry < 0)
            continue;
        if (outputs.slot_live(static_cast<unsigned>(c.slot)))
            change_output(static_cast<unsigned>(c.slot), scan.entries[c.entry], forced);
    }
    // New connectors come up as the monitor's persisted state and the setup of what is
    // connected now say; a changed set of monitors applies its setup (ADR 008 §C.5).
    apply_connected_setup(scan);
    if (ch.dropped > 0)
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.hotplug",
                                        "more connected monitors than output slots — the rest "
                                        "stay off");
    outputs_changed_now();
}

int compositor_impl::add_output_from(const drm::connector_probe& e, const connect_choice* choice) {
    if (!pending_)
        pending_ = std::make_unique<pending_output[]>(output_manager::k_max);
    const int slot = outputs.free_slot(reserved_slots());
    if (slot < 0)
        return -1;
    const unsigned s = static_cast<unsigned>(slot);
    pending_output& pd = pending_[s];
    pd = pending_output{};
    pd.probe = e;
    pd.kms = drm_present_;
    pd.head_known = output_mgmt_heads(p0_).find(e.connector_id) != nullptr;
    if (choice) {
        for (unsigned i = 0; i < choice->mode_count && i < output_op::k_modes; ++i)
            pd.modes[i] = choice->modes[i];
        pd.mode_count = choice->mode_count;
        pd.scale = choice->scale;
        pd.transform = choice->transform;
        pd.adaptive_sync = choice->adaptive_sync && e.vrr_capable;
        pd.has_position = choice->has_position;
        pd.position = choice->position;
        pd.follow_preferred = choice->follow_preferred;
        pd.ticket = choice->ticket;
    } else {
        pd.mode_count = mode_candidates(e, choose_mode(e, nullptr), pd.modes, output_op::k_modes);
    }
    if (pd.mode_count == 0)
        return -1;
    output_op op{};
    op.what = output_op::kind::add;
    op.slot = s;
    op.gen = ++slot_gen_[s];
    op.kms = pd.kms;
    op.cursor_plane = pd.kms && cursor_.hardware_active();
    op.connector_id = e.connector_id;
    op.possible_crtcs = e.possible_crtcs;
    op.routed_crtc_id = e.current_crtc_id;
    for (unsigned i = 0; i < pd.mode_count; ++i)
        op.modes[i] = pd.modes[i];
    op.mode_count = pd.mode_count;
    op.size = {static_cast<int>(pd.modes[0].width), static_cast<int>(pd.modes[0].height)};
    op.from_frame = static_cast<unsigned>(last_published_any_ + 1);
    op.ticket = pd.ticket;
    op.vrr = pd.adaptive_sync;
    op.transform = static_cast<lx::buffer_transform>(pd.transform & 7u);
    slot_phase_[s] = slot_phase::adding;
    post_output_op(op);
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.hotplug",
                                     "output {} connecting name={} preferred={}x{}@{} modes={}", s,
                                     e.name, pd.modes[0].width, pd.modes[0].height,
                                     pd.modes[0].refresh_mhz, e.mode_count);
    mark_heads_dirty();
    return slot;
}

void compositor_impl::on_add_result(unsigned slot, unsigned gen, bool ok, unsigned mode_index,
                                    output_pipeline* pipe) {
    if (slot >= output_manager::k_max || gen != slot_gen_[slot] || !pending_)
        return; // superseded: a retire of this slot is queued behind it
    pending_output& pd = pending_[slot];
    if (pd.headless) {
        // HEADLESS-1 is live already; its (composite-only) pipeline is here now.
        if (ok && outputs.slot_live(slot))
            outs_[slot].pipe = pipe;
        return;
    }
    if (slot_phase_[slot] != slot_phase::adding)
        return;
    if (!ok) {
        slot_phase_[slot] = slot_phase::free;
        mark_heads_dirty();
        if (pd.ticket) {
            // An output configuration enabled it: its apply fails (and is reverted); the
            // connector is not marked failed — the client chose this mode, not the policy.
            on_modeset_result(slot, pd.ticket, false);
            if (outputs.count() == 0)
                add_fallback_headless();
            run_next_mgmt_job();
            return;
        }
        if (outputs.count() == 0)
            add_fallback_headless();
        if (failed_.size() < output_manager::k_max)
            failed_.push_back({pd.probe.connector_id, mode_list_hash(pd.probe)});
        lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.hotplug",
                                         "output {} not enabled: no CRTC, or none of its {} modes "
                                         "passed TEST_ONLY — it stays off until it changes",
                                         pd.probe.name, pd.mode_count);
        return;
    }
    const drm::kms_mode& m = pd.modes[mode_index < pd.mode_count ? mode_index : 0];
    output_desc d = desc_from_probe(pd.probe, m);
    d.virtual_output = !pd.kms;
    // What a configuration chose (a client's apply, the persisted monitor and setup).
    d.scale = pd.scale;
    d.transform = pd.transform;
    // As render could program it (a refused VRR falls back to off).
    d.adaptive_sync = pd.adaptive_sync && (!pipe || !pipe->kms || pipe->kms->vrr());
    d.follow_preferred = mode_index == 0 ? pd.follow_preferred : true;
    {
        bool listed = false;
        for (unsigned k = 0; k < pd.probe.mode_count && !listed; ++k)
            listed = pd.probe.modes[k].same_timings(m);
        d.custom_mode = !listed;
    }
    if (pd.has_position) {
        d.position = pd.position;
        d.auto_position = false;
    }
    if (outputs.add_output(d, static_cast<int>(slot)) < 0) {
        slot_phase_[slot] = slot_phase::retiring;
        output_op op{};
        op.what = output_op::kind::retire;
        op.slot = slot;
        op.gen = ++slot_gen_[slot];
        post_output_op(op);
        return;
    }
    slot_phase_[slot] = slot_phase::live;
    // UI takes the handle before the view and clock are made (`sync_output_views` keeps it).
    outs_[slot].pipe = pipe;
    // While locked a new output comes up locked (M14): the scene's layers are hidden, so its
    // first frame is the lock colour, and `locking` waits for this output's locked frame too.
    lock_shown_.forget(slot);
    // The stand-in goes now that a display is here (after the add: never zero outputs).
    if (fallback_headless_slot_ >= 0 && fallback_headless_slot_ != static_cast<int>(slot) &&
        outputs.slot_live(static_cast<unsigned>(fallback_headless_slot_))) {
        const unsigned fs = static_cast<unsigned>(fallback_headless_slot_);
        fallback_headless_slot_ = -1;
        remove_output(fs, true);
    }
    outputs_changed_now();
    // Every other output powered off by idle (or a client): the new one stays dark too until
    // the next input wakes them all (Mutter).
    {
        bool others = false;
        bool all_off = true;
        for (unsigned s = 0; s < output_manager::k_max; ++s) {
            if (s == slot || !outs_[s].live)
                continue;
            others = true;
            const power_phase ph = outs_[s].power.phase();
            all_off = all_off && (ph == power_phase::off || ph == power_phase::off_pending ||
                                  ph == power_phase::soft_blank);
        }
        if (others && all_off) {
            const bool vt_away = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
            apply_power_actions(slot, outs_[slot].power.request_off(vt_away));
        }
    }
    const unsigned back = wm_.on_output_added(slot);
    // Changed while it was being added (a resize right after the plug): what it is now.
    if (pd.stale && hotplug_ready_)
        hotplug_.request_full(false);
    if (pd.ticket) {
        // An output configuration waits for its first modeset — unless there is none to wait
        // for: no KMS (a virtual output), or it stays dark until the next wake.
        const std::uint32_t bit = 1u << slot;
        const bool vt_away = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
        if (!pd.kms || vt_away || (gate_power_mask_.load(std::memory_order_relaxed) & bit))
            on_modeset_result(slot, pd.ticket, true);
    }
    clocks_.set_vrr(slot, outputs.slot_state(slot).adaptive_sync);
    mark_heads_dirty();
    const output_state& o = outputs.slot_state(slot);
    const lx::rect2i l = o.logical_rect();
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.output",
                                     "output {} added name={} identifier=\"{}\" mode={}x{}@{} "
                                     "scale={:g} logical={}x{}+{}+{} windows_returned={}",
                                     slot, o.name, o.identifier, o.mode.width, o.mode.height,
                                     o.refresh_mhz, o.scale, l.width, l.height, l.x, l.y, back);
    request_frame(1u << slot);
    run_next_mgmt_job();
}

void compositor_impl::change_output(unsigned slot, const drm::connector_probe& e, bool forced) {
    const output_state& o = outputs.slot_state(slot);
    // A custom mode is not in any list: the output keeps it whatever the connector offers
    // (only a link retrain modesets it again).
    const drm::kms_mode* m = o.custom_mode ? &o.kms_mode : choose_mode(e, &o);
    const bool mode_change = m && !m->same_timings(o.kms_mode);
    const bool retrain = e.link_bad;
    const drm::kms_mode current = o.kms_mode;
    // Everything else about the head (preferred mode, the mode list, the host's suggested
    // position, physical size) is taken now; a moved suggestion re-runs the layout.
    outputs.update_head(slot, e);
    if (!mode_change && !retrain)
        return;
    const drm::kms_mode target = mode_change ? *m : current;
    output_pipeline* pipe = outs_[slot].pipe;
    // The resume switches at once (its modeset is the test, and it must not light the old
    // size); a composite-only output has nothing to test; a retrain keeps the mode.
    if (forced || !pipe || !pipe->kms || !mode_change) {
        switch_output_mode(slot, target, !mode_change ? "link-retrain" : forced ? "resume" : "hotplug");
        return;
    }
    pending_output& pd = pending_[slot];
    pd.probe = e;
    pd.mode_count = mode_candidates(e, &target, pd.modes, output_op::k_modes);
    output_op op{};
    op.what = output_op::kind::test_mode;
    op.slot = slot;
    op.gen = ++slot_gen_[slot];
    for (unsigned i = 0; i < pd.mode_count; ++i)
        op.modes[i] = pd.modes[i];
    op.mode_count = pd.mode_count;
    post_output_op(op);
}

void compositor_impl::on_mode_tested(unsigned slot, unsigned gen, bool ok, unsigned mode_index) {
    if (slot >= output_manager::k_max || gen != slot_gen_[slot] ||
        slot_phase_[slot] != slot_phase::live || !outputs.slot_live(slot) || !pending_)
        return;
    if (!ok) {
        const output_state& o = outputs.slot_state(slot);
        lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.hotplug",
                                         "output {} {}: no mode of its new list passed TEST_ONLY "
                                         "— staying at {}x{}",
                                         slot, o.name, o.mode.width, o.mode.height);
        return;
    }
    const pending_output& pd = pending_[slot];
    switch_output_mode(slot, pd.modes[mode_index < pd.mode_count ? mode_index : 0],
                       mode_index > 0 ? "hotplug (fallback mode)" : "hotplug");
}

void compositor_impl::switch_output_mode(unsigned slot, const drm::kms_mode& mode, const char* why) {
    const output_state& o = outputs.slot_state(slot);
    switch_output_mode(slot, mode, why, o.follow_preferred, o.custom_mode, 0);
}

void compositor_impl::switch_output_mode(unsigned slot, const drm::kms_mode& mode, const char* why,
                                         bool follow, bool custom, unsigned ticket) {
    const output_state& o = outputs.slot_state(slot);
    const lx::size2i was = o.mode;
    if (outs_[slot].pipe) {
        // Render takes the new targets right before compositing the first frame built at
        // the new size — every frame built from here on (§A.4).
        output_op op{};
        op.what = output_op::kind::switch_mode;
        op.slot = slot;
        op.gen = ++slot_gen_[slot];
        op.modes[0] = mode;
        op.mode_count = 1;
        op.from_frame = static_cast<unsigned>(last_published_any_ + 1);
        op.ticket = ticket;
        post_output_op(op);
    }
    outputs.set_mode(slot, mode, follow, custom);
    outputs_changed_now();
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.output",
                                     "output {} mode {}x{}@{} (was {}x{}) — {}", slot, mode.width,
                                     mode.height, mode.refresh_mhz, was.width, was.height, why);
    request_frame(1u << slot);
}

void compositor_impl::add_fallback_headless() {
    if (!pending_)
        pending_ = std::make_unique<pending_output[]>(output_manager::k_max);
    const int slot = outputs.free_slot(reserved_slots());
    if (slot < 0)
        return;
    const unsigned s = static_cast<unsigned>(slot);
    output_desc d{};
    d.name = "HEADLESS-1";
    d.mode = {1920, 1080};
    d.refresh_mhz = 60000;
    d.virtual_output = true;
    if (outputs.add_output(d, slot) < 0)
        return;
    fallback_headless_slot_ = slot;
    slot_phase_[s] = slot_phase::live;
    pending_[s] = pending_output{};
    pending_[s].headless = true;
    outs_[s].pipe = nullptr;
    output_op op{};
    op.what = output_op::kind::add;
    op.slot = s;
    op.gen = ++slot_gen_[s];
    op.kms = false;
    op.size = d.mode;
    op.from_frame = static_cast<unsigned>(last_published_any_ + 1);
    post_output_op(op);
    // Windows stranded by an output that went while another was on its way, which then
    // failed to come up: the stand-in takes them in.
    (void)wm_.on_output_added(s);
    lx::trace::logger::global().logf(lx::trace::level::info, "compositor.output",
                                     "output {} HEADLESS-1 stands in (no display connected)", s);
}

void compositor_impl::remove_output(unsigned slot, bool connector_gone, const char* why) {
    if (!outputs.slot_live(slot))
        return;
    char name[32];
    char identifier[208];
    std::snprintf(name, sizeof(name), "%s", outputs.slot_state(slot).name);
    std::snprintf(identifier, sizeof(identifier), "%s", outputs.slot_state(slot).identifier);
    const std::uint32_t bit = 1u << slot;
    // Clients always see at least one output: the stand-in comes before this one goes —
    // unless another is on its way (an output configuration enabling one).
    bool coming = false;
    for (unsigned s = 0; s < output_manager::k_max && !coming; ++s)
        coming = slot_phase_[s] == slot_phase::adding;
    if ((outputs.live_mask() & ~bit) == 0 && !coming)
        add_fallback_headless();
    auto& seat = input_devices_.default_seat();
    const int under = output_at(outputs, seat.pointer_x(), seat.pointer_y());
    const int fallback = fallback_output(outputs, slot, under);
    // §B.5: windows move (and remember this output), layer surfaces are closed (sway), lock
    // surfaces are dropped by the outputs-changed pass below.
    const unsigned moved = wm_.evacuate_output(slot, fallback, identifier);
    layers_.close_output(slot);
    // UI lets go of the pipeline before render takes it down.
    outs_[slot].pipe = nullptr;
    gate_power_mask_.fetch_and(~bit, std::memory_order_acq_rel);
    power_gen_[slot].fetch_add(1, std::memory_order_acq_rel); // a power-off task in flight is stale
    presentation_discard_output(p0_, slot);
    // Nothing is shown on it any more: a surface last shown only here is paced by the
    // fallback throttle (or the output it shows on next), never by this dead slot.
    p0_.published_mask &= ~bit;
    p0_.published_frame[slot] = 0;
    lock_shown_.forget(slot);
    if (fallback_headless_slot_ == static_cast<int>(slot))
        fallback_headless_slot_ = -1;
    outputs.remove_slot(slot);
    slot_phase_[slot] = slot_phase::retiring;
    {
        output_op op{};
        op.what = output_op::kind::retire;
        op.slot = slot;
        op.gen = ++slot_gen_[slot];
        op.connector_gone = connector_gone;
        post_output_op(op);
    }
    // An output configuration waiting for this output's modeset no longer does.
    if (mgmt_ && mgmt_->active && mgmt_->stage == mgmt_run::phase::committing &&
        (mgmt_->waiting & bit)) {
        mgmt_->waiting &= ~bit;
        // Finished from the timer, not from the middle of this removal.
        if (mgmt_->waiting == 0 && outcfg_timer_) {
            mgmt_->deadline_ns = now_ns();
            outcfg_timer_->schedule_at(mgmt_->deadline_ns + 1'000'000);
        }
    }
    // A VT resume waiting for this output's answer no longer does.
    if (vt_resume_pending_ & bit) {
        vt_resume_pending_ &= ~bit;
        if (vt_resume_pending_ == 0)
            apply_session_actions(session_.on_resume_result(vt_resume_session_gen_, vt_resume_outcome_));
    }
    // The pointer was on it: the middle of the fallback, where it is easy to find.
    const bool pointer_moved = under == static_cast<int>(slot) && fallback >= 0 &&
                               outputs.slot_live(static_cast<unsigned>(fallback));
    if (pointer_moved) {
        const lx::rect2i l = outputs.slot_state(static_cast<unsigned>(fallback)).logical_rect();
        seat.set_pointer_position(l.x + l.width / 2, l.y + l.height / 2);
    }
    // Membership first (leave on the objects that still stand for it), then the global goes,
    // its power objects fail, its lock surfaces drop (wire_output.cpp `outputs_changed`).
    outputs_changed_now();
    if (pointer_moved) {
        cursor_.on_motion(seat.pointer_x(), seat.pointer_y(), 0, wm_.grab_active() || wm_.drag_active());
        wm_.on_pointer_motion(static_cast<unsigned>(now_ns() / 1'000'000ull));
    }
    // Windows are suspended only while every output that is left is dark.
    {
        const std::uint32_t live = outputs.live_mask();
        const std::uint32_t dark = gate_power_mask_.load(std::memory_order_relaxed) & live;
        wm_.set_suspend_reason(window_manager::suspend_power, live != 0 && dark == live);
    }
    advance_lock();
    char line[320];
    if (why)
        std::snprintf(line, sizeof(line), "output %u disabled name=%s identifier=\"%s\" windows_moved=%u (%s)",
                      slot, name, identifier, moved, why);
    else
        std::snprintf(line, sizeof(line), "output %u removed name=%s identifier=\"%s\" windows_moved=%u%s",
                      slot, name, identifier, moved, connector_gone ? " (connector gone)" : "");
    lx::trace::logger::global().log(lx::trace::level::info, "compositor.output", line);
    mark_heads_dirty();
}

void compositor_impl::on_card_removed() {
    if (card_gone_.exchange(true, std::memory_order_acq_rel))
        return;
    lx::trace::logger::global().log(lx::trace::level::error, "compositor.hotplug",
                                    "DRM card removed — every display is gone; the session goes "
                                    "on without one");
    hotplug_.stop();
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (outputs.slot_live(s) && outputs.slot_state(s).connector_id != 0)
            remove_output(s, true);
}

void compositor_impl::reclamp_pointer() {
    if (outputs.count() == 0)
        return;
    auto& seat = input_devices_.default_seat();
    double x = seat.pointer_x();
    double y = seat.pointer_y();
    if (!closest_point(outputs, x, y) || (x == seat.pointer_x() && y == seat.pointer_y()))
        return;
    seat.set_pointer_position(x, y);
    // A synthesized motion: the cursor follows and pointer focus is re-evaluated (§A.8).
    cursor_.on_motion(x, y, 0, wm_.grab_active() || wm_.drag_active());
    wm_.on_pointer_motion(static_cast<unsigned>(now_ns() / 1'000'000ull));
}

void compositor_impl::sync_cursor_themes() {
    if (cursor_theme_count_ == 0)
        return; // before start's themes (or without a cursor at all)
    std::inplace_vector<unsigned, output_manager::k_max> used;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const unsigned s120 = static_cast<unsigned>(std::lround(outputs.slot_state(s).scale * 120.0));
        if (!std::ranges::contains(used, s120))
            used.push_back(s120);
    }
    bool changed = false;
    for (const unsigned s120 : used) {
        bool loaded = false;
        for (unsigned i = 0; i < cursor_theme_count_ && !loaded; ++i)
            loaded = cursor_theme_scale120_[i] == s120;
        if (loaded)
            continue;
        int slot = cursor_theme_count_ < k_cursor_scales ? static_cast<int>(cursor_theme_count_) : -1;
        for (unsigned i = 0; i < cursor_theme_count_ && slot < 0; ++i)
            if (!std::ranges::contains(used, cursor_theme_scale120_[i]))
                slot = static_cast<int>(i);
        if (slot < 0)
            break; // more distinct scales than themes: the nearest serves (§A.7)
        const unsigned i = static_cast<unsigned>(slot);
        const long px = std::lround(static_cast<double>(cursor_base_px_) * s120 / 120.0);
        cursor_themes_[i].load(cursor_theme_arg(),
                               px > 0 ? static_cast<unsigned>(px) : cursor_base_px_);
        cursor_theme_scale120_[i] = s120;
        if (i == cursor_theme_count_)
            ++cursor_theme_count_;
        changed = true;
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.cursor",
                                         "cursor theme={} size={} scale={}/120 (new output scale)",
                                         cursor_themes_[i].name(), cursor_themes_[i].size(), s120);
    }
    if (!changed)
        return;
    const xcursor_theme* themes[k_cursor_scales]{};
    for (unsigned i = 0; i < cursor_theme_count_; ++i)
        themes[i] = &cursor_themes_[i];
    cursor_.set_themes(themes, cursor_theme_scale120_, cursor_theme_count_);
}

void compositor_impl::outputs_changed_now() {
    mark_heads_dirty();
    // The layout moved (an output's mode, position or scale; an output added or gone): the
    // views, clocks and render projections follow, layer surfaces are re-arranged.
    if (outputs.layout_generation() != applied_layout_generation_) {
        sync_output_views();
        sync_cursor_themes();
    }
    // Anything a client sees of an output changed: clients are told (each wl_output and
    // xdg_output, then done) and every surface's membership is recomputed.
    if (outputs.generation() != output_generation_) {
        output_generation_ = outputs.generation();
        log_outputs();
        lx::trace::scoped_span outputs_span{"ui", "outputs_changed"};
        notify_outputs_changed(p0_);
    }
    // Captures follow at once (CC9): a gone output stops its sessions, a new mode sends new
    // constraints (window captures publish at the next tick).
    if (p0_.capture) {
        refresh_capture_host();
        capture_tick(p0_, p0_.building_frame, false);
    }
    reclamp_pointer();
}

// ── Output configuration: wlr-output-management and persistence (ADR 008 Part C) ──────

bool compositor_impl::connector_disabled(unsigned connector_id) const {
    return std::ranges::contains(disabled_connectors_, connector_id);
}

void compositor_impl::keep_position(unsigned connector_id, const output_state& o) {
    kept_position k{};
    k.connector_id = connector_id;
    k.has_position = !o.auto_position;
    k.position = o.position;
    k.has_mode = !o.follow_preferred && o.kms_mode.valid();
    k.mode = o.kms_mode;
    for (kept_position& kp : kept_positions_) {
        if (kp.connector_id != connector_id)
            continue;
        if (!k.has_position && !k.has_mode) {
            kp = kept_positions_.back();
            kept_positions_.pop_back();
        } else {
            kp = k;
        }
        return;
    }
    if ((k.has_position || k.has_mode) && kept_positions_.size() < output_manager::k_max)
        kept_positions_.push_back(k);
}

const compositor_impl::kept_position* compositor_impl::kept_position_of(unsigned connector_id) const {
    for (const kept_position& kp : kept_positions_)
        if (kp.connector_id == connector_id)
            return &kp;
    return nullptr;
}

void compositor_impl::set_connector_disabled(unsigned connector_id, bool disabled) {
    for (unsigned& c : disabled_connectors_) {
        if (c != connector_id)
            continue;
        if (!disabled) {
            c = disabled_connectors_.back();
            disabled_connectors_.pop_back();
            mark_heads_dirty();
        }
        return;
    }
    if (disabled && disabled_connectors_.size() < output_manager::k_max) {
        disabled_connectors_.push_back(connector_id);
        mark_heads_dirty();
    }
}

unsigned compositor_impl::virtual_key(const char* name) {
    for (const virtual_head& v : virtual_heads_)
        if (!std::strcmp(v.name, name))
            return v.key;
    if (virtual_heads_.size() >= output_manager::k_max)
        return mgmt::k_virtual_key; // never matched by anything
    virtual_head& v = virtual_heads_.emplace_back();
    v.key = mgmt::k_virtual_key | next_virtual_key_++;
    std::snprintf(v.name, sizeof(v.name), "%s", name);
    return v.key;
}

int compositor_impl::slot_of_head(unsigned key) const {
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const output_state& o = outputs.slot_state(s);
        if (o.connector_id != 0) {
            if (o.connector_id == key)
                return static_cast<int>(s);
            continue;
        }
        for (const virtual_head& v : virtual_heads_)
            if (v.key == key && !std::strcmp(v.name, o.name))
                return static_cast<int>(s);
    }
    return -1;
}

unsigned compositor_impl::max_enabled_heads() const {
    if (!drm_present_)
        return output_manager::k_max;
    const unsigned crtcs = kms.crtc_count();
    return crtcs < output_manager::k_max ? crtcs : output_manager::k_max;
}

unsigned compositor_impl::connected_identifiers(const drm::connector_scan& scan,
                                                char (*ids)[monitor_config::k_id],
                                                unsigned* entries) const {
    unsigned n = 0;
    for (unsigned i = 0; i < scan.count && n < output_manager::k_max; ++i) {
        const drm::connector_probe& e = scan.entries[i];
        if (!desktop_connector(e))
            continue;
        monitor_identity(e.name, e.make, e.model, e.serial, e.edid, ids[n], monitor_config::k_id,
                         nullptr, 0);
        // Two identical monitors: the connector tells them apart (as `output_manager` does).
        for (unsigned j = 0; j < n; ++j) {
            if (std::strcmp(ids[j], ids[n]) != 0)
                continue;
            const std::size_t len = std::strlen(ids[n]);
            std::snprintf(ids[n] + len, monitor_config::k_id - len, " connector:%s", e.name);
            break;
        }
        entries[n++] = i;
    }
    return n;
}

void compositor_impl::choose_for_connector(const drm::connector_probe& e, const char* id,
                                           const setup_config* setup, connect_choice& out) const {
    out = connect_choice{};
    const output_config& cfg = outcfg_.config();
    const monitor_config* mon = find_monitor(cfg, id);
    const setup_output_config* so = nullptr;
    for (unsigned i = 0; setup && i < setup->count && !so; ++i)
        if (!std::strcmp(setup->outputs[i].identifier, id))
            so = &setup->outputs[i];
    out.enabled = so ? so->enabled : true;
    // A VM's monitor (`hotplug_mode_update`): the host owns its size and place (Mutter).
    const bool vm = e.hotplug_mode_update;
    drm::kms_mode custom{};
    const drm::kms_mode* first = nullptr;
    if (mon && mon->has_mode && !mon->mode_preferred && !vm) {
        for (unsigned k = 0; k < e.mode_count; ++k) {
            const drm::kms_mode& m = e.modes[k];
            if (m.width != mon->width || m.height != mon->height)
                continue;
            if (mon->refresh_mhz != 0 && m.refresh_mhz != mon->refresh_mhz)
                continue;
            if (!first || m.refresh_mhz > first->refresh_mhz)
                first = &m;
        }
        if (!first) {
            // Not listed: it was a custom mode (the monitor is the same, so its list is).
            custom = drm::cvt_mode(mon->width, mon->height, mon->refresh_mhz,
                                   drm::cvt_blanking_for_connector(e.type));
            if (custom.valid())
                first = &custom;
        }
        if (first)
            out.follow_preferred = false;
    }
    out.mode_count = mode_candidates(e, first ? first : choose_mode(e, nullptr), out.modes,
                                     output_op::k_modes);
    if (mon && mon->has_scale)
        out.scale = mon->scale;
    if (mon && mon->has_transform)
        out.transform = mon->transform;
    out.adaptive_sync = mon && mon->has_adaptive_sync && mon->adaptive_sync && e.vrr_capable;
    if (so && so->has_position && !vm) {
        out.has_position = true;
        out.position = {so->x, so->y};
    }
}

void compositor_impl::configure_outputs_at_start(const drm::connector_scan& scan) {
    last_scan_ = std::make_unique<drm::connector_scan>(scan);
    char ids[output_manager::k_max][monitor_config::k_id];
    unsigned entries[output_manager::k_max];
    const unsigned n = connected_identifiers(scan, ids, entries);
    const char* ptrs[output_manager::k_max];
    connected_set_.clear();
    for (unsigned k = 0; k < n; ++k) {
        ptrs[k] = ids[k];
        connected_set_.push_back(scan.entries[entries[k]].connector_id);
    }
    std::ranges::sort(connected_set_);
    const setup_config* setup = find_setup(outcfg_.config(), ptrs, n);
    unsigned added = 0;
    for (unsigned k = 0; k < n; ++k) {
        const drm::connector_probe& e = scan.entries[entries[k]];
        connect_choice c{};
        choose_for_connector(e, ids[k], setup, c);
        if (!c.enabled || c.mode_count == 0) {
            if (c.has_position) {
                output_state at{};
                at.position = c.position;
                at.auto_position = false;
                keep_position(e.connector_id, at);
            }
            set_connector_disabled(e.connector_id, true);
            continue;
        }
        output_desc d = desc_from_probe(e, c.modes[0]);
        d.scale = c.scale;
        d.transform = c.transform;
        d.adaptive_sync = c.adaptive_sync;
        d.follow_preferred = c.follow_preferred;
        bool listed = false;
        for (unsigned m = 0; m < e.mode_count && !listed; ++m)
            listed = e.modes[m].same_timings(c.modes[0]);
        d.custom_mode = !listed;
        if (c.has_position) {
            d.position = c.position;
            d.auto_position = false;
        }
        if (outputs.add_output(d) >= 0)
            ++added;
    }
    // A file that disables every connected monitor (only a hand edit can): the first comes up.
    if (added == 0 && n > 0) {
        const drm::connector_probe& e = scan.entries[entries[0]];
        set_connector_disabled(e.connector_id, false);
        (void)outputs.add_output(desc_from_probe(e, *e.preferred_mode()));
        lx::trace::logger::global().log(lx::trace::level::warn, "compositor.outputcfg",
                                        "the persisted setup disables every monitor — enabling "
                                        "the first");
    }
    if (setup || outcfg_.config().monitor_count > 0) {
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.outputcfg",
                                         "output configuration at start: {} monitor{}, setup {}, "
                                         "{} disabled",
                                         n, n == 1 ? "" : "s", setup ? "restored" : "new",
                                         static_cast<unsigned>(disabled_connectors_.size()));
    }
    if (outputs.count() == 0)
        (void)outputs.add_virtual("HEADLESS-1", {0, 0, 1920, 1080}, 60000);
    mark_heads_dirty();
}

void compositor_impl::apply_connected_setup(const drm::connector_scan& scan) {
    char ids[output_manager::k_max][monitor_config::k_id];
    unsigned entries[output_manager::k_max];
    const unsigned n = connected_identifiers(scan, ids, entries);
    const char* ptrs[output_manager::k_max];
    unsigned now_set[output_manager::k_max];
    for (unsigned k = 0; k < n; ++k) {
        ptrs[k] = ids[k];
        now_set[k] = scan.entries[entries[k]].connector_id;
    }
    std::sort(now_set, now_set + n);
    const std::span<const unsigned> now{now_set, n};
    const bool set_changed = !std::ranges::equal(now, connected_set_);
    connected_set_.assign(now.begin(), now.end());
    // Disabled marks of connectors no longer connected go with them.
    for (std::size_t i = 0; i < disabled_connectors_.size();) {
        if (std::ranges::contains(now, disabled_connectors_[i])) {
            ++i;
            continue;
        }
        disabled_connectors_[i] = disabled_connectors_.back();
        disabled_connectors_.pop_back();
        mark_heads_dirty();
    }
    for (std::size_t i = 0; i < kept_positions_.size();) {
        if (std::ranges::contains(now, kept_positions_[i].connector_id)) {
            ++i;
        } else {
            kept_positions_[i] = kept_positions_.back();
            kept_positions_.pop_back();
        }
    }
    // When the set of connected monitors changed, its setup (if one was saved) decides which
    // are enabled and where; an unknown set enables everything and keeps what is placed
    // where it is (KWin 6). Otherwise only new connectors are brought up.
    const setup_config* setup = set_changed ? find_setup(outcfg_.config(), ptrs, n) : nullptr;
    std::inplace_vector<unsigned, output_manager::k_max> to_disable;
    for (unsigned k = 0; k < n; ++k) {
        const drm::connector_probe& e = scan.entries[entries[k]];
        const setup_output_config* so = nullptr;
        for (unsigned i = 0; setup && i < setup->count && !so; ++i)
            if (!std::strcmp(setup->outputs[i].identifier, ids[k]))
                so = &setup->outputs[i];
        const bool want = set_changed ? (so ? so->enabled : true) : !connector_disabled(e.connector_id);
        int slot = -1;
        for (unsigned s = 0; s < output_manager::k_max && slot < 0; ++s)
            if (outputs.slot_live(s) && outputs.slot_state(s).connector_id == e.connector_id)
                slot = static_cast<int>(s);
        if (slot >= 0) {
            if (!want) {
                to_disable.push_back(static_cast<unsigned>(slot));
                keep_position(e.connector_id, outputs.slot_state(static_cast<unsigned>(slot)));
                set_connector_disabled(e.connector_id, true);
            } else if (so && !e.hotplug_mode_update) {
                const lx::output_id id = outputs.slot_state(static_cast<unsigned>(slot)).id;
                if (so->has_position)
                    outputs.set_position(id, {so->x, so->y});
                else
                    outputs.set_auto_position(id);
            }
            continue;
        }
        bool busy = false;
        for (unsigned s = 0; s < output_manager::k_max && !busy; ++s) {
            busy = slot_phase_[s] == slot_phase::adding && !pending_[s].headless &&
                   pending_[s].probe.connector_id == e.connector_id;
            if (busy && mode_list_hash(e) != mode_list_hash(pending_[s].probe))
                pending_[s].stale = true;
        }
        for (const failed_connector& f : failed_)
            busy = busy || f.connector_id == e.connector_id;
        if (busy)
            continue;
        if (!want) {
            if (so && so->has_position && !e.hotplug_mode_update) {
                output_state at{};
                at.position = {so->x, so->y};
                at.auto_position = false;
                keep_position(e.connector_id, at);
            }
            set_connector_disabled(e.connector_id, true);
            continue;
        }
        set_connector_disabled(e.connector_id, false);
        connect_choice c{};
        choose_for_connector(e, ids[k], setup ? setup : find_setup(outcfg_.config(), ptrs, n), c);
        (void)add_output_from(e, &c);
    }
    // Disables last: whatever this setup enables is on its way first, so clients never see
    // the stand-in output in between.
    for (const unsigned slot : to_disable)
        if (outputs.slot_live(slot))
            remove_output(slot, false, "disabled by the setup");
    if (setup)
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.outputcfg",
                                        "connected monitors changed: their saved setup applied");
    mark_heads_dirty();
}

void compositor_impl::build_heads(mgmt::head_set& hs) {
    hs.count = 0;
    const auto fill_modes = [](mgmt::head& h, const drm::connector_probe& e) {
        h.mode_count = 0;
        for (unsigned k = 0; k < e.mode_count && h.mode_count < mgmt::k_max_modes; ++k) {
            const drm::kms_mode& m = e.modes[k];
            const mgmt::mode mm{static_cast<int>(m.width), static_cast<int>(m.height),
                                static_cast<int>(m.refresh_mhz), m.preferred};
            // Distinct timings with one tuple are one mode to a client (the first is used).
            bool dup = false;
            for (unsigned j = 0; j < h.mode_count && !dup; ++j)
                dup = h.modes[j].same(mm);
            if (!dup)
                h.modes[h.mode_count++] = mm;
        }
    };
    const auto set_current = [](mgmt::head& h, int w, int ht, int mhz, bool custom) {
        h.current = -1;
        for (unsigned k = 0; k < h.mode_count && !custom; ++k)
            if (h.modes[k].width == w && h.modes[k].height == ht && h.modes[k].refresh_mhz == mhz &&
                (h.current < 0 || h.modes[k].preferred))
                h.current = static_cast<int>(k);
        if (h.current < 0)
            h.custom = {w, ht, mhz, false};
    };
    // Outputs, in slot order.
    for (unsigned s = 0; s < output_manager::k_max && hs.count < mgmt::k_max_heads; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const output_state& o = outputs.slot_state(s);
        mgmt::head& h = hs.heads[hs.count++];
        h = mgmt::head{};
        h.key = o.connector_id ? o.connector_id : virtual_key(o.name);
        std::snprintf(h.name, sizeof(h.name), "%s", o.name);
        std::snprintf(h.description, sizeof(h.description), "%s", o.description);
        std::snprintf(h.make, sizeof(h.make), "%s", o.make);
        std::snprintf(h.model, sizeof(h.model), "%s", o.model);
        std::snprintf(h.serial, sizeof(h.serial), "%s", o.serial);
        h.phys_w_mm = o.phys_w_mm;
        h.phys_h_mm = o.phys_h_mm;
        const drm::connector_probe* e =
            o.connector_id && last_scan_ ? scan_entry(*last_scan_, o.connector_id) : nullptr;
        if (e)
            fill_modes(h, *e);
        if (h.mode_count == 0) {
            // A virtual output's one mode is the one it has.
            h.modes[0] = {o.mode.width, o.mode.height, static_cast<int>(o.refresh_mhz), true};
            h.mode_count = 1;
        }
        set_current(h, o.mode.width, o.mode.height, static_cast<int>(o.refresh_mhz),
                    o.custom_mode && e != nullptr);
        h.enabled = true;
        h.x = o.position.x;
        h.y = o.position.y;
        h.transform = o.transform;
        h.scale = o.scale;
        h.adaptive_sync = o.adaptive_sync;
        h.vrr_capable = o.vrr_capable;
        h.auto_position = o.auto_position;
    }
    // Connected monitors that are not outputs (disabled, refused, being brought up).
    if (last_scan_) {
        for (unsigned i = 0; i < last_scan_->count && hs.count < mgmt::k_max_heads; ++i) {
            const drm::connector_probe& e = last_scan_->entries[i];
            if (!desktop_connector(e))
                continue;
            bool live = false;
            for (unsigned s = 0; s < output_manager::k_max && !live; ++s)
                live = outputs.slot_live(s) && outputs.slot_state(s).connector_id == e.connector_id;
            // A monitor being brought up is not a head yet: it appears once, enabled, when
            // its output is live (a client configuring it meanwhile would only be cancelled).
            for (unsigned s = 0; s < output_manager::k_max && !live && pending_; ++s)
                live = slot_phase_[s] == slot_phase::adding && !pending_[s].headless &&
                       !pending_[s].head_known && pending_[s].probe.connector_id == e.connector_id;
            if (live)
                continue;
            mgmt::head& h = hs.heads[hs.count++];
            h = mgmt::head{};
            h.key = e.connector_id;
            std::snprintf(h.name, sizeof(h.name), "%s", e.name);
            char id[monitor_config::k_id];
            monitor_identity(e.name, e.make, e.model, e.serial, e.edid, id, sizeof(id),
                             h.description, sizeof(h.description));
            std::snprintf(h.make, sizeof(h.make), "%s", e.make);
            std::snprintf(h.model, sizeof(h.model), "%s", e.model);
            std::snprintf(h.serial, sizeof(h.serial), "%s", e.serial);
            h.phys_w_mm = static_cast<int>(e.phys_w_mm);
            h.phys_h_mm = static_cast<int>(e.phys_h_mm);
            fill_modes(h, e);
            // What enabling it without setting anything gives: its persisted monitor state,
            // else the defaults.
            connect_choice c{};
            choose_for_connector(e, id, nullptr, c);
            const drm::kms_mode& first = c.mode_count > 0 ? c.modes[0] : *e.preferred_mode();
            bool listed = false;
            for (unsigned k = 0; k < e.mode_count && !listed; ++k)
                listed = e.modes[k].same_timings(first);
            set_current(h, static_cast<int>(first.width), static_cast<int>(first.height),
                        static_cast<int>(first.refresh_mhz), !listed);
            h.enabled = false;
            h.scale = c.scale > 0.0 ? representable_scale(c.scale)
                                    : outputs.default_scale_for(e.name, static_cast<int>(first.width),
                                                                static_cast<int>(first.height),
                                                                h.phys_w_mm, h.phys_h_mm);
            h.transform = c.transform;
            h.adaptive_sync = c.adaptive_sync;
            h.vrr_capable = e.vrr_capable;
            h.auto_position = true;
            if (const kept_position* kp = kept_position_of(e.connector_id)) {
                if (kp->has_position) {
                    h.x = kp->position.x;
                    h.y = kp->position.y;
                    h.auto_position = false;
                }
                if (kp->has_mode) {
                    bool in_list = false;
                    for (unsigned k = 0; k < e.mode_count && !in_list; ++k)
                        in_list = e.modes[k].same_timings(kp->mode);
                    set_current(h, static_cast<int>(kp->mode.width), static_cast<int>(kp->mode.height),
                                static_cast<int>(kp->mode.refresh_mhz), !in_list);
                }
            }
        }
    }
    // Virtual outputs a configuration disabled.
    for (const virtual_head& v : virtual_heads_) {
        if (hs.count >= mgmt::k_max_heads)
            break;
        if (!v.disabled)
            continue;
        mgmt::head& h = hs.heads[hs.count++];
        h = mgmt::head{};
        h.key = v.key;
        std::snprintf(h.name, sizeof(h.name), "%s", v.name);
        std::snprintf(h.description, sizeof(h.description), "%s", v.name);
        h.modes[0] = {v.mode.width, v.mode.height, static_cast<int>(v.refresh_mhz), true};
        h.mode_count = 1;
        h.current = 0;
        h.scale = v.scale;
        h.transform = v.transform;
        h.x = v.position.x;
        h.y = v.position.y;
        h.auto_position = v.auto_position;
    }
}

void compositor_impl::flush_output_heads() {
    if (!heads_dirty_)
        return;
    heads_dirty_ = false;
    if (!p0_.output_mgmt_kick)
        return; // no zwlr_output_manager_v1 global
    if (!heads_scratch_)
        heads_scratch_ = std::make_unique<mgmt::head_set>();
    build_heads(*heads_scratch_);
    output_heads_changed(p0_, *heads_scratch_);
}

drm::kms_mode compositor_impl::target_kms_mode(const mgmt::head_target& t,
                                               const drm::connector_probe* e) const {
    if (!e) {
        drm::kms_mode m{};
        m.width = static_cast<unsigned>(t.mode.width);
        m.height = static_cast<unsigned>(t.mode.height);
        m.refresh_mhz = t.mode.refresh_mhz > 0 ? static_cast<unsigned>(t.mode.refresh_mhz) : 60000u;
        m.preferred = true;
        return m;
    }
    if (!t.custom) {
        for (unsigned k = 0; k < e->mode_count; ++k) {
            const drm::kms_mode& m = e->modes[k];
            if (static_cast<int>(m.width) == t.mode.width &&
                static_cast<int>(m.height) == t.mode.height &&
                static_cast<int>(m.refresh_mhz) == t.mode.refresh_mhz && m.preferred == t.mode.preferred)
                return m;
        }
    }
    return drm::cvt_mode(static_cast<unsigned>(t.mode.width), static_cast<unsigned>(t.mode.height),
                         static_cast<unsigned>(t.mode.refresh_mhz > 0 ? t.mode.refresh_mhz : 0),
                         drm::cvt_blanking_for_connector(e->type));
}

namespace {

/// "Virtual-1 1600x900@60000+0+0 x1.5 90, Virtual-2 off" — for the log.
void describe_target(const mgmt::target& t, const mgmt::head_set& heads, char* out, unsigned size) {
    unsigned w = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < t.count && w + 1 < size; ++i) {
        const mgmt::head_target& h = t.heads[i];
        const mgmt::head* hd = heads.find(h.key);
        const char* name = hd ? hd->name : "?";
        int n = 0;
        if (!h.enabled)
            n = std::snprintf(out + w, size - w, "%s%s off", i ? ", " : "", name);
        else
            n = std::snprintf(out + w, size - w, "%s%s %dx%d@%d%s %s%d,%d x%g t%u%s", i ? ", " : "",
                              name, h.mode.width, h.mode.height, h.mode.refresh_mhz,
                              h.custom ? " (custom)" : "", h.position_explicit ? "at " : "auto ",
                              h.x, h.y, h.scale, h.transform, h.adaptive_sync ? " vrr" : "");
        if (n < 0)
            break;
        w += static_cast<unsigned>(n);
        if (w >= size)
            w = size - 1;
    }
}

[[nodiscard]] bool same_targets(const mgmt::target& a, const mgmt::target& b) {
    if (a.count != b.count)
        return false;
    for (unsigned i = 0; i < a.count; ++i) {
        const mgmt::head_target& x = a.heads[i];
        const mgmt::head_target* y = b.find(x.key);
        if (!y || x.enabled != y->enabled)
            return false;
        if (!x.enabled)
            continue;
        if (!x.mode.same_size_refresh(y->mode) || x.custom != y->custom || x.x != y->x ||
            x.y != y->y || x.position_explicit != y->position_explicit ||
            x.transform != y->transform || x.scale != y->scale ||
            x.adaptive_sync != y->adaptive_sync)
            return false;
    }
    return true;
}

bool post_to_worker(void (*fn)(void*), void* arg);

} // namespace

bool compositor_impl::plan_config_test(const mgmt::target& t, config_test& out) const {
    out.count = 0;
    if (!drm_present_ || !last_scan_)
        return false;
    for (unsigned i = 0; i < t.count && out.count < output_manager::k_max; ++i) {
        const mgmt::head_target& ht = t.heads[i];
        const int slot = slot_of_head(ht.key);
        config_test_entry c{};
        if (slot >= 0) {
            const output_state& o = outputs.slot_state(static_cast<unsigned>(slot));
            const output_pipeline* p = outs_[slot].pipe;
            if (o.connector_id == 0 || !p || !p->kms)
                continue; // composite-only: nothing for the kernel
            c.slot = slot;
            if (!ht.enabled) {
                c.enable = false;
                out.entries[out.count++] = c;
                continue;
            }
            const drm::kms_mode m = target_kms_mode(ht, scan_entry(*last_scan_, o.connector_id));
            c.mode = m;
            c.mode_change = !m.same_timings(o.kms_mode) || static_cast<int>(m.width) != o.mode.width ||
                            static_cast<int>(m.height) != o.mode.height;
            c.vrr = ht.adaptive_sync != o.adaptive_sync ? (ht.adaptive_sync ? 1 : 0) : -1;
            if (c.mode_change || c.vrr >= 0)
                out.entries[out.count++] = c;
            continue;
        }
        if (!ht.enabled || (ht.key & mgmt::k_virtual_key))
            continue;
        const drm::connector_probe* e = scan_entry(*last_scan_, ht.key);
        if (!e)
            continue;
        c.slot = -1;
        c.connector_id = e->connector_id;
        c.possible_crtcs = e->possible_crtcs;
        c.routed_crtc_id = e->current_crtc_id;
        c.mode = target_kms_mode(ht, e);
        c.vrr = ht.adaptive_sync ? 1 : 0;
        out.entries[out.count++] = c;
    }
    return out.count > 0;
}

void compositor_impl::run_next_mgmt_job() {
    if (mgmt_ && mgmt_->active)
        return;
    mgmt::job_queue& q = output_mgmt_jobs(p0_);
    while (!q.empty()) {
        // An output mid-way (being added or retired): its answer kicks this again.
        if (reserved_slots() != 0)
            return;
        // The serial must reflect every change made so far.
        flush_output_heads();
        const mgmt::job j = q.front();
        q.pop();
        if (!mgmt_)
            mgmt_ = std::make_unique<mgmt_run>();
        mgmt_run& r = *mgmt_;
        r = mgmt_run{};
        r.job = j;
        if (j.config.serial != output_mgmt_serial(p0_)) {
            r.active = true;
            finish_mgmt_job(mgmt_result::cancelled, "the outputs changed since its serial");
            return;
        }
        const mgmt::head_set& heads = output_mgmt_heads(p0_);
        mgmt::current_target(heads, r.previous);
        if (const auto f = mgmt::resolve(j.config, heads, max_enabled_heads(), r.target);
            f != mgmt::resolve_failure::none) {
            r.active = true;
            finish_mgmt_job(mgmt_result::failed, mgmt::failure_text(f));
            return;
        }
        r.active = true;
        if (same_targets(r.target, r.previous)) {
            // What is already there (kanshi re-applying the restored state at login, C4):
            // no modeset, no save.
            finish_mgmt_job(mgmt_result::succeeded, "nothing to change");
            return;
        }
        r.test_gen = ++config_test_gen_;
        config_test test{};
        if (plan_config_test(r.target, test)) {
            test.gen = r.test_gen;
            r.stage = mgmt_run::phase::testing;
            r.deadline_ns = now_ns() + k_mgmt_deadline_ns;
            if (outcfg_timer_)
                outcfg_timer_->schedule_at(r.deadline_ns);
            {
                std::lock_guard lock{ops_mutex_};
                config_test_ = test;
                config_test_pending_ = true;
            }
            ops_pending_.store(true, std::memory_order_release);
            if (threads_started.load(std::memory_order_acquire))
                wake_render();
            else
                run_output_ops();
            return;
        }
        on_config_tested(r.test_gen, true, 0);
        return;
    }
}

void compositor_impl::on_config_tested(unsigned gen, bool ok, unsigned tests) {
    if (!mgmt_ || !mgmt_->active || mgmt_->stage != mgmt_run::phase::testing ||
        gen != mgmt_->test_gen)
        return;
    mgmt_run& r = *mgmt_;
    flush_output_heads();
    if (r.job.config.serial != output_mgmt_serial(p0_)) {
        finish_mgmt_job(mgmt_result::cancelled, "the outputs changed during its test");
        return;
    }
    if (!ok) {
        char why[64];
        std::snprintf(why, sizeof(why), "TEST_ONLY refused (%u tested)", tests);
        finish_mgmt_job(mgmt_result::failed, tests ? why : "TEST_ONLY refused");
        return;
    }
    if (r.job.test) {
        finish_mgmt_job(mgmt_result::succeeded, tests ? "TEST_ONLY passed" : "valid");
        return;
    }
    r.ticket = ++apply_ticket_counter_;
    if (r.ticket == 0)
        r.ticket = ++apply_ticket_counter_;
    std::uint32_t waiting = 0;
    r.stage = mgmt_run::phase::committing;
    switch_to_target(r.target, r.ticket, waiting);
    r.waiting |= waiting;
    r.deadline_ns = now_ns() + k_mgmt_deadline_ns;
    if (outcfg_timer_)
        outcfg_timer_->schedule_at(r.deadline_ns);
    if (r.waiting == 0)
        complete_apply(r.failed ? "an output could not be enabled" : "no modeset needed");
}

void compositor_impl::switch_to_target(const mgmt::target& t, unsigned ticket,
                                       std::uint32_t& waiting) {
    waiting = 0;
    const bool vt_paused = (display_gate_.load(std::memory_order_relaxed) & gate_vt) != 0;
    const std::uint32_t gated =
        vt_paused ? ~0u : gate_power_mask_.load(std::memory_order_relaxed);
    // 1. Enables first, so the outputs being disabled are never the last ones left (no
    //    stand-in output flashes up in between); they come up asynchronously.
    for (unsigned i = 0; i < t.count; ++i) {
        const mgmt::head_target& ht = t.heads[i];
        if (!ht.enabled || slot_of_head(ht.key) >= 0)
            continue;
        if (ht.key & mgmt::k_virtual_key) {
            for (virtual_head& vh : virtual_heads_) {
                if (vh.key != ht.key)
                    continue;
                vh.disabled = false;
                output_desc d{};
                d.name = vh.name;
                d.mode = {ht.mode.width, ht.mode.height};
                d.refresh_mhz = ht.mode.refresh_mhz > 0 ? static_cast<unsigned>(ht.mode.refresh_mhz) : 60000u;
                d.scale = ht.scale;
                d.transform = ht.transform;
                d.virtual_output = true;
                d.position = {ht.x, ht.y};
                d.auto_position = !ht.position_explicit;
                const int s = outputs.add_output(d);
                if (s < 0)
                    break;
                const unsigned us = static_cast<unsigned>(s);
                slot_phase_[us] = slot_phase::live;
                if (!pending_)
                    pending_ = std::make_unique<pending_output[]>(output_manager::k_max);
                pending_[us] = pending_output{};
                pending_[us].headless = true;
                outs_[us].pipe = nullptr;
                output_op op{};
                op.what = output_op::kind::add;
                op.slot = us;
                op.gen = ++slot_gen_[us];
                op.kms = false;
                op.size = d.mode;
                op.transform = static_cast<lx::buffer_transform>(ht.transform & 7u);
                op.from_frame = static_cast<unsigned>(last_published_any_ + 1);
                post_output_op(op);
                (void)wm_.on_output_added(us);
                break;
            }
            continue;
        }
        const drm::connector_probe* e = last_scan_ ? scan_entry(*last_scan_, ht.key) : nullptr;
        if (!e) {
            if (ticket && mgmt_)
                mgmt_->failed = true;
            continue;
        }
        connect_choice c{};
        c.modes[0] = target_kms_mode(ht, e);
        c.mode_count = c.modes[0].valid() ? 1u : 0u;
        c.scale = ht.scale;
        c.transform = ht.transform;
        c.adaptive_sync = ht.adaptive_sync;
        c.has_position = ht.position_explicit;
        c.position = {ht.x, ht.y};
        // Following the preferred mode exactly when that is the mode it gets.
        c.follow_preferred = ht.mode.preferred && !ht.custom;
        c.ticket = ticket;
        set_connector_disabled(ht.key, false);
        const int s = add_output_from(*e, &c);
        if (s < 0) {
            if (ticket && mgmt_)
                mgmt_->failed = true;
            continue;
        }
        if (ticket)
            waiting |= 1u << s; // answered by the add (refused, dark, or its first modeset)
    }
    // 2. Outputs that stay: mode, adaptive sync, transform, scale, position.
    for (unsigned i = 0; i < t.count; ++i) {
        const mgmt::head_target& ht = t.heads[i];
        if (!ht.enabled)
            continue;
        const int si = slot_of_head(ht.key);
        if (si < 0 || slot_phase_[si] != slot_phase::live)
            continue;
        const unsigned slot = static_cast<unsigned>(si);
        const std::uint32_t bit = 1u << slot;
        const output_state o = outputs.slot_state(slot); // a copy: the setters change it
        const drm::connector_probe* e =
            o.connector_id && last_scan_ ? scan_entry(*last_scan_, o.connector_id) : nullptr;
        const drm::kms_mode m = target_kms_mode(ht, e);
        const bool follow = ht.mode_explicit ? (ht.mode.preferred && !ht.custom) : o.follow_preferred;
        const output_pipeline* pipe = outs_[slot].pipe;
        const bool kms_pipe = pipe && pipe->kms;
        const bool wait = ticket && kms_pipe && !(gated & bit);
        if (static_cast<int>(m.width) != o.mode.width || static_cast<int>(m.height) != o.mode.height ||
            m.refresh_mhz != o.refresh_mhz || (e && !m.same_timings(o.kms_mode))) {
            switch_output_mode(slot, m, "output configuration", follow, ht.custom && e != nullptr,
                               wait ? ticket : 0);
            if (wait)
                waiting |= bit;
        } else {
            outputs.set_follow_preferred(slot, follow);
        }
        if (ht.adaptive_sync != o.adaptive_sync) {
            outputs.set_adaptive_sync(slot, ht.adaptive_sync);
            clocks_.set_vrr(slot, ht.adaptive_sync);
            if (pipe) {
                output_op op{};
                op.what = output_op::kind::set_vrr;
                op.slot = slot;
                op.gen = slot_gen_[slot];
                op.vrr = ht.adaptive_sync;
                op.ticket = wait ? ticket : 0;
                post_output_op(op);
                if (wait)
                    waiting |= bit;
                scene.note_full_damage_output(slot);
            }
        }
        if (ht.transform != o.transform)
            outputs.set_transform(o.id, ht.transform);
        if (ht.scale != o.scale)
            outputs.set_scale(o.id, ht.scale);
        if (ht.position_explicit) {
            if (o.auto_position || o.position.x != ht.x || o.position.y != ht.y)
                outputs.set_position(o.id, {ht.x, ht.y});
        } else if (!o.auto_position) {
            outputs.set_auto_position(o.id);
        }
    }
    // 3. Disables last.
    for (unsigned i = 0; i < t.count; ++i) {
        const mgmt::head_target& ht = t.heads[i];
        if (ht.enabled)
            continue;
        const int si = slot_of_head(ht.key);
        if (si < 0)
            continue;
        const unsigned slot = static_cast<unsigned>(si);
        const output_state o = outputs.slot_state(slot);
        if (o.connector_id) {
            keep_position(o.connector_id, o);
            set_connector_disabled(o.connector_id, true);
        } else {
            for (virtual_head& vh : virtual_heads_) {
                if (vh.key != ht.key)
                    continue;
                vh.disabled = true;
                vh.mode = o.mode;
                vh.refresh_mhz = o.refresh_mhz;
                vh.scale = o.scale;
                vh.transform = o.transform;
                vh.position = o.position;
                vh.auto_position = o.auto_position;
            }
            if (fallback_headless_slot_ == si)
                fallback_headless_slot_ = -1;
        }
        remove_output(slot, false, "disabled by an output configuration");
    }
    outputs_changed_now();
    mark_heads_dirty();
    request_frame();
}

void compositor_impl::on_modeset_result(unsigned slot, unsigned ticket, bool ok) {
    if (!mgmt_ || !mgmt_->active || mgmt_->stage != mgmt_run::phase::committing ||
        ticket != mgmt_->ticket || slot >= output_manager::k_max)
        return;
    mgmt_run& r = *mgmt_;
    const std::uint32_t bit = 1u << slot;
    if (!(r.waiting & bit))
        return;
    r.waiting &= ~bit;
    if (!ok)
        r.failed = true;
    if (r.waiting == 0)
        complete_apply(r.failed ? "a modeset was refused" : "committed");
}

void compositor_impl::complete_apply(const char* why) {
    if (!mgmt_ || !mgmt_->active)
        return;
    mgmt_run& r = *mgmt_;
    if (r.failed) {
        // "The compositor should revert any changes made by the apply request" — back the
        // same way (no test: it is the state that was running).
        std::uint32_t ignore = 0;
        r.stage = mgmt_run::phase::testing; // stray results of the failed apply are ignored
        switch_to_target(r.previous, 0, ignore);
        finish_mgmt_job(mgmt_result::failed, "a modeset was refused after its test — reverted");
        return;
    }
    record_output_config();
    finish_mgmt_job(mgmt_result::succeeded, why);
}

void compositor_impl::finish_mgmt_job(mgmt_result result, const char* why) {
    if (!mgmt_)
        return;
    mgmt_run& r = *mgmt_;
    // The new state (heads and `done`) first, then the answer: a client that reads the state
    // when it hears `succeeded` (wlr-randr exits then) sees what it asked for.
    mark_heads_dirty();
    flush_output_heads();
    output_mgmt_finish(p0_, r.job.token, result);
    {
        char what[1024];
        describe_target(r.target, output_mgmt_heads(p0_), what, sizeof(what));
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.outputcfg",
                                         "output configuration {} {}: {} ({})",
                                         r.job.test ? "test" : "apply",
                                         result == mgmt_result::succeeded ? "succeeded"
                                         : result == mgmt_result::failed  ? "failed"
                                                                          : "cancelled",
                                         what[0] ? what : "-", why ? why : "");
    }
    r.active = false;
    r.waiting = 0;
    r.deadline_ns = 0;
    mark_heads_dirty();
    run_next_mgmt_job();
}

void compositor_impl::record_output_config() {
    if (!last_scan_)
        return;
    output_config& cfg = outcfg_.config();
    char ids[output_manager::k_max][monitor_config::k_id];
    unsigned entries[output_manager::k_max];
    const unsigned n = connected_identifiers(*last_scan_, ids, entries);
    bool changed = false;
    setup_config setup{};
    for (unsigned k = 0; k < n; ++k) {
        const drm::connector_probe& e = last_scan_->entries[entries[k]];
        int slot = -1;
        for (unsigned s = 0; s < output_manager::k_max && slot < 0; ++s)
            if (outputs.slot_live(s) && outputs.slot_state(s).connector_id == e.connector_id)
                slot = static_cast<int>(s);
        setup_output_config& so = setup.outputs[setup.count++];
        std::snprintf(so.identifier, sizeof(so.identifier), "%s", ids[k]);
        so.enabled = slot >= 0;
        if (slot < 0) {
            if (const kept_position* kp = kept_position_of(e.connector_id);
                kp && kp->has_position && !e.hotplug_mode_update) {
                so.has_position = true;
                so.x = kp->position.x;
                so.y = kp->position.y;
            }
            continue;
        }
        const output_state& o = outputs.slot_state(static_cast<unsigned>(slot));
        const bool vm = e.hotplug_mode_update;
        if (!o.auto_position && !vm) {
            so.has_position = true;
            so.x = o.position.x;
            so.y = o.position.y;
        }
        // The monitor's own: only what differs from what it gets anyway (the preferred mode,
        // the default scale, normal, VRR off), so a later default (`--scale`) still applies
        // where nothing was chosen.
        monitor_config m{};
        std::snprintf(m.identifier, sizeof(m.identifier), "%s", ids[k]);
        if (!vm && !o.follow_preferred) {
            m.has_mode = true;
            m.width = static_cast<unsigned>(o.mode.width);
            m.height = static_cast<unsigned>(o.mode.height);
            m.refresh_mhz = o.refresh_mhz;
        }
        const double def = outputs.default_scale_for(o.name, o.mode.width, o.mode.height,
                                                     o.phys_w_mm, o.phys_h_mm);
        if (o.scale != def) {
            m.has_scale = true;
            m.scale = o.scale;
        }
        if (o.transform != 0) {
            m.has_transform = true;
            m.transform = o.transform;
        }
        if (o.vrr_capable && o.adaptive_sync) {
            m.has_adaptive_sync = true;
            m.adaptive_sync = true;
        }
        const bool any = m.has_mode || m.has_scale || m.has_transform || m.has_adaptive_sync;
        if (any || find_monitor(cfg, ids[k]))
            changed = record_monitor(cfg, m) || changed;
    }
    if (setup.count > 0)
        changed = record_setup(cfg, setup) || changed;
    if (!changed)
        return;
    outcfg_.schedule_save(static_cast<unsigned long long>(now_ns()));
    if (outcfg_timer_ && outcfg_.save_due_ns() != 0) {
        const lx::runtime::clock_time due = static_cast<lx::runtime::clock_time>(outcfg_.save_due_ns());
        const bool job_due = mgmt_ && mgmt_->active && mgmt_->deadline_ns != 0 && mgmt_->deadline_ns < due;
        outcfg_timer_->schedule_at(job_due ? mgmt_->deadline_ns : due);
    }
}

void compositor_impl::outcfg_timer_callback() {
    compositor_impl* self = active;
    if (!self)
        return;
    const lx::runtime::clock_time now = now_ns();
    if (self->mgmt_ && self->mgmt_->active && self->mgmt_->deadline_ns != 0 &&
        now >= self->mgmt_->deadline_ns) {
        mgmt_run& r = *self->mgmt_;
        if (r.stage == mgmt_run::phase::testing) {
            self->finish_mgmt_job(mgmt_result::failed, "the TEST_ONLY did not answer");
        } else if (r.waiting == 0) {
            self->complete_apply("an output it waited for went");
        } else {
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.outputcfg",
                                            "apply: no modeset report within 3 s (an output is "
                                            "waiting for its frame) — answered succeeded");
            r.waiting = 0;
            self->complete_apply("modeset still pending");
        }
    }
    (void)self->outcfg_.run_due_save(static_cast<unsigned long long>(now), &post_to_worker);
    lx::runtime::clock_time next = 0;
    if (self->mgmt_ && self->mgmt_->active && self->mgmt_->deadline_ns != 0)
        next = self->mgmt_->deadline_ns;
    if (const auto due = static_cast<lx::runtime::clock_time>(self->outcfg_.save_due_ns());
        due != 0 && (next == 0 || due < next))
        next = due;
    if (self->outcfg_timer_) {
        if (next != 0)
            self->outcfg_timer_->schedule_at(next > now ? next : now + 1'000'000);
        else
            self->outcfg_timer_->set_enabled(false);
    }
}

namespace {
bool post_to_worker(void (*fn)(void*), void* arg) {
    compositor_impl* self = compositor_impl::active;
    if (!self || !self->threads_started.load(std::memory_order_acquire) ||
        self->cfg.worker_thread_count == 0)
        return false;
    return runtime::executor::global().post(runtime::affinity::worker, runtime::task{fn, arg});
}
} // namespace

void compositor_impl::hotplug_timer_callback() {
    if (!active || !active->resume_probe_wait_)
        return;
    active->resume_probe_wait_ = false;
    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.hotplug",
                                    "VT resume: the connector probe took over 250 ms — resuming "
                                    "with the outputs as they were");
    active->finish_display_resume(active->resume_probe_gen_);
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
    // A save of outputs.toml still pending is written now, while the worker that may be
    // writing the previous one still runs.
    outcfg_.flush();
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

bool compositor_impl::render_is_quiet() const {
    if (!ops_deferred_.empty() || capture_renderer_.has_pending() ||
        device_recovery_.needs_recovery())
        return false;
    for (const deferred_forget& f : deferred_forgets_)
        if (f.used)
            return false;
    for (const auto& up : pipes_) {
        if (!up)
            continue;
        const output_pipeline& p = *up;
        // A composited slot only waits for something with a CRTC to hand it to.
        if (p.retiring || p.graveyard || p.staged || !p.missed.empty() ||
            p.frame_outstanding.load(std::memory_order_relaxed) ||
            (p.kms && (p.composited_slot >= 0 || p.kms->flip_pending())) ||
            resume_gen_[p.slot].load(std::memory_order_acquire) != p.resume_done_gen)
            return false;
        if (unsigned front = 0; catch_up_owed(p, front))
            return false;
    }
    return true;
}

bool compositor_impl::render_has_work() const {
    return render_quiet_at_.load(std::memory_order_acquire) !=
               render_posts_.load(std::memory_order_acquire) ||
           render_mask_.load(std::memory_order_acquire) != 0 || !texture_updates_.empty() ||
           ops_pending_.load(std::memory_order_acquire) || cursor_frames_.fresh() ||
           cursor_images_.fresh() || screenshot_pending_.load(std::memory_order_acquire) != 0 ||
           capture_ui_posted_.load(std::memory_order_acquire);
}

void compositor_impl::on_vsync(std::uint32_t forced, bool housekeeping) {
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

    // The outputs whose start is due (two in phase share this tick, M6), plus those the
    // caller forces (the housekeeping tick).
    std::uint32_t due = (clocks_.take_due(tick.timestamp_ns) | forced) & outputs.live_mask();
    due &= ~hold_first_frames(due, tick.timestamp_ns);
    due_mask_ = due;
    // Health: the vblank each served output's repaint aims at (one store per output served).
    for (std::uint32_t m = due; m != 0; m &= m - 1) {
        const unsigned slot = static_cast<unsigned>(std::countr_zero(m));
        outs_[slot].aim_ns = clocks_.clock(slot).next_deadline(tick.timestamp_ns);
        outs_[slot].aim_frame = tick.frame_index;
    }
    in_vsync_ = true;
    tick_quiet_mask_ = 0;
    if (owner) owner->tick_ui(tick);
    in_vsync_ = false;

    // Render runs after every tick: composites of the views just published, a frame still
    // waiting for its flip, and cursor-only commits (the pointer moved, nothing else did).
    // The idle housekeeping tick wakes it only for work: otherwise it found nothing, once a
    // second — half of an idle desktop's wakeups.
    if (!housekeeping || render_has_work()) {
        {
            std::lock_guard lock{render_mutex};
            render_pending = true;
        }
        render_cv.notify_one();
    }
    settle_frame_clocks(due, tick_quiet_mask_);
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

        render_in_tick_.store(true, std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        if (owner) owner->tick_render();
        // The render half of the hot-path budget (render thread only: health reads it here).
        budget_.end_tick_render(static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()));
        runtime::executor::global().drain(runtime::affinity::render);
        render_ticks_.fetch_add(1, std::memory_order_release);
        render_in_tick_.store(false, std::memory_order_release);
    }

    // A frame composited but never committed still holds its fence; this thread owns it.
    for (auto& p : pipes_)
        if (p)
            p->discard_fence();
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
    lx::trace::timeline_instant("startup", "start entered");
    if (!impl_) {
        const auto err = lx::invalid_argument("null impl");
        lx::trace::logger::global().log_error(err, "compositor");
        return err;
    }

    impl_->scheduler_.set_target_fps(impl_->cfg.target_fps);

    // The hardware-Vulkan probe runs beside the logind and KMS opening below: they mostly
    // wait on D-Bus and the kernel, the probe is CPU work in the Vulkan loader. Only the UI
    // thread exists yet, so the scope's environment edit has no concurrent writer.
    bool hardware_vulkan = false;
    std::optional<gfx::hardware_probe_scope> probe_scope;
    std::thread probe;
    if (impl_->cfg.present == present_backend::automatic) {
        probe_scope.emplace();
        probe = std::thread([&hardware_vulkan] {
            hardware_vulkan = gfx::device_selector::has_hardware_vulkan();
        });
    }

    if (auto session = session::logind_session::open(); session)
        impl_->logind_ = std::move(session).value();

    // Prefer logind TakeDevice for the card node; fall back to direct open. `--virtual-outputs`
    // asks for virtual outputs instead of the display: no card is opened (headless and
    // multi-output protocol tests on a host whose display is free to take).
    const bool want_virtual = impl_->cfg.virtual_outputs && *impl_->cfg.virtual_outputs;
    if (!want_virtual) {
        lx::result<drm::kms_device> opened =
            lx::make_error(lx::drm_err::open_failed, "kms not opened");
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

    lx::trace::timeline_instant("startup", "kms opened");
    if (probe.joinable())
        probe.join();
    probe_scope.reset();
    lx::trace::timeline_instant("startup", "graphics probed");

    // Graphics. A Vulkan device is not free: where the only Vulkan driver is a software
    // rasterizer (lavapipe), creating one initialises LLVM and its JIT — about 28 MB of heap
    // (heaptrack) for a device the GL path never composites with. So probe with an instance
    // only, and create the device now only when Vulkan may composite; the fallbacks below
    // create it on demand. Failure must not abort compositor start.
    {
        const auto want = impl_->cfg.present;
        const bool vulkan_may_composite =
            want == present_backend::vulkan || want == present_backend::cpu ||
            (want == present_backend::automatic && hardware_vulkan);
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

    // Input devices open, and the cursor theme is read, beside the present path's set-up
    // (`begin_input`, `begin_cursor_theme`); joined before they are used, and on every early
    // return below.
    // The configuration file first: the keymap, the cursor and the device settings come
    // from it (a small read and parse, ~0.1 ms; design-session-and-wm C.4).
    impl_->load_initial_config();
    impl_->begin_input();
    impl_->begin_cursor_theme();
    struct helper_join {
        detail::compositor_impl* impl;
        ~helper_join() {
            if (impl->input_open_thread_.joinable())
                impl->input_open_thread_.join();
            if (impl->cursor_theme_thread_.joinable())
                impl->cursor_theme_thread_.join();
        }
    } helper_join_guard{impl_};

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
                // The context stays current here until the outputs' scanout targets exist
                // (`create_pipelines`); then it moves to the render thread.
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

    // Outputs (ADR 008 Part A): every connected desktop connector from the device's
    // start-up probe, each in its own slot with its own pipeline. `--scale` is the default
    // for every output (0: sway's HiDPI rule), `--output-scale NAME=S` per connector.
    lx::trace::timeline_instant("startup", "present path ready");
    impl_->outputs.set_default_scale(impl_->cfg.output_scale);
    if (const char* spec = impl_->cfg.output_scales; spec && *spec) {
        // NAME=S[,NAME=S...]
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", spec);
        for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ",")) {
            char* eq = std::strchr(tok, '=');
            if (!eq)
                continue;
            *eq = '\0';
            impl_->outputs.set_scale_override(tok, std::strtod(eq + 1, nullptr));
        }
    }
    // The persisted output configuration (ADR 008 §C.5), read before the first modeset so
    // the outputs come up as they were left. `--virtual-outputs` is a test mode: the user's
    // file is not read there unless LUMEN_OUTPUT_CONFIG names one.
    {
        const char* env = std::getenv("LUMEN_OUTPUT_CONFIG");
        const bool virtual_mode = impl_->cfg.virtual_outputs && *impl_->cfg.virtual_outputs;
        if (!virtual_mode || (env && *env && std::strcmp(env, "0") != 0))
            (void)impl_->outcfg_.load();
    }
    if (!impl_->drm_present_ && impl_->cfg.virtual_outputs && *impl_->cfg.virtual_outputs) {
        // --virtual-outputs WxH[@S][,WxH[@S]...]: N virtual outputs, laid out automatically,
        // each with its own clock (multi-output protocol tests without hardware).
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", impl_->cfg.virtual_outputs);
        unsigned n = 0;
        for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ",")) {
            int w = 0, h = 0;
            double sc = 0.0;
            if (std::sscanf(tok, "%dx%d@%lf", &w, &h, &sc) < 2 || w <= 0 || h <= 0)
                continue;
            output_desc d{};
            char name[32];
            std::snprintf(name, sizeof(name), "HEADLESS-%u", ++n);
            d.name = name;
            d.mode = {w, h};
            d.refresh_mhz = 60000;
            d.scale = sc;
            d.virtual_output = true;
            (void)impl_->outputs.add_output(d);
        }
    }
    if (impl_->outputs.count() == 0) {
        impl_->configure_outputs_at_start(impl_->kms.last_scan());
        // Nothing connected (or no display at all): HEADLESS-1 stands in, and a display
        // that is plugged in later replaces it.
        if (impl_->outputs.count() == 1) {
            const int s = impl_->outputs.slot_of(impl_->outputs.nth(0));
            if (s >= 0 && impl_->outputs.slot_state(static_cast<unsigned>(s)).connector_id == 0)
                impl_->fallback_headless_slot_ = s;
        }
    }
    if (auto made = impl_->create_pipelines(); !made) {
        lx::trace::logger::global().log_error(made.get_error(), "compositor.drm");
        if (impl_->drm_present_) {
            // No output can be driven: composite nowhere rather than fail every frame.
            for (auto& p : impl_->pipes_)
                if (p)
                    p->kms.reset();
        }
    }
    // The GL context has to move to the render thread, which claims it in render_loop; it
    // cannot be current on two threads at once.
    if (impl_->gl_present_) {
        impl_->warm_gl_paths();
        impl_->egl_.release_current();
    }
    if (impl_->outputs.count() > 0) {
        // Kept for output 0 in slot order: the smokes and inject-input.py parse this line.
        const output_state o0 = impl_->outputs.state(impl_->outputs.nth(0));
        const lx::rect2i g = o0.mode_rect();
        const lx::rect2i l = o0.logical_rect();
        lx::trace::logger::global().logf(lx::trace::level::info, "compositor.output",
                                         "output 0 mode={}x{} refresh_mhz={} scale={:g} ({}/120) "
                                         "logical={}x{}",
                                         g.width, g.height, o0.refresh_mhz, o0.scale,
                                         std::lround(o0.scale * 120.0), l.width, l.height);
    }
    impl_->log_outputs();
    impl_->output_generation_ = impl_->outputs.generation();

    lx::trace::timeline_instant("startup", "outputs ready");
    if (auto bound = impl_->wayland.bind(impl_->cfg.socket_name); !bound) {
        lx::trace::logger::global().log_error(bound.get_error(), "compositor");
        return bound.get_error();
    }

    lx::trace::timeline_instant("startup", "socket bound");
    // Cursor: themes first (they decide how large the planes' buffers must be), then one
    // plane per pipeline, then the manager that feeds them. Startup-only file I/O.
    {
        auto* impl = impl_;
        // One theme per distinct output scale, at that scale's pixel size: XCURSOR_SIZE (or
        // 24) is a logical size, so a 1.25 output shows 30 px images — sharp, not a 24 px
        // image stretched (§A.7). At most k_cursor_scales; past that, the nearest is used.
        const unsigned base = impl->cursor_base_px();
        unsigned scales[k_cursor_scales]{};
        unsigned n = 0;
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
            if (!impl->outputs.slot_live(slot))
                continue;
            const unsigned s120 =
                static_cast<unsigned>(std::lround(impl->outputs.slot_state(slot).scale * 120.0));
            bool seen = false;
            for (unsigned i = 0; i < n; ++i)
                seen = seen || scales[i] == s120;
            if (!seen && n < k_cursor_scales)
                scales[n++] = s120;
        }
        if (n == 0)
            scales[n++] = static_cast<unsigned>(std::lround(impl->outputs.default_scale() * 120.0));
        unsigned need = 0;
        if (impl->cursor_theme_thread_.joinable())
            impl->cursor_theme_thread_.join();
        bool adopted = false;
        for (unsigned i = 0; i < n; ++i) {
            if (!adopted && scales[i] == impl->prefetched_scale120_) {
                impl->cursor_themes_[i] = std::move(impl->prefetched_theme_);
                adopted = true;
            } else {
                const long px = std::lround(static_cast<double>(base) * scales[i] / 120.0);
                impl->cursor_themes_[i].load(impl->cursor_theme_arg(),
                                             px > 0 ? static_cast<unsigned>(px) : base);
            }
            if (impl->cursor_themes_[i].max_dimension() > need)
                need = impl->cursor_themes_[i].max_dimension();
            lx::trace::logger::global().logf(
                lx::trace::level::info, "compositor.cursor",
                "cursor theme={} size={} scale={}/120 from_theme={} "
                "builtin={}",
                impl->cursor_themes_[i].name(), impl->cursor_themes_[i].size(), scales[i],
                impl->cursor_themes_[i].from_theme(), impl->cursor_themes_[i].builtin());
        }
        impl->cursor_theme_count_ = n;
        impl->cursor_base_px_ = base;
        for (unsigned i = 0; i < n; ++i)
            impl->cursor_theme_scale120_[i] = scales[i];

        const char* mode = std::getenv("LUMEN_CURSOR");
        const bool force_software = mode && std::strcmp(mode, "software") == 0;
        const char* why_software = nullptr;
        unsigned planes = 0;
        unsigned pipelines = 0;
        lx::size2i plane_size{};
        if (force_software) {
            why_software = "LUMEN_CURSOR=software";
        } else if (!impl->drm_present_) {
            why_software = "no KMS scanout";
        } else {
            impl->cursor_plane_px_ = need;
            impl->create_cursor_planes(need);
            for (const auto& p : impl->pipes_) {
                if (!p || !p->kms)
                    continue;
                ++pipelines;
                if (!p->cursor.valid())
                    continue;
                ++planes;
                const auto caps = p->cursor.caps();
                // The image set is staged into every plane: the smallest plane bounds it.
                if (plane_size.width == 0 || static_cast<int>(caps.width) < plane_size.width)
                    plane_size.width = static_cast<int>(caps.width);
                if (plane_size.height == 0 || static_cast<int>(caps.height) < plane_size.height)
                    plane_size.height = static_cast<int>(caps.height);
            }
            if (pipelines == 0)
                why_software = "no CRTC pipeline";
            else if (planes < pipelines)
                why_software = "an output has no cursor plane";
        }
        char line[160];
        if (!why_software && planes > 0) {
            std::snprintf(line, sizeof(line),
                          "hardware cursor planes=%u size=%dx%d hotspot=%d", planes,
                          plane_size.width, plane_size.height,
                          impl->kms.cursor_hotspot_cap() ? 1 : 0);
        } else {
            // Software everywhere: the planes stay off (a cursor shown on some outputs by a
            // plane and on others by the scene would draw it twice where they meet).
            for (auto& p : impl->pipes_)
                if (p && p->cursor.valid())
                    p->cursor = drm::kms_cursor_plane{};
            std::snprintf(line, sizeof(line), "software cursor: %s",
                          why_software ? why_software : "no cursor plane");
        }
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.cursor", line);

        cursor_manager::setup setup{};
        for (unsigned i = 0; i < n; ++i) {
            setup.themes[i] = &impl->cursor_themes_[i];
            setup.theme_scale120[i] = scales[i];
        }
        setup.theme_count = n;
        setup.frames = &impl->cursor_frames_;
        setup.images = &impl->cursor_images_;
        setup.router = &impl->input_router_;
        setup.scene = &impl->scene;
        setup.overlay = &impl->cursor_layer_;
        setup.hardware = !why_software && planes > 0;
        setup.plane_size = plane_size;
        impl->cursor_.init(setup);
    }

    lx::trace::timeline_instant("startup", "cursor ready");
    impl_->register_p0_globals();
    lx::trace::timeline_instant("startup", "globals registered");
    impl_->finish_input();
    lx::trace::timeline_instant("startup", "input started");

    // The pointer starts in the middle of the first output in layout order (leftmost, then
    // topmost), where it is easy to find.
    if (impl_->outputs.count() > 0) {
        int first = -1;
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
            if (!impl_->outputs.slot_live(slot))
                continue;
            const lx::rect2i l = impl_->outputs.slot_state(slot).logical_rect();
            if (first < 0) {
                first = static_cast<int>(slot);
                continue;
            }
            const lx::rect2i f = impl_->outputs.slot_state(static_cast<unsigned>(first)).logical_rect();
            if (l.x < f.x || (l.x == f.x && l.y < f.y))
                first = static_cast<int>(slot);
        }
        const lx::rect2i g = impl_->outputs.slot_state(static_cast<unsigned>(first)).logical_rect();
        auto& seat = impl_->input_devices_.default_seat();
        seat.set_pointer_position(g.x + g.width / 2, g.y + g.height / 2);
        impl_->cursor_.on_motion(seat.pointer_x(), seat.pointer_y(), 0, false);
    }

    if (auto installed = impl_->shell_bridge_.install(impl_->cfg.shell_binary_path); !installed) {
        lx::trace::logger::global().log_error(installed.get_error(), "compositor");
        return installed.get_error();
    }

    lx::trace::timeline_instant("startup", "shell bridge installed");
    detail::compositor_impl::active = impl_;

    // One frame clock per output, each following its own display's refresh (not a
    // configured target: a 60 Hz timer against a 75 Hz panel beats against the flips), over
    // one shared timer armed at the earliest due output (ADR 008 M6). The configured fps is
    // the period of an output with no refresh to read.
    const runtime::clock_time fallback_ns =
        static_cast<runtime::clock_time>(1'000'000'000.0 / impl_->cfg.target_fps);
    impl_->vsync_timer = std::make_unique<runtime::timer_source>(
        fallback_ns, &detail::compositor_impl::vsync_callback, true);
    impl_->vsync_timer->set_enabled(false);
    impl_->loop.add_source(impl_->vsync_timer.get());
    impl_->housekeeping_timer = std::make_unique<runtime::timer_source>(
        detail::compositor_impl::k_housekeeping_ns,
        &detail::compositor_impl::housekeeping_callback, true);
    impl_->housekeeping_timer->set_enabled(false); // only while every frame clock is stopped
    impl_->loop.add_source(impl_->housekeeping_timer.get());
    impl_->callback_timer_ = std::make_unique<runtime::timer_source>(
        0, &detail::compositor_impl::callback_timer_callback, false);
    impl_->callback_timer_->set_enabled(false);
    impl_->loop.add_source(impl_->callback_timer_.get());
    impl_->first_frame_timer_ = std::make_unique<runtime::timer_source>(
        0, &detail::compositor_impl::first_frame_timer_callback, false);
    impl_->first_frame_timer_->set_enabled(false);
    impl_->loop.add_source(impl_->first_frame_timer_.get());
    impl_->sync_output_views();
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
    // Output configuration: an apply's deadline and the debounced save of outputs.toml.
    impl_->outcfg_timer_ = std::make_unique<runtime::timer_source>(
        0, &detail::compositor_impl::outcfg_timer_callback, false);
    impl_->outcfg_timer_->set_enabled(false);
    impl_->loop.add_source(impl_->outcfg_timer_.get());

    // Every fd the UI thread serves is waited on, not polled: the loop sleeps until one is
    // readable or a timer is due.
    if (const int wfd = impl_->wayland.event_fd(); wfd >= 0) {
        impl_->wayland_fd_source = std::make_unique<runtime::fd_source>(
            wfd, &detail::compositor_impl::wayland_fd_callback);
        impl_->loop.add_source(impl_->wayland_fd_source.get());
    }
    // Fences queued content updates wait on (sync_files, syncobj eventfds): one epoll fd,
    // readable only while one has signalled.
    if (const int cfd = content_updates_poll_fd(impl_->p0_); cfd >= 0) {
        impl_->content_fd_source = std::make_unique<runtime::fd_source>(
            cfd, &detail::compositor_impl::content_fd_callback);
        impl_->loop.add_source(impl_->content_fd_source.get());
    }
    // The card reports readable only while a DRM event (a flip) is queued, and
    // drmHandleEvent consumes it — so the flip handler runs the moment the flip lands.
    if (impl_->drm_present_ && impl_->kms.card_fd() >= 0) {
        impl_->drm_fd_source = std::make_unique<runtime::fd_source>(
            impl_->kms.card_fd(), &detail::compositor_impl::drm_fd_callback);
        impl_->loop.add_source(impl_->drm_fd_source.get());
        // Hotplug (ADR 008 Part B): drm uevents of this card wake the UI loop; the probe runs
        // on the worker. Without a monitor the connectors are still probed at every resume.
        hotplug_host host{};
        host.user = impl_;
        host.scan_ready = [](void* u, const drm::connector_scan& scan, bool forced) {
            auto* self = static_cast<detail::compositor_impl*>(u);
            // A probe that finished after the VT went away may be the kernel's cache: the
            // resume probes everything again.
            if (!forced && self->session_.display_stopped())
                return;
            self->apply_connector_scan(scan, forced);
            if (forced && self->resume_probe_wait_) {
                self->resume_probe_wait_ = false;
                self->finish_display_resume(self->resume_probe_gen_);
            }
        };
        host.card_removed = [](void* u) { static_cast<detail::compositor_impl*>(u)->on_card_removed(); };
        const unsigned long long card =
            static_cast<unsigned long long>(makedev(impl_->drm_major_, impl_->drm_minor_));
        const bool worker = impl_->cfg.worker_thread_count > 0;
        if (auto monitor = impl_->hotplug_.start(impl_->kms, card, impl_->loop, worker, host);
            monitor) {
            lx::trace::logger::global().logf(lx::trace::level::info, "compositor.hotplug",
                                             "hotplug: udev monitor on drm {}:{}",
                                             impl_->drm_major_, impl_->drm_minor_);
        } else {
            lx::trace::logger::global().log_error(monitor.get_error(), "compositor.hotplug");
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.hotplug",
                                            "no hotplug monitor — connectors are probed again "
                                            "only when the session resumes");
            impl_->hotplug_.attach(impl_->kms, impl_->loop, worker, host);
        }
        impl_->hotplug_ready_ = true;
        impl_->hotplug_timer_ = std::make_unique<runtime::timer_source>(
            0, &detail::compositor_impl::hotplug_timer_callback, false);
        impl_->hotplug_timer_->set_enabled(false);
        impl_->loop.add_source(impl_->hotplug_timer_.get());
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

    // The pause/resume machine runs only where there is a display to lose. Starting while
    // the session is in the background pauses right away (input is held until the switch).
    {
        lx::trace::logger::global().logf(
            lx::trace::level::info, "compositor.session",
            "session {} vt={} can_switch_vt={} active={} drm={}:{} found={} type={}",
            impl_->logind_.session_id()[0] ? impl_->logind_.session_id() : "-",
            impl_->logind_.vt_number(), impl_->logind_.can_switch_vt() ? 1 : 0,
            impl_->logind_.is_active() ? 1 : 0, impl_->drm_major_, impl_->drm_minor_,
            session::session_found_by_name(impl_->logind_.found_by()),
            impl_->logind_.session_type()[0] ? impl_->logind_.session_type() : "-");
        impl_->apply_session_actions(impl_->session_.init(
            impl_->drm_present_, impl_->drm_major_, impl_->drm_minor_, impl_->drm_start_inactive_));
    }

    impl_->request_frame();
    lx::trace::logger::global().log(lx::trace::level::info, "compositor",
                                    "threads: ui(main)+render+worker; frame clocks armed");

    // After the socket is served: the control socket, the session environment, READY.
    impl_->apply_initial_config();
    impl_->start_session();
    return {};
}

int compositor::run() {
    if (!impl_) return 1;

    runtime::set_current_affinity(runtime::affinity::ui);
    const int code = impl_->loop.run();
    // The session environment goes first (applications started from now on must not find
    // a display that is going), then the control socket.
    impl_->end_session();
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

void compositor::request_reload() {
    if (!impl_)
        return;
    impl_->reload_requested_.store(true, std::memory_order_release);
    impl_->loop.wake();
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
    // A move to (x, y) is a relative motion by the difference, as a mouse would make it.
    auto& seat = impl_->input_devices_.default_seat();
    e.dx = event.x - seat.pointer_x();
    e.dy = event.y - seat.pointer_y();
    if (event.dx_unaccel == 0.0 && event.dy_unaccel == 0.0) {
        e.dx_unaccel = e.dx;
        e.dy_unaccel = e.dy;
    }
    seat.set_pointer_position(event.x, event.y);
    impl_->on_input_pointer_motion(e);
}

void compositor::inject_pointer_relative(double dx, double dy, double dx_unaccel,
                                         double dy_unaccel, unsigned long long time_us) {
    if (!impl_)
        return;
    auto& seat = impl_->input_devices_.default_seat();
    lx::input::pointer_motion_event e{};
    e.dx = dx;
    e.dy = dy;
    e.dx_unaccel = dx_unaccel;
    e.dy_unaccel = dy_unaccel;
    e.time_us = time_us;
    e.time_ms = static_cast<unsigned>(time_us / 1000ull);
    seat.set_pointer_position(seat.pointer_x() + dx, seat.pointer_y() + dy);
    e.x = seat.pointer_x();
    e.y = seat.pointer_y();
    impl_->on_input_pointer_motion(e);
}

void compositor::inject_gesture(const lx::input::gesture_event& event) {
    if (impl_)
        impl_->on_input_gesture(event);
}

void compositor::inject_pointer_button(const lx::input::pointer_button_event& event) {
    if (impl_)
        impl_->on_input_pointer_button(event);
}

void compositor::simulate_connector_scan(const lx::drm::connector_scan& scan) {
    runtime::assert_affinity(runtime::affinity::ui);
    if (impl_)
        impl_->apply_connector_scan(scan, false);
}

bool compositor::render_and_wait() {
    if (!impl_ || !impl_->threads_started.load(std::memory_order_acquire))
        return false;
    const unsigned before = impl_->render_ticks_.load(std::memory_order_acquire);
    impl_->wake_render();
    for (int i = 0; i < 2000; ++i) {
        // A tick that was already running when woken does not count: one that started after.
        if (impl_->render_ticks_.load(std::memory_order_acquire) - before >= 2u ||
            (i > 5 && impl_->render_ticks_.load(std::memory_order_acquire) != before))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void compositor::run_posted_ui_tasks() {
    runtime::assert_affinity(runtime::affinity::ui);
    // A reload asked for with request_reload (SIGHUP), as the loop's after-dispatch step
    // handles it; first, so a reload's worker results are drained after it.
    if (impl_)
        impl_->handle_reload_request();
    runtime::executor::global().drain(runtime::affinity::ui);
    if (impl_)
        impl_->flush_output_heads();
}

const output_config& compositor::persisted_outputs() const { return impl_->outcfg_.config(); }

void compositor::save_output_config_now() {
    runtime::assert_affinity(runtime::affinity::ui);
    if (impl_)
        impl_->outcfg_.flush();
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
const toplevel_icon_table& compositor::toplevel_icons() const { return impl_->icons_; }
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
scheduler::budget_tracker& compositor::budget() { return impl_->budget_; }
buffer_lifecycle_tracker& compositor::buffer_lifecycle() { return impl_->buffer_lifecycle_; }
runtime::memory_budget_coordinator& compositor::memory() { return impl_->memory_; }

runtime::executor& compositor::executor() {
    return runtime::executor::global();
}

void compositor::tick_ui(scheduler::frame_tick tick) {
    runtime::assert_affinity(runtime::affinity::ui);
    lx::trace::scoped_span span{"ui", "tick_ui"};
    auto* impl = impl_;

    const auto t0 = std::chrono::steady_clock::now();
    // Protocol handlers date texture retirements against this, so it has to be current
    // before any Wayland dispatch below. What is committed from here on belongs to this
    // tick's snapshot at the earliest.
    impl->p0_.frame_index = tick.frame_index;
    impl->p0_.building_frame = tick.frame_index;
    impl->budget_.begin_frame(tick.frame_index);
    impl->p0_.now_ns = static_cast<unsigned long long>(tick.timestamp_ns);
    impl->advance_logout(tick.timestamp_ns);
    // Output heads changed since the last loop iteration (tests drive ticks without the loop).
    impl->flush_output_heads();
    // Staging: free retired CPU-path storage the render thread has let go of, unmap GL
    // blocks idle past the threshold, and report what is left against the budget.
    impl->shm_store_.collect();
    impl->shm_pool_.collect(impl->p0_.now_ns, detail::compositor_impl::k_staging_idle_ns);
    {
        const unsigned long long staged = impl->shm_store_.bytes_held() +
                                          impl->shm_pool_.bytes_mapped();
        lx::trace::timeline_counter("staging_bytes", static_cast<long long>(staged));
        const unsigned clamped =
            staged > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<unsigned>(staged);
        impl->memory_.report_usage(lx::runtime::memory_pool::staging, clamped, 0);
    }
    impl->memory_.tick(tick.frame_index);
    impl->memory_.reset_frame_arenas(lx::runtime::memory_pool::ui);

    // wl_output globals withdrawn 5 s ago are destroyed now (`server::remove_global`).
    wayland().collect_globals(static_cast<unsigned long long>(tick.timestamp_ns));
    // Outputs changed since the last tick (a test moved one; hotplug applies its changes
    // at once, so usually nothing is left here): views, clocks, clients, the pointer.
    if (impl->outputs.layout_generation() != impl->applied_layout_generation_ ||
        impl->outputs.generation() != impl->output_generation_)
        impl->outputs_changed_now();
    if (!impl->initial_damage_noted_) {
        impl->initial_damage_noted_ = true;
        scene().note_full_damage();
    }

    // The outputs this tick serves: the due ones from the vsync timer; every live one when
    // a test drives `tick_ui` directly.
    const std::uint32_t live = impl->outputs.live_mask();
    const std::uint32_t served = impl->in_vsync_ ? (impl->due_mask_ & live) : live;
    const std::uint32_t dark = impl->gate_power_mask_.load(std::memory_order_relaxed);

    {
        // DRM flips and the logind bus have sources of their own and are handled the
        // moment they are readable; requests that arrived since the loop last looked are
        // taken now, so they make this frame.
        lx::trace::scoped_span dispatch_span{"ui", "dispatch"};
        // From here to the publish, what is applied joins this tick's frame (the
        // commit-timing prediction of the served outputs is this tick's deadline).
        impl->p0_.cu_in_tick = true;
        // Unflushed: the tick flushes once at its end, so what these requests produce (a
        // buffer release) leaves together with the frame's events in one send.
        (void)wayland().dispatch(0, false);
    }
    // Queued content updates whose target this tick's frame reaches, or that stopped being
    // shown, apply now and join it.
    content_updates_try(impl->p0_);

    // wl_surface.frame means "the content you committed has been shown". Answering it once
    // per tick instead makes clients render frames that are composited over and never reach
    // the display, so on the KMS path a surface's callbacks wait for a flip of its pace
    // output (M8). Without a present path for an output (virtual) its tick is the clock.
    const bool paused = impl->display_gate_.load(std::memory_order_relaxed) != 0;
    const auto frame_ms = static_cast<unsigned>(tick.timestamp_ns / 1'000'000ull);
    if (paused) {
        // The 1 Hz paused tick: every callback is answered by it — a client blocked in
        // eglSwapBuffers keeps running, and one that honours `suspended` stops drawing on
        // its own (niri's 995 ms throttle, for the same reason).
        for (auto& o : impl->outs_) {
            o.awaiting_resume_flip = false;
            o.last_callback_ns = tick.timestamp_ns;
        }
        fire_frame_callbacks(impl->p0_, frame_ms);
        // Nothing reaches the screen while stopped: queued presentation feedback is
        // discarded once a second, so a client pacing by it (mpv) falls back to its timer.
        presentation_discard(impl->p0_, true);
        // A lock under way while nothing is shown is locked now.
        impl->advance_lock();
    } else {
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
            detail::output_ui& o = impl->outs_[slot];
            if (!(served & (1u << slot)) || (dark & (1u << slot)))
                continue;
            const detail::output_pipeline* p = o.pipe;
            const bool kms_output = p && p->kms;
            if (o.last_callback_ns == 0)
                o.last_callback_ns = tick.timestamp_ns;
            const bool stalled =
                tick.timestamp_ns - o.last_callback_ns >=
                (o.awaiting_resume_flip ? detail::compositor_impl::k_resume_flip_grace_ns
                                        : detail::compositor_impl::k_callback_stall_ns);
            if (!kms_output || o.present_completed || stalled) {
                if (stalled && !o.present_completed && kms_output) {
                    lx::trace::logger::global().logf(lx::trace::level::warn, "compositor",
                                                     "no present in 250ms on output {} — pacing "
                                                     "frame callbacks by tick",
                                                     slot);
                }
                o.present_completed = false;
                o.callback_release_ns = 0;
                o.awaiting_response = true;
                o.last_callback_ns = tick.timestamp_ns;
                unsigned ms = frame_ms;
                if (o.last_feedback.hw_clock && o.last_feedback.actual_present_ns > 0)
                    ms = static_cast<unsigned>(o.last_feedback.actual_present_ns / 1'000'000ull);
                // Surfaces this output paces now; the ones on no output (covered, off every
                // output, unmapped) at most once per 995 ms.
                fire_output_frame_callbacks(impl->p0_, slot, ms,
                                            static_cast<unsigned long long>(tick.timestamp_ns));
            }
        }
    }

    // Window management for this frame — configures coalesced since the last tick go out
    // here, once, rather than once per request that changed them.
    {
        lx::trace::scoped_span wm_span{"ui", "wm.on_tick"};
        impl->wm_.on_tick(tick.timestamp_ns);
    }
    // Frames: the atlas on first use and changed titles, rasterized once per tick however
    // many set_title requests arrived — in this tick's snapshot, with the geometry they fit.
    {
        lx::trace::scoped_span deco_span{"ui", "decorations"};
        impl->decorations_.on_tick();
    }
    // The pointer image, after the window manager (a grab sets the override) and before the
    // scene is published (the software cursor is a scene node).
    if (impl->cursor_hw_failed_.load(std::memory_order_acquire))
        impl->cursor_.on_hardware_failed();
    {
        lx::trace::scoped_span cursor_span{"ui", "cursor"};
        impl->cursor_.on_tick();
    }

    scheduler().on_vsync(tick);
    {
        lx::trace::scoped_span scene_span{"ui", "scene.update"};
        scene().update(tick.delta_seconds);
    }

    // Publish the served outputs that have something new: each view is built with its own
    // projection and damage (M3), and a view with nothing new publishes nothing — its last
    // snapshot is still what it shows.
    const std::uint32_t damaged = scene().pending_damage_mask();
    std::uint32_t published = 0;
    std::uint32_t dropped = 0;
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        const std::uint32_t bit = 1u << slot;
        detail::output_ui& o = impl->outs_[slot];
        if (!(served & bit) || !o.live || (dark & bit))
            continue;
        if (!(damaged & bit) && o.published_once)
            continue;
        lx::trace::scoped_span publish_span{"ui", "publish"};
        if (!scene().commit_frame(slot, tick.frame_index)) {
            dropped |= bit;
            continue;
        }
        published |= bit;
        o.published_once = true;
        o.last_published_frame = tick.frame_index;
        impl->p0_.published_frame[slot] = tick.frame_index;
        impl->p0_.published_mask |= bit;
        if (damaged & bit)
            o.last_damaged_frame = tick.frame_index;
    }
    impl->p0_.lit_mask = paused ? 0u : (live & ~dark);
    impl->p0_.cu_in_tick = false;
    if (published != 0) {
        // Published: commits dispatched between now and the next tick are the next
        // snapshot's. A dropped publish keeps this frame, whose damage carries forward.
        impl->p0_.building_frame = tick.frame_index + 1;
        impl->last_published_any_ = tick.frame_index;
        impl->buffer_lifecycle_.on_frame_submitted(tick.frame_index);
        impl->render_mask_.fetch_or(published, std::memory_order_release);
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
            if (!(published & (1u << slot)))
                continue;
            // wp_presentation: queued feedback this output paces is sampled into the frame
            // (the visibility pass just ran). Not while stopped: nothing is shown.
            if (paused)
                continue;
            presentation_published(impl->p0_, tick.frame_index, slot);
            const detail::output_pipeline* p = impl->outs_[slot].pipe;
            if (!(p && p->kms)) {
                // No display to flip (a virtual output): the tick is the only presentation
                // clock there is, so the frame counts as presented when it is published —
                // in software, so no flags (wlroots' headless backend does the same).
                present_info info{};
                info.timestamp_ns = static_cast<unsigned long long>(tick.timestamp_ns);
                const unsigned mhz = impl->outputs.slot_state(slot).refresh_mhz;
                if (mhz > 0)
                    info.refresh_ns = static_cast<unsigned>((1'000'000'000'000ull + mhz / 2) / mhz);
                presentation_flipped(impl->p0_, tick.frame_index, slot, info);
                // Likewise a locked frame counts as shown once published.
                if (impl->lock_.state() == lock_state::locking &&
                    static_cast<int>(tick.frame_index - impl->outs_[slot].lock_frame) >= 0)
                    impl->note_lock_shown(slot);
            }
        }
    }
    if (published != 0 && !paused)
        impl->evaluate_tearing(published);
    // fifo-v1: each served output latched this tick's frame (published, or nothing new to
    // show): barriers it paces clear, and the updates waiting on them apply — they join the
    // next frame, one update per refresh.
    if (!paused) {
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot)
            if ((served & (1u << slot)) && !(dark & (1u << slot)))
                content_updates_latched(impl->p0_, slot, tick.frame_index);
    }
    if (dropped != 0) {
        // The render thread still holds every other slot of that view. Visible on the
        // timeline as an instant, so a stutter can be told apart from a frame that had
        // nothing new.
        lx::trace::timeline_instant("ui", "publish_dropped");
        lx::trace::logger::global().log(lx::trace::level::trace, "compositor",
                                        "frame dropped (backpressure)");
    }

    // Idle inhibitors (I3): honoured while their surface was on screen on a lit output in
    // its newest published frame. Only looked at while one exists, or while inhibition
    // still has to end — an empty set costs nothing.
    if (impl->inhibitors_.count() > 0 || impl->idle_.inhibited()) {
        const bool inhibited = !paused && idle_inhibitor_visible(impl->p0_);
        if (impl->idle_.set_inhibited(inhibited,
                                      static_cast<unsigned long long>(tick.timestamp_ns))) {
            impl->rearm_idle_timer();
            if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.idle",
                                                inhibited ? "idle inhibited=1" : "idle inhibited=0");
        }
    }

    // An idle tick of an output produces no flip there, and its frame callbacks otherwise
    // wait for one — a client that commits a frame request without new content would then
    // stall until the watchdog. Once every damaged frame of the output has been composited
    // and committed and no flip is queued on it, nothing is waiting on its display: answer
    // the callbacks it paces now (wlroots' "frame done without a commit"). The reads are
    // ordered against render's writes: `rendered_frame` is published after
    // `frame_outstanding` is raised, and `frame_outstanding` is lowered only after the
    // commit has set flip_pending.
    std::uint32_t quiet = 0;
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        const std::uint32_t bit = 1u << slot;
        if (!(served & bit))
            continue;
        detail::output_ui& o = impl->outs_[slot];
        const detail::output_pipeline* p = o.pipe;
        const long long rendered = p ? p->rendered_frame.load(std::memory_order_acquire)
                                     : static_cast<long long>(o.last_published_frame);
        const bool outstanding = p && p->frame_outstanding.load(std::memory_order_acquire);
        const bool flip = p && p->kms && p->kms->flip_pending();
        const bool settled = !(damaged & bit) && rendered >= o.last_damaged_frame &&
                             !outstanding && !flip;
        if (p && p->kms && !paused && !(dark & bit) && settled) {
            o.last_callback_ns = tick.timestamp_ns;
            fire_output_frame_callbacks(impl->p0_, slot, frame_ms,
                                        static_cast<unsigned long long>(tick.timestamp_ns));
        }
        if (settled && !(published & bit))
            quiet |= bit;
    }

    // Release client buffers no output can still sample (the render low-water mark).
    // Done here because sending wl_buffer.release requires the UI thread's ownership of
    // Wayland dispatch.
    long long low_water = impl->render_low_water_.load(std::memory_order_acquire);
    if (impl->p0_.capture) {
        // A window capture job render has yet to composite holds back what its view samples.
        if (const long long floor = capture_view_floor(impl->p0_); floor >= 0 && floor <= low_water)
            low_water = floor - 1;
    }
    (void)impl->buffer_lifecycle_.retire_completed_frames(low_water);
    // Likewise shm buffers the render thread has uploaded from client memory.
    impl->shm_direct_.retire();

    // Screen capture (design-app-compat-capture Part D): completions render left, then this
    // tick's decisions — constraints, copies, curtains, window capture views published with
    // this frame — and the privacy indicator.
    if (impl->p0_.capture) {
        capture_complete(impl->p0_);
        impl->refresh_capture_host();
        capture_tick(impl->p0_, tick.frame_index, true);
    }

    // Whether the next tick has anything to do. Per output: damage to composite, a frame
    // the render thread has not reached, a flip or commit outstanding (`quiet` above);
    // anywhere: a frame callback or buffer waiting on a later frame, deferred WM work.
    const bool global_busy = impl->buffer_lifecycle_.has_unreleased_frames() ||
                             !impl->shm_direct_.idle() || impl->wm_.needs_tick() ||
                             impl->logout_active_; // logout ends on a tick
    // A frame callback keeps the output that paces it ticking (its flip or its idle rule
    // answers it), not every output.
    const std::uint32_t waiting = frame_callbacks_pending(impl->p0_)
                                      ? frame_callbacks_pending_outputs(impl->p0_)
                                      : 0u;
    // A queued fifo update of a shown surface needs its pace output's next latch.
    const std::uint32_t fifo_waiting = content_updates_waiting_outputs(impl->p0_);
    // A window capture waiting for its window to change needs that window's output ticking.
    const std::uint32_t capture_waiting = capture_wanted_outputs(impl->p0_);
    impl->tick_quiet_mask_ =
        global_busy ? 0u : (quiet & ~waiting & ~fifo_waiting & ~capture_waiting);
    // New damage on outputs this tick did not serve (a client of another output committed):
    // their clocks start, on their own phase.
    if (const std::uint32_t pending = scene().pending_damage_mask() & ~served & live & ~dark;
        pending != 0 && !paused)
        impl->request_frame(pending);
    if (capture_waiting != 0 && !paused)
        impl->request_frame(capture_waiting & live & ~dark);

    // Configures and events produced by this tick leave now, not at the next poll.
    wayland().flush();

    const auto us = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    impl->budget_.end_tick_ui(us);
    if (impl->budget_.tick_ui_over_budget()) {
        ++impl->ui_over_budget_;
        if (us > impl->ui_over_budget_max_us_)
            impl->ui_over_budget_max_us_ = us;
    }
    if (++impl->ui_budget_window_ >= detail::compositor_impl::k_budget_report_frames) {
        if (impl->ui_over_budget_ > 0) {
            lx::trace::logger::global().logf(lx::trace::level::warn, "compositor",
                                             "tick_ui over budget in recent frames ({} of {}, "
                                             "worst {} us)",
                                             impl->ui_over_budget_, impl->ui_budget_window_,
                                             impl->ui_over_budget_max_us_);
        }
        impl->ui_over_budget_ = 0;
        impl->ui_over_budget_max_us_ = 0;
        impl->ui_budget_window_ = 0;
    }
}

void compositor::tick_render() {
    runtime::assert_affinity(runtime::affinity::render);
    lx::trace::scoped_span span{"render", "tick_render"};
    auto* impl = impl_;
    // Before anything this tick reads: work UI posts from here on wakes render again.
    const unsigned long long posts_seen = impl->render_posts_.load(std::memory_order_acquire);

    // The pause gate, in this order (see `resume_gen_`). A resume runs its procedure before
    // anything else this tick: the KMS state, the cursor plane and every slot's history are
    // whatever the last master left.
    // Output plans first (ADR 008 §A.4): pipelines added, tested, switched, retired.
    impl->run_output_ops();
    const bool vt_paused =
        (impl->display_gate_.load(std::memory_order_acquire) & detail::compositor_impl::gate_vt) != 0;
    const std::uint32_t dark = impl->gate_power_mask_.load(std::memory_order_acquire);
    for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
        detail::output_pipeline* p = impl->pipes_[slot].get();
        if (!p)
            continue;
        if (p->retiring) {
            impl->finish_retire(slot);
            continue;
        }
        // The previous mode's targets, once the new mode's first frame has flipped.
        if (p->graveyard && (!p->kms || (p->content_committed_once && !p->kms->flip_pending())))
            p->graveyard.reset();
        // Tested targets no switch came for (a newer change superseded the test).
        if (p->staged && !p->switch_pending &&
            detail::compositor_impl::now_ns() - p->staged_since_ns >
                detail::output_pipeline::k_staged_keep_ns)
            p->staged.reset();
    }
    // Adds held back behind a retire whose CRTC was still lit (its flip has landed now).
    impl->run_deferred_output_ops();
    // Screen capture jobs UI posted (a finished read-back is copied out right here).
    impl->capture_renderer_.take(impl->capture_channel_);
    bool reread = false;
    for (auto& up : impl->pipes_) {
        if (!up || up->retiring || !impl->drm_present_ || vt_paused || (dark & (1u << up->slot)))
            continue;
        const unsigned gen = impl->resume_gen_[up->slot].load(std::memory_order_acquire);
        if (gen == up->resume_done_gen)
            continue;
        if (!reread && !impl->card_gone_.load(std::memory_order_acquire)) {
            // Whatever lit CRTCs while we were away (fbcon on every connected connector, a
            // disabled monitor included, or on another CRTC than ours): read again, so the
            // resume modesets turn off every CRTC none of our pipelines drives (wlroots
            // does on every modeset) — else it keeps showing the console, or the kernel
            // refuses the resume for a CRTC left enabled without connectors.
            reread = true;
            if (auto again = impl->kms.enumerate_resources(); !again)
                lx::trace::logger::global().log_error(again.get_error(), "compositor.drm");
        }
        impl->run_resume_procedure(*up, gen);
    }

    impl->memory_.reset_frame_arenas(lx::runtime::memory_pool::render);
    if (impl->device_recovery_.needs_recovery()) {
        if (auto recovered = impl->device_recovery_.handle_device_lost(); recovered) {
            impl->import_cache = gfx::dmabuf_import_cache{impl->graphics.dmabuf(),
                                                          impl->cfg.import_cache};
            impl->pipeline_cache_ = gfx::pipeline_cache{};
            impl->frame_renderer_.set_pipeline_cache(&impl->pipeline_cache_);
        }
    }

    // As late as possible: the cursor position used is the newest the UI has published.
    // Taken whether or not a plane exists — its input time is what latency is measured from.
    if (impl->cursor_frames_.take())
        impl->cursor_cur_ = impl->cursor_frames_.front();
    (void)impl->cursor_images_.take();

    // The views to composite: those UI published since render last looked, and those with
    // damage a failed composite left. Oldest frame first, each composited right after the
    // texture updates its snapshot may show are applied (one GL texture serves every output).
    const std::uint32_t published = impl->render_mask_.exchange(0, std::memory_order_acq_rel);
    unsigned order[output_manager::k_max];
    unsigned n = 0;
    for (auto& up : impl->pipes_) {
        if (!up || up->retiring)
            continue;
        const std::uint32_t bit = 1u << up->slot;
        if ((published & bit) || !up->missed.empty())
            order[n++] = up->slot;
    }
    // Acquired first (each view's newest snapshot, a reader lease), then composited in frame
    // order: the texture updates drained before each composite are exactly those its
    // snapshot may show.
    const scene::immutable_frame_snapshot* snaps[output_manager::k_max]{};
    for (unsigned i = 0; i < n; ++i) {
        detail::output_pipeline& p = *impl->pipes_[order[i]];
        // Before the acquire: the snapshot taken is at least this new, so "every publish was
        // consumed" can only be over-reported (the low-water mark stays conservative).
        const unsigned publishes = scene().snapshots(p.slot).publish_count();
        snaps[p.slot] = &scene().acquire_render_snapshot(p.slot);
        p.consumed_publishes = publishes;
    }
    // Again, after the acquire: a mode switch UI posted before publishing a snapshot of the
    // new size is seen whenever that snapshot is (the publish is a release, the acquire an
    // acquire), so no frame of the new size is composited into targets of the old one.
    impl->run_output_ops();
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && static_cast<int>(snaps[order[j]]->frame_index() -
                                                       snaps[order[j - 1]]->frame_index()) < 0;
             --j) {
            const unsigned t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    const char* commit_kinds[output_manager::k_max]{};
    bool composited[output_manager::k_max]{};
    unsigned rects[output_manager::k_max]{};
    unsigned long long areas[output_manager::k_max]{};
    unsigned frames[output_manager::k_max]{};
    for (unsigned i = 0; i < n; ++i) {
        if (!impl->pipes_[order[i]]) {
            // Retired and handed over by the ops just run (its CRTC was free at once).
            scene().release_render_frame(order[i], *snaps[order[i]]);
            continue;
        }
        detail::output_pipeline& p = *impl->pipes_[order[i]];
        // Retired by the ops just run, or a switch that has to wait a tick (the previous
        // switch's targets may still be on screen): the snapshot is released undrawn and
        // its damage kept.
        // Stale for this pipeline: built for the output that had the slot before, or at the
        // mode it is switching away from — never composited (a modeset of the old mode after
        // a resize while the VT was away could be refused; it would at least show a frame
        // of the old size). The new mode's first frame follows at once.
        const unsigned snap_frame = snaps[p.slot]->frame_index();
        const bool stale =
            (p.has_first_frame && static_cast<int>(snap_frame - p.first_frame) < 0) ||
            (p.switch_pending && static_cast<int>(snap_frame - p.switch_from_frame) < 0);
        if (p.retiring || stale || !impl->apply_mode_switch(p, snap_frame)) {
            if (!p.retiring && !stale)
                p.missed.set_full();
            scene().release_render_frame(p.slot, *snaps[p.slot]);
            continue;
        }
        const auto t_out = std::chrono::steady_clock::now();
        composited[p.slot] =
            impl->composite_output(p, p.slot, *snaps[p.slot], rects[p.slot], areas[p.slot]);
        frames[p.slot] = static_cast<unsigned>(p.rendered_frame.load(std::memory_order_relaxed));
        p.render_cost_us.store(static_cast<unsigned>(
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - t_out)
                                       .count()),
                               std::memory_order_relaxed);
    }
    if (n > 0) {
        import_cache().tick(frames[order[n - 1]]);
    } else {
        // Nothing to composite (no output published): client updates are applied all the
        // same, up to the frame being built, so the staging they hold is handed back and a
        // client committing to a surface no output shows never runs out of buffers.
        impl->drain_texture_updates(impl->frame_index.load(std::memory_order_acquire));
    }

    // Commits: every pipeline with a frame waiting (or only the pointer moved), each its own
    // nonblocking commit and flip (M5) — then the pipelines that need a modeset, together.
    if (impl->drm_present_) {
        for (auto& up : impl->pipes_) {
            if (!up || !up->kms || up->retiring)
                continue;
            impl->prepare_cursor_plane(*up);
            if (up->kms->needs_modeset())
                continue;
            commit_kinds[up->slot] = impl->commit_output(*up, up->slot, composited[up->slot]);
        }
        impl->commit_pending_modesets(commit_kinds);
        for (auto& up : impl->pipes_)
            if (up && !composited[up->slot])
                impl->catch_up_slots(*up);
    }

    // Screen capture that needs no new composite, window captures, curtains (before the
    // retirements below: a capture view still draws what this tick's frames drew).
    impl->run_capture_render();

    // Nothing an output can still sample is kept: client buffers (UI retires them against
    // the mark) and textures whose retirement waited for every view to move past them.
    const long long low_water = impl->compute_low_water();
    impl->render_low_water_.store(low_water, std::memory_order_release);
    impl->apply_due_forgets(low_water);
    impl->render_quiet_at_.store(impl->render_is_quiet() ? posts_seen : 0,
                                 std::memory_order_release);

    // One line per output that did something; an idle tick is silent. Checked before
    // formatting so the frame path pays nothing when debug is off.
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        for (unsigned slot = 0; slot < output_manager::k_max; ++slot) {
            const char* kind = commit_kinds[slot] ? commit_kinds[slot] : "none";
            if (!composited[slot] && kind[0] == 'n')
                continue;
            const detail::output_pipeline* p = impl->pipes_[slot].get();
            lx::trace::logger::global().logf(
                lx::trace::level::debug, "compositor.render",
                "frame={} composite={} rects={} area={} commit={} "
                "render_us={} output={}",
                p ? p->rendered_frame.load(std::memory_order_relaxed) : -1LL,
                composited[slot] ? 1 : 0, rects[slot], areas[slot], kind,
                p ? p->render_cost_us.load(std::memory_order_relaxed) : 0u, slot);
        }
    }
}

namespace detail {

long long compositor_impl::compute_low_water() const {
    long long low = -1;
    bool any_pending = false;
    long long newest = -1;
    for (const auto& up : pipes_) {
        if (!up || up->retiring)
            continue;
        const long long rendered = up->rendered_frame.load(std::memory_order_relaxed);
        if (rendered > newest)
            newest = rendered;
        const unsigned published = const_cast<scene::scene_graph&>(scene).snapshots(up->slot).publish_count();
        const bool pending = published != up->consumed_publishes || !up->missed.empty();
        if (!pending)
            continue;
        if (!any_pending || rendered < low)
            low = rendered;
        any_pending = true;
    }
    // Window captures composite their own views (frames the lifecycle tags too): with no
    // output frame outstanding, the newest view composited is as far as anything has read.
    // A view job UI posted that render has not taken yet is held back on UI's side
    // (`capture_view_floor`).
    if (const long long view = capture_renderer_.view_frame_done(); view > newest)
        newest = view;
    return any_pending ? low : newest;
}

bool compositor_impl::composite_output(output_pipeline& p, unsigned slot,
                                       const scene::immutable_frame_snapshot& snapshot,
                                       unsigned& rects, unsigned long long& area) {
    rects = 0;
    area = 0;
    const unsigned idx = snapshot.frame_index();
    const auto& draws = snapshot.draws();
    {
        lx::trace::scoped_span drain_span{"render", "drain_textures"};
        drain_texture_updates(idx);
    }

    // What changed since the last frame this output saw. A re-acquired snapshot (the UI
    // published nothing new) contributes nothing; a skipped one was folded into this one
    // by `snapshot_buffer::publish`.
    gfx::damage_region frame_damage = p.missed;
    if (lx::trace::timeline_enabled()) {
        const lx::rect2i bounds = snapshot.damage();
        lx::trace::timeline_counter("damage_px",
                                    static_cast<long long>(bounds.width) * bounds.height);
    }
    p.missed.clear();
    if (idx != p.last_seen_frame) {
        frame_damage.add_region(snapshot.damage_rects());
        p.last_seen_frame = idx;
    }

    const bool vt_paused = (display_gate_.load(std::memory_order_acquire) & gate_vt) != 0;
    const bool dark = (gate_power_mask_.load(std::memory_order_acquire) & (1u << slot)) != 0;
    if ((vt_paused || dark) && p.kms) {
        // Client buffers keep being applied and released (a client committing once a second
        // never runs out), so the scene is current at resume — but nothing is composited or
        // committed: master is gone, or the output is off. The damage is dropped; the
        // resume repaints everything.
        p.rendered_frame.store(static_cast<long long>(idx), std::memory_order_release);
        scene.release_render_frame(slot, snapshot);
        return false;
    }

    // Translated only when something is drawn: an idle tick touches no draw data at all.
    const unsigned blit_count = draws.size();
    const auto build_blits = [&]() -> lx::gfx::blit_command* {
        lx::gfx::blit_command* out = blit_storage(blit_count);
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
            out[i].solid = cmd.solid;
            out[i].solid_color = cmd.solid_color;
        }
        return out;
    };

    bool drawn = false;
    if (p.scanout())
        frame_damage.clip(p.scanout_rect);
    if (p.scanout() && !frame_damage.empty()) {
        // Buffer age: the slot about to be drawn into also missed every frame composited
        // since it was last used, so its region is this frame's damage plus that.
        const unsigned back = p.back_slot();
        gfx::damage_region region = frame_damage;
        region.add_region(p.slot_age[back]);
        region.clip(p.scanout_rect);
        lx::gfx::blit_command* blits = build_blits();
        lx::trace::scoped_span composite_span{"render", "composite"};
        if (gl_present_) {
            auto c = gl_compositor_.composite_region(p.gl[back], snapshot.clear_color(), blits,
                                                     blit_count, region);
            if (!c) {
                lx::trace::logger::global().log_error(c.get_error(), "compositor.render");
            } else {
                drawn = true;
                // This frame supersedes any uncommitted one, so its fence replaces that
                // frame's — which is dropped rather than leaked.
                p.discard_fence();
                p.composited_fence = c.value().out_fence_fd;
                publish_release_fence(p.composited_fence, idx);
                if (c.value().draws_skipped > 0)
                    report_unregistered_draw(blits, blit_count);
                // A requested debug screenshot of a fully repainted frame of this output.
                const std::uint32_t bit = 1u << slot;
                if ((screenshot_pending_.load(std::memory_order_relaxed) & bit) &&
                    region.count == 1 && region.rects[0].width == p.scanout_rect.width &&
                    region.rects[0].height == p.scanout_rect.height &&
                    (screenshot_pending_.fetch_and(~bit, std::memory_order_acq_rel) & bit))
                    write_screenshot(p, back);
            }
        } else if (cpu_present_) {
            const auto& fb = p.dumb[back];
            gfx::pixel_surface target{};
            target.pixels = fb.pixels();
            target.width = fb.width();
            target.height = fb.height();
            target.stride = fb.stride();
            target.format = fb.format();
            target.write_combining = true;
            auto c = cpu_compositor_.composite_region(target, snapshot.clear_color(), blits,
                                                      blit_count, region);
            if (!c) {
                lx::trace::logger::global().log_error(c.get_error(), "compositor.render");
            } else {
                drawn = true;
                const auto stats = c.value();
                if (stats.draws_skipped > 0)
                    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                    "draw referenced an unregistered texture");
                if (stats.draws_unsupported > 0)
                    lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                                    "draw format has no conversion to the "
                                                    "scanout format — surface not shown");
            }
        } else if (vulkan_present_) {
            // Vulkan repaints the whole target, which is correct for any buffer age.
            auto c = vk_compositor_.composite(p.vk[back], snapshot.clear_color(), blits, blit_count);
            if (!c) {
                lx::trace::logger::global().log_error(c.get_error(), "compositor.render");
            } else {
                drawn = true;
                p.discard_fence();
                p.composited_fence = c.value().out_fence_fd;
                publish_release_fence(p.composited_fence, idx);
                if (c.value().draws_skipped > 0)
                    report_unregistered_draw(blits, blit_count);
            }
        }
        if (drawn) {
            for (unsigned i = 0; i < p.slot_count; ++i)
                if (i != back)
                    p.slot_age[i].add_region(frame_damage);
            p.slot_age[back].clear();
            p.last_composite_ns = now_ns();
            p.kms_pending.add_region(frame_damage);
            p.frame_outstanding.store(p.kms != nullptr, std::memory_order_release);
            // Newest wins: a frame that could not be shown yet is superseded by this one
            // rather than queued behind it.
            p.composited_slot = static_cast<int>(back);
            p.composited_frame = idx;
            rects = region.count;
            area = region.area();
            // Screen capture (CC1): the frame just composited is the one copied — sessions of
            // this output waiting for new content run on it now, with the damage it brought.
            p.capture_front = static_cast<int>(back);
            p.capture_front_frame = idx;
            p.capture_front_ns = static_cast<unsigned long long>(now_ns());
            if (capture_channel_.sessions.load(std::memory_order_acquire) != 0) {
                capture_frame_source src{};
                src.frame = idx;
                src.time_ns = p.capture_front_ns;
                src.size = p.mode;
                src.blits = blits;
                src.blit_count = blit_count;
                src.clear = snapshot.clear_color();
                if (gl_present_)
                    src.gl_framebuffer = p.gl[back].framebuffer();
                else if (vulkan_present_)
                    src.vk_target = &p.vk[back];
                src.cursor = capture_cursor_for(p);
                capture_renderer_.output_composited(slot, frame_damage, src, capture_channel_);
            }
        } else {
            // Not drawn: try again next tick rather than lose it.
            p.missed.add_region(frame_damage);
        }
    } else if (!p.scanout() && cpu_present_ && !p.kms) {
        // A virtual output on the CPU path: nothing is shown, so nothing is composited here —
        // a screen capture composites this frame's draws into its own buffer.
        if (!frame_damage.empty() || p.capture_front < 0) {
            p.capture_front = 0;
            p.capture_front_frame = idx;
            p.capture_front_ns = static_cast<unsigned long long>(now_ns());
            if (capture_channel_.sessions.load(std::memory_order_acquire) != 0) {
                capture_frame_source src{};
                src.frame = idx;
                src.time_ns = p.capture_front_ns;
                src.size = p.mode;
                src.blits = build_blits();
                src.blit_count = blit_count;
                src.clear = snapshot.clear_color();
                src.cursor = capture_cursor_for(p);
                gfx::damage_region d = frame_damage;
                d.clip({0, 0, p.mode.width, p.mode.height});
                capture_renderer_.output_composited(slot, d, src, capture_channel_);
            }
        }
    } else if (!p.scanout() && headless_present_ && headless_.width() > 0) {
        // Software fallback for no-GPU hosts and CI.
        (void)headless_.begin_frame();
        (void)headless_.clear(lx::color::rgb(0.f, 0.f, 0.f));
        if (blit_count > 0)
            (void)frame_renderer_.draw_to_headless(build_blits(), blit_count, headless_);
        (void)headless_.end_frame_and_dump(nullptr);
    }

    // The composite above waits for GPU completion (or carries its fence to the commit), so
    // the client buffers this snapshot sampled are done with from render's side.
    p.rendered_frame.store(static_cast<long long>(idx), std::memory_order_release);
    scene.release_render_frame(slot, snapshot);
    return drawn;
}

bool compositor_impl::catch_up_owed(const output_pipeline& p, unsigned& front) const {
    if (!gl_present_ || catch_up_failed_ || !gl_compositor_.supports_target_copy() ||
        p.slot_count < 2 || !p.kms || p.retiring || p.graveyard || p.staged ||
        p.switch_pending || p.composited_slot >= 0 || !p.missed.empty() ||
        !p.content_committed_once || p.kms->flip_pending() || p.kms->needs_modeset())
        return false;
    if (display_gate_.load(std::memory_order_acquire) != 0 ||
        (gate_power_mask_.load(std::memory_order_acquire) & (1u << p.slot)) != 0)
        return false;
    // Nothing is waiting or queued, so the slot on screen holds the newest composite — the
    // frame every other slot's age is measured against. Its own age is then empty; anything
    // else means it is not that frame (its flip is drained but not noted yet).
    front = p.front.load(std::memory_order_acquire);
    if (front >= p.slot_count || !p.slot_age[front].empty())
        return false;
    const unsigned long long output_area =
        static_cast<unsigned long long>(p.scanout_rect.width) * p.scanout_rect.height;
    for (unsigned i = 0; i < p.slot_count; ++i) {
        if (i == front || p.slot_age[i].empty())
            continue;
        gfx::damage_region region = p.slot_age[i];
        region.clip(p.scanout_rect);
        // A small age is cheaper to repaint with the frame that next uses the slot.
        if ((region.full ? output_area : region.area()) * 4 >= output_area)
            return true;
    }
    return false;
}

void compositor_impl::catch_up_slots(output_pipeline& p) {
    unsigned front = 0;
    if (!catch_up_owed(p, front) || now_ns() - p.last_composite_ns < k_catch_up_idle_ns)
        return;
    const unsigned long long output_area =
        static_cast<unsigned long long>(p.scanout_rect.width) * p.scanout_rect.height;
    for (unsigned i = 0; i < p.slot_count; ++i) {
        if (i == front || p.slot_age[i].empty())
            continue;
        gfx::damage_region region = p.slot_age[i];
        region.clip(p.scanout_rect);
        const unsigned long long area = region.full ? output_area : region.area();
        if (area * 4 < output_area)
            continue;
        lx::trace::scoped_span span{"render", "catch_up_slot"};
        if (auto ok = gl_compositor_.copy_region(p.gl[front], p.gl[i], region); !ok) {
            lx::trace::logger::global().log_error(ok.get_error(), "compositor.render");
            catch_up_failed_ = true;
            return;
        }
        p.slot_age[i].clear();
        if (lx::trace::logger::global().min_level() <= lx::trace::level::debug)
            lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.render",
                                             "slot catch-up output={} slot={} from={} area={}",
                                             p.slot, i, front, area);
    }
}

const char* compositor_impl::commit_output(output_pipeline& p, unsigned slot, bool composited) {
    const unsigned bit = 1u << slot;
    const bool gated = display_gate_.load(std::memory_order_acquire) != 0 ||
                       (gate_power_mask_.load(std::memory_order_acquire) & bit) != 0;
    const bool flip_in_flight = p.kms->flip_pending();
    if (p.composited_slot >= 0 && flip_in_flight)
        return "deferred";
    if (p.composited_slot >= 0) {
        const auto present_slot = static_cast<unsigned>(p.composited_slot);
        p.presentation.on_present_submitted(p.composited_frame, now_ns());

        drm::atomic_commit_request req{};
        req.request_page_flip = true;
        // The flip event names the frame it shows (presentation feedback).
        req.user_tag = p.composited_frame;
        // Borrowed by the commit; closed below once it has returned. When the GL backend
        // produced no fence it already blocked, so an absent fence is still correct.
        req.in_fence_fd = p.composited_fence;
        req.framebuffer_id = p.framebuffer(present_slot, cpu_present_);

        // Every rect since the last commit, as separate clips: the plane's previous
        // framebuffer is older than this frame's damage alone describes.
        const gfx::damage_region& pending = p.kms_pending;
        if (pending.full || pending.count == 0) {
            req.damage.full_frame = true;
        } else {
            req.damage.full_frame = false;
            req.damage.count = pending.count;
            for (unsigned i = 0; i < pending.count; ++i)
                req.damage.rects[i] = pending.rects[i];
            p.kms->build_damage_blob(req.damage);
        }

        // The pointer rides the frame: both change on the same vblank.
        bool cursor_rode = false;
        if (p.cursor.valid() && p.content_committed_once && p.cursor.dirty()) {
            req.cursor = &p.cursor;
            cursor_rode = true;
        }

        p.pending_flip_slot.store(present_slot, std::memory_order_release);
        p.last_commit_content.store(true, std::memory_order_release);
        publish_commit_timing(p, p.composited_fence);
        lx::trace::scoped_span commit_span{"render", "kms.commit"};
        // Checked again right before the ioctl: a pause that landed during the composite
        // keeps the frame (resume discards it) and commits nothing.
        const bool paused_now =
            display_gate_.load(std::memory_order_acquire) != 0 ||
            (gate_power_mask_.load(std::memory_order_acquire) & bit) != 0;
        const auto committed =
            paused_now
                ? lx::result<void>{lx::make_error(lx::drm_err::page_flip_failed, "session paused")}
                : p.kms->commit(req);
        // A commit that raced the pause is expected, not a failure of the present path:
        // logind drops master *before* it signals, so EACCES usually arrives first and the
        // pause a moment later. Master lost with no pause following is reported below.
        const bool lost_to_pause =
            !committed && (paused_now || display_gate_.load(std::memory_order_acquire) != 0 ||
                           p.kms->last_failure_lost_master());
        if (!committed && lost_to_pause && !paused_now) {
            const auto now = now_ns();
            if (p.master_lost_since_ns == 0)
                p.master_lost_since_ns = now;
            else if (now - p.master_lost_since_ns > 1'000'000'000 && !p.master_lost_reported) {
                p.master_lost_reported = true;
                lx::trace::logger::global().log_error(committed.get_error(), "compositor.drm");
                lx::trace::logger::global().log(lx::trace::level::warn, "compositor.drm",
                                                "DRM master lost and the session did not pause "
                                                "(no logind?) — retrying every frame");
            }
        } else if (committed) {
            p.master_lost_since_ns = 0;
            p.master_lost_reported = false;
        }
        if (!committed)
            retract_commit_timing(p);
        if (!committed && cursor_rode && !lost_to_pause) {
            // Do not let the plane take frames down with it: if the cursor state alone is
            // rejected, the plane is unusable and the frame retries without it.
            drm::atomic_commit_request test{};
            test.update_primary = false;
            test.cursor = &p.cursor;
            test.request_page_flip = false;
            test.test_only = true;
            if (!p.kms->commit(test) && p.kms->last_failure_rejected_state()) {
                fail_cursor_plane("commit rejected the cursor state");
                p.cursor.set_visible(false);
            }
        }
        const char* kind = "failed";
        if (committed) {
            if (p.kms->completion_needs_dispatch())
                post_flip_dispatch();
            p.content_committed_once = true;
            p.ever_lit.store(true, std::memory_order_release);
            p.composited_slot = -1;
            // The kernel has taken its own reference to the fence.
            p.discard_fence();
            p.kms_pending.clear();
            kind = "content";
            note_committed_input(p, true);
        } else {
            // Keep the slot and its fence: the commit is retried next tick without
            // compositing the frame again (or, paused, discarded by the resume).
            kind = lost_to_pause ? "paused" : "failed";
        }
        // Lowered after the commit raised flip_pending (see the idle-callback check).
        p.frame_outstanding.store(!committed, std::memory_order_release);
        // Log on transition only: a failing commit every frame must not flood the hot
        // path, but silently discarding it is how a dead present path stayed invisible.
        if (!committed && lost_to_pause) {
            // Expected; nothing to report.
        } else if (!committed && !p.drm_commit_failing) {
            p.drm_commit_failing = true;
            lx::trace::logger::global().log_error(committed.get_error(), "compositor.drm");
        } else if (committed && p.drm_commit_failing) {
            p.drm_commit_failing = false;
            lx::trace::logger::global().log(lx::trace::level::info, "compositor.drm",
                                            "atomic commit recovered");
        }
        (void)composited;
        return kind;
    }
    if (gated || flip_in_flight || !p.content_committed_once || !p.cursor.valid() ||
        !p.cursor.dirty() || !p.cursor.involves_crtc())
        return "none";
    // Nothing to composite, but the pointer moved on this output: a commit of the cursor
    // plane alone. The scene is not touched — this is what keeps pointer motion off the GPU.
    drm::atomic_commit_request req{};
    req.update_primary = false;
    req.cursor = &p.cursor;
    req.request_page_flip = true;
    // Its flip re-arms the frame clock but presents no client content, and must not rotate
    // the scanout slots.
    p.pending_flip_slot.store(p.front.load(std::memory_order_acquire), std::memory_order_release);
    p.last_commit_content.store(false, std::memory_order_release);
    publish_commit_timing(p, -1);
    if (auto committed = p.kms->commit(req); committed) {
        note_committed_input(p, false);
        return "cursor";
    } else if (display_gate_.load(std::memory_order_acquire) != 0) {
        retract_commit_timing(p);
        return "paused";
    } else {
        retract_commit_timing(p);
        // EBUSY (a flip raced in) and EACCES (DRM master on another VT) are transient;
        // only a state the plane itself refuses, confirmed by TEST_ONLY, disables it.
        drm::atomic_commit_request test{};
        test.update_primary = false;
        test.cursor = &p.cursor;
        test.request_page_flip = false;
        test.test_only = true;
        if (!p.kms->commit(test) && p.kms->last_failure_rejected_state()) {
            lx::trace::logger::global().log_error(committed.get_error(), "compositor.cursor");
            fail_cursor_plane("cursor-only commit rejected");
            p.cursor.set_visible(false);
        }
        return "cursor-failed";
    }
}

void compositor_impl::commit_pending_modesets(const char** kinds) {
    // The pipelines that need a modeset (start, VT resume, power-on): one ALLOW_MODESET for
    // all of them, carrying each one's first frame (M5) — once every one has a frame, or
    // after `k_modeset_wait_ns` with those that do.
    if ((display_gate_.load(std::memory_order_acquire) & gate_vt) != 0)
        return;
    const std::uint32_t dark = gate_power_mask_.load(std::memory_order_acquire);
    output_pipeline* group[output_manager::k_max];
    unsigned want = 0;
    unsigned ready = 0;
    const auto now = now_ns();
    bool waited_out = false;
    for (auto& up : pipes_) {
        if (!up || !up->kms || up->retiring || !up->kms->needs_modeset() ||
            (dark & (1u << up->slot)) || up->kms->flip_pending())
            continue;
        ++want;
        if (up->modeset_wait_since_ns == 0)
            up->modeset_wait_since_ns = now;
        if (now - up->modeset_wait_since_ns >= k_modeset_wait_ns)
            waited_out = true;
        if (up->composited_slot >= 0)
            group[ready++] = up.get();
    }
    if (ready == 0 || (ready < want && !waited_out))
        return;

    drm::modeset_entry entries[output_manager::k_max]{};
    for (unsigned i = 0; i < ready; ++i) {
        output_pipeline& p = *group[i];
        entries[i].pipe = p.kms.get();
        entries[i].framebuffer_id = p.framebuffer(static_cast<unsigned>(p.composited_slot), cpu_present_);
        entries[i].in_fence_fd = p.composited_fence;
        entries[i].user_tag = p.composited_frame;
        // On a resume the plane passed its test before and is stated where the pointer is;
        // at start it is stated off and joins the next commit once tested.
        entries[i].cursor = p.cursor.valid() && (p.cursor_tested || !p.cursor.visible())
                                ? &p.cursor
                                : nullptr;
        p.pending_flip_slot.store(static_cast<unsigned>(p.composited_slot), std::memory_order_release);
        p.last_commit_content.store(true, std::memory_order_release);
        publish_commit_timing(p, p.composited_fence);
    }
    lx::trace::scoped_span commit_span{"render", "kms.modeset"};
    auto committed = drm::commit_modeset(kms, entries, ready, false);
    bool fallback = false;
    if (!committed && group[0]->kms->last_failure_rejected_state()) {
        // The combined state was refused: each output on its own, so one output the driver
        // cannot drive does not keep the others dark.
        lx::trace::logger::global().log_error(committed.get_error(), "compositor.drm");
        if (ready > 1)
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.drm",
                                            "combined modeset refused — modesetting each output "
                                            "on its own");
        fallback = true;
    }
    for (unsigned i = 0; i < ready; ++i) {
        output_pipeline& p = *group[i];
        lx::result<void> r = committed;
        if (fallback) {
            drm::atomic_commit_request req{};
            req.request_page_flip = true;
            req.user_tag = p.composited_frame;
            req.in_fence_fd = p.composited_fence;
            req.framebuffer_id = entries[i].framebuffer_id;
            req.cursor = entries[i].cursor;
            r = p.kms->commit(req);
        }
        const bool lost = !r && (p.kms->last_failure_lost_master() ||
                                 display_gate_.load(std::memory_order_acquire) != 0);
        if (r) {
            if (p.kms->completion_needs_dispatch())
                post_flip_dispatch();
            p.content_committed_once = true;
            p.ever_lit.store(true, std::memory_order_release);
            p.composited_slot = -1;
            p.discard_fence();
            p.kms_pending.clear();
            p.frame_outstanding.store(false, std::memory_order_release);
            p.modeset_wait_since_ns = 0;
            note_committed_input(p, true);
            // A content commit that is also the modeset (the frame rides it).
            kinds[p.slot] = "content";
            if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
                lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.drm",
                                                 "modeset output={} frame={} outputs={}{}", p.slot,
                                                 p.composited_frame, fallback ? 1u : ready,
                                                 fallback ? " (alone)" : "");
            }
        } else {
            kinds[p.slot] = lost ? "paused" : "failed";
            retract_commit_timing(p);
            if (!lost && !p.drm_commit_failing) {
                p.drm_commit_failing = true;
                lx::trace::logger::global().log_error(r.get_error(), "compositor.drm");
            }
        }
        if (p.apply_ticket) {
            // An output configuration waits for this modeset. Lost to a pause is not a
            // refusal: the resume modesets the same state.
            compositor_impl* self = this;
            const unsigned slot = p.slot;
            const unsigned ticket = p.apply_ticket;
            const bool ok = static_cast<bool>(r) || lost;
            p.apply_ticket = 0;
            (void)runtime::executor::global().post(runtime::affinity::ui, [self, slot, ticket, ok] {
                self->on_modeset_result(slot, ticket, ok);
            });
        }
        if (p.resume_result_pending && !(display_gate_.load(std::memory_order_acquire) != 0)) {
            p.resume_result_pending = false;
            post_resume_result(p.slot, p.resume_done_gen,
                               r ? session_resume_outcome::ok
                               : !lost && p.kms->last_failure_rejected_state()
                                   ? session_resume_outcome::rejected
                                   : session_resume_outcome::device_unavailable);
        }
    }
}

} // namespace detail

unsigned compositor::frame_index() const {
    return impl_ ? impl_->frame_index.load(std::memory_order_acquire) : 0;
}

bool compositor::is_running() const {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

} // namespace lx::compositor
