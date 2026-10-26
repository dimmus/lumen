module;

// xdg_wm_base, xdg_surface, xdg_toplevel and xdg_positioner, and the window manager's
// wire ops (configure, close, serials). Part of the protocol wire split; shared
// declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.runtime;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import lx.input;
import :surface;
import :surface_tree;
import :toplevel;
import :output;
import :wire;

namespace {

constexpr lx::trace::category log_liveness{"compositor.liveness"};
constexpr lx::trace::category log_wm{"compositor.wm"};

} // namespace

#if defined(LUMEN_HAS_WAYLAND)
#if defined(LUMEN_HAS_PROTOCOL_GLUE)

namespace lx::compositor::p0 {

namespace {

xdg_surface_obj xdg_surfaces[256]{};
xdg_toplevel_obj xdg_toplevels[256]{};
xdg_wm_base_obj xdg_wm_bases[128]{};
positioner_obj positioners[128]{};

} // namespace

/// The xdg_wm_base an xdg_surface came from, where popup errors are posted; null only for
/// a client being torn down.
wl_resource* wm_base_resource(const xdg_surface_obj* xs) {
    return xs && xs->wm_base ? xs->wm_base->resource : nullptr;
}

namespace {

// ── xdg_toplevel, xdg_surface ──────────────────────────────────────────────

void xdg_toplevel_destroy_resource(struct wl_resource* resource) {
    auto* t = static_cast<xdg_toplevel_obj*>(wl_resource_get_user_data(resource));
    if (!t || !t->used || t->resource != resource)
        return;
    // Destroying the role object unmaps the window and ends the record (xdg-shell).
    if (t->id && t->ctx && t->ctx->wm)
        t->ctx->wm->on_destroyed(t->id);
    // Only at client teardown (the request is `orphaned` otherwise): the decoration object
    // goes inert.
    if (t->deco)
        t->deco->toplevel = nullptr;
    if (t->xdg_surface) {
        t->xdg_surface->toplevel = nullptr;
        if (t->xdg_surface->surface)
            t->xdg_surface->surface->toplevel = {};
    }
    *t = {};
}

void xdg_surface_destroy_resource(struct wl_resource* resource) {
    auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
    if (!xs || !xs->used || xs->resource != resource)
        return;
    // Only reachable with a live toplevel or popup at client disconnect (the destroy
    // request rejects it otherwise). The record cannot outlive its xdg_surface.
    if (xs->toplevel) {
        if (xs->toplevel->id && xs->ctx && xs->ctx->wm)
            xs->ctx->wm->on_destroyed(xs->toplevel->id);
        xs->toplevel->id = {};
        xs->toplevel->xdg_surface = nullptr;
    }
    if (xs->popup) {
        if (xs->popup->surface && xs->ctx && xs->ctx->popups)
            xs->ctx->popups->destroy(xs->popup->surface);
        xs->popup->surface = {};
        xs->popup->xdg_surface = nullptr;
    }
    if (xs->surface) {
        xs->surface->xdg = nullptr;
        xs->surface->toplevel = {};
    }
    if (xs->wm_base && xs->wm_base->surfaces > 0)
        --xs->wm_base->surfaces;
    *xs = {};
}

} // namespace

xdg_toplevel_obj* toplevel_of(wl_resource* resource) {
    auto* t = static_cast<xdg_toplevel_obj*>(wl_resource_get_user_data(resource));
    return t && t->used ? t : nullptr;
}

lx::compositor::toplevel_record* record_of(xdg_toplevel_obj* t) {
    if (!t || !t->id || !t->ctx || !t->ctx->toplevels)
        return nullptr;
    return t->ctx->toplevels->find_mut(t->id);
}

namespace {

/// set_min_size / set_max_size: double-buffered, applied on the next wl_surface.commit.
void toplevel_set_size_limit(wl_resource* resource, int32_t width, int32_t height, bool max) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "negative size limit");
        return;
    }
    auto* rec = record_of(toplevel_of(resource));
    if (!rec)
        return;
    if (max) {
        rec->max_pending = {width, height};
        rec->max_pending_set = true;
    } else {
        rec->min_pending = {width, height};
        rec->min_pending_set = true;
    }
}

const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy =
        [](struct wl_client* client, struct wl_resource* resource) {
            // "The xdg_toplevel_decoration object must be destroyed before its xdg_toplevel."
            if (auto* t = toplevel_of(resource);
                t && t->deco && t->deco->resource && !client_dying(client)) {
                wl_resource_post_error(t->deco->resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
                                       "xdg_toplevel destroyed before its decoration object");
                return;
            }
            wl_resource_destroy(resource);
        },
    .set_parent =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* parent_res) {
            auto* t = toplevel_of(resource);
            if (!t || !t->id || !t->ctx || !t->ctx->wm || !t->ctx->toplevels)
                return;
            lx::toplevel_id parent{};
            if (parent_res) {
                auto* p = toplevel_of(parent_res);
                parent = p ? p->id : lx::toplevel_id{};
                // "The parent toplevel must not be one of the child toplevel's descendants,
                // and the parent must be different from the child toplevel."
                for (lx::toplevel_id at = parent; at; ) {
                    if (at == t->id) {
                        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
                                               "the parent is the toplevel or a descendant of it");
                        return;
                    }
                    const auto* rec = t->ctx->toplevels->find(at);
                    at = rec ? rec->parent : lx::toplevel_id{};
                }
            }
            t->ctx->wm->set_parent(t->id, parent);
        },
    .set_title =
        [](struct wl_client*, struct wl_resource* resource, const char* title) {
            auto* t = toplevel_of(resource);
            if (!t)
                return;
            std::snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
            if (t->id && t->ctx && t->ctx->toplevels) {
                t->ctx->toplevels->on_xdg_title(t->id, t->title);
                if (t->ctx->wm)
                    t->ctx->wm->on_title_changed(t->id);
                if (auto* rec = record_of(t); rec && rec->mapped && t->ctx->shell)
                    t->ctx->shell->emit_toplevel_state(*rec);
            }
        },
    .set_app_id =
        [](struct wl_client*, struct wl_resource* resource, const char* app_id) {
            auto* t = toplevel_of(resource);
            if (!t)
                return;
            std::snprintf(t->app_id, sizeof(t->app_id), "%s", app_id ? app_id : "");
            if (t->id && t->ctx && t->ctx->toplevels) {
                t->ctx->toplevels->on_xdg_app_id(t->id, t->app_id);
                if (auto* rec = record_of(t); rec && rec->mapped && t->ctx->shell)
                    t->ctx->shell->emit_toplevel_state(*rec);
            }
        },
    .show_window_menu = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                           uint32_t, int32_t, int32_t) {},
    .move =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial) {
            auto* t = toplevel_of(resource);
            if (!t || !t->id || !t->ctx || !t->ctx->wm)
                return;
            (void)t->ctx->wm->request_client_grab(t->id, client, serial,
                                                  lx::compositor::grab_mode::move, 0);
        },
    .resize =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial, uint32_t edges) {
            if (!lx::compositor::valid_resize_edges(edges)) {
                wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
                                       "invalid resize edge %u", edges);
                return;
            }
            auto* t = toplevel_of(resource);
            if (!t || !t->id || !t->ctx || !t->ctx->wm)
                return;
            (void)t->ctx->wm->request_client_grab(t->id, client, serial,
                                                  lx::compositor::grab_mode::resize, edges);
        },
    .set_max_size =
        [](struct wl_client*, struct wl_resource* resource, int32_t width, int32_t height) {
            toplevel_set_size_limit(resource, width, height, true);
        },
    .set_min_size =
        [](struct wl_client*, struct wl_resource* resource, int32_t width, int32_t height) {
            toplevel_set_size_limit(resource, width, height, false);
        },
    .set_maximized =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_maximized(t->id, true, "client");
        },
    .unset_maximized =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_maximized(t->id, false, "client");
        },
    .set_fullscreen =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* output) {
            // The output asked for (null: the window's own); one whose output is gone is
            // the window's own too.
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_fullscreen(t->id, true, output ? output_resource_slot(output) : -1,
                                           "client");
        },
    .unset_fullscreen =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_fullscreen(t->id, false, -1, "client");
        },
    // Hidden until activated (the panel, the switcher, xdg-activation); it keeps its
    // workspace (design-session-and-wm W.3).
    .set_minimized =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_minimized(t->id, true, "client");
        },
};

} // namespace

xdg_surface_obj* xdg_surface_from(wl_resource* resource) {
    auto* xs = resource ? static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource))
                        : nullptr;
    return xs && xs->used && xs->resource == resource ? xs : nullptr;
}

positioner_obj* positioner_from(wl_resource* resource) {
    auto* p = resource ? static_cast<positioner_obj*>(wl_resource_get_user_data(resource))
                       : nullptr;
    return p && p->used && p->resource == resource ? p : nullptr;
}

xdg_popup_obj* popup_from(wl_resource* resource) {
    auto* o = resource ? static_cast<xdg_popup_obj*>(wl_resource_get_user_data(resource))
                       : nullptr;
    return o && o->used && o->resource == resource ? o : nullptr;
}

namespace {

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (xs && (xs->toplevel || xs->popup)) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                                       "xdg_surface destroyed before its role object");
                return;
            }
            wl_resource_destroy(resource);
        },
    .get_toplevel =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs || !xs->used || !xs->ctx)
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
            // A surface keeps its role for life: once a toplevel, only ever a toplevel.
            if (surf->role != surface_role::none && surf->role != surface_role::xdg_toplevel) {
                if (wl_resource* wm = wm_base_resource(xs))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_ROLE,
                                           "wl_surface already has another role");
                return;
            }
            xdg_toplevel_obj* top = nullptr;
            for (auto& t : xdg_toplevels)
                if (!t.used) {
                    top = &t;
                    break;
                }
            if (!top) {
                wl_client_post_no_memory(client);
                return;
            }
            *top = {};
            top->used = true;
            top->ctx = xs->ctx;
            top->xdg_surface = xs;
            top->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                               wl_resource_get_version(resource), id);
            if (!top->resource) {
                top->used = false;
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(top->resource, &xdg_toplevel_impl, top,
                                           xdg_toplevel_destroy_resource);
            xs->toplevel = top;
            surf->role = surface_role::xdg_toplevel;

            // No configure and no focus here: the client still has to make its initial
            // commit, and the first configure answers that (xdg-shell). Focus is taken when
            // the window maps, not when it is merely announced.
            if (xs->ctx->toplevels) {
                auto created = xs->ctx->toplevels->create(surf->client, surf->id,
                                                          surf->resource, xs->resource,
                                                          top->resource);
                if (!created) {
                    wl_client_post_no_memory(client);
                    return;
                }
                top->id = created.value();
                surf->toplevel = top->id;
            }
            // The surface roots a tree: its subsurfaces are drawn in its container.
            if (xs->ctx->tree && !xs->ctx->tree->make_root(surf->id,
                                                           lx::compositor::root_kind::toplevel)) {
                wl_client_post_no_memory(client);
                return;
            }
        },
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry =
        [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t width,
           int32_t height) {
            if (width <= 0 || height <= 0) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                                       "window geometry must have a positive size");
                return;
            }
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs)
                return;
            if (xs->popup && xs->popup->surface && xs->ctx && xs->ctx->popups) {
                if (auto* rec = xs->ctx->popups->find(xs->popup->surface)) {
                    rec->window_geometry_pending = {x, y, width, height};
                    rec->window_geometry_pending_set = true;
                }
                return;
            }
            if (!xs->toplevel)
                return;
            if (auto* rec = record_of(xs->toplevel)) {
                rec->window_geometry_pending = {x, y, width, height};
                rec->window_geometry_pending_set = true;
            }
        },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (xs && xs->popup) {
                auto* popups = xs->ctx ? xs->ctx->popups : nullptr;
                if (!popups || !xs->popup->surface)
                    return; // inert: the wl_surface is gone
                using ack = lx::compositor::popup_manager::ack_result;
                if (popups->on_ack(xs->popup->surface, serial) == ack::invalid)
                    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                                           "ack_configure with a serial never sent");
                return;
            }
            if (!xs || !xs->toplevel) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                       "ack_configure on an xdg_surface without a role");
                return;
            }
            auto* rec = record_of(xs->toplevel);
            if (!rec)
                return;
            using ack = lx::compositor::toplevel_manager::ack_result;
            const ack result = lx::compositor::toplevel_manager::on_ack(*rec, serial);
            if (result == ack::invalid) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                                       "ack_configure with a serial never sent");
                return;
            }
            if (result == ack::ok && log_wm.enabled(lx::trace::level::debug)) {
                log_wm.debug("ack id={} serial={}", rec->id.id(), serial);
            }
        },
};

// ── xdg_positioner ─────────────────────────────────────────────────────────

positioner_obj* positioner_rules_of(wl_resource* resource) { return positioner_from(resource); }

const struct xdg_positioner_interface positioner_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_size =
        [](struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
            if (w < 1 || h < 1) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                                       "set_size: width and height must be positive");
                return;
            }
            if (auto* p = positioner_rules_of(r)) {
                p->rules.size = {w, h};
                p->rules.size_set = true;
            }
        },
    .set_anchor_rect =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            if (w < 0 || h < 0) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                                       "set_anchor_rect: negative width or height");
                return;
            }
            if (auto* p = positioner_rules_of(r)) {
                p->rules.anchor_rect = {x, y, w, h};
                p->rules.anchor_rect_set = true;
            }
        },
    .set_anchor =
        [](struct wl_client*, struct wl_resource* r, uint32_t anchor) {
            if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor");
                return;
            }
            if (auto* p = positioner_rules_of(r))
                p->rules.anchor = anchor;
        },
    .set_gravity =
        [](struct wl_client*, struct wl_resource* r, uint32_t gravity) {
            if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity");
                return;
            }
            if (auto* p = positioner_rules_of(r))
                p->rules.gravity = gravity;
        },
    .set_constraint_adjustment =
        [](struct wl_client*, struct wl_resource* r, uint32_t adjustment) {
            // Unknown bits carry no meaning and the spec defines no error for them.
            if (auto* p = positioner_rules_of(r))
                p->rules.adjustment = adjustment & 63u;
        },
    .set_offset =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
            if (auto* p = positioner_rules_of(r))
                p->rules.offset = {x, y};
        },
    .set_reactive =
        [](struct wl_client*, struct wl_resource* r) {
            if (auto* p = positioner_rules_of(r))
                p->rules.reactive = true;
        },
    .set_parent_size =
        [](struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
            if (auto* p = positioner_rules_of(r)) {
                p->rules.parent_size = {w, h};
                p->rules.parent_size_set = true;
            }
        },
    .set_parent_configure =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            if (auto* p = positioner_rules_of(r)) {
                p->rules.parent_configure = serial;
                p->rules.parent_configure_set = true;
            }
        },
};

// ── xdg_wm_base ────────────────────────────────────────────────────────────

/// The clock the frame ticks use (std::chrono::steady_clock), so liveness deadlines compare.
lx::runtime::clock_time steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

xdg_wm_base_obj* wm_base_from(wl_resource* r) {
    auto* o = static_cast<xdg_wm_base_obj*>(wl_resource_get_user_data(r));
    return o && o->used && o->resource == r ? o : nullptr;
}

void wm_base_destroy_resource(wl_resource* r) {
    auto* o = wm_base_from(r);
    if (!o)
        return;
    // A ping sent on this object can no longer be answered. Not at client teardown: the
    // client's liveness entry is forgotten with it.
    if (o->ping_serial != 0 && o->ctx && o->ctx->liveness &&
        !client_dying(wl_resource_get_client(r)))
        o->ctx->liveness->cancel(o->client, o->ping_serial, steady_now_ns());
    // Only with surfaces alive at client teardown: they forget where to post errors.
    if (o->surfaces > 0)
        for (auto& xs : xdg_surfaces)
            if (xs.used && xs.wm_base == o)
                xs.wm_base = nullptr;
    *o = {};
}

const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            if (const auto* o = wm_base_from(resource); o && o->surfaces > 0) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                                       "xdg_wm_base destroyed with xdg_surfaces alive");
                return;
            }
            wl_resource_destroy(resource);
        },
    .create_positioner =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            positioner_obj* p = nullptr;
            for (auto& slot : positioners)
                if (!slot.used) {
                    p = &slot;
                    break;
                }
            if (!p) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource* res = wl_resource_create(client, &xdg_positioner_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *p = {};
            p->used = true;
            p->resource = res;
            wl_resource_set_implementation(res, &positioner_impl, p, [](wl_resource* r) {
                if (auto* o = positioner_from(r))
                    *o = {};
            });
        },
    .get_xdg_surface =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* surface_res) {
            auto* base = wm_base_from(resource);
            if (!base)
                return;
            surface_obj* surf = surface_from(surface_res);
            if (!surf) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                                       "unknown surface");
                return;
            }
            // One xdg_surface per wl_surface, and none on a surface with a non-xdg role
            // (a cursor, a subsurface) — the same rule wlroots applies.
            if (surf->xdg || (surf->role != surface_role::none &&
                              surf->role != surface_role::xdg_toplevel &&
                              surf->role != surface_role::popup)) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                                       "wl_surface already has a role");
                return;
            }
            xdg_surface_obj* xs = nullptr;
            for (auto& s : xdg_surfaces)
                if (!s.used) {
                    xs = &s;
                    break;
                }
            if (!xs) {
                wl_client_post_no_memory(client);
                return;
            }
            *xs = {};
            xs->used = true;
            xs->ctx = base->ctx;
            xs->surface = surf;
            xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                              wl_resource_get_version(resource), id);
            if (!xs->resource) {
                xs->used = false;
                wl_client_post_no_memory(client);
                return;
            }
            xs->wm_base = base;
            ++base->surfaces;
            surf->xdg = xs;
            wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                           xdg_surface_destroy_resource);
        },
    .pong =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial) {
            // A pong for no outstanding ping, or with another serial, is ignored: the
            // protocol defines no error for it.
            const auto* base = wm_base_from(resource);
            if (base && base->ctx && base->ctx->liveness)
                base->ctx->liveness->on_pong(base->client, serial, steady_now_ns());
        },
};

} // namespace

// ── window manager wire ops ────────────────────────────────────────────────

void wm_send_configure(void* toplevel_res, void* xdg_surface_res,
                       const lx::compositor::toplevel_configure& c, lx::size2i bounds,
                       bool initial, void* decoration_res) {
    auto* tl = static_cast<wl_resource*>(toplevel_res);
    auto* xs = static_cast<wl_resource*>(xdg_surface_res);
    if (!tl || !xs)
        return;
    const int version = wl_resource_get_version(tl);

    // A wl_array over stack storage, never released: wl_array_add would reallocate per
    // configure, and a resize sends one per frame.
    uint32_t storage[12];
    wl_array array{};
    array.data = storage;
    array.alloc = sizeof(storage);
    array.size = 0;

    // Maximize, fullscreen and minimize: Lumen has no window menu yet, and a CSD client
    // hides the buttons it is told the compositor does not honour.
    if (initial && version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
        storage[0] = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
        storage[1] = XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
        storage[2] = XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
        array.size = 3 * sizeof(uint32_t);
        xdg_toplevel_send_wm_capabilities(tl, &array);
        array.size = 0;
    }
    if (bounds.width > 0 && bounds.height > 0 &&
        version >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
        xdg_toplevel_send_configure_bounds(tl, bounds.width, bounds.height);

    unsigned n = 0;
    if (c.states & lx::compositor::tl_maximized)
        storage[n++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (c.states & lx::compositor::tl_fullscreen)
        storage[n++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (c.states & lx::compositor::tl_resizing)
        storage[n++] = XDG_TOPLEVEL_STATE_RESIZING;
    if (c.states & lx::compositor::tl_activated)
        storage[n++] = XDG_TOPLEVEL_STATE_ACTIVATED;
    if ((c.states & lx::compositor::tl_suspended) &&
        version >= XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
        storage[n++] = XDG_TOPLEVEL_STATE_SUSPENDED;
    // A snapped window (design-session-and-wm K.3): the tiled edges, xdg_wm_base v2+.
    if ((c.states & lx::compositor::tl_tiled_all) &&
        version >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
        if (c.states & lx::compositor::tl_tiled_left)
            storage[n++] = XDG_TOPLEVEL_STATE_TILED_LEFT;
        if (c.states & lx::compositor::tl_tiled_right)
            storage[n++] = XDG_TOPLEVEL_STATE_TILED_RIGHT;
        if (c.states & lx::compositor::tl_tiled_top)
            storage[n++] = XDG_TOPLEVEL_STATE_TILED_TOP;
        if (c.states & lx::compositor::tl_tiled_bottom)
            storage[n++] = XDG_TOPLEVEL_STATE_TILED_BOTTOM;
    }
    array.size = n * sizeof(uint32_t);
    xdg_toplevel_send_configure(tl, c.size.width, c.size.height, &array);
    if (decoration_res)
        zxdg_toplevel_decoration_v1_send_configure(
            static_cast<wl_resource*>(decoration_res),
            c.decoration == lx::compositor::decoration_mode::server
                ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    xdg_surface_send_configure(xs, c.serial);
}

void wm_send_close(void* toplevel_res) {
    if (toplevel_res)
        xdg_toplevel_send_close(static_cast<wl_resource*>(toplevel_res));
}

unsigned wm_next_serial(void* user) {
    // The seat's counter, not wl_display_next_serial: every input event already draws from
    // it, and one serial space is what lets a move/resize request's serial be checked
    // against the press that allowed it without colliding with a configure serial.
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (ctx && ctx->seat)
        return ctx->seat->next_serial();
    if (ctx && ctx->server && ctx->server->native_display())
        return wl_display_next_serial(static_cast<wl_display*>(ctx->server->native_display()));
    return 0;
}

void bind_xdg_wm_base(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    xdg_wm_base_obj* o = nullptr;
    for (auto& slot : xdg_wm_bases)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o)
        return;
    auto res = wayland::resource::create(client, &xdg_wm_base_interface, version, id);
    if (!res)
        return;
    *o = {};
    o->used = true;
    o->ctx = g_p0_ctx;
    o->client = lx::client_id{client.id()};
    o->resource = static_cast<wl_resource*>(res.native());
    wl_resource_set_implementation(o->resource, &xdg_wm_base_impl, o, wm_base_destroy_resource);
}

namespace {

// ── liveness wire ops ──────────────────────────────────────────────────────

/// One of the client's xdg_wm_base objects — one that created surfaces if there is one, as
/// that is the one its toolkit dispatches. False when it has none.
bool liveness_send_ping(void*, lx::client_id client, unsigned serial) {
    xdg_wm_base_obj* pick = nullptr;
    for (auto& b : xdg_wm_bases) {
        if (!b.used || !(b.client == client))
            continue;
        if (!pick || (pick->surfaces == 0 && b.surfaces > 0))
            pick = &b;
    }
    if (!pick || client_dying(wl_resource_get_client(pick->resource)))
        return false;
    xdg_wm_base_send_ping(pick->resource, serial);
    pick->ping_serial = serial;
    return true;
}

unsigned liveness_next_serial(void* user) { return wm_next_serial(user); }

/// Not responding (or responsive again): the window manager marks the toplevels (title
/// suffix, dim, the shell's flag) and the router withholds (or resumes) pointer motion.
/// Shown, never acted on: the client is not killed.
void liveness_changed(void* user, lx::client_id client, bool on, unsigned ms) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (!ctx)
        return;
    wayland::client_connection* conn =
        ctx->server ? ctx->server->find_client(static_cast<unsigned>(client.id())) : nullptr;
    void* native = conn ? conn->native() : nullptr;
    const unsigned pid = conn ? conn->credentials().pid : 0u;
    const unsigned toplevels = ctx->wm ? ctx->wm->set_client_not_responding(client, on) : 0u;
    if (ctx->input && native)
        ctx->input->set_motion_muted(native, on,
                                     static_cast<unsigned>(steady_now_ns() / 1'000'000));
    if (on)
        log_liveness.info("not-responding client={} pid={} after_ms={} toplevels={}", client.id(),
                          pid, ms, toplevels);
    else
        log_liveness.info("responsive client={} pid={} was_ms={}", client.id(), pid, ms);
}

/// The router's user-input hook: a key or button press reached `client` — ping it if it is
/// due (Mutter pings on click).
void liveness_user_input(void* user, void* client, unsigned,
                         lx::compositor::input_serial_kind kind) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (!ctx || !ctx->liveness)
        return;
    const unsigned id = wire_client_id(static_cast<wl_client*>(client));
    if (!id)
        return;
    ctx->liveness->request(lx::client_id{id}, steady_now_ns(),
                           kind == lx::compositor::input_serial_kind::key_press
                               ? lx::compositor::ping_reason::key
                               : lx::compositor::ping_reason::press);
}

} // namespace

void install_liveness(p0_protocol_context& ctx) {
    if (!ctx.liveness)
        return;
    lx::compositor::liveness_ops ops{};
    ops.send_ping = &liveness_send_ping;
    ops.next_serial = &liveness_next_serial;
    ops.changed = &liveness_changed;
    ops.user = &ctx;
    ctx.liveness->set_ops(ops);
    if (ctx.input)
        ctx.input->set_user_input_hook(&liveness_user_input, &ctx);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
