module;

#include <cstring>
#include <memory>

#if defined(LUMEN_HAS_INPUT)
#include <cerrno>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#endif

import lx.foundation;
import lx.runtime;

export module lx.input;

export import :keymap;

export namespace lx::input {

enum class button { left, middle, right, side, extra, unknown };
enum class axis { horizontal_scroll, vertical_scroll };

/// Where a scroll came from. Clients treat these differently — a wheel click is a discrete
/// notch, a touchpad is continuous and kinetic — and `wl_pointer.axis_source` exists to
/// tell them apart.
enum class axis_source { wheel, finger, continuous, wheel_tilt, unknown };

struct pointer_motion_event {
    /// Relative movement, already accelerated by libinput.
    double dx = 0.0;
    double dy = 0.0;
    /// Absolute position in output coordinates; only meaningful when `absolute`.
    double x = 0.0;
    double y = 0.0;
    bool absolute = false;
    unsigned time_ms = 0;
};

struct pointer_button_event {
    button which = button::unknown;
    unsigned evdev_code = 0;
    bool pressed = false;
    unsigned time_ms = 0;
};

struct pointer_axis_event {
    double horizontal = 0.0;
    double vertical = 0.0;
    /// Wheel clicks, in units of one notch. Zero for continuous sources.
    double horizontal_discrete = 0.0;
    double vertical_discrete = 0.0;
    axis_source source = axis_source::unknown;
    unsigned time_ms = 0;
};

struct key_event {
    unsigned evdev_keycode = 0;
    bool pressed = false;
    unsigned time_ms = 0;
};

struct touch_event {
    int slot = 0;
    double x = 0.0;
    double y = 0.0;
    enum class phase { down, motion, up, cancel } kind = phase::down;
    unsigned time_ms = 0;
};

/// Where translated events go. Plain function pointers with a context, not `runtime::task`:
/// these fire synchronously inside `dispatch()` rather than being queued, and the seat must
/// not allocate on the input path.
struct event_sink {
    void (*pointer_motion)(const pointer_motion_event&, void*) = nullptr;
    void (*pointer_button)(const pointer_button_event&, void*) = nullptr;
    void (*pointer_axis)(const pointer_axis_event&, void*) = nullptr;
    void (*key)(const key_event&, void*) = nullptr;
    /// Fired only when the state actually changed — clients redraw on modifier events, so
    /// sending one per keystroke is wasteful and visible.
    void (*modifiers)(const modifier_state&, void*) = nullptr;
    void (*touch)(const touch_event&, void*) = nullptr;
    /// A device appeared, with its libinput name and what it can do. Reported because
    /// "nothing happens when I type" is usually a keyboard that was never opened, and a
    /// device count alone cannot tell you that.
    void (*device_added)(const char* name, bool keyboard, bool pointer, bool touch,
                         void*) = nullptr;
    void* user = nullptr;
};

/// Opens and closes device nodes on libinput's behalf.
///
/// A compositor does not have permission to open `/dev/input/*` directly; logind does it
/// and passes back a file descriptor. This indirection is how that gets plugged in without
/// `lx.input` depending on `lx.session` — and it is also what makes the seat testable, by
/// substituting a provider that opens fixtures.
struct device_provider {
    int (*open_device)(const char* path, int flags, void* user) = nullptr;
    void (*close_device)(int fd, void* user) = nullptr;
    void* user = nullptr;
};

/// Keyboard focus and the seat's serial counter.
///
/// Serials are not decoration. A client validates the serial on a popup grab, a drag start
/// and an activation request, and reusing or inventing one is how grabs get rejected or,
/// worse, accepted when they should not be.
class seat {
public:
    [[nodiscard]] lx::seat_id id() const { return id_; }

    [[nodiscard]] unsigned next_serial() { return ++serial_; }
    [[nodiscard]] unsigned last_serial() const { return serial_; }

    void set_keyboard_focus(lx::surface_id surface) { keyboard_focus_ = surface; }
    [[nodiscard]] lx::surface_id keyboard_focus() const { return keyboard_focus_; }

    void set_pointer_focus(lx::surface_id surface) { pointer_focus_ = surface; }
    [[nodiscard]] lx::surface_id pointer_focus() const { return pointer_focus_; }

    void set_pointer_position(double x, double y) {
        pointer_x_ = x;
        pointer_y_ = y;
    }
    [[nodiscard]] double pointer_x() const { return pointer_x_; }
    [[nodiscard]] double pointer_y() const { return pointer_y_; }

    /// Clamps the pointer into a region, which is what keeps it on screen.
    void constrain_pointer(lx::rect2i region) {
        if (region.width <= 0 || region.height <= 0)
            return;
        const double max_x = region.x + region.width - 1;
        const double max_y = region.y + region.height - 1;
        if (pointer_x_ < region.x) pointer_x_ = region.x;
        if (pointer_y_ < region.y) pointer_y_ = region.y;
        if (pointer_x_ > max_x) pointer_x_ = max_x;
        if (pointer_y_ > max_y) pointer_y_ = max_y;
    }

    [[nodiscard]] keyboard_keymap& keymap() { return keymap_; }
    [[nodiscard]] const keyboard_keymap& keymap() const { return keymap_; }

    /// Keys currently held, so a focus change can tell the newly focused surface what is
    /// already down — `wl_keyboard.enter` carries that array, and getting it wrong leaves
    /// clients with stuck modifiers.
    [[nodiscard]] const unsigned* pressed_keys() const { return pressed_; }
    [[nodiscard]] unsigned pressed_count() const { return pressed_count_; }
    void note_key(unsigned evdev_keycode, bool pressed);

private:
    static constexpr unsigned k_max_pressed = 32;

    lx::seat_id id_{1};
    unsigned serial_ = 0;
    lx::surface_id keyboard_focus_{};
    lx::surface_id pointer_focus_{};
    double pointer_x_ = 0.0;
    double pointer_y_ = 0.0;
    keyboard_keymap keymap_{};
    unsigned pressed_[k_max_pressed]{};
    unsigned pressed_count_ = 0;
};

/// libinput context bound to a udev seat.
class input_manager {
public:
    input_manager() = default;
    ~input_manager();

    input_manager(const input_manager&) = delete;
    input_manager& operator=(const input_manager&) = delete;
    input_manager(input_manager&& other) noexcept;
    input_manager& operator=(input_manager&& other) noexcept;

    /// `seat_name` is a udev seat, almost always "seat0". The provider supplies device fds.
    [[nodiscard]] static lx::result<input_manager> open(device_provider provider,
                                                        const char* seat_name = "seat0");

    /// Poll this in the event loop; readable means events are waiting.
    [[nodiscard]] int fd() const;

    void set_sink(event_sink sink) { sink_ = sink; }
    [[nodiscard]] seat& default_seat() { return seat_; }

    /// Drains libinput, updates keyboard state, and calls the sink. Returns how many events
    /// were translated.
    unsigned dispatch();

    /// Devices currently attached. Zero is legitimate — a machine can boot with none.
    [[nodiscard]] unsigned device_count() const { return device_count_; }

    /// Suspend and resume for VT switching: on switch away the fds must be released or the
    /// next session cannot claim them.
    void suspend();
    [[nodiscard]] lx::result<void> resume();

private:
    void destroy();
    void handle_event(void* libinput_event);

    void* udev_ = nullptr;
    void* libinput_ = nullptr;
    event_sink sink_{};
    seat seat_{};
    /// Heap-allocated because libinput stores this pointer for the life of the context and
    /// calls back through it — including from `libinput_unref`, to close the devices it
    /// still holds. A by-value member would move with the manager and leave libinput
    /// pointing at a destroyed object; the crash then lands at teardown, far from the move
    /// that caused it, and only when devices actually opened.
    std::unique_ptr<device_provider> provider_{};
    unsigned device_count_ = 0;
    bool suspended_ = false;
};

} // namespace lx::input

module :private;

void lx::input::seat::note_key(unsigned evdev_keycode, bool pressed) {
    for (unsigned i = 0; i < pressed_count_; ++i) {
        if (pressed_[i] != evdev_keycode)
            continue;
        if (!pressed) {
            pressed_[i] = pressed_[--pressed_count_];
        }
        return; // already recorded, or just removed
    }
    if (pressed && pressed_count_ < k_max_pressed)
        pressed_[pressed_count_++] = evdev_keycode;
}

#if defined(LUMEN_HAS_INPUT)

namespace {

int lx_open_restricted(const char* path, int flags, void* user) {
    auto* provider = static_cast<lx::input::device_provider*>(user);
    if (provider && provider->open_device)
        return provider->open_device(path, flags, provider->user);
    // No provider: direct open. Works when the process already has the privilege — running
    // as root, or a member of the input group — and fails cleanly when it does not.
    const int fd = ::open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void lx_close_restricted(int fd, void* user) {
    auto* provider = static_cast<lx::input::device_provider*>(user);
    if (provider && provider->close_device) {
        provider->close_device(fd, provider->user);
        return;
    }
    ::close(fd);
}

const libinput_interface k_interface = {
    .open_restricted = lx_open_restricted,
    .close_restricted = lx_close_restricted,
};

[[nodiscard]] lx::input::button to_button(unsigned code) {
    switch (code) {
    case BTN_LEFT: return lx::input::button::left;
    case BTN_MIDDLE: return lx::input::button::middle;
    case BTN_RIGHT: return lx::input::button::right;
    case BTN_SIDE: return lx::input::button::side;
    case BTN_EXTRA: return lx::input::button::extra;
    default: return lx::input::button::unknown;
    }
}

[[nodiscard]] lx::input::axis_source to_axis_source(libinput_pointer_axis_source src) {
    switch (src) {
    case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL: return lx::input::axis_source::wheel;
    case LIBINPUT_POINTER_AXIS_SOURCE_FINGER: return lx::input::axis_source::finger;
    case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS: return lx::input::axis_source::continuous;
    case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT: return lx::input::axis_source::wheel_tilt;
    default: return lx::input::axis_source::unknown;
    }
}

} // namespace

#endif // LUMEN_HAS_INPUT

lx::input::input_manager::~input_manager() { destroy(); }

void lx::input::input_manager::destroy() {
#if defined(LUMEN_HAS_INPUT)
    // Order matters: unref closes every device libinput still holds, and each close goes
    // back through the provider. Releasing the provider first would free it out from under
    // those callbacks.
    if (libinput_)
        libinput_unref(static_cast<struct libinput*>(libinput_));
    if (udev_)
        udev_unref(static_cast<struct udev*>(udev_));
#endif
    libinput_ = nullptr;
    udev_ = nullptr;
    provider_.reset();
    device_count_ = 0;
}

lx::input::input_manager::input_manager(input_manager&& other) noexcept
    : udev_{other.udev_}, libinput_{other.libinput_}, sink_{other.sink_},
      provider_{std::move(other.provider_)}, device_count_{other.device_count_},
      suspended_{other.suspended_} {
    other.udev_ = nullptr;
    other.libinput_ = nullptr;
    other.device_count_ = 0;
}

lx::input::input_manager& lx::input::input_manager::operator=(input_manager&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    udev_ = other.udev_;
    libinput_ = other.libinput_;
    sink_ = other.sink_;
    provider_ = std::move(other.provider_);
    device_count_ = other.device_count_;
    suspended_ = other.suspended_;
    other.udev_ = nullptr;
    other.libinput_ = nullptr;
    other.device_count_ = 0;
    return *this;
}

lx::result<lx::input::input_manager> lx::input::input_manager::open(device_provider provider,
                                                                    const char* seat_name) {
#if defined(LUMEN_HAS_INPUT)
    input_manager mgr;
    mgr.provider_ = std::make_unique<device_provider>(provider);

    auto* udev = udev_new();
    if (!udev)
        return lx::make_error(lx::error_domain::io, 0, "udev_new failed");
    mgr.udev_ = udev;

    // The provider is handed to libinput as user data and must outlive the context, at a
    // fixed address: libinput calls back through it from `libinput_unref` as well as at
    // open time. The manager owns the allocation, so moves carry the pointer rather than
    // relocating the object.
    auto* li = libinput_udev_create_context(&k_interface, mgr.provider_.get(), udev);
    if (!li) {
        return lx::make_error(lx::error_domain::io, 0, "libinput_udev_create_context failed");
    }
    mgr.libinput_ = li;

    if (libinput_udev_assign_seat(li, seat_name ? seat_name : "seat0") != 0) {
        return lx::make_error(lx::error_domain::io, 0,
                              "libinput_udev_assign_seat failed — no permission for the seat");
    }

    // Compile a keymap up front. Without one, keys arrive as codes with no meaning and
    // clients get no keymap at all.
    if (auto compiled = mgr.seat_.keymap().compile(); !compiled)
        return compiled.get_error();

    return mgr;
#else
    (void)provider;
    (void)seat_name;
    return lx::not_implemented("lx::input::input_manager::open");
#endif
}

int lx::input::input_manager::fd() const {
#if defined(LUMEN_HAS_INPUT)
    if (!libinput_)
        return -1;
    return libinput_get_fd(static_cast<struct libinput*>(libinput_));
#else
    return -1;
#endif
}

void lx::input::input_manager::suspend() {
#if defined(LUMEN_HAS_INPUT)
    if (libinput_ && !suspended_) {
        libinput_suspend(static_cast<struct libinput*>(libinput_));
        suspended_ = true;
    }
#endif
}

lx::result<void> lx::input::input_manager::resume() {
#if defined(LUMEN_HAS_INPUT)
    if (!libinput_ || !suspended_)
        return {};
    if (libinput_resume(static_cast<struct libinput*>(libinput_)) != 0)
        return lx::make_error(lx::error_domain::io, 0, "libinput_resume failed");
    suspended_ = false;
    return {};
#else
    return lx::not_implemented("lx::input::input_manager::resume");
#endif
}

#if defined(LUMEN_HAS_INPUT)

void lx::input::input_manager::handle_event(void* raw) {
    auto* ev = static_cast<libinput_event*>(raw);
    const auto type = libinput_event_get_type(ev);

    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED: {
        ++device_count_;
        if (sink_.device_added) {
            auto* dev = libinput_event_get_device(ev);
            sink_.device_added(
                libinput_device_get_name(dev),
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0,
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER) != 0,
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH) != 0,
                sink_.user);
        }
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED:
        if (device_count_ > 0)
            --device_count_;
        break;

    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* p = libinput_event_get_pointer_event(ev);
        pointer_motion_event out{};
        out.dx = libinput_event_pointer_get_dx(p);
        out.dy = libinput_event_pointer_get_dy(p);
        out.time_ms = libinput_event_pointer_get_time(p);
        seat_.set_pointer_position(seat_.pointer_x() + out.dx, seat_.pointer_y() + out.dy);
        out.x = seat_.pointer_x();
        out.y = seat_.pointer_y();
        if (sink_.pointer_motion)
            sink_.pointer_motion(out, sink_.user);
        break;
    }

    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
        auto* p = libinput_event_get_pointer_event(ev);
        pointer_motion_event out{};
        out.absolute = true;
        // Transformed against a unit box; the compositor scales into output space, since
        // only it knows the layout.
        out.x = libinput_event_pointer_get_absolute_x_transformed(p, 1);
        out.y = libinput_event_pointer_get_absolute_y_transformed(p, 1);
        out.time_ms = libinput_event_pointer_get_time(p);
        if (sink_.pointer_motion)
            sink_.pointer_motion(out, sink_.user);
        break;
    }

    case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto* p = libinput_event_get_pointer_event(ev);
        pointer_button_event out{};
        out.evdev_code = libinput_event_pointer_get_button(p);
        out.which = to_button(out.evdev_code);
        out.pressed = libinput_event_pointer_get_button_state(p) ==
                      LIBINPUT_BUTTON_STATE_PRESSED;
        out.time_ms = libinput_event_pointer_get_time(p);
        if (sink_.pointer_button)
            sink_.pointer_button(out, sink_.user);
        break;
    }

    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
        auto* p = libinput_event_get_pointer_event(ev);
        pointer_axis_event out{};
        out.time_ms = libinput_event_pointer_get_time(p);
        out.source = to_axis_source(libinput_event_pointer_get_axis_source(p));
        if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
            out.horizontal = libinput_event_pointer_get_scroll_value(
                p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
            if (type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL) {
                out.horizontal_discrete = libinput_event_pointer_get_scroll_value_v120(
                                              p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) /
                                          120.0;
            }
        }
        if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
            out.vertical = libinput_event_pointer_get_scroll_value(
                p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            if (type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL) {
                out.vertical_discrete = libinput_event_pointer_get_scroll_value_v120(
                                            p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) /
                                        120.0;
            }
        }
        if (sink_.pointer_axis)
            sink_.pointer_axis(out, sink_.user);
        break;
    }

    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto* k = libinput_event_get_keyboard_event(ev);
        key_event out{};
        out.evdev_keycode = libinput_event_keyboard_get_key(k);
        out.pressed = libinput_event_keyboard_get_key_state(k) ==
                      LIBINPUT_KEY_STATE_PRESSED;
        out.time_ms = libinput_event_keyboard_get_time(k);

        // xkb state first: a client that reads modifiers after the key event must see the
        // state that key produced, and the compositor's own bindings need it too.
        const bool mods_changed = seat_.keymap().update_key(out.evdev_keycode, out.pressed);
        seat_.note_key(out.evdev_keycode, out.pressed);

        if (sink_.key)
            sink_.key(out, sink_.user);
        // After the key, and only on a real change — clients redraw on modifier events.
        if (mods_changed && sink_.modifiers)
            sink_.modifiers(seat_.keymap().modifiers(), sink_.user);
        break;
    }

    case LIBINPUT_EVENT_TOUCH_DOWN:
    case LIBINPUT_EVENT_TOUCH_MOTION:
    case LIBINPUT_EVENT_TOUCH_UP:
    case LIBINPUT_EVENT_TOUCH_CANCEL: {
        auto* t = libinput_event_get_touch_event(ev);
        touch_event out{};
        out.time_ms = libinput_event_touch_get_time(t);
        if (type == LIBINPUT_EVENT_TOUCH_DOWN || type == LIBINPUT_EVENT_TOUCH_MOTION) {
            out.slot = libinput_event_touch_get_slot(t);
            out.x = libinput_event_touch_get_x_transformed(t, 1);
            out.y = libinput_event_touch_get_y_transformed(t, 1);
        } else if (type == LIBINPUT_EVENT_TOUCH_UP) {
            out.slot = libinput_event_touch_get_slot(t);
        }
        out.kind = type == LIBINPUT_EVENT_TOUCH_DOWN     ? touch_event::phase::down
                   : type == LIBINPUT_EVENT_TOUCH_MOTION ? touch_event::phase::motion
                   : type == LIBINPUT_EVENT_TOUCH_UP     ? touch_event::phase::up
                                                         : touch_event::phase::cancel;
        if (sink_.touch)
            sink_.touch(out, sink_.user);
        break;
    }

    default:
        break;
    }
}

#endif // LUMEN_HAS_INPUT

unsigned lx::input::input_manager::dispatch() {
#if defined(LUMEN_HAS_INPUT)
    if (!libinput_)
        return 0;
    auto* li = static_cast<struct libinput*>(libinput_);
    if (libinput_dispatch(li) != 0)
        return 0;

    unsigned handled = 0;
    while (auto* ev = libinput_get_event(li)) {
        handle_event(ev);
        libinput_event_destroy(ev);
        ++handled;
    }
    return handled;
#else
    return 0;
#endif
}
