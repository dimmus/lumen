export module lx.compositor:keybindings;

import std;
import lx.config;

// The compositor's key bindings: its own (VT switching, the logout chord), the configured
// binding table (config.toml [bindings], design-session-and-wm C.3: combination → action,
// keys, buttons and taps) and the key grabs the shell registers over zlm_shell_v1 v4 (kept
// for compatibility, after the table). Wayland-free and allocation-free, UI affinity: a
// grab's owner is an opaque pointer (the zlm_key_grab_v1 resource), a binding is an entry of
// the table, and every decision is returned to the caller to execute.

export namespace lx::compositor {

/// zlm_key_grab_v1.modifier. Caps Lock and Num Lock are never part of a match.
enum key_modifier : unsigned {
    key_mod_shift = 0x1,
    key_mod_ctrl = 0x2,
    key_mod_alt = 0x4,
    key_mod_super = 0x8,
};
inline constexpr unsigned k_key_mods_all = 0xFu;

/// zlm_key_grab_v1.flag. `not_inhibitable` (v6): the grab fires also while the focused
/// surface inhibits shortcuts (keyboard-shortcuts-inhibit-unstable-v1).
enum key_grab_flag : unsigned { key_grab_tap = 0x1, key_grab_not_inhibitable = 0x2 };
inline constexpr unsigned k_key_grab_flags_all = 0x3u;

/// zlm_key_grab_v1.refusal (same values), `none` for an accepted grab.
enum class key_grab_refusal : unsigned { none = 0, reserved = 1, taken = 2, invalid = 3 };

/// The keysyms the bindings name (xkbcommon-keysyms.h values; that header stays out of
/// this partition so it remains free of input-library types).
namespace keysyms {
inline constexpr unsigned backspace = 0xff08;
inline constexpr unsigned terminate_server = 0xfed5;
inline constexpr unsigned return_key = 0xff0d;
inline constexpr unsigned escape = 0xff1b;
inline constexpr unsigned f1 = 0xffbe;
inline constexpr unsigned f12 = 0xffc9;
inline constexpr unsigned shift_l = 0xffe1;
inline constexpr unsigned shift_r = 0xffe2;
inline constexpr unsigned control_l = 0xffe3;
inline constexpr unsigned control_r = 0xffe4;
inline constexpr unsigned alt_l = 0xffe9;
inline constexpr unsigned alt_r = 0xffea;
inline constexpr unsigned super_l = 0xffeb;
inline constexpr unsigned super_r = 0xffec;
inline constexpr unsigned switch_vt_1 = 0x1008fe01;
inline constexpr unsigned switch_vt_12 = 0x1008fe0c;
} // namespace keysyms

/// One key event, as the bindings see it.
struct key_input {
    unsigned code = 0; ///< evdev
    bool pressed = false;
    unsigned time_ms = 0;
    /// The keysym with the current modifiers and layout (`XF86Switch_VT_2` for Ctrl+Alt+F2
    /// under the default keymap).
    unsigned sym = 0;
    /// First shift level in the active layout, and in layout 0.
    unsigned raw = 0;
    unsigned raw0 = 0;
    /// `key_modifier` bits held, this key's own effect included.
    unsigned mods = 0;
    /// Another key or a pointer button is down besides this one.
    bool others_held = false;
};

enum class key_action : unsigned char { none, switch_vt, logout, grab, binding };

/// One configured binding (lx::config::binding without its origin).
struct binding_entry {
    lx::config::combo keys{};
    lx::config::action_id action = lx::config::action_id::none;
    unsigned argument = 0;
    unsigned char options = 0;
};

struct key_decision {
    /// The key is the compositor's: neither its press nor (through the router's
    /// forwarded-key rule) its release reaches a client.
    bool consume = false;
    key_action action = key_action::none;
    unsigned vt = 0;      ///< switch_vt
    void* grab = nullptr; ///< grab: its owner
    const binding_entry* binding = nullptr; ///< binding: the entry (valid until set_bindings)
};

class keybindings {
public:
    static constexpr unsigned k_max_grabs = 32;
    static constexpr unsigned k_max_bindings = lx::config::k_max_bindings;

    /// Replaces the configured table (a reload). Disarms a tap; entries returned before are
    /// invalid after.
    void set_bindings(std::span<const binding_entry> table);
    [[nodiscard]] unsigned binding_count() const { return static_cast<unsigned>(bindings_.size()); }
    /// A press of a configured key combination: exact modifiers, the key at its first level
    /// (as `match_grab`). Locked: only `allow-when-locked` bindings; inhibited (the focused
    /// surface inhibits shortcuts): only `allow-when-inhibited` ones.
    [[nodiscard]] key_decision match_binding(const key_input& in, bool inhibited = false);
    /// A configured pointer-button binding for evdev `button` with exactly `mods` held, or
    /// null. Never while locked (no pointer action is lock-safe).
    [[nodiscard]] const binding_entry* match_button(unsigned button, unsigned mods,
                                                    bool inhibited = false) const;
    /// The tap `observe_tap` fired, when it was a configured binding's (else null: a grab's).
    [[nodiscard]] const binding_entry* tap_binding(void* fired) const;
    /// A shell that takes actions (zlm_shell_v1 v7) is bound. Without one, bindings to shell
    /// actions do not match: the key goes on to a legacy shell's grabs (v4–v6) or to the
    /// client, rather than being swallowed for nobody — except switcher.next, which the
    /// compositor runs as a quick switch then.
    void set_shell_actions(bool deliverable) {
        shell_actions_ = deliverable;
        tap_owner_ = nullptr;
    }
    [[nodiscard]] bool shell_actions() const { return shell_actions_; }

    /// Super+Escape, exactly (no other modifier): the compositor's way back from a client
    /// that inhibits shortcuts — it toggles the focused surface's inhibitor — and, with
    /// pointer constraints, releases the pointer. Never inhibitable; consumed only when it
    /// acts (the caller decides), so a client without an inhibitor still gets the key.
    [[nodiscard]] static bool escape_hatch(const key_input& in);

    /// Compositor-internal bindings, checked on every press before anything else (also
    /// during a window-manager grab, also with no shell): XF86Switch_VT_1…12 → switch to
    /// that VT (consumed even when switching is impossible or it is our own VT, as sway
    /// does), Ctrl+Alt+BackSpace → logout. Keysyms, not keycodes: a keymap that disables
    /// VT switching (srvrkeys:none) is respected.
    [[nodiscard]] static key_decision internal(const key_input& in, bool can_switch_vt,
                                               unsigned own_vt);

    /// Validates a grab request: `invalid` for unknown bits or a tap grab that is not a
    /// bare modifier key, `reserved` for a combination `internal` keeps (and Ctrl+Alt+F1…F12,
    /// which produce the VT keysyms under the default keymap).
    [[nodiscard]] static key_grab_refusal check(unsigned keysym, unsigned mods, unsigned flags);
    /// `check`, then `taken` when another grab holds the same combination, or when the
    /// table is full; otherwise records it under `owner`.
    key_grab_refusal add_grab(unsigned keysym, unsigned mods, unsigned flags, void* owner);
    /// The owner went (its resource was destroyed): the combination returns to clients.
    void remove_grab(void* owner);
    [[nodiscard]] unsigned grab_count() const { return static_cast<unsigned>(grabs_.size()); }

    /// Whether some key produces `keysym` at its first level in the active layout — which
    /// decides whether a layout-0 match counts. Without one, only the active layout matches.
    using layout_query = bool (*)(unsigned keysym, void* user);
    void set_layout_query(layout_query query, void* user) {
        layout_query_ = query;
        layout_user_ = user;
    }

    /// A press of a (non-tap) grabbed combination: exact modifiers. Disarms a tap. With
    /// `inhibited` (the focused surface inhibits shortcuts) only `not_inhibitable` grabs
    /// match: the key goes to the client.
    [[nodiscard]] key_decision match_grab(const key_input& in, bool inhibited = false);

    /// Tap observation, for every key that reaches this point. A press of a tap grab's key
    /// with nothing else held arms it; any other press disarms. The release of the armed
    /// key returns the grab to fire (the key itself still goes to the client). With
    /// `inhibited`, only `not_inhibitable` taps arm.
    [[nodiscard]] void* observe_tap(const key_input& in, bool inhibited = false);
    /// A pointer button press or a scroll while the tap key is held: not a tap.
    void note_other_input() { tap_owner_ = nullptr; }
    /// Session pause, or a consumed key: nothing is armed.
    void reset() { tap_owner_ = nullptr; }
    /// The session is locked (ext-session-lock-v1): no grab matches and no tap fires —
    /// keys belong to the lock surface; the shell's bindings (the launcher, Super+Return)
    /// must not be usable behind the lock. `internal` bindings are unaffected (VT switch,
    /// logout: the keyboard ways out, which end the session without revealing it).
    void set_locked(bool locked) {
        locked_ = locked;
        tap_owner_ = nullptr;
    }
    [[nodiscard]] bool locked() const { return locked_; }
    [[nodiscard]] bool tap_armed() const { return tap_owner_ != nullptr; }

    // ── Held actions (zlm_shell_v1 v7 held_action, design-session-and-wm K.1) ──

    /// What a key event does during a hold (`on_hold_key`).
    enum class hold_step : unsigned char {
        /// Goes on as usual: a modifier's own press, a release while the hold's modifier is
        /// still down, or no hold.
        none,
        /// A key press: to the hold (its `key` event), never to the focused client — so the
        /// router never forwards it and its release reaches nobody either.
        divert,
        /// The hold's modifier is up: the hold ends (`released`); the event itself goes on (the
        /// client saw that modifier's press).
        released,
    };
    /// A held binding fired: from now on, while its primary modifier stays down, key presses
    /// go to `owner` (the zlm_held_action_v1 resource) instead of the focused client. The
    /// primary modifier is the binding's highest one (Super, then Alt, Ctrl, Shift: GNOME's
    /// `primaryModifier`), so Alt+Shift+Tab ends when Alt is let go whatever Shift does.
    /// Disarms a tap.
    void begin_hold(unsigned binding_mods, void* owner);
    [[nodiscard]] bool hold_active() const { return hold_owner_ != nullptr; }
    [[nodiscard]] void* hold_owner() const { return hold_owner_; }
    /// The modifier whose release ends the hold (one `key_modifier` bit).
    [[nodiscard]] unsigned hold_mods() const { return hold_mods_; }
    /// Ends the hold (released, destroyed, cancelled); returns the owner it had (null: none).
    void* end_hold();
    [[nodiscard]] hold_step on_hold_key(const key_input& in);
    /// The configured key binding `in` would run (exact modifiers, the same filters as
    /// `match_binding`), or null — without consuming anything or touching a tap. During a
    /// hold the shell is told which action a diverted key is bound to.
    [[nodiscard]] const binding_entry* peek_binding(const key_input& in, bool inhibited = false) const;
    /// `b` may run now (the lock, shortcuts inhibition and shell filters of `match_binding`).
    /// A repeating binding asks before every repeat: a lock that came while its key is held
    /// stops it unless it is allowed when locked.
    [[nodiscard]] bool allows(const binding_entry& b, bool inhibited = false) const {
        return !filtered(b, inhibited);
    }

private:
    struct grab {
        unsigned keysym = 0;
        unsigned mods = 0;
        unsigned flags = 0;
        void* owner = nullptr;
    };
    [[nodiscard]] bool keysym_matches(unsigned grab_sym, const key_input& in) const;

    [[nodiscard]] bool filtered(const binding_entry& b, bool inhibited) const;

    std::inplace_vector<grab, k_max_grabs> grabs_{};
    std::inplace_vector<binding_entry, k_max_bindings> bindings_{};
    void* tap_owner_ = nullptr;
    void* hold_owner_ = nullptr;
    unsigned hold_mods_ = 0;
    unsigned tap_code_ = 0;
    bool shell_actions_ = false;
    bool locked_ = false;
    layout_query layout_query_ = nullptr;
    void* layout_user_ = nullptr;
};

} // namespace lx::compositor

namespace {

[[nodiscard]] bool is_modifier_keysym(unsigned sym) {
    using namespace lx::compositor::keysyms;
    return sym == super_l || sym == super_r || sym == alt_l || sym == alt_r ||
           sym == control_l || sym == control_r || sym == shift_l || sym == shift_r;
}

} // namespace

lx::compositor::key_decision lx::compositor::keybindings::internal(const key_input& in,
                                                                   bool can_switch_vt,
                                                                   unsigned own_vt) {
    key_decision d{};
    if (!in.pressed)
        return d;
    if (in.sym >= keysyms::switch_vt_1 && in.sym <= keysyms::switch_vt_12) {
        d.consume = true;
        const unsigned vt = in.sym - keysyms::switch_vt_1 + 1;
        if (can_switch_vt && vt != own_vt) {
            d.action = key_action::switch_vt;
            d.vt = vt;
        }
        return d;
    }
    const bool chord = (in.mods & key_mod_ctrl) && (in.mods & key_mod_alt);
    if (chord && (in.sym == keysyms::backspace || in.raw == keysyms::backspace ||
                  in.sym == keysyms::terminate_server)) {
        d.consume = true;
        d.action = key_action::logout;
    }
    return d;
}

lx::compositor::key_grab_refusal lx::compositor::keybindings::check(unsigned keysym,
                                                                    unsigned mods,
                                                                    unsigned flags) {
    if ((mods & ~k_key_mods_all) || (flags & ~k_key_grab_flags_all) || keysym == 0)
        return key_grab_refusal::invalid;
    if ((flags & key_grab_tap) && (!is_modifier_keysym(keysym) || mods != 0))
        return key_grab_refusal::invalid;
    if (keysym >= keysyms::switch_vt_1 && keysym <= keysyms::switch_vt_12)
        return key_grab_refusal::reserved;
    // The escape hatch from an inhibiting client must stay the compositor's.
    if (keysym == keysyms::escape && mods == key_mod_super)
        return key_grab_refusal::reserved;
    const bool chord = (mods & key_mod_ctrl) && (mods & key_mod_alt);
    if (chord && (keysym == keysyms::backspace || keysym == keysyms::terminate_server ||
                  (keysym >= keysyms::f1 && keysym <= keysyms::f12)))
        return key_grab_refusal::reserved;
    return key_grab_refusal::none;
}

lx::compositor::key_grab_refusal lx::compositor::keybindings::add_grab(unsigned keysym,
                                                                       unsigned mods,
                                                                       unsigned flags,
                                                                       void* owner) {
    if (const auto r = check(keysym, mods, flags); r != key_grab_refusal::none)
        return r;
    if (!owner)
        return key_grab_refusal::invalid;
    const bool tap = (flags & key_grab_tap) != 0;
    for (const grab& g : grabs_) {
        if (g.keysym == keysym && g.mods == mods && ((g.flags & key_grab_tap) != 0) == tap)
            return key_grab_refusal::taken;
    }
    // A full table is reported as taken: the protocol has no other word for "no room", and
    // 32 grabs is far past what a shell registers.
    if (grabs_.size() == k_max_grabs)
        return key_grab_refusal::taken;
    grabs_.push_back({keysym, mods, flags, owner});
    return key_grab_refusal::none;
}

void lx::compositor::keybindings::remove_grab(void* owner) {
    if (!owner)
        return;
    for (unsigned i = 0; i < grabs_.size();) {
        if (grabs_[i].owner == owner) {
            grabs_[i] = grabs_.back();
            grabs_.pop_back();
            continue;
        }
        ++i;
    }
    if (tap_owner_ == owner)
        tap_owner_ = nullptr;
}

bool lx::compositor::keybindings::keysym_matches(unsigned grab_sym, const key_input& in) const {
    if (in.raw == grab_sym)
        return true;
    // Layout 0 stands in only when the active layout has no key for the keysym at all — so
    // Super+Return still works under a Cyrillic layout, and a layout that does have the
    // keysym elsewhere is not overridden by layout 0's position.
    return in.raw0 == grab_sym && in.raw != grab_sym && layout_query_ &&
           !layout_query_(grab_sym, layout_user_);
}

void lx::compositor::keybindings::set_bindings(std::span<const binding_entry> table) {
    bindings_.clear();
    for (const binding_entry& b : table) {
        if (bindings_.size() == k_max_bindings)
            break;
        bindings_.push_back(b);
    }
    tap_owner_ = nullptr;
}

bool lx::compositor::keybindings::filtered(const binding_entry& b, bool inhibited) const {
    // With no shell to take them, shell actions step aside — except switcher.next, which the
    // compositor then runs itself as a quick switch (design C.3).
    if (!shell_actions_ && lx::config::action(b.action).owner == lx::config::action_owner::shell &&
        b.action != lx::config::action_id::switcher_next)
        return true;
    if (locked_ && !(b.options & lx::config::bo_allow_when_locked))
        return true;
    return inhibited && !(b.options & lx::config::bo_allow_when_inhibited);
}

lx::compositor::key_decision lx::compositor::keybindings::match_binding(const key_input& in,
                                                                        bool inhibited) {
    key_decision d{};
    if (!in.pressed)
        return d;
    for (const binding_entry& b : bindings_) {
        if (b.keys.kind != lx::config::combo_kind::key || b.keys.mods != in.mods ||
            !keysym_matches(b.keys.code, in) || filtered(b, inhibited))
            continue;
        tap_owner_ = nullptr;
        d.consume = true;
        d.action = key_action::binding;
        d.binding = &b;
        return d;
    }
    return d;
}

const lx::compositor::binding_entry* lx::compositor::keybindings::match_button(
    unsigned button, unsigned mods, bool inhibited) const {
    if (locked_)
        return nullptr;
    for (const binding_entry& b : bindings_)
        if (b.keys.kind == lx::config::combo_kind::button && b.keys.code == button &&
            b.keys.mods == mods && !filtered(b, inhibited))
            return &b;
    return nullptr;
}

const lx::compositor::binding_entry* lx::compositor::keybindings::tap_binding(void* fired) const {
    if (!fired || bindings_.empty())
        return nullptr;
    const auto* p = static_cast<const binding_entry*>(fired);
    return p >= bindings_.data() && p < bindings_.data() + bindings_.size() ? p : nullptr;
}

bool lx::compositor::keybindings::escape_hatch(const key_input& in) {
    return in.pressed && in.mods == key_mod_super &&
           (in.sym == keysyms::escape || in.raw == keysyms::escape);
}

lx::compositor::key_decision lx::compositor::keybindings::match_grab(const key_input& in,
                                                                     bool inhibited) {
    key_decision d{};
    if (!in.pressed || locked_)
        return d;
    for (const grab& g : grabs_) {
        if ((g.flags & key_grab_tap) || g.mods != in.mods || !keysym_matches(g.keysym, in))
            continue;
        if (inhibited && !(g.flags & key_grab_not_inhibitable))
            continue;
        tap_owner_ = nullptr;
        d.consume = true;
        d.action = key_action::grab;
        d.grab = g.owner;
        return d;
    }
    return d;
}

void* lx::compositor::keybindings::observe_tap(const key_input& in, bool inhibited) {
    if (in.pressed) {
        tap_owner_ = nullptr;
        if (in.others_held)
            return nullptr;
        // Configured taps first (a modifier key alone: Super → the launcher).
        for (const binding_entry& b : bindings_) {
            if (b.keys.kind != lx::config::combo_kind::tap || filtered(b, inhibited))
                continue;
            const unsigned bit = (in.raw == keysyms::super_l || in.raw == keysyms::super_r) ? key_mod_super
                                 : (in.raw == keysyms::alt_l || in.raw == keysyms::alt_r) ? key_mod_alt
                                 : (in.raw == keysyms::control_l || in.raw == keysyms::control_r)
                                     ? key_mod_ctrl
                                 : (in.raw == keysyms::shift_l || in.raw == keysyms::shift_r)
                                     ? key_mod_shift
                                     : 0u;
            if (bit && bit == b.keys.mods) {
                tap_owner_ = const_cast<binding_entry*>(&b);
                tap_code_ = in.code;
                return nullptr;
            }
        }
        if (locked_)
            return nullptr;
        for (const grab& g : grabs_) {
            if ((g.flags & key_grab_tap) && keysym_matches(g.keysym, in) &&
                (!inhibited || (g.flags & key_grab_not_inhibitable))) {
                tap_owner_ = g.owner;
                tap_code_ = in.code;
                break;
            }
        }
        return nullptr;
    }
    if (!tap_owner_ || in.code != tap_code_)
        return nullptr;
    void* fire = tap_owner_;
    tap_owner_ = nullptr;
    return fire;
}

void lx::compositor::keybindings::begin_hold(unsigned binding_mods, void* owner) {
    // The highest modifier of the binding: Super > Alt > Ctrl > Shift.
    unsigned primary = 0;
    for (unsigned bit : {unsigned{key_mod_super}, unsigned{key_mod_alt}, unsigned{key_mod_ctrl},
                         unsigned{key_mod_shift}})
        if (binding_mods & bit) {
            primary = bit;
            break;
        }
    hold_owner_ = primary ? owner : nullptr;
    hold_mods_ = primary;
    tap_owner_ = nullptr;
}

void* lx::compositor::keybindings::end_hold() {
    void* owner = hold_owner_;
    hold_owner_ = nullptr;
    hold_mods_ = 0;
    return owner;
}

lx::compositor::keybindings::hold_step lx::compositor::keybindings::on_hold_key(
    const key_input& in) {
    if (!hold_owner_)
        return hold_step::none;
    if (in.pressed)
        // Shift pressed for Alt+Shift+Tab updates the client's modifiers like any modifier;
        // every other key is the hold's.
        return is_modifier_keysym(in.raw) || is_modifier_keysym(in.raw0) ? hold_step::none
                                                                         : hold_step::divert;
    return (in.mods & hold_mods_) == 0 ? hold_step::released : hold_step::none;
}

const lx::compositor::binding_entry* lx::compositor::keybindings::peek_binding(
    const key_input& in, bool inhibited) const {
    if (!in.pressed)
        return nullptr;
    for (const binding_entry& b : bindings_)
        if (b.keys.kind == lx::config::combo_kind::key && b.keys.mods == in.mods &&
            keysym_matches(b.keys.code, in) && !filtered(b, inhibited))
            return &b;
    return nullptr;
}
