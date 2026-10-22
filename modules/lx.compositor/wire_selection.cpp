module;

// The selection model shared by the clipboard (wl_data_*) and the primary selection
// (zwp_primary_selection_*_v1): one selection per seat and family, offered to the client
// with keyboard focus — before its `enter`, again on every change — and data passed
// source-to-target by fd. The family units (wire_data_device.cpp,
// wire_primary_selection.cpp) only marshal; every rule lives here. Part of the protocol
// wire split; shared declarations live in the :wire partition (wire.cppm).
//
// Choices, against the peers (wlroots, Mutter, KWin):
// - set_selection must quote an input or focus serial delivered to that client, not older
//   than the current selection's (wlroots' rule, input serials only). Keyboard focus is
//   not required (Mutter requires it, losing a Ctrl+C followed at once by a click
//   elsewhere). A rejected source is cancelled rather than left dangling.
// - An offer goes inert when its client loses keyboard focus (the spec: "valid until …
//   the client loses keyboard focus"; Mutter), so an unfocused client cannot read the
//   clipboard at will through an offer it kept.
// - Sources and devices are allocated per request and freed with their object (wlroots,
//   KWin): from a shared fixed table, one client that never destroys them would make the
//   next client's create_data_source or get_data_device fail with no_memory — fatal to a
//   client that did nothing wrong. Offers, which the compositor creates, stay in a table
//   with a per-client cap: running out there only withholds that client's selection.
// - receive is forwarded only for a type the source offered (Smithay); the fd is always
//   closed here after marshalling (libwayland dups it into the message).
// - Data control (ext-data-control-v1, zwlr-data-control v2; wire_data_control.cpp) is a
//   third kind of device and source on the same model: any source kind may own either
//   selection (each source carries its own send/cancelled ops), data-control devices hear
//   of every change of both selections whatever has the keyboard, and their offers do not
//   go inert on focus changes — a clipboard manager keeps the selection alive after its
//   owner exits by taking it over.
// UI affinity only. Allocation only where a client request creates an object.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :data_transfer;
import :input_router;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

xfer_family_ops xfer_ops[k_xfer_families]{};

namespace {

xfer_offer offers[k_max_xfer_offers]{};
/// Every live device, newest first (intrusive; each allocated by its get_*_device request).
xfer_device* devices = nullptr;

struct seat_selection {
    xfer_source* source = nullptr;
    /// Serial of the last accepted set_selection (set or unset): a later request must not
    /// quote an older one.
    unsigned serial = 0;
    bool ever_set = false;
};
seat_selection selection[k_xfer_families]{};

/// Mirror of the client owning keyboard focus (input_router's focus-client hook).
wl_client* focus_client = nullptr;

/// Clients already warned that their offers fill the table (lx client ids, never reused).
unsigned warned_full[16]{};
unsigned warned_next = 0;

constexpr const char* k_category = "compositor.selection";

const char* kind_name(xfer_family f) { return f == xfer_family::clipboard ? "clipboard" : "primary"; }

void sel_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, k_category, line);
}

template <typename... Args>
void sel_log(std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lx::trace::level::debug, k_category, fmt,
                                     std::forward<Args>(args)...);
}

unsigned res_id(wl_resource* r) { return r ? wl_resource_get_id(r) : 0; }

void unlink_offer(xfer_offer* o) {
    if (!o->source)
        return;
    if (o->prev)
        o->prev->next = o->next;
    else
        o->source->offers = o->next;
    if (o->next)
        o->next->prev = o->prev;
    o->prev = o->next = nullptr;
    o->source = nullptr;
}

void link_offer(xfer_source* s, xfer_offer* o) {
    o->source = s;
    o->prev = nullptr;
    o->next = s->offers;
    if (s->offers)
        s->offers->prev = o;
    s->offers = o;
}

/// Every offer of `s` goes inert: the selection moved on, or the source is gone.
void inert_offers_of(xfer_source* s) {
    while (s->offers)
        unlink_offer(s->offers);
}

void offer_destroy_resource(wl_resource* r) {
    auto* o = static_cast<xfer_offer*>(wl_resource_get_user_data(r));
    if (!o || !o->live || o->resource != r)
        return;
    // A drag-and-drop offer's end can end the operation (destroyed after the drop without
    // finish, or an `ask` dismissed): the source hears of it before the unlink.
    if (o->use == offer_use::dnd && o->source)
        dnd_offer_destroyed(o);
    unlink_offer(o);
    *o = {};
}

/// A free offer slot for `client`, or null when the table is full or the client already
/// holds `k_max_offers_per_client` live offers (it is not destroying them).
xfer_offer* alloc_offer(wl_client* client) {
    xfer_offer* free_slot = nullptr;
    unsigned held = 0;
    for (auto& o : offers) {
        if (!o.live) {
            if (!free_slot)
                free_slot = &o;
        } else if (o.client == client) {
            ++held;
        }
    }
    if (!free_slot || held >= k_max_offers_per_client)
        return nullptr;
    *free_slot = {};
    free_slot->live = true;
    free_slot->client = client;
    return free_slot;
}

void warn_offer_table_full(wl_client* client) {
    const unsigned id = wire_client_id(client);
    for (unsigned w : warned_full)
        if (w == id)
            return;
    warned_full[warned_next++ % 16] = id;
    lx::trace::logger::global().logf(lx::trace::level::warn, k_category,
                                     "selection offer table full client={} — sending no selection",
                                     id);
}

/// The source's own marshalling: a data-control source owning the clipboard is sent
/// ext_data_control_source_v1.send, not wl_data_source.send.
const xfer_family_ops& family_ops(xfer_family f) { return xfer_ops[static_cast<unsigned>(f)]; }

void source_send(xfer_source* src, const char* mime, int32_t fd) {
    if (src->ops && src->ops->send_send)
        src->ops->send_send(src->resource, mime, fd);
    else if (!src->ops && family_ops(src->family).send_send)
        family_ops(src->family).send_send(src->resource, mime, fd);
}

void source_cancel(xfer_source* src) {
    if (src->ops && src->ops->send_cancelled)
        src->ops->send_cancelled(src->resource);
    else if (!src->ops && family_ops(src->family).send_cancelled)
        family_ops(src->family).send_cancelled(src->resource);
}

/// A new offer of `src` for the device `dev_res` of `client`, announced with `ops`
/// (data_offer + every mime type). Null when no slot is left (warned once per client) or,
/// with `no_memory` set, when the resource could not be created (no_memory posted).
xfer_offer* create_offer_with(wl_client* client, wl_resource* dev_res, const xfer_family_ops& ops,
                              xfer_family family, xfer_source* src, offer_use use,
                              bool& no_memory) {
    no_memory = false;
    xfer_offer* o = alloc_offer(client);
    if (!o) {
        warn_offer_table_full(client);
        return nullptr;
    }
    o->resource = wl_resource_create(client, ops.offer_interface,
                                     wl_resource_get_version(dev_res), 0);
    if (!o->resource) {
        *o = {};
        wl_client_post_no_memory(client);
        no_memory = true;
        return nullptr;
    }
    o->family = family;
    o->use = use;
    wl_resource_set_implementation(o->resource, ops.offer_impl, o, offer_destroy_resource);
    link_offer(src, o);
    ops.send_data_offer(dev_res, o->resource);
    for (unsigned i = 0; i < src->mimes.count(); ++i)
        ops.send_offer(o->resource, src->mimes.at(i));
    return o;
}

xfer_offer* create_offer(xfer_device* dev, xfer_source* src, offer_use use, bool& no_memory) {
    return create_offer_with(dev->client, dev->resource, family_ops(dev->family), dev->family,
                             src, use, no_memory);
}

/// Sends the current selection of the device's family to it: a fresh offer with every mime
/// type, then `selection` — or `selection(NULL)` when there is none (or no offer slot).
void deliver(xfer_device* dev, const char* cause) {
    const xfer_family_ops& ops = xfer_ops[static_cast<unsigned>(dev->family)];
    if (!ops.send_selection)
        return;
    xfer_source* src = selection[static_cast<unsigned>(dev->family)].source;
    // An owner being torn down (its surfaces go before its source, so focus can move while
    // the source still exists) cannot serve a paste: offer nothing rather than a dead end.
    if (src && client_dying(src->client))
        src = nullptr;
    xfer_offer* o = nullptr;
    if (src) {
        bool no_memory = false;
        o = create_offer(dev, src, offer_use::selection, no_memory);
        if (no_memory)
            return;
        if (!o)
            cause = "full";
    }
    ops.send_selection(dev->resource, o ? o->resource : nullptr);
    if (surface_debug()) {
        sel_log("selection offer kind={} client={} offer={} mimes={} cause={}",
                kind_name(dev->family), wire_client_id(dev->client),
                res_id(o ? o->resource : nullptr), o ? src->mimes.count() : 0u, cause);
    }
}

/// Delivers family `f`'s selection to every device of `client` of that family.
void deliver_to_client(xfer_family f, wl_client* client, const char* cause) {
    if (!client || client_dying(client))
        return;
    for (xfer_device* d = devices; d; d = d->next)
        if (d->client == client && d->family == f)
            deliver(d, cause);
}

void source_destroy_resource(wl_resource* r) {
    auto* s = static_cast<xfer_source*>(wl_resource_get_user_data(r));
    if (!s || !s->live || s->resource != r)
        return;
    // A drag source: the operation ends (the target is left) before its offers go inert.
    if (s->role == source_role::dnd)
        dnd_source_destroyed(s);
    inert_offers_of(s);
    auto& sel = selection[static_cast<unsigned>(s->family)];
    if (sel.source == s) {
        // The owner gave the selection up (or exited): nobody owns it now, and the focused
        // client is told so (wlroots). Nothing persists it — that is a clipboard manager's
        // job (ext-data-control), not the compositor's.
        sel.source = nullptr;
        if (surface_debug()) {
            sel_log("selection clear kind={} cause=source-destroyed", kind_name(s->family));
        }
        deliver_to_client(s->family, focus_client, "clear");
        data_control_selection_changed(s->family);
    }
    delete s;
}

void device_destroy_resource(wl_resource* r) {
    auto* d = static_cast<xfer_device*>(wl_resource_get_user_data(r));
    if (!d || !d->live || d->resource != r)
        return;
    if (d->prev)
        d->prev->next = d->next;
    else
        devices = d->next;
    if (d->next)
        d->next->prev = d->prev;
    delete d;
}

void log_reject(xfer_family f, wl_client* client, uint32_t serial, const char* why) {
    if (!surface_debug())
        return;
    sel_log("selection reject kind={} client={} serial={} reason={}", kind_name(f),
            wire_client_id(client), serial, why);
}

} // namespace

xfer_offer* xfer_create_offer(xfer_device* dev, xfer_source* src, offer_use use) {
    bool no_memory = false;
    return dev && src ? create_offer(dev, src, use, no_memory) : nullptr;
}

xfer_offer* xfer_create_control_offer(wl_client* client, wl_resource* device_res,
                                      const xfer_family_ops& ops, xfer_family family,
                                      xfer_source* source) {
    bool no_memory = false;
    return client && device_res && source
               ? create_offer_with(client, device_res, ops, family, source, offer_use::control,
                                   no_memory)
               : nullptr;
}

xfer_source* xfer_selection_source(xfer_family f) {
    xfer_source* src = selection[static_cast<unsigned>(f)].source;
    return src && !client_dying(src->client) ? src : nullptr;
}

void xfer_set_selection_control(xfer_family f, xfer_source* src) {
    auto& sel = selection[static_cast<unsigned>(f)];
    if (src) {
        src->used = true;
        src->family = f;
    }
    xfer_source* old = sel.source;
    if (old && old != src) {
        inert_offers_of(old);
        old->role = source_role::none;
        if (!client_dying(old->client))
            source_cancel(old);
    }
    // The serial stays: a clipboard manager re-setting the selection must not make the next
    // Ctrl+C of an application look stale (wlroots takes a fresh display serial here, which
    // then rejects an application's older input serial).
    sel.source = src;
    sel.ever_set = sel.ever_set || src != nullptr;
    if (src)
        src->role = source_role::selection;
    if (surface_debug()) {
        sel_log("selection set kind={} client={} source={} mimes={} by=data-control", kind_name(f),
                src ? wire_client_id(src->client) : 0u, res_id(src ? src->resource : nullptr),
                src ? src->mimes.count() : 0u);
    }
    deliver_to_client(f, focus_client, "set");
    data_control_selection_changed(f);
}

void xfer_inert_offer(xfer_offer* o) {
    if (o)
        unlink_offer(o);
}

unsigned xfer_devices_of(wl_client* client, xfer_family family, xfer_device** out,
                         unsigned cap) {
    unsigned n = 0;
    if (!client)
        return 0;
    for (xfer_device* d = devices; d && n < cap; d = d->next)
        if (d->client == client && d->family == family)
            out[n++] = d;
    return n;
}

unsigned wire_client_id(wl_client* client) {
    if (!client || !g_p0_ctx || !g_p0_ctx->server)
        return 0;
    const auto* conn = g_p0_ctx->server->find_client_by_native(client);
    return conn ? conn->id() : 0;
}

xfer_source* xfer_create_source(wl_client* client, xfer_family family,
                                const wl_interface* iface, const void* impl, int version,
                                uint32_t id) {
    // Default-initialized, not value-initialized: the 4 KiB mime arena is left as it is
    // (mime_list::clear semantics); every other member has its initializer.
    auto* s = new (std::nothrow) xfer_source;
    wl_resource* res = s ? wl_resource_create(client, iface, version, id) : nullptr;
    if (!res) {
        delete s;
        wl_client_post_no_memory(client);
        return nullptr;
    }
    s->resource = res;
    s->client = client;
    s->family = family;
    s->live = true;
    wl_resource_set_implementation(res, impl, s, source_destroy_resource);
    return s;
}

xfer_device* xfer_create_device(wl_client* client, xfer_family family,
                                const wl_interface* iface, const void* impl, int version,
                                uint32_t id) {
    auto* d = new (std::nothrow) xfer_device{};
    wl_resource* res = d ? wl_resource_create(client, iface, version, id) : nullptr;
    if (!res) {
        delete d;
        wl_client_post_no_memory(client);
        return nullptr;
    }
    d->resource = res;
    d->client = client;
    d->family = family;
    d->live = true;
    d->next = devices;
    if (devices)
        devices->prev = d;
    devices = d;
    wl_resource_set_implementation(res, impl, d, device_destroy_resource);
    // Created while its client holds the keyboard: it would otherwise not hear of the
    // selection until the next change (wlroots does the same).
    if (client == focus_client)
        deliver(d, "device");
    return d;
}

xfer_source* xfer_source_from(wl_resource* r) {
    if (!r)
        return nullptr;
    auto* s = static_cast<xfer_source*>(wl_resource_get_user_data(r));
    return s && s->live && s->resource == r ? s : nullptr;
}

xfer_device* xfer_device_from(wl_resource* r) {
    if (!r)
        return nullptr;
    auto* d = static_cast<xfer_device*>(wl_resource_get_user_data(r));
    return d && d->live && d->resource == r ? d : nullptr;
}

xfer_offer* xfer_offer_from(wl_resource* r) {
    if (!r)
        return nullptr;
    auto* o = static_cast<xfer_offer*>(wl_resource_get_user_data(r));
    return o && o->live && o->resource == r ? o : nullptr;
}

void xfer_source_offer(xfer_source* s, const char* mime) {
    if (s->used) {
        // Offers already sent cannot grow; the type would be refused at receive anyway.
        if (surface_debug()) {
            sel_log("selection offer-late kind={} client={} source={} mime={:.96}",
                    kind_name(s->family), wire_client_id(s->client), res_id(s->resource),
                    mime ? mime : "-");
        }
        return;
    }
    if (!s->mimes.add(mime) && !s->mime_overflow_logged) {
        s->mime_overflow_logged = true;
        if (surface_debug()) {
            sel_log("selection offer-overflow kind={} client={} source={} mimes={}",
                    kind_name(s->family), wire_client_id(s->client), res_id(s->resource),
                    s->mimes.count());
        }
    }
}

void xfer_set_selection(xfer_device* dev, wl_resource* source_res, uint32_t serial) {
    xfer_source* src = xfer_source_from(source_res);
    if (source_res && !src)
        return;
    const xfer_family f = dev->family;
    const xfer_family_ops& ops = xfer_ops[static_cast<unsigned>(f)];
    wl_client* client = dev->client;

    if (src && src->used) {
        // "The source can be used only once" (wl_data_device.error.used_source). The
        // primary selection has no error enum: the request is refused and the source told.
        if (f == xfer_family::clipboard) {
            wl_resource_post_error(dev->resource, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                                   "wl_data_source@%u has already been used",
                                   res_id(src->resource));
            return;
        }
        log_reject(f, client, serial, "used");
        if (ops.send_cancelled)
            ops.send_cancelled(src->resource);
        return;
    }
    if (src && src->actions_set) {
        // A source that declared drag-and-drop actions is a drag source.
        wl_resource_post_error(src->resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                               "drag-and-drop source used as selection");
        return;
    }
    if (src)
        src->used = true;

    // The serial of an input event this client was sent, and not older than the selection
    // in place: a stale request (raced by a newer copy elsewhere) must not win.
    auto& sel = selection[static_cast<unsigned>(f)];
    const char* why = nullptr;
    if (!g_p0_ctx || !g_p0_ctx->input || !g_p0_ctx->input->serials().delivered(client, serial))
        why = "serial";
    else if (sel.ever_set && serial_newer(sel.serial, serial))
        why = "stale";
    if (why) {
        log_reject(f, client, serial, why);
        if (src && ops.send_cancelled)
            ops.send_cancelled(src->resource);
        return;
    }

    xfer_source* old = sel.source;
    if (old && old != src) {
        inert_offers_of(old);
        old->role = source_role::none;
        if (!client_dying(old->client))
            source_cancel(old);
    }
    sel.source = src;
    sel.serial = serial;
    sel.ever_set = true;
    if (src)
        src->role = source_role::selection;
    if (surface_debug()) {
        char line[160];
        if (src)
            std::snprintf(line, sizeof(line),
                          "selection set kind=%s client=%u source=%u mimes=%u serial=%u",
                          kind_name(f), wire_client_id(client), res_id(src->resource),
                          src->mimes.count(), serial);
        else
            std::snprintf(line, sizeof(line), "selection unset kind=%s client=%u serial=%u",
                          kind_name(f), wire_client_id(client), serial);
        sel_log(line);
    }
    deliver_to_client(f, focus_client, "set");
    data_control_selection_changed(f);
}

void xfer_offer_receive(xfer_offer* o, const char* mime, int32_t fd) {
    xfer_source* src = o->source;
    const char* why = nullptr;
    if (!src)
        why = "inert";
    else if (o->finished)
        why = "finished";
    else if (!src->mimes.contains(mime))
        why = "mime";
    else if (client_dying(src->client))
        why = "source-gone";
    if (!why)
        source_send(src, mime, fd);
    // Marshalling dup'ed it (or nothing was sent): the compositor's copy is closed either way.
    ::close(fd);
    if (surface_debug()) {
        sel_log("selection receive kind={} client={} source_client={} mime={:.96} result={}{}{}",
                kind_name(o->family), wire_client_id(o->client),
                src ? wire_client_id(src->client) : 0u, mime ? mime : "-", why ? "ignored" : "sent",
                why ? " reason=" : "", why ? why : "");
    }
}

void xfer_focus_changed(void*, void* old_client_ptr, void* new_client_ptr) {
    auto* old_client = static_cast<wl_client*>(old_client_ptr);
    auto* new_client = static_cast<wl_client*>(new_client_ptr);
    if (old_client) {
        unsigned inert[k_xfer_families]{};
        for (auto& o : offers) {
            if (!o.live || o.client != old_client || o.use != offer_use::selection || !o.source)
                continue;
            unlink_offer(&o);
            ++inert[static_cast<unsigned>(o.family)];
        }
        if (surface_debug()) {
            for (unsigned f = 0; f < k_xfer_families; ++f) {
                if (!inert[f])
                    continue;
                sel_log("selection inert kind={} client={} offers={} cause=focus-lost",
                        kind_name(static_cast<xfer_family>(f)), wire_client_id(old_client),
                        inert[f]);
            }
        }
    }
    focus_client = new_client;
    // Before the keyboard `enter` (the router calls this between leave and enter): "the
    // selection event is sent to a client immediately before receiving keyboard focus".
    deliver_to_client(xfer_family::clipboard, new_client, "focus");
    deliver_to_client(xfer_family::primary, new_client, "focus");
}

void wire_client_destroyed(void*, unsigned client_id, void* client) {
    if (g_p0_ctx && g_p0_ctx->input)
        g_p0_ctx->input->forget_client(client);
    if (g_p0_ctx && g_p0_ctx->liveness)
        g_p0_ctx->liveness->forget(lx::client_id{client_id});
    if (focus_client == client)
        focus_client = nullptr;
    dnd_client_destroyed(static_cast<wl_client*>(client));
    pointer_gestures_client_destroyed(static_cast<wl_client*>(client));
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
