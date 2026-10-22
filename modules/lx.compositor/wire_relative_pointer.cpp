module;

// relative-pointer-unstable-v1 (design-app-compat-capture C.2, RP1): the device's motion
// itself — accelerated and unaccelerated deltas, microsecond time — for games, 3D viewers
// and VM consoles, whether or not the pointer can move (a pointer lock keeps sending it;
// pointer-constraints says so).
//
// Delivered to the client with pointer focus, to every relative pointer it made, on every
// motion event (before the `wl_pointer.motion` it may come with, in the same frame: sway's
// order) — not while that client is muted for not answering pings, like its wl_pointer.motion.
// Unaccelerated deltas come from libinput (device units normalised to 1000 dpi);
// an absolute device (a tablet, the VMware console mouse) has no motion of its own, so its
// delta is the change of its position scaled into the layout — both deltas equal, as sway
// does. A warp (a lock's cursor hint, a confinement brought inside a new region) makes
// none. No errors of its own. Visible to sandboxed clients.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1)
#include "relative-pointer-unstable-v1-server-protocol.h"
#define LUMEN_HAS_RELATIVE_POINTER 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :input_router;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_RELATIVE_POINTER)

namespace {

/// One zwp_relative_pointer_v1. Allocated per request (the client's own object count bounds
/// them), unlinked by its destructor.
struct relative_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    relative_obj* prev = nullptr;
    relative_obj* next = nullptr;
};

relative_obj* relatives = nullptr;

void relative_destroy_resource(wl_resource* r) {
    auto* o = static_cast<relative_obj*>(wl_resource_get_user_data(r));
    if (!o)
        return;
    if (o->prev)
        o->prev->next = o->next;
    else
        relatives = o->next;
    if (o->next)
        o->next->prev = o->prev;
    delete o;
}

const struct zwp_relative_pointer_v1_interface relative_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void manager_get(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                 struct wl_resource* /*pointer*/) {
    auto* o = new (std::nothrow) relative_obj{};
    wl_resource* res = o ? wl_resource_create(client, &zwp_relative_pointer_v1_interface,
                                              wl_resource_get_version(resource), id)
                         : nullptr;
    if (!res) {
        delete o;
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = res;
    o->client = client;
    o->next = relatives;
    if (relatives)
        relatives->prev = o;
    relatives = o;
    wl_resource_set_implementation(res, &relative_impl, o, relative_destroy_resource);
}

const struct zwp_relative_pointer_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_relative_pointer = &manager_get,
};

} // namespace

void bind_relative_pointer_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_relative_pointer_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

namespace {

bool send_relative(unsigned long long time_us, double dx, double dy, double dxu, double dyu) {
    if (!relatives || !g_p0_ctx || !g_p0_ctx->input)
        return false;
    auto* focus = static_cast<wl_client*>(g_p0_ctx->input->pointer_focus_client());
    // A client that stopped answering pings is not fed motion it cannot read (the router
    // mutes its wl_pointer.motion the same way).
    if (!focus || g_p0_ctx->input->motion_muted(focus))
        return false;
    bool sent = false;
    const auto hi = static_cast<uint32_t>(time_us >> 32);
    const auto lo = static_cast<uint32_t>(time_us & 0xFFFFFFFFull);
    for (relative_obj* o = relatives; o; o = o->next) {
        if (o->client != focus)
            continue;
        zwp_relative_pointer_v1_send_relative_motion(o->resource, hi, lo, wl_fixed_from_double(dx),
                                                     wl_fixed_from_double(dy),
                                                     wl_fixed_from_double(dxu),
                                                     wl_fixed_from_double(dyu));
        sent = true;
    }
    return sent;
}

} // namespace

#else

namespace {
bool send_relative(unsigned long long, double, double, double, double) { return false; }
} // namespace

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

bool lx::compositor::relative_pointer_motion(p0_protocol_context& ctx, unsigned long long time_us,
                                             double dx, double dy, double dx_unaccel,
                                             double dy_unaccel) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::send_relative(time_us, dx, dy, dx_unaccel, dy_unaccel);
#else
    (void)time_us;
    (void)dx;
    (void)dy;
    (void)dx_unaccel;
    (void)dy_unaccel;
    return false;
#endif
}
