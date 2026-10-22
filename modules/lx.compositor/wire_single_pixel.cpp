module;

// single-pixel-buffer-v1 and alpha-modifier-v1 (staging), design-app-compat-capture §A.7,
// §A.8.
//   - A single-pixel buffer is a `buffer_obj` holding one premultiplied colour (the u32
//     channels scaled to 0..1). Applied, it is a solid draw of the surface's logical size
//     (a viewport destination sizes it): no texture, no upload, released at apply. Opaque iff
//     alpha is UINT32_MAX, so an opaque one occludes what lies under it. GTK and Firefox use
//     these for backgrounds and letterboxing.
//   - The alpha multiplier is double-buffered and sticky; applied, it becomes the surface
//     node's opacity, which every backend multiplies in its blending space. A multiplier
//     below one makes the draw blend and stop occluding. Destroying the object is
//     set_multiplier(UINT32_MAX).
//
// Every error:
//   wp_alpha_modifier_v1.already_constructed       get_surface on a surface that has one
//   wp_alpha_modifier_surface_v1.no_surface        set_multiplier after the wl_surface
// (single-pixel-buffer-v1 defines none; explicit sync on such a buffer is
// wp_linux_drm_syncobj_surface_v1.unsupported_buffer, wire_syncobj.cpp.)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1)
#include "single-pixel-buffer-v1-server-protocol.h"
#define LUMEN_HAS_SINGLE_PIXEL 1
#endif
#if defined(LUMEN_PROTOCOL_ALPHA_MODIFIER_V1)
#include "alpha-modifier-v1-server-protocol.h"
#define LUMEN_HAS_ALPHA_MODIFIER 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import :surface;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

#if defined(LUMEN_HAS_SINGLE_PIXEL)

[[nodiscard]] float channel(uint32_t v) {
    return static_cast<float>(static_cast<double>(v) / 4294967295.0);
}

const struct wp_single_pixel_buffer_manager_v1_interface single_pixel_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .create_u32_rgba_buffer =
        [](struct wl_client* client, struct wl_resource*, uint32_t id, uint32_t r, uint32_t g,
           uint32_t b, uint32_t a) {
            buffer_obj* buf = alloc_buffer();
            if (!buf) {
                wl_client_post_no_memory(client);
                return;
            }
            // wl_buffer v1, as every compositor-created buffer here.
            buf->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
            if (!buf->resource) {
                *buf = {};
                wl_client_post_no_memory(client);
                return;
            }
            buf->is_single_pixel = true;
            buf->id = next_buffer_id();
            buf->width = 1;
            buf->height = 1;
            // "The values are premultiplied": stored as they are, like a texel.
            buf->solid = lx::color::rgb(channel(r), channel(g), channel(b), channel(a));
            wl_resource_set_implementation(buf->resource, &buffer_impl, buf,
                                           buffer_destroy_resource);
        },
};

#endif

#if defined(LUMEN_HAS_ALPHA_MODIFIER)

void alpha_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    if (a && a->surface)
        a->surface->pending.alpha = 0xFFFFFFFFu; // "equivalent to set_multiplier(UINT32_MAX)"
    addon_free(a);
}

const struct wp_alpha_modifier_surface_v1_interface alpha_surface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_multiplier =
        [](struct wl_client*, struct wl_resource* r, uint32_t factor) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE,
                                       "set_multiplier after the wl_surface was destroyed");
                return;
            }
            a->surface->pending.alpha = factor;
        },
};

const struct wp_alpha_modifier_v1_interface alpha_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_surface =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->alpha_ctl) {
                wl_resource_post_error(r, WP_ALPHA_MODIFIER_V1_ERROR_ALREADY_CONSTRUCTED,
                                       "the surface already has a wp_alpha_modifier_surface_v1");
                return;
            }
            (void)addon_create(client, &wp_alpha_modifier_surface_v1_interface,
                               wl_resource_get_version(r), id, s, &s->alpha_ctl,
                               &alpha_surface_impl, alpha_destroy_resource);
        },
};

#endif

} // namespace

#if defined(LUMEN_HAS_SINGLE_PIXEL)
void bind_single_pixel_buffer_manager(wayland::client_connection& client, unsigned id,
                                      int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_single_pixel_buffer_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&single_pixel_impl, g_p0_ctx, nullptr);
}
#endif

#if defined(LUMEN_HAS_ALPHA_MODIFIER)
void bind_alpha_modifier(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_alpha_modifier_v1_interface, version, id);
    if (res)
        res.set_implementation(&alpha_manager_impl, g_p0_ctx, nullptr);
}
#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
