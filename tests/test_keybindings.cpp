// The compositor's key bindings: VT switching and the logout chord (internal), the shell's
// key grabs (exact modifiers, Caps/Num ignored, the layout-0 fallback for non-Latin layouts,
// reserved and taken refusals) and the Super tap (fires on release only when nothing else
// happened while it was held). Pure, plus a real xkb keymap for the layout cases.
#include "lumen_test.hpp"

import std;
import lx.compositor;
import lx.input;
import lx.config;

using namespace lx::compositor;

namespace {

int g_a = 0, g_b = 0, g_c = 0;
void* const A = &g_a;
void* const B = &g_b;
void* const C = &g_c;

key_input press(unsigned code, unsigned raw, unsigned mods = 0, unsigned sym = 0) {
    key_input in{};
    in.code = code;
    in.pressed = true;
    in.raw = raw;
    in.raw0 = raw;
    in.sym = sym ? sym : raw;
    in.mods = mods;
    return in;
}
key_input release(unsigned code, unsigned raw, unsigned mods = 0) {
    key_input in = press(code, raw, mods);
    in.pressed = false;
    return in;
}

constexpr unsigned k_key_f2 = 60;
constexpr unsigned k_key_backspace = 14;
constexpr unsigned k_key_enter = 28;
constexpr unsigned k_key_leftmeta = 125;
constexpr unsigned k_key_a = 30;
constexpr unsigned k_key_c = 46;

} // namespace

LUMEN_TEST(vt_keysyms_switch) {
    for (unsigned n = 1; n <= 12; ++n) {
        const key_input in = press(59, keysyms::f1, key_mod_ctrl | key_mod_alt,
                                   keysyms::switch_vt_1 + n - 1);
        const key_decision d = keybindings::internal(in, true, 1);
        LUMEN_CHECK(d.consume);
        if (n == 1) {
            LUMEN_CHECK(d.action == key_action::none); // our own VT: consumed, nothing
        } else {
            LUMEN_CHECK(d.action == key_action::switch_vt && d.vt == n);
        }
    }
    // No VTs on the seat: still the compositor's key, nothing happens.
    const key_decision none = keybindings::internal(
        press(k_key_f2, keysyms::f1 + 1, key_mod_ctrl | key_mod_alt, keysyms::switch_vt_1 + 1),
        false, 1);
    LUMEN_CHECK(none.consume && none.action == key_action::none);
    // Plain F2 is a client's.
    LUMEN_CHECK(!keybindings::internal(press(k_key_f2, keysyms::f1 + 1), true, 1).consume);
    // Releases never bind.
    LUMEN_CHECK(!keybindings::internal(release(k_key_f2, keysyms::f1 + 1), true, 1).consume);
}

LUMEN_TEST(ctrl_alt_backspace_logs_out) {
    const key_decision d = keybindings::internal(
        press(k_key_backspace, keysyms::backspace, key_mod_ctrl | key_mod_alt), true, 1);
    LUMEN_CHECK(d.consume && d.action == key_action::logout);
    // With Shift as well (the old binding allowed it).
    LUMEN_CHECK(keybindings::internal(press(k_key_backspace, keysyms::backspace,
                                            key_mod_ctrl | key_mod_alt | key_mod_shift),
                                      true, 1)
                    .consume);
    // Ctrl+Backspace alone is a client's.
    LUMEN_CHECK(!keybindings::internal(press(k_key_backspace, keysyms::backspace, key_mod_ctrl),
                                       true, 1)
                     .consume);
}

LUMEN_TEST(grab_refusals) {
    keybindings kb{};
    // Reserved: the compositor's own.
    LUMEN_CHECK(kb.add_grab(keysyms::backspace, key_mod_ctrl | key_mod_alt, 0, A) ==
                key_grab_refusal::reserved);
    LUMEN_CHECK(kb.add_grab(keysyms::switch_vt_1, 0, 0, A) == key_grab_refusal::reserved);
    LUMEN_CHECK(kb.add_grab(keysyms::f1 + 3, key_mod_ctrl | key_mod_alt, 0, A) ==
                key_grab_refusal::reserved);
    // Invalid: unknown bits, a tap that is not a bare modifier.
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, 0x10, 0, A) == key_grab_refusal::invalid);
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, 0, 0x4, A) == key_grab_refusal::invalid);
    // Super+Escape: the escape hatch from a client that inhibits shortcuts.
    LUMEN_CHECK(kb.add_grab(keysyms::escape, key_mod_super, 0, A) == key_grab_refusal::reserved);
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, 0, key_grab_tap, A) == key_grab_refusal::invalid);
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, key_mod_super, key_grab_tap, A) ==
                key_grab_refusal::invalid);
    LUMEN_CHECK(kb.grab_count() == 0);
    // Taken.
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, A) == key_grab_refusal::none);
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, B) == key_grab_refusal::taken);
    // Same key, other modifiers: a different combination.
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super | key_mod_shift, 0, B) ==
                key_grab_refusal::none);
    // The owner goes: the combination is free again.
    kb.remove_grab(A);
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, C) == key_grab_refusal::none);
    LUMEN_CHECK(kb.grab_count() == 2);
}

LUMEN_TEST(grab_matches_exact_modifiers) {
    keybindings kb{};
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, A) == key_grab_refusal::none);
    const key_decision d = kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super));
    LUMEN_CHECK(d.consume && d.action == key_action::grab && d.grab == A);
    // Extra or missing modifiers do not match (Caps/Num never reach `mods`).
    LUMEN_CHECK(!kb.match_grab(press(k_key_enter, keysyms::return_key,
                                     key_mod_super | key_mod_shift))
                     .consume);
    LUMEN_CHECK(!kb.match_grab(press(k_key_enter, keysyms::return_key)).consume);
    // A release never matches (its press was consumed; the router swallows it).
    LUMEN_CHECK(!kb.match_grab(release(k_key_enter, keysyms::return_key, key_mod_super)).consume);
    // Tap grabs are not taken by match_grab.
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, B) == key_grab_refusal::none);
    LUMEN_CHECK(!kb.match_grab(press(k_key_leftmeta, keysyms::super_l, key_mod_super)).consume);
}

LUMEN_TEST(inhibited_skips_grabs_but_not_inhibitable) {
    keybindings kb{};
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, A) == key_grab_refusal::none);
    // The lock key: v6 not_inhibitable.
    LUMEN_CHECK(kb.add_grab('l', key_mod_super, key_grab_not_inhibitable, B) ==
                key_grab_refusal::none);
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, C) == key_grab_refusal::none);
    // Inhibited: Super+Return goes to the client, Super+L still locks.
    LUMEN_CHECK(!kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super), true).consume);
    const key_decision l = kb.match_grab(press(38, 'l', key_mod_super), true);
    LUMEN_CHECK(l.consume && l.grab == B);
    // The Super tap neither arms nor fires while inhibited.
    LUMEN_CHECK(kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super), true) ==
                nullptr);
    LUMEN_CHECK(!kb.tap_armed());
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l), true) == nullptr);
    // Not inhibited: as before.
    LUMEN_CHECK(kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super)).consume);
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == C);
    // A not_inhibitable tap arms while inhibited.
    keybindings kb2{};
    LUMEN_CHECK(kb2.add_grab(keysyms::super_l, 0, key_grab_tap | key_grab_not_inhibitable, A) ==
                key_grab_refusal::none);
    (void)kb2.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super), true);
    LUMEN_CHECK(kb2.observe_tap(release(k_key_leftmeta, keysyms::super_l), true) == A);
}

LUMEN_TEST(escape_hatch_is_exactly_super_escape) {
    constexpr unsigned k_key_esc = 1;
    LUMEN_CHECK(keybindings::escape_hatch(press(k_key_esc, keysyms::escape, key_mod_super)));
    LUMEN_CHECK(!keybindings::escape_hatch(release(k_key_esc, keysyms::escape, key_mod_super)));
    LUMEN_CHECK(!keybindings::escape_hatch(press(k_key_esc, keysyms::escape)));
    LUMEN_CHECK(!keybindings::escape_hatch(
        press(k_key_esc, keysyms::escape, key_mod_super | key_mod_shift)));
    // Not an internal binding by itself: whether it acts is the compositor's (an inhibitor or
    // a pointer constraint on the focused surface).
    LUMEN_CHECK(!keybindings::internal(press(k_key_esc, keysyms::escape, key_mod_super), true, 1)
                     .consume);
}

LUMEN_TEST(no_grab_or_tap_while_locked) {
    keybindings kb{};
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, A) == key_grab_refusal::none);
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, B) == key_grab_refusal::none);
    // A tap armed before the lock does not fire after it.
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    kb.set_locked(true);
    LUMEN_CHECK(kb.locked() && !kb.tap_armed());
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // Locked: no grab matches, no tap arms or fires; the keys belong to the lock surface.
    LUMEN_CHECK(!kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super)).consume);
    LUMEN_CHECK(kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super)) == nullptr);
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // The compositor's own bindings stay: the keyboard ways out.
    LUMEN_CHECK(keybindings::internal(
                    press(k_key_backspace, keysyms::backspace, key_mod_ctrl | key_mod_alt), true, 1)
                    .consume);
    // Unlocked: as before.
    kb.set_locked(false);
    LUMEN_CHECK(kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super)).grab == A);
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == B);
}

LUMEN_TEST(layout0_fallback_with_query) {
    keybindings kb{};
    static bool active_has = false;
    kb.set_layout_query([](unsigned, void*) { return active_has; }, nullptr);
    LUMEN_CHECK(kb.add_grab('c', key_mod_super, 0, A) == key_grab_refusal::none);
    key_input in = press(k_key_c, 0x6f3 /* Cyrillic_es */, key_mod_super);
    in.raw0 = 'c';
    active_has = false; // the active layout has no 'c': layout 0 stands in
    LUMEN_CHECK(kb.match_grab(in).consume);
    active_has = true; // it has one elsewhere: that key is the binding, not this one
    LUMEN_CHECK(!kb.match_grab(in).consume);
    // Without a query only the active layout counts.
    kb.set_layout_query(nullptr, nullptr);
    LUMEN_CHECK(!kb.match_grab(in).consume);
}

LUMEN_TEST(tap_fires_on_lone_release) {
    keybindings kb{};
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, A) == key_grab_refusal::none);
    LUMEN_CHECK(kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super)) == nullptr);
    LUMEN_CHECK(kb.tap_armed());
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == A);
    LUMEN_CHECK(!kb.tap_armed());
    // Once only.
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
}

LUMEN_TEST(tap_cancelled_by_other_input) {
    keybindings kb{};
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, A) == key_grab_refusal::none);
    // Super+key.
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    (void)kb.observe_tap(press(k_key_a, 'a', key_mod_super));
    (void)kb.observe_tap(release(k_key_a, 'a', key_mod_super));
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // Super+button (Super+drag).
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    kb.note_other_input();
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // Super+scroll.
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    kb.note_other_input();
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // Pressed while another key was already held: never armed.
    key_input held = press(k_key_leftmeta, keysyms::super_l, key_mod_super);
    held.others_held = true;
    (void)kb.observe_tap(held);
    LUMEN_CHECK(!kb.tap_armed());
    // A consumed grab press disarms too.
    LUMEN_CHECK(kb.add_grab(keysyms::return_key, key_mod_super, 0, B) == key_grab_refusal::none);
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    LUMEN_CHECK(kb.match_grab(press(k_key_enter, keysyms::return_key, key_mod_super)).grab == B);
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // The grab's owner going disarms.
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    kb.remove_grab(A);
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
    // A session pause resets.
    LUMEN_CHECK(kb.add_grab(keysyms::super_l, 0, key_grab_tap, C) == key_grab_refusal::none);
    (void)kb.observe_tap(press(k_key_leftmeta, keysyms::super_l, key_mod_super));
    kb.reset();
    LUMEN_CHECK(kb.observe_tap(release(k_key_leftmeta, keysyms::super_l)) == nullptr);
}

LUMEN_TEST(real_keymap_layout_fallback) {
    lx::input::keyboard_keymap km;
    lx::input::keymap_config cfg{};
    cfg.layout = "us,ru";
    if (auto r = km.compile(cfg); !r) {
        std::printf("SKIP: no xkb data for us,ru\n");
        return;
    }
    // Lock group 1 (ru) as if the user had switched layouts.
    LUMEN_CHECK(km.update_mask({.depressed = 0, .latched = 0, .locked = 0, .group = 1}));
    LUMEN_CHECK(km.active_layout() == 1);
    const unsigned raw = km.level1_keysym(k_key_c, km.active_layout());
    const unsigned raw0 = km.level1_keysym(k_key_c, 0);
    LUMEN_CHECK(raw0 == 'c' && raw != 'c' && raw != 0);
    LUMEN_CHECK(!km.layout_has_keysym('c', 1));
    LUMEN_CHECK(km.layout_has_keysym('c', 0));
    LUMEN_CHECK(km.layout_has_keysym(keysyms::return_key, 1));

    keybindings kb{};
    kb.set_layout_query(
        [](unsigned sym, void* u) {
            auto* k = static_cast<lx::input::keyboard_keymap*>(u);
            return k->layout_has_keysym(sym, k->active_layout());
        },
        &km);
    LUMEN_CHECK(kb.add_grab('c', key_mod_super, 0, A) == key_grab_refusal::none);
    key_input in = press(k_key_c, raw, key_mod_super);
    in.raw0 = raw0;
    LUMEN_CHECK(kb.match_grab(in).grab == A);
}

LUMEN_TEST(xkb_vt_keysyms_from_the_default_keymap) {
    lx::input::keyboard_keymap km;
    lx::input::keymap_config cfg{};
    cfg.layout = "us";
    if (auto r = km.compile(cfg); !r) {
        std::printf("SKIP: no xkb data\n");
        return;
    }
    // Ctrl+Alt+F2 produces XF86Switch_VT_2 under xkeyboard-config's default (srvrkeys).
    (void)km.update_key(29, true);  // KEY_LEFTCTRL
    (void)km.update_key(56, true);  // KEY_LEFTALT
    (void)km.update_key(k_key_f2, true);
    LUMEN_CHECK(km.keysym(k_key_f2) == keysyms::switch_vt_1 + 1);
    LUMEN_CHECK(km.level1_keysym(k_key_f2, km.active_layout()) == keysyms::f1 + 1);
    // Every key let go at once (a pause): Caps Lock would survive, the held ones do not.
    LUMEN_CHECK(km.reset_depressed());
    LUMEN_CHECK(km.modifiers().depressed == 0);
    LUMEN_CHECK(km.keysym(k_key_f2) == keysyms::f1 + 1);
}

// ── The configured binding table (config.toml [bindings], design-session-and-wm C.3) ──

namespace {

binding_entry entry(const char* keys, lx::config::action_id action, unsigned char options = 0) {
    binding_entry e{};
    const char* error = nullptr;
    (void)lx::config::parse_combo(keys, e.keys, error);
    e.action = action;
    e.options = options;
    return e;
}

using lx::config::action_id;

} // namespace

LUMEN_TEST(bindings_match_exact_modifiers_and_first_level) {
    keybindings k;
    const binding_entry table[] = {entry("Alt+F4", action_id::window_close),
                                   entry("Super+Shift+Left", action_id::window_move_to_output_left)};
    k.set_bindings(table);
    LUMEN_CHECK(k.binding_count() == 2);
    const key_decision d = k.match_binding(press(62, 0xffc1 /* F4 */, key_mod_alt));
    LUMEN_CHECK(d.consume && d.action == key_action::binding && d.binding &&
                d.binding->action == action_id::window_close);
    LUMEN_CHECK(!k.match_binding(press(62, 0xffc1, key_mod_alt | key_mod_shift)).consume);
    LUMEN_CHECK(!k.match_binding(press(62, 0xffc1, 0)).consume);
    LUMEN_CHECK(!k.match_binding(release(62, 0xffc1, key_mod_alt)).consume);
    const key_decision m =
        k.match_binding(press(105, 0xff51 /* Left */, key_mod_super | key_mod_shift));
    LUMEN_CHECK(m.consume && m.binding->action == action_id::window_move_to_output_left);
}

LUMEN_TEST(bindings_before_grabs_and_shell_actions_need_a_v7_shell) {
    keybindings k;
    const binding_entry table[] = {entry("Super+Return", action_id::shell_terminal)};
    k.set_bindings(table);
    LUMEN_CHECK(k.add_grab(keysyms::return_key, key_mod_super, 0, A) == key_grab_refusal::none);
    const key_input in = press(k_key_enter, keysyms::return_key, key_mod_super);
    // No shell takes actions: the binding steps aside, the legacy grab gets the key.
    LUMEN_CHECK(!k.match_binding(in).consume);
    LUMEN_CHECK(k.match_grab(in).grab == A);
    k.set_shell_actions(true);
    const key_decision d = k.match_binding(in);
    LUMEN_CHECK(d.consume && d.binding->action == action_id::shell_terminal);
}

LUMEN_TEST(binding_taps_and_their_owner) {
    keybindings k;
    k.set_shell_actions(true);
    const binding_entry table[] = {entry("Super", action_id::shell_launcher)};
    k.set_bindings(table);
    (void)k.observe_tap(press(k_key_leftmeta, keysyms::super_l));
    void* fired = k.observe_tap(release(k_key_leftmeta, keysyms::super_l));
    const binding_entry* b = k.tap_binding(fired);
    LUMEN_CHECK(fired && b && b->action == action_id::shell_launcher);
    // Another key meanwhile: no tap.
    (void)k.observe_tap(press(k_key_leftmeta, keysyms::super_l));
    (void)k.observe_tap(press(k_key_a, 'a', key_mod_super));
    LUMEN_CHECK(!k.observe_tap(release(k_key_leftmeta, keysyms::super_l)));
    // A grab's tap is not a binding's.
    keybindings g;
    LUMEN_CHECK(g.add_grab(keysyms::super_l, 0, key_grab_tap, A) == key_grab_refusal::none);
    (void)g.observe_tap(press(k_key_leftmeta, keysyms::super_l));
    void* gf = g.observe_tap(release(k_key_leftmeta, keysyms::super_l));
    LUMEN_CHECK(gf == A && !g.tap_binding(gf));
}

LUMEN_TEST(bindings_while_locked_and_inhibited) {
    keybindings k;
    k.set_shell_actions(true);
    const binding_entry table[] = {
        entry("Super+L", action_id::shell_lock, lx::config::bo_allow_when_inhibited),
        entry("Alt+F4", action_id::window_close),
        entry("Ctrl+Alt+Delete", action_id::session_logout, lx::config::bo_allow_when_locked),
    };
    k.set_bindings(table);
    const key_input lock = press(38, 'l', key_mod_super);
    const key_input close = press(62, 0xffc1, key_mod_alt);
    const key_input logout = press(111, 0xffff, key_mod_ctrl | key_mod_alt);
    // Inhibited: only allow-when-inhibited.
    LUMEN_CHECK(k.match_binding(lock, true).consume);
    LUMEN_CHECK(!k.match_binding(close, true).consume);
    // Locked: only allow-when-locked.
    k.set_locked(true);
    LUMEN_CHECK(!k.match_binding(lock).consume);
    LUMEN_CHECK(!k.match_binding(close).consume);
    LUMEN_CHECK(k.match_binding(logout).consume);
    k.set_locked(false);
    LUMEN_CHECK(k.match_binding(close).consume);
}

// A `repeat = true` binding matched before a lock asks again before each repeat: its timer
// used to run the action behind the lock screen until the key was let go.
LUMEN_TEST(a_held_binding_is_asked_again_after_a_lock) {
    keybindings k;
    const binding_entry table[] = {
        entry("Super+Right", action_id::workspace_next, lx::config::bo_repeat),
        entry("Ctrl+Alt+Delete", action_id::session_logout, lx::config::bo_allow_when_locked),
    };
    k.set_bindings(table);
    const key_decision d = k.match_binding(press(106, 0xff53 /* Right */, key_mod_super));
    LUMEN_CHECK(d.consume && d.binding && k.allows(*d.binding));
    k.set_locked(true);
    LUMEN_CHECK(!k.allows(*d.binding));
    LUMEN_CHECK(!k.allows(*d.binding, true));
    const key_decision l = k.match_binding(press(111, 0xffff, key_mod_ctrl | key_mod_alt));
    LUMEN_CHECK(l.consume && k.allows(*l.binding));
    k.set_locked(false);
    LUMEN_CHECK(k.allows(*d.binding) && !k.allows(*d.binding, true));
}

LUMEN_TEST(button_bindings) {
    keybindings k;
    const binding_entry table[] = {entry("Super+BTN_LEFT", action_id::window_move_interactive),
                                   entry("Super+BTN_RIGHT", action_id::window_resize_interactive)};
    k.set_bindings(table);
    const binding_entry* b = k.match_button(0x110, key_mod_super);
    LUMEN_CHECK(b && b->action == action_id::window_move_interactive);
    LUMEN_CHECK(!k.match_button(0x110, 0));
    LUMEN_CHECK(!k.match_button(0x110, key_mod_super | key_mod_shift));
    LUMEN_CHECK(k.match_button(0x111, key_mod_super)->action == action_id::window_resize_interactive);
    k.set_locked(true);
    LUMEN_CHECK(!k.match_button(0x110, key_mod_super));
}

LUMEN_TEST(binding_table_from_the_configuration) {
    // The default table: shell.lock only with a lock command (else Super+L reaches clients).
    lx::config::config_snapshot c = lx::config::default_config();
    std::inplace_vector<binding_entry, keybindings::k_max_bindings> t;
    binding_table(c, t);
    const auto has = [&](action_id a) {
        return std::ranges::any_of(t, [&](const binding_entry& e) { return e.action == a; });
    };
    LUMEN_CHECK(!has(action_id::shell_lock) && has(action_id::shell_terminal) &&
                has(action_id::window_move_interactive));
    std::snprintf(c.idle.lock_command, sizeof(c.idle.lock_command), "swaylock -f");
    binding_table(c, t);
    LUMEN_CHECK(has(action_id::shell_lock));
    // Layout 0 stands in under a non-Latin layout, as for grabs.
    keybindings k;
    k.set_bindings(t);
    k.set_shell_actions(true);
    key_input cyr = press(k_key_enter, 0, key_mod_super);
    cyr.raw = 0x6c1; // not Return in the active layout
    cyr.raw0 = keysyms::return_key;
    k.set_layout_query([](unsigned, void*) { return false; }, nullptr);
    LUMEN_CHECK(k.match_binding(cyr).consume);
}

// ── Held actions (zlm v7, design-session-and-wm K.1) ──

namespace {
constexpr unsigned k_key_tab = 15;
constexpr unsigned k_key_leftalt = 56;
constexpr unsigned k_key_leftshift = 42;
constexpr unsigned k_key_esc = 1;
constexpr unsigned k_sym_tab = 0xff09;
} // namespace

LUMEN_TEST(hold_diverts_presses_until_the_primary_modifier_goes_up) {
    keybindings k;
    k.set_shell_actions(true);
    int owner = 0;
    LUMEN_CHECK(!k.hold_active() && k.on_hold_key(press(k_key_tab, k_sym_tab, key_mod_alt)) ==
                                        keybindings::hold_step::none);
    // Alt+Shift+Tab: Alt is the modifier that ends it (GNOME's primaryModifier).
    k.begin_hold(key_mod_alt | key_mod_shift, &owner);
    LUMEN_CHECK(k.hold_active() && k.hold_owner() == &owner && k.hold_mods() == key_mod_alt);
    // Another Tab, Escape, a letter: the hold's. A modifier's own press goes on (the client
    // keeps its modifier state).
    LUMEN_CHECK(k.on_hold_key(press(k_key_tab, k_sym_tab, key_mod_alt | key_mod_shift)) ==
                keybindings::hold_step::divert);
    LUMEN_CHECK(k.on_hold_key(press(k_key_esc, keysyms::escape, key_mod_alt)) ==
                keybindings::hold_step::divert);
    LUMEN_CHECK(k.on_hold_key(press(k_key_leftshift, keysyms::shift_l, key_mod_alt | key_mod_shift)) ==
                keybindings::hold_step::none);
    // Shift up, Alt still down: nothing ends; Tab's release goes on (the router drops it).
    LUMEN_CHECK(k.on_hold_key(release(k_key_leftshift, keysyms::shift_l, key_mod_alt)) ==
                keybindings::hold_step::none);
    LUMEN_CHECK(k.on_hold_key(release(k_key_tab, k_sym_tab, key_mod_alt)) ==
                keybindings::hold_step::none);
    // Alt up: released (the event still goes on: the client saw Alt's press).
    LUMEN_CHECK(k.on_hold_key(release(k_key_leftalt, keysyms::alt_l, 0)) ==
                keybindings::hold_step::released);
    LUMEN_CHECK(k.end_hold() == &owner && !k.hold_active() && k.end_hold() == nullptr);
    // Super+Tab with Shift: Super is primary over Alt, Ctrl and Shift.
    k.begin_hold(key_mod_super | key_mod_alt | key_mod_shift, &owner);
    LUMEN_CHECK(k.hold_mods() == key_mod_super);
    (void)k.end_hold();
    // No modifier: no hold (a held binding always has one).
    k.begin_hold(0, &owner);
    LUMEN_CHECK(!k.hold_active());
}

LUMEN_TEST(hold_names_the_binding_of_a_diverted_key) {
    keybindings k;
    k.set_shell_actions(true);
    const binding_entry table[] = {entry("Alt+Tab", action_id::switcher_next),
                                   entry("Alt+Shift+Tab", action_id::switcher_previous),
                                   entry("Super", action_id::shell_launcher)};
    k.set_bindings(table);
    // A peek neither consumes nor disarms an armed tap.
    LUMEN_CHECK(k.observe_tap(press(k_key_leftmeta, keysyms::super_l)) == nullptr && k.tap_armed());
    const binding_entry* prev = k.peek_binding(press(k_key_tab, k_sym_tab, key_mod_alt | key_mod_shift));
    LUMEN_CHECK(prev && prev->action == action_id::switcher_previous && k.tap_armed());
    const binding_entry* next = k.peek_binding(press(k_key_tab, k_sym_tab, key_mod_alt));
    LUMEN_CHECK(next && next->action == action_id::switcher_next);
    LUMEN_CHECK(!k.peek_binding(press(k_key_tab, k_sym_tab, key_mod_ctrl)));
    LUMEN_CHECK(!k.peek_binding(release(k_key_tab, k_sym_tab, key_mod_alt)));
}

LUMEN_TEST(switcher_next_without_a_shell_is_the_compositors_quick_switch) {
    keybindings k;
    const binding_entry table[] = {entry("Alt+Tab", action_id::switcher_next),
                                   entry("Alt+Shift+Tab", action_id::switcher_previous),
                                   entry("Super+Return", action_id::shell_terminal)};
    k.set_bindings(table);
    // No v7 shell: switcher.next still matches (the compositor runs it), the other shell
    // actions step aside for the client.
    LUMEN_CHECK(k.match_binding(press(k_key_tab, k_sym_tab, key_mod_alt)).consume);
    LUMEN_CHECK(!k.match_binding(press(k_key_tab, k_sym_tab, key_mod_alt | key_mod_shift)).consume);
    LUMEN_CHECK(!k.match_binding(press(k_key_enter, keysyms::return_key, key_mod_super)).consume);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
