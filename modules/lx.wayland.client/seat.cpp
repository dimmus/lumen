module;

// Bodies of lx.wayland.client:seat — wl_seat devices, xkbcommon state, cursor shapes,
// zwp_text_input_v3. Event handlers copy into fixed members and call the app's handler; the
// keymap (compiled by xkbcommon when it changes) is the only allocation.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

namespace lx::wayland {

namespace {

constexpr lx::trace::category log_seat{"wayland.client.seat"};

/// Copies `src` into `dst` (cap bytes with the NUL), cut at a UTF-8 character boundary.
/// True when it had to cut.
bool copy_utf8(char* dst, std::size_t cap, const char* src) {
    if (!src)
        src = "";
    std::size_t n = std::strlen(src);
    bool cut = false;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u)
            --n; // never end inside a sequence
        cut = true;
    }
    std::memcpy(dst, src, n);
    dst[n] = '\0';
    return cut;
}

} // namespace

#if LX_WLC_REAL

namespace {

surface* surface_of(void* wl) { return surface::from_native(wl); }

const wl_seat_listener k_seat_listener = {
    .capabilities = [](void* data, wl_seat*,
                       std::uint32_t caps) { static_cast<seat*>(data)->on_capabilities(caps); },
    .name = [](void* data, wl_seat*, const char* name) { static_cast<seat*>(data)->on_name(name); },
};

const wl_pointer_listener k_pointer_listener = {
    .enter =
        [](void* data, wl_pointer*, std::uint32_t serial, wl_surface* s, wl_fixed_t x,
           wl_fixed_t y) {
            static_cast<seat*>(data)->on_pointer_enter(serial, s, wl_fixed_to_double(x),
                                                       wl_fixed_to_double(y));
        },
    .leave = [](void* data, wl_pointer*, std::uint32_t serial,
                wl_surface* s) { static_cast<seat*>(data)->on_pointer_leave(serial, s); },
    .motion =
        [](void* data, wl_pointer*, std::uint32_t time, wl_fixed_t x, wl_fixed_t y) {
            static_cast<seat*>(data)->on_pointer_motion(time, wl_fixed_to_double(x),
                                                        wl_fixed_to_double(y));
        },
    .button =
        [](void* data, wl_pointer*, std::uint32_t serial, std::uint32_t time,
           std::uint32_t button, std::uint32_t state) {
            static_cast<seat*>(data)->on_pointer_button(serial, time, button, state);
        },
    .axis =
        [](void* data, wl_pointer*, std::uint32_t time, std::uint32_t axis, wl_fixed_t value) {
            static_cast<seat*>(data)->on_pointer_axis(time, axis, wl_fixed_to_double(value));
        },
    .frame = [](void* data, wl_pointer*) { static_cast<seat*>(data)->on_pointer_frame(); },
    .axis_source = [](void* data, wl_pointer*,
                      std::uint32_t source) { static_cast<seat*>(data)->on_pointer_axis_source(source); },
    .axis_stop = [](void* data, wl_pointer*, std::uint32_t time,
                    std::uint32_t axis) { static_cast<seat*>(data)->on_pointer_axis_stop(time, axis); },
    // v5–7 notches; v8 replaced them with axis_value120. Folded into value120 either way,
    // so the app sees one representation.
    .axis_discrete =
        [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t discrete) {
            static_cast<seat*>(data)->on_pointer_axis_value120(axis, discrete * 120);
        },
    .axis_value120 =
        [](void* data, wl_pointer*, std::uint32_t axis, std::int32_t value120) {
            static_cast<seat*>(data)->on_pointer_axis_value120(axis, value120);
        },
    .axis_relative_direction =
        [](void* data, wl_pointer*, std::uint32_t axis, std::uint32_t direction) {
            static_cast<seat*>(data)->on_pointer_axis_relative_direction(axis, direction);
        },
};

const wl_keyboard_listener k_keyboard_listener = {
    .keymap =
        [](void* data, wl_keyboard*, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
            static_cast<seat*>(data)->on_keymap(format, fd, size);
        },
    .enter =
        [](void* data, wl_keyboard*, std::uint32_t serial, wl_surface* s, wl_array* keys) {
            static_cast<seat*>(data)->on_keyboard_enter(
                serial, s, static_cast<const std::uint32_t*>(keys->data),
                keys->size / sizeof(std::uint32_t));
        },
    .leave = [](void* data, wl_keyboard*, std::uint32_t serial,
                wl_surface* s) { static_cast<seat*>(data)->on_keyboard_leave(serial, s); },
    .key =
        [](void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t time, std::uint32_t key,
           std::uint32_t state) { static_cast<seat*>(data)->on_key(serial, time, key, state); },
    .modifiers =
        [](void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t dep, std::uint32_t lat,
           std::uint32_t lock, std::uint32_t group) {
            static_cast<seat*>(data)->on_modifiers(serial, dep, lat, lock, group);
        },
    .repeat_info = [](void* data, wl_keyboard*, std::int32_t rate,
                      std::int32_t delay) { static_cast<seat*>(data)->on_repeat_info(rate, delay); },
};

const wl_touch_listener k_touch_listener = {
    .down =
        [](void* data, wl_touch*, std::uint32_t serial, std::uint32_t time, wl_surface* s,
           std::int32_t id, wl_fixed_t x, wl_fixed_t y) {
            touch_event e{};
            e.type = touch_event::kind::down;
            e.serial = serial;
            e.time_ms = time;
            e.target = surface_of(s);
            e.id = id;
            e.x = wl_fixed_to_double(x);
            e.y = wl_fixed_to_double(y);
            static_cast<seat*>(data)->on_touch(e);
        },
    .up =
        [](void* data, wl_touch*, std::uint32_t serial, std::uint32_t time, std::int32_t id) {
            touch_event e{};
            e.type = touch_event::kind::up;
            e.serial = serial;
            e.time_ms = time;
            e.id = id;
            static_cast<seat*>(data)->on_touch(e);
        },
    .motion =
        [](void* data, wl_touch*, std::uint32_t time, std::int32_t id, wl_fixed_t x,
           wl_fixed_t y) {
            touch_event e{};
            e.type = touch_event::kind::motion;
            e.time_ms = time;
            e.id = id;
            e.x = wl_fixed_to_double(x);
            e.y = wl_fixed_to_double(y);
            static_cast<seat*>(data)->on_touch(e);
        },
    .frame =
        [](void* data, wl_touch*) {
            touch_event e{};
            e.type = touch_event::kind::frame;
            static_cast<seat*>(data)->on_touch(e);
        },
    .cancel =
        [](void* data, wl_touch*) {
            touch_event e{};
            e.type = touch_event::kind::cancel;
            static_cast<seat*>(data)->on_touch(e);
        },
    .shape =
        [](void* data, wl_touch*, std::int32_t id, wl_fixed_t major, wl_fixed_t minor) {
            touch_event e{};
            e.type = touch_event::kind::shape;
            e.id = id;
            e.major = wl_fixed_to_double(major);
            e.minor = wl_fixed_to_double(minor);
            static_cast<seat*>(data)->on_touch(e);
        },
    .orientation =
        [](void* data, wl_touch*, std::int32_t id, wl_fixed_t orientation) {
            touch_event e{};
            e.type = touch_event::kind::orientation;
            e.id = id;
            e.orientation = wl_fixed_to_double(orientation);
            static_cast<seat*>(data)->on_touch(e);
        },
};

const zwp_text_input_v3_listener k_text_input_listener = {
    .enter = [](void* data, zwp_text_input_v3*,
                wl_surface* s) { static_cast<text_input*>(data)->on_enter(s); },
    .leave = [](void* data, zwp_text_input_v3*,
                wl_surface* s) { static_cast<text_input*>(data)->on_leave(s); },
    .preedit_string =
        [](void* data, zwp_text_input_v3*, const char* text, std::int32_t begin, std::int32_t end) {
            static_cast<text_input*>(data)->on_preedit(text, begin, end);
        },
    .commit_string = [](void* data, zwp_text_input_v3*,
                        const char* text) { static_cast<text_input*>(data)->on_commit_string(text); },
    .delete_surrounding_text =
        [](void* data, zwp_text_input_v3*, std::uint32_t before, std::uint32_t after) {
            static_cast<text_input*>(data)->on_delete_surrounding(before, after);
        },
    .done = [](void* data, zwp_text_input_v3*,
               std::uint32_t serial) { static_cast<text_input*>(data)->on_done(serial); },
    .action =
        [](void* data, zwp_text_input_v3*, std::uint32_t action, std::uint32_t serial) {
            static_cast<text_input*>(data)->on_action(action, serial);
        },
    .language = [](void*, zwp_text_input_v3*, const char*) {},
    .preedit_hint = [](void*, zwp_text_input_v3*, std::uint32_t, std::uint32_t, std::uint32_t) {},
};

} // namespace

// ── seat ──

seat::~seat() { release(); }

void seat::bind(display* d, wl_seat* proxy, std::uint32_t global_name, std::uint32_t version) {
    display_ = d;
    proxy_ = proxy;
    global_name_ = global_name;
    version_ = version;
    caps_ = 0;
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy), &lx_wlc_tag);
    wl_seat_add_listener(proxy, &k_seat_listener, this);
}

void seat::release() {
    caps_ = 0;
    update_devices();
    free_keymap();
#if defined(LUMEN_HAS_XKBCOMMON)
    if (xkb_)
        xkb_context_unref(xkb_);
#endif
    xkb_ = nullptr;
    if (proxy_) {
        if (version_ >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(proxy_);
        else
            wl_seat_destroy(proxy_);
    }
    proxy_ = nullptr;
    pointer_focus_ = nullptr;
    keyboard_focus_ = nullptr;
    frame_ = {};
}

void seat::on_capabilities(std::uint32_t caps) {
    std::uint32_t mine = 0;
    if (caps & WL_SEAT_CAPABILITY_POINTER)
        mine |= seat_pointer;
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD)
        mine |= seat_keyboard;
    if (caps & WL_SEAT_CAPABILITY_TOUCH)
        mine |= seat_touch;
    caps_ = mine;
    update_devices();
    if (handler_)
        handler_->on_capabilities(*this, caps_);
}

void seat::update_devices() {
    const bool release_requests = version_ >= 3;
    if ((caps_ & seat_pointer) && !pointer_ && proxy_) {
        pointer_ = wl_seat_get_pointer(proxy_);
        if (pointer_) {
            wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(pointer_), &lx_wlc_tag);
            wl_pointer_add_listener(pointer_, &k_pointer_listener, this);
        }
    } else if (!(caps_ & seat_pointer) && pointer_) {
        // The device goes without a leave: what was pending is delivered, then the leave the
        // compositor will never send (hover must not stick).
        flush_pointer_frame();
        if (pointer_focus_) {
            pointer_frame f{};
            f.mask = pointer_frame::has_leave;
            f.target = pointer_focus_;
            pointer_focus_ = nullptr;
            if (handler_)
                handler_->on_pointer(*this, f);
        }
        if (cursor_shape_)
            wp_cursor_shape_device_v1_destroy(cursor_shape_);
        cursor_shape_ = nullptr;
        if (release_requests)
            wl_pointer_release(pointer_);
        else
            wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
        pointer_focus_ = nullptr;
        frame_ = {};
    }
    if ((caps_ & seat_keyboard) && !keyboard_ && proxy_) {
        keyboard_ = wl_seat_get_keyboard(proxy_);
        if (keyboard_) {
            wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(keyboard_), &lx_wlc_tag);
            wl_keyboard_add_listener(keyboard_, &k_keyboard_listener, this);
        }
    } else if (!(caps_ & seat_keyboard) && keyboard_) {
        // No wl_keyboard.leave or key release will come: the focus loss is synthesised, so a
        // held key's repeat stops.
        if (surface* had = keyboard_focus_) {
            keyboard_focus_ = nullptr;
            if (handler_)
                handler_->on_keyboard_focus(*this, had, 0, false);
        }
        if (release_requests)
            wl_keyboard_release(keyboard_);
        else
            wl_keyboard_destroy(keyboard_);
        keyboard_ = nullptr;
        keyboard_focus_ = nullptr;
    }
    if ((caps_ & seat_touch) && !touch_ && proxy_) {
        touch_ = wl_seat_get_touch(proxy_);
        if (touch_) {
            wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(touch_), &lx_wlc_tag);
            wl_touch_add_listener(touch_, &k_touch_listener, this);
        }
    } else if (!(caps_ & seat_touch) && touch_) {
        // Points still down end as the protocol's cancel.
        if (touch_points_ > 0 && handler_) {
            touch_event e{};
            e.type = touch_event::kind::cancel;
            handler_->on_touch(*this, e);
        }
        touch_points_ = 0;
        if (release_requests)
            wl_touch_release(touch_);
        else
            wl_touch_destroy(touch_);
        touch_ = nullptr;
    }
}

void seat::on_name(const char* name) {
    std::size_t n = name ? std::strlen(name) : 0;
    n = std::min(n, sizeof(name_) - 1);
    if (n)
        std::memcpy(name_, name, n);
    name_[n] = '\0';
}

bool seat::set_cursor_shape(cursor_shape shape) {
    if (!pointer_ || !display_)
        return false;
    if (!cursor_shape_) {
        auto* manager = static_cast<wp_cursor_shape_manager_v1*>(
            display_->native_global(global_kind::cursor_shape));
        if (!manager)
            return false;
        cursor_shape_ = wp_cursor_shape_manager_v1_get_pointer(manager, pointer_);
        if (!cursor_shape_)
            return false;
    }
    std::uint32_t value = static_cast<std::uint32_t>(shape);
    if (value > WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT &&
        display_->version(global_kind::cursor_shape) < 2)
        value = static_cast<std::uint32_t>(cursor_shape::default_); // v2 shapes on a v1 server
    wp_cursor_shape_device_v1_set_shape(cursor_shape_, enter_serial_, value);
    return true;
}

// Pointer: everything up to wl_pointer.frame is one `pointer_frame`.

void seat::flush_pointer_frame() {
    if (frame_.mask == 0)
        return;
    const pointer_frame f = frame_;
    const bool left = (f.mask & pointer_frame::has_leave) && !(f.mask & pointer_frame::has_enter);
    frame_ = {};
    frame_.target = left ? nullptr : f.target;
    frame_.x = f.x;
    frame_.y = f.y;
    if (handler_)
        handler_->on_pointer(*this, f);
}

void seat::on_pointer_enter(std::uint32_t serial, void* wl_surface, double x, double y) {
    // A leave and an enter of the same surface in one frame are one transition; a leave of
    // another surface (moving between two of the client's) is delivered as its own frame
    // first, so the left surface hears it; a second enter is a new frame.
    surface* entered = surface_of(wl_surface);
    if ((frame_.mask & pointer_frame::has_enter) ||
        ((frame_.mask & pointer_frame::has_leave) && frame_.target != entered))
        flush_pointer_frame();
    enter_serial_ = serial;
    pointer_focus_ = entered;
    frame_.mask |= pointer_frame::has_enter;
    frame_.target = pointer_focus_;
    frame_.serial = serial;
    frame_.x = x;
    frame_.y = y;
    if (version_ < WL_POINTER_FRAME_SINCE_VERSION)
        flush_pointer_frame();
}

void seat::on_pointer_leave(std::uint32_t serial, void* wl_surface) {
    if (frame_.mask & (pointer_frame::has_motion | pointer_frame::has_button | pointer_frame::has_axis))
        flush_pointer_frame(); // what happened over the old surface is delivered first
    frame_.mask |= pointer_frame::has_leave;
    frame_.target = surface_of(wl_surface);
    frame_.serial = serial;
    pointer_focus_ = nullptr;
    if (version_ < WL_POINTER_FRAME_SINCE_VERSION)
        flush_pointer_frame();
}

void seat::on_pointer_motion(std::uint32_t time, double x, double y) {
    frame_.mask |= pointer_frame::has_motion;
    frame_.target = pointer_focus_;
    frame_.time_ms = time;
    frame_.x = x;
    frame_.y = y;
    if (version_ < WL_POINTER_FRAME_SINCE_VERSION)
        flush_pointer_frame();
}

void seat::on_pointer_button(std::uint32_t serial, std::uint32_t time, std::uint32_t button,
                             std::uint32_t state) {
    if (frame_.mask & pointer_frame::has_button)
        flush_pointer_frame(); // two buttons in one frame: two inputs
    input_serial_ = serial;
    frame_.mask |= pointer_frame::has_button;
    frame_.target = pointer_focus_;
    frame_.serial = serial;
    frame_.time_ms = time;
    frame_.button = button;
    frame_.pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (version_ < WL_POINTER_FRAME_SINCE_VERSION)
        flush_pointer_frame();
}

void seat::on_pointer_axis(std::uint32_t time, std::uint32_t axis, double value) {
    const unsigned i = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1u : 0u;
    frame_.mask |= pointer_frame::has_axis;
    frame_.target = pointer_focus_;
    frame_.time_ms = time;
    frame_.axis_value[i] += value;
    if (version_ < WL_POINTER_FRAME_SINCE_VERSION)
        flush_pointer_frame();
}

void seat::on_pointer_frame() { flush_pointer_frame(); }

void seat::on_pointer_axis_source(std::uint32_t source) {
    frame_.mask |= pointer_frame::has_axis_source;
    frame_.source = source;
}

void seat::on_pointer_axis_stop(std::uint32_t time, std::uint32_t axis) {
    const unsigned i = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1u : 0u;
    frame_.mask |= pointer_frame::has_axis_stop;
    frame_.time_ms = time;
    frame_.stop[i] = true;
}

void seat::on_pointer_axis_value120(std::uint32_t axis, std::int32_t value120) {
    const unsigned i = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1u : 0u;
    frame_.mask |= pointer_frame::has_axis_value120;
    frame_.value120[i] += value120;
}

void seat::on_pointer_axis_relative_direction(std::uint32_t axis, std::uint32_t direction) {
    const unsigned i = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 1u : 0u;
    frame_.mask |= pointer_frame::has_axis_relative_direction;
    frame_.direction[i] = direction;
}

void seat::forget_surface(const surface* s) {
    if (!s)
        return;
    if (pointer_focus_ == s)
        pointer_focus_ = nullptr;
    if (keyboard_focus_ == s)
        keyboard_focus_ = nullptr;
    // A frame still being folded for it: its target is unknown from now on (routers drop it).
    if (frame_.target == s)
        frame_.target = nullptr;
}

// Keyboard.

void seat::free_keymap() {
#if defined(LUMEN_HAS_XKBCOMMON)
    if (state_)
        xkb_state_unref(state_);
    if (keymap_)
        xkb_keymap_unref(keymap_);
#endif
    state_ = nullptr;
    keymap_ = nullptr;
    modifiers_ = 0;
    for (auto& m : mod_index_)
        m = ~0u;
}

void seat::on_keymap(std::uint32_t format, int fd, std::uint32_t size) {
#if defined(LUMEN_HAS_XKBCOMMON)
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        ::close(fd);
        free_keymap();
        return;
    }
    // MAP_PRIVATE: from wl_seat v7 the compositor may share one read-only fd.
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        log_seat.warn("keymap of {} B could not be mapped: {}", size, std::strerror(errno));
        return;
    }
    if (!xkb_)
        xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap =
        xkb_ ? xkb_keymap_new_from_buffer(xkb_, static_cast<const char*>(map),
                                          ::strnlen(static_cast<const char*>(map), size),
                                          XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
             : nullptr;
    ::munmap(map, size);
    xkb_state* state = keymap ? xkb_state_new(keymap) : nullptr;
    if (!state) {
        if (keymap)
            xkb_keymap_unref(keymap);
        log_seat.warn("the seat's keymap did not compile; keys arrive without keysyms");
        return;
    }
    free_keymap();
    keymap_ = keymap;
    state_ = state;
    const char* names[6] = {XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL,
                            XKB_MOD_NAME_ALT,   XKB_MOD_NAME_NUM,  XKB_MOD_NAME_LOGO};
    for (unsigned i = 0; i < 6; ++i)
        mod_index_[i] = xkb_keymap_mod_get_index(keymap_, names[i]);
    if (handler_)
        handler_->on_keymap(*this);
#else
    (void)format;
    (void)size;
    ::close(fd);
#endif
}

std::uint32_t seat::effective_modifiers() const {
    std::uint32_t m = 0;
#if defined(LUMEN_HAS_XKBCOMMON)
    if (!state_)
        return 0;
    for (unsigned i = 0; i < 6; ++i)
        if (mod_index_[i] != XKB_MOD_INVALID &&
            xkb_state_mod_index_is_active(state_, mod_index_[i], XKB_STATE_MODS_EFFECTIVE) > 0)
            m |= 1u << i;
#endif
    return m;
}

void seat::on_keyboard_enter(std::uint32_t serial, void* wl_surface, const std::uint32_t* keys,
                             std::size_t count) {
    (void)keys;
    (void)count;
    keyboard_focus_ = surface_of(wl_surface);
    input_serial_ = serial;
    if (handler_)
        handler_->on_keyboard_focus(*this, keyboard_focus_, serial, true);
}

void seat::on_keyboard_leave(std::uint32_t serial, void* wl_surface) {
    surface* s = surface_of(wl_surface);
    keyboard_focus_ = nullptr;
    if (handler_)
        handler_->on_keyboard_focus(*this, s, serial, false);
}

void seat::on_key(std::uint32_t serial, std::uint32_t time, std::uint32_t key, std::uint32_t state) {
    input_serial_ = serial;
    key_event e{};
    e.serial = serial;
    e.time_ms = time;
    e.key = key;
    e.state = state == WL_KEYBOARD_KEY_STATE_PRESSED ? key_state::pressed
#if defined(WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION)
              : state == WL_KEYBOARD_KEY_STATE_REPEATED ? key_state::repeated
#endif
                                                        : key_state::released;
#if defined(LUMEN_HAS_XKBCOMMON)
    if (state_) {
        const xkb_keycode_t code = key + 8;
        e.keysym = xkb_state_key_get_one_sym(state_, code);
        if (e.state != key_state::released) {
            const int n = xkb_state_key_get_utf8(state_, code, e.utf8, sizeof(e.utf8));
            e.utf8_len = n > 0 ? static_cast<std::uint8_t>(std::min<int>(n, sizeof(e.utf8) - 1)) : 0;
        }
        e.repeats = xkb_keymap_key_repeats(keymap_, code) != 0;
    }
#endif
    e.modifiers = modifiers_;
    if (handler_)
        handler_->on_key(*this, e);
}

void seat::on_modifiers(std::uint32_t serial, std::uint32_t depressed, std::uint32_t latched,
                        std::uint32_t locked, std::uint32_t group) {
    (void)serial;
#if defined(LUMEN_HAS_XKBCOMMON)
    if (state_)
        (void)xkb_state_update_mask(state_, depressed, latched, locked, 0, 0, group);
#else
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
#endif
    modifiers_ = effective_modifiers();
    if (handler_)
        handler_->on_modifiers(*this, modifiers_);
}

void seat::on_repeat_info(std::int32_t rate, std::int32_t delay) {
    repeat_rate_ = rate;
    repeat_delay_ = delay;
    if (handler_)
        handler_->on_repeat_info(*this, rate, delay);
}

void seat::on_touch(const touch_event& e) {
    if (e.type == touch_event::kind::down) {
        input_serial_ = e.serial;
        ++touch_points_;
    } else if (e.type == touch_event::kind::up && touch_points_ > 0) {
        --touch_points_;
    } else if (e.type == touch_event::kind::cancel) {
        touch_points_ = 0;
    }
    if (handler_)
        handler_->on_touch(*this, e);
}

// ── text_input ──

text_input::~text_input() { destroy(); }

lx::result<void> text_input::create(display& d, seat& s) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::text_input::create: already created");
    auto* manager =
        static_cast<zwp_text_input_manager_v3*>(d.native_global(global_kind::text_input));
    if (!manager || !s.bound())
        return lx::make_error(lx::wayland_err::missing_global,
                              "zwp_text_input_manager_v3 is not available");
    proxy_ = zwp_text_input_manager_v3_get_text_input(manager, static_cast<wl_seat*>(s.native()));
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "get_text_input failed");
    display_ = &d;
    d.link_text_input(this);
    version_ = zwp_text_input_v3_get_version(proxy_);
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    zwp_text_input_v3_add_listener(proxy_, &k_text_input_listener, this);
    commits_ = 0;
    return {};
}

void text_input::destroy() {
    if (display_ && linked_)
        display_->unlink_text_input(this);
    display_ = nullptr;
    if (proxy_)
        zwp_text_input_v3_destroy(proxy_);
    proxy_ = nullptr;
    focus_ = nullptr;
    enabled_ = pending_enabled_ = false;
}

void text_input::enable() {
    if (!proxy_)
        return;
    zwp_text_input_v3_enable(proxy_);
    pending_enabled_ = true;
}

void text_input::disable() {
    if (!proxy_)
        return;
    zwp_text_input_v3_disable(proxy_);
    pending_enabled_ = false;
}

void text_input::set_surrounding_text(std::string_view text, std::int32_t cursor,
                                      std::int32_t anchor) {
    if (!proxy_)
        return;
    // The protocol bounds the text at 4000 bytes; the wire needs a NUL-ended copy.
    char buf[4001];
    std::size_t n = std::min<std::size_t>(text.size(), 4000);
    while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u)
        --n;
    if (n < text.size())
        ++truncated_;
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
    const auto clamp = [n](std::int32_t v) {
        return std::clamp<std::int32_t>(v, 0, static_cast<std::int32_t>(n));
    };
    zwp_text_input_v3_set_surrounding_text(proxy_, buf, clamp(cursor), clamp(anchor));
}

void text_input::set_text_change_cause(std::uint32_t cause) {
    if (proxy_)
        zwp_text_input_v3_set_text_change_cause(proxy_, cause);
}

void text_input::set_content_type(std::uint32_t hint, std::uint32_t purpose) {
    if (proxy_)
        zwp_text_input_v3_set_content_type(proxy_, hint, purpose);
}

void text_input::set_cursor_rectangle(rect2i r) {
    if (proxy_)
        zwp_text_input_v3_set_cursor_rectangle(proxy_, r.x, r.y, r.width, r.height);
}

void text_input::set_available_actions(std::span<const std::uint32_t> actions) {
    if (!proxy_ || version_ < ZWP_TEXT_INPUT_V3_SET_AVAILABLE_ACTIONS_SINCE_VERSION)
        return;
    // A wl_array over the caller's storage (never grown: wl_array_add would allocate).
    wl_array a{};
    a.data = const_cast<std::uint32_t*>(actions.data());
    a.size = actions.size_bytes();
    a.alloc = actions.size_bytes();
    zwp_text_input_v3_set_available_actions(proxy_, &a);
}

void text_input::show_input_panel() {
    if (proxy_ && version_ >= ZWP_TEXT_INPUT_V3_SHOW_INPUT_PANEL_SINCE_VERSION)
        zwp_text_input_v3_show_input_panel(proxy_);
}

void text_input::hide_input_panel() {
    if (proxy_ && version_ >= ZWP_TEXT_INPUT_V3_HIDE_INPUT_PANEL_SINCE_VERSION)
        zwp_text_input_v3_hide_input_panel(proxy_);
}

void text_input::commit() {
    if (!proxy_)
        return;
    zwp_text_input_v3_commit(proxy_);
    ++commits_;
    enabled_ = pending_enabled_;
}

void text_input::forget_surface(const surface* s) {
    if (s && focus_ == s)
        focus_ = nullptr;
}

void text_input::on_enter(void* wl_surface) {
    focus_ = surface::from_native(wl_surface);
    if (handler_)
        handler_->on_focus(*this, focus_, true);
}

void text_input::on_leave(void* wl_surface) {
    surface* s = surface::from_native(wl_surface);
    focus_ = nullptr;
    // Leaving disables: the protocol resets the state (enable again on the next enter).
    enabled_ = pending_enabled_ = false;
    if (handler_)
        handler_->on_focus(*this, s, false);
}

void text_input::on_preedit(const char* text, std::int32_t begin, std::int32_t end) {
    has_preedit_ = true;
    if (copy_utf8(preedit_, sizeof(preedit_), text))
        ++truncated_;
    preedit_begin_ = begin;
    preedit_end_ = end;
}

void text_input::on_commit_string(const char* text) {
    has_commit_ = true;
    if (copy_utf8(commit_, sizeof(commit_), text))
        ++truncated_;
}

void text_input::on_delete_surrounding(std::uint32_t before, std::uint32_t after) {
    has_delete_ = true;
    delete_before_ = before;
    delete_after_ = after;
}

void text_input::on_done(std::uint32_t serial) {
    text_input_done d{};
    d.serial = serial;
    d.current = serial == commits_;
    // A done without a preedit_string clears the preedit (the protocol's initial state).
    d.has_preedit = has_preedit_;
    d.preedit = has_preedit_ ? preedit_ : "";
    d.preedit_cursor_begin = has_preedit_ ? preedit_begin_ : -1;
    d.preedit_cursor_end = has_preedit_ ? preedit_end_ : -1;
    d.has_commit = has_commit_;
    d.commit = has_commit_ ? commit_ : "";
    d.has_delete = has_delete_;
    d.delete_before = has_delete_ ? delete_before_ : 0;
    d.delete_after = has_delete_ ? delete_after_ : 0;
    if (handler_)
        handler_->on_done(*this, d);
    has_preedit_ = has_commit_ = has_delete_ = false;
    preedit_[0] = commit_[0] = '\0';
}

void text_input::on_action(std::uint32_t action, std::uint32_t serial) {
    if (handler_)
        handler_->on_action(*this, action, serial);
}

#else // !LX_WLC_REAL

seat::~seat() = default;
void seat::bind(display*, wl_seat*, std::uint32_t, std::uint32_t) {}
void seat::release() {}
void seat::on_capabilities(std::uint32_t) {}
void seat::update_devices() {}
void seat::on_name(const char*) {}
bool seat::set_cursor_shape(cursor_shape) { return false; }
void seat::flush_pointer_frame() {}
void seat::on_pointer_enter(std::uint32_t, void*, double, double) {}
void seat::on_pointer_leave(std::uint32_t, void*) {}
void seat::on_pointer_motion(std::uint32_t, double, double) {}
void seat::on_pointer_button(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void seat::on_pointer_axis(std::uint32_t, std::uint32_t, double) {}
void seat::on_pointer_frame() {}
void seat::on_pointer_axis_source(std::uint32_t) {}
void seat::on_pointer_axis_stop(std::uint32_t, std::uint32_t) {}
void seat::on_pointer_axis_value120(std::uint32_t, std::int32_t) {}
void seat::on_pointer_axis_relative_direction(std::uint32_t, std::uint32_t) {}
void seat::free_keymap() {}
void seat::on_keymap(std::uint32_t, int fd, std::uint32_t) {
    if (fd >= 0)
        ::close(fd);
}
std::uint32_t seat::effective_modifiers() const { return 0; }
void seat::on_keyboard_enter(std::uint32_t, void*, const std::uint32_t*, std::size_t) {}
void seat::on_keyboard_leave(std::uint32_t, void*) {}
void seat::on_key(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void seat::on_modifiers(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t) {}
void seat::on_repeat_info(std::int32_t, std::int32_t) {}
void seat::on_touch(const touch_event&) {}
void seat::forget_surface(const surface*) {}

text_input::~text_input() = default;
lx::result<void> text_input::create(display&, seat&) {
    return lx::not_implemented("lx::wayland::text_input::create");
}
void text_input::destroy() {}
void text_input::enable() {}
void text_input::disable() {}
void text_input::set_surrounding_text(std::string_view, std::int32_t, std::int32_t) {}
void text_input::set_text_change_cause(std::uint32_t) {}
void text_input::set_content_type(std::uint32_t, std::uint32_t) {}
void text_input::set_cursor_rectangle(rect2i) {}
void text_input::set_available_actions(std::span<const std::uint32_t>) {}
void text_input::show_input_panel() {}
void text_input::hide_input_panel() {}
void text_input::commit() {}
void text_input::forget_surface(const surface*) {}
void text_input::on_enter(void*) {}
void text_input::on_leave(void*) {}
void text_input::on_preedit(const char* text, std::int32_t, std::int32_t) {
    (void)copy_utf8(preedit_, sizeof(preedit_), text);
}
void text_input::on_commit_string(const char*) {}
void text_input::on_delete_surrounding(std::uint32_t, std::uint32_t) {}
void text_input::on_done(std::uint32_t) {}
void text_input::on_action(std::uint32_t, std::uint32_t) {}

#endif // LX_WLC_REAL

} // namespace lx::wayland
