module;

// wp_fractional_scale_manager_v1 (fractional-scale-v1.xml, staging): tells a surface the
// scale it should render at, in 120ths. Part of the protocol wire split; shared
// declarations live in the :wire partition (wire.cppm).
//
// The value is the largest scale among the outputs the surface is on (wlroots' scene; a
// surface that spans a 1x and a 1.5x output renders for the sharper one), sent on change
// only. Membership changes drive it (wire_output.cpp `update_membership` calls
// `fractional_scale_update`), so it follows a window across outputs, the pointer's output
// for a cursor surface, and an output scale change.
//
// The first value goes out when the object is created (D7), before the surface's first
// configure: a client that renders its first frame at 1 and then again at 1.25 shows a
// flash of wrong-size content and wastes a frame (GTK does exactly that without it).
//
// Errors: wp_fractional_scale_manager_v1.fractional_scale_exists. wp_fractional_scale_v1
// defines none. Objects are allocated per request, freed with their resource.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "fractional-scale-v1-server-protocol.h"
#define LUMEN_HAS_FRACTIONAL_SCALE 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :wire;

namespace {

constexpr lx::trace::category log_scale{"compositor.scale"};

} // namespace

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_FRACTIONAL_SCALE)

namespace {

fractional_obj* fractional_from(wl_resource* r) {
    auto* f = r ? static_cast<fractional_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return f && f->used && f->resource == r ? f : nullptr;
}

void fractional_destroy_resource(wl_resource* r) {
    fractional_obj* f = fractional_from(r);
    if (!f)
        return;
    if (f->surface && f->surface->fractional == f) {
        f->surface->fractional = nullptr;
        // A new object starts from "never sent" again.
        f->surface->pref_scale120 = 0;
    }
    delete f;
}

void send_preferred(surface_obj* s, std::uint32_t scale120) {
    s->pref_scale120 = scale120;
    wp_fractional_scale_v1_send_preferred_scale(s->fractional->resource, scale120);
    if (log_scale.enabled(lx::trace::level::debug)) {
        log_scale.debug("fractional preferred surface={} scale120={}", s->id.id(), scale120);
    }
}

const struct wp_fractional_scale_v1_interface fractional_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void manager_get_fractional_scale(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t id, struct wl_resource* surface_res) {
    surface_obj* s = surface_from(surface_res);
    if (!s)
        return;
    if (s->fractional) {
        wl_resource_post_error(resource,
                               WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                               "the surface already has a fractional scale object");
        return;
    }
    auto* f = new (std::nothrow) fractional_obj{};
    wl_resource* res = f ? wl_resource_create(client, &wp_fractional_scale_v1_interface,
                                              wl_resource_get_version(resource), id)
                         : nullptr;
    if (!res) {
        delete f;
        wl_client_post_no_memory(client);
        return;
    }
    f->used = true;
    f->resource = res;
    f->surface = s;
    s->fractional = f;
    s->pref_scale120 = 0;
    wl_resource_set_implementation(res, &fractional_impl, f, fractional_destroy_resource);
    send_preferred(s, expected_scale_120(s));
}

const struct wp_fractional_scale_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_fractional_scale = manager_get_fractional_scale,
};

} // namespace

void bind_fractional_scale(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wp_fractional_scale_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

void fractional_scale_update(surface_obj* s, std::uint32_t scale120) {
    if (!s || !s->fractional || !s->fractional->resource || scale120 == 0 ||
        scale120 == s->pref_scale120)
        return;
    send_preferred(s, scale120);
}

#else

void fractional_scale_update(surface_obj*, std::uint32_t) {}

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
