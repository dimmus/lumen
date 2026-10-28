module;

// The configuration's schema (design-session-and-wm C.2): every key the file may hold, its
// type, range or choices, its default, who applies it (the compositor or the shell) and when
// (live, at start-up only, at the next open), as one table of descriptors — what the parser
// validates against, what "did you mean" searches, and what the documentation lists.
//
// `config_snapshot` is the parsed result: plain data with fixed-size strings and tables, so
// it can be copied, compared field by field and kept as "the previous configuration" without
// allocating. Both processes hold one (the compositor ~12 KiB, the shell the same).

export module lx.config:schema;

import std;
import :keys;
import :actions;

export namespace lx::config {

// ── Pointer devices (libinput) ──

enum class tap_map : unsigned char { lrm, lmr };
enum class scroll_method : unsigned char { device_default, two_finger, edge, on_button_down, none };
enum class click_method : unsigned char { device_default, button_areas, clickfinger };
enum class accel_profile : unsigned char { device_default, adaptive, flat };

/// Which `pointer_settings` fields a file set (a per-device override applies only those).
enum pointer_field : std::uint32_t {
    pf_tap = 1u << 0,
    pf_tap_button_map = 1u << 1,
    pf_drag = 1u << 2,
    pf_drag_lock = 1u << 3,
    pf_three_finger_drag = 1u << 4,
    pf_natural_scroll = 1u << 5,
    pf_scroll_method = 1u << 6,
    pf_click_method = 1u << 7,
    pf_accel_profile = 1u << 8,
    pf_accel_speed = 1u << 9,
    pf_left_handed = 1u << 10,
    pf_disable_while_typing = 1u << 11,
    pf_middle_emulation = 1u << 12,
    pf_scroll_factor = 1u << 13,
};
inline constexpr std::uint32_t k_pointer_fields_all = (1u << 14) - 1u;
/// The keys each class section takes ([input.touchpad] all of them).
inline constexpr std::uint32_t k_mouse_fields = pf_natural_scroll | pf_accel_profile |
                                                pf_accel_speed | pf_left_handed |
                                                pf_middle_emulation | pf_scroll_factor;
inline constexpr std::uint32_t k_trackpoint_fields =
    pf_accel_profile | pf_accel_speed | pf_scroll_factor;

struct pointer_settings {
    bool tap = false;
    tap_map tap_button_map = tap_map::lrm;
    bool drag = true;
    bool drag_lock = false;
    bool three_finger_drag = false;
    bool natural_scroll = false;
    scroll_method scroll = scroll_method::device_default;
    click_method click = click_method::device_default;
    accel_profile accel = accel_profile::device_default;
    double accel_speed = 0.0;
    bool left_handed = false;
    bool disable_while_typing = true;
    bool middle_emulation = false;
    /// Compositor-side multiplier of scroll values.
    double scroll_factor = 1.0;
    /// `pointer_field` bits the file set.
    std::uint32_t set = 0;
};

struct device_override {
    /// libinput's device name, exactly.
    char name[128]{};
    pointer_settings settings{};
    unsigned layer = 0;
    unsigned line = 0;
};

// ── The other sections ──

struct keyboard_settings {
    char layout[128]{};
    char variant[128]{};
    char options[256]{};
    char model[64]{};
    char rules[64]{};
    int repeat_rate = 25;
    int repeat_delay = 600;
    bool numlock = false;
};

struct cursor_settings {
    char theme[64]{};
    int size = 0;
};

enum class focus_mode : unsigned char { click, sloppy };
struct focus_settings {
    focus_mode mode = focus_mode::click;
    bool raise_on_click = true;
    bool raise_on_sloppy = false;
};

struct window_settings {
    bool edge_snap = true;
    int edge_snap_distance = 8;
    int edge_snap_corner = 64;
    bool snap_preview = true;
    bool drag_restore = true;
};

enum class workspace_mode : unsigned char { dynamic, fixed };
struct workspace_settings {
    static constexpr unsigned k_max_names = 16;
    workspace_mode mode = workspace_mode::dynamic;
    int count = 4;
    char names[k_max_names][32]{};
    unsigned name_count = 0;
    bool switch_on_activate = true;
    bool move_follows = true;
};

enum class switcher_scope : unsigned char { workspace, all };
struct switcher_settings {
    switcher_scope scope = switcher_scope::workspace;
    bool include_minimized = true;
    int open_delay = 150;
};

enum class idle_policy_kind : unsigned char { shell, none };
struct idle_settings {
    idle_policy_kind policy = idle_policy_kind::shell;
    int blank_after = 600;
    int blank_locked_after = 60;
    char lock_command[512]{};
    bool lock_on_blank = true;
};

struct background_settings {
    char spec[512] = "lumen";
};

enum class notification_position : unsigned char {
    top_right,
    top_center,
    top_left,
    bottom_right,
    bottom_left,
};
struct notification_settings {
    notification_position position = notification_position::top_right;
    int timeout = 5;
    int timeout_low = 3;
    int critical_timeout = 0;
    int max_visible = 3;
    bool dnd_while_sharing = true;
    int history = 50;
};

/// `auto`: the render and UI threads ask for real-time, high priority or latency hints,
/// whichever the kernel or rtkit grants (threading.md, "Thread scheduling"); `off`: their
/// scheduling is left as the process started.
enum class realtime_mode : unsigned char { automatic, off };
struct compositor_settings {
    realtime_mode realtime = realtime_mode::automatic;
};

// ── Bindings ──

enum binding_option : unsigned char {
    /// Fires also while the focused client inhibits shortcuts.
    bo_allow_when_inhibited = 0x1,
    /// Fires also while the session is locked (lock-safe actions only).
    bo_allow_when_locked = 0x2,
    /// Fires again on key repeat while held.
    bo_repeat = 0x4,
};

struct binding {
    combo keys{};
    action_id action = action_id::none;
    /// action_arg::index: the 1-based index; else 0.
    unsigned argument = 0;
    unsigned char options = 0;
    /// Where it came from (`k_no_layer` for a default binding).
    unsigned layer = ~0u;
    unsigned line = 0;
};

inline constexpr unsigned k_max_bindings = 128;
inline constexpr unsigned k_max_devices = 16;

struct config_snapshot {
    int version = 1;
    keyboard_settings keyboard{};
    pointer_settings touchpad{};
    pointer_settings mouse{};
    pointer_settings trackpoint{};
    std::inplace_vector<device_override, k_max_devices> devices{};
    cursor_settings cursor{};
    focus_settings focus{};
    window_settings windows{};
    workspace_settings workspaces{};
    switcher_settings switcher{};
    idle_settings idle{};
    background_settings background{};
    notification_settings notifications{};
    compositor_settings compositor{};
    bool default_bindings = true;
    /// Effective: the defaults (unless `default_bindings` is false) with the files' bindings
    /// over them.
    std::inplace_vector<binding, k_max_bindings> bindings{};
};

/// The configuration with no file: every default, the default bindings.
[[nodiscard]] config_snapshot default_config();

/// The class sections' effective settings for a device (`pointer_settings` of
/// `touchpad`/`mouse`/`trackpoint` with the `[[input.device]]` override of that name over
/// them). `kind`: 0 touchpad, 1 mouse, 2 trackpoint.
[[nodiscard]] pointer_settings device_settings(const config_snapshot& c, unsigned kind,
                                               std::string_view device_name);

// ── Descriptors ──

enum class value_kind : unsigned char { boolean, integer, number, string, enumeration, string_list };
enum class key_owner : unsigned char { compositor, shell };
enum class reload_class : unsigned char {
    /// Applied as soon as the file changes.
    live,
    /// Read at start-up only.
    start,
    /// Applies the next time the thing it configures opens.
    next_open,
};

struct key_desc {
    const char* section = "";
    const char* key = "";
    value_kind kind = value_kind::boolean;
    key_owner owner = key_owner::compositor;
    reload_class reload = reload_class::live;
    double lo = 0.0;
    double hi = 0.0;
    /// enumeration: the accepted words, index = the stored value.
    const char* const* names = nullptr;
    unsigned name_count = 0;
    /// string: the field's capacity (bytes, NUL included).
    unsigned capacity = 0;
    /// The field in a snapshot.
    void* (*field)(config_snapshot&) = nullptr;
};

/// A key of the pointer sections (`[input.touchpad]`, `[input.mouse]`, `[input.trackpoint]`,
/// `[[input.device]]`): the field of a `pointer_settings`.
struct pointer_key_desc {
    const char* key = "";
    value_kind kind = value_kind::boolean;
    std::uint32_t bit = 0;
    double lo = 0.0;
    double hi = 0.0;
    const char* const* names = nullptr;
    unsigned name_count = 0;
    void* (*field)(pointer_settings&) = nullptr;
};

/// Every key outside the pointer sections and `[bindings]`.
[[nodiscard]] std::span<const key_desc> schema_keys();
[[nodiscard]] std::span<const pointer_key_desc> pointer_keys();
/// The sections a file may have.
[[nodiscard]] std::span<const char* const> schema_sections();

/// A default binding, as the documented file writes it.
struct default_binding {
    const char* keys = "";
    action_id action = action_id::none;
    unsigned argument = 0;
    unsigned char options = 0;
};
[[nodiscard]] std::span<const default_binding> default_bindings();

/// An enumeration value's word ("adaptive").
[[nodiscard]] const char* enum_word(const char* const* names, unsigned count, unsigned value);

} // namespace lx::config

namespace {

using namespace lx::config;

constexpr const char* k_tap_maps[] = {"lrm", "lmr"};
constexpr const char* k_scroll_methods[] = {"default", "two-finger", "edge", "on-button-down",
                                            "none"};
constexpr const char* k_click_methods[] = {"default", "button-areas", "clickfinger"};
constexpr const char* k_accel_profiles[] = {"default", "adaptive", "flat"};
constexpr const char* k_focus_modes[] = {"click", "sloppy"};
constexpr const char* k_workspace_modes[] = {"dynamic", "static"};
constexpr const char* k_switcher_scopes[] = {"workspace", "all"};
constexpr const char* k_idle_policies[] = {"shell", "none"};
constexpr const char* k_realtime_modes[] = {"auto", "off"};
constexpr const char* k_positions[] = {"top-right", "top-center", "top-left", "bottom-right",
                                       "bottom-left"};

template <std::size_t N>
constexpr unsigned count_of(const char* const (&)[N]) {
    return static_cast<unsigned>(N);
}

constexpr key_owner k_c = key_owner::compositor;
constexpr key_owner k_s = key_owner::shell;
constexpr reload_class k_live = reload_class::live;

#define LX_FIELD(expr) [](config_snapshot& c) -> void* { return &(c.expr); }
#define LX_PFIELD(expr) [](pointer_settings& p) -> void* { return &(p.expr); }

const key_desc k_keys[] = {
    // [input.keyboard]
    {"input.keyboard", "layout", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 128,
     LX_FIELD(keyboard.layout)},
    {"input.keyboard", "variant", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 128,
     LX_FIELD(keyboard.variant)},
    {"input.keyboard", "options", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 256,
     LX_FIELD(keyboard.options)},
    {"input.keyboard", "model", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 64,
     LX_FIELD(keyboard.model)},
    {"input.keyboard", "rules", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 64,
     LX_FIELD(keyboard.rules)},
    {"input.keyboard", "repeat-rate", value_kind::integer, k_c, k_live, 0, 1000, nullptr, 0, 0,
     LX_FIELD(keyboard.repeat_rate)},
    {"input.keyboard", "repeat-delay", value_kind::integer, k_c, k_live, 1, 10000, nullptr, 0, 0,
     LX_FIELD(keyboard.repeat_delay)},
    {"input.keyboard", "numlock", value_kind::boolean, k_c, reload_class::start, 0, 0, nullptr, 0,
     0, LX_FIELD(keyboard.numlock)},
    // [cursor]
    {"cursor", "theme", value_kind::string, k_c, k_live, 0, 0, nullptr, 0, 64,
     LX_FIELD(cursor.theme)},
    {"cursor", "size", value_kind::integer, k_c, k_live, 0, 256, nullptr, 0, 0,
     LX_FIELD(cursor.size)},
    // [focus]
    {"focus", "mode", value_kind::enumeration, k_c, k_live, 0, 0, k_focus_modes,
     count_of(k_focus_modes), 0, LX_FIELD(focus.mode)},
    {"focus", "raise-on-click", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(focus.raise_on_click)},
    {"focus", "raise-on-sloppy", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(focus.raise_on_sloppy)},
    // [windows]
    {"windows", "edge-snap", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(windows.edge_snap)},
    {"windows", "edge-snap-distance", value_kind::integer, k_c, k_live, 1, 64, nullptr, 0, 0,
     LX_FIELD(windows.edge_snap_distance)},
    {"windows", "edge-snap-corner", value_kind::integer, k_c, k_live, 0, 512, nullptr, 0, 0,
     LX_FIELD(windows.edge_snap_corner)},
    {"windows", "snap-preview", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(windows.snap_preview)},
    {"windows", "drag-restore", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(windows.drag_restore)},
    // [workspaces]
    {"workspaces", "mode", value_kind::enumeration, k_c, k_live, 0, 0, k_workspace_modes,
     count_of(k_workspace_modes), 0, LX_FIELD(workspaces.mode)},
    {"workspaces", "count", value_kind::integer, k_c, k_live, 1, 16, nullptr, 0, 0,
     LX_FIELD(workspaces.count)},
    {"workspaces", "names", value_kind::string_list, k_c, k_live, 0, 16, nullptr, 0, 32,
     LX_FIELD(workspaces)},
    {"workspaces", "switch-on-activate", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(workspaces.switch_on_activate)},
    {"workspaces", "move-follows", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(workspaces.move_follows)},
    // [switcher]
    {"switcher", "scope", value_kind::enumeration, k_s, reload_class::next_open, 0, 0,
     k_switcher_scopes, count_of(k_switcher_scopes), 0, LX_FIELD(switcher.scope)},
    {"switcher", "include-minimized", value_kind::boolean, k_s, reload_class::next_open, 0, 0,
     nullptr, 0, 0, LX_FIELD(switcher.include_minimized)},
    {"switcher", "open-delay", value_kind::integer, k_s, reload_class::next_open, 0, 2000,
     nullptr, 0, 0, LX_FIELD(switcher.open_delay)},
    // [idle]
    {"idle", "policy", value_kind::enumeration, k_s, k_live, 0, 0, k_idle_policies,
     count_of(k_idle_policies), 0, LX_FIELD(idle.policy)},
    {"idle", "blank-after", value_kind::integer, k_s, k_live, 0, 86400, nullptr, 0, 0,
     LX_FIELD(idle.blank_after)},
    {"idle", "blank-locked-after", value_kind::integer, k_s, k_live, 0, 86400, nullptr, 0, 0,
     LX_FIELD(idle.blank_locked_after)},
    {"idle", "lock-command", value_kind::string, k_s, k_live, 0, 0, nullptr, 0, 512,
     LX_FIELD(idle.lock_command)},
    {"idle", "lock-on-blank", value_kind::boolean, k_s, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(idle.lock_on_blank)},
    // [background]
    {"background", "spec", value_kind::string, k_s, k_live, 0, 0, nullptr, 0, 512,
     LX_FIELD(background.spec)},
    // [notifications]
    {"notifications", "position", value_kind::enumeration, k_s, k_live, 0, 0, k_positions,
     count_of(k_positions), 0, LX_FIELD(notifications.position)},
    {"notifications", "timeout", value_kind::integer, k_s, k_live, 1, 3600, nullptr, 0, 0,
     LX_FIELD(notifications.timeout)},
    {"notifications", "timeout-low", value_kind::integer, k_s, k_live, 1, 3600, nullptr, 0, 0,
     LX_FIELD(notifications.timeout_low)},
    {"notifications", "critical-timeout", value_kind::integer, k_s, k_live, 0, 3600, nullptr, 0,
     0, LX_FIELD(notifications.critical_timeout)},
    {"notifications", "max-visible", value_kind::integer, k_s, k_live, 1, 10, nullptr, 0, 0,
     LX_FIELD(notifications.max_visible)},
    {"notifications", "dnd-while-sharing", value_kind::boolean, k_s, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(notifications.dnd_while_sharing)},
    {"notifications", "history", value_kind::integer, k_s, k_live, 0, 500, nullptr, 0, 0,
     LX_FIELD(notifications.history)},
    // [compositor]
    {"compositor", "realtime", value_kind::enumeration, k_c, reload_class::start, 0, 0,
     k_realtime_modes, count_of(k_realtime_modes), 0, LX_FIELD(compositor.realtime)},
    // [general]
    {"general", "default-bindings", value_kind::boolean, k_c, k_live, 0, 0, nullptr, 0, 0,
     LX_FIELD(default_bindings)},
};

const pointer_key_desc k_pointer_keys[] = {
    {"tap", value_kind::boolean, pf_tap, 0, 0, nullptr, 0, LX_PFIELD(tap)},
    {"tap-button-map", value_kind::enumeration, pf_tap_button_map, 0, 0, k_tap_maps,
     count_of(k_tap_maps), LX_PFIELD(tap_button_map)},
    {"drag", value_kind::boolean, pf_drag, 0, 0, nullptr, 0, LX_PFIELD(drag)},
    {"drag-lock", value_kind::boolean, pf_drag_lock, 0, 0, nullptr, 0, LX_PFIELD(drag_lock)},
    {"three-finger-drag", value_kind::boolean, pf_three_finger_drag, 0, 0, nullptr, 0,
     LX_PFIELD(three_finger_drag)},
    {"natural-scroll", value_kind::boolean, pf_natural_scroll, 0, 0, nullptr, 0,
     LX_PFIELD(natural_scroll)},
    {"scroll-method", value_kind::enumeration, pf_scroll_method, 0, 0, k_scroll_methods,
     count_of(k_scroll_methods), LX_PFIELD(scroll)},
    {"click-method", value_kind::enumeration, pf_click_method, 0, 0, k_click_methods,
     count_of(k_click_methods), LX_PFIELD(click)},
    {"accel-profile", value_kind::enumeration, pf_accel_profile, 0, 0, k_accel_profiles,
     count_of(k_accel_profiles), LX_PFIELD(accel)},
    {"accel-speed", value_kind::number, pf_accel_speed, -1.0, 1.0, nullptr, 0,
     LX_PFIELD(accel_speed)},
    {"left-handed", value_kind::boolean, pf_left_handed, 0, 0, nullptr, 0, LX_PFIELD(left_handed)},
    {"disable-while-typing", value_kind::boolean, pf_disable_while_typing, 0, 0, nullptr, 0,
     LX_PFIELD(disable_while_typing)},
    {"middle-emulation", value_kind::boolean, pf_middle_emulation, 0, 0, nullptr, 0,
     LX_PFIELD(middle_emulation)},
    {"scroll-factor", value_kind::number, pf_scroll_factor, 0.05, 20.0, nullptr, 0,
     LX_PFIELD(scroll_factor)},
};

#undef LX_FIELD
#undef LX_PFIELD

constexpr const char* k_sections[] = {
    "input",      "input.keyboard", "input.touchpad", "input.mouse",   "input.trackpoint",
    "input.device", "cursor",       "focus",          "windows",       "workspaces",
    "switcher",   "idle",           "background",     "notifications", "compositor",
    "general",    "bindings",
};

using enum lx::config::action_id;

// Only actions this build executes have a default binding: an unbound key keeps reaching
// applications.
constexpr default_binding k_default_bindings[] = {
    {"Super", shell_launcher, 0, 0},
    {"Super+Return", shell_terminal, 0, 0},
    {"Super+L", shell_lock, 0, bo_allow_when_inhibited},
    {"Alt+F4", window_close, 0, 0},
    {"Super+H", window_minimize, 0, 0},
    {"Super+Shift+Left", window_move_to_output_left, 0, 0},
    {"Super+Shift+Right", window_move_to_output_right, 0, 0},
    {"Super+BTN_LEFT", window_move_interactive, 0, 0},
    {"Super+BTN_RIGHT", window_resize_interactive, 0, 0},
    // Workspaces (design-session-and-wm C.3): GNOME's Super+Page_Up/Down and the
    // Ctrl+Alt-free Super+Ctrl+Left/Right, sway's and niri's Super+N.
    {"Super+1", workspace_switch, 1, 0},
    {"Super+2", workspace_switch, 2, 0},
    {"Super+3", workspace_switch, 3, 0},
    {"Super+4", workspace_switch, 4, 0},
    {"Super+5", workspace_switch, 5, 0},
    {"Super+6", workspace_switch, 6, 0},
    {"Super+7", workspace_switch, 7, 0},
    {"Super+8", workspace_switch, 8, 0},
    {"Super+9", workspace_switch, 9, 0},
    {"Super+Shift+1", workspace_move_window, 1, 0},
    {"Super+Shift+2", workspace_move_window, 2, 0},
    {"Super+Shift+3", workspace_move_window, 3, 0},
    {"Super+Shift+4", workspace_move_window, 4, 0},
    {"Super+Shift+5", workspace_move_window, 5, 0},
    {"Super+Shift+6", workspace_move_window, 6, 0},
    {"Super+Shift+7", workspace_move_window, 7, 0},
    {"Super+Shift+8", workspace_move_window, 8, 0},
    {"Super+Shift+9", workspace_move_window, 9, 0},
    {"Super+Page_Down", workspace_next, 0, 0},
    {"Super+Ctrl+Right", workspace_next, 0, 0},
    {"Super+Page_Up", workspace_previous, 0, 0},
    {"Super+Ctrl+Left", workspace_previous, 0, 0},
    {"Super+Shift+Page_Down", workspace_move_window_next, 0, 0},
    {"Super+Ctrl+Shift+Right", workspace_move_window_next, 0, 0},
    {"Super+Shift+Page_Up", workspace_move_window_previous, 0, 0},
    {"Super+Ctrl+Shift+Left", workspace_move_window_previous, 0, 0},
    // The window switcher (design-session-and-wm K.1, K.2): held while the modifier is down.
    // GNOME's and KDE's Alt+Tab and Super+Tab, Alt+grave for the windows of one application.
    {"Alt+Tab", switcher_next, 0, 0},
    {"Super+Tab", switcher_next, 0, 0},
    {"Alt+Shift+Tab", switcher_previous, 0, 0},
    {"Super+Shift+Tab", switcher_previous, 0, 0},
    {"Alt+grave", switcher_next_app, 0, 0},
    {"Alt+Shift+grave", switcher_previous_app, 0, 0},
    // Snapping (K.3, Windows 11's Super+arrows) and keyboard move/resize (K.6, GNOME's and
    // KDE's Alt+F7/Alt+F8).
    {"Super+Left", window_snap_left, 0, 0},
    {"Super+Right", window_snap_right, 0, 0},
    {"Super+Up", window_snap_up, 0, 0},
    {"Super+Down", window_snap_down, 0, 0},
    {"Alt+F7", window_move_keyboard, 0, 0},
    {"Alt+F8", window_resize_keyboard, 0, 0},
};

} // namespace

lx::config::config_snapshot lx::config::default_config() {
    config_snapshot c{};
    // Touchpads: tap to click, natural scrolling and disable-while-typing on (GNOME's and
    // KDE's defaults, and most distributions' sway; libinput's own default leaves tap off).
    c.touchpad.tap = true;
    c.touchpad.natural_scroll = true;
    c.touchpad.disable_while_typing = true;
    c.touchpad.drag = true;
    for (const default_binding& d : k_default_bindings) {
        binding b{};
        const char* error = nullptr;
        if (!parse_combo(d.keys, b.keys, error))
            continue;
        b.action = d.action;
        b.argument = d.argument;
        b.options = d.options;
        b.layer = ~0u;
        c.bindings.push_back(b);
    }
    return c;
}

lx::config::pointer_settings lx::config::device_settings(const config_snapshot& c, unsigned kind,
                                                         std::string_view device_name) {
    pointer_settings s = kind == 0 ? c.touchpad : kind == 1 ? c.mouse : c.trackpoint;
    for (const device_override& d : c.devices) {
        if (device_name != d.name)
            continue;
        pointer_settings& to = s;
        const pointer_settings& from = d.settings;
        for (const pointer_key_desc& k : k_pointer_keys) {
            if (!(from.set & k.bit))
                continue;
            void* dst = k.field(to);
            void* src = k.field(const_cast<pointer_settings&>(from));
            switch (k.kind) {
            case value_kind::boolean: *static_cast<bool*>(dst) = *static_cast<bool*>(src); break;
            case value_kind::number:
                *static_cast<double*>(dst) = *static_cast<double*>(src);
                break;
            case value_kind::enumeration:
                *static_cast<unsigned char*>(dst) = *static_cast<unsigned char*>(src);
                break;
            default: break;
            }
        }
        s.set |= from.set;
    }
    return s;
}

std::span<const lx::config::key_desc> lx::config::schema_keys() { return k_keys; }
std::span<const lx::config::pointer_key_desc> lx::config::pointer_keys() { return k_pointer_keys; }
std::span<const char* const> lx::config::schema_sections() { return k_sections; }
std::span<const lx::config::default_binding> lx::config::default_bindings() {
    return k_default_bindings;
}

const char* lx::config::enum_word(const char* const* names, unsigned count, unsigned value) {
    return names && value < count ? names[value] : "?";
}
