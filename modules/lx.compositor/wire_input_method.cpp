module;

// zwp_input_method_manager_v2 (input-method-unstable-v2.xml, v1) and
// zwp_virtual_keyboard_manager_v1 (virtual-keyboard-unstable-v1.xml, v1), both from wlroots'
// protocol/ directory: the input method, its popup surfaces and keyboard grab, and virtual
// keyboards. Part of the protocol wire split; shared declarations live in the :wire partition
// (wire.cppm), the relay's rules in :text_input (pure, tested on its own), the key routing in
// :input_router. Also installs the relay (its sink, the router's focus hook).
//
// Why these and not wayland-protocols' experimental xx-input-method: fcitx5 5.1
// (waylandimserverv2.cpp) and ibus 1.5 (ibuswaylandim.c) speak input-method-v2 and need
// virtual-keyboard-v1 to forward the keys they do not consume; nothing shipping speaks
// xx_input_method_v1 (design T1).
//
// Every error of the protocols:
//   zwp_input_method_v2.role                         get_input_popup_surface on a surface
//                                                    with another role (or a live popup)
//   zwp_virtual_keyboard_v1.no_keymap                key or modifiers before a keymap
//   zwp_virtual_keyboard_v1.invalid_keymap_format    keymap with a format that is neither
//                                                    no_keymap nor xkb_v1 (value 1: wlroots
//                                                    main; the 0.20.2 XML lacks the entry)
//   zwp_virtual_keyboard_manager_v1.unauthorized     never: every session client may create
//                                                    one (sway, niri; design X1)
// A second input method on the seat is sent `unavailable` as its only event and is inert; an
// xkb_v1 keymap that does not compile is refused (logged; the keyboard keeps the keymap it
// had, so keys before a usable one are still `no_keymap`).
//
// Keys: while the input method is active, physical keys go to its grab and nothing else sees
// them (the router; design T2); its virtual keyboard's keys go to the focused client like a
// physical keyboard's — never back into the grab, never through compositor bindings, never
// counted as user activity (I2). A virtual keyboard whose keymap is the seat's (fcitx5 and
// ibus forward the grab's keymap unchanged) types with no keymap switch; another keymap is
// sent to the focused client's keyboards first and the seat's is restored before the next
// physical key.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2)
#include "input-method-unstable-v2-server-protocol.h"
#define LUMEN_HAS_INPUT_METHOD 1
#endif
#if defined(LUMEN_PROTOCOL_VIRTUAL_KEYBOARD_UNSTABLE_V1)
#include "virtual-keyboard-unstable-v1-server-protocol.h"
#define LUMEN_HAS_VIRTUAL_KEYBOARD 1
#endif
#endif
#include <time.h>

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import lx.input;
import :surface;
import :surface_tree;
import :output;
import :input_router;
import :text_input;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

[[nodiscard]] bool ime_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void ime_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, "compositor.ime", line);
}

template <typename... Args>
void ime_log(lx::trace::level lvl, std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lvl, "compositor.ime", fmt, std::forward<Args>(args)...);
}

[[nodiscard]] lx::compositor::text_input_relay* relay() {
    return g_p0_ctx ? g_p0_ctx->text_input : nullptr;
}

/// The router and the seat, both or neither.
[[nodiscard]] bool have_router() {
    return g_p0_ctx && g_p0_ctx->input && g_p0_ctx->seat;
}

/// The output whose logical rect holds the point, else the first live one; empty when none.
[[nodiscard]] lx::rect2i output_bounds_at(int x, int y) {
    auto* om = g_p0_ctx ? g_p0_ctx->outputs : nullptr;
    if (!om)
        return {};
    lx::rect2i first{};
    for (unsigned slot = 0; slot < lx::compositor::output_manager::k_max; ++slot) {
        if (!om->slot_live(slot))
            continue;
        const lx::rect2i r = om->slot_state(slot).logical_rect();
        if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height)
            return r;
        if (first.width <= 0)
            first = r;
    }
    return first;
}

} // namespace

#if defined(LUMEN_HAS_INPUT_METHOD)

namespace {

struct grab_obj;

/// zwp_input_method_v2: the relay's record and the live keyboard grab, if any.
struct im_obj {
    lx::compositor::input_method_record record{};
    grab_obj* grab = nullptr;
};

/// zwp_input_method_keyboard_grab_v2. Inert once its input method is gone or replaced its
/// grab (`im` null).
struct grab_obj {
    wl_resource* resource = nullptr;
    im_obj* im = nullptr;
};

im_obj* im_from(wl_resource* r) {
    return r ? static_cast<im_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

grab_obj* grab_from(wl_resource* r) {
    return r ? static_cast<grab_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

input_popup_obj* popup_from(wl_resource* r) {
    return r ? static_cast<input_popup_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

// ── the keyboard grab ──

void grab_send_key(void* grab, unsigned serial, unsigned time_ms, unsigned key, bool pressed) {
    zwp_input_method_keyboard_grab_v2_send_key(static_cast<wl_resource*>(grab), serial, time_ms,
                                               key,
                                               pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                                       : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void grab_send_modifiers(void* grab, unsigned serial, const lx::input::modifier_state& m) {
    zwp_input_method_keyboard_grab_v2_send_modifiers(static_cast<wl_resource*>(grab), serial,
                                                     m.depressed, m.latched, m.locked, m.group);
}

/// The grab no longer receives keys (released, replaced, or its input method went).
void grab_detach(grab_obj* g) {
    if (!g || !g->im)
        return;
    im_obj* im = g->im;
    g->im = nullptr;
    if (im->grab == g) {
        im->grab = nullptr;
        if (have_router())
            g_p0_ctx->input->set_im_grab({}, *g_p0_ctx->seat);
    }
}

void grab_destroy_resource(wl_resource* r) {
    grab_obj* g = grab_from(r);
    if (!g)
        return;
    grab_detach(g);
    delete g;
}

const struct zwp_input_method_keyboard_grab_v2_interface grab_impl = {
    .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void im_grab_keyboard(struct wl_client* client, struct wl_resource* im_res, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_input_method_keyboard_grab_v2_interface,
                                          wl_resource_get_version(im_res), id);
    auto* g = res ? new (std::nothrow) grab_obj{} : nullptr;
    if (!g) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    g->resource = res;
    wl_resource_set_implementation(res, &grab_impl, g, &grab_destroy_resource);
    im_obj* im = im_from(im_res);
    if (!im || !im->record.live || !have_router())
        return; // inert: an unavailable input method grabs nothing
    // One grab per input method: a new one replaces the old (fcitx5 releases first anyway).
    if (im->grab) {
        if (ime_debug())
            ime_log(lx::trace::level::debug, "input method grab replaced");
        grab_detach(im->grab);
    }
    int fd = -1;
    unsigned size = 0;
    if (seat_keymap(fd, size))
        zwp_input_method_keyboard_grab_v2_send_keymap(res, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                                      size);
    // wl_keyboard's (wire_seat.cpp): one repeat rate for the seat.
    zwp_input_method_keyboard_grab_v2_send_repeat_info(res, 40, 400);
    g->im = im;
    im->grab = g;
    lx::compositor::im_grab_ops ops{};
    ops.grab = res;
    ops.key = &grab_send_key;
    ops.modifiers = &grab_send_modifiers;
    g_p0_ctx->input->set_im_grab(ops, *g_p0_ctx->seat);
    if (ime_debug()) {
        ime_log(lx::trace::level::debug, "input method grab keyboard routing={}",
                g_p0_ctx->input->im_routing_active() ? 1 : 0);
    }
}

// ── popups ──

void popup_destroy_resource(wl_resource* r) {
    input_popup_obj* o = popup_from(r);
    if (!o)
        return;
    if (auto* rl = relay())
        rl->remove_popup(o->record);
    if (o->surface && o->surface->input_popup == o)
        o->surface->input_popup = nullptr;
    delete o;
}

const struct zwp_input_popup_surface_v2_interface popup_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void im_get_input_popup_surface(struct wl_client* client, struct wl_resource* im_res, uint32_t id,
                                struct wl_resource* surface_res) {
    // Always created, to stay in sync with the client — an inert one when nothing can be
    // shown for it.
    wl_resource* res = wl_resource_create(client, &zwp_input_popup_surface_v2_interface,
                                          wl_resource_get_version(im_res), id);
    auto* o = res ? new (std::nothrow) input_popup_obj{} : nullptr;
    if (!o) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    o->record.resource = res;
    wl_resource_set_implementation(res, &popup_impl, o, &popup_destroy_resource);
    surface_obj* s = surface_from(surface_res);
    if (!s)
        return;
    if ((s->role != surface_role::none && s->role != surface_role::input_popup) || s->input_popup) {
        wl_resource_post_error(im_res, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                               "wl_surface@%u already has another role",
                               wl_resource_get_id(surface_res));
        return;
    }
    if (!g_p0_ctx || !g_p0_ctx->tree ||
        !g_p0_ctx->tree->make_root(s->id, lx::compositor::root_kind::input_popup)) {
        wl_client_post_no_memory(client);
        return;
    }
    s->role = surface_role::input_popup;
    s->input_popup = o;
    o->surface = s;
    o->record.surface = s->resource;
    im_obj* im = im_from(im_res);
    auto* rl = relay();
    if (!im || !rl)
        return;
    rl->add_popup(o->record, im->record);
    // Content committed before the role (kept, as for every role-less surface) shows at once.
    rl->popup_committed(o->record, s->has_content && !s->surface_size.empty(), s->surface_size);
}

// ── zwp_input_method_v2 ──

void im_destroy_resource(wl_resource* r) {
    im_obj* im = im_from(r);
    if (!im)
        return;
    if (im->grab)
        grab_detach(im->grab);
    const bool was_live = im->record.live;
    if (auto* rl = relay())
        rl->remove_input_method(im->record);
    if (was_live)
        ime_log(lx::trace::level::info, "input method gone");
    delete im;
}

const struct zwp_input_method_v2_interface im_impl = {
    .commit_string =
        [](struct wl_client*, struct wl_resource* r, const char* text) {
            if (auto* im = im_from(r); im && relay())
                relay()->im_commit_string(im->record, text ? text : "");
        },
    .set_preedit_string =
        [](struct wl_client*, struct wl_resource* r, const char* text, int32_t begin,
           int32_t end) {
            if (auto* im = im_from(r); im && relay())
                relay()->im_preedit(im->record, text ? text : "", begin, end);
        },
    .delete_surrounding_text =
        [](struct wl_client*, struct wl_resource* r, uint32_t before, uint32_t after) {
            if (auto* im = im_from(r); im && relay())
                relay()->im_delete_surrounding(im->record, before, after);
        },
    .commit =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            auto* im = im_from(r);
            if (!im || !relay())
                return;
            if (ime_debug() && im->record.live && serial != im->record.dones) {
                ime_log(lx::trace::level::debug,
                        "input method commit serial={} stale (dones={}): dropped", serial,
                        im->record.dones);
            }
            relay()->im_commit(im->record, serial);
        },
    .get_input_popup_surface = &im_get_input_popup_surface,
    .grab_keyboard = &im_grab_keyboard,
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void manager_get_input_method(struct wl_client* client, struct wl_resource* manager,
                              struct wl_resource*, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_input_method_v2_interface,
                                          wl_resource_get_version(manager), id);
    auto* im = res ? new (std::nothrow) im_obj{} : nullptr;
    if (!im) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    im->record.resource = res;
    im->record.client = client;
    wl_resource_set_implementation(res, &im_impl, im, &im_destroy_resource);
    auto* rl = relay();
    if (!rl || !rl->add_input_method(im->record)) {
        // "The compositor must issue this event as the only event on the object if there was
        // another input_method object associated with the same seat at the time of its
        // creation."
        zwp_input_method_v2_send_unavailable(res);
        ime_log(lx::trace::level::info, "input method unavailable: the seat has one already");
        return;
    }
    ime_log(lx::trace::level::info, "input method bound");
}

const struct zwp_input_method_manager_v2_interface manager_impl = {
    .get_input_method = &manager_get_input_method,
    // Input methods created from it live on.
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

// ── the relay's zwp_input_method_v2 and popup events ──

[[nodiscard]] wl_resource* live(void* r) {
    auto* res = static_cast<wl_resource*>(r);
    return res && !client_dying(wl_resource_get_client(res)) ? res : nullptr;
}

void ev_activate(void*, void* im) {
    if (auto* r = live(im)) {
        zwp_input_method_v2_send_activate(r);
        if (ime_debug())
            ime_log(lx::trace::level::debug, "input method activate");
    }
}

void ev_deactivate(void*, void* im) {
    if (auto* r = live(im)) {
        zwp_input_method_v2_send_deactivate(r);
        if (ime_debug())
            ime_log(lx::trace::level::debug, "input method deactivate");
    }
}

void ev_surrounding(void*, void* im, const char* text, unsigned cursor, unsigned anchor) {
    if (auto* r = live(im))
        zwp_input_method_v2_send_surrounding_text(r, text ? text : "", cursor, anchor);
}

void ev_cause(void*, void* im, unsigned cause) {
    if (auto* r = live(im))
        zwp_input_method_v2_send_text_change_cause(r, cause);
}

void ev_content_type(void*, void* im, unsigned hint, unsigned purpose) {
    if (auto* r = live(im))
        zwp_input_method_v2_send_content_type(r, hint, purpose);
}

void ev_im_done(void*, void* im) {
    if (auto* r = live(im))
        zwp_input_method_v2_send_done(r);
}

void ev_routing(void*, bool on) {
    if (have_router())
        g_p0_ctx->input->set_im_routing(on, *g_p0_ctx->seat);
}

bool ev_surface_rect(void*, void* surface, lx::rect2i& global, lx::rect2i& bounds) {
    surface_obj* s = surface_from(static_cast<wl_resource*>(surface));
    if (!s || !g_p0_ctx || !g_p0_ctx->tree)
        return false;
    const lx::rect2i r = g_p0_ctx->tree->node_rect(s->id);
    if (r.width <= 0 || r.height <= 0)
        return false;
    global = r;
    bounds = output_bounds_at(r.x, r.y);
    return bounds.width > 0 && bounds.height > 0;
}

void ev_popup_show(void*, void* popup, void* surface, lx::point2i origin) {
    surface_obj* s = surface_from(static_cast<wl_resource*>(surface));
    auto* tree = g_p0_ctx ? g_p0_ctx->tree : nullptr;
    if (!s || !tree || !g_p0_ctx->ime_popup_layer)
        return;
    tree->set_root_origin(s->id, origin);
    if (!tree->root_mapped(s->id)) {
        if (!tree->map_root(s->id, *g_p0_ctx->ime_popup_layer, ~0u)) {
            ime_log(lx::trace::level::warn, "input popup map failed: layer full");
            return;
        }
    } else {
        tree->sync_scene(s->id, "ime-popup");
    }
    if (ime_debug()) {
        ime_log(lx::trace::level::debug, "ime popup show surface={} popup={} x={} y={} {}x{}",
                s->id.id(), wl_resource_get_id(static_cast<wl_resource*>(popup)), origin.x,
                origin.y, s->surface_size.width, s->surface_size.height);
    }
}

void ev_popup_hide(void*, void*, void* surface) {
    surface_obj* s = surface_from(static_cast<wl_resource*>(surface));
    auto* tree = g_p0_ctx ? g_p0_ctx->tree : nullptr;
    if (!s || !tree || !tree->root_mapped(s->id))
        return;
    tree->unmap_root(s->id);
    if (ime_debug()) {
        ime_log(lx::trace::level::debug, "ime popup hide surface={}", s->id.id());
    }
}

void ev_popup_rect(void*, void* popup, lx::rect2i rect) {
    if (auto* r = live(popup)) {
        zwp_input_popup_surface_v2_send_text_input_rectangle(r, rect.x, rect.y, rect.width,
                                                             rect.height);
        if (ime_debug()) {
            ime_log(lx::trace::level::debug, "ime popup text_input_rectangle {},{} {}x{}", rect.x,
                    rect.y, rect.width, rect.height);
        }
    }
}

} // namespace

void bind_input_method_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_input_method_manager_v2_interface, version,
                                         id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

#endif // LUMEN_HAS_INPUT_METHOD

#if defined(LUMEN_HAS_VIRTUAL_KEYBOARD)

namespace {

/// The wire value of zwp_virtual_keyboard_v1.error.invalid_keymap_format (wlroots main; the
/// tagged XML Lumen generates from predates the entry).
#if defined(ZWP_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP_FORMAT)
constexpr uint32_t k_vk_invalid_keymap_format = ZWP_VIRTUAL_KEYBOARD_V1_ERROR_INVALID_KEYMAP_FORMAT;
#else
constexpr uint32_t k_vk_invalid_keymap_format = 1;
#endif

/// Keymap ids handed to virtual keyboards; 0 is the seat's.
unsigned g_next_keymap_id = 0;

/// zwp_virtual_keyboard_v1. Allocated per request.
struct vk_obj {
    wl_resource* resource = nullptr;
    bool has_keymap = false;
    /// Its own keymap (normalized and sealed), unless it is the seat's (`keymap.id` 0).
    lx::unique_fd fd{};
    lx::compositor::keymap_ref keymap{};
    /// Keys it holds down: released to the focus when it goes.
    lx::compositor::forwarded_keys held{};
};

vk_obj* vk_from(wl_resource* r) {
    return r ? static_cast<vk_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

/// Whether the sealed keymap in `fd` (`size` bytes) is the seat's, byte for byte.
[[nodiscard]] bool same_as_seat(int fd, unsigned size) {
    int seat_fd = -1;
    unsigned seat_size = 0;
    if (!seat_keymap(seat_fd, seat_size) || seat_size != size || size == 0)
        return false;
    void* a = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (a == MAP_FAILED)
        return false;
    void* b = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, seat_fd, 0);
    bool same = false;
    if (b != MAP_FAILED) {
        same = std::memcmp(a, b, size) == 0;
        ::munmap(b, size);
    }
    ::munmap(a, size);
    return same;
}

/// The keymap stops being shown to anyone (replaced, or the keyboard goes).
void drop_keymap(vk_obj* vk) {
    if (vk->keymap.id != 0 && have_router())
        g_p0_ctx->input->virtual_keymap_gone(vk->keymap.id, *g_p0_ctx->seat);
    vk->keymap = {};
    vk->fd = lx::unique_fd{};
}

void vk_keymap(struct wl_client*, struct wl_resource* r, uint32_t format, int32_t fd,
               uint32_t size) {
    lx::unique_fd owned{fd}; // closed however this ends
    vk_obj* vk = vk_from(r);
    if (!vk)
        return;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP &&
        format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        wl_resource_post_error(r, k_vk_invalid_keymap_format,
                               "keymap format %u is not a wl_keyboard.keymap_format", format);
        return;
    }
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
        // Raw keycodes: clients read them with the seat's keymap.
        drop_keymap(vk);
        vk->has_keymap = true;
        return;
    }
    // Compiled per request (UI thread; a keymap is sent once per keyboard, never per key).
    // Read, not mapped: a client's file shorter than `size` (or truncated meanwhile) would
    // raise SIGBUS in the compositor through a mapping.
    unsigned out_size = 0;
    auto normalized = lx::input::normalize_keymap_fd(owned.get(), size, out_size);
    if (!normalized) {
        // Warned once a run (a client cannot flood the log), debug after that.
        static bool warned = false;
        if (!warned || ime_debug())
            ime_log(warned ? lx::trace::level::debug : lx::trace::level::warn,
                    "virtual keyboard keymap refused: it cannot be read or does not compile");
        warned = true;
        return;
    }
    lx::unique_fd fresh = std::move(normalized).value();
    drop_keymap(vk);
    vk->has_keymap = true;
    if (same_as_seat(fresh.get(), out_size))
        return; // the seat's: keys go out with no keymap switch
    vk->fd = std::move(fresh);
    if (++g_next_keymap_id == 0)
        ++g_next_keymap_id;
    vk->keymap.id = g_next_keymap_id;
    vk->keymap.fd = vk->fd.get();
    vk->keymap.size = out_size;
    if (ime_debug()) {
        ime_log(lx::trace::level::debug, "virtual keyboard keymap {} bytes differs from the seat's",
                out_size);
    }
}

void vk_key(struct wl_client*, struct wl_resource* r, uint32_t time, uint32_t key, uint32_t state) {
    vk_obj* vk = vk_from(r);
    if (!vk)
        return;
    if (!vk->has_keymap) {
        wl_resource_post_error(r, ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                               "key before a keymap was set");
        return;
    }
    if (!have_router() || key >= lx::compositor::forwarded_keys::k_codes)
        return;
    lx::input::key_event ev{};
    ev.evdev_keycode = key;
    ev.pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    ev.time_ms = time;
    if (ev.pressed)
        vk->held.set(key);
    else
        vk->held.clear(key);
    g_p0_ctx->input->send_virtual_key(ev, vk->keymap, *g_p0_ctx->seat);
}

void vk_modifiers(struct wl_client*, struct wl_resource* r, uint32_t depressed, uint32_t latched,
                  uint32_t locked, uint32_t group) {
    vk_obj* vk = vk_from(r);
    if (!vk)
        return;
    if (!vk->has_keymap) {
        wl_resource_post_error(r, ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                               "modifiers before a keymap was set");
        return;
    }
    if (!have_router())
        return;
    lx::input::modifier_state m{};
    m.depressed = depressed;
    m.latched = latched;
    m.locked = locked;
    m.group = group;
    g_p0_ctx->input->send_virtual_modifiers(m, vk->keymap, *g_p0_ctx->seat);
}

void vk_destroy_resource(wl_resource* r) {
    vk_obj* vk = vk_from(r);
    if (!vk)
        return;
    // Keys it still holds are released to the focus, as an unplugged keyboard's are.
    if (have_router() && vk->held.any()) {
        unsigned codes[16];
        unsigned n = 0;
        vk->held.for_each([&](unsigned code) {
            if (n < 16)
                codes[n++] = code;
        });
        for (unsigned i = 0; i < n; ++i) {
            lx::input::key_event ev{};
            ev.evdev_keycode = codes[i];
            ev.pressed = false;
            g_p0_ctx->input->send_virtual_key(ev, vk->keymap, *g_p0_ctx->seat);
        }
    }
    drop_keymap(vk);
    delete vk;
}

const struct zwp_virtual_keyboard_v1_interface vk_impl = {
    .keymap = &vk_keymap,
    .key = &vk_key,
    .modifiers = &vk_modifiers,
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void vkm_create(struct wl_client* client, struct wl_resource* manager, struct wl_resource*,
                uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_virtual_keyboard_v1_interface,
                                          wl_resource_get_version(manager), id);
    auto* vk = res ? new (std::nothrow) vk_obj{} : nullptr;
    if (!vk) {
        if (res)
            wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    vk->resource = res;
    wl_resource_set_implementation(res, &vk_impl, vk, &vk_destroy_resource);
}

const struct zwp_virtual_keyboard_manager_v1_interface vkm_impl = {
    .create_virtual_keyboard = &vkm_create,
};

} // namespace

void bind_virtual_keyboard_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_virtual_keyboard_manager_v1_interface,
                                         version, id);
    if (!res)
        return;
    res.set_implementation(&vkm_impl, g_p0_ctx, nullptr);
}

#endif // LUMEN_HAS_VIRTUAL_KEYBOARD

// ── installation and the wl_surface hooks ────────────────────────────────────

void text_input_focus_hook(void*, void* surface, void* client, void* destroyed) {
    auto* rl = relay();
    if (!rl)
        return;
    if (destroyed)
        rl->focus_destroyed(destroyed);
    else
        rl->keyboard_focus(surface, client);
}

void install_text_input(p0_protocol_context& ctx) {
    if (!ctx.text_input)
        return;
    lx::compositor::text_input_sink sink{};
    sink.user = &ctx;
    text_input_sink_events(sink);
#if defined(LUMEN_HAS_INPUT_METHOD)
    sink.im_activate = &ev_activate;
    sink.im_deactivate = &ev_deactivate;
    sink.im_surrounding = &ev_surrounding;
    sink.im_change_cause = &ev_cause;
    sink.im_content_type = &ev_content_type;
    sink.im_done = &ev_im_done;
    sink.routing = &ev_routing;
    sink.surface_rect = &ev_surface_rect;
    sink.popup_show = &ev_popup_show;
    sink.popup_hide = &ev_popup_hide;
    sink.popup_rect = &ev_popup_rect;
#endif
    ctx.text_input->set_sink(sink);
    // The router's focus-surface hook is wire_install.cpp's, which calls
    // `text_input_focus_hook` (and keyboard-shortcuts-inhibit's).
    if (ctx.input)
        ctx.input->set_seat_keymap_source(&seat_keymap);
}

void input_popup_committed(surface_obj* s) {
    input_popup_obj* o = s->input_popup;
    auto* rl = relay();
    if (!o || !rl)
        return;
    rl->popup_committed(o->record, s->has_content && !s->surface_size.empty(), s->surface_size);
}

bool input_popup_drawable(const surface_obj* s) {
    // While its object lives and belongs to the seat's input method: content is kept for the
    // moment it is shown.
    return s->input_popup && s->input_popup->record.im != nullptr;
}

void input_popup_surface_destroyed(surface_obj* s) {
    input_popup_obj* o = s->input_popup;
    s->input_popup = nullptr;
    if (!o)
        return;
    if (auto* rl = relay())
        rl->remove_popup(o->record);
    o->surface = nullptr;
    o->record.surface = nullptr;
}

void text_input_surface_applied(surface_obj* s) {
    if (auto* rl = relay(); rl && rl->text_input_count() != 0)
        rl->surface_committed(s->resource);
}

void text_input_surface_moved(lx::surface_id surface) {
    auto* rl = relay();
    if (!rl || !rl->active() || rl->popup_count() == 0)
        return;
    if (surface_obj* s = surface_by_id(surface))
        rl->surface_moved(s->resource);
}

void text_input_outputs_changed() {
    if (auto* rl = relay())
        rl->outputs_changed();
}

bool input_popup_hit_impl(double x, double y, void*& surface_res, lx::surface_id& surface,
                          double& sx, double& sy) {
    auto* rl = relay();
    auto* tree = g_p0_ctx ? g_p0_ctx->tree : nullptr;
    if (!rl || !tree || !g_p0_ctx->surfaces || rl->popup_count() == 0)
        return false;
    bool found = false;
    rl->for_each_popup([&](const lx::compositor::input_popup_record& p) {
        if (found || !p.shown || !p.surface)
            return;
        surface_obj* s = surface_from(static_cast<wl_resource*>(p.surface));
        if (!s || !tree->root_mapped(s->id))
            return;
        lx::compositor::tree_hit th{};
        if (!tree->hit(s->id, x, y, th))
            return;
        surface_res = g_p0_ctx->surfaces->resource(th.surface);
        if (!surface_res)
            return;
        surface = th.surface;
        sx = th.sx;
        sy = th.sy;
        found = true;
    });
    return found;
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

bool lx::compositor::input_popup_hit(p0_protocol_context& ctx, double x, double y,
                                     void*& surface_res, lx::surface_id& surface, double& sx,
                                     double& sy) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::input_popup_hit_impl(x, y, surface_res, surface, sx, sy);
#else
    (void)x;
    (void)y;
    (void)surface_res;
    (void)surface;
    (void)sx;
    (void)sy;
    return false;
#endif
}
