module;

#include <atomic>

import lx.foundation;
import lx.runtime;

export module lx.drm:atomic;

export namespace lx::drm {

class kms_device;
class kms_cursor_plane;

/// Outputs (CRTC pipelines) one device drives at most; also the width of every mask over
/// pipelines.
inline constexpr unsigned k_max_pipelines = 16;

struct kms_damage_region {
    lx::rect2i rects[16]{};
    unsigned count = 0;
    bool full_frame = true;
};

/// A `drmModeModeInfo`, kept as its 68 uapi bytes so libdrm stays out of interfaces.
struct drm_mode_blob {
    unsigned char bytes[68]{};
};

/// One mode of a connector, as probed (`kms_device::probe_connectors`).
struct kms_mode {
    unsigned width = 0;
    unsigned height = 0;
    /// mHz, from the timings (not the kernel's rounded `vrefresh`): 59.94 Hz is 59940.
    unsigned refresh_mhz = 0;
    /// DRM_MODE_TYPE_PREFERRED.
    bool preferred = false;
    /// DRM_MODE_FLAG_* (interlace, doublescan, …).
    unsigned flags = 0;
    drm_mode_blob kms{};

    [[nodiscard]] bool valid() const { return width > 0 && height > 0; }
    /// Same timings (what a modeset would program), whatever the type bits say.
    [[nodiscard]] bool same_timings(const kms_mode& o) const;
};

/// A KMS property as `neutral_property_value` sees it (libdrm stays out of the interface).
struct kms_property_shape {
    enum class kind : unsigned char { range, signed_range, enumeration, bitmask, blob, object };
    kind type = kind::range;
    /// DRM_MODE_PROP_IMMUTABLE: the driver's, never settable.
    bool immutable = false;
    /// Range bounds; a signed range's as two's complement.
    unsigned long long min = 0;
    unsigned long long max = 0;
    /// Enum / bitmask entries (a bitmask entry's value is its bit index).
    const char* const* enum_names = nullptr;
    const unsigned long long* enum_values = nullptr;
    unsigned enum_count = 0;
};

/// How a property another DRM master may have left changed is put back to the state the
/// driver starts with.
enum class neutral_rule : unsigned char {
    /// 0: no blob (LUT, CTM, HDR metadata), off (VRR).
    zero,
    range_min,
    range_max,
    /// The named entry: an enum's value, a bitmask's bit (rotate-0 → 1 << 0).
    enum_entry,
};

/// The value that resets `p` under `rule` (`enum_name` for `enum_entry`). False when it
/// cannot be set — immutable, the wrong kind for the rule, or no such entry — and is then
/// left out of the commit.
[[nodiscard]] bool neutral_property_value(const kms_property_shape& p, neutral_rule rule,
                                          const char* enum_name, unsigned long long& out);

struct atomic_commit_request {
    unsigned framebuffer_id = 0;
    kms_damage_region damage{};
    bool request_page_flip = true;
    bool async = false;
    /// Optional explicit-sync input fence (sync_file FD). Borrowed: the caller keeps
    /// ownership and must close it after the commit returns. The modeset carries it too.
    int in_fence_fd = -1;
    /// When set, receives an owned sync_file FD for the commit completion fence, or -1.
    /// The caller must close it.
    int* out_fence_fd_ptr = nullptr;
    /// Borrowed. When set and `dirty()`, the cursor plane's state rides this commit — so the
    /// pointer and the frame under it reach the screen in the same vblank. A modeset states
    /// it in full whenever it is set (after `kms_cursor_plane::invalidate`).
    kms_cursor_plane* cursor = nullptr;
    /// False for a cursor-only commit: the primary plane is left exactly as it is (no FB,
    /// damage or fences), and the commit fails rather than performing the first modeset.
    bool update_primary = true;
    /// DRM_MODE_ATOMIC_TEST_ONLY: validate, apply nothing, raise no event.
    bool test_only = false;
    /// The caller's name for what this commit shows (the compositor: the frame index of the
    /// composite it scans out; 0 = none). Echoed in the `page_flip_event` of this commit —
    /// a CRTC takes one flip at a time, so the event of the in-flight commit is unambiguous.
    unsigned user_tag = 0;
};

struct page_flip_event {
    /// The CRTC's vblank counter at the flip, as the kernel reports it (MSC); 0 for a
    /// blocking modeset's synthetic completion.
    unsigned sequence = 0;
    /// CLOCK_MONOTONIC (`DRM_CAP_TIMESTAMP_MONOTONIC`): the kernel's flip timestamp, or the
    /// time the blocking modeset returned.
    lx::runtime::clock_time timestamp_ns = 0;
    kms_damage_region applied_damage{};
    bool presented = false;
    /// The kernel's flip event: the driver signalled the flip's completion. False for a
    /// blocking modeset's completion, whose time is sampled here in software.
    bool hw = false;
    /// `timestamp_ns` and `sequence` are the CRTC's vblank time and counter. False when the
    /// driver has no vblank support (vmwgfx): its events carry sequence 0 and the time the
    /// event was sent.
    bool vblank_timestamp = false;
    /// The commit carried the primary plane (a frame), not only the cursor plane.
    bool content = false;
    /// `atomic_commit_request::user_tag` of the commit this event completes.
    unsigned tag = 0;
    /// The CRTC the event came from.
    unsigned crtc_id = 0;
};

using page_flip_handler = void (*)(page_flip_event event, void* user_data);

/// What a pipeline drives: one CRTC, the primary plane scanning out on it and the connector
/// it is routed to. Chosen by `assign_crtcs` (`:crtc_alloc`).
struct kms_pipeline_desc {
    unsigned crtc_id = 0;
    /// Index of the CRTC in the resource list: the bit `possible_crtcs` masks use.
    unsigned crtc_index = 0;
    unsigned primary_plane_id = 0;
    unsigned connector_id = 0;
};

class kms_pipeline;
struct modeset_entry;
[[nodiscard]] lx::result<void> commit_modeset(kms_device& device, const modeset_entry* entries,
                                              unsigned count, bool test_only);

/// `drmModeAtomicCommit` user data. One shape for a single pipeline's commit and for a
/// combined modeset: the pipelines whose event it still owes (registry bits) and each one's
/// flip generation when it was issued. The device routes every event by its CRTC; an event
/// whose pipeline's generation moved since (a resume disowned it) is dropped.
///
/// The bits are per pipeline, armed by the commit and consumed by that pipeline's event: a
/// cookie shared by several commits in flight at once (the device's combined cookies, when
/// separate modesets are issued within one flip latency) never loses an earlier commit's
/// event to a later one. A pipeline has at most one commit in flight, so its bit and
/// generation are never armed twice before its event consumes them.
struct flip_cookie {
    kms_device* device = nullptr;
    std::atomic<unsigned> mask{0};
    std::atomic<unsigned> generation[k_max_pipelines]{};

    /// Committing thread, before the ioctl (the event can be drained before it returns).
    void arm(unsigned slot, unsigned flip_generation) {
        if (slot >= k_max_pipelines)
            return;
        generation[slot].store(flip_generation, std::memory_order_release);
        mask.fetch_or(1u << slot, std::memory_order_acq_rel);
    }
    /// A commit that raised no event after all (refused, or retried blocking without one).
    void disarm(unsigned slots) { mask.fetch_and(~slots, std::memory_order_acq_rel); }
    /// Event-draining thread: the event for pipeline `slot`, now at `flip_generation`.
    /// True when the cookie owed it one from that generation; the bit is consumed either way.
    [[nodiscard]] bool take(unsigned slot, unsigned flip_generation) {
        if (slot >= k_max_pipelines)
            return false;
        const unsigned bit = 1u << slot;
        if (!(mask.fetch_and(~bit, std::memory_order_acq_rel) & bit))
            return false;
        return generation[slot].load(std::memory_order_acquire) == flip_generation;
    }
};

/// One CRTC pipeline: the per-output half of KMS (wlroots' `wlr_drm_connector` + its CRTC,
/// KWin's `DrmPipeline`). Frames are committed per pipeline — one nonblocking atomic commit
/// and one flip event per CRTC, so outputs flip independently (ADR 008 M5); configuration
/// changes of several pipelines go through `commit_modeset` together.
///
/// Render affinity for commits; the flip handler runs on the thread that calls
/// `kms_device::dispatch_events` (UI).
class kms_pipeline {
public:
    /// `route_events` false: the pipeline is never entered in the device's registry, so no
    /// flip event reaches it and no other thread can see it — a candidate checked by
    /// TEST_ONLY on the committing thread (`commit_modeset` with `test_only`) and dropped.
    kms_pipeline(kms_device& device, const kms_pipeline_desc& desc, bool route_events = true);
    /// Puts the CRTC back the way the console left it, so exiting on a TTY does not leave
    /// a dead display behind, and leaves the device's registry.
    ~kms_pipeline();

    kms_pipeline(const kms_pipeline&) = delete;
    kms_pipeline& operator=(const kms_pipeline&) = delete;

    [[nodiscard]] const kms_pipeline_desc& desc() const { return desc_; }
    [[nodiscard]] unsigned crtc_id() const { return desc_.crtc_id; }
    [[nodiscard]] unsigned connector_id() const { return desc_.connector_id; }

    /// The mode the next modeset programs. Taking a new one makes the next content commit a
    /// modeset again (`needs_modeset`). The framebuffer must be sized to it.
    void set_mode(const kms_mode& mode);
    [[nodiscard]] const kms_mode& mode() const { return mode_; }
    /// The next content commit is a full modeset: never modeset, `set_mode`, or
    /// `prepare_resume` since.
    [[nodiscard]] bool needs_modeset() const { return !modeset_attempted_; }
    /// Adaptive sync (VRR_ENABLED on the CRTC, ADR 008 §C.4): stated by every modeset of this
    /// pipeline (off unless set; the neutral reset leaves another master's VRR behind). A
    /// change makes the next content commit a modeset. Only meaningful on a connector with
    /// `vrr_capable`; the CRTC must have the property.
    void set_vrr(bool on) {
        if (vrr_ != on) {
            vrr_ = on;
            modeset_attempted_ = false;
        }
    }
    [[nodiscard]] bool vrr() const { return vrr_; }

    /// Committing thread (render). The first commit — and the first after
    /// `prepare_resume` or `set_mode` — is a full modeset of this pipeline alone (see
    /// `commit_modeset`); several pipelines that all need one are better committed together
    /// through `commit_modeset`. Nonblocking, with a flip event; the flip handler never runs
    /// on the committing thread: a failure is reported by the return value only.
    [[nodiscard]] lx::result<void> commit(const atomic_commit_request& request);
    void set_page_flip_handler(page_flip_handler handler, void* user_data = nullptr);
    /// The last successful commit completed synchronously (a blocking modeset), so no
    /// event will arrive on the card fd: the committing thread must have the event-draining
    /// thread call `kms_device::dispatch_events`, which reports it as the flip.
    [[nodiscard]] bool completion_needs_dispatch() const {
        return synthetic_flip_.load(std::memory_order_acquire);
    }
    /// Committing thread, before the first commit after the session regained the device
    /// (VT switch back) or the CRTC was powered off: the KMS state is whatever the last
    /// master left, so the next content commit is a full modeset again. Flip events still in
    /// flight are disowned (their generation is stale), `flip_pending` is cleared. The CRTC
    /// state saved at the first modeset (the console's, restored at exit) is kept.
    void prepare_resume();
    /// Committing thread. Display power off (zwlr_output_power_v1 off): the kernel's atomic
    /// "DPMS off" — CRTC ACTIVE=0 with MODE_ID and the connector routing kept, and `cursor`
    /// (when given) taken off its CRTC. One **blocking** ALLOW_MODESET commit with no event.
    /// A driver that refuses it (EINVAL) is asked once more with the primary plane detached
    /// too. Requires a CRTC this object has modeset before (mode_invalid otherwise). Power
    /// comes back on through `prepare_resume`. Fails with EBUSY semantics while a flip is
    /// pending — check `flip_pending` first.
    [[nodiscard]] lx::result<void> commit_power_off(kms_cursor_plane* cursor);

    /// The console state saved at the first modeset is not put back when this object goes:
    /// its output was unplugged or disabled at runtime, so there is nothing to give back
    /// (and lighting a monitor the session turned off would be wrong).
    void forget_saved_state();

    [[nodiscard]] bool supports_atomic_damage() const;
    /// Clips are limited to the pipeline's mode, which is what the scanout framebuffer is
    /// sized to; the kernel fails the whole commit for a clip past it.
    void build_damage_blob(const kms_damage_region& region);
    void set_framebuffer(unsigned fb_id);

    /// True while a flip requested by `commit()` has not yet been reported by
    /// `kms_device::dispatch_events()`. A CRTC takes one flip at a time, so committing again
    /// before then is rejected with EBUSY. Set on the committing thread, cleared on the
    /// thread that drains events.
    [[nodiscard]] bool flip_pending() const;

    /// True when the last failed `commit()` was the kernel refusing the requested state
    /// (EINVAL, ERANGE, ENOSPC). False when the device was unavailable instead — EACCES
    /// after DRM master went to another VT, EBUSY, EINTR, ENOMEM — which says nothing
    /// about the state asked for. Committing thread only.
    [[nodiscard]] bool last_failure_rejected_state() const { return last_rejected_state_; }
    /// The last failed `commit()` found DRM master gone (EACCES/EPERM): another session
    /// is in front. logind drops master before it signals the pause, so a commit can lose
    /// this race. Committing thread only.
    [[nodiscard]] bool last_failure_lost_master() const { return last_lost_master_; }
    /// Registry slot in the device (0..15), the bit of masks over pipelines.
    [[nodiscard]] unsigned registry_slot() const { return reg_slot_; }
    /// Leaves the device's registry now (the committing thread, when its output is retired):
    /// no event is routed to it any more and `commit_modeset` no longer counts its CRTC as
    /// owned. The thread draining events may still hold it until its current dispatch
    /// returns, so the object itself is destroyed there (ADR 008 §B.3). Idempotent.
    void leave_registry();

private:
    friend class kms_device;
    friend lx::result<void> commit_modeset(kms_device& device, const modeset_entry* entries,
                                           unsigned count, bool test_only);

    [[nodiscard]] lx::result<void> ensure_props();
    /// Looks up the modeset-only properties (CRTC MODE_ID/ACTIVE/VRR_ENABLED, connector
    /// CRTC_ID) and the neutral settings, once.
    [[nodiscard]] bool ensure_modeset_props(int fd);
    /// Reports one flip to the handler (event-draining thread only). `hw`: a kernel event
    /// (its sequence and CRTC are real), else a blocking modeset's synthetic completion.
    void emit_flip(lx::runtime::clock_time timestamp_ns, unsigned sequence, bool hw);
    /// Records what the commit about to raise the flip shows (committing thread, before the
    /// ioctl — the event can be drained before the ioctl returns).
    void note_inflight(const atomic_commit_request& request) {
        inflight_tag_.store(request.user_tag, std::memory_order_relaxed);
        inflight_content_.store(request.update_primary, std::memory_order_release);
    }
    /// A kernel event for this pipeline (event-draining thread).
    void on_kernel_flip(flip_cookie* cookie, unsigned sequence,
                        lx::runtime::clock_time timestamp_ns);
    /// A blocking modeset of this pipeline completed: reported at the next dispatch.
    void sweep_synthetic();
    /// The cookie this pipeline's next single commit carries; armed (generation stamped)
    /// when the commit asks for an event.
    [[nodiscard]] flip_cookie& next_cookie(bool with_event);
    void set_failure(int commit_errno);

    kms_device* device_ = nullptr;
    kms_pipeline_desc desc_{};
    unsigned reg_slot_ = ~0u;
    kms_mode mode_{};

    flip_cookie cookies_[2]{};
    std::atomic<unsigned> flip_generation_{0};
    /// A blocking modeset completed; `dispatch_events` reports it.
    std::atomic<bool> synthetic_flip_{false};

    /// Modeset-only properties (looked up once, at the first modeset). 0 when absent.
    bool modeset_props_ready_ = false;
    unsigned prop_conn_crtc_ = 0;
    unsigned prop_crtc_mode_ = 0;
    unsigned prop_crtc_active_ = 0;
    unsigned prop_crtc_vrr_ = 0;
    bool vrr_ = false;

    /// Neutral values a modeset writes (see `commit_modeset`), resolved at the first
    /// modeset — the cursor plane's when it is first part of one.
    struct neutral_setting {
        unsigned object = 0;
        unsigned prop = 0;
        unsigned long long value = 0;
    };
    static constexpr unsigned k_max_neutral = 24;
    neutral_setting neutral_[k_max_neutral]{};
    unsigned neutral_count_ = 0;
    unsigned neutral_cursor_plane_ = 0;
    /// Appends the neutral settings of one object (`object_type` = DRM_MODE_OBJECT_*).
    void resolve_neutral(int fd, unsigned object, unsigned object_type, bool cursor_plane);

    page_flip_handler flip_handler_ = nullptr;
    void* flip_user_ = nullptr;
    bool atomic_damage_ = false;
    /// What the in-flight commit shows (`note_inflight`): written by the committing thread
    /// before it raises `flip_pending_`, read by the event-draining thread.
    std::atomic<unsigned> inflight_tag_{0};
    std::atomic<bool> inflight_content_{false};
    /// Flip timestamps come from the CRTC's vblank (`page_flip_event::vblank_timestamp`):
    /// probed once by the committing thread after the first modeset, read by the
    /// event-draining thread.
    std::atomic<bool> vblank_timestamps_{true};
    bool vblank_probed_ = false;
    unsigned framebuffer_id_ = 0;
    unsigned damage_blob_id_ = 0;
    std::atomic<bool> flip_pending_{false};
    bool last_rejected_state_ = false;
    bool last_lost_master_ = false;

    unsigned prop_crtc_id_ = 0;
    unsigned prop_fb_id_ = 0;
    unsigned prop_src_x_ = 0;
    unsigned prop_src_y_ = 0;
    unsigned prop_src_w_ = 0;
    unsigned prop_src_h_ = 0;
    unsigned prop_crtc_x_ = 0;
    unsigned prop_crtc_y_ = 0;
    unsigned prop_crtc_w_ = 0;
    unsigned prop_crtc_h_ = 0;
    unsigned prop_fb_damage_clips_ = 0;
    unsigned prop_in_fence_fd_ = 0;
    unsigned prop_out_fence_ptr_ = 0;
    bool props_ready_ = false;
    unsigned mode_blob_id_ = 0;
    bool modeset_attempted_ = false;
    /// Owning `drmModeCrtc*` captured before the first modeset (opaque here to keep libdrm
    /// out of the module interface).
    void* saved_crtc_ = nullptr;
    unsigned saved_connector_ = 0;
};

/// One pipeline's part of a combined modeset (`commit_modeset`).
struct modeset_entry {
    kms_pipeline* pipe = nullptr;
    /// False: the pipeline goes dark — CRTC off (ACTIVE=0, no MODE_ID), connector unrouted,
    /// planes detached (an output being disabled or removed).
    bool enable = true;
    /// The first frame of the new configuration (sized to the pipeline's mode).
    unsigned framebuffer_id = 0;
    /// Borrowed sync_file the primary plane waits on, -1 for none.
    int in_fence_fd = -1;
    /// Borrowed; stated in full when given (shown where wanted, or off).
    kms_cursor_plane* cursor = nullptr;
    unsigned user_tag = 0;
    bool request_page_flip = true;
    /// TEST_ONLY of another mode than the pipeline's own (`set_mode` would make its next
    /// commit a modeset): a mode change is tested before the output switches to it. Null:
    /// `pipe->mode()`.
    const kms_mode* mode = nullptr;
    /// A disabled entry whose connector object no longer exists (an MST port that went):
    /// only its CRTC and planes are turned off.
    bool connector_gone = false;
    /// VRR_ENABLED for this modeset: -1 the pipeline's own (`set_vrr`), 0 or 1 to TEST_ONLY
    /// another (an output configuration that changes adaptive sync).
    int vrr = -1;
};

/// A configuration change of several pipelines in **one** ALLOW_MODESET commit (sway's
/// `wlr_backend_commit`, KWin's pipeline commit): each enabled entry gets connector → CRTC,
/// its `mode()` as MODE_ID, ACTIVE=1, its primary plane (with the in-fence) and cursor plane,
/// and every other plane left on its CRTC detached; a disabled entry goes dark. Everything
/// a previous master can leave behind is reset to the driver's initial state where the
/// object has it (`neutral_property_value`): CRTC gamma/degamma LUT, CTM and VRR; connector
/// link-status, Colorspace, HDR_OUTPUT_METADATA, max bpc and Broadcast RGB; rotation, alpha,
/// pixel blend mode and zpos of the primary and cursor planes. CRTCs no registered pipeline
/// owns that a previous master left active are turned off in the same commit, with their
/// planes and the connectors routed to them — otherwise the console or another compositor
/// keeps scanning a stale picture out on a monitor this session does not use (wlroots MR
/// 3967).
///
/// `test_only`: DRM_MODE_ATOMIC_TEST_ONLY of exactly that state (no event, nothing applied).
/// Otherwise nonblocking with one flip event per CRTC; a driver that refuses a nonblocking
/// modeset (EINVAL) gets it once more blocking — then each pipeline's completion is reported
/// by the next `kms_device::dispatch_events` (`completion_needs_dispatch`) — and, refused
/// again, once more without the neutral settings. Every pipeline must have no flip pending.
/// On failure no pipeline changed state; `last_failure_*` of every entry's pipeline says why.
[[nodiscard]] lx::result<void> commit_modeset(kms_device& device, const modeset_entry* entries,
                                              unsigned count, bool test_only);

} // namespace lx::drm
