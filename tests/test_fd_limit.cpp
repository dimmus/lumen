// lx.session::fd_limit: the compositor's RLIMIT_NOFILE soft limit is raised to the hard
// limit, and a child started with `restore_in_child` between fork and exec runs with the
// limit the process started with (what `ulimit -n` in /bin/sh reports).
#include "lumen_test.hpp"

#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.session;

namespace {

/// `/bin/sh -c 'ulimit -n'` in a child, after `restore_in_child` when `restore`.
long child_ulimit(bool restore) {
    int fds[2];
    LUMEN_CHECK(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    LUMEN_CHECK(pid >= 0);
    if (pid == 0) {
        if (restore)
            lx::session::fd_limit::restore_in_child();
        ::dup2(fds[1], 1);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", "ulimit -n", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    char buf[64] = {};
    std::size_t got = 0;
    while (got + 1 < sizeof(buf)) {
        const ssize_t n = ::read(fds[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    LUMEN_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return std::strtol(buf, nullptr, 10);
}

} // namespace

LUMEN_TEST(raise_lifts_soft_to_hard_and_children_get_the_start_limit_back) {
    rlimit lim{};
    LUMEN_CHECK(::getrlimit(RLIMIT_NOFILE, &lim) == 0);
    if (lim.rlim_max <= 256) {
        std::printf("    skipped (hard limit %llu)\n",
                    static_cast<unsigned long long>(lim.rlim_max));
        return;
    }
    // Start where a systemd user service does (soft below hard), whatever this shell had.
    const rlim_t start = lim.rlim_max > 1024 ? 1024 : 256;
    lim.rlim_cur = start;
    LUMEN_CHECK(::setrlimit(RLIMIT_NOFILE, &lim) == 0);
    // Nothing raised yet: restoring is a no-op.
    LUMEN_CHECK(child_ulimit(true) == static_cast<long>(start));

    auto raised = lx::session::fd_limit::raise();
    LUMEN_CHECK(raised.has_value());
    LUMEN_CHECK(raised.value().before == start);
    LUMEN_CHECK(raised.value().after == lim.rlim_max);
    rlimit now{};
    LUMEN_CHECK(::getrlimit(RLIMIT_NOFILE, &now) == 0);
    LUMEN_CHECK(now.rlim_cur == lim.rlim_max && now.rlim_max == lim.rlim_max);
    // A second call answers the same start limit and changes nothing.
    auto again = lx::session::fd_limit::raise();
    LUMEN_CHECK(again.has_value() && again.value().before == start &&
                again.value().after == lim.rlim_max);

    // A child that does not restore inherits the raised limit; one that does gets `start`.
    LUMEN_CHECK(child_ulimit(false) == static_cast<long>(lim.rlim_max));
    LUMEN_CHECK(child_ulimit(true) == static_cast<long>(start));
    // The parent keeps its raised limit.
    LUMEN_CHECK(::getrlimit(RLIMIT_NOFILE, &now) == 0 && now.rlim_cur == lim.rlim_max);
    std::printf("    soft %llu -> %llu, restored child: %ld\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(lim.rlim_max), child_ulimit(true));
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
