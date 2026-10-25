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
    page_up,
    page_down,
    /// The forward delete key ("Delete"; `delete` is C++'s).
    delete_forward,
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
    /// The character the key types (xkb's keysym → UTF-32 with the modifiers applied), 0 for
    /// none: what a text input inserts, and what ctrl-shortcuts (ctrl-c: 'c') are read from.
    char32_t codepoint = 0;
};

/// What a pointer did to the node it is routed to (`ui_node::on_pointer`). Positions are
/// surface coordinates; `local_point` maps one into a node's layout space (inside a scrolled
/// viewport the two differ).
enum class pointer_kind : unsigned char {
    /// The left button went down on the node (the pressed node; after focus moved there).
    press,
    /// The pointer moved while the node is the pressed one (wherever the pointer is).
    drag,
    /// The left button went up; the node was the pressed one (before any activation).
    release,
    /// A scroll wheel or touchpad axis over the node: offered to it, then each ancestor,
    /// until one takes it.
    wheel,
    /// The secondary (right) button went down over the node: offered up the ancestors (a
    /// context menu's area takes it).
    secondary,
};

struct pointer_event {
    pointer_kind kind = pointer_kind::press;
    lx::point2i at{};
    mouse_button button = mouse_button::left;
    /// `k_mod_*` bits held at the event.
    unsigned modifiers = 0;
    /// 1 for a click, 2 for a double click, 3 for a triple (presses within 400 ms and 4 px).
    unsigned clicks = 1;
    /// Wheel: surface pixels to scroll by, positive down / right.
    float dx = 0.f;
    float dy = 0.f;
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
