module;

// lx.app:input — a window's input routing (ADR-010 §4.9, §7 Phase 4): wl_pointer frames,
// wl_keyboard keys through xkb, key repeat, wl_touch and zwp_text_input_v3 turned into the
// calls `lx::ui::ui_root` takes (`pointer_move` / `pointer_button` / `pointer_axis` /
// `pointer_leave`, `key`, `ime()`), and the cursor shape the hovered node asks for sent
// through wp_cursor_shape_v1.
//
// Coalescing, as the compositor does it for its own pointer: lx.wayland.client already folds a
// wl_pointer.frame into one event; the router keeps only the latest motion and runs its hit
// test once — when the window's frame source asks (`flush_motion`, before a frame's tick), or
// before a button, axis or key, which must land where the pointer is. While a frame callback
// is outstanding, motions only overwrite the pending position: at most one hit-test pass per
// frame however fast the device reports.
//
// Key repeat is the client's on Wayland (wl_keyboard.repeat_info; GTK, Qt, winit and Chromium
// all run it): the router keeps the held key and its next deadline; the application arms its
// timerfd at that deadline only while a repeatable key is held. A compositor that repeats
// itself (wl_keyboard v10 `repeated`) is followed instead.
//
// Text input (zwp_text_input_v3) drives the focused field's `ime_target`: one `done` is one
// `ime_update` in the protocol's order. The field's state — surrounding text, caret, cursor
// rectangle — is sent when it changes (focus, typing, a caret move), never per frame; a `done`
// whose serial is not the client's commit count is applied and the state it produced held
// until a `done` whose serial matches (the protocol's rule), or until the user's next key.
//
// Touch is a pointer for v1: the first touch point presses, moves and releases the left
// button at its position; other points are ignored (a deviation: no multi-touch gestures).
//
// UI affinity throughout. Nothing here allocates per event.

export module lx.app:input;

import std;

import lx.foundation;
import lx.runtime;
import lx.ui;
import lx.wayland.client;

export namespace lx::app {

/// What the router did, summed since the window opened (tests and the bench).
struct input_stats {
    std::uint64_t pointer_frames = 0;   ///< wl_pointer.frame groups received
    std::uint64_t motions = 0;          ///< frames with a motion
    std::uint64_t motions_coalesced = 0; ///< motions overwritten before their hit test ran
    std::uint64_t hit_tests = 0;        ///< `ui_root::pointer_move` passes
    std::uint64_t buttons = 0;
    std::uint64_t axes = 0;
    std::uint64_t keys = 0;             ///< key presses and releases delivered
    std::uint64_t repeats = 0;          ///< repeated presses delivered (the app's timer or v10)
    std::uint64_t cursor_shapes = 0;    ///< wp_cursor_shape_device_v1.set_shape sent
    std::uint64_t touches = 0;
    std::uint64_t ime_commits = 0;      ///< zwp_text_input_v3.commit sent
    std::uint64_t ime_dones = 0;
    std::uint64_t ime_applied = 0;      ///< `ime_update`s applied to a field
    std::uint64_t ime_stale = 0;        ///< dones whose serial was not the commit count
};

/// Wheel detents: lines per detent, as Firefox and GTK scroll (3), and the line in px at
/// the theme's rem.
inline constexpr float k_wheel_lines = 3.f;

/// The input side of one window. UI affinity.
class input_router final : public lx::wayland::text_input_handler, public lx::ui::node_observer {
public:
    input_router() = default;
    ~input_router() override;
    input_router(const input_router&) = delete;
    input_router& operator=(const input_router&) = delete;

    /// Starts routing to `root` for `surface` (the window's), with a text input on `seat`
    /// when the compositor offers zwp_text_input_manager_v3.
    void bind(lx::wayland::display& display, lx::wayland::surface& surface,
              lx::ui::ui_root& root);
    void unbind();

    // ── seat events (the window forwards those whose target is its surface) ──
    void on_pointer(lx::wayland::seat& seat, const lx::wayland::pointer_frame& f);
    void on_keyboard_focus(lx::wayland::seat& seat, bool entered);
    void on_key(lx::wayland::seat& seat, const lx::wayland::key_event& e);
    void on_modifiers(lx::wayland::seat& seat, std::uint32_t modifiers);
    void on_repeat_info(lx::wayland::seat& seat, std::int32_t rate, std::int32_t delay_ms);
    void on_touch(lx::wayland::seat& seat, const lx::wayland::touch_event& e);

    // ── the frame loop's hooks ──

    /// A motion is waiting for its hit test.
    [[nodiscard]] bool motion_pending() const { return motion_pending_; }
    /// Runs the waiting motion's hit test (at most one), then the cursor shape.
    void flush_motion();
    /// After a frame's commit: the text input's state if it changed.
    void after_frame();

    /// The held key's next repeat, CLOCK_MONOTONIC ns; 0 when none is held.
    [[nodiscard]] std::uint64_t repeat_deadline_ns() const { return repeat_next_ns_; }
    /// The repeat timer expired at `now_ns`: one repeated press when it is due.
    void on_repeat_due(std::uint64_t now_ns);

    [[nodiscard]] const input_stats& stats() const { return stats_; }
    /// The pointer is over the window (surface-local logical px when it is).
    [[nodiscard]] bool pointer_inside() const { return inside_; }
    [[nodiscard]] lx::ui::cursor_style cursor() const { return cursor_; }
    [[nodiscard]] bool text_input_enabled() const { return ime_enabled_; }
    [[nodiscard]] lx::wayland::text_input& text_input() { return text_input_; }

    // text_input_handler
    void on_focus(lx::wayland::text_input& t, lx::wayland::surface* target, bool entered) override;
    void on_done(lx::wayland::text_input& t, const lx::wayland::text_input_done& d) override;

    // node_observer: the field the keyboard focus was on when the window lost it.
    void node_destroyed(lx::ui::ui_node& node) override;

private:
    void deliver_key(const lx::wayland::key_event& e, bool pressed);
    void update_cursor();
    /// Sends what changed of the focused field to the input method (`force`: everything).
    void sync_text_input(bool force);
    [[nodiscard]] std::uint64_t now_ns() const;

    lx::wayland::display* display_ = nullptr;
    lx::wayland::surface* surface_ = nullptr;
    lx::wayland::seat* seat_ = nullptr;
    lx::ui::ui_root* root_ = nullptr;
    lx::wayland::text_input text_input_{};

    // pointer
    bool inside_ = false;
    bool motion_pending_ = false;
    lx::point2i pending_{};
    bool cursor_sent_ = false;
    lx::ui::cursor_style cursor_ = lx::ui::cursor_style::arrow;
    bool cursor_warned_ = false;
    std::int32_t touch_id_ = -1;

    // keyboard
    unsigned modifiers_ = 0;
    std::int32_t repeat_rate_ = 25;
    std::int32_t repeat_delay_ms_ = 600;
    std::uint32_t repeat_key_ = 0;
    lx::wayland::key_event repeat_event_{};
    std::uint64_t repeat_next_ns_ = 0;
    bool compositor_repeats_ = false;
    /// The node that had the focus when the window lost the keyboard (restored on enter).
    lx::ui::ui_node* saved_focus_ = nullptr;
    bool saved_ring_ = false;

    // text input
    bool ime_focus_ = false;    ///< the compositor's enter for our surface
    bool ime_enabled_ = false;  ///< enable sent (and not disabled since)
    lx::ui::ime_target* ime_target_ = nullptr;
    std::uint32_t sent_size_ = 0;
    std::uint32_t sent_cursor_ = 0;
    std::uint32_t sent_anchor_ = 0;
    lx::rect2i sent_rect_{};
    std::uint32_t change_cause_ = 1; ///< zwp_text_input_v3 change_cause: 0 input_method, 1 other
    /// A done arrived whose serial was not our commit count: state held until a matching one.
    bool ime_deferred_ = false;
    char sent_text_[lx::wayland::k_text_input_bytes]{};

    input_stats stats_{};
};

} // namespace lx::app
