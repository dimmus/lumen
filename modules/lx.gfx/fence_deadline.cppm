module;

// dma-fence deadline hints (SYNC_IOC_SET_DEADLINE, Linux 6.4): tells the driver behind a
// sync_file when its fence is needed, so one that honours the hint (i915/xe waitboost, msm's
// devfreq boost) clocks up in time for the vblank instead of after a missed one. A hint, never
// a wait: the ioctl returns at once, signalled or not; a driver without a `set_deadline` hook
// accepts it and does nothing (vmwgfx). Behind a plain function so the fallback is tested
// without a fence.

#include <errno.h>
#include <sys/ioctl.h>
#include <linux/sync_file.h>

#ifndef SYNC_IOC_SET_DEADLINE // Linux 6.4
struct sync_set_deadline {
    __u64 deadline_ns;
    __u64 pad;
};
#define SYNC_IOC_SET_DEADLINE _IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline)
#endif

export module lx.gfx:fence_deadline;

import std;

import lx.trace;

namespace lx::gfx {
constexpr lx::trace::category log_fence{"gfx"};
}

export namespace lx::gfx {

/// SYNC_IOC_SET_DEADLINE on `sync_file` (borrowed) for `deadline_ns` (CLOCK_MONOTONIC):
/// 0, or a negative errno — ENOTTY from a kernel before 6.4 (and from any fd that is not a
/// sync_file).
[[nodiscard]] int sync_file_set_deadline(int sync_file, unsigned long long deadline_ns);

/// Deadline hints for the fences one thread hands on (render: its composite fences; UI:
/// client buffer fences), each instance confined to that thread. Probed by use: the first
/// ENOTTY turns it off for good, so an older kernel costs one failed ioctl. No allocation;
/// support is logged once (debug) when it is first known.
class fence_deadline {
public:
    using set_fn = int (*)(int sync_file, unsigned long long deadline_ns);
    enum class support : unsigned char { unknown, yes, no };

    /// `what` names the fences in the one log line. `set`: the ioctl (a fake in tests).
    explicit fence_deadline(const char* what, set_fn set = &sync_file_set_deadline)
        : what_{what}, set_{set} {}

    /// Asks the fences in `sync_file` (borrowed) to signal by `deadline_ns`: one ioctl. No
    /// fence (-1), no deadline (0) or no kernel support: nothing, false.
    bool hint(int sync_file, unsigned long long deadline_ns);
    [[nodiscard]] support state() const { return state_; }

private:
    const char* what_;
    set_fn set_;
    support state_ = support::unknown;
};

} // namespace lx::gfx

int lx::gfx::sync_file_set_deadline(int sync_file, unsigned long long deadline_ns) {
    sync_set_deadline req{};
    req.deadline_ns = deadline_ns;
    return ::ioctl(sync_file, SYNC_IOC_SET_DEADLINE, &req) == 0 ? 0 : -errno;
}

bool lx::gfx::fence_deadline::hint(int sync_file, unsigned long long deadline_ns) {
    if (sync_file < 0 || deadline_ns == 0 || state_ == support::no)
        return false;
    const int r = set_(sync_file, deadline_ns);
    if (r == 0) {
        if (state_ == support::unknown) {
            state_ = support::yes;
            log_fence.debug("dma-fence deadline hints on {} fences (SYNC_IOC_SET_DEADLINE)", what_);
        }
        return true;
    }
    if (r == -ENOTTY) {
        state_ = support::no;
        log_fence.debug("dma-fence deadline hints unavailable (SYNC_IOC_SET_DEADLINE: ENOTTY, "
                        "Linux < 6.4): {} fences are not hinted",
                        what_);
    }
    return false;
}
