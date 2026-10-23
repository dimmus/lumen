module;

// The configuration parser (`:parse`): the schema-driven handler over the TOML reader, the
// layering, the bindings' rules and the diagnostics' wording. See parse.cppm.

#include <math.h>
#include <stdarg.h>

#if defined(LUMEN_HAS_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

module lx.config;

import std;

import :toml;
import :diagnostics;
import :keys;
import :actions;
import :schema;
import :parse;

namespace lx::config {

namespace {

enum class section_kind : unsigned char { top, generic, pointer, device, bindings, unknown };

struct user_binding {
    binding b{};
    /// Bound to `none`: removes a default (or an earlier layer's) binding.
    bool remove = false;
};

struct inline_state {
    bool open = false;
    /// Not in [bindings]: the members are skipped (the key already has its error).
    bool ignored = false;
    char key[96]{};
    unsigned line = 0;
    char action[64]{};
    bool have_action = false;
    char argument[32]{};
    unsigned char options = 0;
};

struct parse_state {
    config_snapshot* out = nullptr;
    const config_snapshot* previous = nullptr;
    diagnostics* diag = nullptr;
    unsigned layer = 0;
    section_kind kind = section_kind::top;
    char section[128]{};
    pointer_settings* ptr = nullptr;
    std::uint32_t ptr_mask = 0;
    const char* ptr_name = "";
    unsigned ptr_kind = 0;
    bool device_open = false;
    device_override device{};
    inline_state in{};
    std::inplace_vector<user_binding, k_max_bindings>* user = nullptr;
    /// Combinations bound in this layer (duplicates are warned).
    std::inplace_vector<combo, k_max_bindings> layer_combos{};
    unsigned line = 0;
};

[[nodiscard]] bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto la = static_cast<char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i]);
        const auto lb = static_cast<char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] - 'A' + 'a' : b[i]);
        if (la != lb)
            return false;
    }
    return true;
}

/// "section.key" into `out`.
void full_key(const char* section, const char* key, char* out, std::size_t cap) {
    if (section && *section)
        std::snprintf(out, cap, "%s.%s", section, key);
    else
        std::snprintf(out, cap, "%s", key);
}

/// The value as the file wrote it, for a message: strings quoted and cut at 64 bytes.
void quote_value(const toml::value& v, char* out, std::size_t cap) {
    switch (v.type) {
    case toml::value::kind::string: {
        const std::size_t n = std::strlen(v.str);
        if (n > 64)
            std::snprintf(out, cap, "\"%.61s...\"", v.str);
        else
            std::snprintf(out, cap, "\"%s\"", v.str);
        return;
    }
    case toml::value::kind::integer: std::snprintf(out, cap, "%lld", v.integer); return;
    case toml::value::kind::floating: std::snprintf(out, cap, "%g", v.floating); return;
    case toml::value::kind::boolean: std::snprintf(out, cap, "%s", v.boolean ? "true" : "false"); return;
    case toml::value::kind::array: std::snprintf(out, cap, "[%u items]", v.count); return;
    }
    if (cap)
        out[0] = '\0';
}

/// A field's value as the file would write it.
void format_field(value_kind kind, const void* p, const char* const* names, unsigned name_count,
                  char* out, std::size_t cap) {
    switch (kind) {
    case value_kind::boolean:
        std::snprintf(out, cap, "%s", *static_cast<const bool*>(p) ? "true" : "false");
        return;
    case value_kind::integer: std::snprintf(out, cap, "%d", *static_cast<const int*>(p)); return;
    case value_kind::number: {
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof(buf) - 1, *static_cast<const double*>(p));
        *r.ptr = '\0';
        std::snprintf(out, cap, "%s", buf);
        return;
    }
    case value_kind::string: std::snprintf(out, cap, "\"%.60s\"", static_cast<const char*>(p)); return;
    case value_kind::enumeration:
        std::snprintf(out, cap, "%s",
                      enum_word(names, name_count, *static_cast<const unsigned char*>(p)));
        return;
    case value_kind::string_list: {
        const auto* w = static_cast<const workspace_settings*>(p);
        std::snprintf(out, cap, "[%u names]", w->name_count);
        return;
    }
    }
}

void copy_field(value_kind kind, void* dst, const void* src, unsigned capacity) {
    switch (kind) {
    case value_kind::boolean: *static_cast<bool*>(dst) = *static_cast<const bool*>(src); break;
    case value_kind::integer: *static_cast<int*>(dst) = *static_cast<const int*>(src); break;
    case value_kind::number: *static_cast<double*>(dst) = *static_cast<const double*>(src); break;
    case value_kind::string:
        std::snprintf(static_cast<char*>(dst), capacity, "%s", static_cast<const char*>(src));
        break;
    case value_kind::enumeration:
        *static_cast<unsigned char*>(dst) = *static_cast<const unsigned char*>(src);
        break;
    case value_kind::string_list: {
        auto* d = static_cast<workspace_settings*>(dst);
        const auto* s = static_cast<const workspace_settings*>(src);
        std::memcpy(d->names, s->names, sizeof(d->names));
        d->name_count = s->name_count;
        break;
    }
    }
}

struct field_ref {
    value_kind kind = value_kind::boolean;
    double lo = 0.0;
    double hi = 0.0;
    const char* const* names = nullptr;
    unsigned name_count = 0;
    unsigned capacity = 0;
};

/// Validates `v` for `f` and stores it in `dst`. On a bad value: false and `why` (the part
/// of the message before "; keeping …").
bool store_value(const field_ref& f, const toml::value& v, void* dst, char* why, std::size_t cap) {
    char got[96];
    quote_value(v, got, sizeof(got));
    switch (f.kind) {
    case value_kind::boolean:
        if (v.type != toml::value::kind::boolean) {
            std::snprintf(why, cap, "must be true or false (got %s)", got);
            return false;
        }
        *static_cast<bool*>(dst) = v.boolean;
        return true;
    case value_kind::integer:
        if (v.type != toml::value::kind::integer) {
            std::snprintf(why, cap, "must be a whole number (got %s)", got);
            return false;
        }
        if (static_cast<double>(v.integer) < f.lo || static_cast<double>(v.integer) > f.hi) {
            std::snprintf(why, cap, "%s is outside %g..%g", got, f.lo, f.hi);
            return false;
        }
        *static_cast<int*>(dst) = static_cast<int>(v.integer);
        return true;
    case value_kind::number: {
        if (!v.is_number()) {
            std::snprintf(why, cap, "must be a number (got %s)", got);
            return false;
        }
        const double d = v.number();
        if (!std::isfinite(d) || d < f.lo || d > f.hi) {
            std::snprintf(why, cap, "%s is outside %g..%g", got, f.lo, f.hi);
            return false;
        }
        *static_cast<double*>(dst) = d;
        return true;
    }
    case value_kind::string: {
        if (v.type != toml::value::kind::string) {
            std::snprintf(why, cap, "must be a string (got %s)", got);
            return false;
        }
        if (std::strlen(v.str) + 1 > f.capacity) {
            std::snprintf(why, cap, "is longer than %u bytes", f.capacity - 1);
            return false;
        }
        std::snprintf(static_cast<char*>(dst), f.capacity, "%s", v.str);
        return true;
    }
    case value_kind::enumeration: {
        char choices[128] = "";
        std::size_t n = 0;
        for (unsigned i = 0; i < f.name_count; ++i) {
            const int w = std::snprintf(choices + n, sizeof(choices) - n, "%s%s", i ? ", " : "",
                                        f.names[i]);
            if (w > 0)
                n = std::min(sizeof(choices) - 1, n + static_cast<std::size_t>(w));
        }
        if (v.type != toml::value::kind::string) {
            std::snprintf(why, cap, "must be one of %s (got %s)", choices, got);
            return false;
        }
        for (unsigned i = 0; i < f.name_count; ++i) {
            if (iequal(v.str, f.names[i])) {
                *static_cast<unsigned char*>(dst) = static_cast<unsigned char>(i);
                return true;
            }
        }
        const char* near = nullptr;
        for (unsigned i = 0; i < f.name_count && !near; ++i)
            if (edit_distance(v.str, f.names[i], 2) <= 2)
                near = f.names[i];
        if (near)
            std::snprintf(why, cap, "%s is not one of %s (did you mean %s?)", got, choices, near);
        else
            std::snprintf(why, cap, "%s is not one of %s", got, choices);
        return false;
    }
    case value_kind::string_list: {
        auto* w = static_cast<workspace_settings*>(dst);
        if (v.type != toml::value::kind::array) {
            std::snprintf(why, cap, "must be an array of strings (got %s)", got);
            return false;
        }
        if (v.count > workspace_settings::k_max_names) {
            std::snprintf(why, cap, "has %u names, at most %u", v.count,
                          workspace_settings::k_max_names);
            return false;
        }
        for (unsigned i = 0; i < v.count; ++i) {
            if (v.items[i].type != toml::value::kind::string) {
                std::snprintf(why, cap, "must be an array of strings");
                return false;
            }
            if (std::strlen(v.items[i].str) + 1 > f.capacity) {
                std::snprintf(why, cap, "has a name longer than %u bytes", f.capacity - 1);
                return false;
            }
        }
        w->name_count = v.count;
        for (unsigned i = 0; i < v.count; ++i)
            std::snprintf(w->names[i], sizeof(w->names[i]), "%s", v.items[i].str);
        return true;
    }
    }
    return false;
}

field_ref ref_of(const key_desc& k) {
    return {k.kind, k.lo, k.hi, k.names, k.name_count, k.capacity};
}
field_ref ref_of(const pointer_key_desc& k) {
    return {k.kind, k.lo, k.hi, k.names, k.name_count, 0};
}

const char* suggest_in_section(std::string_view section, std::string_view key) {
    const char* best = nullptr;
    unsigned best_d = 3;
    for (const key_desc& k : schema_keys()) {
        if (section != k.section)
            continue;
        const unsigned d = edit_distance(key, k.key, 2);
        if (d < best_d) {
            best_d = d;
            best = k.key;
        }
    }
    return best;
}

const char* suggest_pointer_key(std::string_view key, std::uint32_t mask) {
    const char* best = nullptr;
    unsigned best_d = 3;
    for (const pointer_key_desc& k : pointer_keys()) {
        if (!(k.bit & mask))
            continue;
        const unsigned d = edit_distance(key, k.key, 2);
        if (d < best_d) {
            best_d = d;
            best = k.key;
        }
    }
    return best;
}

const char* suggest_section(std::string_view name) {
    const char* best = nullptr;
    unsigned best_d = 3;
    for (const char* s : schema_sections()) {
        const unsigned d = edit_distance(name, s, 2);
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    return best;
}

void unknown_key(parse_state& st, const char* key, const char* suggestion) {
    char fk[128];
    full_key(st.section, key, fk, sizeof(fk));
    if (suggestion)
        st.diag->addf(severity::warning, st.layer, st.line, fk,
                      "unknown key (did you mean {}?); ignored", suggestion);
    else
        st.diag->add(severity::warning, st.layer, st.line, fk, "unknown key; ignored");
}

// ── Devices ──

void finish_device(parse_state& st) {
    if (!st.device_open)
        return;
    st.device_open = false;
    if (!st.device.name[0]) {
        st.diag->add(severity::error, st.layer, st.device.line, "input.device",
                     "an [[input.device]] needs a name (libinput's device name); ignored");
        return;
    }
    for (device_override& d : st.out->devices) {
        if (std::strcmp(d.name, st.device.name) != 0)
            continue;
        // The same device in another layer (or twice): its keys over the earlier ones.
        for (const pointer_key_desc& k : pointer_keys()) {
            if (!(st.device.settings.set & k.bit))
                continue;
            copy_field(k.kind, k.field(d.settings), k.field(st.device.settings), 0);
        }
        d.settings.set |= st.device.settings.set;
        return;
    }
    if (st.out->devices.size() == k_max_devices) {
        st.diag->addf(severity::error, st.layer, st.device.line, "input.device",
                      "more than {} [[input.device]] entries; \"{}\" ignored", k_max_devices,
                      st.device.name);
        return;
    }
    st.out->devices.push_back(st.device);
}

// ── Bindings ──

void add_binding(parse_state& st, const char* key_text, unsigned line, const char* action_text,
                 const char* argument_text, unsigned char options) {
    char fk[96];
    std::snprintf(fk, sizeof(fk), "bindings.%s", key_text);
    combo c{};
    const char* error = nullptr;
    if (!parse_combo(key_text, c, error)) {
        st.diag->addf(severity::error, st.layer, line, fk, "{}; binding ignored", error);
        return;
    }
    if (reserved(c)) {
        st.diag->add(severity::error, st.layer, line, fk,
                     "reserved for the compositor (VT switching, Ctrl+Alt+BackSpace logout, "
                     "Super+Escape); binding ignored");
        return;
    }
    const action_info* a = find_action(action_text);
    if (!a) {
        if (const char* near = suggest_action(action_text))
            st.diag->addf(severity::error, st.layer, line, fk,
                          "unknown action \"{}\" (did you mean {}?); binding ignored", action_text,
                          near);
        else
            st.diag->addf(severity::error, st.layer, line, fk,
                          "unknown action \"{}\" (see lumen-ctl action --list); binding ignored",
                          action_text);
        return;
    }
    unsigned argument = 0;
    if (a->arg == action_arg::index) {
        char* end = nullptr;
        const long n = argument_text && *argument_text ? std::strtol(argument_text, &end, 10) : 0;
        if (!argument_text || !*argument_text || !end || *end || n < 1 || n > 99) {
            st.diag->addf(severity::error, st.layer, line, fk,
                          "{} needs a number 1..99 (\"{} 2\"); binding ignored", a->name, a->name);
            return;
        }
        argument = static_cast<unsigned>(n);
    } else if (argument_text && *argument_text) {
        st.diag->addf(severity::error, st.layer, line, fk,
                      "{} takes no argument (got \"{}\"); binding ignored", a->name, argument_text);
        return;
    }
    if (a->pointer && c.kind != combo_kind::button) {
        st.diag->addf(severity::error, st.layer, line, fk,
                      "{} needs a pointer button (e.g. Super+BTN_LEFT); binding ignored", a->name);
        return;
    }
    if (a->held && (c.kind != combo_kind::key || c.mods == 0)) {
        st.diag->addf(severity::error, st.layer, line, fk,
                      "{} holds the keyboard while its modifier is down: it needs a key with a "
                      "modifier (e.g. Alt+Tab); binding ignored",
                      a->name);
        return;
    }
    if ((options & bo_allow_when_locked) && !a->lock_safe && a->id != action_id::none) {
        st.diag->addf(severity::error, st.layer, line, fk,
                      "allow-when-locked: {} may not run while the session is locked (only "
                      "session.logout and session.reload-config); option ignored",
                      a->name);
        options = static_cast<unsigned char>(options & ~bo_allow_when_locked);
    }
    if ((options & bo_repeat) && c.kind != combo_kind::key) {
        st.diag->add(severity::warning, st.layer, line, fk,
                     "repeat applies to keys only; option ignored");
        options = static_cast<unsigned char>(options & ~bo_repeat);
    }
    if (!a->available) {
        st.diag->addf(severity::warning, st.layer, line, fk,
                      "{} is not available in this version of Lumen yet; binding ignored", a->name);
        return;
    }
    // Twice in one file (two spellings of one combination): the later one wins.
    if (std::ranges::contains(st.layer_combos, c)) {
        st.diag->add(severity::warning, st.layer, line, fk,
                     "this combination is bound twice in this file; the later binding wins");
    } else if (st.layer_combos.size() < k_max_bindings) {
        st.layer_combos.push_back(c);
    }
    user_binding u{};
    u.b.keys = c;
    u.b.action = a->id;
    u.b.argument = argument;
    u.b.options = options;
    u.b.layer = st.layer;
    u.b.line = line;
    u.remove = a->id == action_id::none;
    for (user_binding& e : *st.user) {
        if (e.b.keys == c) {
            e = u;
            return;
        }
    }
    if (st.user->size() == k_max_bindings) {
        st.diag->addf(severity::error, st.layer, line, fk,
                      "more than {} bindings; binding ignored", k_max_bindings);
        return;
    }
    st.user->push_back(u);
}

/// "action [argument]".
void binding_from_string(parse_state& st, const char* key, const char* text) {
    std::string_view s{text};
    while (!s.empty() && s.front() == ' ')
        s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ')
        s.remove_suffix(1);
    char action[64]{};
    char argument[32]{};
    const std::size_t sp = s.find(' ');
    const std::string_view a = s.substr(0, sp);
    std::snprintf(action, sizeof(action), "%.*s", static_cast<int>(a.size()), a.data());
    if (sp != std::string_view::npos) {
        std::string_view rest = s.substr(sp + 1);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        std::snprintf(argument, sizeof(argument), "%.*s", static_cast<int>(rest.size()),
                      rest.data());
    }
    add_binding(st, key, st.line, action, argument, 0);
}

// ── Callbacks ──

bool on_table(void* user, const char* name, bool array, toml::parse_error& err) {
    auto& st = *static_cast<parse_state*>(user);
    st.line = err.line;
    finish_device(st);
    st.ptr = nullptr;
    std::snprintf(st.section, sizeof(st.section), "%s", name);
    const std::string_view n{name};
    if (array) {
        if (n == "input.device") {
            st.kind = section_kind::device;
            st.device_open = true;
            st.device = device_override{};
            st.device.layer = st.layer;
            st.device.line = err.line;
            return true;
        }
        st.kind = section_kind::unknown;
        st.diag->addf(severity::warning, st.layer, err.line, name,
                      "[[{}]] is not a known array of tables; its keys are ignored", name);
        return true;
    }
    if (n == "input.device") {
        st.kind = section_kind::unknown;
        st.diag->add(severity::error, st.layer, err.line, name,
                     "per-device settings are [[input.device]] (an array of tables); ignored");
        return true;
    }
    if (n == "input.touchpad" || n == "input.mouse" || n == "input.trackpoint") {
        st.kind = section_kind::pointer;
        if (n == "input.touchpad") {
            st.ptr = &st.out->touchpad;
            st.ptr_mask = k_pointer_fields_all;
            st.ptr_name = "touchpad";
            st.ptr_kind = 0;
        } else if (n == "input.mouse") {
            st.ptr = &st.out->mouse;
            st.ptr_mask = k_mouse_fields;
            st.ptr_name = "mouse";
            st.ptr_kind = 1;
        } else {
            st.ptr = &st.out->trackpoint;
            st.ptr_mask = k_trackpoint_fields;
            st.ptr_name = "trackpoint";
            st.ptr_kind = 2;
        }
        return true;
    }
    if (n == "bindings") {
        st.kind = section_kind::bindings;
        st.layer_combos.clear();
        return true;
    }
    for (const char* s : schema_sections()) {
        if (n == s) {
            st.kind = section_kind::generic;
            return true;
        }
    }
    st.kind = section_kind::unknown;
    if (const char* near = suggest_section(n))
        st.diag->addf(severity::warning, st.layer, err.line, name,
                      "unknown section [{}] (did you mean [{}]?); its keys are ignored", name, near);
    else
        st.diag->addf(severity::warning, st.layer, err.line, name,
                      "unknown section [{}]; its keys are ignored", name);
    return true;
}

void bad_value(parse_state& st, const char* key, const char* why, value_kind kind, const void* kept,
               const char* const* names, unsigned name_count) {
    char fk[128];
    full_key(st.section, key, fk, sizeof(fk));
    char keeping[96];
    format_field(kind, kept, names, name_count, keeping, sizeof(keeping));
    st.diag->addf(severity::error, st.layer, st.line, fk, "{}; keeping {}", why, keeping);
}

void on_generic(parse_state& st, const char* key, const toml::value& v) {
    for (const key_desc& k : schema_keys()) {
        if (std::strcmp(k.section, st.section) != 0 || std::strcmp(k.key, key) != 0)
            continue;
        void* dst = k.field(*st.out);
        char why[160];
        if (store_value(ref_of(k), v, dst, why, sizeof(why)))
            return;
        // Keeps the value in use (or, at start-up, what the defaults and earlier layers set).
        if (st.previous)
            copy_field(k.kind, dst, k.field(const_cast<config_snapshot&>(*st.previous)), k.capacity);
        bad_value(st, key, why, k.kind, dst, k.names, k.name_count);
        return;
    }
    unknown_key(st, key, suggest_in_section(st.section, key));
}

void on_pointer(parse_state& st, pointer_settings& to, std::uint32_t mask, const char* key,
                const toml::value& v, const pointer_settings* prev) {
    for (const pointer_key_desc& k : pointer_keys()) {
        if (std::strcmp(k.key, key) != 0)
            continue;
        if (!(k.bit & mask)) {
            char fk[128];
            full_key(st.section, key, fk, sizeof(fk));
            st.diag->addf(severity::warning, st.layer, st.line, fk,
                          "not a {} setting (it is a touchpad's; use [input.touchpad] or "
                          "[[input.device]]); ignored",
                          st.ptr_name);
            return;
        }
        void* dst = k.field(to);
        char why[160];
        if (store_value(ref_of(k), v, dst, why, sizeof(why))) {
            to.set |= k.bit;
            return;
        }
        if (prev)
            copy_field(k.kind, dst, k.field(const_cast<pointer_settings&>(*prev)), 0);
        bad_value(st, key, why, k.kind, dst, k.names, k.name_count);
        return;
    }
    unknown_key(st, key, suggest_pointer_key(key, mask));
}

bool on_key_value(void* user, const char* key, const toml::value& v, toml::parse_error& err) {
    auto& st = *static_cast<parse_state*>(user);
    st.line = err.line;
    if (st.in.open) {
        if (st.in.ignored)
            return true;
        char fk[128];
        std::snprintf(fk, sizeof(fk), "bindings.%s.%s", st.in.key, key);
        const std::string_view k{key};
        if (k == "action") {
            if (v.type != toml::value::kind::string) {
                st.diag->add(severity::error, st.layer, st.line, fk, "must be an action name");
                return true;
            }
            std::snprintf(st.in.action, sizeof(st.in.action), "%s", v.str);
            st.in.have_action = true;
        } else if (k == "argument") {
            if (v.type == toml::value::kind::integer)
                std::snprintf(st.in.argument, sizeof(st.in.argument), "%lld", v.integer);
            else if (v.type == toml::value::kind::string)
                std::snprintf(st.in.argument, sizeof(st.in.argument), "%s", v.str);
            else
                st.diag->add(severity::error, st.layer, st.line, fk,
                             "must be a number or a string");
        } else if (k == "allow-when-inhibited" || k == "allow-when-locked" || k == "repeat") {
            if (v.type != toml::value::kind::boolean) {
                st.diag->add(severity::error, st.layer, st.line, fk, "must be true or false");
                return true;
            }
            const unsigned char bit = k == "allow-when-inhibited" ? bo_allow_when_inhibited
                                      : k == "allow-when-locked"  ? bo_allow_when_locked
                                                                  : bo_repeat;
            st.in.options = static_cast<unsigned char>(v.boolean ? (st.in.options | bit)
                                                                 : (st.in.options & ~bit));
        } else {
            const char* known[] = {"action", "argument", "allow-when-inhibited",
                                   "allow-when-locked", "repeat"};
            const char* near = nullptr;
            for (const char* n : known)
                if (!near && edit_distance(k, n, 2) <= 2)
                    near = n;
            if (near)
                st.diag->addf(severity::warning, st.layer, st.line, fk,
                              "unknown binding option (did you mean {}?); ignored", near);
            else
                st.diag->add(severity::warning, st.layer, st.line, fk,
                             "unknown binding option (action, argument, allow-when-inhibited, "
                             "allow-when-locked, repeat); ignored");
        }
        return true;
    }
    switch (st.kind) {
    case section_kind::top:
        if (std::strcmp(key, "version") == 0) {
            if (v.type != toml::value::kind::integer || v.integer < 1) {
                st.diag->add(severity::error, st.layer, st.line, "version",
                             "must be a whole number from 1; ignored");
            } else if (v.integer > 1) {
                st.diag->addf(severity::warning, st.layer, st.line, "version",
                              "written for a newer Lumen (version {}); keys this version does "
                              "not know are ignored",
                              v.integer);
                st.out->version = static_cast<int>(v.integer);
            }
            return true;
        }
        if (const char* near = suggest_section(key))
            st.diag->addf(severity::warning, st.layer, st.line, key,
                          "unknown key outside any section (did you mean [{}]?); ignored", near);
        else
            st.diag->add(severity::warning, st.layer, st.line, key,
                         "unknown key outside any section; ignored");
        return true;
    case section_kind::generic: on_generic(st, key, v); return true;
    case section_kind::pointer: {
        const pointer_settings* prev = nullptr;
        if (st.previous)
            prev = st.ptr_kind == 0   ? &st.previous->touchpad
                   : st.ptr_kind == 1 ? &st.previous->mouse
                                      : &st.previous->trackpoint;
        on_pointer(st, *st.ptr, st.ptr_mask, key, v, prev);
        return true;
    }
    case section_kind::device:
        if (std::strcmp(key, "name") == 0) {
            if (v.type != toml::value::kind::string || !*v.str ||
                std::strlen(v.str) >= sizeof(st.device.name)) {
                st.diag->add(severity::error, st.layer, st.line, "input.device.name",
                             "must be the device's name as libinput reports it (lumen-ctl "
                             "status lists them)");
                return true;
            }
            std::snprintf(st.device.name, sizeof(st.device.name), "%s", v.str);
            return true;
        }
        st.ptr_name = "device";
        on_pointer(st, st.device.settings, k_pointer_fields_all, key, v, nullptr);
        return true;
    case section_kind::bindings:
        if (v.type != toml::value::kind::string) {
            char fk[96];
            std::snprintf(fk, sizeof(fk), "bindings.%s", key);
            st.diag->add(severity::error, st.layer, st.line, fk,
                         "must be \"action [argument]\" or { action = \"…\", … }; binding "
                         "ignored");
            return true;
        }
        binding_from_string(st, key, v.str);
        return true;
    case section_kind::unknown: return true;
    }
    return true;
}

bool on_inline(void* user, const char* key, bool begin, toml::parse_error& err) {
    auto& st = *static_cast<parse_state*>(user);
    st.line = err.line;
    if (begin) {
        st.in = inline_state{};
        st.in.open = true;
        st.in.line = err.line;
        std::snprintf(st.in.key, sizeof(st.in.key), "%s", key);
        if (st.kind != section_kind::bindings) {
            st.in.ignored = true;
            if (st.kind != section_kind::unknown) {
                char fk[128];
                full_key(st.section, key, fk, sizeof(fk));
                st.diag->add(severity::error, st.layer, err.line, fk,
                             "an inline table is only valid in [bindings]; ignored");
            }
        }
        return true;
    }
    const inline_state in = st.in;
    st.in = inline_state{};
    if (in.ignored)
        return true;
    if (!in.have_action) {
        char fk[96];
        std::snprintf(fk, sizeof(fk), "bindings.%s", in.key);
        st.diag->add(severity::error, st.layer, in.line, fk,
                     "a binding table needs action = \"…\"; binding ignored");
        return true;
    }
    add_binding(st, in.key, in.line, in.action, in.argument, in.options);
    return true;
}

} // namespace

// ── config_text ──

bool config_text::add_layer(std::string_view path, std::string_view content) {
    if (layers.size() == k_max_layers)
        return false;
    layer_info info{};
    std::snprintf(info.path, sizeof(info.path), "%.*s", static_cast<int>(path.size()),
                  path.data());
    bytes.append(k_layer_marker);
    bytes.append(info.path);
    bytes.push_back('\n');
    info.begin = bytes.size();
    bytes.append(content);
    info.end = bytes.size();
    // The next marker starts a line of its own.
    if (!content.empty() && content.back() != '\n')
        bytes.push_back('\n');
    layers.push_back(info);
    return true;
}

config_text split_layers(std::string_view bytes) {
    config_text t{};
    t.bytes.assign(bytes);
    const std::size_t size = t.bytes.size();
    layer_info cur{};
    bool open = false;
    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t nl = t.bytes.find('\n', pos);
        const std::size_t line_end = nl == std::string::npos ? size : nl;
        const std::string_view line{t.bytes.data() + pos, line_end - pos};
        if (line.starts_with(k_layer_marker)) {
            if (open && t.layers.size() < k_max_layers) {
                cur.end = pos;
                t.layers.push_back(cur);
            }
            cur = layer_info{};
            const std::string_view p = line.substr(k_layer_marker.size());
            std::snprintf(cur.path, sizeof(cur.path), "%.*s", static_cast<int>(p.size()),
                          p.data());
            cur.begin = line_end < size ? line_end + 1 : size;
            open = true;
        } else if (!open) {
            // Text before any marker: a layer of its own, with no path.
            cur = layer_info{};
            cur.begin = pos;
            open = true;
        }
        pos = line_end + 1;
    }
    if (open && t.layers.size() < k_max_layers) {
        cur.end = size;
        t.layers.push_back(cur);
    }
    return t;
}

bool parse_config(const config_text& text, config_snapshot& out, diagnostics& diag,
                  const config_snapshot* previous) {
    auto cfg = std::make_unique<config_snapshot>(default_config());
    auto user = std::make_unique<std::inplace_vector<user_binding, k_max_bindings>>();
    for (unsigned li = 0; li < text.layers.size(); ++li) {
        const layer_info& layer = text.layers[li];
        parse_state st{};
        st.out = cfg.get();
        st.previous = previous;
        st.diag = &diag;
        st.layer = li;
        st.user = user.get();
        toml::handler h{};
        h.user = &st;
        h.table = &on_table;
        h.key_value = &on_key_value;
        h.inline_table = &on_inline;
        toml::parse_error err{};
        const std::size_t end = std::min(layer.end, text.bytes.size());
        const std::size_t begin = std::min(layer.begin, end);
        if (!toml::parse(text.bytes.data() + begin, end - begin, h, err)) {
            diag.addf(severity::error, li, err.line, "", "{}; the configuration is not used",
                      err.message);
            return false;
        }
        finish_device(st);
    }
    // The bindings: the defaults (unless turned off), then every file's over them.
    if (!cfg->default_bindings)
        cfg->bindings.clear();
    for (const user_binding& u : *user) {
        auto it = std::ranges::find_if(cfg->bindings,
                                       [&](const binding& b) { return b.keys == u.b.keys; });
        if (u.remove) {
            if (it != cfg->bindings.end())
                cfg->bindings.erase(it);
            continue;
        }
        if (it != cfg->bindings.end()) {
            *it = u.b;
        } else if (cfg->bindings.size() < k_max_bindings) {
            cfg->bindings.push_back(u.b);
        } else {
            diag.addf(severity::error, u.b.layer, u.b.line, "bindings",
                      "more than {} bindings; the rest are ignored", k_max_bindings);
            break;
        }
    }
    out = *cfg;
    return true;
}

void format_value(const key_desc& key, const config_snapshot& c, char* out, std::size_t cap) {
    format_field(key.kind, key.field(const_cast<config_snapshot&>(c)), key.names, key.name_count,
                 out, cap);
}

namespace {
#if defined(LUMEN_HAS_XKBCOMMON)
struct xkb_capture {
    char first[160]{};
};
void xkb_log(struct xkb_context* ctx, enum xkb_log_level level, const char* format,
             va_list args) {
    auto* cap = static_cast<xkb_capture*>(xkb_context_get_user_data(ctx));
    if (!cap || cap->first[0] || level > XKB_LOG_LEVEL_ERROR)
        return;
    std::vsnprintf(cap->first, sizeof(cap->first), format, args);
    // One line, no trailing newline.
    for (char* p = cap->first; *p; ++p)
        if (*p == '\n')
            *p = (p[1] ? ' ' : '\0');
}
#endif
} // namespace

bool check_keymap(const keyboard_settings& k, diagnostics& diag) {
#if defined(LUMEN_HAS_XKBCOMMON)
    xkb_capture cap{};
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return false;
    xkb_context_set_user_data(ctx, &cap);
    xkb_context_set_log_fn(ctx, &xkb_log);
    xkb_rule_names names{};
    names.rules = k.rules[0] ? k.rules : nullptr;
    names.model = k.model[0] ? k.model : nullptr;
    names.layout = k.layout[0] ? k.layout : nullptr;
    names.variant = k.variant[0] ? k.variant : nullptr;
    names.options = k.options[0] ? k.options : nullptr;
    xkb_keymap* map = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        diag.addf(severity::error, k_no_layer, 0, "input.keyboard",
                  "the keymap (layout \"{}\", variant \"{}\", options \"{}\") does not compile: "
                  "{}",
                  k.layout, k.variant, k.options, cap.first[0] ? cap.first : "xkbcommon said no more");
    }
    const bool ok = map != nullptr;
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);
    return ok;
#else
    (void)k;
    (void)diag;
    return false;
#endif
}

} // namespace lx::config
