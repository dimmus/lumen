module;

// lx.wayland.client:seat — wl_seat with its pointer, keyboard and touch as raw events:
// pointer events folded per wl_pointer.frame, the keymap fd compiled into an xkbcommon
// keymap and state (keysyms, text, modifiers), repeat info passed on (the repeat timer is the
// app's), touch points as they come. Cursor shapes through wp_cursor_shape_v1, text input
// through zwp_text_input_v3. Bodies in seat.cpp.

struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_touch;
struct wp_cursor_shape_device_v1;
struct zwp_text_input_v3;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

export module lx.wayland.client:seat;

import std;

import lx.foundation;
import :types;

export namespace lx::wayland {

/// One wl_seat. Owned by the display (bound with the registry, gone with its global); the
/// app sets a handler. UI affinity.
class seat {
public:
    seat() = default;
    ~seat();
    seat(const seat&) = delete;
    seat& operator=(const seat&) = delete;

    void set_handler(seat_handler* h) { handler_ = h; }
    [[nodiscard]] std::uint32_t capabilities() const { return caps_; }
    [[nodiscard]] const char* name() const { return name_; }
    [[nodiscard]] std::uint32_t global_name() const { return global_name_; }
    [[nodiscard]] display* owner() const { return display_; }

    /// The serial of the last pointer enter (what set_cursor / cursor-shape and a
    /// move/resize want), and of the last input event of any device (press, key, touch).
    [[nodiscard]] std::uint32_t pointer_enter_serial() const { return enter_serial_; }
    [[nodiscard]] std::uint32_t last_input_serial() const { return input_serial_; }
    [[nodiscard]] surface* pointer_focus() const { return pointer_focus_; }
    [[nodiscard]] surface* keyboard_focus() const { return keyboard_focus_; }

    /// The pointer's cursor, by shape (wp_cursor_shape_device_v1; false without it). Sent
    /// against the last enter serial.
    bool set_cursor_shape(cursor_shape shape);

    /// The compiled keymap (null before the first wl_keyboard.keymap).
    [[nodiscard]] bool has_keymap() const { return keymap_ != nullptr; }
    [[nodiscard]] std::uint32_t modifiers() const { return modifiers_; }
    [[nodiscard]] std::int32_t repeat_rate() const { return repeat_rate_; }
    [[nodiscard]] std::int32_t repeat_delay_ms() const { return repeat_delay_; }
    /// `xkb_keymap*` / `xkb_state*` as `void*`, for the app's own lookups.
    [[nodiscard]] void* native_keymap() const { return keymap_; }
    [[nodiscard]] void* native_state() const { return state_; }
    [[nodiscard]] void* native() const { return proxy_; }
    [[nodiscard]] void* native_pointer() const { return pointer_; }

    // Lifecycle (the display calls these).
    void bind(display* d, wl_seat* proxy, std::uint32_t global_name, std::uint32_t version);
    void release();
    [[nodiscard]] bool bound() const { return proxy_ != nullptr; }

    // Event entry points (listener thunks).
    void on_capabilities(std::uint32_t caps);
    void on_name(const char* name);
    void on_pointer_enter(std::uint32_t serial, void* wl_surface, double x, double y);
    void on_pointer_leave(std::uint32_t serial, void* wl_surface);
    void on_pointer_motion(std::uint32_t time, double x, double y);
    void on_pointer_button(std::uint32_t serial, std::uint32_t time, std::uint32_t button,
                           std::uint32_t state);
    void on_pointer_axis(std::uint32_t time, std::uint32_t axis, double value);
    void on_pointer_frame();
    void on_pointer_axis_source(std::uint32_t source);
    void on_pointer_axis_stop(std::uint32_t time, std::uint32_t axis);
    void on_pointer_axis_value120(std::uint32_t axis, std::int32_t value120);
    void on_pointer_axis_relative_direction(std::uint32_t axis, std::uint32_t direction);
    void on_keymap(std::uint32_t format, int fd, std::uint32_t size);
    void on_keyboard_enter(std::uint32_t serial, void* wl_surface, const std::uint32_t* keys,
                           std::size_t count);
    void on_keyboard_leave(std::uint32_t serial, void* wl_surface);
    void on_key(std::uint32_t serial, std::uint32_t time, std::uint32_t key, std::uint32_t state);
    void on_modifiers(std::uint32_t serial, std::uint32_t depressed, std::uint32_t latched,
                      std::uint32_t locked, std::uint32_t group);
    void on_repeat_info(std::int32_t rate, std::int32_t delay);
    void on_touch(const touch_event& e);

    /// Pointer events of frames that were never closed by `frame` (a wl_pointer < 5 has no
    /// frames: each event is delivered alone).
    void flush_pointer_frame();

private:
    void update_devices();
    void free_keymap();
    [[nodiscard]] std::uint32_t effective_modifiers() const;

    display* display_ = nullptr;
    wl_seat* proxy_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_touch* touch_ = nullptr;
    wp_cursor_shape_device_v1* cursor_shape_ = nullptr;
    seat_handler* handler_ = nullptr;
    std::uint32_t global_name_ = 0;
    std::uint32_t version_ = 0;
    std::uint32_t caps_ = 0;
    std::uint32_t enter_serial_ = 0;
    std::uint32_t input_serial_ = 0;
    surface* pointer_focus_ = nullptr;
    surface* keyboard_focus_ = nullptr;
    pointer_frame frame_{};
    xkb_context* xkb_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    xkb_state* state_ = nullptr;
    /// xkb modifier indices, resolved per keymap (~0u: absent).
    std::uint32_t mod_index_[6]{~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    std::uint32_t modifiers_ = 0;
    std::int32_t repeat_rate_ = 25;
    std::int32_t repeat_delay_ = 600;
    char name_[32]{};
};

/// zwp_text_input_v3 on a seat: enable / disable, surrounding text, content type, cursor
/// rectangle, commit (counted: `done`'s serial is compared with it), and the preedit /
/// commit / delete that each `done` applies, copied into fixed buffers. UI affinity.
class text_input {
public:
    text_input() = default;
    ~text_input();
    text_input(const text_input&) = delete;
    text_input& operator=(const text_input&) = delete;

    [[nodiscard]] lx::result<void> create(display& d, seat& s);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    void set_handler(text_input_handler* h) { handler_ = h; }

    // Pending state, applied by `commit`.
    void enable();
    void disable();
    /// UTF-8 text around the cursor (≤ 4000 bytes, the protocol's bound; cut at a character
    /// boundary otherwise), cursor and anchor as byte offsets into it.
    void set_surrounding_text(std::string_view text, std::int32_t cursor, std::int32_t anchor);
    /// zwp_text_input_v3.change_cause (input_method 0, other 1).
    void set_text_change_cause(std::uint32_t cause);
    void set_content_type(std::uint32_t hint, std::uint32_t purpose);
    /// Surface-local, logical pixels.
    void set_cursor_rectangle(rect2i r);
    /// v2: the actions the input method may offer (finish 1, submit 2, ...).
    void set_available_actions(std::span<const std::uint32_t> actions);
    void show_input_panel();
    void hide_input_panel();
    void commit();

    [[nodiscard]] std::uint32_t commit_count() const { return commits_; }
    [[nodiscard]] surface* focus() const { return focus_; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::uint32_t version() const { return version_; }
    /// Strings cut to `k_text_input_bytes` so far.
    [[nodiscard]] std::uint32_t truncated() const { return truncated_; }

    // Event entry points.
    void on_enter(void* wl_surface);
    void on_leave(void* wl_surface);
    void on_preedit(const char* text, std::int32_t begin, std::int32_t end);
    void on_commit_string(const char* text);
    void on_delete_surrounding(std::uint32_t before, std::uint32_t after);
    void on_done(std::uint32_t serial);
    void on_action(std::uint32_t action, std::uint32_t serial);

private:
    zwp_text_input_v3* proxy_ = nullptr;
    text_input_handler* handler_ = nullptr;
    surface* focus_ = nullptr;
    std::uint32_t version_ = 0;
    std::uint32_t commits_ = 0;
    std::uint32_t truncated_ = 0;
    bool enabled_ = false;
    bool pending_enabled_ = false;
    // Pending per `done`.
    bool has_preedit_ = false;
    bool has_commit_ = false;
    bool has_delete_ = false;
    std::int32_t preedit_begin_ = -1;
    std::int32_t preedit_end_ = -1;
    std::uint32_t delete_before_ = 0;
    std::uint32_t delete_after_ = 0;
    char preedit_[k_text_input_bytes]{};
    char commit_[k_text_input_bytes]{};
};

} // namespace lx::wayland
