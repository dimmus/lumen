module;

// xdg_popup and xdg_surface.get_popup, and the popup manager's wire ops. Part of the
// protocol wire split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#endif
#endif

module lx.compositor;

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

xdg_popup_obj xdg_popups[lx::compositor::popup_manager::k_max]{};

// ── xdg_popup ──────────────────────────────────────────────────────────────

void xdg_popup_destroy_resource(struct wl_resource* resource) {
    auto* o = popup_from(resource);
    if (!o)
        return;
    // Unmapped, off the grab stack (the grab returns to the parent popup if it had one),
    // keyboard focus re-resolved — all inside the popup manager.
    if (o->surface && o->ctx && o->ctx->popups)
        o->ctx->popups->destroy(o->surface);
    if (o->xdg_surface)
        o->xdg_surface->popup = nullptr;
    *o = {};
}

const struct xdg_popup_interface xdg_popup_impl = {
    .destroy =
        [](struct wl_client* client, struct wl_resource* resource) {
            // "The only popup you are allowed to destroy at all times is the topmost one."
            auto* o = popup_from(resource);
            if (o && o->surface && o->ctx && o->ctx->popups &&
                o->ctx->popups->has_live_children(o->surface) && !client_dying(client)) {
                if (wl_resource* wm = wm_base_resource(o->xdg_surface)) {
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                                           "xdg_popup destroyed while it has child popups");
                    return;
                }
            }
            wl_resource_destroy(resource);
        },
    .grab =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial) {
            auto* o = popup_from(resource);
            if (!o || !o->surface || !o->ctx || !o->ctx->wm)
                return;
            using grab = lx::compositor::popup_manager::grab_result;
            switch (o->ctx->wm->request_popup_grab(o->surface, client, serial)) {
            case grab::invalid_grab:
                wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB,
                                       "grab requested on a mapped popup");
                break;
            case grab::invalid_parent:
                // The spec says "an error will be raised" and names none; this is the
                // one that describes it (recorded in docs/architecture.md).
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                           "grabbing popup whose parent popup took no grab");
                break;
            case grab::not_topmost:
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                                           "grabbing popup whose parent is not the topmost "
                                           "grabbing popup");
                break;
            case grab::ok:
            case grab::denied:
                break;
            }
        },
    .reposition =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* positioner,
           uint32_t token) {
            auto* o = popup_from(resource);
            if (!o)
                return;
            const positioner_obj* p = positioner_from(positioner);
            if (!p || !p->rules.complete()) {
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                                           "reposition with an incomplete xdg_positioner");
                return;
            }
            if (o->surface && o->ctx && o->ctx->popups)
                o->ctx->popups->reposition(o->surface, p->rules, token);
        },
};

} // namespace

void popup_send_configure(void* popup_res, void* xdg_surface_res, lx::rect2i rel,
                          unsigned serial, bool repositioned, unsigned token) {
    auto* p = static_cast<wl_resource*>(popup_res);
    auto* xs = static_cast<wl_resource*>(xdg_surface_res);
    if (!p || !xs)
        return;
    if (repositioned && wl_resource_get_version(p) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
        xdg_popup_send_repositioned(p, token);
    xdg_popup_send_configure(p, rel.x, rel.y, rel.width, rel.height);
    xdg_surface_send_configure(xs, serial);
}

void popup_send_done(void* popup_res) {
    if (popup_res)
        xdg_popup_send_popup_done(static_cast<wl_resource*>(popup_res));
}

bool popup_client_alive(void* client) { return !client_dying(static_cast<wl_client*>(client)); }

/// xdg_surface.get_popup.
void xdg_surface_get_popup(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                           struct wl_resource* parent_res, struct wl_resource* positioner_res) {
    auto* xs = xdg_surface_from(resource);
    if (!xs || !xs->ctx || !xs->ctx->popups)
        return;
    if (xs->toplevel || xs->popup) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface already has a role object");
        return;
    }
    surface_obj* surf = xs->surface;
    if (!surf) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "the wl_surface is gone");
        return;
    }
    wl_resource* wm = wm_base_resource(xs);
    if (surf->role != surface_role::none && surf->role != surface_role::popup) {
        if (wm)
            wl_resource_post_error(wm, XDG_WM_BASE_ERROR_ROLE, "wl_surface already has a role");
        return;
    }
    const positioner_obj* pos = positioner_from(positioner_res);
    if (!pos || !pos->rules.complete()) {
        if (wm)
            wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                                   "get_popup with an incomplete xdg_positioner");
        return;
    }
    using kind = lx::compositor::popup_parent_kind;
    kind parent_kind = kind::none;
    lx::surface_id parent{};
    lx::toplevel_id owner{};
    if (parent_res) {
        // The parent is an xdg_surface with the toplevel or popup role, and not this one.
        const xdg_surface_obj* px = xdg_surface_from(parent_res);
        if (px && px != xs && px->surface && px->toplevel && px->toplevel->id) {
            parent_kind = kind::toplevel;
            parent = px->surface->id;
            owner = px->toplevel->id;
        } else if (px && px != xs && px->surface && px->popup && px->popup->surface) {
            parent_kind = kind::popup;
            parent = px->surface->id;
            if (const auto* pr = xs->ctx->popups->find(parent))
                owner = pr->owner;
        } else {
            if (wm)
                wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                       "popup parent is not an xdg_toplevel or xdg_popup");
            return;
        }
    }
    xdg_popup_obj* o = nullptr;
    for (auto& slot : xdg_popups)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res =
        wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->ctx = xs->ctx;
    o->xdg_surface = xs;
    o->surface = surf->id;
    wl_resource_set_implementation(res, &xdg_popup_impl, o, xdg_popup_destroy_resource);
    xs->popup = o;
    surf->role = surface_role::popup;
    using created = lx::compositor::popup_manager::create_error;
    if (xs->ctx->popups->create(surf->id, surf->client, client, surf->resource, xs->resource,
                                res, parent_kind, parent, owner, pos->rules) != created::none) {
        o->surface = {};
        wl_client_post_no_memory(client);
    }
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
