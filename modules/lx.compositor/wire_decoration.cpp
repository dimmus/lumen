module;

// xdg-decoration (zxdg_decoration_manager_v1, zxdg_toplevel_decoration_v1). Part of the
// protocol wire split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
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

decoration_obj toplevel_decorations[256]{};

// ── xdg-decoration ─────────────────────────────────────────────────────────

decoration_obj* decoration_from(wl_resource* r) {
    auto* d = r ? static_cast<decoration_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return d && d->used && d->resource == r ? d : nullptr;
}

void deco_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
}

void decoration_destroy_resource(wl_resource* r) {
    auto* d = decoration_from(r);
    if (!d)
        return;
    if (d->toplevel) {
        d->toplevel->deco = nullptr;
        if (auto* rec = record_of(d->toplevel)) {
            // "Switch back to a mode without any server-side decorations at the next
            // commit": nothing to send it on, so nothing is configured. Every commit while
            // no decoration object exists applies client-side (window_manager), whatever
            // the configure it answers carried, and no later configure says server-side.
            rec->deco_resource = nullptr;
            rec->deco_requested = lx::compositor::decoration_request::unset;
            rec->deco_configure_due = false;
            rec->deco_awaiting_configure = false;
            rec->desired.decoration = lx::compositor::decoration_mode::client;
            rec->last_sent.decoration = lx::compositor::decoration_mode::client;
            if (surface_debug()) {
                char line[64];
                std::snprintf(line, sizeof(line), "decoration destroy id=%llu", rec->id.id());
                deco_log(line);
            }
        }
    }
    *d = {};
}

/// set_mode / unset_mode: what the client prefers; the policy answers with a configure.
void decoration_set_request(wl_resource* resource, lx::compositor::decoration_request req) {
    auto* d = decoration_from(resource);
    auto* rec = d && d->toplevel ? record_of(d->toplevel) : nullptr;
    if (!rec)
        return; // inert
    rec->deco_requested = req;
    if (surface_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "decoration request id=%llu mode=%s", rec->id.id(),
                      req == lx::compositor::decoration_request::server   ? "server"
                      : req == lx::compositor::decoration_request::client ? "client"
                                                                          : "unset");
        deco_log(line);
    }
    if (d->ctx && d->ctx->wm)
        d->ctx->wm->on_decoration_request(rec->id, false);
}

const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_mode =
        [](struct wl_client*, struct wl_resource* r, uint32_t mode) {
            if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
                mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
                wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
                                       "invalid decoration mode %u", mode);
                return;
            }
            decoration_set_request(r, mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                      ? lx::compositor::decoration_request::server
                                      : lx::compositor::decoration_request::client);
        },
    .unset_mode =
        [](struct wl_client*, struct wl_resource* r) {
            decoration_set_request(r, lx::compositor::decoration_request::unset);
        },
};

const struct zxdg_decoration_manager_v1_interface decoration_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_toplevel_decoration =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* toplevel_res) {
            const int version = wl_resource_get_version(resource);
            decoration_obj* d = nullptr;
            for (auto& slot : toplevel_decorations)
                if (!slot.used) {
                    d = &slot;
                    break;
                }
            wl_resource* res =
                d ? wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, version, id)
                  : nullptr;
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *d = {};
            d->used = true;
            d->resource = res;
            d->ctx = g_p0_ctx;
            wl_resource_set_implementation(res, &decoration_impl, d, decoration_destroy_resource);

            // Errors are posted on the new object: the error enum is
            // zxdg_toplevel_decoration_v1's, so that is the interface a client decodes the
            // code against.
            xdg_toplevel_obj* t = toplevel_of(toplevel_res);
            if (t && t->deco) {
                wl_resource_post_error(res, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                                       "xdg_toplevel already has a decoration object");
                return;
            }
            auto* rec = record_of(t);
            if (!t || !rec)
                return; // inert: the toplevel's record is gone
            const surface_obj* surf = t->xdg_surface ? t->xdg_surface->surface : nullptr;
            const bool has_buffer =
                surf && (surf->has_content || (surf->pending.buffer_set && surf->pending.buffer) ||
                         (surf->cached.buffer_set && surf->cached.buffer));
            if (version < 2 && has_buffer) {
                wl_resource_post_error(res, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                                       "xdg_toplevel has a buffer attached or committed");
                return;
            }
            d->toplevel = t;
            t->deco = d;
            rec->deco_resource = res;
            rec->deco_version = static_cast<unsigned>(version);
            rec->deco_requested = lx::compositor::decoration_request::unset;
            // Created again before a commit: the previous mode is retained (v2 text) — the
            // switch back to client-side, decided at commit by the missing object, is
            // called off.
            rec->deco_awaiting_configure = version < 2;
            if (surface_debug()) {
                char line[96];
                std::snprintf(line, sizeof(line), "decoration create id=%llu version=%d",
                              rec->id.id(), version);
                deco_log(line);
            }
            if (g_p0_ctx && g_p0_ctx->wm)
                g_p0_ctx->wm->on_decoration_request(rec->id, true);
        },
};

} // namespace

void bind_decoration_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_decoration_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&decoration_manager_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
