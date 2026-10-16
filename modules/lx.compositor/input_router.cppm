module;

#include <cstddef>
#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

import lx.foundation;
import lx.input;
import lx.trace;

export module lx.compositor:input_router;

export namespace lx::compositor {

/// What an input serial was sent for.
enum class input_serial_kind : unsigned char {
    focus,
    key_press,
    key_release,
    modifiers,
    button_press,
    button_release
};

/// Wraparound-safe "serial `a` was issued after serial `b`": serials are 32-bit counters
/// that wrap, so plain `>` is wrong after 2^32 events.
[[nodiscard]] constexpr bool serial_newer(unsigned a, unsigned b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

/// The serials of input and focus events delivered to each client, so a request quoting a
/// serial ("the serial of the event that triggered this", wl_data_device.set_selection,
/// xdg_activation_v1) can be checked against what that client was really sent.
///
/// Serials come from one seat counter that configures share, so "is this serial recent" is
/// not the question; "was this one of *this client's* input events" is. Kept as ranges per
/// client, wlroots' ringset: consecutive serials to the same client (a key's press and
/// release, an enter and its modifiers) compress into one range. A serial older than the
/// oldest range kept is rejected — no real client quotes an event that old, and accepting
/// it (wlroots does) would let a background client claim an ancient serial.
///
/// Wayland-free: clients are opaque `wl_client*` pointers. Fixed storage, no allocation;
/// beyond `k_clients` the least recently noted client is evicted.
class input_serial_log {
public:
    static constexpr unsigned k_clients = 64;
    static constexpr unsigned k_ranges = 32;

    void note(void* client, unsigned serial, input_serial_kind kind);
    [[nodiscard]] bool delivered(void* client, unsigned serial) const;
    /// The newest key or button press delivered to `client`, or 0 when none was.
    [[nodiscard]] unsigned last_user_serial(void* client) const;
    [[nodiscard]] unsigned last_button_press(void* client) const;
    [[nodiscard]] unsigned last_key_press(void* client) const;
    /// The client is gone (its destroy signal); libwayland may reuse the pointer after.
    void forget(void* client);

private:
    struct range {
        unsigned first = 0;
        unsigned last = 0;
    };
    struct entry {
        void* client = nullptr;
        unsigned long long stamp = 0; // LRU
        range ranges[k_ranges]{};
        unsigned count = 0; // ranges held
        unsigned head = 0;  // index of the newest
        unsigned last_button_press = 0;
        unsigned last_key_press = 0;
        unsigned last_user = 0;
    };
    [[nodiscard]] const entry* find(void* client) const;

    entry entries_[k_clients]{};
    unsigned long long clock_ = 0;
};

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
    /// `wl_client*` owning the keyboard-focus surface, or null.
    [[nodiscard]] void* keyboard_focus_client() const { return keyboard_focus_client_; }

    /// Called when the client owning keyboard focus changes (to or from null included):
    /// after the old surface's `leave`s and before the new one's `enter`s, because the
    /// selection must reach a client "immediately before receiving keyboard focus"
    /// (wl_data_device.selection). Also when the focused surface is destroyed.
    using focus_client_hook = void (*)(void* user, void* old_client, void* new_client);
    void set_focus_client_hook(focus_client_hook fn, void* user) {
        focus_hook_ = fn;
        focus_hook_user_ = user;
    }

    /// Called after a key press or a button press reached `client` (`wl_client*`, with at
    /// least one of its resources): the liveness tracker pings on user input (Mutter).
    using user_input_hook = void (*)(void* user, void* client, unsigned serial,
                                     input_serial_kind kind);
    void set_user_input_hook(user_input_hook fn, void* user) {
        input_hook_ = fn;
        input_hook_user_ = user;
    }

    /// Withholds (or resumes) `wl_pointer.motion` to `client` (`wl_client*`): a client that
    /// stopped answering pings is not fed coalescable motion it cannot read, which would
    /// only fill its connection (Mutter). Keys, buttons and enter/leave keep flowing — a
    /// client that catches up must see them. On resume a client that has pointer focus gets
    /// one motion at the current position. At most `k_max_muted` clients; more stay unmuted.
    void set_motion_muted(void* client, bool muted, unsigned time_ms);
    [[nodiscard]] bool motion_muted(void* client) const;
    /// The client is gone: nothing keyed by its `wl_client*` may outlive it.
    void forget_client(void* client);
    static constexpr unsigned k_max_muted = 16;

    /// Input and focus serials delivered per client (keyboard enter/leave/key/modifiers,
    /// pointer enter/leave/button). Configure serials are not input and are never noted.
    [[nodiscard]] input_serial_log& serials() { return serials_; }
    [[nodiscard]] const input_serial_log& serials() const { return serials_; }

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
    /// True when `serial` is the last key press delivered to `client` (`wl_client*`), or
    /// the release of that same key — what xdg_popup.grab may quote for a keyboard-opened
    /// menu (Mutter's rule: a toolkit may open the menu only once the key is released).
    [[nodiscard]] bool is_key_grab_serial(unsigned serial, void* client) const;
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
        /// True when any resource of the set belongs to `client` (`wl_client*`).
        [[nodiscard]] bool has_client(void* client) const;
    };

    /// Notes `serial` for `client` when at least one of its resources in `set` received it.
    void note_serial(const resource_set& set, void* client, unsigned serial,
                     input_serial_kind kind);
    void notify_focus_client(void* new_client);

    resource_set keyboards_{};
    resource_set pointers_{};
    resource_set touches_{};
    void* keyboard_focus_ = nullptr;
    void* keyboard_focus_client_ = nullptr;
    focus_client_hook focus_hook_ = nullptr;
    void* focus_hook_user_ = nullptr;
    user_input_hook input_hook_ = nullptr;
    void* input_hook_user_ = nullptr;
    void* muted_[k_max_muted]{};
    unsigned muted_count_ = 0;
    input_serial_log serials_{};
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
    /// Last key press delivered, its key and its client; the release serial of that key.
    unsigned last_key_press_serial_ = 0;
    unsigned last_key_press_code_ = 0;
    unsigned last_key_release_serial_ = 0;
    void* last_key_client_ = nullptr;

    void note_enter_serial(unsigned serial);
    void clear_pointer_focus_state();
};

} // namespace lx::compositor


// ── input_serial_log ───────────────────────────────────────────────────────

void lx::compositor::input_serial_log::note(void* client, unsigned serial,
                                            input_serial_kind kind) {
    if (!client)
        return;
    entry* e = nullptr;
    entry* victim = nullptr; // a free slot if there is one, else the least recently noted
    for (auto& candidate : entries_) {
        if (candidate.client == client) {
            e = &candidate;
            break;
        }
        if (!victim || (victim->client && (!candidate.client || candidate.stamp < victim->stamp)))
            victim = &candidate;
    }
    if (!e) {
        e = victim;
        *e = {};
        e->client = client;
    }
    e->stamp = ++clock_;

    if (e->count == 0) {
        e->ranges[0] = {serial, serial};
        e->head = 0;
        e->count = 1;
    } else {
        range& newest = e->ranges[e->head];
        if (serial == newest.last + 1u) {
            newest.last = serial;
        } else if (serial != newest.last) {
            e->head = (e->head + 1) % k_ranges;
            e->ranges[e->head] = {serial, serial};
            if (e->count < k_ranges)
                ++e->count;
        }
    }
    switch (kind) {
    case input_serial_kind::key_press:
        e->last_key_press = serial;
        e->last_user = serial;
        break;
    case input_serial_kind::button_press:
        e->last_button_press = serial;
        e->last_user = serial;
        break;
    default: break;
    }
}

const lx::compositor::input_serial_log::entry*
lx::compositor::input_serial_log::find(void* client) const {
    if (!client)
        return nullptr;
    for (const auto& e : entries_)
        if (e.client == client)
            return &e;
    return nullptr;
}

bool lx::compositor::input_serial_log::delivered(void* client, unsigned serial) const {
    const entry* e = find(client);
    if (!e)
        return false;
    // Newest range first: a serial newer than a range's end lies between that range and the
    // next newer one (or in the future), and nothing older can contain it.
    for (unsigned i = 0; i < e->count; ++i) {
        const range& r = e->ranges[(e->head + k_ranges - i) % k_ranges];
        if (serial_newer(serial, r.last))
            return false;
        if (!serial_newer(r.first, serial))
            return true;
    }
    return false;
}

unsigned lx::compositor::input_serial_log::last_user_serial(void* client) const {
    const entry* e = find(client);
    return e ? e->last_user : 0;
}

unsigned lx::compositor::input_serial_log::last_button_press(void* client) const {
    const entry* e = find(client);
    return e ? e->last_button_press : 0;
}

unsigned lx::compositor::input_serial_log::last_key_press(void* client) const {
    const entry* e = find(client);
    return e ? e->last_key_press : 0;
}

void lx::compositor::input_serial_log::forget(void* client) {
    if (!client)
        return;
    for (auto& e : entries_)
        if (e.client == client)
            e = {};
}

// ── input_router ───────────────────────────────────────────────────────────

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

bool lx::compositor::input_router::resource_set::has_client(void* client) const {
#if defined(LUMEN_HAS_WAYLAND)
    if (!client)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (wl_resource_get_client(static_cast<wl_resource*>(items[i])) == client)
            return true;
#else
    (void)client;
#endif
    return false;
}

void lx::compositor::input_router::note_serial(const resource_set& set, void* client,
                                               unsigned serial, input_serial_kind kind) {
    if (set.has_client(client))
        serials_.note(client, serial, kind);
}

void lx::compositor::input_router::set_motion_muted(void* client, bool muted, unsigned time_ms) {
    if (!client)
        return;
    for (unsigned i = 0; i < muted_count_; ++i) {
        if (muted_[i] != client)
            continue;
        if (muted)
            return;
        muted_[i] = muted_[--muted_count_];
        muted_[muted_count_] = nullptr;
        // Caught up: the pointer may have moved a long way while it was withheld.
        if (pointer_focus_ && pointer_focus_client_ == client)
            send_pointer_motion(pointer_sx_, pointer_sy_, time_ms);
        return;
    }
    if (muted && muted_count_ < k_max_muted)
        muted_[muted_count_++] = client;
}

bool lx::compositor::input_router::motion_muted(void* client) const {
    for (unsigned i = 0; i < muted_count_; ++i)
        if (muted_[i] == client)
            return client != nullptr;
    return false;
}

void lx::compositor::input_router::forget_client(void* client) {
    serials_.forget(client);
    for (unsigned i = 0; i < muted_count_; ++i) {
        if (muted_[i] == client) {
            muted_[i] = muted_[--muted_count_];
            muted_[muted_count_] = nullptr;
            return;
        }
    }
}

void lx::compositor::input_router::notify_focus_client(void* new_client) {
    void* old_client = keyboard_focus_client_;
    if (old_client == new_client)
        return;
    keyboard_focus_client_ = new_client;
    if (focus_hook_)
        focus_hook_(focus_hook_user_, old_client, new_client);
}

void lx::compositor::input_router::send_enter_to(void* keyboard_resource, void* surface,
                                                 lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!keyboard_resource || !surface)
        return;
    auto* kb = static_cast<wl_resource*>(keyboard_resource);
    void* client = wl_resource_get_client(kb);

    // The keys held right now. A client told it has focus but not what is pressed will
    // hold any modifier that was down across the transition until it is tapped again.
    // A wl_array over stack storage, never released: wl_array_add would allocate on every
    // focus change, which a click-to-focus makes.
    constexpr unsigned k_max_held = 32;
    uint32_t held[k_max_held];
    unsigned n = 0;
    for (unsigned i = 0; i < seat.pressed_count() && n < k_max_held; ++i)
        held[n++] = seat.pressed_keys()[i];
    wl_array keys{};
    keys.data = held;
    keys.size = n * sizeof(uint32_t);
    keys.alloc = sizeof(held);
    const unsigned enter_serial = seat.next_serial();
    wl_keyboard_send_enter(kb, enter_serial, static_cast<wl_resource*>(surface), &keys);
    serials_.note(client, enter_serial, input_serial_kind::focus);

    // Modifiers immediately after, for the same reason the key array is sent.
    const auto mods = seat.keymap().modifiers();
    const unsigned mods_serial = seat.next_serial();
    wl_keyboard_send_modifiers(kb, mods_serial, mods.depressed, mods.latched, mods.locked,
                               mods.group);
    serials_.note(client, mods_serial, input_serial_kind::modifiers);
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
    serials_.note(wl_resource_get_client(p), serial, input_serial_kind::focus);
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
    // dangling pointer. The client has lost keyboard focus with it (no `leave`: the surface
    // is gone), which is what selection offers are tied to.
    if (keyboard_focus_ == r) {
        keyboard_focus_ = nullptr;
        notify_focus_client(nullptr);
    }
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
        note_serial(keyboards_, keyboard_focus_client_, serial, input_serial_kind::focus);
    }

    keyboard_focus_ = surface;
    seat.set_keyboard_focus(surface ? lx::surface_id{reinterpret_cast<unsigned long long>(surface)}
                                    : lx::surface_id{});
    // Between the leaves and the enters: what the new client must know before it is told
    // it has the keyboard (its selection) goes out here.
    notify_focus_client(surface ? wl_resource_get_client(static_cast<wl_resource*>(surface))
                                : nullptr);
    if (!surface)
        return;

    // `enter` carries the keys already held. Without it the client believes nothing is
    // pressed and any modifier held across the focus change is stuck until it is tapped.
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (resource_set::same_client(keyboards_.items[i], surface))
            send_enter_to(keyboards_.items[i], surface, seat);
    }
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
        note_serial(pointers_, pointer_focus_client_, serial, input_serial_kind::focus);
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
    note_serial(pointers_, pointer_focus_client_, serial, input_serial_kind::focus);
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
    if (event.pressed) {
        last_key_press_serial_ = serial;
        last_key_press_code_ = event.evdev_keycode;
        last_key_release_serial_ = 0;
        last_key_client_ = wl_resource_get_client(static_cast<wl_resource*>(keyboard_focus_));
    } else if (event.evdev_keycode == last_key_press_code_) {
        last_key_release_serial_ = serial;
    }
    note_serial(keyboards_, keyboard_focus_client_, serial,
                event.pressed ? input_serial_kind::key_press : input_serial_kind::key_release);
    for (unsigned i = 0; i < keyboards_.count; ++i) {
        if (!resource_set::same_client(keyboards_.items[i], keyboard_focus_))
            continue;
        wl_keyboard_send_key(static_cast<wl_resource*>(keyboards_.items[i]), serial,
                             event.time_ms, event.evdev_keycode,
                             event.pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                           : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    if (event.pressed && input_hook_ && keyboards_.has_client(keyboard_focus_client_))
        input_hook_(input_hook_user_, keyboard_focus_client_, serial,
                    input_serial_kind::key_press);
#else
    (void)event;
    (void)seat;
#endif
}

bool lx::compositor::input_router::is_key_grab_serial(unsigned serial, void* client) const {
    return serial != 0 && client != nullptr && client == last_key_client_ &&
           (serial == last_key_press_serial_ || serial == last_key_release_serial_);
}

void lx::compositor::input_router::send_modifiers(const lx::input::modifier_state& mods,
                                                  lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!keyboard_focus_)
        return;
    const unsigned serial = seat.next_serial();
    note_serial(keyboards_, keyboard_focus_client_, serial, input_serial_kind::modifiers);
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
    if (muted_count_ != 0 && motion_muted(pointer_focus_client_))
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

unsigned lx::compositor::input_router::send_pointer_button(
    const lx::input::pointer_button_event& event, lx::input::seat& seat) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!pointer_focus_)
        return 0;
    const unsigned serial = seat.next_serial();
    note_serial(pointers_, pointer_focus_client_, serial,
                event.pressed ? input_serial_kind::button_press
                              : input_serial_kind::button_release);
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
    if (event.pressed && input_hook_ && pointers_.has_client(pointer_focus_client_))
        input_hook_(input_hook_user_, pointer_focus_client_, serial,
                    input_serial_kind::button_press);
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
