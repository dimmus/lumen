module;

// ext-data-control-v1 (staging) and zwlr-data-control-unstable-v1 v2: clipboard managers
// and command-line tools (wl-clipboard's wl-copy/wl-paste, cliphist through
// `wl-paste --watch`) read and set both selections without keyboard focus
// (design-app-compat-capture B.6, DC1). One implementation serves both protocols — they are
// the same protocol under two names — on the selection model of wire_selection.cpp: a
// data-control source may own the clipboard or the primary selection, every data-control
// device is sent both selections when it is created and every change after, and its offers
// stay valid until the selection moves on.
//
// Privileged like the session-wide globals (sway, niri): hidden from sandboxed clients by
// the server's filter (`deny_sandboxed`), open to the session's unsandboxed clients.
//
// Every error:
//   *_device.used_source    set_selection / set_primary_selection with a source already
//                           used (wlr: also below v2, where the code exists in the header)
//   *_source.invalid_offer  offer after the source was used
// `finished` is never sent: the one seat never goes.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_DATA_CONTROL_V1)
#include "ext-data-control-v1-server-protocol.h"
#define LUMEN_HAS_EXT_DATA_CONTROL 1
#endif
#if defined(LUMEN_PROTOCOL_WLR_DATA_CONTROL_UNSTABLE_V1)
#include "wlr-data-control-unstable-v1-server-protocol.h"
#define LUMEN_HAS_WLR_DATA_CONTROL 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_EXT_DATA_CONTROL) || defined(LUMEN_HAS_WLR_DATA_CONTROL)

namespace {

/// What differs between the two protocols.
struct dc_protocol {
    const char* name = "";
    const wl_interface* device_iface = nullptr;
    const wl_interface* source_iface = nullptr;
    const void* device_impl = nullptr;
    const void* source_impl = nullptr;
    /// Offer marshalling per selection: [clipboard] sends `selection`, [primary]
    /// `primary_selection`.
    xfer_family_ops ops[k_xfer_families]{};
    xfer_source_ops source_ops{};
    uint32_t used_source = 1;
    /// The device version from which primary_selection exists.
    int primary_since = 1;
};

/// ext_data_control_device_v1 / zwlr_data_control_device_v1. Allocated per request, in one
/// list.
struct dc_device {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    const dc_protocol* proto = nullptr;
    dc_device* prev = nullptr;
    dc_device* next = nullptr;
};

dc_device* devices = nullptr;

const char* family_name(xfer_family f) { return f == xfer_family::clipboard ? "clipboard" : "primary"; }

/// Family `f`'s selection to `d`: a fresh offer (data_offer, offer…) then the selection
/// event, or the event with null when there is none (or no offer slot is left).
void deliver(dc_device* d, xfer_family f, const char* cause) {
    if (f == xfer_family::primary && wl_resource_get_version(d->resource) < d->proto->primary_since)
        return;
    const xfer_family_ops& ops = d->proto->ops[static_cast<unsigned>(f)];
    xfer_source* src = xfer_selection_source(f);
    xfer_offer* o = src ? xfer_create_control_offer(d->client, d->resource, ops, f, src) : nullptr;
    ops.send_selection(d->resource, o ? o->resource : nullptr);
    if (surface_debug()) {
        lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.selection",
                                         "data-control offer proto={} kind={} client={} mimes={} "
                                         "cause={}",
                                         d->proto->name, family_name(f), wire_client_id(d->client),
                                         o ? src->mimes.count() : 0u, cause);
    }
}

void device_destroy_resource(wl_resource* r) {
    auto* d = static_cast<dc_device*>(wl_resource_get_user_data(r));
    if (!d)
        return;
    if (d->prev)
        d->prev->next = d->next;
    else
        devices = d->next;
    if (d->next)
        d->next->prev = d->prev;
    delete d;
}

void device_set(wl_resource* r, wl_resource* source_res, xfer_family f) {
    auto* d = static_cast<dc_device*>(wl_resource_get_user_data(r));
    if (!d)
        return;
    xfer_source* src = xfer_source_from(source_res);
    if (source_res && !src)
        return;
    if (src && src->used) {
        wl_resource_post_error(r, d->proto->used_source, "the source has already been used");
        return;
    }
    xfer_set_selection_control(f, src);
}

void source_offer(wl_resource* r, const char* mime) {
    xfer_source* src = xfer_source_from(r);
    if (!src)
        return;
    if (src->used) {
        // "offer sent after ext_data_control_device_v1.set_selection": invalid_offer, 1 in
        // both protocols (EXT_/ZWLR_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER).
        wl_resource_post_error(r, 1, "offer after the source was used");
        return;
    }
    xfer_source_offer(src, mime);
}

void offer_receive(wl_resource* r, const char* mime, int32_t fd) {
    if (auto* o = xfer_offer_from(r))
        xfer_offer_receive(o, mime, fd);
    else
        ::close(fd);
}

void create_source(wl_client* client, wl_resource* manager, uint32_t id, const dc_protocol& p) {
    xfer_source* src = xfer_create_source(client, xfer_family::clipboard, p.source_iface,
                                          p.source_impl, wl_resource_get_version(manager), id);
    if (src)
        src->ops = &p.source_ops;
}

void create_device(wl_client* client, wl_resource* manager, uint32_t id, const dc_protocol& p) {
    auto* d = new (std::nothrow) dc_device{};
    wl_resource* res =
        d ? wl_resource_create(client, p.device_iface, wl_resource_get_version(manager), id) : nullptr;
    if (!res) {
        delete d;
        wl_client_post_no_memory(client);
        return;
    }
    d->resource = res;
    d->client = client;
    d->proto = &p;
    d->next = devices;
    if (devices)
        devices->prev = d;
    devices = d;
    wl_resource_set_implementation(res, p.device_impl, d, device_destroy_resource);
    // Both selections at once: a clipboard manager starts from what is there.
    deliver(d, xfer_family::clipboard, "device");
    deliver(d, xfer_family::primary, "device");
}

// ── ext-data-control-v1 ──

#if defined(LUMEN_HAS_EXT_DATA_CONTROL)

extern const dc_protocol ext_proto;

const struct ext_data_control_offer_v1_interface ext_offer_impl = {
    .receive = [](struct wl_client*, struct wl_resource* r, const char* mime,
                  int32_t fd) { offer_receive(r, mime, fd); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct ext_data_control_source_v1_interface ext_source_impl = {
    .offer = [](struct wl_client*, struct wl_resource* r, const char* mime) { source_offer(r, mime); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct ext_data_control_device_v1_interface ext_device_impl = {
    .set_selection = [](struct wl_client*, struct wl_resource* r,
                        struct wl_resource* s) { device_set(r, s, xfer_family::clipboard); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_primary_selection = [](struct wl_client*, struct wl_resource* r,
                                struct wl_resource* s) { device_set(r, s, xfer_family::primary); },
};

const struct ext_data_control_manager_v1_interface ext_manager_impl = {
    .create_data_source = [](struct wl_client* c, struct wl_resource* r,
                             uint32_t id) { create_source(c, r, id, ext_proto); },
    .get_data_device = [](struct wl_client* c, struct wl_resource* r, uint32_t id,
                          struct wl_resource*) { create_device(c, r, id, ext_proto); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

dc_protocol make_ext() {
    dc_protocol p{};
    p.name = "ext";
    p.device_iface = &ext_data_control_device_v1_interface;
    p.source_iface = &ext_data_control_source_v1_interface;
    p.device_impl = &ext_device_impl;
    p.source_impl = &ext_source_impl;
    for (auto& o : p.ops) {
        o.name = "ext-data-control";
        o.offer_interface = &ext_data_control_offer_v1_interface;
        o.offer_impl = &ext_offer_impl;
        o.send_data_offer = [](wl_resource* d, wl_resource* o2) {
            ext_data_control_device_v1_send_data_offer(d, o2);
        };
        o.send_offer = [](wl_resource* o2, const char* m) { ext_data_control_offer_v1_send_offer(o2, m); };
    }
    p.ops[0].send_selection = [](wl_resource* d, wl_resource* o2) {
        ext_data_control_device_v1_send_selection(d, o2);
    };
    p.ops[1].send_selection = [](wl_resource* d, wl_resource* o2) {
        ext_data_control_device_v1_send_primary_selection(d, o2);
    };
    p.source_ops.send_send = [](wl_resource* s, const char* m, int32_t fd) {
        ext_data_control_source_v1_send_send(s, m, fd);
    };
    p.source_ops.send_cancelled = [](wl_resource* s) { ext_data_control_source_v1_send_cancelled(s); };
    p.used_source = EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE;
    static_assert(EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER == 1);
    p.primary_since = 1;
    return p;
}

const dc_protocol ext_proto = make_ext();

#endif

// ── zwlr-data-control-unstable-v1 ──

#if defined(LUMEN_HAS_WLR_DATA_CONTROL)

extern const dc_protocol wlr_proto;

const struct zwlr_data_control_offer_v1_interface wlr_offer_impl = {
    .receive = [](struct wl_client*, struct wl_resource* r, const char* mime,
                  int32_t fd) { offer_receive(r, mime, fd); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct zwlr_data_control_source_v1_interface wlr_source_impl = {
    .offer = [](struct wl_client*, struct wl_resource* r, const char* mime) { source_offer(r, mime); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct zwlr_data_control_device_v1_interface wlr_device_impl = {
    .set_selection = [](struct wl_client*, struct wl_resource* r,
                        struct wl_resource* s) { device_set(r, s, xfer_family::clipboard); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_primary_selection = [](struct wl_client*, struct wl_resource* r,
                                struct wl_resource* s) { device_set(r, s, xfer_family::primary); },
};

const struct zwlr_data_control_manager_v1_interface wlr_manager_impl = {
    .create_data_source = [](struct wl_client* c, struct wl_resource* r,
                             uint32_t id) { create_source(c, r, id, wlr_proto); },
    .get_data_device = [](struct wl_client* c, struct wl_resource* r, uint32_t id,
                          struct wl_resource*) { create_device(c, r, id, wlr_proto); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

dc_protocol make_wlr() {
    dc_protocol p{};
    p.name = "wlr";
    p.device_iface = &zwlr_data_control_device_v1_interface;
    p.source_iface = &zwlr_data_control_source_v1_interface;
    p.device_impl = &wlr_device_impl;
    p.source_impl = &wlr_source_impl;
    for (auto& o : p.ops) {
        o.name = "wlr-data-control";
        o.offer_interface = &zwlr_data_control_offer_v1_interface;
        o.offer_impl = &wlr_offer_impl;
        o.send_data_offer = [](wl_resource* d, wl_resource* o2) {
            zwlr_data_control_device_v1_send_data_offer(d, o2);
        };
        o.send_offer = [](wl_resource* o2, const char* m) { zwlr_data_control_offer_v1_send_offer(o2, m); };
    }
    p.ops[0].send_selection = [](wl_resource* d, wl_resource* o2) {
        zwlr_data_control_device_v1_send_selection(d, o2);
    };
    p.ops[1].send_selection = [](wl_resource* d, wl_resource* o2) {
        zwlr_data_control_device_v1_send_primary_selection(d, o2);
    };
    p.source_ops.send_send = [](wl_resource* s, const char* m, int32_t fd) {
        zwlr_data_control_source_v1_send_send(s, m, fd);
    };
    p.source_ops.send_cancelled = [](wl_resource* s) { zwlr_data_control_source_v1_send_cancelled(s); };
    p.used_source = ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE;
    static_assert(ZWLR_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER == 1);
    p.primary_since = ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION;
    return p;
}

const dc_protocol wlr_proto = make_wlr();

#endif

} // namespace

#if defined(LUMEN_HAS_EXT_DATA_CONTROL)
void bind_ext_data_control_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &ext_data_control_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&ext_manager_impl, g_p0_ctx, nullptr);
}
#endif

#if defined(LUMEN_HAS_WLR_DATA_CONTROL)
void bind_wlr_data_control_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwlr_data_control_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&wlr_manager_impl, g_p0_ctx, nullptr);
}
#endif

void data_control_selection_changed(xfer_family f) {
    for (dc_device* d = devices; d; d = d->next)
        if (!client_dying(d->client))
            deliver(d, f, "change");
}

#else

void data_control_selection_changed(xfer_family) {}

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
