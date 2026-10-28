module;

#include <errno.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77 // Linux 6.5
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424 // Linux 5.1
#endif

import lx.foundation;
import lx.runtime;

export module lx.session:privilege;

import std;

export namespace lx::session {

struct credentials {
    unsigned uid = 0;
    unsigned gid = 0;
    unsigned pid = 0;
};

struct shell_verification {
    unsigned expected_uid = 0;
    const char* shell_binary_path = "lumen-shell";
    bool require_flatpak_token = false;
};

enum class privilege_result {
    allowed,
    denied_uid_mismatch,
    denied_binary_mismatch,
    denied_missing_portal_token,
    denied_not_shell_global,
};

/// Validates privileged zlm_shell_v1 access (SO_PEERCRED + binary path + optional portal).
/// The caller reads the binary path through the peer's pid and confirms the pid still names
/// the peer (`pidfd_alive`) before asking.
class privilege_checker {
public:
    explicit privilege_checker(shell_verification config = {});

    /// `client_binary_path` is the peer's executable as the kernel names it
    /// (`/proc/<pid>/exe`); it must equal `shell_binary_path` exactly, which the caller
    /// resolves to a canonical absolute path once. Null (unknown) is a mismatch.
    [[nodiscard]] privilege_result check_shell_bind(const credentials& peer,
                                                    const char* client_binary_path,
                                                    const char* portal_token) const;

    [[nodiscard]] privilege_result check_global(const credentials& peer,
                                                const char* global_name,
                                                bool global_privileged) const;

    [[nodiscard]] const shell_verification& config() const;

private:
    shell_verification config_{};
};

/// SO_PEERPIDFD (Linux 6.5): a pidfd for the process that connected `socket_fd`. Unlike the
/// number SO_PEERCRED reports, it keeps naming that process after it exits: a pid read for
/// the peer can be confirmed with `pidfd_alive` afterwards. Fails when the kernel lacks the
/// option (see `peer_pidfd_supported`) or — before Linux 6.16 — when the peer has exited.
[[nodiscard]] lx::result<lx::unique_fd> peer_pidfd(int socket_fd);

/// The kernel hands out peer pidfds: SO_PEERPIDFD on a fresh socketpair, whose peer is this
/// process, succeeds; false before Linux 6.5 (ENOPROTOOPT). Probes on each call.
[[nodiscard]] bool peer_pidfd_supported();

/// The process `pidfd` refers to has not exited (signal 0 through pidfd_send_signal; EPERM
/// still means it exists). A zombie counts as running: it holds its pid until reaped.
[[nodiscard]] bool pidfd_alive(int pidfd);

} // namespace lx::session


lx::session::privilege_checker::privilege_checker(shell_verification config)
    : config_{config} {}

lx::session::privilege_result
lx::session::privilege_checker::check_shell_bind(const credentials& peer,
                                                 const char* client_binary_path,
                                                 const char* portal_token) const {
    if (config_.expected_uid != 0 && peer.uid != config_.expected_uid)
        return privilege_result::denied_uid_mismatch;
    if (config_.require_flatpak_token && (!portal_token || portal_token[0] == '\0'))
        return privilege_result::denied_missing_portal_token;
    if (config_.shell_binary_path &&
        (!client_binary_path || std::strcmp(client_binary_path, config_.shell_binary_path) != 0))
        return privilege_result::denied_binary_mismatch;
    (void)portal_token;
    return privilege_result::allowed;
}

lx::session::privilege_result
lx::session::privilege_checker::check_global(const credentials& peer,
                                             const char* global_name,
                                             bool global_privileged) const {
    if (!global_privileged) return privilege_result::allowed;
    if (!global_name) return privilege_result::denied_not_shell_global;
    return check_shell_bind(peer, config_.shell_binary_path, nullptr);
}

const lx::session::shell_verification& lx::session::privilege_checker::config() const {
    return config_;
}

lx::result<lx::unique_fd> lx::session::peer_pidfd(int socket_fd) {
    int fd = -1;
    socklen_t len = sizeof(fd);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERPIDFD, &fd, &len) != 0 || fd < 0)
        return lx::make_error(lx::io_err::not_found, "SO_PEERPIDFD: no pidfd for the peer");
    return lx::unique_fd{fd};
}

bool lx::session::peer_pidfd_supported() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return false;
    const bool ok = peer_pidfd(sv[0]).has_value();
    ::close(sv[0]);
    ::close(sv[1]);
    return ok;
}

bool lx::session::pidfd_alive(int pidfd) {
    if (pidfd < 0)
        return false;
    return ::syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) == 0 || errno == EPERM;
}
