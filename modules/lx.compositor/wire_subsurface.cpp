module;

// wl_subcompositor / wl_subsurface. Part of the protocol wire split; shared declarations
// live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
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

namespace lx::compositor::p0 {

namespace {

subsurface_obj subsurfaces[k_max_surfaces]{};

// ── wl_subcompositor / wl_subsurface ──────────────────────────────────────
//
// Wire glue only: parentage, cached child state and paint order live in `surface_tree`,
// and the content-update choreography in `surface_commit` / `apply_cached`.

subsurface_obj* subsurface_from(wl_resource* r) {
    auto* o = static_cast<subsurface_obj*>(wl_resource_get_user_data(r));
    return o && o->used && o->resource == r ? o : nullptr;
}

/// The subsurface's surface while it still plays the role; null when inert.
surface_obj* subsurface_surface(wl_resource* r) {
    auto* o = subsurface_from(r);
    return o && o->surface && o->surface->ctx && o->surface->ctx->tree ? o->surface : nullptr;
}

void subsurface_log_sync(const surface_obj* s) {
    if (!surface_debug())
        return;
    const auto* tree = s->ctx->tree;
    surface_log("subsurface sync surface={} mode={} effective={}", s->id.id(),
                tree->sync_flag(s->id) ? "sync" : "desync",
                tree->effectively_sync(s->id) ? "sync" : "desync");
}

void subsurface_place(wl_resource* resource, wl_resource* sibling_res, bool above) {
    surface_obj* s = subsurface_surface(resource);
    surface_obj* sibling = surface_from(sibling_res);
    if (!s || !sibling)
        return;
    using restack = lx::compositor::surface_tree::restack_error;
    if (s->ctx->tree->place(s->id, sibling->id, above) == restack::bad_surface)
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "place_%s: the reference is neither the parent nor a sibling",
                               above ? "above" : "below");
}

void subsurface_destroy_resource(wl_resource* resource) {
    auto* o = subsurface_from(resource);
    if (!o)
        return;
    surface_obj* s = o->surface;
    *o = {};
    if (!s)
        return;
    s->sub = nullptr;
    // Immediate (wl_subsurface.destroy): the subtree is unmapped and the surface forgets its
    // position and z-order. It keeps the subsurface role; a new wl_subsurface may follow.
    if (auto* tree = s->ctx ? s->ctx->tree : nullptr)
        tree->unlink(s->id);
    // Without a parent nothing synchronizes it any more: content it had cached is applied
    // (buffers released, callbacks answered) — not drawn, since the surface is unmapped.
    if (client_dying(wl_resource_get_client(resource)))
        discard_cached(s);
    else
        flush_desync(s, "unlink");
}

const struct wl_subsurface_interface subsurface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_position =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
            // Double-buffered on the parent: applied when the parent's state is.
            if (surface_obj* s = subsurface_surface(r))
                s->ctx->tree->set_position_pending(s->id, {x, y});
        },
    .place_above =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* sibling) {
            subsurface_place(r, sibling, true);
        },
    .place_below =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* sibling) {
            subsurface_place(r, sibling, false);
        },
    .set_sync =
        [](struct wl_client*, struct wl_resource* r) {
            if (surface_obj* s = subsurface_surface(r)) {
                s->ctx->tree->set_sync(s->id, true);
                subsurface_log_sync(s);
            }
        },
    .set_desync =
        [](struct wl_client*, struct wl_resource* r) {
            surface_obj* s = subsurface_surface(r);
            if (!s)
                return;
            auto* tree = s->ctx->tree;
            const bool was_sync = tree->effectively_sync(s->id);
            tree->set_sync(s->id, false);
            subsurface_log_sync(s);
            // Effectively synchronized → desynchronized: its cached state, and that of
            // descendants it was holding back, applies now (wlroots unlocks it the same way).
            if (was_sync && !tree->effectively_sync(s->id))
                flush_desync(s, "set-desync");
        },
};

void subcompositor_get_subsurface(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t id, struct wl_resource* surface_res,
                                  struct wl_resource* parent_res) {
    surface_obj* s = surface_from(surface_res);
    surface_obj* p = surface_from(parent_res);
    if (!s || !p || !s->ctx || !s->ctx->tree)
        return;
    // "The to-be sub-surface must not already have another role, and it must not have an
    // existing wl_subsurface object."
    if ((s->role != surface_role::none && s->role != surface_role::subsurface) || s->sub) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u already has a role or a wl_subsurface",
                               wl_resource_get_id(surface_res));
        return;
    }
    subsurface_obj* o = nullptr;
    for (auto& slot : subsurfaces)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    using link = lx::compositor::surface_tree::link_error;
    switch (s->ctx->tree->link(s->id, p->id)) {
    case link::none:
        break;
    case link::bad_surface:
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u cannot be a sub-surface",
                               wl_resource_get_id(surface_res));
        return;
    case link::bad_parent:
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                               "parent wl_surface@%u is the surface itself or one of its "
                               "descendants",
                               wl_resource_get_id(parent_res));
        return;
    case link::no_memory:
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res = wl_resource_create(client, &wl_subsurface_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        s->ctx->tree->unlink(s->id);
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->surface = s;
    s->sub = o;
    s->role = surface_role::subsurface;
    wl_resource_set_implementation(res, &subsurface_impl, o, subsurface_destroy_resource);
    if (surface_debug()) {
        surface_log("subsurface create surface={} parent={} root={}", s->id.id(), p->id.id(),
                    s->ctx->tree->root_of(s->id).id());
    }
}

const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .get_subsurface = subcompositor_get_subsurface,
};

} // namespace

void bind_subcompositor(wayland::client_connection& client, unsigned id, int version) {
    auto res = wayland::resource::create(client, &wl_subcompositor_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&subcompositor_impl, nullptr, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
