module;

// zwp_primary_selection_device_manager_v1 and its device, source and offer: the primary
// selection's marshalling ("select to copy, middle-click to paste"). The model — one per
// seat, offered to the keyboard-focused client before its `enter`, serial-checked — is the
// clipboard's (wire_selection.cpp). Version 1 has no actions, no drag-and-drop and no error
// enum: a reused source is refused and cancelled instead of being a protocol error.
//
// A P1 protocol: without its generated glue this unit compiles to nothing and the global is
// not advertised. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_PRIMARY_SELECTION_UNSTABLE_V1)
#include "primary-selection-unstable-v1-server-protocol.h"
#define LUMEN_HAS_PRIMARY_SELECTION 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :wire;

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PRIMARY_SELECTION)

namespace lx::compositor::p0 {

namespace {

const struct zwp_primary_selection_source_v1_interface source_impl = {
    .offer =
        [](struct wl_client*, struct wl_resource* resource, const char* mime) {
            if (auto* s = xfer_source_from(resource))
                xfer_source_offer(s, mime);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

const struct zwp_primary_selection_offer_v1_interface offer_impl = {
    .receive =
        [](struct wl_client*, struct wl_resource* resource, const char* mime, int32_t fd) {
            if (auto* o = xfer_offer_from(resource))
                xfer_offer_receive(o, mime, fd);
            else
                ::close(fd);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

const struct zwp_primary_selection_device_v1_interface device_impl = {
    .set_selection =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* source,
           uint32_t serial) {
            if (auto* d = xfer_device_from(resource))
                xfer_set_selection(d, source, serial);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

const struct zwp_primary_selection_device_manager_v1_interface manager_impl = {
    .create_source =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            (void)xfer_create_source(client, xfer_family::primary,
                                     &zwp_primary_selection_source_v1_interface, &source_impl,
                                     wl_resource_get_version(resource), id);
        },
    .get_device =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* /*seat*/) {
            (void)xfer_create_device(client, xfer_family::primary,
                                     &zwp_primary_selection_device_v1_interface, &device_impl,
                                     wl_resource_get_version(resource), id);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

} // namespace

void install_primary_selection(p0_protocol_context&) {
    auto& ops = xfer_ops[static_cast<unsigned>(xfer_family::primary)];
    ops.name = "primary";
    ops.offer_interface = &zwp_primary_selection_offer_v1_interface;
    ops.offer_impl = &offer_impl;
    ops.send_data_offer = [](wl_resource* device, wl_resource* offer) {
        zwp_primary_selection_device_v1_send_data_offer(device, offer);
    };
    ops.send_offer = [](wl_resource* offer, const char* mime) {
        zwp_primary_selection_offer_v1_send_offer(offer, mime);
    };
    ops.send_selection = [](wl_resource* device, wl_resource* offer) {
        zwp_primary_selection_device_v1_send_selection(device, offer);
    };
    ops.send_cancelled = [](wl_resource* source) {
        zwp_primary_selection_source_v1_send_cancelled(source);
    };
    ops.send_send = [](wl_resource* source, const char* mime, int32_t fd) {
        zwp_primary_selection_source_v1_send_send(source, mime, fd);
    };
}

void bind_primary_selection_manager(wayland::client_connection& client, unsigned id,
                                    int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(
        client, &zwp_primary_selection_device_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND && LUMEN_HAS_PRIMARY_SELECTION
