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
    /// Registers a seat-derived resource, and brings it up to date immediately.
    ///
    /// Bringing it up to date is not optional. A client creates its `wl_keyboard` whenever
    /// it likes — `wev` does so *after* mapping its toplevel — and if focus was already
    /// decided by then, the `enter` went out to a set of resources that did not yet include
    /// this one. The client would receive keys for a surface it was never told it had focus
    /// on, which is a protocol violation and leaves well-behaved clients ignoring the keys.
    void add_keyboard(void* wl_keyboard_resource, lx::input::seat& seat);
    void add_pointer(void* wl_pointer_resource, lx::input::seat& seat);
    void add_touch(void* wl_touch_resource);
    void remove(void* resource);

    /// `surface` is a `wl_surface` resource, or null for "no focus". Sends `leave` to the
    /// previous surface's keyboards and `enter` to the new one's, with the keys currently
    /// held so the client starts from the right state.
    void set_keyboard_focus(void* wl_surface_resource, lx::input::seat& seat);
    [[nodiscard]] void* keyboard_focus() const { return keyboard_focus_; }

    /// Pointer focus, with the position in surface-local coordinates. Sends `leave` to the
    /// old surface and `enter` (+ `frame`) to the new one. Returns the `enter` serial, or 0
    /// when focus did not change or there is no new focus.
    unsigned set_pointer_focus(void* wl_surface_resource, double sx, double sy,
                               lx::input::seat& seat);
    [[nodiscard]] void* pointer_focus() const { return pointer_focus_; }
    /// `wl_client*` owning the pointer-focus surface, or null.
    [[nodiscard]] void* pointer_focus_client() const { return pointer_focus_client_; }
    /// True when `serial` is an `enter` this router sent for the current pointer focus —
    /// what wl_pointer.set_cursor and wp_cursor_shape_device_v1.set_shape must quote.
    [[nodiscard]] bool is_pointer_enter_serial(unsigned serial) const;
    /// Bumped on every pointer-focus change, including focus lost to a destroyed surface,
    /// so state tied to the focused client (its cursor request) can tell it is stale.
    [[nodiscard]] unsigned pointer_focus_generation() const { return pointer_focus_generation_; }

    void send_key(const lx::input::key_event& event, lx::input::seat& seat);
    void send_modifiers(const lx::input::modifier_state& mods, lx::input::seat& seat);
    /// Surface-local position; also remembered for a `wl_pointer` created later, whose
    /// `enter` must carry where the pointer is on that surface.
    void send_pointer_motion(double sx, double sy, unsigned time_ms);
    /// Returns the serial the button event carried, or 0 when nothing has pointer focus.
    /// A press serial is what xdg_toplevel.move/resize must quote.
    unsigned send_pointer_button(const lx::input::pointer_button_event& event,
                                 lx::input::seat& seat);
    void send_pointer_axis(const lx::input::pointer_axis_event& event);

    [[nodiscard]] unsigned keyboard_count() const { return keyboards_.count; }
    [[nodiscard]] unsigned pointer_count() const { return pointers_.count; }
    [[nodiscard]] unsigned touch_count() const { return touches_.count; }

private:
    /// Sends `enter` plus the modifiers that must follow it to one keyboard resource.
    /// Shared by focus changes and late resource creation so the two cannot drift.
    void send_enter_to(void* keyboard_resource, void* surface, lx::input::seat& seat);

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
    void* pointer_focus_client_ = nullptr;
    /// Last surface-local pointer position sent to the focus (enter or motion).
    double pointer_sx_ = 0.0;
    double pointer_sy_ = 0.0;
    /// Enter serials sent for the current pointer focus: one per focus change, plus one per
    /// wl_pointer the client creates while already focused.
    static constexpr unsigned k_enter_serials = 4;
    unsigned pointer_enter_serials_[k_enter_serials]{};
    unsigned pointer_enter_count_ = 0;
    unsigned pointer_focus_generation_ = 0;

    void note_enter_serial(unsigned serial);
    void clear_pointer_focus_state();
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

void lx::compositor::input_router::send_enter_to(void* keyboard_resource, void* surface,
                                                 lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!keyboard_resource || !surface)
        return;
    auto* kb = static_cast<wl_resource*>(keyboard_resource);

    // The keys held right now. A client told it has focus but not what is pressed will
    // hold any modifier that was down across the transition until it is tapped again.
    wl_array keys{};
    wl_array_init(&keys);
    for (unsigned i = 0; i < seat.pressed_count(); ++i) {
        if (auto* slot = static_cast<uint32_t*>(wl_array_add(&keys, sizeof(uint32_t))))
            *slot = seat.pressed_keys()[i];
    }
    wl_keyboard_send_enter(kb, seat.next_serial(), static_cast<wl_resource*>(surface), &keys);
    wl_array_release(&keys);

    // Modifiers immediately after, for the same reason the key array is sent.
    const auto mods = seat.keymap().modifiers();
    wl_keyboard_send_modifiers(kb, seat.next_serial(), mods.depressed, mods.latched,
                               mods.locked, mods.group);
#else
    (void)keyboard_resource;
    (void)surface;
    (void)seat;
#endif
}

void lx::compositor::input_router::add_keyboard(void* r, lx::input::seat& seat) {
    keyboards_.add(r);
#if defined(LUMEN_HAS_WAYLAND)
    // Already focused? Then this resource missed the `enter` and has to be told now.
    if (!r || !keyboard_focus_ || !resource_set::same_client(r, keyboard_focus_))
        return;
    send_enter_to(r, keyboard_focus_, seat);
#else
    (void)seat;
#endif
}

void lx::compositor::input_router::add_pointer(void* r, lx::input::seat& seat) {
    pointers_.add(r);
#if defined(LUMEN_HAS_WAYLAND)
    if (!r || !pointer_focus_ || !resource_set::same_client(r, pointer_focus_))
        return;
    auto* p = static_cast<wl_resource*>(r);
    const unsigned serial = seat.next_serial();
    note_enter_serial(serial);
    wl_pointer_send_enter(p, serial, static_cast<wl_resource*>(pointer_focus_),
                          wl_fixed_from_double(pointer_sx_), wl_fixed_from_double(pointer_sy_));
    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(p);
#else
    (void)seat;
#endif
}

void lx::compositor::input_router::add_touch(void* r) { touches_.add(r); }

void lx::compositor::input_router::remove(void* r) {
    keyboards_.remove(r);
    pointers_.remove(r);
    touches_.remove(r);
    // A destroyed resource must not stay the focus, or the next event is sent into a
    // dangling pointer.
    if (keyboard_focus_ == r)
        keyboard_focus_ = nullptr;
    if (pointer_focus_ == r) {
        pointer_focus_ = nullptr;
        clear_pointer_focus_state();
    }
}

void lx::compositor::input_router::note_enter_serial(unsigned serial) {
    if (pointer_enter_count_ < k_enter_serials) {
        pointer_enter_serials_[pointer_enter_count_++] = serial;
        return;
    }
    for (unsigned i = 1; i < k_enter_serials; ++i)
        pointer_enter_serials_[i - 1] = pointer_enter_serials_[i];
    pointer_enter_serials_[k_enter_serials - 1] = serial;
}

void lx::compositor::input_router::clear_pointer_focus_state() {
    pointer_focus_client_ = nullptr;
    pointer_enter_count_ = 0;
    ++pointer_focus_generation_;
}

bool lx::compositor::input_router::is_pointer_enter_serial(unsigned serial) const {
    if (serial == 0)
        return false;
    for (unsigned i = 0; i < pointer_enter_count_; ++i)
        if (pointer_enter_serials_[i] == serial)
            return true;
    return false;
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

    (void)serial;
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (resource_set::same_client(keyboards_.items[i], surface))
            send_enter_to(keyboards_.items[i], surface, seat);
    }
    wl_array_release(&keys);
#else
    (void)surface;
    (void)seat;
#endif
}

unsigned lx::compositor::input_router::set_pointer_focus(void* surface, double sx, double sy,
                                                         lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (pointer_focus_ == surface)
        return 0;

    if (pointer_focus_) {
        const unsigned serial = seat.next_serial();
        for (unsigned i = 0; i < pointers_.count; ++i) {
            if (resource_set::same_client(pointers_.items[i], pointer_focus_)) {
                auto* p = static_cast<wl_resource*>(pointers_.items[i]);
                wl_pointer_send_leave(p, serial, static_cast<wl_resource*>(pointer_focus_));
                // `leave` is a frame of its own (wl_pointer v5): a client that groups
                // events by frame would otherwise merge it with whatever comes next.
                if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(p);
            }
        }
    }

    pointer_focus_ = surface;
    clear_pointer_focus_state();
    seat.set_pointer_focus(surface ? lx::surface_id{reinterpret_cast<unsigned long long>(surface)}
                                   : lx::surface_id{});
    if (!surface)
        return 0;

    pointer_focus_client_ = wl_resource_get_client(static_cast<wl_resource*>(surface));
    pointer_sx_ = sx;
    pointer_sy_ = sy;
    const unsigned serial = seat.next_serial();
    note_enter_serial(serial);
    for (unsigned i = 0; i < pointers_.count; ++i) {
        if (!resource_set::same_client(pointers_.items[i], surface))
            continue;
        auto* p = static_cast<wl_resource*>(pointers_.items[i]);
        wl_pointer_send_enter(p, serial, static_cast<wl_resource*>(surface),
                              wl_fixed_from_double(sx), wl_fixed_from_double(sy));
        if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(p);
    }
    return serial;
#else
    (void)surface;
    (void)sx;
    (void)sy;
    (void)seat;
    return 0;
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
    pointer_sx_ = sx;
    pointer_sy_ = sy;
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

unsigned lx::compositor::input_router::send_pointer_button(
    const lx::input::pointer_button_event& event, lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!pointer_focus_)
        return 0;
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
    return serial;
#else
    (void)event;
    (void)seat;
    return 0;
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
