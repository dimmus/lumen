module;

import lx.foundation;
import lx.shell.policy;

export module lx.portal;

export namespace lx::portal {

/// Optional Flatpak build adapter — validates portal capability tokens for
/// privileged shell access (see docs/uml/16-security.mmd).
struct capability_token {
    const char* token = "";
    const char* app_id = "";
};

class desktop_portal {
public:
    [[nodiscard]] static lx::result<desktop_portal> open();

    [[nodiscard]] bool validate_shell_token(const capability_token& token) const;
    [[nodiscard]] bool validate_screenshot_token(const capability_token& token) const;

    /// Bridge xdg-desktop-portal requests to compositor policy (P1).
    void register_policy_bridge(lx::shell::policy_registry& policies);

private:
    bool available_ = false;
};

} // namespace lx::portal

module :private;

lx::result<lx::portal::desktop_portal> lx::portal::desktop_portal::open() {
    return lx::not_implemented("lx::portal::desktop_portal::open");
}

bool lx::portal::desktop_portal::validate_shell_token(const capability_token& token) const {
    (void)token;
    return false;
}

bool lx::portal::desktop_portal::validate_screenshot_token(const capability_token& token) const {
    (void)token;
    return false;
}

void lx::portal::desktop_portal::register_policy_bridge(lx::shell::policy_registry&) {}
