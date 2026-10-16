module;

// wl_data_device_manager, wl_data_device, wl_data_source and wl_data_offer: the clipboard
// family's marshalling and its version-specific request checks. The selection model itself
// is shared with the primary selection (wire_selection.cpp); drag-and-drop lives in
// wire_dnd.cpp. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>

#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :data_transfer;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

// ── wl_data_source ─────────────────────────────────────────────────────────

const struct wl_data_source_interface data_source_impl = {
    .offer =
        [](struct wl_client*, struct wl_resource* resource, const char* mime) {
            if (auto* s = xfer_source_from(resource))
                xfer_source_offer(s, mime);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    // v3. Once, with only defined bits, before the source is used (the error is
    // invalid_action_mask in all three cases, as in wlroots and Mutter).
    .set_actions =
        [](struct wl_client*, struct wl_resource* resource, uint32_t actions) {
            auto* s = xfer_source_from(resource);
            if (!s)
                return;
            if (!lx::compositor::valid_dnd_mask(actions)) {
                wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                                       "invalid action mask %u", actions);
                return;
            }
            if (s->actions_set) {
                wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                                       "set_actions called twice");
                return;
            }
            if (s->used) {
                wl_resource_post_error(resource, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                                       "set_actions after start_drag/set_selection");
                return;
            }
            s->dnd_actions = actions;
            s->actions_set = true;
        },
};

// ── wl_data_offer ──────────────────────────────────────────────────────────

const struct wl_data_offer_interface data_offer_impl = {
    // Feedback for drag-and-drop only; a selection offer has nothing to accept.
    .accept =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial, const char* mime) {
            auto* o = xfer_offer_from(resource);
            if (o && o->use == offer_use::dnd)
                dnd_offer_accept(o, serial, mime);
        },
    .receive =
        [](struct wl_client*, struct wl_resource* resource, const char* mime, int32_t fd) {
            if (auto* o = xfer_offer_from(resource))
                xfer_offer_receive(o, mime, fd);
            else
                ::close(fd);
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    // v3: drag-and-drop requests. On a selection offer each is the error the spec names.
    .finish =
        [](struct wl_client*, struct wl_resource* resource) {
            auto* o = xfer_offer_from(resource);
            if (o && o->use == offer_use::selection)
                wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                                       "offer is not drag-and-drop");
            else if (o)
                dnd_offer_finish(o);
        },
    .set_actions =
        [](struct wl_client*, struct wl_resource* resource, uint32_t actions,
           uint32_t preferred) {
            auto* o = xfer_offer_from(resource);
            if (o && o->use == offer_use::selection)
                wl_resource_post_error(resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
                                       "offer is not drag-and-drop");
            else if (o)
                dnd_offer_set_actions(o, actions, preferred);
        },
};

// ── wl_data_device ─────────────────────────────────────────────────────────

const struct wl_data_device_interface data_device_impl = {
    .start_drag =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* source,
           struct wl_resource* origin, struct wl_resource* icon, uint32_t serial) {
            if (auto* d = xfer_device_from(resource))
                dnd_start_drag(d, source, origin, icon, serial);
        },
    .set_selection =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* source,
           uint32_t serial) {
            if (auto* d = xfer_device_from(resource))
                xfer_set_selection(d, source, serial);
        },
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

// ── wl_data_device_manager ─────────────────────────────────────────────────

const struct wl_data_device_manager_interface data_device_manager_impl = {
    .create_data_source =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            (void)xfer_create_source(client, xfer_family::clipboard, &wl_data_source_interface,
                                     &data_source_impl, wl_resource_get_version(resource), id);
        },
    .get_data_device =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* /*seat*/) {
            (void)xfer_create_device(client, xfer_family::clipboard, &wl_data_device_interface,
                                     &data_device_impl, wl_resource_get_version(resource), id);
        },
#if defined(WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION)
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
#endif
};

} // namespace

void install_data_device(p0_protocol_context&) {
    auto& ops = xfer_ops[static_cast<unsigned>(xfer_family::clipboard)];
    ops.name = "clipboard";
    ops.offer_interface = &wl_data_offer_interface;
    ops.offer_impl = &data_offer_impl;
    ops.send_data_offer = [](wl_resource* device, wl_resource* offer) {
        wl_data_device_send_data_offer(device, offer);
    };
    ops.send_offer = [](wl_resource* offer, const char* mime) {
        wl_data_offer_send_offer(offer, mime);
    };
    ops.send_selection = [](wl_resource* device, wl_resource* offer) {
        wl_data_device_send_selection(device, offer);
    };
    // Every version: on replacement (all versions) and on a refused set_selection, which the
    // selection in place has in effect replaced (Mutter does the same).
    ops.send_cancelled = [](wl_resource* source) { wl_data_source_send_cancelled(source); };
    ops.send_send = [](wl_resource* source, const char* mime, int32_t fd) {
        wl_data_source_send_send(source, mime, fd);
    };
}

void bind_data_device_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wl_data_device_manager_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&data_device_manager_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
