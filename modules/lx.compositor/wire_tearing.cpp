module;

// tearing-control-v1 and content-type-v1 (staging): two per-surface hints, double-buffered
// and sticky like the buffer scale (design-app-compat-capture §A.5, §A.6).
//   - The presentation hint (vsync / async) reaches the output policy (`allow_async`): an
//     output flips asynchronously only when the card can (DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP) and
//     its topmost content is a fullscreen window whose surface asks for async. Destroying the
//     object reverts to vsync at the next commit; it goes inert with its wl_surface.
//   - The content type feeds `surface_manager::content_hint` (scanout priority) and the
//     connector's `content type` property of the output a window is fullscreen on, where the
//     connector has one.
//
// Every error:
//   wp_tearing_control_manager_v1.tearing_control_exists  a second object for a surface
//   wp_content_type_manager_v1.already_constructed        a second object for a surface
// (Neither add-on object defines errors of its own; an unknown enum value is a protocol
// error of libwayland's enum check — the value is ignored here.)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_TEARING_CONTROL_V1)
#include "tearing-control-v1-server-protocol.h"
#define LUMEN_HAS_TEARING_CONTROL 1
#endif
#if defined(LUMEN_PROTOCOL_CONTENT_TYPE_V1)
#include "content-type-v1-server-protocol.h"
#define LUMEN_HAS_CONTENT_TYPE 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import :surface;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

#if defined(LUMEN_HAS_TEARING_CONTROL)

/// "Destroy this surface tearing object and revert the presentation hint to vsync. The
/// change will be applied on the next wl_surface.commit."
void tearing_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    if (a && a->surface)
        a->surface->pending.tearing = k_tearing_vsync;
    addon_free(a);
}

const struct wp_tearing_control_v1_interface tearing_impl = {
    .set_presentation_hint =
        [](struct wl_client*, struct wl_resource* r, uint32_t hint) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface)
                return; // inert: "should be destroyed"
            if (hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC ||
                hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)
                a->surface->pending.tearing = static_cast<unsigned char>(hint);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct wp_tearing_control_manager_v1_interface tearing_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_tearing_control =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->tearing_ctl) {
                wl_resource_post_error(
                    r, WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS,
                    "the surface already has a wp_tearing_control_v1");
                return;
            }
            (void)addon_create(client, &wp_tearing_control_v1_interface,
                               wl_resource_get_version(r), id, s, &s->tearing_ctl, &tearing_impl,
                               tearing_destroy_resource);
        },
};

#endif

#if defined(LUMEN_HAS_CONTENT_TYPE)

/// Destroying the object resets the type to none at the next commit (wlroots, Mutter).
void content_type_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    if (a && a->surface)
        a->surface->pending.content_type = 0;
    addon_free(a);
}

const struct wp_content_type_v1_interface content_type_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_content_type =
        [](struct wl_client*, struct wl_resource* r, uint32_t type) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface)
                return;
            if (type <= WP_CONTENT_TYPE_V1_TYPE_GAME)
                a->surface->pending.content_type = static_cast<unsigned char>(type);
        },
};

const struct wp_content_type_manager_v1_interface content_type_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_surface_content_type =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->content_type_ctl) {
                wl_resource_post_error(r, WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED,
                                       "the surface already has a wp_content_type_v1");
                return;
            }
            (void)addon_create(client, &wp_content_type_v1_interface, wl_resource_get_version(r),
                               id, s, &s->content_type_ctl, &content_type_impl,
                               content_type_destroy_resource);
        },
};

#endif

} // namespace

#if defined(LUMEN_HAS_TEARING_CONTROL)
void bind_tearing_control_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wp_tearing_control_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&tearing_manager_impl, g_p0_ctx, nullptr);
}
#endif

#if defined(LUMEN_HAS_CONTENT_TYPE)
void bind_content_type_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wp_content_type_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&content_type_manager_impl, g_p0_ctx, nullptr);
}
#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

bool lx::compositor::surface_wants_async(p0_protocol_context& ctx, lx::surface_id surface) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    const p0::surface_obj* s = p0::surface_by_id(surface);
    return s && s->tearing_current == p0::k_tearing_async;
#else
    (void)surface;
    return false;
#endif
}

unsigned lx::compositor::surface_content_type(p0_protocol_context& ctx, lx::surface_id surface) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    const p0::surface_obj* s = p0::surface_by_id(surface);
    return s ? s->content_type_current : 0u;
#else
    (void)surface;
    return 0;
#endif
}
