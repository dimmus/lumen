module;

#if defined(LUMEN_HAS_INPUT)
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#endif

import lx.foundation;
import lx.runtime;

export module lx.input;

import std;

export import :keymap;
export import :device_config;

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
    /// The same movement without pointer acceleration, in the device's units normalised to
    /// 1000 dpi (libinput's `get_dx_unaccelerated`): what relative-pointer-unstable-v1 hands
    /// games and VM viewers alongside the accelerated delta.
    double dx_unaccel = 0.0;
    double dy_unaccel = 0.0;
    /// Pointer position. After relative motion it is the seat position in output
    /// coordinates; for an `absolute` device it is normalised to [0, 1] of the device's
    /// box, and the compositor — which alone knows the output layout — scales it.
    double x = 0.0;
    double y = 0.0;
    bool absolute = false;
    /// An `absolute` device's previous position (normalised like `x`, `y`) when
    /// `has_prev`: the compositor scales the difference into a relative delta (sway does the
    /// same), because a tablet or a VM console mouse reports no motion of its own — and a
    /// locked pointer must still yield one.
    double prev_x = 0.0;
    double prev_y = 0.0;
    bool has_prev = false;
    unsigned time_ms = 0;
    /// libinput's microsecond timestamp (CLOCK_MONOTONIC). `time_ms` is what the wire
    /// carries; this is what latency is measured against.
    unsigned long long time_us = 0;
};

/// A touchpad gesture (libinput's swipe, pinch and hold), for pointer-gestures-unstable-v1.
/// One struct for the three kinds and their phases: begin carries the finger count, update
/// the deltas (and for a pinch the scale since begin and the rotation since the last
/// update, in degrees clockwise), end whether it was cancelled.
struct gesture_event {
    enum class kind : unsigned char { swipe, pinch, hold } type = kind::swipe;
    enum class phase : unsigned char { begin, update, end } stage = phase::begin;
    unsigned fingers = 0;
    double dx = 0.0;
    double dy = 0.0;
    double dx_unaccel = 0.0;
    double dy_unaccel = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
    bool cancelled = false;
    unsigned time_ms = 0;
    unsigned long long time_us = 0;
};

struct pointer_button_event {
    button which = button::unknown;
    unsigned evdev_code = 0;
    bool pressed = false;
    unsigned time_ms = 0;
    unsigned long long time_us = 0;
    /// A release libinput made because the device is going away (revoked at a VT switch,
    /// unplugged, the seat suspended) — not the user letting go.
    bool device_removed = false;
};

struct pointer_axis_event {
    double horizontal = 0.0;
    double vertical = 0.0;
    /// Wheel clicks, in units of one notch. Zero for continuous sources.
    double horizontal_discrete = 0.0;
    double vertical_discrete = 0.0;
    axis_source source = axis_source::unknown;
    unsigned time_ms = 0;
    unsigned long long time_us = 0;
    /// The device's configured scroll multiplier (`pointer_config::scroll_factor`), applied
    /// by the compositor to the values it sends.
    double scroll_factor = 1.0;
};

struct key_event {
    unsigned evdev_keycode = 0;
    bool pressed = false;
    unsigned time_ms = 0;
    unsigned long long time_us = 0;
    /// As `pointer_button_event::device_removed`.
    bool device_removed = false;
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
    /// Touchpad gestures (every phase of every kind).
    void (*gesture)(const gesture_event&, void*) = nullptr;
    /// A device appeared, with its libinput name and what it can do. Reported because
    /// "nothing happens when I type" is usually a keyboard that was never opened, and a
    /// device count alone cannot tell you that.
    /// `gesture`: a touchpad whose swipes, pinches and holds libinput reports.
    void (*device_added)(const char* name, bool keyboard, bool pointer, bool touch, bool gesture,
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
    [[nodiscard]] const unsigned* pressed_keys() const { return pressed_.data(); }
    [[nodiscard]] unsigned pressed_count() const { return static_cast<unsigned>(pressed_.size()); }
    void note_key(unsigned evdev_keycode, bool pressed);
    /// No key is held any more (a session pause released them all).
    void clear_pressed() { pressed_.clear(); }

private:
    static constexpr unsigned k_max_pressed = 32;

    lx::seat_id id_{1};
    unsigned serial_ = 0;
    lx::surface_id keyboard_focus_{};
    lx::surface_id pointer_focus_{};
    double pointer_x_ = 0.0;
    double pointer_y_ = 0.0;
    keyboard_keymap keymap_{};
    std::inplace_vector<unsigned, k_max_pressed> pressed_;
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
    /// `keymap` is the layout the seat's keymap is compiled with (empty: the system default);
    /// `num_lock` starts the session with Num Lock on. `compiled`, when valid, is that
    /// keymap compiled already (moved in; not compiled twice).
    [[nodiscard]] static lx::result<input_manager> open(device_provider provider,
                                                        const char* seat_name = "seat0",
                                                        keymap_config keymap = {},
                                                        bool num_lock = false,
                                                        keyboard_keymap* compiled = nullptr);

    /// The configuration a pointer device should have: fills `out` for the device named
    /// `name` of class `kind` and returns true; false leaves the device as libinput set it.
    /// Called for every pointer device as it appears, and for all of them by
    /// `reapply_device_config` (a configuration reload). UI affinity, like `dispatch`.
    using device_config_fn = bool (*)(void* user, const char* name, device_class kind,
                                      pointer_config& out);
    /// After applying (or finding a setting unsupported): one line's worth for the log.
    using device_report_fn = void (*)(void* user, const char* name, device_class kind,
                                      const apply_report& report);
    void set_device_config(device_config_fn fn, device_report_fn report, void* user) {
        device_config_ = fn;
        device_report_ = report;
        device_config_user_ = user;
    }
    /// Applies the configuration to every device present again.
    void reapply_device_config();
    /// The keyboards' LEDs follow the keymap's state (Caps Lock, Num Lock): after a
    /// modifier change or a new keymap. Only when the lit set changed.
    void update_leds();
    /// libinput's device configuration calls (what `apply_pointer_config` is given).
    [[nodiscard]] static const device_ops& libinput_ops();

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
    /// next session cannot claim them. `suspend` closes every device (each close goes back
    /// through the provider) and drains the queue at once: libinput emits a release for
    /// every key and button still down, and those must reach the sink now, through the
    /// normal path, so clients end with balanced state (Clutter drains the same way).
    /// `resume` re-enumerates the seat and reopens every device through the provider.
    void suspend();
    [[nodiscard]] lx::result<void> resume();
    [[nodiscard]] bool suspended() const { return suspended_; }

private:
    void destroy();
    void handle_event(void* libinput_event);
    /// Hands every queued event to `handle_event`, a fixed-size batch at a time. A device
    /// going away (a revoked fd, an unplug, `suspend`) has libinput release its held keys
    /// and buttons before DEVICE_REMOVED, in the same batch: those releases are marked
    /// `device_removed`, so a Super tap or a drop is not read into them.
    unsigned drain();

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
    /// The devices present (a reference held on each), their class and scroll multiplier.
    struct device_entry {
        void* device = nullptr; // libinput_device*
        device_class kind = device_class::other;
        bool keyboard = false;
        double scroll_factor = 1.0;
    };
    unsigned leds_ = ~0u;
    static constexpr unsigned k_max_devices = 32;
    std::inplace_vector<device_entry, k_max_devices> devices_{};
    device_config_fn device_config_ = nullptr;
    device_report_fn device_report_ = nullptr;
    void* device_config_user_ = nullptr;
    void configure_device(device_entry& e);
    void release_devices();
    bool suspended_ = false;
    /// The event being handled comes from a device that is going away (see `dispatch`).
    bool removing_ = false;
    /// The last position of each absolute pointer device (`pointer_motion_event::prev_x`),
    /// fixed storage: a seat has one or two such devices; beyond the table a device simply
    /// reports no previous position.
    struct absolute_track {
        void* device = nullptr;
        double x = 0.0;
        double y = 0.0;
    };
    static constexpr unsigned k_absolute_tracks = 8;
    absolute_track absolute_[k_absolute_tracks]{};
};

} // namespace lx::input

module :private;

void lx::input::seat::note_key(unsigned evdev_keycode, bool pressed) {
    for (unsigned& key : pressed_) {
        if (key != evdev_keycode)
            continue;
        if (!pressed) {
            key = pressed_.back();
            pressed_.pop_back();
        }
        return; // already recorded, or just removed
    }
    if (pressed)
        pressed_.try_push_back(evdev_keycode); // dropped when full
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

} // namespace

#endif // LUMEN_HAS_INPUT

lx::input::input_manager::~input_manager() { destroy(); }

void lx::input::input_manager::release_devices() {
#if defined(LUMEN_HAS_INPUT)
    for (const device_entry& e : devices_)
        libinput_device_unref(static_cast<libinput_device*>(e.device));
#endif
    devices_.clear();
}

void lx::input::input_manager::destroy() {
    release_devices();
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
      seat_{std::move(other.seat_)}, provider_{std::move(other.provider_)},
      device_count_{other.device_count_}, devices_{other.devices_},
      device_config_{other.device_config_}, device_report_{other.device_report_},
      device_config_user_{other.device_config_user_}, suspended_{other.suspended_} {
    other.udev_ = nullptr;
    other.libinput_ = nullptr;
    other.device_count_ = 0;
    other.devices_.clear();
}

lx::input::input_manager& lx::input::input_manager::operator=(input_manager&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    udev_ = other.udev_;
    libinput_ = other.libinput_;
    sink_ = other.sink_;
    // The seat carries the compiled keymap. Leaving it behind silently drops xkb
    // translation — keys keep arriving, with no keysym and no modifiers.
    seat_ = std::move(other.seat_);
    provider_ = std::move(other.provider_);
    device_count_ = other.device_count_;
    devices_ = other.devices_;
    device_config_ = other.device_config_;
    device_report_ = other.device_report_;
    device_config_user_ = other.device_config_user_;
    suspended_ = other.suspended_;
    other.udev_ = nullptr;
    other.libinput_ = nullptr;
    other.device_count_ = 0;
    other.devices_.clear();
    return *this;
}

lx::result<lx::input::input_manager> lx::input::input_manager::open(device_provider provider,
                                                                    const char* seat_name,
                                                                    keymap_config keymap,
                                                                    bool num_lock,
                                                                    keyboard_keymap* compiled) {
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
    if (compiled && compiled->valid()) {
        mgr.seat_.keymap() = std::move(*compiled);
    } else if (auto built = mgr.seat_.keymap().compile(keymap); !built) {
        // A configured layout that does not compile must not leave the seat without a
        // keymap: the system default instead (the caller reports the configuration error).
        if (!keymap.layout && !keymap.variant && !keymap.options && !keymap.model && !keymap.rules)
            return built.get_error();
        if (auto fallback = mgr.seat_.keymap().compile(); !fallback)
            return fallback.get_error();
    }
    if (num_lock)
        (void)mgr.seat_.keymap().set_num_lock(true);

    return mgr;
#else
    (void)provider;
    (void)seat_name;
    (void)keymap;
    (void)num_lock;
    (void)compiled;
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
        auto* li = static_cast<struct libinput*>(libinput_);
        libinput_suspend(li);
        suspended_ = true;
        // The neutral-state events libinput_suspend queued: key and button releases, touch
        // cancels, then DEVICE_REMOVED for each device.
        (void)drain();
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
        auto* added = libinput_event_get_device(ev);
        // The LEDs follow the xkb state, not whatever the text console (or another session)
        // left on the keyboard.
        if (libinput_device_has_capability(added, LIBINPUT_DEVICE_CAP_KEYBOARD))
            libinput_device_led_update(added,
                                       static_cast<libinput_led>(seat_.keymap().led_mask()));
        const bool pointer = libinput_device_has_capability(added, LIBINPUT_DEVICE_CAP_POINTER);
        const bool keys = libinput_device_has_capability(added, LIBINPUT_DEVICE_CAP_KEYBOARD);
        if ((pointer || keys) && devices_.size() < k_max_devices) {
            device_entry e{};
            e.device = libinput_device_ref(added);
            e.keyboard = keys;
            if (!pointer) {
                e.kind = device_class::other;
            } else if (libinput_device_config_tap_get_finger_count(added) > 0) {
                e.kind = device_class::touchpad;
            } else {
                e.kind = device_class::mouse;
                if (udev_device* ud = libinput_device_get_udev_device(added)) {
                    const char* stick = udev_device_get_property_value(ud, "ID_INPUT_POINTINGSTICK");
                    if (stick && std::strcmp(stick, "1") == 0)
                        e.kind = device_class::trackpoint;
                    udev_device_unref(ud);
                }
            }
            devices_.push_back(e);
            configure_device(devices_.back());
        }
        if (sink_.device_added) {
            auto* dev = added;
            sink_.device_added(
                libinput_device_get_name(dev),
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0,
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER) != 0,
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH) != 0,
                libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_GESTURE) != 0,
                sink_.user);
        }
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED:
        if (device_count_ > 0)
            --device_count_;
        for (unsigned i = 0; i < devices_.size(); ++i) {
            if (devices_[i].device != libinput_event_get_device(ev))
                continue;
            libinput_device_unref(static_cast<libinput_device*>(devices_[i].device));
            devices_[i] = devices_.back();
            devices_.pop_back();
            break;
        }
        // A device that comes back starts without a previous position.
        for (auto& t : absolute_)
            if (t.device == libinput_event_get_device(ev))
                t = {};
        break;

    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto* p = libinput_event_get_pointer_event(ev);
        pointer_motion_event out{};
        out.dx = libinput_event_pointer_get_dx(p);
        out.dy = libinput_event_pointer_get_dy(p);
        out.dx_unaccel = libinput_event_pointer_get_dx_unaccelerated(p);
        out.dy_unaccel = libinput_event_pointer_get_dy_unaccelerated(p);
        out.time_ms = libinput_event_pointer_get_time(p);
        out.time_us = libinput_event_pointer_get_time_usec(p);
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
        out.time_us = libinput_event_pointer_get_time_usec(p);
        {
            void* dev = libinput_event_get_device(ev);
            absolute_track* slot = nullptr;
            for (auto& t : absolute_) {
                if (t.device == dev) {
                    slot = &t;
                    break;
                }
                if (!slot && !t.device)
                    slot = &t;
            }
            if (slot) {
                out.has_prev = slot->device == dev;
                out.prev_x = slot->x;
                out.prev_y = slot->y;
                *slot = {dev, out.x, out.y};
            }
        }
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
        out.time_us = libinput_event_pointer_get_time_usec(p);
        out.device_removed = removing_;
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
        out.time_us = libinput_event_pointer_get_time_usec(p);
        // The source is the event type. libinput_event_pointer_get_axis_source() is only
        // valid for the deprecated LIBINPUT_EVENT_POINTER_AXIS, and calling it on these
        // makes libinput print "client bug: Invalid event type ... passed to
        // libinput_event_pointer_get_axis_source()" — which it did, 18 times in one run.
        for (const device_entry& d : devices_) {
            if (d.device == libinput_event_get_device(ev)) {
                out.scroll_factor = d.scroll_factor;
                break;
            }
        }
        out.source = type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL    ? axis_source::wheel
                     : type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ? axis_source::finger
                                                                    : axis_source::continuous;
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
        out.time_us = libinput_event_keyboard_get_time_usec(k);
        out.device_removed = removing_;

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

    case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
    case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
    case LIBINPUT_EVENT_GESTURE_SWIPE_END:
    case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
    case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
    case LIBINPUT_EVENT_GESTURE_PINCH_END:
    case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
    case LIBINPUT_EVENT_GESTURE_HOLD_END: {
        auto* g = libinput_event_get_gesture_event(ev);
        gesture_event out{};
        switch (type) {
        case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
        case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
        case LIBINPUT_EVENT_GESTURE_SWIPE_END: out.type = gesture_event::kind::swipe; break;
        case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
        case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
        case LIBINPUT_EVENT_GESTURE_PINCH_END: out.type = gesture_event::kind::pinch; break;
        default: out.type = gesture_event::kind::hold; break;
        }
        out.stage = type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                            type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN ||
                            type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN
                        ? gesture_event::phase::begin
                    : type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ||
                            type == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE
                        ? gesture_event::phase::update
                        : gesture_event::phase::end;
        out.fingers = static_cast<unsigned>(libinput_event_gesture_get_finger_count(g));
        out.time_ms = libinput_event_gesture_get_time(g);
        out.time_us = libinput_event_gesture_get_time_usec(g);
        if (out.stage == gesture_event::phase::update) {
            out.dx = libinput_event_gesture_get_dx(g);
            out.dy = libinput_event_gesture_get_dy(g);
            out.dx_unaccel = libinput_event_gesture_get_dx_unaccelerated(g);
            out.dy_unaccel = libinput_event_gesture_get_dy_unaccelerated(g);
            if (out.type == gesture_event::kind::pinch) {
                out.scale = libinput_event_gesture_get_scale(g);
                out.rotation = libinput_event_gesture_get_angle_delta(g);
            }
        }
        if (out.stage == gesture_event::phase::end)
            out.cancelled = libinput_event_gesture_get_cancelled(g) != 0;
        if (sink_.gesture)
            sink_.gesture(out, sink_.user);
        break;
    }

    default:
        break;
    }
}

#endif // LUMEN_HAS_INPUT

#if defined(LUMEN_HAS_INPUT)
namespace {

libinput_device* li(void* d) { return static_cast<libinput_device*>(d); }

const lx::input::device_ops k_libinput_ops = {
    .tap_finger_count = [](void* d) { return libinput_device_config_tap_get_finger_count(li(d)); },
    .set_tap =
        [](void* d, bool on) {
            (void)libinput_device_config_tap_set_enabled(
                li(d), on ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
        },
    .set_tap_button_map =
        [](void* d, unsigned map) {
            (void)libinput_device_config_tap_set_button_map(
                li(d), map ? LIBINPUT_CONFIG_TAP_MAP_LMR : LIBINPUT_CONFIG_TAP_MAP_LRM);
        },
    .set_drag =
        [](void* d, bool on) {
            (void)libinput_device_config_tap_set_drag_enabled(
                li(d), on ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
        },
    .set_drag_lock =
        [](void* d, bool on) {
            (void)libinput_device_config_tap_set_drag_lock_enabled(
                li(d), on ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
        },
#if defined(LUMEN_HAS_LIBINPUT_3FG_DRAG)
    .three_finger_drag_fingers =
        [](void* d) { return static_cast<int>(libinput_device_config_3fg_drag_get_finger_count(li(d))); },
    .set_three_finger_drag =
        [](void* d, bool on) {
            (void)libinput_device_config_3fg_drag_set_enabled(
                li(d), on ? LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG : LIBINPUT_CONFIG_3FG_DRAG_DISABLED);
        },
#else
    .three_finger_drag_fingers = nullptr,
    .set_three_finger_drag = nullptr,
#endif
    .natural_scroll_supported =
        [](void* d) { return libinput_device_config_scroll_has_natural_scroll(li(d)) != 0; },
    .set_natural_scroll =
        [](void* d, bool on) {
            (void)libinput_device_config_scroll_set_natural_scroll_enabled(li(d), on ? 1 : 0);
        },
    .scroll_methods =
        [](void* d) { return static_cast<unsigned>(libinput_device_config_scroll_get_methods(li(d))); },
    .default_scroll_method =
        [](void* d) {
            return static_cast<unsigned>(libinput_device_config_scroll_get_default_method(li(d)));
        },
    .set_scroll_method =
        [](void* d, unsigned m) {
            (void)libinput_device_config_scroll_set_method(
                li(d), static_cast<libinput_config_scroll_method>(m));
        },
    .click_methods =
        [](void* d) { return static_cast<unsigned>(libinput_device_config_click_get_methods(li(d))); },
    .default_click_method =
        [](void* d) {
            return static_cast<unsigned>(libinput_device_config_click_get_default_method(li(d)));
        },
    .set_click_method =
        [](void* d, unsigned m) {
            (void)libinput_device_config_click_set_method(
                li(d), static_cast<libinput_config_click_method>(m));
        },
    .accel_supported = [](void* d) { return libinput_device_config_accel_is_available(li(d)) != 0; },
    .accel_profiles =
        [](void* d) { return static_cast<unsigned>(libinput_device_config_accel_get_profiles(li(d))); },
    .default_accel_profile =
        [](void* d) {
            return static_cast<unsigned>(libinput_device_config_accel_get_default_profile(li(d)));
        },
    .set_accel_profile =
        [](void* d, unsigned p) {
            (void)libinput_device_config_accel_set_profile(
                li(d), static_cast<libinput_config_accel_profile>(p));
        },
    .set_accel_speed =
        [](void* d, double s) { (void)libinput_device_config_accel_set_speed(li(d), s); },
    .left_handed_supported =
        [](void* d) { return libinput_device_config_left_handed_is_available(li(d)) != 0; },
    .set_left_handed =
        [](void* d, bool on) { (void)libinput_device_config_left_handed_set(li(d), on ? 1 : 0); },
    .dwt_supported = [](void* d) { return libinput_device_config_dwt_is_available(li(d)) != 0; },
    .set_dwt =
        [](void* d, bool on) {
            (void)libinput_device_config_dwt_set_enabled(
                li(d), on ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
        },
    .middle_emulation_supported =
        [](void* d) { return libinput_device_config_middle_emulation_is_available(li(d)) != 0; },
    .set_middle_emulation =
        [](void* d, bool on) {
            (void)libinput_device_config_middle_emulation_set_enabled(
                li(d), on ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                          : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
        },
};

} // namespace
#endif

const lx::input::device_ops& lx::input::input_manager::libinput_ops() {
#if defined(LUMEN_HAS_INPUT)
    return k_libinput_ops;
#else
    static const device_ops none{};
    return none;
#endif
}

void lx::input::input_manager::configure_device(device_entry& e) {
#if defined(LUMEN_HAS_INPUT)
    if (!device_config_ || e.kind == device_class::other)
        return;
    const char* name = libinput_device_get_name(li(e.device));
    pointer_config c{};
    if (!device_config_(device_config_user_, name, e.kind, c))
        return;
    apply_report report{};
    apply_pointer_config(e.device, k_libinput_ops, e.kind, c, report);
    e.scroll_factor = c.scroll_factor;
    if (device_report_)
        device_report_(device_config_user_, name, e.kind, report);
#else
    (void)e;
#endif
}

void lx::input::input_manager::reapply_device_config() {
    for (device_entry& e : devices_)
        configure_device(e);
}

void lx::input::input_manager::update_leds() {
#if defined(LUMEN_HAS_INPUT)
    const unsigned leds = seat_.keymap().led_mask();
    if (leds == leds_)
        return;
    leds_ = leds;
    for (const device_entry& e : devices_)
        if (e.keyboard)
            libinput_device_led_update(li(e.device), static_cast<libinput_led>(leds));
#endif
}

unsigned lx::input::input_manager::drain() {
#if defined(LUMEN_HAS_INPUT)
    auto* li = static_cast<struct libinput*>(libinput_);
    if (!li)
        return 0;
    constexpr unsigned k_batch = 64;
    constexpr unsigned k_max_gone = 8;
    unsigned handled = 0;
    for (;;) {
        libinput_event* batch[k_batch];
        unsigned n = 0;
        while (n < k_batch) {
            libinput_event* ev = libinput_get_event(li);
            if (!ev)
                break;
            batch[n++] = ev;
        }
        if (n == 0)
            break;
        libinput_device* gone[k_max_gone];
        unsigned gone_count = 0;
        for (unsigned i = 0; i < n && gone_count < k_max_gone; ++i)
            if (libinput_event_get_type(batch[i]) == LIBINPUT_EVENT_DEVICE_REMOVED)
                gone[gone_count++] = libinput_event_get_device(batch[i]);
        for (unsigned i = 0; i < n; ++i) {
            removing_ = false;
            if (gone_count) {
                libinput_device* dev = libinput_event_get_device(batch[i]);
                for (unsigned g = 0; g < gone_count; ++g)
                    removing_ = removing_ || gone[g] == dev;
            }
            handle_event(batch[i]);
            libinput_event_destroy(batch[i]);
            ++handled;
        }
        removing_ = false;
    }
    return handled;
#else
    return 0;
#endif
}

unsigned lx::input::input_manager::dispatch() {
#if defined(LUMEN_HAS_INPUT)
    if (!libinput_)
        return 0;
    auto* li = static_cast<struct libinput*>(libinput_);
    if (libinput_dispatch(li) != 0)
        return 0;

    return drain();
#else
    return 0;
#endif
}
