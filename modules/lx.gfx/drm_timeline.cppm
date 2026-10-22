module;

// DRM timeline syncobjs for linux-drm-syncobj-v1 (explicit sync): the client's timelines,
// imported once as handles on the compositor's DRM fd, and the operations the protocol needs
// on their points — is the acquire point signalled, wake me when it is (an eventfd the UI
// loop watches), signal a release point now, or signal it when a sync_file does. Wayland-free,
// and behind an ops table so the protocol logic is tested without a syncobj-capable driver.
// Bodies are in drm_timeline.cpp.

#include <atomic>
#include <mutex>

export module lx.gfx:drm_timeline;

import lx.foundation;

export namespace lx::gfx {

/// The kernel operations, as plain functions (real libdrm ones from `drm_timeline_ops_for`;
/// a fake in tests). Each returns 0 or a negative errno.
struct drm_timeline_ops {
    int (*fd_to_handle)(void* user, int syncobj_fd, unsigned* handle) = nullptr;
    /// `*signalled` = whether `point` has signalled (never blocks).
    int (*query)(void* user, unsigned handle, unsigned long long point, bool* signalled) = nullptr;
    /// Makes `eventfd` readable once `point` has signalled (DRM_IOCTL_SYNCOBJ_EVENTFD, flags 0).
    int (*eventfd)(void* user, unsigned handle, unsigned long long point, int eventfd) = nullptr;
    int (*signal)(void* user, unsigned handle, unsigned long long point) = nullptr;
    /// Attaches `sync_file`'s fence at `point` (a temporary syncobj, then a transfer).
    int (*import_sync_file)(void* user, unsigned handle, unsigned long long point,
                            int sync_file) = nullptr;
    void (*destroy)(void* user, unsigned handle) = nullptr;
    void* user = nullptr;
};

/// The real ops on `drm_fd` (borrowed). Empty ops without libdrm.
[[nodiscard]] drm_timeline_ops drm_timeline_ops_for(int drm_fd);
/// Whether explicit sync can be offered on `drm_fd`: timeline syncobjs
/// (DRM_CAP_SYNCOBJ_TIMELINE) and DRM_IOCTL_SYNCOBJ_EVENTFD (probed with handle 0, which a
/// kernel that has the ioctl answers with ENOENT — wlroots' and Mutter's probe).
[[nodiscard]] bool drm_timeline_supported(int drm_fd);

/// Imported timelines, refcounted: a wp_linux_drm_syncobj_timeline_v1 holds one reference,
/// every point set from it another until that point is used ("timeline points ... are not
/// unset" by destroying the object). UI affinity; fixed table.
class drm_timeline_table {
public:
    static constexpr unsigned k_max = 256;

    drm_timeline_table() = default;
    ~drm_timeline_table();
    drm_timeline_table(const drm_timeline_table&) = delete;
    drm_timeline_table& operator=(const drm_timeline_table&) = delete;

    void bind(const drm_timeline_ops& ops) { ops_ = ops; }
    [[nodiscard]] bool bound() const { return ops_.fd_to_handle != nullptr; }

    /// Imports a syncobj fd (always closed) as a timeline with one reference. Returns its
    /// id (> 0), or an error (`drm_err::timeline_failed`: not a syncobj; or the table is full).
    [[nodiscard]] lx::result<unsigned> import(int syncobj_fd);
    void ref(unsigned id);
    void unref(unsigned id);
    [[nodiscard]] unsigned live() const { return live_; }

    [[nodiscard]] lx::result<bool> signalled(unsigned id, unsigned long long point) const;
    [[nodiscard]] lx::result<void> notify(unsigned id, unsigned long long point, int eventfd) const;
    [[nodiscard]] lx::result<void> signal(unsigned id, unsigned long long point) const;
    /// Signals `point` when `sync_file` does (`sync_file` borrowed). Without a fence (-1) the
    /// point is signalled now.
    [[nodiscard]] lx::result<void> signal_after(unsigned id, unsigned long long point,
                                                int sync_file) const;

private:
    struct entry {
        unsigned handle = 0;
        unsigned refs = 0;
    };
    [[nodiscard]] const entry* find(unsigned id) const;
    drm_timeline_ops ops_{};
    entry entries_[k_max + 1]{};
    unsigned live_ = 0;
};

/// The newest composite fence (a sync_file) of a frame that sampled an explicit-sync
/// buffer, published by render and read by UI to signal release points: the GPU executes
/// composites in order, so the newest covers every earlier read. Render: `publish` (takes
/// ownership); UI: `dup` (a new fd the caller owns, or -1: none, the composite blocked until
/// done). A mutex, not a lock-free slot: the fd must not be closed while UI dups it.
class release_fence_mailbox {
public:
    ~release_fence_mailbox();
    void publish(int sync_file, unsigned frame);
    [[nodiscard]] int dup(unsigned* frame = nullptr) const;
    /// Whether render should publish (some explicit-sync surface exists). UI sets, render
    /// reads; a stale read costs one dup or one missed fence before a release that then
    /// signals at once — after render's low-water mark, which already passed the frame.
    void set_wanted(bool wanted) { wanted_.store(wanted, std::memory_order_relaxed); }
    [[nodiscard]] bool wanted() const { return wanted_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_{};
    int fd_ = -1;
    unsigned frame_ = 0;
    std::atomic<bool> wanted_{false};
};

} // namespace lx::gfx
