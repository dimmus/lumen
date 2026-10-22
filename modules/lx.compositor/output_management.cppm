module;

// wlr-output-management (zwlr_output_manager_v1 v4, ADR 008 C1/C2, §C.1–C.3), the wire-free
// half: heads and their modes as the protocol shows them, what changed between two
// snapshots of them (the events a bound manager is sent), configurations as a client builds
// them with every protocol error the spec defines, their validation into a complete target
// state, and the queue of apply/test jobs. Pure; the wire half is
// wire_output_management.cpp, the compositor's half (test, switch, commit, persistence) is in
// compositor_impl.cpp. `test_output_management` drives this model alone.
//
// Strictness. Every error of the protocol is raised, including the two wlroots and niri do
// not check — `already_set` (a property set twice, or a mode and a custom mode) and
// `unconfigured_head` (a head left out of the configuration). The spec calls both protocol
// errors. Order at apply/test: `already_used` first (a malformed request whatever the
// state), then the serial — a head that appeared after the client's `done` is not the
// client's fault, and `cancelled` describes exactly that — then `unconfigured_head`.
//
// Modes are identified by their tuple (size, refresh, preferred): the protocol has no event
// that takes back "preferred", so a mode whose tuple changes is finished and a new object
// introduced. A custom mode in use (not in the head's list) is shown as one more mode
// object, as wlroots' virtual mode.
//
// Member bodies are in output_management.cpp (Clang 23 module-partition workaround).

export module lx.compositor:output_management;

import std;

export namespace lx::compositor::mgmt {

inline constexpr unsigned k_max_heads = 32;
inline constexpr unsigned k_max_modes = 96;
/// Heads without a connector (virtual outputs) have keys from here up.
inline constexpr unsigned k_virtual_key = 0x8000'0000u;

struct mode {
    int width = 0;
    int height = 0;
    /// mHz; 0: unknown / unspecified.
    int refresh_mhz = 0;
    bool preferred = false;

    [[nodiscard]] bool same(const mode& o) const {
        return width == o.width && height == o.height && refresh_mhz == o.refresh_mhz &&
               preferred == o.preferred;
    }
    [[nodiscard]] bool same_size_refresh(const mode& o) const {
        return width == o.width && height == o.height && refresh_mhz == o.refresh_mhz;
    }
};

/// One head: a connected monitor (enabled or not) or a virtual output.
struct head {
    /// Stable while the head exists: its connector's KMS id, or `k_virtual_key | n`.
    unsigned key = 0;
    char name[32]{};
    char description[160]{};
    char make[64]{};
    char model[64]{};
    char serial[64]{};
    int phys_w_mm = 0;
    int phys_h_mm = 0;
    mode modes[k_max_modes]{};
    unsigned mode_count = 0;
    bool enabled = false;
    /// The current mode (enabled), or the one it would get (disabled): an index into
    /// `modes`, or -1 for `custom`.
    int current = -1;
    mode custom{};
    /// Layout position, transform, scale and adaptive sync — when disabled, what enabling it
    /// without setting them gives (the defaults, as wlroots keeps the last state).
    int x = 0;
    int y = 0;
    unsigned transform = 0;
    double scale = 1.0;
    bool adaptive_sync = false;
    /// The connector supports adaptive sync (`vrr_capable`).
    bool vrr_capable = false;
    /// Enabling it without a position places it automatically (no explicit position yet).
    bool auto_position = true;

    [[nodiscard]] mode current_mode() const {
        return current >= 0 && static_cast<unsigned>(current) < mode_count ? modes[current] : custom;
    }
    [[nodiscard]] int preferred_index() const;
};

struct head_set {
    head heads[k_max_heads]{};
    unsigned count = 0;

    [[nodiscard]] const head* find(unsigned key) const;
};

/// What changed in a head (bits; `changed_*`): the events a bound manager is sent.
enum : unsigned {
    changed_enabled = 1u << 0,
    changed_mode = 1u << 1,
    changed_position = 1u << 2,
    changed_transform = 1u << 3,
    changed_scale = 1u << 4,
    changed_adaptive_sync = 1u << 5,
    /// The mode list (any tuple came or went).
    changed_modes = 1u << 6,
};

/// The same monitor on the same connector: what `name`, `description`, `physical_size`,
/// `make`, `model` and `serial_number` (sent once per object) say. A head whose identity
/// changed is a new head (the old one is finished).
[[nodiscard]] bool same_identity(const head& a, const head& b);
/// `changed_*` bits between two states of one head. Only what the protocol shows counts:
/// a disabled head's position, scale or transform is not visible, so changing it is not a
/// change (enabling sends everything).
[[nodiscard]] unsigned head_changes(const head& before, const head& after);

/// A manager's view of the heads changed: what to finish, what to introduce, what to resend.
struct heads_diff {
    /// Keys of heads gone (or whose identity changed).
    unsigned removed[k_max_heads]{};
    unsigned removed_count = 0;
    /// Indices into the new set of heads to introduce.
    unsigned added[k_max_heads]{};
    unsigned added_count = 0;
    /// Indices into the new set of heads that changed, and their `changed_*` bits.
    unsigned changed[k_max_heads]{};
    unsigned changed_bits[k_max_heads]{};
    unsigned changed_count = 0;

    [[nodiscard]] bool empty() const {
        return removed_count == 0 && added_count == 0 && changed_count == 0;
    }
};
void diff_heads(const head_set& before, const head_set& after, heads_diff& out);

// ── configurations ──

enum class config_error : unsigned char {
    none,
    // zwlr_output_configuration_v1.error
    already_configured_head,
    unconfigured_head,
    already_used,
    // zwlr_output_configuration_head_v1.error
    already_set,
    invalid_mode,
    invalid_custom_mode,
    invalid_transform,
    invalid_scale,
    invalid_adaptive_sync_state,
};
/// The protocol's error code of `e` on its interface (configuration or configuration head).
[[nodiscard]] unsigned error_code(config_error e);
[[nodiscard]] const char* error_text(config_error e);

/// One `enable_head` / `disable_head` of a configuration, with what was set on it.
struct config_head {
    unsigned key = 0;
    bool enabled = false;
    bool mode_set = false;
    /// `set_custom_mode` (else `set_mode` with a mode of the head's).
    bool custom = false;
    mode mode{};
    bool position_set = false;
    int x = 0;
    int y = 0;
    bool transform_set = false;
    unsigned transform = 0;
    bool scale_set = false;
    double scale = 1.0;
    bool adaptive_sync_set = false;
    bool adaptive_sync = false;
};

/// zwlr_output_configuration_head_v1 requests, checked in the spec's order.
[[nodiscard]] config_error set_mode(config_head& h, const mode& m);
[[nodiscard]] config_error set_custom_mode(config_head& h, int width, int height, int refresh_mhz);
[[nodiscard]] config_error set_position(config_head& h, int x, int y);
[[nodiscard]] config_error set_transform(config_head& h, int transform);
[[nodiscard]] config_error set_scale(config_head& h, double scale);
[[nodiscard]] config_error set_adaptive_sync(config_head& h, unsigned state);

struct configuration {
    unsigned serial = 0;
    config_head heads[k_max_heads]{};
    unsigned count = 0;
    /// `apply` or `test` was sent: anything but `destroy` is `already_used`.
    bool used = false;

    [[nodiscard]] config_head* find(unsigned key);
    [[nodiscard]] const config_head* find(unsigned key) const;
};

/// `enable_head` / `disable_head` of head `key`. `out` receives the new entry (enable).
[[nodiscard]] config_error add_head(configuration& c, unsigned key, bool enabled,
                                    config_head** out = nullptr);

enum class submit_verdict : unsigned char {
    ok,
    /// `apply`/`test` twice, or a request after it: protocol error `already_used`.
    already_used,
    /// The serial is not the current one: `cancelled` (the client's view is outdated).
    cancelled,
    /// A current head is in no `enable_head`/`disable_head`: protocol error.
    unconfigured_head,
};
/// `apply` / `test`: marks `c` used and judges it (§C.2's order).
[[nodiscard]] submit_verdict submit(configuration& c, unsigned current_serial, const head_set& heads);

/// The complete state a configuration asks for: every head, enabled or not.
struct head_target {
    unsigned key = 0;
    bool enabled = false;
    mode mode{};
    /// `mode` is not one of the head's (a custom mode).
    bool custom = false;
    /// The client set the mode (`set_mode` / `set_custom_mode`): the output stops following
    /// its preferred mode unless that is what it chose (H3).
    bool mode_explicit = false;
    int x = 0;
    int y = 0;
    /// The client set the position (or it is enabled already and keeps its own).
    bool position_explicit = false;
    unsigned transform = 0;
    double scale = 1.0;
    bool adaptive_sync = false;
};
struct target {
    head_target heads[k_max_heads]{};
    unsigned count = 0;

    [[nodiscard]] const head_target* find(unsigned key) const;
};

enum class resolve_failure : unsigned char {
    none,
    /// Every head disabled: powering everything off is zwlr_output_power_v1's job (niri).
    no_head_enabled,
    /// A custom mode outside 1..16384, or a head that cannot take custom modes.
    bad_custom_mode,
    /// A set_mode's mode is no longer the head's (stale client).
    mode_gone,
    /// A scale that rounds to nothing usable (> 10 after rounding to n/120).
    bad_scale,
    /// Adaptive sync asked of a head without `vrr_capable`.
    adaptive_sync_unsupported,
    /// More enabled heads than the hardware has CRTCs (or than output slots).
    too_many_enabled,
};
[[nodiscard]] const char* failure_text(resolve_failure f);

/// The target state of a configuration that `submit` passed: unset properties keep the head's
/// current (or, for a head being enabled, its default) values; scales are rounded to what a
/// fractional-scale client can be told (n/120). `max_enabled`: CRTCs (or slots) available.
[[nodiscard]] resolve_failure resolve(const configuration& c, const head_set& heads,
                                      unsigned max_enabled, target& out);

/// The target that keeps `heads` as they are (a revert, the persisted state's comparison).
void current_target(const head_set& heads, target& out);

/// One apply or test waiting for the compositor. The configuration is a copy: the client may
/// destroy its object meanwhile (`token` is then cleared and the result dropped).
struct job {
    configuration config{};
    bool test = false;
    /// The wire's handle on the configuration object (0: detached): a number never reused,
    /// not its address — a job still running when its object is destroyed must not answer
    /// a new object the allocator put at the same address.
    unsigned long long token = 0;
};

/// One job runs at a time; up to `k_max` wait behind it (§C.3 step 7). Beyond that a job is
/// refused (`cancelled`). Pure ring buffer, no allocation.
class job_queue {
public:
    static constexpr unsigned k_max = 4;
    /// False when full.
    bool push(const job& j);
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] unsigned size() const { return count_; }
    [[nodiscard]] job& front() { return jobs_[head_]; }
    void pop();
    /// Clears `token` from every queued job that carries it (the object went).
    void detach(unsigned long long token);

private:
    job jobs_[k_max]{};
    unsigned head_ = 0;
    unsigned count_ = 0;
};

} // namespace lx::compositor::mgmt
