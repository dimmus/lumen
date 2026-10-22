module;

// lumen-shell's idle policy (design I1): when to power the outputs off. Pure — no Wayland:
// the shell feeds it ext_idle_notification_v1 events and executes what it returns through
// zwlr_output_power_v1. The compositor holds the mechanism only (activity, inhibitors, the
// power state machine, waking on input) and has no timeouts of its own; this is the default
// policy, and `--idle-policy=none` leaves it to an external daemon instead:
//
//   lumen-shell --idle-policy=none
//   swayidle -w timeout 600 'wlopm --off "*"' lock 'swaylock -f' before-sleep 'swaylock -f'
//
// Waking needs nothing here: input wakes the outputs inside the compositor (I5), which then
// reports `mode on` to every power object.
//
// The lock policy lives here too (ext-session-lock-v1 is the compositor's; which locker, and
// when, is the shell's): `--lock-command` names the locker (Lumen ships none, so without one
// nothing locks automatically and Super+L does nothing); with it the blank timeout locks
// first and blanks once the compositor reports `locked` (GNOME's order: the screen never goes
// dark unlocked); logind's Lock and before-sleep reach the shell as zlm_shell_v1
// lock_requested and start it too; a locker that dies (lock_state abandoned) is started
// again, at most once per 5 s, giving up after three in a row. While locked, a second, shorter
// idle timeout (`--blank-locked-after`, 60 s) blanks the lock screen (GNOME and KDE do the
// same).

export module lx.shell:idle_policy;

import std;

export namespace lx::shell {

struct idle_policy_config {
    /// `--idle-policy=shell|none`. False: no notification, no power requests — an external
    /// daemon (swayidle + wlopm) decides.
    bool enabled = true;
    /// `--blank-after S`: seconds without input, idle inhibitors obeyed, before every output
    /// is powered off. 0 = never.
    unsigned blank_after_s = 600;
    /// `--blank-locked-after S`: the same while the session is locked. 0 = never.
    unsigned blank_locked_after_s = 60;
    /// `--lock-command CMD`: the locker, an Exec line (`swaylock -f -c 000000`). Empty: no
    /// automatic locking, no Super+L, lock_requested ignored.
    const char* lock_command = nullptr;
    /// `--lock-on-blank` / `--no-lock-on-blank`: the blank timeout locks first, then blanks
    /// once locked. Meaningful only with a lock command.
    bool lock_on_blank = true;
};

/// zlm_shell_v1.lock_state (v5) values.
enum class shell_lock : unsigned { unlocked = 0, locking = 1, locked = 2, abandoned = 3 };

/// What the blank timeout asks for.
enum class idle_action : unsigned char {
    none,
    /// Power the outputs off now.
    blank,
    /// Start the locker; the outputs go off once the session is locked.
    lock_then_blank,
};

/// `shell` or `none`. False for anything else.
[[nodiscard]] bool parse_idle_policy(const char* text, bool& enabled);
/// A whole number of seconds, at most a week. False for anything else.
[[nodiscard]] bool parse_idle_seconds(const char* text, unsigned& seconds);

class idle_policy {
public:
    explicit idle_policy(idle_policy_config config = {}) : config_{config} {}

    void configure(idle_policy_config config) { config_ = config; }
    [[nodiscard]] const idle_policy_config& config() const { return config_; }

    /// The ext_idle_notification_v1 timeout to ask for (get_idle_notification: inhibitors
    /// obeyed), in ms; 0 when the shell asks for none (policy off, blanking disabled, or a
    /// global missing — nothing it could do on `idled`).
    [[nodiscard]] unsigned blank_timeout_ms(bool have_notifier, bool have_output_power) const;
    /// `idled` arrived: what to do (blank now, lock then blank, or nothing).
    [[nodiscard]] idle_action on_idled();
    /// `resumed` arrived (the compositor has woken the outputs already). A blank waiting for
    /// the lock is dropped: the user is back.
    void on_resumed() {
        idle_ = false;
        blank_when_locked_ = false;
    }
    [[nodiscard]] bool idle() const { return idle_; }

    // ── Lock policy ──
    [[nodiscard]] bool can_lock() const {
        return config_.lock_command && config_.lock_command[0] != '\0';
    }
    /// zlm_shell_v1.lock_state: true when the outputs are to be powered off now (the lock a
    /// blank timeout started is in place).
    [[nodiscard]] bool on_lock_state(shell_lock state);
    [[nodiscard]] shell_lock lock_state() const { return lock_; }
    /// The locked-screen timeout (ms) to ask for now: non-zero only while locked (or
    /// abandoned) with blanking on.
    [[nodiscard]] unsigned locked_timeout_ms(bool have_notifier, bool have_output_power) const;
    /// The locked-screen notification idled: true when the outputs are to be powered off.
    [[nodiscard]] bool on_locked_idled() const;
    /// zlm_shell_v1.lock_requested (logind, before sleep) or Super+L: true when the locker is
    /// to be started now.
    [[nodiscard]] bool on_lock_requested() const;
    /// The locker was started at `now_ns` (counts towards the relaunch limit).
    void note_locker_started(long long now_ns);
    /// The session became abandoned at `now_ns` (the locker died): nanoseconds to wait before
    /// starting it again (0: now), or -1 after the third failure in a row (the user must
    /// unlock through logind: `loginctl unlock-session`).
    [[nodiscard]] long long relaunch_delay(long long now_ns) const;
    /// Lockers started since the session was last unlocked.
    [[nodiscard]] unsigned locker_starts() const { return starts_; }
    static constexpr unsigned k_max_relaunches = 3;
    static constexpr long long k_relaunch_spacing_ns = 5'000'000'000LL;

private:
    idle_policy_config config_{};
    bool idle_ = false;
    shell_lock lock_ = shell_lock::unlocked;
    /// The blank timeout started the locker; blank once locked.
    bool blank_when_locked_ = false;
    unsigned starts_ = 0;
    long long last_start_ns_ = 0;
};

} // namespace lx::shell

bool lx::shell::parse_idle_policy(const char* text, bool& enabled) {
    if (!text)
        return false;
    if (std::strcmp(text, "shell") == 0) {
        enabled = true;
        return true;
    }
    if (std::strcmp(text, "none") == 0) {
        enabled = false;
        return true;
    }
    return false;
}

bool lx::shell::parse_idle_seconds(const char* text, unsigned& seconds) {
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 7L * 24 * 3600)
        return false;
    seconds = static_cast<unsigned>(v);
    return true;
}

unsigned lx::shell::idle_policy::blank_timeout_ms(bool have_notifier,
                                                  bool have_output_power) const {
    if (!config_.enabled || config_.blank_after_s == 0 || !have_notifier || !have_output_power)
        return 0;
    return config_.blank_after_s * 1000u;
}

lx::shell::idle_action lx::shell::idle_policy::on_idled() {
    if (idle_)
        return idle_action::none; // idled twice without resumed would be the compositor's error
    idle_ = true;
    if (!config_.enabled || config_.blank_after_s == 0)
        return idle_action::none;
    // Never dark while unlocked when a locker exists: lock, blank once locked.
    if (lock_ == shell_lock::unlocked && config_.lock_on_blank && can_lock()) {
        blank_when_locked_ = true;
        return idle_action::lock_then_blank;
    }
    return idle_action::blank;
}

bool lx::shell::idle_policy::on_lock_state(shell_lock state) {
    lock_ = state;
    if (state == shell_lock::unlocked) {
        blank_when_locked_ = false;
        starts_ = 0;
        return false;
    }
    if (state == shell_lock::locked && blank_when_locked_) {
        blank_when_locked_ = false;
        return true;
    }
    return false;
}

unsigned lx::shell::idle_policy::locked_timeout_ms(bool have_notifier,
                                                   bool have_output_power) const {
    if (!config_.enabled || config_.blank_locked_after_s == 0 || !have_notifier ||
        !have_output_power)
        return 0;
    if (lock_ != shell_lock::locked && lock_ != shell_lock::abandoned)
        return 0;
    return config_.blank_locked_after_s * 1000u;
}

bool lx::shell::idle_policy::on_locked_idled() const {
    return config_.enabled && config_.blank_locked_after_s != 0 &&
           (lock_ == shell_lock::locked || lock_ == shell_lock::abandoned);
}

bool lx::shell::idle_policy::on_lock_requested() const {
    return can_lock() && lock_ == shell_lock::unlocked;
}

void lx::shell::idle_policy::note_locker_started(long long now_ns) {
    ++starts_;
    last_start_ns_ = now_ns;
}

long long lx::shell::idle_policy::relaunch_delay(long long now_ns) const {
    if (!can_lock() || starts_ >= k_max_relaunches + 1)
        return -1; // the first start plus three relaunches
    const long long since = now_ns - last_start_ns_;
    return starts_ == 0 || since >= k_relaunch_spacing_ns ? 0 : k_relaunch_spacing_ns - since;
}
