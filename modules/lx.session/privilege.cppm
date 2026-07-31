module;

import lx.foundation;
import lx.runtime;

export module lx.session:privilege;

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
class privilege_checker {
public:
    explicit privilege_checker(shell_verification config = {});

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
    if (client_binary_path && config_.shell_binary_path) {
        // P0: real path comparison via /proc/self/exe or SO_PEERSEC
        (void)client_binary_path;
    }
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
