module;

// Bodies of `lx.gfx:drm_timeline`. The libdrm calls are the ones Mutter uses
// (src/common/meta-drm-timeline.c): drmSyncobjEventfd with flags 0 (wake when *signalled*,
// not merely materialized), and a release point set from a sync_file through a temporary
// syncobj and drmSyncobjTransfer.

#include <errno.h>

#if defined(LUMEN_HAS_DRM)
#include <xf86drm.h>
#include <sys/eventfd.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <signal.h>

module lx.gfx;

import std;

import lx.foundation;
import :drm_timeline;

namespace lx::gfx {

namespace {

lx::error timeline_error(const char* message) {
    return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::timeline_failed),
                          message);
}

#if defined(LUMEN_HAS_DRM)
int fd_of(void* user) { return static_cast<int>(reinterpret_cast<long>(user)); }

int real_fd_to_handle(void* user, int syncobj_fd, unsigned* handle) {
    std::uint32_t h = 0;
    const int r = drmSyncobjFDToHandle(fd_of(user), syncobj_fd, &h);
    *handle = h;
    return r != 0 ? -errno : 0;
}

int real_query(void* user, unsigned handle, unsigned long long point, bool* signalled) {
    std::uint32_t h = handle;
    std::uint64_t p = point;
    // Timeout 0 with WAIT_FOR_SUBMIT: an unmaterialized point is "not yet", not an error.
    const int r = drmSyncobjTimelineWait(fd_of(user), &h, &p, 1, 0,
                                         DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, nullptr);
    if (r == 0) {
        *signalled = true;
        return 0;
    }
    if (errno == ETIME || errno == ETIMEDOUT) {
        *signalled = false;
        return 0;
    }
    return -errno;
}

int real_eventfd(void* user, unsigned handle, unsigned long long point, int efd) {
    return drmSyncobjEventfd(fd_of(user), handle, point, efd, 0) != 0 ? -errno : 0;
}

int real_signal(void* user, unsigned handle, unsigned long long point) {
    std::uint32_t h = handle;
    std::uint64_t p = point;
    return drmSyncobjTimelineSignal(fd_of(user), &h, &p, 1) != 0 ? -errno : 0;
}

int real_import_sync_file(void* user, unsigned handle, unsigned long long point, int sync_file) {
    const int fd = fd_of(user);
    std::uint32_t tmp = 0;
    if (drmSyncobjCreate(fd, 0, &tmp) != 0)
        return -errno;
    int r = drmSyncobjImportSyncFile(fd, tmp, sync_file);
    if (r == 0)
        r = drmSyncobjTransfer(fd, handle, point, tmp, 0, 0);
    const int err = r != 0 ? -errno : 0;
    drmSyncobjDestroy(fd, tmp);
    return err;
}

void real_destroy(void* user, unsigned handle) { (void)drmSyncobjDestroy(fd_of(user), handle); }
#endif

} // namespace

drm_timeline_ops drm_timeline_ops_for(int drm_fd) {
    drm_timeline_ops ops{};
#if defined(LUMEN_HAS_DRM)
    if (drm_fd < 0)
        return ops;
    ops.fd_to_handle = &real_fd_to_handle;
    ops.query = &real_query;
    ops.eventfd = &real_eventfd;
    ops.signal = &real_signal;
    ops.import_sync_file = &real_import_sync_file;
    ops.destroy = &real_destroy;
    ops.user = reinterpret_cast<void*>(static_cast<long>(drm_fd));
#else
    (void)drm_fd;
#endif
    return ops;
}

bool drm_timeline_supported(int drm_fd) {
#if defined(LUMEN_HAS_DRM)
    if (drm_fd < 0)
        return false;
    std::uint64_t cap = 0;
    if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || cap == 0)
        return false;
    const int efd = ::eventfd(0, EFD_CLOEXEC);
    if (efd < 0)
        return false;
    const int r = drmSyncobjEventfd(drm_fd, 0, 0, efd, 0);
    const int err = r != 0 ? errno : 0;
    ::close(efd);
    return r != 0 && err == ENOENT;
#else
    (void)drm_fd;
    return false;
#endif
}

// ── drm_timeline_table ─────────────────────────────────────────────────────

drm_timeline_table::~drm_timeline_table() {
    for (unsigned i = 1; i <= k_max; ++i)
        if (entries_[i].refs && ops_.destroy)
            ops_.destroy(ops_.user, entries_[i].handle);
}

const drm_timeline_table::entry* drm_timeline_table::find(unsigned id) const {
    return id >= 1 && id <= k_max && entries_[id].refs ? &entries_[id] : nullptr;
}

lx::result<unsigned> drm_timeline_table::import(int syncobj_fd) {
    unsigned handle = 0;
    const int r = ops_.fd_to_handle ? ops_.fd_to_handle(ops_.user, syncobj_fd, &handle) : -ENODEV;
#if !defined(_WIN32)
    if (syncobj_fd >= 0)
        ::close(syncobj_fd);
#endif
    if (r != 0)
        return timeline_error("drm timeline: not a syncobj fd");
    for (unsigned i = 1; i <= k_max; ++i) {
        if (entries_[i].refs)
            continue;
        entries_[i].handle = handle;
        entries_[i].refs = 1;
        ++live_;
        return i;
    }
    if (ops_.destroy)
        ops_.destroy(ops_.user, handle);
    return timeline_error("drm timeline: table full");
}

void drm_timeline_table::ref(unsigned id) {
    if (id >= 1 && id <= k_max && entries_[id].refs)
        ++entries_[id].refs;
}

void drm_timeline_table::unref(unsigned id) {
    if (id < 1 || id > k_max || entries_[id].refs == 0)
        return;
    if (--entries_[id].refs != 0)
        return;
    if (ops_.destroy)
        ops_.destroy(ops_.user, entries_[id].handle);
    entries_[id] = {};
    --live_;
}

lx::result<bool> drm_timeline_table::signalled(unsigned id, unsigned long long point) const {
    const entry* e = find(id);
    if (!e || !ops_.query)
        return timeline_error("drm timeline: unknown timeline");
    bool done = false;
    if (ops_.query(ops_.user, e->handle, point, &done) != 0)
        return timeline_error("drm timeline: point query failed");
    return done;
}

lx::result<void> drm_timeline_table::notify(unsigned id, unsigned long long point,
                                            int eventfd) const {
    const entry* e = find(id);
    if (!e || !ops_.eventfd)
        return timeline_error("drm timeline: unknown timeline");
    if (ops_.eventfd(ops_.user, e->handle, point, eventfd) != 0)
        return timeline_error("drm timeline: SYNCOBJ_EVENTFD failed");
    return {};
}

lx::result<void> drm_timeline_table::signal(unsigned id, unsigned long long point) const {
    const entry* e = find(id);
    if (!e || !ops_.signal)
        return timeline_error("drm timeline: unknown timeline");
    if (ops_.signal(ops_.user, e->handle, point) != 0)
        return timeline_error("drm timeline: signal failed");
    return {};
}

lx::result<void> drm_timeline_table::signal_after(unsigned id, unsigned long long point,
                                                  int sync_file) const {
    if (sync_file < 0)
        return signal(id, point);
    const entry* e = find(id);
    if (!e || !ops_.import_sync_file)
        return timeline_error("drm timeline: unknown timeline");
    if (ops_.import_sync_file(ops_.user, e->handle, point, sync_file) != 0)
        return timeline_error("drm timeline: sync_file transfer failed");
    return {};
}

// ── release_fence_mailbox ──────────────────────────────────────────────────

release_fence_mailbox::~release_fence_mailbox() {
#if !defined(_WIN32)
    if (fd_ >= 0)
        ::close(fd_);
#endif
}

void release_fence_mailbox::publish(int sync_file, unsigned frame) {
    int old = -1;
    {
        std::lock_guard lock{mutex_};
        old = fd_;
        fd_ = sync_file;
        frame_ = frame;
    }
#if !defined(_WIN32)
    if (old >= 0)
        ::close(old);
#endif
}

int release_fence_mailbox::dup(unsigned* frame) const {
    std::lock_guard lock{mutex_};
    if (frame)
        *frame = frame_;
#if !defined(_WIN32)
    return fd_ >= 0 ? ::fcntl(fd_, F_DUPFD_CLOEXEC, 0) : -1;
#else
    return -1;
#endif
}

} // namespace lx::gfx
