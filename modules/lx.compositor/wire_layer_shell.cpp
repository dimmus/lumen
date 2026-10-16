module;

// wlr-layer-shell (zwlr_layer_shell_v1, zwlr_layer_surface_v1) and the layer shell's wire
// ops. Part of the protocol wire split; shared declarations live in the :wire partition
// (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
// wayland-scanner names an argument `namespace`, a C++ keyword; renamed for this header
// only (Wayfire does the same).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#undef namespace
#pragma clang diagnostic pop
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

layer_surface_obj layer_surfaces[lx::compositor::layer_shell::k_max]{};

// ── wlr-layer-shell ────────────────────────────────────────────────────────
//
// Wire glue only: records, the configure flow, arrangement, exclusive zones and keyboard
// interactivity live in `layer_shell`; the commit rules in `layer_prechecks`.

layer_surface_obj* layer_surface_from(wl_resource* r) {
    auto* o = r ? static_cast<layer_surface_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return o && o->used && o->resource == r ? o : nullptr;
}

/// The layer record of a zwlr_layer_surface_v1 while its wl_surface lives; null when inert.
lx::compositor::layer_record* layer_record_of(wl_resource* r) {
    auto* o = layer_surface_from(r);
    if (!o || !o->surface || !o->ctx || !o->ctx->layers)
        return nullptr;
    return o->ctx->layers->find(o->surface->id);
}

void layer_surface_destroy_resource(wl_resource* r) {
    auto* o = layer_surface_from(r);
    if (!o)
        return;
    // The role object going away unmaps the surface (its popups first) and frees the
    // record; the wl_surface keeps the layer role, and a new layer surface may follow.
    if (o->surface) {
        if (o->ctx && o->ctx->layers)
            o->ctx->layers->destroy(o->surface->id);
        o->surface->layer = nullptr;
    }
    *o = {};
}

[[nodiscard]] int layer_clamp_size(uint32_t v) {
    return v > (1u << 30) ? (1 << 30) : static_cast<int>(v);
}

const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
    .set_size =
        [](struct wl_client*, struct wl_resource* r, uint32_t width, uint32_t height) {
            if (auto* rec = layer_record_of(r))
                rec->pending.size = {layer_clamp_size(width), layer_clamp_size(height)};
        },
    .set_anchor =
        [](struct wl_client*, struct wl_resource* r, uint32_t anchor) {
            constexpr uint32_t all = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            if (anchor > all) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                                       "invalid anchor %u", anchor);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.anchor = anchor;
        },
    .set_exclusive_zone =
        [](struct wl_client*, struct wl_resource* r, int32_t zone) {
            if (auto* rec = layer_record_of(r))
                rec->pending.zone = zone;
        },
    .set_margin =
        [](struct wl_client*, struct wl_resource* r, int32_t top, int32_t right, int32_t bottom,
           int32_t left) {
            if (auto* rec = layer_record_of(r)) {
                rec->pending.margin_top = top;
                rec->pending.margin_right = right;
                rec->pending.margin_bottom = bottom;
                rec->pending.margin_left = left;
            }
        },
    .set_keyboard_interactivity =
        [](struct wl_client*, struct wl_resource* r, uint32_t k) {
            // on_demand exists since version 4; anything past it never did.
            if (k > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND ||
                (k == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
                 wl_resource_get_version(r) <
                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION)) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                                       "invalid keyboard interactivity %u", k);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.interactivity = k;
        },
    .get_popup =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* popup_res) {
            auto* rec = layer_record_of(r);
            xdg_popup_obj* po = popup_from(popup_res);
            if (!rec || !po || !po->surface || !po->ctx || !po->ctx->popups)
                return;
            // "This popup should have been created via xdg_surface::get_popup with the
            // parent set to NULL, and this request must be invoked before committing the
            // popup's initial state." The spec names no error; invalid_popup_parent is the
            // one that describes it (recorded in docs/architecture.md).
            if (!po->ctx->popups->set_layer_parent(po->surface, rec->surface)) {
                if (wl_resource* wm = wm_base_resource(po->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                           "layer_surface.get_popup on a popup that has a parent "
                                           "or made its initial commit");
            }
        },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            auto* o = layer_surface_from(r);
            if (!o || !o->surface || !o->ctx || !o->ctx->layers)
                return;
            using ack = lx::compositor::layer_shell::ack_result;
            if (o->ctx->layers->on_ack(o->surface->id, serial) == ack::invalid)
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                                       "ack_configure with a serial never sent");
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_layer =
        [](struct wl_client*, struct wl_resource* r, uint32_t layer) {
            // The spec's enum is zwlr_layer_shell_v1.layer, and so is the error wlroots posts
            // here — on the layer surface.
            if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
                wl_resource_post_error(r, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                                       "invalid layer %u", layer);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.layer = layer;
        },
    .set_exclusive_edge =
        [](struct wl_client*, struct wl_resource* r, uint32_t edge) {
            // One edge or none; whether it is an anchored one is checked at commit.
            if (edge != 0 && (edge > ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT || (edge & (edge - 1)) != 0)) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                                       "exclusive edge %u is not a single edge", edge);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.exclusive_edge = edge;
        },
};

} // namespace

void layer_send_configure(void* layer_res, unsigned serial, int width, int height) {
    if (layer_res)
        zwlr_layer_surface_v1_send_configure(static_cast<wl_resource*>(layer_res), serial,
                                             static_cast<uint32_t>(width > 0 ? width : 0),
                                             static_cast<uint32_t>(height > 0 ? height : 0));
}

void layer_send_closed(void* layer_res) {
    if (layer_res)
        zwlr_layer_surface_v1_send_closed(static_cast<wl_resource*>(layer_res));
}

namespace {

void layer_shell_get_layer_surface(struct wl_client* client, struct wl_resource* resource,
                                   uint32_t id, struct wl_resource* surface_res,
                                   struct wl_resource* /*output: output 0 until D5*/,
                                   uint32_t layer, const char* name_space) {
    surface_obj* s = surface_from(surface_res);
    if (!s || !s->ctx || !s->ctx->layers)
        return;
    // wlroots' order: the layer value, then the role, then the buffer.
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer %u", layer);
        return;
    }
    if ((s->role != surface_role::none && s->role != surface_role::layer) || s->layer) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                               "wl_surface@%u already has another role",
                               wl_resource_get_id(surface_res));
        return;
    }
    // "Creating a layer surface from a wl_surface which has a buffer attached or committed
    // is a client error."
    if (s->has_content || (s->pending.buffer_set && s->pending.buffer)) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                               "wl_surface@%u has a buffer attached or committed",
                               wl_resource_get_id(surface_res));
        return;
    }
    layer_surface_obj* o = nullptr;
    for (auto& slot : layer_surfaces)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res = wl_resource_create(client, &zwlr_layer_surface_v1_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    using created = lx::compositor::layer_shell::create_error;
    if (s->ctx->layers->create(s->id, s->client, client, s->resource, res,
                               static_cast<unsigned>(wl_resource_get_version(resource)), layer,
                               name_space) != created::none) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->ctx = s->ctx;
    o->surface = s;
    s->layer = o;
    s->role = surface_role::layer;
    wl_resource_set_implementation(res, &layer_surface_impl, o, layer_surface_destroy_resource);
}

const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
    .get_layer_surface = layer_shell_get_layer_surface,
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

} // namespace

void bind_layer_shell(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwlr_layer_shell_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&layer_shell_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
