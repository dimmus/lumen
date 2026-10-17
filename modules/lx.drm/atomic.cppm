module;

#include <atomic>

import lx.foundation;
import lx.runtime;

export module lx.drm:atomic;

export namespace lx::drm {

class kms_device;
class kms_cursor_plane;

struct kms_damage_region {
    lx::rect2i rects[16]{};
    unsigned count = 0;
    bool full_frame = true;
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
    unsigned connector = 0;
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
    /// The CRTC the event came from (0 for a synthetic completion).
    unsigned crtc_id = 0;
};

using page_flip_handler = void (*)(page_flip_event event, void* user_data);

class kms_atomic_commit {
public:
    explicit kms_atomic_commit(kms_device& device);
    /// Puts the CRTC back the way the console left it, so exiting on a TTY does not leave
    /// a dead display behind.
    ~kms_atomic_commit();

    kms_atomic_commit(const kms_atomic_commit&) = delete;
    kms_atomic_commit& operator=(const kms_atomic_commit&) = delete;

    /// Committing thread (render). The first commit — and the first after
    /// `prepare_resume` — is a full modeset: connector → CRTC, MODE_ID and ACTIVE, the
    /// primary plane with its in-fence, the cursor plane when given, and every other plane
    /// left on this CRTC (another master's overlay) disabled. Everything else a previous
    /// master can leave behind is reset to the driver's initial state where the object has
    /// it (`neutral_property_value`): CRTC gamma/degamma LUT, CTM and VRR; connector
    /// link-status, Colorspace, HDR_OUTPUT_METADATA, max bpc and Broadcast RGB; rotation,
    /// alpha, pixel blend mode and zpos of the primary and cursor planes. A driver that
    /// refuses those (EINVAL) gets the modeset once more without them. Nonblocking, with a flip event; a
    /// driver that refuses a nonblocking modeset (EINVAL) gets it once more blocking, and
    /// that completion is reported by the next `dispatch_events` (see
    /// `completion_needs_dispatch`). The flip handler never runs on the committing thread:
    /// a failure is reported by the return value only.
    [[nodiscard]] lx::result<void> commit(const atomic_commit_request& request);
    void set_page_flip_handler(page_flip_handler handler, void* user_data = nullptr);
    /// The thread that owns the flip handler (UI): drains flip events (the card fd is
    /// nonblocking, so this never waits) and reports a blocking modeset's completion.
    /// An event from before the last `prepare_resume` is dropped unreported.
    void dispatch_events();
    /// The last successful commit completed synchronously (a blocking modeset), so no
    /// event will arrive on the card fd: the committing thread must have the event-draining
    /// thread call `dispatch_events`, which reports it as the flip.
    [[nodiscard]] bool completion_needs_dispatch() const {
        return synthetic_flip_.load(std::memory_order_acquire);
    }
    /// Committing thread, before the first commit after the session regained the device
    /// (VT switch back): the KMS state is whatever the last master left, so the next
    /// content commit is a full modeset again. Flip events still in flight are disowned
    /// (their generation is stale), `flip_pending` is cleared. The CRTC state saved at the
    /// first modeset (the console's, restored at exit) is kept.
    void prepare_resume();
    /// Committing thread. Display power off (zwlr_output_power_v1 off): the kernel's atomic
    /// "DPMS off" — CRTC ACTIVE=0 with MODE_ID and the connector routing kept, and `cursor`
    /// (when given) taken off its CRTC. One **blocking** ALLOW_MODESET commit with no event:
    /// drivers differ on whether a disabled CRTC sends one, and the caller needs none. The
    /// primary plane stays attached (an inactive CRTC's planes are not scanned out, and the
    /// power-on modeset states it again); a driver that refuses that (EINVAL) is asked once
    /// more with the primary plane detached too. Requires a CRTC this object has modeset
    /// before (mode_invalid otherwise). Power comes back on through `prepare_resume`: the
    /// next content commit is a full modeset with ACTIVE=1. Fails with EBUSY semantics while
    /// a flip is pending — check `flip_pending` first.
    [[nodiscard]] lx::result<void> commit_power_off(kms_cursor_plane* cursor,
                                                    unsigned connector = 0);

    [[nodiscard]] bool supports_atomic_damage() const;
    /// Clips are limited to the connector's active mode, which is what the scanout
    /// framebuffer is sized to; the kernel fails the whole commit for a clip past it.
    void build_damage_blob(const kms_damage_region& region, unsigned connector_index = 0);
    void set_framebuffer(unsigned fb_id);

    /// True while a flip requested by `commit()` has not yet been reported by
    /// `dispatch_events()`. A CRTC takes one flip at a time, so committing again before
    /// then is rejected with EBUSY. Set on the committing thread, cleared on the thread
    /// that drains events.
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

private:
    [[nodiscard]] lx::result<void> ensure_props();
    /// Full atomic modeset: binds connector → CRTC, sets the mode and enables the plane.
    /// Without it the CRTC keeps the mode the console left behind and the driver never
    /// shows our framebuffer.
    [[nodiscard]] lx::result<void> apply_modeset(const atomic_commit_request& request,
                                                 unsigned crtc, unsigned fb, unsigned width,
                                                 unsigned height);
    /// Reports one flip to the handler (event-draining thread only). `hw`: a kernel event
    /// (its sequence and CRTC are real), else a blocking modeset's synthetic completion.
    void emit_flip(lx::runtime::clock_time timestamp_ns, unsigned sequence, unsigned crtc_id,
                   bool hw);
    /// Records what the commit about to raise the flip shows (committing thread, before the
    /// ioctl — the event can be drained before the ioctl returns).
    void note_inflight(const atomic_commit_request& request) {
        inflight_tag_.store(request.user_tag, std::memory_order_relaxed);
        inflight_content_.store(request.update_primary, std::memory_order_release);
    }

    /// `drmModeAtomicCommit` user data: which generation of commits an event belongs to.
    /// Two slots, so the one the in-flight commits point at is not rewritten by the
    /// generation after it.
    struct flip_cookie {
        kms_atomic_commit* owner = nullptr;
        std::atomic<unsigned> generation{0};
    };
    flip_cookie cookies_[2]{};
    std::atomic<unsigned> flip_generation_{0};
    /// A blocking modeset completed; `dispatch_events` reports it.
    std::atomic<bool> synthetic_flip_{false};

    /// Modeset-only properties (looked up once, at the first modeset). 0 when absent.
    bool modeset_props_ready_ = false;
    unsigned prop_conn_crtc_ = 0;
    unsigned prop_crtc_mode_ = 0;
    unsigned prop_crtc_active_ = 0;

    /// Neutral values a modeset writes (see `commit`), resolved at the first modeset —
    /// the cursor plane's when it is first part of one.
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

    kms_device* device_ = nullptr;
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

} // namespace lx::drm
