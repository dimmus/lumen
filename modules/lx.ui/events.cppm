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
};

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
