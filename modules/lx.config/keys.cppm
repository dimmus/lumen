module;

// Key and button combinations as the configuration names them (design-session-and-wm C.3):
// `Mod+Mod+Key`, modifiers `Super|Logo|Mod4`, `Ctrl|Control`, `Alt|Mod1`, `Shift`
// (case-insensitive); the key an xkb keysym name (exact, then case-insensitive; letters are
// matched at their first shift level, so `Super+L` is the l key), a mouse button
// (`BTN_LEFT` … `BTN_EXTRA`), or the combination is a bare modifier (`Super`): a tap.
// Wayland-free; keysym names come from xkbcommon when it is there, else from a small table
// of the common ones.

#if defined(LUMEN_HAS_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

export module lx.config:keys;

import std;

export namespace lx::config {

/// The modifier bits (zlm_key_grab_v1.modifier's values). Caps Lock and Num Lock never take
/// part in a match.
enum key_mod : unsigned {
    mod_shift = 0x1,
    mod_ctrl = 0x2,
    mod_alt = 0x4,
    mod_super = 0x8,
};
inline constexpr unsigned k_mods_all = 0xFu;

enum class combo_kind : unsigned char {
    /// A key with exactly `mods` held; `code` is its keysym at the first shift level.
    key,
    /// A pointer button with exactly `mods` held; `code` is the evdev code (BTN_LEFT 0x110).
    button,
    /// A modifier pressed and released alone; `mods` is that modifier's bit, `code` 0.
    tap,
};

struct combo {
    combo_kind kind = combo_kind::key;
    unsigned mods = 0;
    unsigned code = 0;
    [[nodiscard]] bool operator==(const combo&) const = default;
};

/// The keysyms the reserved check names (xkbcommon-keysyms.h values).
namespace keysym {
inline constexpr unsigned backspace = 0xff08;
inline constexpr unsigned tab = 0xff09;
inline constexpr unsigned return_key = 0xff0d;
inline constexpr unsigned escape = 0xff1b;
inline constexpr unsigned terminate_server = 0xfed5;
inline constexpr unsigned f1 = 0xffbe;
inline constexpr unsigned f12 = 0xffc9;
inline constexpr unsigned switch_vt_1 = 0x1008fe01;
inline constexpr unsigned switch_vt_12 = 0x1008fe0c;
} // namespace keysym

/// evdev button codes a combination may name.
inline constexpr unsigned k_btn_left = 0x110;
inline constexpr unsigned k_btn_right = 0x111;
inline constexpr unsigned k_btn_middle = 0x112;
inline constexpr unsigned k_btn_side = 0x113;
inline constexpr unsigned k_btn_extra = 0x114;

/// Parses `text`. False with `error` set (a static string) when it is not a combination.
[[nodiscard]] bool parse_combo(std::string_view text, combo& out, const char*& error);
/// The canonical spelling ("Super+Shift+Return", "Super+BTN_LEFT", "Super").
void format_combo(const combo& c, char* out, std::size_t cap);
/// A keysym by name, as `parse_combo` reads it (lowercased at the first level); 0: none.
[[nodiscard]] unsigned keysym_from_name(std::string_view name);
/// The keysym's name into `out` (empty when unknown).
void keysym_name(unsigned sym, char* out, std::size_t cap);

/// A combination the compositor keeps for itself, never configurable: the VT keysyms,
/// Ctrl+Alt+F1…F12, Ctrl+Alt+BackSpace (and Terminate_Server), Super+Escape — the keyboard
/// ways out (lx.compositor's `keybindings::internal` and `escape_hatch`).
[[nodiscard]] bool reserved(const combo& c);
/// The same rule for a raw keysym and modifier set (zlm_shell_v1.grab_key).
[[nodiscard]] bool reserved_key(unsigned keysym, unsigned mods);

} // namespace lx::config


namespace {

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

/// The modifier a name stands for (0: not a modifier name).
[[nodiscard]] unsigned modifier_named(std::string_view s) {
    if (iequal(s, "super") || iequal(s, "logo") || iequal(s, "mod4") || iequal(s, "win"))
        return lx::config::mod_super;
    if (iequal(s, "ctrl") || iequal(s, "control"))
        return lx::config::mod_ctrl;
    if (iequal(s, "alt") || iequal(s, "mod1"))
        return lx::config::mod_alt;
    if (iequal(s, "shift"))
        return lx::config::mod_shift;
    return 0;
}

struct named_button {
    const char* name;
    unsigned code;
};
constexpr named_button k_buttons[] = {
    {"BTN_LEFT", lx::config::k_btn_left},   {"BTN_RIGHT", lx::config::k_btn_right},
    {"BTN_MIDDLE", lx::config::k_btn_middle}, {"BTN_SIDE", lx::config::k_btn_side},
    {"BTN_EXTRA", lx::config::k_btn_extra},
};

/// Without xkbcommon: the names a binding usually uses.
struct named_keysym {
    const char* name;
    unsigned sym;
};
[[maybe_unused]] constexpr named_keysym k_fallback_keysyms[] = {
    {"Return", 0xff0d},    {"Tab", 0xff09},       {"Escape", 0xff1b},    {"BackSpace", 0xff08},
    {"Delete", 0xffff},    {"space", 0x20},       {"grave", 0x60},       {"Home", 0xff50},
    {"End", 0xff57},       {"Left", 0xff51},      {"Up", 0xff52},        {"Right", 0xff53},
    {"Down", 0xff54},      {"Page_Up", 0xff55},   {"Prior", 0xff55},     {"Page_Down", 0xff56},
    {"Next", 0xff56},      {"Print", 0xff61},     {"Insert", 0xff63},    {"minus", 0x2d},
    {"equal", 0x3d},       {"comma", 0x2c},       {"period", 0x2e},      {"slash", 0x2f},
    {"semicolon", 0x3b},   {"apostrophe", 0x27},  {"bracketleft", 0x5b}, {"bracketright", 0x5d},
    {"backslash", 0x5c},   {"Super_L", 0xffeb},   {"Super_R", 0xffec},   {"Alt_L", 0xffe9},
    {"Alt_R", 0xffea},     {"Control_L", 0xffe3}, {"Control_R", 0xffe4}, {"Shift_L", 0xffe1},
    {"Shift_R", 0xffe2},   {"Terminate_Server", 0xfed5},
};

} // namespace

unsigned lx::config::keysym_from_name(std::string_view name) {
    if (name.empty() || name.size() >= 64)
        return 0;
    // One printable character names its own keysym ('+', '1', 'a'): Latin-1 keysyms are the
    // characters' codes.
    if (name.size() == 1 && name[0] > 0x20 && name[0] < 0x7f) {
        const char ch = name[0];
        return static_cast<unsigned>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    char buf[64];
    std::memcpy(buf, name.data(), name.size());
    buf[name.size()] = '\0';
#if defined(LUMEN_HAS_XKBCOMMON)
    xkb_keysym_t sym = xkb_keysym_from_name(buf, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(buf, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol)
        return 0;
    // A binding names the key at its first shift level: `L` is the l key.
    return xkb_keysym_to_lower(sym);
#else
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z')
            return static_cast<unsigned>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return static_cast<unsigned>(c);
    }
    if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3) {
        unsigned n = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9')
                return 0;
            n = n * 10 + static_cast<unsigned>(name[i] - '0');
        }
        if (n >= 1 && n <= 24)
            return 0xffbe + n - 1;
    }
    for (const auto& k : k_fallback_keysyms)
        if (iequal(name, k.name))
            return k.sym;
    return 0;
#endif
}

void lx::config::keysym_name(unsigned sym, char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    // xkbcommon names these Prior and Next (their first names); nobody writes that.
    if (sym == 0xff55 || sym == 0xff56) {
        std::snprintf(out, cap, "%s", sym == 0xff55 ? "Page_Up" : "Page_Down");
        return;
    }
#if defined(LUMEN_HAS_XKBCOMMON)
    if (xkb_keysym_get_name(sym, out, cap) < 0)
        out[0] = '\0';
#else
    if ((sym >= 'a' && sym <= 'z') || (sym >= '0' && sym <= '9')) {
        std::snprintf(out, cap, "%c", static_cast<char>(sym));
        return;
    }
    if (sym >= 0xffbe && sym <= 0xffd5) {
        std::snprintf(out, cap, "F%u", sym - 0xffbe + 1);
        return;
    }
    for (const auto& k : k_fallback_keysyms) {
        if (k.sym == sym) {
            std::snprintf(out, cap, "%s", k.name);
            return;
        }
    }
    std::snprintf(out, cap, "0x%x", sym);
#endif
}

bool lx::config::parse_combo(std::string_view text, combo& out, const char*& error) {
    out = combo{};
    error = nullptr;
    // Trim.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    if (text.empty()) {
        error = "empty combination";
        return false;
    }
    unsigned mods = 0;
    std::string_view rest = text;
    std::string_view last{};
    for (;;) {
        const std::size_t plus = rest.find('+');
        // "Super++" is Super and the plus key: a '+' right after '+' (or at the end) is the
        // key itself.
        if (plus == std::string_view::npos || plus + 1 == rest.size()) {
            last = rest;
            break;
        }
        std::string_view part = rest.substr(0, plus);
        while (!part.empty() && part.back() == ' ')
            part.remove_suffix(1);
        while (!part.empty() && part.front() == ' ')
            part.remove_prefix(1);
        const unsigned m = modifier_named(part);
        if (!m) {
            error = part.empty() ? "empty part in a combination" : "unknown modifier";
            return false;
        }
        if (mods & m) {
            error = "a modifier named twice";
            return false;
        }
        mods |= m;
        rest = rest.substr(plus + 1);
    }
    while (!last.empty() && last.front() == ' ')
        last.remove_prefix(1);
    if (last.empty()) {
        error = "a combination needs a key";
        return false;
    }
    if (const unsigned m = modifier_named(last); m) {
        if (mods != 0) {
            // "Super+Shift" as a tap would need two keys alone: not a tap, not a key.
            error = "a tap is one modifier alone (\"Super\"); a modifier cannot be the key";
            return false;
        }
        out.kind = combo_kind::tap;
        out.mods = m;
        return true;
    }
    for (const auto& b : k_buttons) {
        if (iequal(last, b.name)) {
            out.kind = combo_kind::button;
            out.mods = mods;
            out.code = b.code;
            return true;
        }
    }
    if (last.size() > 4 && iequal(last.substr(0, 4), "BTN_")) {
        error = "unknown button (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA)";
        return false;
    }
    const unsigned sym = keysym_from_name(last);
    if (!sym) {
        error = "unknown key name (an xkb keysym name, e.g. Return, F4, a, Page_Down)";
        return false;
    }
    out.kind = combo_kind::key;
    out.mods = mods;
    out.code = sym;
    return true;
}

void lx::config::format_combo(const combo& c, char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    std::size_t n = 0;
    const auto put = [&](const char* s) {
        const int w = std::snprintf(out + n, cap - n, "%s", s);
        if (w > 0)
            n = std::min(cap - 1, n + static_cast<std::size_t>(w));
    };
    out[0] = '\0';
    if (c.kind == combo_kind::tap) {
        put(c.mods == mod_super ? "Super"
            : c.mods == mod_alt ? "Alt"
            : c.mods == mod_ctrl ? "Ctrl"
                                 : "Shift");
        return;
    }
    if (c.mods & mod_super)
        put("Super+");
    if (c.mods & mod_ctrl)
        put("Ctrl+");
    if (c.mods & mod_alt)
        put("Alt+");
    if (c.mods & mod_shift)
        put("Shift+");
    if (c.kind == combo_kind::button) {
        for (const auto& b : k_buttons)
            if (b.code == c.code)
                put(b.name);
        return;
    }
    char name[64];
    keysym_name(c.code, name, sizeof(name));
    if (!name[0])
        std::snprintf(name, sizeof(name), "0x%x", c.code);
    put(name);
}

bool lx::config::reserved_key(unsigned sym, unsigned mods) {
    if (sym >= keysym::switch_vt_1 && sym <= keysym::switch_vt_12)
        return true;
    if (sym == keysym::escape && mods == mod_super)
        return true;
    const bool chord = (mods & mod_ctrl) && (mods & mod_alt);
    return chord && (sym == keysym::backspace || sym == keysym::terminate_server ||
                     (sym >= keysym::f1 && sym <= keysym::f12));
}

bool lx::config::reserved(const combo& c) {
    return c.kind == combo_kind::key && reserved_key(c.code, c.mods);
}
