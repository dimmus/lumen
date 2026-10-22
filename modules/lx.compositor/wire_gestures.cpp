module;

// pointer-gestures-unstable-v1, version 3 (design-app-compat-capture C.4, G1): touchpad
// swipe, pinch and hold from libinput, to the client with pointer focus.
//
// A gesture is latched at begin to the pointer focus's surface and client (wlroots and
// Mutter): its updates and its end go to the same client's objects even if the pointer
// focus moves meanwhile, so a client never sees an update without its begin or a begin
// without its end. A client without objects of that kind simply gets nothing; one that
// made several gets every event on each. Serials of begin and end come from the seat's
// counter. While the session is locked the pointer focus is a lock surface or nothing, so a
// gesture reaches no one else. The compositor's own gesture policy is consulted at begin
// (`gesture_consumed`, none consumed today): future workspace swipes take a gesture there,
// with no change on the wire. Hold objects exist from v3 (`get_hold_gesture`); `release`
// (v2) destroys the manager. No errors of its own. Visible to sandboxed clients.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_POINTER_GESTURES_UNSTABLE_V1)
#include "pointer-gestures-unstable-v1-server-protocol.h"
#define LUMEN_HAS_POINTER_GESTURES 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import lx.input;
import :input_router;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_POINTER_GESTURES)

namespace {

using gkind = lx::input::gesture_event::kind;
using gphase = lx::input::gesture_event::phase;

/// One gesture object (swipe, pinch or hold). Allocated per request (bounded by the client's
/// own objects), unlinked by its destructor.
struct gesture_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    gkind kind = gkind::swipe;
    gesture_obj* prev = nullptr;
    gesture_obj* next = nullptr;
};

gesture_obj* gestures = nullptr;

/// The gesture in progress: its kind, and the client it was latched to at begin (null when
/// none, or nobody had the pointer then — its updates and end then go nowhere).
struct latched_gesture {
    bool active = false;
    gkind kind = gkind::swipe;
    wl_client* client = nullptr;
} current;

void gesture_destroy_resource(wl_resource* r) {
    auto* o = static_cast<gesture_obj*>(wl_resource_get_user_data(r));
    if (!o)
        return;
    if (o->prev)
        o->prev->next = o->next;
    else
        gestures = o->next;
    if (o->next)
        o->next->prev = o->prev;
    delete o;
}

const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};
const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};
const struct zwp_pointer_gesture_hold_v1_interface hold_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void make_gesture(struct wl_client* client, struct wl_resource* manager, uint32_t id, gkind kind) {
    const wl_interface* iface = kind == gkind::swipe   ? &zwp_pointer_gesture_swipe_v1_interface
                                : kind == gkind::pinch ? &zwp_pointer_gesture_pinch_v1_interface
                                                       : &zwp_pointer_gesture_hold_v1_interface;
    auto* o = new (std::nothrow) gesture_obj{};
    wl_resource* res =
        o ? wl_resource_create(client, iface, wl_resource_get_version(manager), id) : nullptr;
    if (!res) {
        delete o;
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = res;
    o->client = client;
    o->kind = kind;
    o->next = gestures;
    if (gestures)
        gestures->prev = o;
    gestures = o;
    const void* impl = kind == gkind::swipe   ? static_cast<const void*>(&swipe_impl)
                       : kind == gkind::pinch ? static_cast<const void*>(&pinch_impl)
                                              : static_cast<const void*>(&hold_impl);
    wl_resource_set_implementation(res, impl, o, gesture_destroy_resource);
}

const struct zwp_pointer_gestures_v1_interface manager_impl = {
    .get_swipe_gesture = [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
                            struct wl_resource* /*pointer*/) {
        make_gesture(client, resource, id, gkind::swipe);
    },
    .get_pinch_gesture = [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
                            struct wl_resource* /*pointer*/) {
        make_gesture(client, resource, id, gkind::pinch);
    },
    .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_hold_gesture = [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
                           struct wl_resource* /*pointer*/) {
        make_gesture(client, resource, id, gkind::hold);
    },
};

/// The compositor's gesture policy: true takes the gesture for the compositor (nothing is
/// sent for it). No compositor gesture exists yet.
bool gesture_consumed(const lx::input::gesture_event&) { return false; }

void send_gesture(const lx::input::gesture_event& e) {
    p0_protocol_context* ctx = g_p0_ctx;
    if (!ctx || !ctx->input)
        return;
    if (e.stage == gphase::begin) {
        // A begin while one is in progress (a device that lost its end): the old one is over.
        current = {};
        if (gesture_consumed(e))
            return;
        auto* surface = static_cast<wl_resource*>(ctx->input->pointer_focus());
        auto* client = static_cast<wl_client*>(ctx->input->pointer_focus_client());
        if (!surface || !client)
            return;
        current.active = true;
        current.kind = e.type;
        current.client = client;
        const unsigned serial = ctx->seat ? ctx->seat->next_serial() : 0;
        for (gesture_obj* o = gestures; o; o = o->next) {
            if (o->client != client || o->kind != e.type)
                continue;
            switch (e.type) {
            case gkind::swipe:
                zwp_pointer_gesture_swipe_v1_send_begin(o->resource, serial, e.time_ms, surface,
                                                        e.fingers);
                break;
            case gkind::pinch:
                zwp_pointer_gesture_pinch_v1_send_begin(o->resource, serial, e.time_ms, surface,
                                                        e.fingers);
                break;
            case gkind::hold:
                zwp_pointer_gesture_hold_v1_send_begin(o->resource, serial, e.time_ms, surface,
                                                       e.fingers);
                break;
            }
        }
        return;
    }
    if (!current.active || current.kind != e.type)
        return;
    const unsigned serial = e.stage == gphase::end && ctx->seat ? ctx->seat->next_serial() : 0;
    for (gesture_obj* o = gestures; o; o = o->next) {
        if (o->client != current.client || o->kind != e.type)
            continue;
        if (e.stage == gphase::update) {
            if (e.type == gkind::swipe)
                zwp_pointer_gesture_swipe_v1_send_update(o->resource, e.time_ms,
                                                         wl_fixed_from_double(e.dx),
                                                         wl_fixed_from_double(e.dy));
            else if (e.type == gkind::pinch)
                zwp_pointer_gesture_pinch_v1_send_update(
                    o->resource, e.time_ms, wl_fixed_from_double(e.dx), wl_fixed_from_double(e.dy),
                    wl_fixed_from_double(e.scale), wl_fixed_from_double(e.rotation));
            continue;
        }
        const int32_t cancelled = e.cancelled ? 1 : 0;
        switch (e.type) {
        case gkind::swipe:
            zwp_pointer_gesture_swipe_v1_send_end(o->resource, serial, e.time_ms, cancelled);
            break;
        case gkind::pinch:
            zwp_pointer_gesture_pinch_v1_send_end(o->resource, serial, e.time_ms, cancelled);
            break;
        case gkind::hold:
            zwp_pointer_gesture_hold_v1_send_end(o->resource, serial, e.time_ms, cancelled);
            break;
        }
    }
    if (e.stage == gphase::end)
        current = {};
}

} // namespace

void bind_pointer_gestures(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_pointer_gestures_v1_interface, version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

void pointer_gestures_client_destroyed(wl_client* client) {
    if (current.client == client)
        current = {};
}

#else

namespace {
void send_gesture(const lx::input::gesture_event&) {}
} // namespace

void pointer_gestures_client_destroyed(wl_client*) {}

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::pointer_gesture(p0_protocol_context& ctx,
                                     const lx::input::gesture_event& event) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::send_gesture(event);
#else
    (void)event;
#endif
}
