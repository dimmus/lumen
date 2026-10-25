module;

import lx.foundation;

export module lx.ui:events;

export namespace lx::ui {

class widget;

enum class mouse_button { left, middle, right };
enum class key {
    unknown,
    tab,
    backspace,
    enter,
    escape,
    left,
    right,
    up,
    down,
    space,
    home,
    end,
};

/// `key_event::modifiers` bits: xkb's real modifier masks (what `wl_keyboard.modifiers`
/// carries as `mods_depressed | mods_latched | mods_locked`).
inline constexpr unsigned k_mod_shift = 1u << 0;
inline constexpr unsigned k_mod_ctrl = 1u << 2;
inline constexpr unsigned k_mod_alt = 1u << 3;
inline constexpr unsigned k_mod_super = 1u << 6;

struct mouse_event {
    lx::point2i position{};
    mouse_button button = mouse_button::left;
    bool pressed = false;
    unsigned click_count = 1;
};

struct key_event {
    key key = key::unknown;
    unsigned keycode = 0;
    bool pressed = false;
    unsigned modifiers = 0;
};

struct resize_event {
    lx::size2i old_size{};
    lx::size2i new_size{};
};

[[nodiscard]] key_event from_xkb_keycode(unsigned keycode, bool pressed, unsigned modifiers);

} // namespace lx::ui


lx::ui::key_event lx::ui::from_xkb_keycode(unsigned keycode, bool pressed, unsigned modifiers) {
    key_event e{};
    e.keycode = keycode;
    e.pressed = pressed;
    e.modifiers = modifiers;
    return e;
}
