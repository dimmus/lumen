module;

// xdg-foreign, unstable v2 and v1: one client exports a window as a handle, another imports
// the handle and parents its own window onto it — how a portal's file chooser or a
// sandboxed app's dialog sits on the app's window although they are different clients
// (design-app-compat-capture B.4, XF1). Both versions share one registry
// (`foreign_registry`, as wlroots' wlr_xdg_foreign_registry), so either may import what
// either exported: GTK 3 and older Firefox speak only v1, Qt 6 only v2, GTK 4 both.
//
// set_parent_of is xdg_toplevel.set_parent across clients (`window_manager::set_parent`:
// placed on and centred over the parent, a modal dialog blocks it). Each import records the
// children it parented (≤ 8) so the relation is undone when the imported or the exported
// object goes, or the exported window is destroyed — then every import of it is sent
// `destroyed`, which "may happen for various reasons, for example if the exported surface or
// the exported surface handle has been destroyed, if the handle used for importing was
// invalid".
//
// Every error (v2; v1 defines none):
//   zxdg_exporter_v2.invalid_surface   export_toplevel of a surface that is not an
//                                      xdg_toplevel
//   zxdg_imported_v2.invalid_surface   set_parent_of a surface that is not an xdg_toplevel
// v1 has no error enum: a non-toplevel export yields a handle that imports as `destroyed`
// (wlroots' v1 behaviour), a non-toplevel set_parent_of is ignored. Handles are 128 random
// bits: a client cannot parent onto a window whose handle it was not given.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V2)
#include "xdg-shell-server-protocol.h"
#include "xdg-foreign-unstable-v2-server-protocol.h"
#define LUMEN_HAS_FOREIGN_V2 1
#endif
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V1)
// v1's exporter request is named `export`, a C++ keyword: renamed for this header only (as
// the layer-shell header's `namespace` is).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define export export_
#include "xdg-foreign-unstable-v1-server-protocol.h"
#undef export
#pragma clang diagnostic pop
#define LUMEN_HAS_FOREIGN_V1 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :toplevel;
import :foreign_registry;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_FOREIGN_V2) || defined(LUMEN_HAS_FOREIGN_V1)

namespace {

constexpr unsigned k_import_children = 8;

struct imported_obj;

/// zxdg_exported_v2 / zxdg_exported_v1. `reg` 0: inert (its window went, or it never named
/// one). Allocated per request.
struct exported_obj {
    wl_resource* resource = nullptr;
    unsigned reg = 0;
    lx::toplevel_id toplevel{};
    /// The live imports of its handle (intrusive).
    imported_obj* imports = nullptr;
};

/// zxdg_imported_v2 / zxdg_imported_v1. `exported` null: inert (`destroyed` was sent).
struct imported_obj {
    wl_resource* resource = nullptr;
    exported_obj* exported = nullptr;
    bool v2 = false;
    lx::toplevel_id children[k_import_children]{};
    unsigned child_count = 0;
    imported_obj* prev = nullptr;
    imported_obj* next = nullptr;
};

constexpr const char* k_category = "compositor.foreign";

void foreign_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, k_category, line);
}

template <typename... Args>
void foreign_log(std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lx::trace::level::debug, k_category, fmt,
                                     std::forward<Args>(args)...);
}

lx::compositor::foreign_registry* registry() { return g_p0_ctx ? g_p0_ctx->foreign : nullptr; }

void link(exported_obj* e, imported_obj* i) {
    i->exported = e;
    i->prev = nullptr;
    i->next = e->imports;
    if (e->imports)
        e->imports->prev = i;
    e->imports = i;
}

void unlink(imported_obj* i) {
    exported_obj* e = i->exported;
    if (!e)
        return;
    if (i->prev)
        i->prev->next = i->next;
    else
        e->imports = i->next;
    if (i->next)
        i->next->prev = i->prev;
    i->prev = i->next = nullptr;
    i->exported = nullptr;
}

/// The relations the import set up are undone: children still parented onto the exported
/// window get no parent (nothing else of the client's choosing).
void undo_children(imported_obj* i, lx::toplevel_id parent) {
    for (unsigned c = 0; c < i->child_count; ++c) {
        auto* rec = g_p0_ctx && g_p0_ctx->toplevels
                        ? g_p0_ctx->toplevels->find_mut(i->children[c])
                        : nullptr;
        if (!rec || !parent || rec->parent != parent)
            continue;
        if (g_p0_ctx->wm)
            g_p0_ctx->wm->set_parent(rec->id, {});
        else
            rec->parent = {};
    }
    i->child_count = 0;
}

void send_destroyed(imported_obj* i) {
#if defined(LUMEN_HAS_FOREIGN_V2)
    if (i->v2) {
        zxdg_imported_v2_send_destroyed(i->resource);
        return;
    }
#endif
#if defined(LUMEN_HAS_FOREIGN_V1)
    if (!i->v2)
        zxdg_imported_v1_send_destroyed(i->resource);
#endif
}

/// The export goes inert: its handle is withdrawn, and each import is told (unless its own
/// client is being torn down) and undone.
void retire_export(exported_obj* e) {
    if (auto* reg = registry(); reg && e->reg)
        reg->remove(e->reg);
    e->reg = 0;
    while (imported_obj* i = e->imports) {
        undo_children(i, e->toplevel);
        unlink(i);
        if (!client_dying(wl_resource_get_client(i->resource)))
            send_destroyed(i);
    }
    e->toplevel = {};
}

void exported_destroy_resource(wl_resource* r) {
    auto* e = static_cast<exported_obj*>(wl_resource_get_user_data(r));
    if (!e)
        return;
    retire_export(e);
    delete e;
}

void imported_destroy_resource(wl_resource* r) {
    auto* i = static_cast<imported_obj*>(wl_resource_get_user_data(r));
    if (!i)
        return;
    if (i->exported)
        undo_children(i, i->exported->toplevel);
    unlink(i);
    delete i;
}

/// The toplevel record of an xdg_toplevel's wl_surface, or null.
lx::compositor::toplevel_record* toplevel_of_surface(wl_resource* surface_res) {
    surface_obj* s = surface_from(surface_res);
    if (!s || s->role != surface_role::xdg_toplevel || !s->toplevel || !g_p0_ctx->toplevels)
        return nullptr;
    return g_p0_ctx->toplevels->find_mut(s->toplevel);
}

/// Creates the exported object for `surface_res` (`rec` null: an inert v1 export) and sends
/// its handle.
void do_export(wl_client* client, wl_resource* manager, const wl_interface* iface,
               const void* impl, uint32_t id, lx::compositor::toplevel_record* rec,
               void (*send_handle)(wl_resource*, const char*)) {
    auto* e = new (std::nothrow) exported_obj{};
    wl_resource* res =
        e ? wl_resource_create(client, iface, wl_resource_get_version(manager), id) : nullptr;
    if (!res) {
        delete e;
        wl_client_post_no_memory(client);
        return;
    }
    e->resource = res;
    wl_resource_set_implementation(res, impl, e, exported_destroy_resource);
    char handle[lx::compositor::k_foreign_handle_bytes]{};
    if (rec && registry())
        e->reg = registry()->add(rec->id, e, handle);
    if (e->reg)
        e->toplevel = rec->id;
    else if (!lx::compositor::foreign_registry::make_handle(handle))
        std::snprintf(handle, sizeof(handle), "%s", "0");
    // A handle is sent either way: an unregistered one imports as `destroyed`.
    send_handle(res, handle);
    if (surface_debug()) {
        foreign_log("export toplevel={} handle={:.8}… registered={}", rec ? rec->id.id() : 0ull,
                    handle, e->reg ? 1 : 0);
    }
}

void do_import(wl_client* client, wl_resource* manager, const wl_interface* iface,
               const void* impl, uint32_t id, const char* handle, bool v2) {
    auto* i = new (std::nothrow) imported_obj{};
    wl_resource* res =
        i ? wl_resource_create(client, iface, wl_resource_get_version(manager), id) : nullptr;
    if (!res) {
        delete i;
        wl_client_post_no_memory(client);
        return;
    }
    i->resource = res;
    i->v2 = v2;
    wl_resource_set_implementation(res, impl, i, imported_destroy_resource);
    const unsigned reg_id = registry() ? registry()->find(handle) : 0;
    const auto* reg = reg_id ? registry()->get(reg_id) : nullptr;
    auto* e = reg ? static_cast<exported_obj*>(reg->owner) : nullptr;
    if (e && e->reg == reg_id)
        link(e, i);
    else
        send_destroyed(i); // "If the handle is invalid, the destroyed event will be sent immediately"
    if (surface_debug()) {
        foreign_log("import handle={:.8}… result={}", handle ? handle : "",
                    i->exported ? "ok" : "destroyed");
    }
}

/// set_parent_of: `false` when `surface_res` is not an xdg_toplevel (v2 posts the error).
bool do_set_parent_of(imported_obj* i, wl_resource* surface_res) {
    auto* child = toplevel_of_surface(surface_res);
    if (!child)
        return false;
    exported_obj* e = i->exported;
    if (!e || !e->toplevel || !g_p0_ctx->wm || !g_p0_ctx->toplevels)
        return true; // inert: nothing to parent onto
    // A parent that is the child or one of its descendants would make a loop (the same rule
    // as xdg_toplevel.set_parent, which is an error there; here the request is ignored).
    for (lx::toplevel_id at = e->toplevel; at;) {
        if (at == child->id) {
            foreign_log("set_parent_of ignored: the window is the exported one or its ancestor");
            return true;
        }
        const auto* rec = g_p0_ctx->toplevels->find(at);
        at = rec ? rec->parent : lx::toplevel_id{};
    }
    g_p0_ctx->wm->set_parent(child->id, e->toplevel);
    bool known = false;
    for (unsigned c = 0; c < i->child_count; ++c)
        known = known || i->children[c] == child->id;
    if (!known) {
        if (i->child_count < k_import_children)
            i->children[i->child_count++] = child->id;
        else
            i->children[k_import_children - 1] = child->id; // the newest is remembered
    }
    if (surface_debug()) {
        foreign_log("set_parent_of child={} parent={}", child->id.id(), e->toplevel.id());
    }
    return true;
}

// ── v2 ──

#if defined(LUMEN_HAS_FOREIGN_V2)

const struct zxdg_exported_v2_interface exported_v2_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct zxdg_imported_v2_interface imported_v2_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_parent_of =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* surface) {
            auto* i = static_cast<imported_obj*>(wl_resource_get_user_data(r));
            if (i && !do_set_parent_of(i, surface))
                wl_resource_post_error(r, ZXDG_IMPORTED_V2_ERROR_INVALID_SURFACE,
                                       "set_parent_of a surface that is not an xdg_toplevel");
        },
};

const struct zxdg_exporter_v2_interface exporter_v2_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .export_toplevel =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface) {
            auto* rec = toplevel_of_surface(surface);
            if (!rec) {
                wl_resource_post_error(r, ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE,
                                       "export_toplevel of a surface that is not an xdg_toplevel");
                return;
            }
            do_export(client, r, &zxdg_exported_v2_interface, &exported_v2_impl, id, rec,
                      [](wl_resource* e, const char* h) { zxdg_exported_v2_send_handle(e, h); });
        },
};

const struct zxdg_importer_v2_interface importer_v2_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .import_toplevel =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id, const char* handle) {
            do_import(client, r, &zxdg_imported_v2_interface, &imported_v2_impl, id, handle, true);
        },
};

#endif

// ── v1 ──

#if defined(LUMEN_HAS_FOREIGN_V1)

const struct zxdg_exported_v1_interface exported_v1_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct zxdg_imported_v1_interface imported_v1_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_parent_of =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* surface) {
            if (auto* i = static_cast<imported_obj*>(wl_resource_get_user_data(r)))
                (void)do_set_parent_of(i, surface);
        },
};

const struct zxdg_exporter_v1_interface exporter_v1_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .export_ =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface) {
            do_export(client, r, &zxdg_exported_v1_interface, &exported_v1_impl, id,
                      toplevel_of_surface(surface),
                      [](wl_resource* e, const char* h) { zxdg_exported_v1_send_handle(e, h); });
        },
};

const struct zxdg_importer_v1_interface importer_v1_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .import =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id, const char* handle) {
            do_import(client, r, &zxdg_imported_v1_interface, &imported_v1_impl, id, handle,
                      false);
        },
};

#endif

} // namespace

#if defined(LUMEN_HAS_FOREIGN_V2)
void bind_exporter_v2(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_exporter_v2_interface, version, id);
    if (res)
        res.set_implementation(&exporter_v2_impl, g_p0_ctx, nullptr);
}
void bind_importer_v2(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_importer_v2_interface, version, id);
    if (res)
        res.set_implementation(&importer_v2_impl, g_p0_ctx, nullptr);
}
#endif
#if defined(LUMEN_HAS_FOREIGN_V1)
void bind_exporter_v1(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_exporter_v1_interface, version, id);
    if (res)
        res.set_implementation(&exporter_v1_impl, g_p0_ctx, nullptr);
}
void bind_importer_v1(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_importer_v1_interface, version, id);
    if (res)
        res.set_implementation(&importer_v1_impl, g_p0_ctx, nullptr);
}
#endif

void foreign_toplevel_destroyed(lx::toplevel_id id) {
    auto* reg = registry();
    if (!reg || !id || reg->count() == 0)
        return;
    for (unsigned r = 1; r <= lx::compositor::k_foreign_max_exports; ++r) {
        const auto* e = reg->get(r);
        if (e && e->toplevel == id && e->owner)
            retire_export(static_cast<exported_obj*>(e->owner));
    }
}

#else

void foreign_toplevel_destroyed(lx::toplevel_id) {}

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
