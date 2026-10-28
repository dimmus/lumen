// The configuration file (lx.config, design-session-and-wm Part C): inline tables in the
// TOML reader; layers and their marker lines; every schema key's type, range and choices;
// defaults without a file; a syntax error rejects everything, a bad value keeps the previous
// value, an unknown key warns with a suggestion; the bindings grammar (modifiers, keysyms,
// buttons, taps, options, `none`, reserved, duplicates, held without a modifier, actions not
// available yet); diagnostics mapped to their file and line; the locations (XDG layers,
// LUMEN_CONFIG); the inotify watch following an editor's rename and a directory that appears;
// and the documented default file (deploy/lumen/config.toml) parsing to exactly the defaults.
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.config;

namespace {

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

namespace cfg = lx::config;
namespace toml = lx::config::toml;

/// One layer from `text`, parsed.
struct parsed {
    cfg::config_snapshot c{};
    cfg::diagnostics d{};
    bool ok = false;
};

parsed parse_one(const char* text, const cfg::config_snapshot* previous = nullptr) {
    parsed p{};
    p.c = cfg::default_config();
    cfg::config_text t{};
    (void)t.add_layer("/home/u/.config/lumen/config.toml", text);
    p.ok = cfg::parse_config(t, p.c, p.d, previous);
    return p;
}

bool has_diag(const cfg::diagnostics& d, const char* key_part, const char* message_part,
              unsigned line = 0) {
    for (const auto& x : d.items()) {
        if (key_part && !std::strstr(x.key, key_part))
            continue;
        if (message_part && !std::strstr(x.message, message_part))
            continue;
        if (line && x.line != line)
            continue;
        return true;
    }
    return false;
}

void dump(const cfg::diagnostics& d) {
    for (const auto& x : d.items())
        std::printf("      [%s] layer %u line %u %s: %s\n", cfg::severity_name(x.sev), x.layer,
                    x.line, x.key, x.message);
}

const cfg::binding* find_binding(const cfg::config_snapshot& c, const char* keys) {
    cfg::combo k{};
    const char* error = nullptr;
    if (!cfg::parse_combo(keys, k, error))
        return nullptr;
    for (const auto& b : c.bindings)
        if (b.keys == k)
            return &b;
    return nullptr;
}

// ── TOML inline tables ──

void test_inline_tables() {
    struct log {
        std::string s;
    } l;
    toml::handler h{};
    h.user = &l;
    h.table = [](void* u, const char* name, bool, toml::parse_error&) {
        static_cast<log*>(u)->s += std::string("[") + name + "]";
        return true;
    };
    h.key_value = [](void* u, const char* key, const toml::value& v, toml::parse_error&) {
        auto& s = static_cast<log*>(u)->s;
        s += key;
        s += "=";
        if (v.type == toml::value::kind::string)
            s += v.str;
        else if (v.type == toml::value::kind::boolean)
            s += v.boolean ? "true" : "false";
        else if (v.type == toml::value::kind::integer)
            s += std::to_string(v.integer);
        else if (v.type == toml::value::kind::array)
            s += "[" + std::to_string(v.count) + "]";
        s += ";";
        return true;
    };
    h.inline_table = [](void* u, const char* key, bool begin, toml::parse_error&) {
        static_cast<log*>(u)->s += begin ? std::string("{") + key + ":" : std::string("}");
        return true;
    };
    toml::parse_error err{};
    const char* text = "[bindings]\n\"Super+L\" = { action = \"shell.lock\", repeat = true, "
                       "argument = 3 }\nx = {}\ny = { a = [1, 2] } # c\n";
    const bool ok = toml::parse(text, std::strlen(text), h, err);
    check(ok, "an inline table parses");
    check(l.s == "[bindings]{Super+L:action=shell.lock;repeat=true;argument=3;}{x:}{y:a=[2];}",
          "an inline table is reported as begin, members, end");
    if (l.s != "[bindings]{Super+L:action=shell.lock;repeat=true;argument=3;}{x:}{y:a=[2];}")
        std::printf("      got %s\n", l.s.c_str());

    const auto fails_at = [&](const char* t, unsigned line, bool with_inline = true) {
        toml::handler hh = h;
        if (!with_inline)
            hh.inline_table = nullptr;
        toml::parse_error e{};
        return !toml::parse(t, std::strlen(t), hh, e) && e.line == line;
    };
    check(fails_at("a = 1\nb = { c = { d = 1 } }\n", 2), "a nested inline table is refused");
    check(fails_at("b = { c = 1,\n d = 2 }\n", 1), "an inline table must end on its line");
    check(fails_at("b = { c = 1, }\n", 1), "a trailing comma in an inline table is refused");
    check(fails_at("b = { c = 1 } x\n", 1), "text after an inline table is refused");
    check(fails_at("b = [ { c = 1 } ]\n", 1), "an inline table in an array is refused");
    check(fails_at("a = 1\nb = {x = 1}\n", 2, false),
          "without an inline_table handler, inline tables stay an error (outputs.toml)");
}

// ── Defaults, documented file ──

void test_defaults() {
    const cfg::config_snapshot d = cfg::default_config();
    check(d.keyboard.repeat_rate == 25 && d.keyboard.repeat_delay == 600, "repeat 25/600");
    check(!d.keyboard.layout[0] && !d.keyboard.numlock, "keyboard: system layout, numlock off");
    check(d.touchpad.tap && d.touchpad.natural_scroll && d.touchpad.disable_while_typing,
          "touchpad: tap, natural scroll, dwt on");
    check(!d.mouse.natural_scroll && d.mouse.accel == cfg::accel_profile::device_default,
          "mouse: natural scroll off, device's accel profile");
    check(d.cursor.size == 0 && !d.cursor.theme[0], "cursor from the environment");
    check(d.focus.mode == cfg::focus_mode::click && d.focus.raise_on_click, "click to focus");
    check(d.idle.policy == cfg::idle_policy_kind::shell && d.idle.blank_after == 600 &&
              d.idle.blank_locked_after == 60 && d.idle.lock_on_blank && !d.idle.lock_command[0],
          "idle defaults (as lumen-shell's flags)");
    check(std::strcmp(d.background.spec, "lumen") == 0, "background lumen");
    check(d.bindings.size() == cfg::default_bindings().size(), "every default binding resolves");
    const auto* tap = find_binding(d, "Super");
    check(tap && tap->action == cfg::action_id::shell_launcher &&
              tap->keys.kind == cfg::combo_kind::tap,
          "Super tap opens the launcher");
    const auto* lock = find_binding(d, "Super+L");
    check(lock && lock->action == cfg::action_id::shell_lock &&
              (lock->options & cfg::bo_allow_when_inhibited),
          "Super+L locks, also while inhibited");
    const auto* move = find_binding(d, "Super+BTN_LEFT");
    check(move && move->action == cfg::action_id::window_move_interactive &&
              move->keys.kind == cfg::combo_kind::button && move->keys.code == 0x110,
          "Super+BTN_LEFT moves");
    const auto* ws9 = find_binding(d, "Super+9");
    const auto* mv2 = find_binding(d, "Super+Shift+2");
    check(ws9 && ws9->action == cfg::action_id::workspace_switch && ws9->argument == 9 && mv2 &&
              mv2->action == cfg::action_id::workspace_move_window && mv2->argument == 2,
          "Super+N switches to workspace N, Super+Shift+N moves the window there");
    const auto* next = find_binding(d, "Super+Page_Down");
    const auto* prev = find_binding(d, "Super+Ctrl+Left");
    check(next && next->action == cfg::action_id::workspace_next && prev &&
              prev->action == cfg::action_id::workspace_previous,
          "Super+Page_Down / Super+Ctrl+Left: next / previous workspace");
    const auto* hide = find_binding(d, "Super+H");
    check(hide && hide->action == cfg::action_id::window_minimize, "Super+H minimizes");
    // Part 4: the switcher (held), snapping, keyboard move and resize.
    const auto* alt_tab = find_binding(d, "Alt+Tab");
    const auto* alt_shift_tab = find_binding(d, "Alt+Shift+Tab");
    const auto* grave = find_binding(d, "Alt+grave");
    check(alt_tab && alt_tab->action == cfg::action_id::switcher_next && alt_shift_tab &&
              alt_shift_tab->action == cfg::action_id::switcher_previous && grave &&
              grave->action == cfg::action_id::switcher_next_app &&
              cfg::action(alt_tab->action).held,
          "Alt+Tab / Alt+Shift+Tab / Alt+grave: the switcher, held");
    const auto* super_tab = find_binding(d, "Super+Tab");
    check(super_tab && super_tab->action == cfg::action_id::switcher_next, "Super+Tab switches too");
    const auto* left = find_binding(d, "Super+Left");
    const auto* up = find_binding(d, "Super+Up");
    const auto* down = find_binding(d, "Super+Down");
    check(left && left->action == cfg::action_id::window_snap_left && up &&
              up->action == cfg::action_id::window_snap_up && down &&
              down->action == cfg::action_id::window_snap_down,
          "Super+arrows snap");
    // ADR 012: tiling is off by default; its keys are bound.
    const auto* super_t = find_binding(d, "Super+T");
    const auto* float_key = find_binding(d, "Super+Shift+space");
    const auto* super_j = find_binding(d, "Super+J");
    check(d.tiling.layout == cfg::tiling_layout::floating && d.tiling.master_ratio == 0.55 &&
              d.tiling.master_count == 1 && d.tiling.gaps == 8 && super_t &&
              super_t->action == cfg::action_id::layout_toggle && float_key &&
              float_key->action == cfg::action_id::window_float_toggle && super_j &&
              super_j->action == cfg::action_id::layout_focus_next &&
              cfg::find_action("layout.promote") &&
              cfg::find_action("layout.promote")->owner == cfg::action_owner::compositor,
          "[tiling]: floating by default; Super+T, Super+Shift+space, Super+J bound");
    const auto* f7 = find_binding(d, "Alt+F7");
    const auto* f8 = find_binding(d, "Alt+F8");
    check(f7 && f7->action == cfg::action_id::window_move_keyboard && f8 &&
              f8->action == cfg::action_id::window_resize_keyboard,
          "Alt+F7 / Alt+F8: keyboard move and resize");
    for (const auto& b : d.bindings)
        check(cfg::action(b.action).available, "a default binding's action is available");

    // An empty configuration is the defaults, with no diagnostics.
    const parsed empty = parse_one("");
    check(empty.ok && empty.d.empty() && empty.c.bindings.size() == d.bindings.size(),
          "an empty file: the defaults, nothing to report");
}

void test_documented_file() {
    std::string text;
    const char* why = nullptr;
    bool missing = false;
    const bool read = cfg::read_file(LUMEN_SOURCE_DIR "/deploy/lumen/config.toml", text, why,
                                     missing);
    check(read, "deploy/lumen/config.toml reads");
    const parsed p = parse_one(text.c_str());
    check(p.ok && p.d.empty(), "the documented file parses with no diagnostic");
    dump(p.d);
    const cfg::config_snapshot d = cfg::default_config();
    // Every schema key has its default.
    bool same = true;
    for (const auto& k : cfg::schema_keys()) {
        char a[128];
        char b[128];
        cfg::format_value(k, p.c, a, sizeof(a));
        cfg::format_value(k, d, b, sizeof(b));
        if (std::strcmp(a, b) != 0) {
            same = false;
            std::printf("      %s.%s: file %s, default %s\n", k.section, k.key, a, b);
        }
    }
    check(same, "every key in the documented file is its default");
    for (const auto* sec : {&p.c.touchpad, &p.c.mouse, &p.c.trackpoint}) {
        const auto* def = sec == &p.c.touchpad ? &d.touchpad : sec == &p.c.mouse ? &d.mouse : &d.trackpoint;
        check(sec->tap == def->tap && sec->natural_scroll == def->natural_scroll &&
                  sec->accel == def->accel && sec->accel_speed == def->accel_speed &&
                  sec->scroll_factor == def->scroll_factor,
              "pointer sections are their defaults");
    }
    bool bindings_same = p.c.bindings.size() == d.bindings.size();
    for (const auto& b : d.bindings) {
        const auto it = std::ranges::find_if(p.c.bindings, [&](const cfg::binding& x) {
            return x.keys == b.keys && x.action == b.action && x.options == b.options;
        });
        bindings_same = bindings_same && it != p.c.bindings.end();
    }
    check(bindings_same, "the documented [bindings] are exactly the default bindings");
    // Every schema key appears in the documented file.
    bool all_documented = true;
    for (const auto& k : cfg::schema_keys()) {
        char needle[96];
        std::snprintf(needle, sizeof(needle), "\n%s = ", k.key);
        if (!std::strstr(text.c_str(), needle)) {
            all_documented = false;
            std::printf("      %s.%s is not in the documented file\n", k.section, k.key);
        }
    }
    check(all_documented, "the documented file names every key");
}

// ── Values ──

void test_values() {
    const parsed p = parse_one(R"([input.keyboard]
layout = "us,ru"
options = "grp:alt_shift_toggle"
repeat-rate = 40
repeat-delay = 250
numlock = true
[input.touchpad]
tap = false
accel-profile = "flat"
accel-speed = -0.5
scroll-method = "edge"
tap-button-map = "lmr"
[input.mouse]
natural-scroll = true
accel-speed = 1
scroll-factor = 2.5
[cursor]
theme = "Adwaita"
size = 32
[focus]
mode = "sloppy"
raise-on-sloppy = true
[workspaces]
mode = "static"
count = 6
names = ["web", "mail"]
[idle]
policy = "none"
blank-after = 0
lock-command = "swaylock -f"
[background]
spec = "#203040"
[notifications]
position = "bottom-left"
[tiling]
layout = "master-stack"
master-ratio = 0.6
master-count = 2
gaps = 0
)");
    check(p.ok && p.d.empty(), "valid values: no diagnostic");
    dump(p.d);
    check(std::strcmp(p.c.keyboard.layout, "us,ru") == 0 &&
              std::strcmp(p.c.keyboard.options, "grp:alt_shift_toggle") == 0,
          "keyboard strings");
    check(p.c.keyboard.repeat_rate == 40 && p.c.keyboard.repeat_delay == 250 && p.c.keyboard.numlock,
          "keyboard numbers and numlock");
    check(!p.c.touchpad.tap && p.c.touchpad.accel == cfg::accel_profile::flat &&
              p.c.touchpad.accel_speed == -0.5 && p.c.touchpad.scroll == cfg::scroll_method::edge &&
              p.c.touchpad.tap_button_map == cfg::tap_map::lmr,
          "touchpad values");
    check((p.c.touchpad.set & cfg::pf_tap) && (p.c.touchpad.set & cfg::pf_accel_speed) &&
              !(p.c.touchpad.set & cfg::pf_drag),
          "what a section set is recorded");
    check(p.c.mouse.natural_scroll && p.c.mouse.accel_speed == 1.0 && p.c.mouse.scroll_factor == 2.5,
          "an integer is a number too (accel-speed = 1)");
    check(std::strcmp(p.c.cursor.theme, "Adwaita") == 0 && p.c.cursor.size == 32, "cursor");
    check(p.c.focus.mode == cfg::focus_mode::sloppy && p.c.focus.raise_on_sloppy, "focus");
    check(p.c.workspaces.mode == cfg::workspace_mode::fixed && p.c.workspaces.count == 6 &&
              p.c.workspaces.name_count == 2 && std::strcmp(p.c.workspaces.names[1], "mail") == 0,
          "workspaces: static, count, names");
    check(p.c.idle.policy == cfg::idle_policy_kind::none && p.c.idle.blank_after == 0 &&
              std::strcmp(p.c.idle.lock_command, "swaylock -f") == 0,
          "idle");
    check(p.c.notifications.position == cfg::notification_position::bottom_left, "an enum word");
    check(std::strcmp(p.c.background.spec, "#203040") == 0, "background spec");
    check(p.c.tiling.layout == cfg::tiling_layout::master_stack && p.c.tiling.master_ratio == 0.6 &&
              p.c.tiling.master_count == 2 && p.c.tiling.gaps == 0,
          "tiling: layout, ratio, count, gaps");
}

void test_bad_values() {
    // Type, range, enum: each key alone is rejected and keeps its value; the rest applies.
    const parsed p = parse_one(R"([input.keyboard]
repeat-rate = 5000
repeat-delay = "fast"
layout = "de"
[input.touchpad]
accel-speed = 1.5
tap = "yes"
accel-profile = "adaptiv"
[cursor]
size = 1.5
[workspaces]
names = ["a", 2]
)");
    check(p.ok, "bad values do not reject the file");
    check(std::strcmp(p.c.keyboard.layout, "de") == 0, "the good key next to bad ones applies");
    check(p.c.keyboard.repeat_rate == 25 && has_diag(p.d, "input.keyboard.repeat-rate",
                                                     "5000 is outside 0..1000; keeping 25", 2),
          "out of range: error with its line, keeps the default");
    check(p.c.keyboard.repeat_delay == 600 &&
              has_diag(p.d, "repeat-delay", "must be a whole number (got \"fast\")"),
          "wrong type");
    check(p.c.touchpad.accel_speed == 0.0 &&
              has_diag(p.d, "input.touchpad.accel-speed", "1.5 is outside -1..1; keeping 0"),
          "accel-speed outside -1..1 keeps 0");
    check(p.c.touchpad.tap && has_diag(p.d, "input.touchpad.tap", "must be true or false"),
          "tap = \"yes\" is refused, tap stays on");
    check(has_diag(p.d, "accel-profile", "did you mean adaptive?"), "enum suggestion");
    check(has_diag(p.d, "cursor.size", "must be a whole number"), "a float for an integer");
    check(p.c.workspaces.name_count == 0 && has_diag(p.d, "workspaces.names", "array of strings"),
          "a list with a non-string");
    check(p.d.errors() == 7 && p.d.warnings() == 0, "seven errors, no warnings");
    dump(p.d);

    // With the configuration in use given, a bad value keeps *that* value.
    cfg::config_snapshot running = cfg::default_config();
    running.keyboard.repeat_rate = 33;
    running.touchpad.accel_speed = 0.25;
    const parsed q = parse_one("[input.keyboard]\nrepeat-rate = -1\n[input.touchpad]\naccel-speed = 9\n",
                               &running);
    check(q.c.keyboard.repeat_rate == 33 && has_diag(q.d, "repeat-rate", "keeping 33"),
          "a bad value keeps the previous configuration's value");
    check(q.c.touchpad.accel_speed == 0.25 && has_diag(q.d, "accel-speed", "keeping 0.25"),
          "a bad pointer value keeps the previous configuration's value");
}

void test_unknown() {
    const parsed p = parse_one(R"(version = 2
colour = 1
[input.touchpad]
tapp = true
[input.mouse]
tap = true
[cursr]
size = 3
[focus]
mod = "sloppy"
)");
    check(p.ok, "unknown keys do not reject the file");
    check(has_diag(p.d, "version", "newer Lumen"), "a newer version warns");
    check(has_diag(p.d, "input.touchpad.tapp", "did you mean tap?", 4), "did you mean tap?");
    check(has_diag(p.d, "input.mouse.tap", "not a mouse setting"), "tap is a touchpad's");
    check(has_diag(p.d, "cursr", "did you mean [cursor]?", 7), "an unknown section suggests one");
    check(!has_diag(p.d, "size", nullptr), "keys of an unknown section are not reported again");
    check(has_diag(p.d, "focus.mod", "did you mean mode?"), "did you mean mode?");
    check(p.d.errors() == 0 && p.d.warnings() == 6, "all warnings");
    dump(p.d);
}

void test_syntax_error() {
    cfg::config_snapshot out = cfg::default_config();
    out.keyboard.repeat_rate = 77; // what is in use
    cfg::config_text t{};
    (void)t.add_layer("/etc/xdg/lumen/config.toml", "[input.keyboard]\nrepeat-rate = 30\n");
    (void)t.add_layer("/home/u/.config/lumen/config.toml",
                      "[cursor]\nsize = 30\n[input.keyboard\nlayout = \"de\"\n");
    cfg::diagnostics d{};
    const bool ok = cfg::parse_config(t, out, d, nullptr);
    check(!ok, "a syntax error in any layer rejects the configuration");
    check(out.keyboard.repeat_rate == 77 && out.cursor.size == 0,
          "the snapshot is untouched (the previous configuration stays)");
    check(d.errors() == 1 && d.items().size() == 1 && d.items()[0].layer == 1 &&
              d.items()[0].line == 3 && std::strstr(d.items()[0].message, "expected ']'"),
          "the error names the layer and the file's own line");
    char line[256];
    cfg::format_diagnostic(d.items()[0], t.path_of(d.items()[0].layer), false, line, sizeof(line));
    check(std::strncmp(line, "config.toml:3: expected ']'", 27) == 0, "format: file:line: message");
}

void test_layers() {
    cfg::config_text t{};
    (void)t.add_layer("/etc/xdg/lumen/config.toml", R"([input.keyboard]
layout = "us,de"
repeat-rate = 30
[input.touchpad]
tap = false
[[input.device]]
name = "Mouse A"
accel-speed = -0.5
[bindings]
"Super+Y" = "window.close"
"Alt+F4" = "none"
)");
    (void)t.add_layer("/home/u/.config/lumen/config.toml", R"([input.keyboard]
repeat-rate = 10
[[input.device]]
name = "Mouse A"
left-handed = true
[[input.device]]
accel-speed = 0.2
[bindings]
"Super+Y" = "window.maximize-toggle"
"Super+Q" = { action = "window.close" }
)");
    cfg::config_snapshot c = cfg::default_config();
    cfg::diagnostics d{};
    const bool ok = cfg::parse_config(t, c, d, nullptr);
    check(ok, "two layers parse");
    check(std::strcmp(c.keyboard.layout, "us,de") == 0 && c.keyboard.repeat_rate == 10,
          "key by key: the user's file over the system's");
    check(!c.touchpad.tap, "a key only the system layer sets stays");
    check(c.devices.size() == 1 && c.devices[0].settings.accel_speed == -0.5 &&
              c.devices[0].settings.left_handed &&
              (c.devices[0].settings.set & (cfg::pf_accel_speed | cfg::pf_left_handed)) ==
                  (cfg::pf_accel_speed | cfg::pf_left_handed),
          "[[input.device]] merges by name across layers");
    check(has_diag(d, "input.device", "needs a name", 6) && d.items()[0].layer == 1,
          "a device without a name: error on the user's layer, its line");
    const auto* y = find_binding(c, "Super+Y");
    check(y && y->action == cfg::action_id::window_maximize_toggle && y->layer == 1 && y->line == 9,
          "a binding of the later layer replaces the earlier one (with its origin)");
    check(!find_binding(c, "Alt+F4"), "\"none\" removes a default binding");
    check(find_binding(c, "Super+Q") && find_binding(c, "Super+Return"),
          "other defaults stay; an inline-table binding");
    // Pointer settings of a device: class section, then its override.
    const cfg::pointer_settings mouse = cfg::device_settings(c, 1, "Mouse A");
    check(mouse.accel_speed == -0.5 && mouse.left_handed && !mouse.natural_scroll,
          "device_settings: the override over the class section");
    const cfg::pointer_settings other = cfg::device_settings(c, 1, "Mouse B");
    check(other.accel_speed == 0.0 && !other.left_handed, "another device: the class section");

    // The effective bytes split back into the same layers (what the shell does).
    const cfg::config_text back = cfg::split_layers(t.bytes);
    bool same = back.layers.size() == t.layers.size();
    for (std::size_t i = 0; same && i < back.layers.size(); ++i)
        same = std::strcmp(back.layers[i].path, t.layers[i].path) == 0 &&
               back.layers[i].begin == t.layers[i].begin;
    check(same, "split_layers recovers each layer's path and start");
    cfg::config_snapshot c2 = cfg::default_config();
    cfg::diagnostics d2{};
    check(cfg::parse_config(back, c2, d2, nullptr) && d2.same_as(d) &&
              c2.keyboard.repeat_rate == 10 && c2.bindings.size() == c.bindings.size(),
          "the split bytes parse to the same configuration and diagnostics");
    const cfg::config_text loose = cfg::split_layers("[cursor]\nsize = 40\n");
    check(loose.layers.size() == 1 && loose.layers[0].path[0] == '\0' && loose.layers[0].begin == 0,
          "bytes without a marker are one layer without a path");
    check(cfg::split_layers("").layers.empty(), "no bytes, no layer");

    cfg::config_snapshot c3 = cfg::default_config();
    cfg::diagnostics d3{};
    check(cfg::parse_config(cfg::split_layers("[general]\ndefault-bindings = false\n"
                                              "[bindings]\n\"Super+T\" = \"shell.terminal\"\n"),
                            c3, d3, nullptr) &&
              c3.bindings.size() == 1,
          "default-bindings = false keeps only the file's");
}

void test_bindings() {
    const parsed p = parse_one(R"([bindings]
"Ctrl+Alt+Shift+Super+x" = "window.close"
"logo+mod1+Page_Down" = "window.fullscreen-toggle"
"Super+BTN_MIDDLE" = "window.close"
"Alt" = "shell.terminal"
"Super+Shift" = "shell.terminal"
"Super+Foo" = "window.close"
"Hyper+x" = "window.close"
"Ctrl+Alt+F2" = "window.close"
"Ctrl+Alt+BackSpace" = "window.close"
"Super+Escape" = "window.close"
"Super+Z" = "window.closee"
"Super+V" = "window.close 3"
"Super+1" = "workspace.switch 1"
"Super+D" = "notifications.toggle-dnd"
"Super+Left" = "window.snap-left"
"Tab" = "switcher.next"
"Alt+Tab" = "switcher.previous"
"Super+X" = "window.move-interactive"
"Super+C" = { action = "window.close", allow-when-locked = true }
"Super+B" = { action = "session.logout", allow-when-locked = true, colour = 1 }
"Super+N" = { allow-when-locked = true }
"super+c" = "window.maximize-toggle"
"Super+K" = "keyboard.next-layout"
"Super+BTN_FOO" = "window.close"
)");
    check(p.ok, "bindings parse");
    dump(p.d);
    const auto* all = find_binding(p.c, "Super+Ctrl+Alt+Shift+x");
    check(all && all->keys.mods == cfg::k_mods_all, "every modifier, any order and case");
    const auto* pd = find_binding(p.c, "Super+Alt+Page_Down");
    check(pd && pd->action == cfg::action_id::window_fullscreen_toggle,
          "Logo and Mod1 are Super and Alt");
    check(find_binding(p.c, "Super+BTN_MIDDLE"), "a button binding");
    const auto* alt = find_binding(p.c, "Alt");
    check(alt && alt->keys.kind == cfg::combo_kind::tap && alt->keys.mods == cfg::mod_alt,
          "a bare modifier is a tap");
    check(has_diag(p.d, "Super+Shift", "a tap is one modifier alone"), "two modifiers alone: error");
    check(has_diag(p.d, "Super+Foo", "unknown key name"), "an unknown keysym");
    check(has_diag(p.d, "Hyper+x", "unknown modifier"), "an unknown modifier");
    check(has_diag(p.d, "Ctrl+Alt+F2", "reserved") && has_diag(p.d, "Ctrl+Alt+BackSpace", "reserved") &&
              has_diag(p.d, "Super+Escape", "reserved"),
          "the compositor's own keys are reserved");
    check(has_diag(p.d, "Super+Z", "did you mean window.close?"), "an unknown action suggests one");
    check(has_diag(p.d, "Super+V", "takes no argument"), "an argument where none is taken");
    const auto* dnd = find_binding(p.c, "Super+D");
    check(dnd && dnd->action == cfg::action_id::notifications_toggle_dnd &&
              !has_diag(p.d, "Super+D", "not available in this version"),
          "a notification action binds (Part 5: every action is available)");
    const auto* snap = find_binding(p.c, "Super+Left");
    check(snap && snap->action == cfg::action_id::window_snap_left, "window.snap-left binds (Part 4)");
    check(has_diag(p.d, "Tab", "holds the keyboard while its modifier is down") &&
              !find_binding(p.c, "Tab"),
          "a held action needs a modifier");
    const auto* held = find_binding(p.c, "Alt+Tab");
    check(held && held->action == cfg::action_id::switcher_previous && cfg::action(held->action).held,
          "a held action with a modifier binds (and is held)");
    const auto* ws = find_binding(p.c, "Super+1");
    check(ws && ws->action == cfg::action_id::workspace_switch && ws->argument == 1,
          "workspace.switch takes its index");
    check(has_diag(p.d, "Super+X", "needs a pointer button"), "move-interactive on a key");
    const auto* c = find_binding(p.c, "Super+c");
    check(has_diag(p.d, "Super+C", "may not run while the session is locked") && c &&
              c->action == cfg::action_id::window_maximize_toggle && !(c->options & cfg::bo_allow_when_locked),
          "allow-when-locked refused for a non-lock-safe action; the later spelling wins");
    check(has_diag(p.d, "super+c", "bound twice in this file"), "two spellings of one combination");
    const auto* b = find_binding(p.c, "Super+B");
    check(b && (b->options & cfg::bo_allow_when_locked) && has_diag(p.d, "colour", "unknown binding option"),
          "allow-when-locked for logout; an unknown option warned");
    check(has_diag(p.d, "Super+N", "needs action"), "an inline binding without an action");
    check(find_binding(p.c, "Super+K"), "keyboard.next-layout binds");
    check(has_diag(p.d, "BTN_FOO", "unknown button"), "an unknown button");

    // Grammar alone.
    cfg::combo k{};
    const char* e = nullptr;
    check(cfg::parse_combo("Super+L", k, e) && k.code == 0x6c && k.mods == cfg::mod_super,
          "Super+L is the l key (first level)");
    check(cfg::parse_combo("Super++", k, e) && k.code == 0x2b, "Super++ is the plus key");
    check(cfg::parse_combo(" Ctrl + Return ", k, e) && k.code == 0xff0d && k.mods == cfg::mod_ctrl,
          "spaces around parts");
    check(!cfg::parse_combo("Super+Super+a", k, e), "a modifier twice is refused");
    char name[64];
    (void)cfg::parse_combo("shift+super+Page_Up", k, e);
    cfg::format_combo(k, name, sizeof(name));
    check(std::strcmp(name, "Super+Shift+Page_Up") == 0, "canonical spelling");
    (void)cfg::parse_combo("super", k, e);
    cfg::format_combo(k, name, sizeof(name));
    check(std::strcmp(name, "Super") == 0, "a tap's spelling");
    check(cfg::reserved_key(0x1008fe05, 0) && !cfg::reserved_key(0xffbf, cfg::mod_alt),
          "VT keysyms reserved; Alt+F2 is not");
    check(cfg::find_action("session.logout") && cfg::action(cfg::action_id::session_logout).lock_safe,
          "action table by name and id");
    check(std::strcmp(cfg::action(cfg::action_id::window_close).name, "window.close") == 0,
          "action table order matches the ids");
}

void test_edit_distance() {
    check(cfg::edit_distance("tapp", "tap") == 1 && cfg::edit_distance("acel-speed", "accel-speed") == 1,
          "insertions and deletions");
    check(cfg::edit_distance("mdoe", "mode") == 1, "a transposition is one edit");
    check(cfg::edit_distance("abcdef", "xyz") == 3, "far apart is capped at limit + 1");
}

void test_diagnostics_bound() {
    std::string text = "[cursor]\n";
    for (int i = 0; i < 100; ++i)
        text += "bogus" + std::to_string(i) + " = 1\n";
    const parsed p = parse_one(text.c_str());
    check(p.ok && p.d.items().size() == cfg::diagnostics::k_max && p.d.warnings() == 100 &&
              p.d.dropped() == 100 - cfg::diagnostics::k_max,
          "diagnostics are bounded; the rest counted");
}

// ── Locations and reading ──

const char* fake_env(const char* name) {
    if (std::strcmp(name, "XDG_CONFIG_DIRS") == 0)
        return "/opt/a:relative/b:/etc/xdg";
    if (std::strcmp(name, "XDG_CONFIG_HOME") == 0)
        return "/home/u/.cfg";
    return nullptr;
}

const char* fake_env_forced(const char* name) {
    return std::strcmp(name, "LUMEN_CONFIG") == 0 ? "/tmp/x.toml" : fake_env(name);
}

const char* fake_env_off(const char* name) {
    return std::strcmp(name, "LUMEN_CONFIG") == 0 ? "0" : fake_env(name);
}

void test_locate() {
    const cfg::config_location l = cfg::locate_config(&fake_env);
    check(l.files.size() == 3 && std::strcmp(l.files[0].path, "/etc/xdg/lumen/config.toml") == 0 &&
              std::strcmp(l.files[1].path, "/opt/a/lumen/config.toml") == 0 &&
              std::strcmp(l.files[2].path, "/home/u/.cfg/lumen/config.toml") == 0,
          "XDG_CONFIG_DIRS reversed (relative entries ignored), then XDG_CONFIG_HOME");
    check(std::strcmp(l.user_file.path, "/home/u/.cfg/lumen/config.toml") == 0, "the user's file");
    const cfg::config_location f = cfg::locate_config(&fake_env_forced);
    check(f.explicit_file && f.files.size() == 1 && std::strcmp(f.files[0].path, "/tmp/x.toml") == 0,
          "LUMEN_CONFIG=PATH is the only layer");
    const cfg::config_location off = cfg::locate_config(&fake_env_off);
    check(off.disabled && off.files.empty(), "LUMEN_CONFIG=0: none");
}

std::string make_scratch() {
    char tmpl[] = "/tmp/lumen-test-config-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

void write_file(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::fputs(text, f);
    std::fclose(f);
}

void test_read_and_watch() {
    const std::string root = make_scratch();
    check(!root.empty(), "scratch directory");
    if (root.empty())
        return;
    cfg::config_location loc{};
    cfg::config_path sys{};
    std::snprintf(sys.path, sizeof(sys.path), "%s/sys/lumen/config.toml", root.c_str());
    cfg::config_path user{};
    std::snprintf(user.path, sizeof(user.path), "%s/home/lumen/config.toml", root.c_str());
    loc.files.push_back(sys);
    loc.files.push_back(user);
    loc.user_file = user;

    cfg::config_text t{};
    cfg::diagnostics d{};
    check(cfg::read_config(loc, t, d) && t.layers.empty() && d.empty(),
          "no file: no layer, nothing wrong (the defaults)");

    cfg::config_watch w{};
    check(w.open(loc) && w.fd() >= 0, "the watch opens");
    check(!w.drain(), "nothing happened yet");
    // The directory appears (mkdir -p), then the file is written by rename (an editor).
    ::mkdir((root + "/home").c_str(), 0700);
    ::mkdir((root + "/home/lumen").c_str(), 0700);
    check(w.drain(), "a directory on the way appearing re-arms (and may be a change)");
    write_file(root + "/home/lumen/.config.toml.swp", "x");
    check(!w.drain(), "another file in the directory is not a change");
    write_file(root + "/home/lumen/config.toml.new", "[cursor]\nsize = 30\n");
    (void)w.drain();
    ::rename((root + "/home/lumen/config.toml.new").c_str(), user.path);
    check(w.drain(), "an editor's rename over config.toml is a change");
    check(cfg::read_config(loc, t, d) && t.layers.size() == 1 &&
              std::strcmp(t.layers[0].path, user.path) == 0,
          "the user's layer is read");
    write_file(user.path, "[cursor]\nsize = 31\n");
    check(w.drain(), "writing it in place is a change");
    ::unlink(user.path);
    check(w.drain(), "removing it is a change");
    // A symlinked file: writing the target (elsewhere) is a change.
    ::mkdir((root + "/dots").c_str(), 0700);
    write_file(root + "/dots/lumen.toml", "[cursor]\nsize = 40\n");
    (void)::symlink((root + "/dots/lumen.toml").c_str(), user.path);
    check(w.drain(), "the symlink appearing is a change");
    write_file(root + "/dots/lumen.toml.tmp", "[cursor]\nsize = 41\n");
    (void)w.drain();
    ::rename((root + "/dots/lumen.toml.tmp").c_str(), (root + "/dots/lumen.toml").c_str());
    check(w.drain(), "renaming over the symlink's target is a change");
    check(cfg::read_config(loc, t, d) && t.layers.size() == 1 &&
              std::strstr(t.bytes.c_str(), "size = 41"),
          "the symlink is read through");

    // Too large: the configuration is refused.
    std::string big(cfg::k_max_layer_bytes + 10, '#');
    write_file(root + "/dots/lumen.toml", big.c_str());
    cfg::diagnostics d2{};
    check(!cfg::read_config(loc, t, d2) && has_diag(d2, nullptr, "larger than 256 KiB"),
          "a layer over 256 KiB is refused");
    w.close();
    std::string cmd = "rm -rf '" + root + "'";
    (void)std::system(cmd.c_str());
}

void test_check_keymap() {
    cfg::keyboard_settings k{};
    std::snprintf(k.layout, sizeof(k.layout), "us,ru");
    cfg::diagnostics d{};
    const bool built_with_xkb = cfg::check_keymap(k, d);
    if (!built_with_xkb && d.empty()) {
        check(true, "check_keymap: built without xkbcommon (skipped)");
        return;
    }
    check(built_with_xkb && d.empty(), "us,ru compiles");
    std::snprintf(k.layout, sizeof(k.layout), "xx-not-a-layout");
    cfg::diagnostics d2{};
    check(!cfg::check_keymap(k, d2) && has_diag(d2, "input.keyboard", "does not compile"),
          "an unknown layout does not compile, with xkbcommon's reason");
    dump(d2);
}

} // namespace

int main() {
    std::printf("test_config\n");
    test_inline_tables();
    test_defaults();
    test_documented_file();
    test_values();
    test_bad_values();
    test_unknown();
    test_syntax_error();
    test_layers();
    test_bindings();
    test_edit_distance();
    test_diagnostics_bound();
    test_locate();
    test_read_and_watch();
    test_check_keymap();
    std::printf("%d checks, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}
