module;

// The compositor's configuration (design-session-and-wm C.4, C.5): the snapshot in use, the
// bytes it came from (what the shell is handed), the diagnostics of the latest generation,
// the watch, and what changed between two snapshots — so a reload applies only what moved.
// Also the conversions from the file's terms to the subsystems' (the binding table, a
// device's libinput settings, the keymap's names). Executing a change is the compositor's
// (compositor_impl.cpp): this partition knows no compositor internals. UI affinity. Bodies in
// config_apply.cpp (Clang 23 module-partition workaround).

export module lx.compositor:config_apply;

import std;
import lx.config;
import lx.input;
import :keybindings;

export namespace lx::compositor {

/// What a reload changed (`diff_config`).
enum config_change : unsigned {
    cfg_keymap = 1u << 0,   ///< layout, variant, options, model, rules
    cfg_repeat = 1u << 1,   ///< repeat-rate, repeat-delay
    cfg_pointer = 1u << 2,  ///< the pointer sections and devices
    cfg_cursor = 1u << 3,   ///< cursor theme, size
    cfg_focus = 1u << 4,    ///< [focus]
    cfg_bindings = 1u << 5, ///< [bindings], general.default-bindings, idle.lock-command
    cfg_shell = 1u << 6,    ///< a key the shell applies
    cfg_windows = 1u << 7,  ///< [windows], [tiling], [animations]
    cfg_workspaces = 1u << 8,
    cfg_outputs = 1u << 9, ///< [[output]]
    cfg_ui = 1u << 10,     ///< [ui], including the server-side decoration palette
};

[[nodiscard]] unsigned diff_config(const lx::config::config_snapshot& a,
                                   const lx::config::config_snapshot& b);

class config_state {
public:
    enum class outcome : unsigned char {
        /// The files did not change (and the reload was not forced): no new generation.
        unchanged,
        /// A syntax error (or a file that cannot be read): the snapshot in use stays.
        rejected,
        /// A new snapshot is in use.
        accepted,
    };
    struct result {
        outcome what = outcome::unchanged;
        /// `config_change` bits (accepted only).
        unsigned changes = 0;
        /// The accepted bytes differ from the previous accepted bytes.
        bool bytes_changed = false;
    };

    config_state();
    ~config_state();
    config_state(const config_state&) = delete;
    config_state& operator=(const config_state&) = delete;

    /// Locates the layers and starts watching them. `env` for tests.
    void init(lx::config::env_lookup env = nullptr);
    /// Reads and parses every layer. `force`: a new generation even when no byte changed
    /// (`lumen-ctl reload`, SIGHUP).
    [[nodiscard]] result reload(bool force);

    [[nodiscard]] const lx::config::config_snapshot& current() const { return *current_; }
    [[nodiscard]] const lx::config::config_text& accepted_text() const { return accepted_; }
    /// The latest generation's diagnostics (apply-time ones are added by the compositor).
    [[nodiscard]] lx::config::diagnostics& diagnostics() { return diag_; }
    [[nodiscard]] const lx::config::diagnostics& diagnostics() const { return diag_; }
    /// The file a diagnostic's layer names (of the bytes read last, accepted or not).
    [[nodiscard]] const char* path_of(unsigned layer) const;
    [[nodiscard]] unsigned generation() const { return generation_; }
    /// The latest generation was accepted.
    [[nodiscard]] bool accepted_last() const { return accepted_last_; }
    [[nodiscard]] const lx::config::config_location& location() const { return location_; }
    [[nodiscard]] lx::config::config_watch& watch() { return watch_; }
    /// What the current generation read: "~/.config/lumen/config.toml" (or the explicit file),
    /// a system layer when the user has no file, "built-in defaults (no …)" when nothing was
    /// read, "(none: LUMEN_CONFIG=0)" when disabled.
    [[nodiscard]] const char* describe_source() const;

private:
    lx::config::config_location location_{};
    lx::config::config_watch watch_{};
    std::unique_ptr<lx::config::config_snapshot> current_;
    lx::config::config_text accepted_{};
    lx::config::config_text read_{};
    lx::config::diagnostics diag_{};
    unsigned generation_ = 0;
    bool accepted_last_ = true;
    bool loaded_ = false;
    char source_[520]{};
    char defaults_[560]{};
};

/// The table the key matcher uses: every binding of `c`, minus `shell.lock` when no lock
/// command is configured (the key then keeps reaching applications, as without a locker).
void binding_table(const lx::config::config_snapshot& c,
                   std::inplace_vector<binding_entry, keybindings::k_max_bindings>& out);

/// libinput settings for a device of class `kind` named `name`.
[[nodiscard]] lx::input::pointer_config pointer_config_of(const lx::config::config_snapshot& c,
                                                          const char* name,
                                                          lx::input::device_class kind);

/// The keymap names (null for "the system default"); pointers into `k`.
[[nodiscard]] lx::input::keymap_config keymap_config_of(const lx::config::keyboard_settings& k);

/// The interface theme applications start with ([ui], ADR-013 §4.7): the configured name or
/// `.lxtheme` path as written, or — when none is configured — the built-in the appearance
/// selects ("one-light" / "one-dark"). This is what the compositor exports as `LUMEN_THEME`
/// (at start-up and again when a reload changed it), so an application that starts later
/// comes up in the configured look; `start_theme_job` resolves the same way.
[[nodiscard]] std::string_view resolved_ui_theme(const lx::config::ui_settings& ui);

/// The bookkeeping of the session-environment re-exports (ADR-013 §4.7). The user bus
/// carries one export at a time (`lx.session user_environment`), so a re-export that finds
/// another in flight is *owed* (`owe`) and leaves when that one's reply arrives (`drain` —
/// cursor first: a reload exports `[cursor]` before it reaches `[ui]`; a `drain` send that
/// finds the bus busy again simply owes itself back). The resolved `LUMEN_THEME` last sent
/// is remembered so only a changed value re-exports (`[ui]` density and font-size edits do
/// not churn the bus). `drop` is teardown: an export whose reply never came must not
/// re-export into an environment `unset_now` is clearing.
class env_reexports {
public:
    /// Which re-export a busy bus held back.
    enum class which : unsigned char { cursor, theme };

    /// This re-export cannot leave now (another one is in flight): send it on the reply.
    void owe(which w);
    /// An export's reply arrived, so the bus is free: send what was held back, in the order
    /// the reload produced it. `send` runs one re-export; it may call `owe` again (an export
    /// it started first is now the one in flight).
    template <typename Send>
    void drain(Send&& send) {
        if (owed_ & k_cursor) {
            owed_ &= ~k_cursor;
            send(which::cursor);
        }
        if (owed_ & k_theme) {
            owed_ &= ~k_theme;
            send(which::theme);
        }
    }
    /// Teardown (`end_session`): owed re-exports are dropped — a retry from inside
    /// `unset_now`'s flush would only re-set what it is unsetting — and the remembered
    /// theme value leaves with the session (the environment is being unset anyway).
    void drop();

    /// Would this resolved theme value reach the user manager as a re-export? Only a
    /// changed value does — what start-up exported counts as sent.
    [[nodiscard]] bool theme_changed(std::string_view resolved) const;
    /// The export left (start-up's batch, or a re-export that was sent): `resolved` is the
    /// value the user manager now carries.
    void theme_sent(std::string_view resolved);
    /// The export did not reach the manager (refused, timed out, or never sent): the next
    /// reload re-sends even the same value, so it must not count as carried.
    void theme_forget();

private:
    static constexpr unsigned k_cursor = 1u << static_cast<unsigned>(which::cursor);
    static constexpr unsigned k_theme = 1u << static_cast<unsigned>(which::theme);
    unsigned owed_ = 0;
    char theme_[96]{}; // the resolved value last sent ("" = none the manager carries)
};

} // namespace lx::compositor
