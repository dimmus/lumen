module;

#include <cstddef>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

import lx.foundation;
import lx.input;
import lx.trace;

export module lx.compositor:input_router;

export namespace lx::compositor {

/// Delivers seat events to the clients that should receive them.
///
/// Translating input is only half the job; the other half is deciding *who* hears it and
/// telling the previous holder it has stopped. Wayland makes that explicit — every focus
/// change is a `leave` to one surface and an `enter` to another, both carrying serials —
/// and getting it wrong is not subtle: a client that never receives `leave` keeps drawing
/// itself focused forever, and one that receives `enter` without the held-key array is left
/// with modifiers stuck down.
///
/// Resources are tracked per client because a client may create several `wl_keyboard`
/// objects from the same seat, and all of them must be fed.
class input_router {
public:
    /// Registers a seat-derived resource. Unregistered automatically on destroy.
    void add_keyboard(void* wl_keyboard_resource);
    void add_pointer(void* wl_pointer_resource);
    void add_touch(void* wl_touch_resource);
    void remove(void* resource);

    /// `surface` is a `wl_surface` resource, or null for "no focus". Sends `leave` to the
    /// previous surface's keyboards and `enter` to the new one's, with the keys currently
    /// held so the client starts from the right state.
    void set_keyboard_focus(void* wl_surface_resource, lx::input::seat& seat);
    [[nodiscard]] void* keyboard_focus() const { return keyboard_focus_; }

    /// Pointer focus, with the position in surface-local coordinates.
    void set_pointer_focus(void* wl_surface_resource, double sx, double sy,
                           lx::input::seat& seat);
    [[nodiscard]] void* pointer_focus() const { return pointer_focus_; }

    void send_key(const lx::input::key_event& event, lx::input::seat& seat);
    void send_modifiers(const lx::input::modifier_state& mods, lx::input::seat& seat);
    void send_pointer_motion(double sx, double sy, unsigned time_ms);
    void send_pointer_button(const lx::input::pointer_button_event& event,
                             lx::input::seat& seat);
    void send_pointer_axis(const lx::input::pointer_axis_event& event);

    [[nodiscard]] unsigned keyboard_count() const { return keyboards_.count; }
    [[nodiscard]] unsigned pointer_count() const { return pointers_.count; }
    [[nodiscard]] unsigned touch_count() const { return touches_.count; }

private:
    static constexpr unsigned k_max_resources = 64;

    struct resource_set {
        void* items[k_max_resources]{};
        unsigned count = 0;

        void add(void* r);
        void remove(void* r);
        /// True when `r` belongs to the same client as `surface`.
        [[nodiscard]] static bool same_client(void* r, void* surface);
    };

    resource_set keyboards_{};
    resource_set pointers_{};
    resource_set touches_{};
    void* keyboard_focus_ = nullptr;
    void* pointer_focus_ = nullptr;
};

} // namespace lx::compositor


void lx::compositor::input_router::resource_set::add(void* r) {
    if (!r || count >= k_max_resources)
        return;
    for (unsigned i = 0; i < count; ++i) {
        if (items[i] == r)
            return;
    }
    items[count++] = r;
}

void lx::compositor::input_router::resource_set::remove(void* r) {
    for (unsigned i = 0; i < count; ++i) {
        if (items[i] != r)
            continue;
        items[i] = items[--count];
        items[count] = nullptr;
        return;
    }
}

bool lx::compositor::input_router::resource_set::same_client(void* r, void* surface) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!r || !surface)
        return false;
    return wl_resource_get_client(static_cast<wl_resource*>(r)) ==
           wl_resource_get_client(static_cast<wl_resource*>(surface));
#else
    (void)r;
    (void)surface;
    return false;
#endif
}

void lx::compositor::input_router::add_keyboard(void* r) { keyboards_.add(r); }
void lx::compositor::input_router::add_pointer(void* r) { pointers_.add(r); }
void lx::compositor::input_router::add_touch(void* r) { touches_.add(r); }

void lx::compositor::input_router::remove(void* r) {
    keyboards_.remove(r);
    pointers_.remove(r);
    touches_.remove(r);
    // A destroyed resource must not stay the focus, or the next event is sent into a
    // dangling pointer.
    if (keyboard_focus_ == r)
        keyboard_focus_ = nullptr;
    if (pointer_focus_ == r)
        pointer_focus_ = nullptr;
}

void lx::compositor::input_router::set_keyboard_focus(void* surface, lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (keyboard_focus_ == surface)
        return;

    if (keyboard_focus_) {
        const unsigned serial = seat.next_serial();
        for (unsigned i = 0; i < keyboards_.count; ++i) {
            if (resource_set::same_client(keyboards_.items[i], keyboard_focus_)) {
                wl_keyboard_send_leave(static_cast<wl_resource*>(keyboards_.items[i]), serial,
                                       static_cast<wl_resource*>(keyboard_focus_));
            }
        }
    }

    keyboard_focus_ = surface;
    seat.set_keyboard_focus(surface ? lx::surface_id{reinterpret_cast<unsigned long long>(surface)}
                                    : lx::surface_id{});
    if (!surface)
        return;

    // `enter` carries the keys already held. Without it the client believes nothing is
    // pressed and any modifier held across the focus change is stuck until it is tapped.
    const unsigned serial = seat.next_serial();
    wl_array keys{};
    wl_array_init(&keys);
    for (unsigned i = 0; i < seat.pressed_count(); ++i) {
        if (auto* slot = static_cast<uint32_t*>(wl_array_add(&keys, sizeof(uint32_t))))
            *slot = seat.pressed_keys()[i];
    }

    const auto mods = seat.keymap().modifiers();
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (!resource_set::same_client(keyboards_.items[i], surface))
            continue;
        auto* kb = static_cast<wl_resource*>(keyboards_.items[i]);
        wl_keyboard_send_enter(kb, serial, static_cast<wl_resource*>(surface), &keys);
        // Modifiers immediately after enter, for the same reason the key array is sent.
        wl_keyboard_send_modifiers(kb, seat.next_serial(), mods.depressed, mods.latched,
                                   mods.locked, mods.group);
    }
    wl_array_release(&keys);
#else
    (void)surface;
    (void)seat;
#endif
}

void lx::compositor::input_router::set_pointer_focus(void* surface, double sx, double sy,
                                                     lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (pointer_focus_ == surface)
        return;

    if (pointer_focus_) {
        const unsigned serial = seat.next_serial();
        for (unsigned i = 0; i < pointers_.count; ++i) {
            if (resource_set::same_client(pointers_.items[i], pointer_focus_)) {
                wl_pointer_send_leave(static_cast<wl_resource*>(pointers_.items[i]), serial,
                                      static_cast<wl_resource*>(pointer_focus_));
            }
        }
    }

    pointer_focus_ = surface;
    seat.set_pointer_focus(surface ? lx::surface_id{reinterpret_cast<unsigned long long>(surface)}
                                   : lx::surface_id{});
    if (!surface)
        return;

    const unsigned serial = seat.next_serial();
    for (unsigned i = 0; i < pointers_.count; ++i) {
        if (!resource_set::same_client(pointers_.items[i], surface))
            continue;
        auto* p = static_cast<wl_resource*>(pointers_.items[i]);
        wl_pointer_send_enter(p, serial, static_cast<wl_resource*>(surface),
                              wl_fixed_from_double(sx), wl_fixed_from_double(sy));
        if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(p);
    }
#else
    (void)surface;
    (void)sx;
    (void)sy;
    (void)seat;
#endif
}

void lx::compositor::input_router::send_key(const lx::input::key_event& event,
                                            lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!keyboard_focus_)
        return;
    const unsigned serial = seat.next_serial();
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (!resource_set::same_client(keyboards_.items[i], keyboard_focus_))
            continue;
        wl_keyboard_send_key(static_cast<wl_resource*>(keyboards_.items[i]), serial,
                             event.time_ms, event.evdev_keycode,
                             event.pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                           : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
#else
    (void)event;
    (void)seat;
#endif
}

void lx::compositor::input_router::send_modifiers(const lx::input::modifier_state& mods,
                                                  lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!keyboard_focus_)
        return;
    const unsigned serial = seat.next_serial();
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (!resource_set::same_client(keyboards_.items[i], keyboard_focus_))
            continue;
        wl_keyboard_send_modifiers(static_cast<wl_resource*>(keyboards_.items[i]), serial,
                                   mods.depressed, mods.latched, mods.locked, mods.group);
    }
#else
    (void)mods;
    (void)seat;
#endif
}

void lx::compositor::input_router::send_pointer_motion(double sx, double sy,
                                                       unsigned time_ms) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!pointer_focus_)
        return;
    for (unsigned i = 0; i < pointers_.count; ++i) {
        if (!resource_set::same_client(pointers_.items[i], pointer_focus_))
            continue;
        auto* p = static_cast<wl_resource*>(pointers_.items[i]);
        wl_pointer_send_motion(p, time_ms, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
        if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(p);
    }
#else
    (void)sx;
    (void)sy;
    (void)time_ms;
#endif
}

void lx::compositor::input_router::send_pointer_button(
    const lx::input::pointer_button_event& event, lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!pointer_focus_)
        return;
    const unsigned serial = seat.next_serial();
    for (unsigned i = 0; i < pointers_.count; ++i) {
        if (!resource_set::same_client(pointers_.items[i], pointer_focus_))
            continue;
        auto* p = static_cast<wl_resource*>(pointers_.items[i]);
        wl_pointer_send_button(p, serial, event.time_ms, event.evdev_code,
                               event.pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                             : WL_POINTER_BUTTON_STATE_RELEASED);
        if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(p);
    }
#else
    (void)event;
    (void)seat;
#endif
}

void lx::compositor::input_router::send_pointer_axis(
    const lx::input::pointer_axis_event& event) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!pointer_focus_)
        return;
    for (unsigned i = 0; i < pointers_.count; ++i) {
        if (!resource_set::same_client(pointers_.items[i], pointer_focus_))
            continue;
        auto* p = static_cast<wl_resource*>(pointers_.items[i]);
        const int version = wl_resource_get_version(p);

        // Source first: it tells the client whether this is a notch or a continuous drag,
        // which decides whether kinetic scrolling applies.
        if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
            uint32_t src = WL_POINTER_AXIS_SOURCE_WHEEL;
            switch (event.source) {
            case lx::input::axis_source::finger: src = WL_POINTER_AXIS_SOURCE_FINGER; break;
            case lx::input::axis_source::continuous:
                src = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
                break;
            case lx::input::axis_source::wheel_tilt:
                src = WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
                break;
            default: break;
            }
            wl_pointer_send_axis_source(p, src);
        }

        if (event.vertical != 0.0) {
            if (event.vertical_discrete != 0.0 &&
                version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
                wl_pointer_send_axis_discrete(p, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              static_cast<int32_t>(event.vertical_discrete));
            }
            wl_pointer_send_axis(p, event.time_ms, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(event.vertical));
        }
        if (event.horizontal != 0.0) {
            if (event.horizontal_discrete != 0.0 &&
                version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
                wl_pointer_send_axis_discrete(p, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                              static_cast<int32_t>(event.horizontal_discrete));
            }
            wl_pointer_send_axis(p, event.time_ms, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 wl_fixed_from_double(event.horizontal));
        }
        if (version >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(p);
    }
#else
    (void)event;
#endif
}
