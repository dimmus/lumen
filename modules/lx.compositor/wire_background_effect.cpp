module;

// ext-background-effect-v1 (staging): blur regions.
//   - The manager announces what the composite backend can do (`capabilities`, at bind): blur
//     on GL ES 3 and Vulkan, nothing on the CPU path, where a region is accepted and ignored.
//   - set_blur_region copies the wl_region (null: none); it is double-buffered, applied with
//     the wl_surface.commit, clipped to the surface and made disjoint there
//     (wire_surface.cpp `blur_rects`), and becomes the surface node's blur draws.
//     Destroying the object removes the region at the next commit.
//
// Every error:
//   ext_background_effect_manager_v1.background_effect_exists   a second object for a surface
//   ext_background_effect_surface_v1.surface_destroyed          set_blur_region after the
//                                                               wl_surface is gone

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_BACKGROUND_EFFECT_V1)
#include "ext-background-effect-v1-server-protocol.h"
#define LUMEN_HAS_BACKGROUND_EFFECT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import :surface;
import :wire;

#if defined(LUMEN_HAS_BACKGROUND_EFFECT)

namespace lx::compositor::p0 {

namespace {

void effect_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    if (a && a->surface) {
        // "The effect regions will be removed on the next commit."
        a->surface->pending.blur = {};
        a->surface->pending.blur.reset_empty();
        a->surface->pending.blur_set = true;
    }
    addon_free(a);
}

const struct ext_background_effect_surface_v1_interface effect_surface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_blur_region =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* region) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                                       "set_blur_region after the wl_surface was destroyed");
                return;
            }
            surface_obj* s = a->surface;
            // Copy semantics: the wl_region may be destroyed right after.
            if (const auto* reg =
                    region ? static_cast<region_obj*>(wl_resource_get_user_data(region)) : nullptr) {
                s->pending.blur = reg->region;
            } else {
                s->pending.blur = {};
                s->pending.blur.reset_empty();
            }
            s->pending.blur_set = true;
        },
};

const struct ext_background_effect_manager_v1_interface effect_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_background_effect =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->background_effect) {
                wl_resource_post_error(
                    r, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                    "the surface already has an ext_background_effect_surface_v1");
                return;
            }
            (void)addon_create(client, &ext_background_effect_surface_v1_interface,
                               wl_resource_get_version(r), id, s, &s->background_effect,
                               &effect_surface_impl, effect_destroy_resource);
        },
};

} // namespace

void bind_background_effect_manager(wayland::client_connection& client, unsigned id,
                                    int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &ext_background_effect_manager_v1_interface,
                                         version, id);
    if (!res)
        return;
    res.set_implementation(&effect_manager_impl, g_p0_ctx, nullptr);
    // The backend is chosen before clients connect and never changes: sent once, at bind.
    ext_background_effect_manager_v1_send_capabilities(
        static_cast<wl_resource*>(res.native()),
        g_p0_ctx->background_blur ? EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR : 0u);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_BACKGROUND_EFFECT
