module;

// wp_viewporter (viewporter.xml, stable): a surface's source crop and destination size,
// double-buffered with the rest of wl_surface state (they live in `surface_pending`, so a
// synchronized subsurface caches them like everything else). Part of the protocol wire
// split; shared declarations live in the :wire partition (wire.cppm), the geometry itself
// in :viewport.
//
// Every error of the protocol:
//   wp_viewporter.viewport_exists  get_viewport on a surface that already has one
//   wp_viewport.bad_value          set_source / set_destination out of range
//   wp_viewport.bad_size           at commit: no destination and a non-integer source size
//   wp_viewport.out_of_buffer      at commit: the source reaches past the buffer
//   wp_viewport.no_surface         set_source / set_destination after the wl_surface died
// The commit-time two are checked on the state the commit produces (`viewport_precheck`),
// before any state moves — wlroots does the same.
//
// Viewport objects are allocated per request and freed with their resource: an inert one
// (its surface gone) belongs to the client that keeps it, and must not occupy a slot of a
// table every client shares.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_VIEWPORTER)
#include "viewporter-server-protocol.h"
#define LUMEN_HAS_VIEWPORTER 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :viewport;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_VIEWPORTER)

namespace {

viewport_obj* viewport_from(wl_resource* r) {
    auto* v = r ? static_cast<viewport_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return v && v->used && v->resource == r ? v : nullptr;
}

/// "wp_viewport.destroy: the associated wl_surface's crop and scale state is removed. The
/// change is applied on the next wl_surface.commit." The destructor does it, so a client
/// that disconnects with a viewport leaves nothing behind either.
void viewport_destroy_resource(wl_resource* r) {
    viewport_obj* v = viewport_from(r);
    if (!v)
        return;
    if (surface_obj* s = v->surface) {
        s->pending.viewport = {};
        if (s->viewport == v)
            s->viewport = nullptr;
    }
    delete v;
}

void viewport_set_source(struct wl_client*, struct wl_resource* resource, wl_fixed_t x,
                         wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
    viewport_obj* v = viewport_from(resource);
    if (!v || !v->surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_source on a viewport whose wl_surface is gone");
        return;
    }
    // The sentinel is compared exactly on the raw fixed values, never as doubles.
    const wl_fixed_t unset = wl_fixed_from_int(-1);
    viewport_state& st = v->surface->pending.viewport;
    if (x == unset && y == unset && width == unset && height == unset) {
        st.has_src = false;
        st.src_x = st.src_y = st.src_w = st.src_h = 0;
        return;
    }
    if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "source x/y negative or width/height not positive");
        return;
    }
    st.has_src = true;
    st.src_x = x;
    st.src_y = y;
    st.src_w = width;
    st.src_h = height;
}

void viewport_set_destination(struct wl_client*, struct wl_resource* resource, int32_t width,
                              int32_t height) {
    viewport_obj* v = viewport_from(resource);
    if (!v || !v->surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "set_destination on a viewport whose wl_surface is gone");
        return;
    }
    viewport_state& st = v->surface->pending.viewport;
    if (width == -1 && height == -1) {
        st.has_dst = false;
        st.dst_w = st.dst_h = 0;
        return;
    }
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "destination width/height not positive");
        return;
    }
    st.has_dst = true;
    st.dst_w = width;
    st.dst_h = height;
}

const struct wp_viewport_interface viewport_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

void viewporter_get_viewport(struct wl_client* client, struct wl_resource* resource,
                             uint32_t id, struct wl_resource* surface_res) {
    surface_obj* s = surface_from(surface_res);
    if (!s)
        return;
    if (s->viewport) {
        wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "the surface already has a viewport");
        return;
    }
    auto* v = new (std::nothrow) viewport_obj{};
    wl_resource* res =
        v ? wl_resource_create(client, &wp_viewport_interface, wl_resource_get_version(resource),
                               id)
          : nullptr;
    if (!res) {
        delete v;
        wl_client_post_no_memory(client);
        return;
    }
    v->used = true;
    v->resource = res;
    v->surface = s;
    s->viewport = v;
    wl_resource_set_implementation(res, &viewport_impl, v, viewport_destroy_resource);
}

const struct wp_viewporter_interface viewporter_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_viewport = viewporter_get_viewport,
};

} // namespace

void bind_viewporter(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_viewporter_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&viewporter_impl, g_p0_ctx, nullptr);
}

bool viewport_precheck(surface_obj* s) {
    if (!s->viewport)
        return true; // no viewport, nothing pending in it (its destructor unset it)
    switch (lx::compositor::check_viewport(pending_geometry(s))) {
    case lx::compositor::viewport_error::none:
        return true;
    case lx::compositor::viewport_error::bad_size:
        wl_resource_post_error(s->viewport->resource, WP_VIEWPORT_ERROR_BAD_SIZE,
                               "source size is not an integer and no destination is set");
        return false;
    case lx::compositor::viewport_error::out_of_buffer:
        wl_resource_post_error(s->viewport->resource, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                               "source rectangle extends outside the buffer");
        return false;
    }
    return true;
}

#else // no viewporter glue: nothing can have a viewport

bool viewport_precheck(surface_obj*) { return true; }

#endif

void viewport_surface_destroyed(surface_obj* s) {
    if (s->viewport) {
        s->viewport->surface = nullptr;
        s->viewport = nullptr;
    }
    if (s->fractional) {
        s->fractional->surface = nullptr;
        s->fractional = nullptr;
    }
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
