module;

import lx.foundation;
import lx.runtime;

export module lx.input;

export namespace lx::input {

enum class button { left, middle, right, side, extra };
enum class axis { x, y, horizontal_scroll, vertical_scroll };

struct pointer_event {
    lx::surface_id surface{};
    lx::point2i position{};
    button button{};
    bool pressed = false;
    unsigned serial = 0;
};

struct keyboard_event {
    lx::surface_id surface{};
    unsigned key_code = 0;
    bool pressed = false;
    unsigned serial = 0;
};

class keyboard {
public:
    void set_focus(lx::surface_id surface);
    [[nodiscard]] lx::surface_id focus() const;
    void set_keymap(const char* layout, const char* variant);
};

class pointer {
public:
    [[nodiscard]] lx::point2i position() const;
    [[nodiscard]] lx::surface_id focus() const;
};

class seat {
public:
    [[nodiscard]] lx::seat_id id() const;
    [[nodiscard]] pointer& get_pointer();
    [[nodiscard]] keyboard& get_keyboard();

    void notify(pointer_event event);
    void notify(keyboard_event event);
    void process_events();

private:
    lx::seat_id id_{1};
    pointer pointer_{};
    keyboard keyboard_{};
};

class input_manager {
public:
    [[nodiscard]] static lx::result<input_manager> open();
    void poll();
    [[nodiscard]] seat& default_seat();

private:
    seat default_seat_{};
};

} // namespace lx::input

module :private;

void lx::input::keyboard::set_focus(lx::surface_id) {}
lx::surface_id lx::input::keyboard::focus() const { return {}; }
void lx::input::keyboard::set_keymap(const char*, const char*) {}
lx::point2i lx::input::pointer::position() const { return {}; }
lx::surface_id lx::input::pointer::focus() const { return {}; }
lx::seat_id lx::input::seat::id() const { return id_; }
lx::input::pointer& lx::input::seat::get_pointer() { return pointer_; }
lx::input::keyboard& lx::input::seat::get_keyboard() { return keyboard_; }
void lx::input::seat::notify(pointer_event) {}
void lx::input::seat::notify(keyboard_event) {}
void lx::input::seat::process_events() {}
lx::result<lx::input::input_manager> lx::input::input_manager::open() {
    return lx::not_implemented("lx::input::input_manager::open");
}
void lx::input::input_manager::poll() {}
lx::input::seat& lx::input::input_manager::default_seat() { return default_seat_; }
