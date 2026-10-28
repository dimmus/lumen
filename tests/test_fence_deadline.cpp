// dma-fence deadline hints (`lx.gfx:fence_deadline`): what is hinted and what is skipped,
// the once-only ENOTTY fallback on a fake ioctl, and the real ioctl — on a pipe (ENOTTY, as a
// kernel before 6.4 answers a sync_file) and, where /dev/udmabuf is open to this user, on a
// real sync_file exported from a dma-buf (accepted on 6.4+).
#include "lumen_test.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

import std;
import lx.gfx;

using lx::gfx::fence_deadline;

namespace {

struct fake_ioctl {
    static inline int result = 0;
    static inline unsigned calls = 0;
    static inline int last_fd = -1;
    static inline unsigned long long last_deadline = 0;

    static void reset(int r) {
        result = r;
        calls = 0;
        last_fd = -1;
        last_deadline = 0;
    }
    static int set(int fd, unsigned long long deadline) {
        ++calls;
        last_fd = fd;
        last_deadline = deadline;
        return result;
    }
};

/// A sync_file from a udmabuf's (empty) fences, or -1 where /dev/udmabuf is not available.
int udmabuf_sync_file() {
    const int dev = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0)
        return -1;
    int out = -1;
    const int mem = ::memfd_create("lumen-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mem >= 0 && ::ftruncate(mem, 4096) == 0 && ::fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
        udmabuf_create req{};
        req.memfd = static_cast<__u32>(mem);
        req.size = 4096;
        const int buf = ::ioctl(dev, UDMABUF_CREATE, &req);
        if (buf >= 0) {
            dma_buf_export_sync_file exp{};
            exp.flags = DMA_BUF_SYNC_READ;
            exp.fd = -1;
            if (::ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) == 0)
                out = exp.fd;
            ::close(buf);
        }
    }
    if (mem >= 0)
        ::close(mem);
    ::close(dev);
    return out;
}

} // namespace

LUMEN_TEST(hint_sets_the_deadline_with_one_call) {
    fake_ioctl::reset(0);
    fence_deadline d{"test", &fake_ioctl::set};
    LUMEN_CHECK(d.state() == fence_deadline::support::unknown);
    LUMEN_CHECK(d.hint(7, 16'666'667));
    LUMEN_CHECK(fake_ioctl::calls == 1 && fake_ioctl::last_fd == 7 &&
                fake_ioctl::last_deadline == 16'666'667);
    LUMEN_CHECK(d.state() == fence_deadline::support::yes);
}

LUMEN_TEST(hint_skips_without_a_fence_or_a_deadline) {
    fake_ioctl::reset(0);
    fence_deadline d{"test", &fake_ioctl::set};
    LUMEN_CHECK(!d.hint(-1, 1000));
    LUMEN_CHECK(!d.hint(7, 0));
    LUMEN_CHECK(fake_ioctl::calls == 0 && d.state() == fence_deadline::support::unknown);
}

LUMEN_TEST(enotty_turns_hints_off_for_good) {
    fake_ioctl::reset(-ENOTTY);
    fence_deadline d{"test", &fake_ioctl::set};
    LUMEN_CHECK(!d.hint(7, 1000));
    LUMEN_CHECK(d.state() == fence_deadline::support::no);
    fake_ioctl::result = 0; // even if it would work now, it is not asked again
    LUMEN_CHECK(!d.hint(7, 2000) && !d.hint(8, 3000));
    LUMEN_CHECK(fake_ioctl::calls == 1);
}

LUMEN_TEST(other_errors_leave_hints_on) {
    fake_ioctl::reset(-EBADF);
    fence_deadline d{"test", &fake_ioctl::set};
    LUMEN_CHECK(!d.hint(7, 1000));
    LUMEN_CHECK(d.state() == fence_deadline::support::unknown);
    fake_ioctl::result = 0;
    LUMEN_CHECK(d.hint(7, 2000) && fake_ioctl::calls == 2);
}

LUMEN_TEST(real_ioctl_on_a_pipe_is_enotty) {
    int p[2];
    LUMEN_CHECK(::pipe2(p, O_CLOEXEC) == 0);
    LUMEN_CHECK(lx::gfx::sync_file_set_deadline(p[0], 1000) == -ENOTTY);
    fence_deadline d{"test"};
    LUMEN_CHECK(!d.hint(p[0], 1000) && d.state() == fence_deadline::support::no);
    ::close(p[0]);
    ::close(p[1]);
}

LUMEN_TEST(real_ioctl_on_a_sync_file) {
    const int fd = udmabuf_sync_file();
    if (fd < 0) {
        std::printf("  (skipped: no /dev/udmabuf for this user)\n");
        return;
    }
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const auto now = static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
                     static_cast<unsigned long long>(ts.tv_nsec);
    const int r = lx::gfx::sync_file_set_deadline(fd, now + 16'666'667);
    // Linux 6.4+ takes it (on an already-signalled fence, a no-op); older kernels: ENOTTY.
    LUMEN_CHECK(r == 0 || r == -ENOTTY);
    fence_deadline d{"test"};
    LUMEN_CHECK(d.hint(fd, now + 16'666'667) == (r == 0));
    LUMEN_CHECK(d.state() ==
                (r == 0 ? fence_deadline::support::yes : fence_deadline::support::no));
    ::close(fd);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
