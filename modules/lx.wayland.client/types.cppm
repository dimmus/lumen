module;

// lx.wayland.client:types — the plain values the client wrapper hands out (events, output
// and configure state, dmabuf feedback) and the handler interfaces events are delivered
// through. No libwayland type appears here: proxies cross as `void*` (`native()`), as Vulkan
// handles do out of lx.gfx.

export module lx.wayland.client:types;

import std;

import lx.foundation;

export namespace lx::wayland {

class display;
class seat;
class surface;
class buffer;
class toplevel;
class popup;
class text_input;
class dmabuf_feedback;
class data_device;
class data_offer;
class data_source;

/// The globals the client runtime binds, each at a capped version it implements (the
/// bound version is `min(advertised, cap)`; `display::global(kind)` reports both).
enum class global_kind : unsigned {
    compositor,       ///< wl_compositor, cap 6 (damage_buffer ≥ 4, offset ≥ 5, preferred scale 6). Required.
    subcompositor,    ///< wl_subcompositor, cap 1.
    shm,              ///< wl_shm, cap 2 (release). Required.
    xdg_wm_base,      ///< xdg_wm_base, cap 7 (bounds 4, wm_capabilities 5, suspended 6, constrained 7). Required.
    decoration,       ///< zxdg_decoration_manager_v1, cap 2.
    dmabuf,           ///< zwp_linux_dmabuf_v1, cap 5 (feedback ≥ 4; v6's sampling device later).
    syncobj,          ///< wp_linux_drm_syncobj_manager_v1, cap 1.
    viewporter,       ///< wp_viewporter, cap 1.
    fractional_scale, ///< wp_fractional_scale_manager_v1, cap 1.
    presentation,     ///< wp_presentation, cap 2.
    text_input,       ///< zwp_text_input_manager_v3, cap 2 (actions, input panel).
    cursor_shape,     ///< wp_cursor_shape_manager_v1, cap 2.
    single_pixel,     ///< wp_single_pixel_buffer_manager_v1, cap 1.
    activation,       ///< xdg_activation_v1, cap 1.
    xdg_output,       ///< zxdg_output_manager_v1, cap 3.
    data_device_manager, ///< wl_data_device_manager, cap 3 (wl_data_device.release).
    primary_selection,   ///< zwp_primary_selection_device_manager_v1, cap 1.
    count,
};
inline constexpr unsigned k_global_kinds = static_cast<unsigned>(global_kind::count);

struct global_info {
    const char* interface = "";
    unsigned cap = 0;
    /// Registry name; 0 while not advertised.
    std::uint32_t name = 0;
    std::uint32_t advertised = 0;
    /// 0: not bound (absent, or its protocol glue was not built).
    std::uint32_t bound = 0;
    bool required = false;
};

/// What connect reached: a wl_display error after it ends the connection.
struct protocol_error_info {
    /// errno from wl_display_get_error (EPROTO for a protocol error); 0 while healthy.
    int display_errno = 0;
    /// The interface and object that raised it, and its code (EPROTO only).
    const char* interface = "";
    std::uint32_t object_id = 0;
    std::uint32_t code = 0;
};

// ── outputs ──

inline constexpr unsigned k_max_outputs = 16;

struct output_info {
    std::uint32_t global_name = 0;
    /// wl_output.geometry (compositor space; physical size in mm).
    point2i position{};
    size2i physical_mm{};
    std::int32_t subpixel = 0;
    std::int32_t transform = 0;
    /// The current mode and its refresh in mHz.
    size2i mode{};
    std::int32_t refresh_mhz = 0;
    std::int32_t scale = 1;
    /// zxdg_output_v1 (when bound): the logical rect.
    rect2i logical{};
    char name[32]{};
    char description[96]{};
    char make[32]{};
    char model[32]{};
    /// A wl_output.done has arrived since the last change.
    bool done = false;
};

// ── surfaces ──

/// wp_presentation_feedback.presented / .discarded for one commit.
struct presentation_event {
    /// The `commit_id` `surface::commit` returned for the commit this answers.
    std::uint64_t commit_id = 0;
    bool presented = false;
    /// CLOCK_* of `display::presentation_clock()`; nanoseconds since its epoch.
    std::uint64_t time_ns = 0;
    std::uint32_t refresh_ns = 0;
    std::uint64_t sequence = 0;
    /// wp_presentation_feedback kind bits (vsync 1, hw_clock 2, hw_completion 4, zero_copy 8).
    std::uint32_t flags = 0;
};

class surface_handler {
public:
    virtual ~surface_handler() = default;
    /// The frame callback requested with `surface::request_frame` fired (time in ms).
    virtual void on_frame(surface& s, std::uint32_t time_ms) {
        (void)s;
        (void)time_ms;
    }
    /// wl_surface.preferred_buffer_scale (v6) or wp_fractional_scale_v1.preferred_scale
    /// changed; `scale120` is in 1/120 (120 = 1.0), the integer scale × 120 without
    /// fractional scaling.
    virtual void on_preferred_scale(surface& s, std::uint32_t scale120) {
        (void)s;
        (void)scale120;
    }
    virtual void on_preferred_transform(surface& s, std::uint32_t transform) {
        (void)s;
        (void)transform;
    }
    /// The surface entered or left output slot `output` (`display::output(slot)`).
    virtual void on_output(surface& s, unsigned output, bool entered) {
        (void)s;
        (void)output;
        (void)entered;
    }
    virtual void on_presentation(surface& s, const presentation_event& e) {
        (void)s;
        (void)e;
    }
};

class buffer_handler {
public:
    virtual ~buffer_handler() = default;
    /// wl_buffer.release: the compositor no longer reads it.
    virtual void on_release(buffer& b) = 0;
    /// A zwp_linux_buffer_params_v1.failed for a dmabuf this buffer was created from.
    virtual void on_failed(buffer& b) { (void)b; }
};

// ── xdg_shell ──

/// xdg_toplevel.state bits as received (the protocol's enum value N is bit N-1).
enum toplevel_state_bit : std::uint32_t {
    state_maximized = 1u << 0,
    state_fullscreen = 1u << 1,
    state_resizing = 1u << 2,
    state_activated = 1u << 3,
    state_tiled_left = 1u << 4,
    state_tiled_right = 1u << 5,
    state_tiled_top = 1u << 6,
    state_tiled_bottom = 1u << 7,
    state_suspended = 1u << 8,
    state_constrained_left = 1u << 9,
    state_constrained_right = 1u << 10,
    state_constrained_top = 1u << 11,
    state_constrained_bottom = 1u << 12,
};

/// xdg_toplevel.wm_capabilities bits (v5): what the compositor's window menu, maximise,
/// fullscreen and minimise do. All set when the compositor never says (v < 5).
enum wm_capability_bit : std::uint32_t {
    wm_window_menu = 1u << 0,
    wm_maximize = 1u << 1,
    wm_fullscreen = 1u << 2,
    wm_minimize = 1u << 3,
};

enum class decoration_mode : std::uint8_t {
    /// No decoration object (no zxdg_decoration_manager_v1): the client decorates itself.
    none,
    client_side,
    server_side,
};

/// One configure sequence — every xdg_toplevel / zxdg_toplevel_decoration_v1 event up to
/// the xdg_surface.configure that closes it. The owner draws for it and calls
/// `toplevel::ack_configure(serial)` before the commit that shows the result.
struct toplevel_configure {
    std::uint32_t serial = 0;
    /// 0 on an axis: the client picks.
    size2i size{};
    std::uint32_t states = 0;
    /// configure_bounds (v4): the largest useful size; 0 = unknown.
    size2i bounds{};
    std::uint32_t wm_capabilities = wm_window_menu | wm_maximize | wm_fullscreen | wm_minimize;
    decoration_mode decoration = decoration_mode::none;
    /// The first configure since the role was created (the one the first buffer waits for).
    bool initial = false;
};

class toplevel_handler {
public:
    virtual ~toplevel_handler() = default;
    virtual void on_configure(toplevel& t, const toplevel_configure& c) = 0;
    virtual void on_close(toplevel& t) { (void)t; }
};

/// xdg_positioner, as data: what `popup::create` / `reposition` send.
struct positioner_desc {
    size2i size{};
    rect2i anchor_rect{};
    /// xdg_positioner.anchor / .gravity enum values (0 = none, 1 top, 2 bottom, 3 left, 4 right,
    /// 5 top_left, 6 bottom_left, 7 top_right, 8 bottom_right).
    std::uint32_t anchor = 0;
    std::uint32_t gravity = 0;
    /// xdg_positioner.constraint_adjustment bits (slide_x 1, slide_y 2, flip_x 4, flip_y 8,
    /// resize_x 16, resize_y 32).
    std::uint32_t constraint_adjustment = 0;
    point2i offset{};
    /// v3: re-evaluated when the parent moves or resizes.
    bool reactive = false;
    size2i parent_size{};
    std::uint32_t parent_configure = 0;
};

struct popup_configure {
    std::uint32_t serial = 0;
    /// Relative to the parent's window geometry.
    rect2i geometry{};
    /// A reposition's token when this configure answers one (`repositioned`), else 0.
    std::uint32_t reposition_token = 0;
    bool initial = false;
};

class popup_handler {
public:
    virtual ~popup_handler() = default;
    virtual void on_configure(popup& p, const popup_configure& c) = 0;
    /// xdg_popup.popup_done: dismissed (a click outside the grab, a parent unmapped).
    virtual void on_done(popup& p) { (void)p; }
};

// ── seat ──

enum seat_capability_bit : std::uint32_t {
    seat_pointer = 1u << 0,
    seat_keyboard = 1u << 1,
    seat_touch = 1u << 2,
};

/// One wl_pointer.frame: every event of the frame folded into one value (what GTK, winit
/// and Chromium deliver), so a motion and a scroll from one hardware event are one input.
/// Without frames (wl_pointer < 5) each event is its own frame.
struct pointer_frame {
    enum bits : std::uint32_t {
        has_enter = 1u << 0,
        has_leave = 1u << 1,
        has_motion = 1u << 2,
        has_button = 1u << 3,
        has_axis = 1u << 4,
        has_axis_source = 1u << 5,
        has_axis_stop = 1u << 6,
        has_axis_value120 = 1u << 7,
        has_axis_relative_direction = 1u << 8,
    };
    std::uint32_t mask = 0;
    /// The surface the pointer is over (after an enter in this frame: the entered one;
    /// after a leave: the one it left; a leave of one surface and an enter of another are
    /// two frames). Null over a surface this wrapper did not create or has destroyed since:
    /// route by target, never treat null as one's own.
    surface* target = nullptr;
    /// enter or button serial.
    std::uint32_t serial = 0;
    std::uint32_t time_ms = 0;
    /// Surface-local, logical pixels (enter / motion).
    double x = 0.0;
    double y = 0.0;
    /// evdev code (BTN_LEFT 0x110) and pressed.
    std::uint32_t button = 0;
    bool pressed = false;
    /// [0] vertical, [1] horizontal: continuous distance, value120 (one detent = 120),
    /// stop flags, relative direction (0 identical, 1 inverted).
    double axis_value[2]{};
    std::int32_t value120[2]{};
    bool stop[2]{};
    std::uint32_t direction[2]{};
    /// wl_pointer.axis_source (wheel 0, finger 1, continuous 2, wheel_tilt 3).
    std::uint32_t source = 0;
};

enum modifier_bit : std::uint32_t {
    mod_shift = 1u << 0,
    mod_caps = 1u << 1,
    mod_ctrl = 1u << 2,
    mod_alt = 1u << 3,
    mod_num = 1u << 4,
    mod_super = 1u << 5,
};

enum class key_state : std::uint8_t { released, pressed, repeated };

struct key_event {
    std::uint32_t serial = 0;
    std::uint32_t time_ms = 0;
    /// evdev code as sent (xkb keycode − 8).
    std::uint32_t key = 0;
    key_state state = key_state::released;
    /// xkb keysym of the key under the current state (0 without a keymap).
    std::uint32_t keysym = 0;
    /// Effective modifiers after the key (`modifier_bit`).
    std::uint32_t modifiers = 0;
    /// The text the key types (empty for releases and keys that type nothing), NUL-ended.
    char utf8[16]{};
    std::uint8_t utf8_len = 0;
    /// Whether xkb says this key repeats (the repeat itself is the app's: `repeat_rate`).
    bool repeats = false;
};

struct touch_event {
    enum class kind : std::uint8_t { down, up, motion, frame, cancel, shape, orientation };
    kind type = kind::frame;
    std::int32_t id = 0;
    std::uint32_t serial = 0;
    std::uint32_t time_ms = 0;
    surface* target = nullptr;
    double x = 0.0;
    double y = 0.0;
    /// shape: major / minor axis; orientation: degrees.
    double major = 0.0;
    double minor = 0.0;
    double orientation = 0.0;
};

class seat_handler {
public:
    virtual ~seat_handler() = default;
    virtual void on_capabilities(seat& s, std::uint32_t caps) {
        (void)s;
        (void)caps;
    }
    virtual void on_pointer(seat& s, const pointer_frame& f) {
        (void)s;
        (void)f;
    }
    /// A keymap was compiled (xkb state rebuilt).
    virtual void on_keymap(seat& s) { (void)s; }
    virtual void on_keyboard_focus(seat& s, surface* target, std::uint32_t serial, bool entered) {
        (void)s;
        (void)target;
        (void)serial;
        (void)entered;
    }
    virtual void on_key(seat& s, const key_event& e) {
        (void)s;
        (void)e;
    }
    virtual void on_modifiers(seat& s, std::uint32_t modifiers) {
        (void)s;
        (void)modifiers;
    }
    /// wl_keyboard.repeat_info: keys per second (0 = no repeat) and the delay in ms.
    virtual void on_repeat_info(seat& s, std::int32_t rate, std::int32_t delay_ms) {
        (void)s;
        (void)rate;
        (void)delay_ms;
    }
    virtual void on_touch(seat& s, const touch_event& e) {
        (void)s;
        (void)e;
    }
};

/// wp_cursor_shape_device_v1.shape values (v1 1–34, v2 adds 35–36).
enum class cursor_shape : std::uint32_t {
    default_ = 1, context_menu, help, pointer, progress, wait, cell, crosshair, text,
    vertical_text, alias, copy, move, no_drop, not_allowed, grab, grabbing, e_resize,
    n_resize, ne_resize, nw_resize, s_resize, se_resize, sw_resize, w_resize, ew_resize,
    ns_resize, nesw_resize, nwse_resize, col_resize, row_resize, all_scroll, zoom_in,
    zoom_out, dnd_ask, all_resize,
};

// ── text input (zwp_text_input_v3) ──

/// Bytes kept per preedit / commit string (the protocol caps a message at 4 KiB). A longer
/// string is cut at a UTF-8 boundary and counted (`text_input::truncated()`).
inline constexpr unsigned k_text_input_bytes = 4096;

/// The state one `done` applies, in the protocol's order: delete the surrounding text,
/// insert the commit string, then show the preedit.
struct text_input_done {
    std::uint32_t serial = 0;
    /// serial == the number of commits sent: the compositor saw the latest state. When
    /// false the client applies the text but keeps its own state (the protocol's rule).
    bool current = false;
    const char* preedit = "";
    std::int32_t preedit_cursor_begin = -1;
    std::int32_t preedit_cursor_end = -1;
    const char* commit = "";
    std::uint32_t delete_before = 0;
    std::uint32_t delete_after = 0;
    bool has_preedit = false;
    bool has_commit = false;
    bool has_delete = false;
};

class text_input_handler {
public:
    virtual ~text_input_handler() = default;
    virtual void on_focus(text_input& t, surface* target, bool entered) {
        (void)t;
        (void)target;
        (void)entered;
    }
    virtual void on_done(text_input& t, const text_input_done& d) = 0;
    /// v2: the input method asked for an action (finish 1, submit 2, …).
    virtual void on_action(text_input& t, std::uint32_t action, std::uint32_t serial) {
        (void)t;
        (void)action;
        (void)serial;
    }
};

// ── data transfer (wl_data_device: the clipboard and drag and drop; the primary selection) ──

/// Which selection a data device, offer or source serves: the clipboard (wl_data_device,
/// which also carries drag and drop) or the primary selection (zwp_primary_selection_v1:
/// what is selected, pasted with the middle button).
enum class selection_kind : std::uint8_t { clipboard, primary };

/// wl_data_device_manager.dnd_action bits.
inline constexpr std::uint32_t k_dnd_none = 0;
inline constexpr std::uint32_t k_dnd_copy = 1;
inline constexpr std::uint32_t k_dnd_move = 2;
inline constexpr std::uint32_t k_dnd_ask = 4;

class data_device_handler {
public:
    virtual ~data_device_handler() = default;
    /// The selection changed: `offer` is what another client (or this one) offers, valid
    /// until the next selection or the device's destruction; null when there is none. Sent
    /// to the client with keyboard focus, on entering it and on every change while it has it.
    virtual void on_selection(data_device& d, data_offer* offer) = 0;
    /// A drag entered `target` (one of this client's surfaces; null if it is not one) at
    /// surface-local (x, y). `offer` is the drag's (null for a drag within another client,
    /// which has no data to offer); `serial` is what `data_offer::accept` answers with.
    virtual void on_drag_enter(data_device& d, data_offer* offer, surface* target, double x,
                               double y, std::uint32_t serial) {
        (void)d, (void)offer, (void)target, (void)x, (void)y, (void)serial;
    }
    virtual void on_drag_motion(data_device& d, double x, double y) { (void)d, (void)x, (void)y; }
    /// The drag left (or was cancelled over us): its offer is gone.
    virtual void on_drag_leave(data_device& d) { (void)d; }
    /// Dropped here: `offer` stays until `data_device::finish_drop`.
    virtual void on_drop(data_device& d, data_offer* offer) { (void)d, (void)offer; }
};

class data_source_handler {
public:
    virtual ~data_source_handler() = default;
    /// A client asked for the data as `mime`: write it to `fd` and close it. The handler owns
    /// `fd` (it is closed for it only if there is no handler).
    virtual void on_send(data_source& s, const char* mime, int fd) = 0;
    /// The source was replaced, refused, its selection cleared, or its drag not dropped
    /// anywhere that took it: it will send no more.
    virtual void on_cancelled(data_source& s) = 0;
    /// Drag and drop: the target accepts `mime` (null: nothing), the action chosen so far,
    /// the drop done (data may still be read), the target done with it.
    virtual void on_target(data_source& s, const char* mime) { (void)s, (void)mime; }
    virtual void on_action(data_source& s, std::uint32_t action) { (void)s, (void)action; }
    virtual void on_drop_performed(data_source& s) { (void)s; }
    virtual void on_dnd_finished(data_source& s) { (void)s; }
};

// ── dmabuf feedback (zwp_linux_dmabuf_feedback_v1) ──

inline constexpr unsigned k_max_dmabuf_tranches = 8;
inline constexpr unsigned k_max_tranche_formats = 512;

struct format_modifier {
    std::uint32_t format = 0;
    std::uint64_t modifier = 0;
};

struct dmabuf_tranche {
    /// dev_t of the device buffers for this tranche are allocated on.
    std::uint64_t target_device = 0;
    /// zwp_linux_dmabuf_feedback_v1.tranche_flags (scanout = 1).
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    /// Entries past the cap (dropped, `overflow_action::warn_and_drop`).
    std::uint32_t dropped = 0;
    format_modifier formats[k_max_tranche_formats]{};

    [[nodiscard]] bool scanout() const { return (flags & 1u) != 0; }
    [[nodiscard]] std::span<const format_modifier> entries() const {
        return {formats, count};
    }
};

/// One `done`'s feedback, immutable until the next `done`: the main device, the format
/// table's size and the tranches in the compositor's preference order.
struct dmabuf_feedback_snapshot {
    /// Incremented per `done` (0: nothing received yet).
    std::uint32_t generation = 0;
    std::uint64_t main_device = 0;
    /// Entries in the format table (16 B each).
    std::uint32_t table_entries = 0;
    std::uint32_t tranche_count = 0;
    /// Tranches past the cap.
    std::uint32_t tranches_dropped = 0;
    /// Indices that pointed past the table (ignored).
    std::uint32_t bad_indices = 0;
    dmabuf_tranche tranches[k_max_dmabuf_tranches]{};

    [[nodiscard]] std::span<const dmabuf_tranche> list() const {
        return {tranches, tranche_count};
    }
    /// Whether any tranche offers `format` with `modifier`.
    [[nodiscard]] bool supports(std::uint32_t format, std::uint64_t modifier) const;
};

class dmabuf_feedback_handler {
public:
    virtual ~dmabuf_feedback_handler() = default;
    virtual void on_feedback(dmabuf_feedback& f, const dmabuf_feedback_snapshot& s) = 0;
};

/// One dma-buf plane for `buffer::create_dmabuf`.
struct dmabuf_plane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

/// Unknown globals (an extension the caller binds itself: the shell's zlm_shell_v1,
/// layer-shell) and removals, with the registry as `void*` (wl_registry*); and the outputs
/// and seats the display tracks coming and going (hotplug: one panel per output). Set it
/// before `connect` to hear the ones bound there.
class global_handler {
public:
    virtual ~global_handler() = default;
    virtual void on_global(display& d, std::uint32_t name, const char* interface,
                           std::uint32_t version) {
        (void)d;
        (void)name;
        (void)interface;
        (void)version;
    }
    virtual void on_global_remove(display& d, std::uint32_t name) {
        (void)d;
        (void)name;
    }
    /// Output slot `slot` (`display::output_at`) is complete: its first wl_output.done.
    virtual void on_output_added(display& d, unsigned slot) {
        (void)d;
        (void)slot;
    }
    /// A later wl_output.done: its mode, scale, position or name changed.
    virtual void on_output_changed(display& d, unsigned slot) {
        (void)d;
        (void)slot;
    }
    /// The output's global is going: `output_at(slot)` is still valid during the call, every
    /// surface has left it (`surface_handler::on_output`), and the slot is free after.
    virtual void on_output_removed(display& d, unsigned slot) {
        (void)d;
        (void)slot;
    }
    /// A wl_seat was bound (its capabilities follow) / its global is going (its devices are
    /// already released, their focus lost).
    virtual void on_seat_added(display& d, seat& s) {
        (void)d;
        (void)s;
    }
    virtual void on_seat_removed(display& d, seat& s) {
        (void)d;
        (void)s;
    }
};

} // namespace lx::wayland
