module;

// wp_cursor_shape_manager_v1 / wp_cursor_shape_device_v1. Part of the protocol wire
// split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "cursor-shape-v1-server-protocol.h"
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import lx.input;
import :surface;
import :surface_tree;
import :toplevel;
import :output;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)
#if defined(LUMEN_HAS_PROTOCOL_GLUE)

namespace lx::compositor::p0 {

namespace {

/// wp_cursor_shape_device_v1 objects for wl_pointers. Fixed table: a client has one per
/// pointer, and the pointer's destructor makes its devices inert.
struct shape_device_obj {
    wl_resource* resource = nullptr;
    wl_resource* pointer = nullptr;
    bool used = false;
};
shape_device_obj shape_devices[64]{};

} // namespace

void shape_devices_drop_pointer(wl_resource* pointer) {
    for (auto& d : shape_devices)
        if (d.used && d.pointer == pointer)
            d.pointer = nullptr;
}

namespace {

const struct wp_cursor_shape_device_v1_interface shape_device_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_shape =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t serial,
           uint32_t shape) {
            if (!wp_cursor_shape_device_v1_shape_is_valid(shape,
                                                          wl_resource_get_version(resource))) {
                wl_resource_post_error(resource, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                                       "invalid cursor shape");
                return;
            }
            // Null user data: a tablet-tool device (no tablet support) — inert.
            auto* dev = static_cast<shape_device_obj*>(wl_resource_get_user_data(resource));
            if (!dev || !dev->used || !dev->pointer)
                return;
            if (!cursor_request_allowed(client, serial))
                return;
            g_p0_ctx->cursor->set_client_shape(static_cast<lx::compositor::cursor_shape>(shape));
        },
};

const struct wp_cursor_shape_manager_v1_interface shape_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_pointer =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* pointer) {
            shape_device_obj* dev = nullptr;
            for (auto& d : shape_devices)
                if (!d.used) {
                    dev = &d;
                    break;
                }
            if (!dev) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource* res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *dev = {};
            dev->used = true;
            dev->resource = res;
            dev->pointer = pointer;
            wl_resource_set_implementation(res, &shape_device_impl, dev, [](wl_resource* r) {
                auto* d = static_cast<shape_device_obj*>(wl_resource_get_user_data(r));
                if (d && d->resource == r)
                    *d = {};
            });
        },
    .get_tablet_tool_v2 =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource*) {
            wl_resource* res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &shape_device_impl, nullptr, nullptr);
        },
};

} // namespace

void bind_cursor_shape(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_cursor_shape_manager_v1_interface, version,
                                         id);
    if (!res)
        return;
    res.set_implementation(&shape_manager_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
