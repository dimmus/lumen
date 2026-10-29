module;

// The compositor's configuration state (`:config_apply`). See the partition.

module lx.compositor;

import std;

import lx.config;
import lx.input;
import :keybindings;
import :config_apply;

namespace lx::compositor {

namespace {

namespace cfg = lx::config;

bool same_pointer(const cfg::pointer_settings& a, const cfg::pointer_settings& b) {
    return a.tap == b.tap && a.tap_button_map == b.tap_button_map && a.drag == b.drag &&
           a.drag_lock == b.drag_lock && a.three_finger_drag == b.three_finger_drag &&
           a.natural_scroll == b.natural_scroll && a.scroll == b.scroll && a.click == b.click &&
           a.accel == b.accel && a.accel_speed == b.accel_speed && a.left_handed == b.left_handed &&
           a.disable_while_typing == b.disable_while_typing &&
           a.middle_emulation == b.middle_emulation && a.scroll_factor == b.scroll_factor &&
           a.set == b.set;
}

bool same_bindings(const cfg::config_snapshot& a, const cfg::config_snapshot& b) {
    if (a.bindings.size() != b.bindings.size())
        return false;
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        const auto& x = a.bindings[i];
        const auto& y = b.bindings[i];
        if (!(x.keys == y.keys) || x.action != y.action || x.argument != y.argument ||
            x.options != y.options)
            return false;
    }
    return true;
}

} // namespace

unsigned diff_config(const cfg::config_snapshot& a, const cfg::config_snapshot& b) {
    unsigned c = 0;
    const auto& ka = a.keyboard;
    const auto& kb = b.keyboard;
    if (std::strcmp(ka.layout, kb.layout) || std::strcmp(ka.variant, kb.variant) ||
        std::strcmp(ka.options, kb.options) || std::strcmp(ka.model, kb.model) ||
        std::strcmp(ka.rules, kb.rules))
        c |= cfg_keymap;
    if (ka.repeat_rate != kb.repeat_rate || ka.repeat_delay != kb.repeat_delay)
        c |= cfg_repeat;
    bool pointer = !same_pointer(a.touchpad, b.touchpad) || !same_pointer(a.mouse, b.mouse) ||
                   !same_pointer(a.trackpoint, b.trackpoint) ||
                   a.devices.size() != b.devices.size();
    for (std::size_t i = 0; !pointer && i < a.devices.size(); ++i)
        pointer = std::strcmp(a.devices[i].name, b.devices[i].name) != 0 ||
                  !same_pointer(a.devices[i].settings, b.devices[i].settings);
    if (pointer)
        c |= cfg_pointer;
    if (std::strcmp(a.cursor.theme, b.cursor.theme) || a.cursor.size != b.cursor.size)
        c |= cfg_cursor;
    if (a.focus.mode != b.focus.mode || a.focus.raise_on_click != b.focus.raise_on_click ||
        a.focus.raise_on_sloppy != b.focus.raise_on_sloppy)
        c |= cfg_focus;
    if (!same_bindings(a, b) || a.default_bindings != b.default_bindings ||
        (a.idle.lock_command[0] != 0) != (b.idle.lock_command[0] != 0))
        c |= cfg_bindings;
    if (std::memcmp(&a.windows, &b.windows, sizeof(a.windows)) != 0 ||
        a.tiling.layout != b.tiling.layout || a.tiling.master_ratio != b.tiling.master_ratio ||
        a.tiling.master_count != b.tiling.master_count || a.tiling.gaps != b.tiling.gaps ||
        a.animations.enabled != b.animations.enabled ||
        a.animations.stiffness != b.animations.stiffness ||
        a.animations.damping_ratio != b.animations.damping_ratio)
        c |= cfg_windows;
    if (a.workspaces.mode != b.workspaces.mode || a.workspaces.count != b.workspaces.count ||
        a.workspaces.name_count != b.workspaces.name_count ||
        std::memcmp(a.workspaces.names, b.workspaces.names, sizeof(a.workspaces.names)) != 0 ||
        a.workspaces.switch_on_activate != b.workspaces.switch_on_activate ||
        a.workspaces.move_follows != b.workspaces.move_follows)
        c |= cfg_workspaces;
    if (a.idle.policy != b.idle.policy || a.idle.blank_after != b.idle.blank_after ||
        a.idle.blank_locked_after != b.idle.blank_locked_after ||
        std::strcmp(a.idle.lock_command, b.idle.lock_command) ||
        a.idle.lock_on_blank != b.idle.lock_on_blank ||
        std::strcmp(a.background.spec, b.background.spec) ||
        std::memcmp(&a.switcher, &b.switcher, sizeof(a.switcher)) != 0 ||
        std::memcmp(&a.notifications, &b.notifications, sizeof(a.notifications)) != 0)
        c |= cfg_shell;
    return c;
}

config_state::config_state() : current_{std::make_unique<cfg::config_snapshot>(cfg::default_config())} {}
config_state::~config_state() = default;

void config_state::init(cfg::env_lookup env) {
    location_ = cfg::locate_config(env);
    if (location_.disabled)
        std::snprintf(source_, sizeof(source_), "(none: LUMEN_CONFIG=0)");
    else if (const char* home = std::getenv("HOME");
             home && *home && std::strncmp(location_.user_file.path, home, std::strlen(home)) == 0 &&
             location_.user_file.path[std::strlen(home)] == '/')
        std::snprintf(source_, sizeof(source_), "~%s", location_.user_file.path + std::strlen(home));
    else
        std::snprintf(source_, sizeof(source_), "%s", location_.user_file.path);
    if (!location_.disabled)
        std::snprintf(defaults_, sizeof(defaults_), "built-in defaults (no %s)", source_);
    (void)watch_.open(location_);
}

const char* config_state::describe_source() const {
    // What this generation actually read: the user's file, another layer (a system file
    // with no user file), or nothing at all, which is the compiled-in defaults.
    if (location_.disabled)
        return source_;
    if (read_.layers.empty())
        return defaults_;
    for (const auto& l : read_.layers)
        if (std::strcmp(l.path, location_.user_file.path) == 0)
            return source_;
    return read_.layers.back().path;
}

const char* config_state::path_of(unsigned layer) const { return read_.path_of(layer); }

config_state::result config_state::reload(bool force) {
    result r{};
    cfg::config_text text{};
    cfg::diagnostics diag{};
    const bool readable = cfg::read_config(location_, text, diag);
    if (loaded_ && !force && readable && text.bytes == read_.bytes)
        return r; // nothing changed on disk (an editor's backup file, a touch)
    loaded_ = true;
    ++generation_;
    read_ = std::move(text);
    if (!readable) {
        diag_ = diag;
        accepted_last_ = false;
        r.what = outcome::rejected;
        return r;
    }
    auto next = std::make_unique<cfg::config_snapshot>(*current_);
    if (!cfg::parse_config(read_, *next, diag, current_.get())) {
        diag_ = diag;
        accepted_last_ = false;
        r.what = outcome::rejected;
        return r;
    }
    diag_ = diag;
    accepted_last_ = true;
    r.what = outcome::accepted;
    r.changes = diff_config(*current_, *next);
    r.bytes_changed = read_.bytes != accepted_.bytes;
    accepted_ = read_;
    current_ = std::move(next);
    return r;
}

void binding_table(const cfg::config_snapshot& c,
                   std::inplace_vector<binding_entry, keybindings::k_max_bindings>& out) {
    out.clear();
    const bool can_lock = c.idle.lock_command[0] != 0;
    for (const cfg::binding& b : c.bindings) {
        if (b.action == cfg::action_id::shell_lock && !can_lock)
            continue;
        if (b.action == cfg::action_id::none || !cfg::action(b.action).available)
            continue;
        if (out.size() == out.capacity())
            break;
        out.push_back({b.keys, b.action, b.argument, b.options});
    }
}

lx::input::pointer_config pointer_config_of(const cfg::config_snapshot& c, const char* name,
                                            lx::input::device_class kind) {
    const unsigned k = kind == lx::input::device_class::touchpad     ? 0u
                       : kind == lx::input::device_class::trackpoint ? 2u
                                                                     : 1u;
    const cfg::pointer_settings s = cfg::device_settings(c, k, name ? name : "");
    lx::input::pointer_config p{};
    p.tap = s.tap;
    p.tap_button_map = static_cast<unsigned>(s.tap_button_map);
    p.drag = s.drag;
    p.drag_lock = s.drag_lock;
    p.three_finger_drag = s.three_finger_drag;
    p.natural_scroll = s.natural_scroll;
    p.scroll_method = static_cast<unsigned>(s.scroll);
    p.click_method = static_cast<unsigned>(s.click);
    p.accel_profile = static_cast<unsigned>(s.accel);
    p.accel_speed = s.accel_speed;
    p.left_handed = s.left_handed;
    p.disable_while_typing = s.disable_while_typing;
    p.middle_emulation = s.middle_emulation;
    p.scroll_factor = s.scroll_factor;
    return p;
}

lx::input::keymap_config keymap_config_of(const cfg::keyboard_settings& k) {
    lx::input::keymap_config m{};
    m.rules = k.rules[0] ? k.rules : nullptr;
    m.model = k.model[0] ? k.model : nullptr;
    m.layout = k.layout[0] ? k.layout : nullptr;
    m.variant = k.variant[0] ? k.variant : nullptr;
    m.options = k.options[0] ? k.options : nullptr;
    return m;
}

} // namespace lx::compositor
