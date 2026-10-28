// Race-free client identification: a peer's pidfd (SO_PEERPIDFD, Linux 6.5) taken at
// connect, and the /proc/<pid> answers of the privileged-shell and sandbox checks confirmed
// through it. A peer that exited is denied even when its pid names a live process with the
// right executable; a zombie peer, or one whose leader thread exited, is denied and counts as
// sandboxed; without a pidfd (older kernels) the SO_PEERCRED pid is taken alone.
// In-process AF_UNIX sockets only: no display, no root.
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server-core.h>
#endif

#include "lumen_test.hpp"

import lx.foundation;
import lx.session;
import lx.wayland.server;

namespace {

/// The pid a pidfd refers to, from its fdinfo; 0 when unknown, -1 once it has exited.
int pidfd_pid(int pidfd) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd);
    std::FILE* f = std::fopen(path, "r");
    if (!f)
        return 0;
    char line[128];
    int pid = 0;
    while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "Pid: %d", &pid) == 1)
            break;
    std::fclose(f);
    return pid;
}

int g_child_sock = -1;

void* child_reader(void*) {
    char byte = 0;
    (void)::read(g_child_sock, &byte, 1);
    ::_exit(0);
}

/// A connection accepted from a forked child, which waits until `release` and then exits.
/// With `leader_exits` the child's main thread exits at once and another thread waits.
struct child_peer {
    pid_t pid = -1;
    int conn = -1; ///< the server side of the child's connection

    explicit child_peer(bool leader_exits = false) {
        const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        LUMEN_CHECK(listener >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        // Abstract namespace: nothing left on the filesystem.
        const int n = std::snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                                    "lumen-test-peer-pidfd-%d", static_cast<int>(::getpid()));
        const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
        LUMEN_CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&addr), len) == 0);
        LUMEN_CHECK(::listen(listener, 1) == 0);
        pid = ::fork();
        LUMEN_CHECK(pid >= 0);
        if (pid == 0) {
            const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0 || ::connect(s, reinterpret_cast<sockaddr*>(&addr), len) != 0)
                ::_exit(1);
            g_child_sock = s;
            if (leader_exits) {
                pthread_t t;
                if (::pthread_create(&t, nullptr, child_reader, nullptr) != 0)
                    ::_exit(1);
                // SYS_exit ends this thread alone, without pthread_exit's unwinding.
                ::syscall(SYS_exit, 0);
            }
            child_reader(nullptr);
        }
        conn = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        ::close(listener);
        LUMEN_CHECK(conn >= 0);
    }
    /// The child exits but is not reaped: a zombie, still holding its pid.
    void exit_unreaped() {
        const char byte = 1;
        LUMEN_CHECK(::write(conn, &byte, 1) == 1);
        siginfo_t info{};
        LUMEN_CHECK(::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) == 0);
    }
    void reap() {
        int status = 0;
        LUMEN_CHECK(::waitpid(pid, &status, 0) == pid);
        LUMEN_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    /// The child exits and is reaped: its pid is free to be reused from here on.
    void release() {
        exit_unreaped();
        reap();
    }
    /// Waits until the child's /proc root is released (its leader thread has exited).
    [[nodiscard]] bool wait_root_gone() const {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/root", static_cast<int>(pid));
        for (int i = 0; i < 2000; ++i) {
            const int fd = ::open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                return true;
            ::close(fd);
            ::usleep(1000);
        }
        return false;
    }
};

/// The path the server resolves the shell binary to, and that /proc/<pid>/exe reports: this
/// test, so this process (and a forked child) counts as the shell.
void set_self_as_shell(lx::wayland::server& srv) {
    char self[PATH_MAX]{};
    LUMEN_CHECK(::realpath("/proc/self/exe", self) != nullptr);
    srv.set_shell_binary_path(self);
}

} // namespace

LUMEN_TEST(own_pidfd_from_socketpair) {
    if (!lx::session::peer_pidfd_supported()) {
        std::printf("  SKIP  SO_PEERPIDFD unsupported (Linux < 6.5)\n");
        return;
    }
    int sv[2];
    LUMEN_CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    auto fd = lx::session::peer_pidfd(sv[0]);
    LUMEN_CHECK(fd.has_value());
    LUMEN_CHECK(fd->get() >= 0);
    LUMEN_CHECK(pidfd_pid(fd->get()) == static_cast<int>(::getpid()));
    LUMEN_CHECK(lx::session::pidfd_alive(fd->get()));
    ::close(sv[0]);
    ::close(sv[1]);
    // Not a socket at all: no pidfd.
    LUMEN_CHECK(!lx::session::peer_pidfd(-1).has_value());
    LUMEN_CHECK(!lx::session::pidfd_alive(-1));
}

LUMEN_TEST(exited_peer_pidfd_is_not_alive) {
    if (!lx::session::peer_pidfd_supported()) {
        std::printf("  SKIP  SO_PEERPIDFD unsupported (Linux < 6.5)\n");
        return;
    }
    child_peer child;
    auto fd = lx::session::peer_pidfd(child.conn);
    LUMEN_CHECK(fd.has_value());
    LUMEN_CHECK(pidfd_pid(fd->get()) == static_cast<int>(child.pid));
    LUMEN_CHECK(lx::session::pidfd_alive(fd->get()));
    // A zombie holds its pid but has exited: its /proc root and executable are gone.
    child.exit_unreaped();
    LUMEN_CHECK(!lx::session::pidfd_alive(fd->get()));
    child.reap();
    // The pidfd keeps naming the child, which no longer runs.
    LUMEN_CHECK(!lx::session::pidfd_alive(fd->get()));
    ::close(child.conn);
}

#if defined(LUMEN_HAS_WAYLAND)
LUMEN_TEST(privileged_check_confirms_through_pidfd) {
    lx::wayland::server srv;
    LUMEN_CHECK(srv.bind_socketless().has_value());
    set_self_as_shell(srv);
    auto* display = static_cast<wl_display*>(srv.native_display());
    LUMEN_CHECK(display != nullptr);

    int sv[2];
    LUMEN_CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    wl_client* client = wl_client_create(display, sv[0]);
    LUMEN_CHECK(client != nullptr);
    auto* conn = srv.ensure_client(client);
    LUMEN_CHECK(conn != nullptr);
    const auto cred = conn->credentials();
    LUMEN_CHECK(cred.pid == static_cast<unsigned>(::getpid()));
    LUMEN_CHECK(!cred.peer_gone);
    const bool pidfds = lx::session::peer_pidfd_supported();
    LUMEN_CHECK(pidfds == (cred.pidfd >= 0));
    LUMEN_CHECK(srv.allow_privileged_global(cred, "zlm_shell_v1"));
    LUMEN_CHECK(!conn->sandboxed());

    // Fallback (no SO_PEERPIDFD): the SO_PEERCRED pid alone, as on kernels before 6.5.
    auto fallback = cred;
    fallback.pidfd = -1;
    LUMEN_CHECK(srv.allow_privileged_global(fallback, "zlm_shell_v1"));
    // The kernel has pidfds but had none for this peer: it was already gone.
    auto gone = fallback;
    gone.peer_gone = true;
    LUMEN_CHECK(!srv.allow_privileged_global(gone, "zlm_shell_v1"));

    if (pidfds) {
        // The pid now names a live process running the shell binary (this one), but the
        // peer it was recorded for has exited — the reuse race. Denied.
        child_peer child;
        auto dead = lx::session::peer_pidfd(child.conn);
        LUMEN_CHECK(dead.has_value());
        child.release();
        auto reused = cred;
        reused.pidfd = dead->get();
        LUMEN_CHECK(!srv.allow_privileged_global(reused, "zlm_shell_v1"));
        ::close(child.conn);
    }
    wl_client_destroy(client);
    ::close(sv[1]);
}

LUMEN_TEST(client_whose_peer_exited_is_denied) {
    lx::wayland::server srv;
    LUMEN_CHECK(srv.bind_socketless().has_value());
    set_self_as_shell(srv);
    auto* display = static_cast<wl_display*>(srv.native_display());

    // The child connects and exits before the client is first looked at, as a peer that
    // forked its connection off and left would.
    child_peer child;
    wl_client* client = wl_client_create(display, child.conn);
    LUMEN_CHECK(client != nullptr);
    child.release();
    auto* conn = srv.ensure_client(client);
    LUMEN_CHECK(conn != nullptr);
    const auto cred = conn->credentials();
    LUMEN_CHECK(cred.pid == static_cast<unsigned>(child.pid));
    if (lx::session::peer_pidfd_supported()) {
        // Linux 6.16 and later hand out a pidfd for an exited peer; earlier ones refuse.
        LUMEN_CHECK(cred.pidfd >= 0 || cred.peer_gone);
        LUMEN_CHECK(conn->sandboxed());
    }
    LUMEN_CHECK(!srv.allow_privileged_global(cred, "zlm_shell_v1"));
    wl_client_destroy(client);
}

LUMEN_TEST(zombie_peer_is_sandboxed_and_denied) {
    if (!lx::session::peer_pidfd_supported()) {
        std::printf("  SKIP  SO_PEERPIDFD unsupported (Linux < 6.5)\n");
        return;
    }
    lx::wayland::server srv;
    LUMEN_CHECK(srv.bind_socketless().has_value());
    set_self_as_shell(srv);
    auto* display = static_cast<wl_display*>(srv.native_display());

    // The peer exits and its parent never reaps it, while a child keeps the connection.
    child_peer child;
    wl_client* client = wl_client_create(display, child.conn);
    LUMEN_CHECK(client != nullptr);
    child.exit_unreaped();
    auto* conn = srv.ensure_client(client);
    LUMEN_CHECK(conn != nullptr);
    const auto cred = conn->credentials();
    LUMEN_CHECK(cred.pid == static_cast<unsigned>(child.pid));
    LUMEN_CHECK(cred.pidfd >= 0 || cred.peer_gone);
    LUMEN_CHECK(conn->sandboxed());
    LUMEN_CHECK(!srv.allow_privileged_global(cred, "zlm_shell_v1"));
    wl_client_destroy(client);
    child.reap();
}

LUMEN_TEST(peer_whose_leader_exited_is_sandboxed_and_denied) {
    if (!lx::session::peer_pidfd_supported()) {
        std::printf("  SKIP  SO_PEERPIDFD unsupported (Linux < 6.5)\n");
        return;
    }
    lx::wayland::server srv;
    LUMEN_CHECK(srv.bind_socketless().has_value());
    set_self_as_shell(srv);
    auto* display = static_cast<wl_display*>(srv.native_display());

    // The peer's main thread exits while another thread keeps the connection: the group
    // still runs, but the leader's /proc root and executable are released.
    child_peer child{true};
    LUMEN_CHECK(child.wait_root_gone());
    wl_client* client = wl_client_create(display, child.conn);
    LUMEN_CHECK(client != nullptr);
    auto* conn = srv.ensure_client(client);
    LUMEN_CHECK(conn != nullptr);
    const auto cred = conn->credentials();
    LUMEN_CHECK(cred.pid == static_cast<unsigned>(child.pid));
    LUMEN_CHECK(cred.pidfd >= 0);
    // The pidfd alone confirms the peer here; the unreadable root has to refuse.
    LUMEN_CHECK(lx::session::pidfd_alive(cred.pidfd));
    LUMEN_CHECK(conn->sandboxed());
    LUMEN_CHECK(!srv.allow_privileged_global(cred, "zlm_shell_v1"));
    child.release();
    wl_client_destroy(client);
}
#endif

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
