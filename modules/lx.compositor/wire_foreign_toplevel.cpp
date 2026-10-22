module;

// ext-foreign-toplevel-list-v1 (staging): a list of the mapped windows, for capture pickers
// (xdg-desktop-portal-wlr's window chooser, `grim -T`) and taskbars (design-app-compat-capture
// D.1, FT1). Every bound list gets one handle per mapped toplevel — at bind, and at every map
// after — then the handle's identifier, title and app id and `done`; a title or app id change
// is sent with `done`; an unmap sends `closed`, after which the handle is inert (requests other
// than destroy are ignored; the protocol has none). `stop` ends the list: `finished`, and no
// more `toplevel` events.
//
// Identifiers are 32 hex digits from 128 random bits, new at every map: "If the toplevel is
// unmapped the identifier must not be reused", and an unguessable one keeps a client from
// naming a window it was never shown. A handle is the source of a window capture
// (ext_foreign_toplevel_image_capture_source_manager_v1, wire_capture.cpp).
//
// Session-wide, so hidden from sandboxed clients (SC2): they share their screen through the
// portal. The window manager's map / unmap / state changes reach here through the shell
// bridge's observer (`toplevel_observer`), whether or not a shell is bound.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__linux__)
#include <sys/random.h>
#endif

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1)
#include "ext-foreign-toplevel-list-v1-server-protocol.h"
#define LUMEN_HAS_FOREIGN_TOPLEVEL_LIST 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :toplevel;
import :shell_bridge;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

/// 64-bit FNV-1a: what a handle was last told, without keeping the strings.
unsigned long long text_hash(const char* s) {
    unsigned long long h = 1469598103934665603ull;
    for (const char* p = s ? s : ""; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    return h;
}

/// 32 lowercase hex digits of 128 random bits.
void make_identifier(char (&out)[33]) {
    unsigned char bytes[16]{};
    std::size_t got = 0;
#if defined(__linux__)
    while (got < sizeof(bytes)) {
        const ssize_t n = ::getrandom(bytes + got, sizeof(bytes) - got, 0);
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
#endif
    if (got < sizeof(bytes)) {
        // getrandom cannot fail for 16 bytes once the pool is initialized; never reuse even
        // then — a counter mixed with the clock.
        static unsigned long long counter = 0;
        const unsigned long long v = ++counter ^ reinterpret_cast<unsigned long long>(&counter);
        for (unsigned i = 0; i < 16; ++i)
            bytes[i] = static_cast<unsigned char>((v >> ((i % 8) * 8)) ^ (i * 0x9Du));
    }
    static const char k_hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[i * 2] = k_hex[bytes[i] >> 4];
        out[i * 2 + 1] = k_hex[bytes[i] & 0xF];
    }
    out[32] = '\0';
}

} // namespace

#if defined(LUMEN_HAS_FOREIGN_TOPLEVEL_LIST)

namespace {

/// ext_foreign_toplevel_list_v1. Allocated per bind, freed with its resource.
struct list_obj {
    wl_resource* resource = nullptr;
    bool stopped = false;
    list_obj* prev = nullptr;
    list_obj* next = nullptr;
};

/// ext_foreign_toplevel_handle_v1. `toplevel` empty once closed (inert). Allocated per
/// event, freed with its resource; linked into one list (every handle of every client).
struct handle_obj {
    wl_resource* resource = nullptr;
    lx::toplevel_id toplevel{};
    /// The mapping it stands for (a remap is a new handle).
    char identifier[33]{};
    unsigned long long title_hash = 0;
    unsigned long long app_id_hash = 0;
    handle_obj* prev = nullptr;
    handle_obj* next = nullptr;
};

list_obj* g_lists = nullptr;
handle_obj* g_handles = nullptr;

void handle_unlink(handle_obj* h) {
    if (h->prev)
        h->prev->next = h->next;
    else if (g_handles == h)
        g_handles = h->next;
    if (h->next)
        h->next->prev = h->prev;
    h->prev = h->next = nullptr;
}

void handle_destroy_resource(wl_resource* resource) {
    auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(resource));
    if (!h)
        return;
    handle_unlink(h);
    delete h;
}

const struct ext_foreign_toplevel_handle_v1_interface handle_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

/// Sends the title and app id when they differ from what `h` was told (and `force`: the
/// initial batch, where an empty one is not sent at all). True when anything went out.
bool send_strings(handle_obj* h, const toplevel_record& rec, bool initial) {
    bool sent = false;
    const unsigned long long th = text_hash(rec.title);
    const unsigned long long ah = text_hash(rec.app_id);
    if ((initial && rec.title && *rec.title) || (!initial && th != h->title_hash)) {
        ext_foreign_toplevel_handle_v1_send_title(h->resource, rec.title ? rec.title : "");
        sent = true;
    }
    if ((initial && rec.app_id && *rec.app_id) || (!initial && ah != h->app_id_hash)) {
        ext_foreign_toplevel_handle_v1_send_app_id(h->resource, rec.app_id ? rec.app_id : "");
        sent = true;
    }
    h->title_hash = th;
    h->app_id_hash = ah;
    return sent;
}

/// A handle of `rec` for list `l`: toplevel, identifier, title, app id, done.
void announce(list_obj* l, const toplevel_record& rec) {
    if (!l || l->stopped || !rec.mapped || !rec.foreign_identifier[0])
        return;
    wl_client* client = wl_resource_get_client(l->resource);
    wl_resource* res = wl_resource_create(client, &ext_foreign_toplevel_handle_v1_interface,
                                          wl_resource_get_version(l->resource), 0);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* h = new (std::nothrow) handle_obj{};
    if (!h) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    h->resource = res;
    h->toplevel = rec.id;
    std::memcpy(h->identifier, rec.foreign_identifier, sizeof(h->identifier));
    h->next = g_handles;
    if (g_handles)
        g_handles->prev = h;
    g_handles = h;
    wl_resource_set_implementation(res, &handle_impl, h, &handle_destroy_resource);
    ext_foreign_toplevel_list_v1_send_toplevel(l->resource, res);
    ext_foreign_toplevel_handle_v1_send_identifier(res, rec.foreign_identifier);
    (void)send_strings(h, rec, true);
    ext_foreign_toplevel_handle_v1_send_done(res);
}

void list_unlink(list_obj* l) {
    if (l->prev)
        l->prev->next = l->next;
    else if (g_lists == l)
        g_lists = l->next;
    if (l->next)
        l->next->prev = l->prev;
    l->prev = l->next = nullptr;
}

void list_destroy_resource(wl_resource* resource) {
    auto* l = static_cast<list_obj*>(wl_resource_get_user_data(resource));
    if (!l)
        return;
    // Its handles stay: they are objects of their own ("destroy the handles and then this
    // object" is the client's order), and keep receiving closed / changes.
    list_unlink(l);
    delete l;
}

const struct ext_foreign_toplevel_list_v1_interface list_impl = {
    .stop =
        [](struct wl_client*, struct wl_resource* resource) {
            auto* l = static_cast<list_obj*>(wl_resource_get_user_data(resource));
            if (!l || l->stopped)
                return;
            l->stopped = true;
            ext_foreign_toplevel_list_v1_send_finished(resource);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

} // namespace

void bind_foreign_toplevel_list(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx || !g_p0_ctx->toplevels)
        return;
    auto res = wayland::resource::create(client, &ext_foreign_toplevel_list_v1_interface, version, id);
    if (!res)
        return;
    auto* l = new (std::nothrow) list_obj{};
    if (!l) {
        wl_resource_destroy(static_cast<wl_resource*>(res.native()));
        return;
    }
    l->resource = static_cast<wl_resource*>(res.native());
    l->next = g_lists;
    if (g_lists)
        g_lists->prev = l;
    g_lists = l;
    wl_resource_set_implementation(l->resource, &list_impl, l, &list_destroy_resource);
    // Every window mapped now, bottom to top.
    g_p0_ctx->toplevels->for_each_bottom_up([&](const toplevel_record& rec) { announce(l, rec); });
}

lx::toplevel_id foreign_toplevel_of(wl_resource* handle, const char*& identifier) {
    identifier = "";
    if (!handle || !wl_resource_instance_of(handle, &ext_foreign_toplevel_handle_v1_interface,
                                            &handle_impl))
        return {};
    auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(handle));
    if (!h || !h->toplevel)
        return {};
    identifier = h->identifier;
    return h->toplevel;
}

#else

lx::toplevel_id foreign_toplevel_of(wl_resource*, const char*& identifier) {
    identifier = "";
    return {};
}

#endif // LUMEN_HAS_FOREIGN_TOPLEVEL_LIST

namespace {

void observe_mapped(void*, toplevel_record& rec) {
    make_identifier(rec.foreign_identifier);
#if defined(LUMEN_HAS_FOREIGN_TOPLEVEL_LIST)
    for (list_obj* l = g_lists; l; l = l->next)
        announce(l, rec);
#endif
}

void observe_unmapped(void*, toplevel_record& rec) {
#if defined(LUMEN_HAS_FOREIGN_TOPLEVEL_LIST)
    for (handle_obj* h = g_handles; h; h = h->next) {
        if (h->toplevel != rec.id)
            continue;
        ext_foreign_toplevel_handle_v1_send_closed(h->resource);
        h->toplevel = {};
    }
#endif
    // Window captures of this mapping stop (CC9: "Unmapped toplevel → stopped").
    capture_toplevel_unmapped(rec.id);
    rec.foreign_identifier[0] = '\0';
}

void observe_changed(void*, const toplevel_record& rec) {
#if defined(LUMEN_HAS_FOREIGN_TOPLEVEL_LIST)
    if (!rec.mapped)
        return;
    for (handle_obj* h = g_handles; h; h = h->next)
        if (h->toplevel == rec.id && send_strings(h, rec, false))
            ext_foreign_toplevel_handle_v1_send_done(h->resource);
#else
    (void)rec;
#endif
}

} // namespace

void install_toplevel_observer(p0_protocol_context& ctx) {
    if (!ctx.shell)
        return;
    toplevel_observer o{};
    o.mapped = &observe_mapped;
    o.unmapped = &observe_unmapped;
    o.changed = &observe_changed;
    ctx.shell->set_observer(o);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
