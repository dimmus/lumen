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
    cfg_windows = 1u << 7,  ///< [windows]
    cfg_workspaces = 1u << 8,
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
    /// "~/.config/lumen/config.toml" (or the explicit file; "(none)" when disabled).
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
    char source_[128]{};
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

} // namespace lx::compositor
