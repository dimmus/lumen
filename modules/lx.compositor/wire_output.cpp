module;

// wl_output, zxdg_output_manager_v1 (xdg-output) and output membership: wl_surface.enter /
// leave and wl_surface.preferred_buffer_scale / _transform. Part of the protocol wire
// split; shared declarations live in the :wire partition (wire.cppm).
//
// Membership is computed from scene-node rects (wlroots' scene does the same): the surface
// tree reports every change of a node's global rect through one hook, and each surface keeps
// a mask of the output slots it overlaps. enter/leave go to every wl_output the surface's
// client bound for that output; a wl_output bound late replays the enters it missed.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-output-unstable-v1-server-protocol.h"
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

namespace lx::compositor::p0 {

namespace {

constexpr const char* k_category = "compositor.output";

/// One bound wl_output. A slot of the output manager, by index (outputs never move slot
/// while connected).
///
/// Bound wl_output and zxdg_output_v1 objects are allocated per bind / get_xdg_output and
/// freed with the object, in intrusive lists (wlroots keeps a resource list per output):
/// from a shared fixed table, a client that binds without releasing would make the next
/// client's bind fail with no_memory, fatal to that client.
struct output_res {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    unsigned slot = 0;
    bool live = false;
    output_res* prev = nullptr;
    output_res* next = nullptr;
};
/// One zxdg_output_v1. Inert once its wl_output is gone.
struct xdg_output_res {
    wl_resource* resource = nullptr;
    output_res* output = nullptr;
    bool live = false;
    xdg_output_res* prev = nullptr;
    xdg_output_res* next = nullptr;
};

/// Newest first.
output_res* output_resources = nullptr;
xdg_output_res* xdg_output_resources = nullptr;

/// Links `n` at the head of `head`'s list.
template <class T> void list_push(T*& head, T* n) {
    n->prev = nullptr;
    n->next = head;
    if (head)
        head->prev = n;
    head = n;
}

template <class T> void list_unlink(T*& head, T* n) {
    if (n->prev)
        n->prev->next = n->next;
    else
        head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    n->prev = n->next = nullptr;
}

void out_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, k_category, line);
}

lx::compositor::output_manager* outputs() {
    return g_p0_ctx ? g_p0_ctx->outputs : nullptr;
}

/// The slot the (single, until D5) wl_output global stands for: the first live output.
int global_slot() {
    auto* om = outputs();
    if (!om || om->count() == 0)
        return -1;
    return om->slot_of(om->nth(0));
}

/// wl_output.scale: the integer a client without fractional-scale renders at, the ceiling
/// (wlroots, KWin) — downscaled 2x content is sharper than upscaled 1x.
int ceil_scale(double scale) {
    const int c = static_cast<int>(std::ceil(scale));
    return c < 1 ? 1 : c;
}

/// geometry, mode, scale (v2), and with `with_identity` name and description (v4): every
/// wl_output event but `done`.
void send_output_state(wl_resource* res, const lx::compositor::output_state& o,
                       bool with_identity) {
    const int version = wl_resource_get_version(res);
    // geometry's x, y are the position in the global (logical) space; mode is in pixels.
    wl_output_send_geometry(res, o.position.x, o.position.y, o.phys_w_mm, o.phys_h_mm,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "Lumen", o.name,
                            static_cast<int32_t>(o.transform));
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        o.mode.width, o.mode.height,
                        static_cast<int32_t>(o.refresh_mhz));
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(res, ceil_scale(o.scale));
    if (with_identity && version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(res, o.name);
        wl_output_send_description(res, o.description);
    }
}

void send_output_done(wl_resource* res) {
    if (wl_resource_get_version(res) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
}

#if defined(LUMEN_HAS_PROTOCOL_GLUE)
/// The logical rect: the mode divided by the scale, rounded (wlroots' effective
/// resolution); they coincide at scale 1.
lx::rect2i logical_rect(const lx::compositor::output_state& o) { return o.logical_rect(); }

/// logical_position, logical_size, and with `with_identity` name and description (v2+);
/// then zxdg_output_v1.done for v1–2 (v3 objects are closed by wl_output.done).
void send_xdg_output_state(wl_resource* res, const lx::compositor::output_state& o,
                           bool with_identity) {
    const int version = wl_resource_get_version(res);
    const lx::rect2i l = logical_rect(o);
    zxdg_output_v1_send_logical_position(res, l.x, l.y);
    zxdg_output_v1_send_logical_size(res, l.width, l.height);
    if (with_identity && version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
        zxdg_output_v1_send_name(res, o.name);
        zxdg_output_v1_send_description(res, o.description);
    }
    if (version < 3)
        zxdg_output_v1_send_done(res);
}
#endif

/// Moves `s` onto the outputs in `mask`: enter for the new ones, leave for the old ones, to
/// every wl_output its client bound for each; then the preferred buffer scale and transform
/// (v6) when they changed. A surface being destroyed, or of a client being torn down, has
/// its mask recorded and nothing sent.
void update_membership(surface_obj* s, std::uint32_t mask, int primary) {
    if (!s || !s->resource)
        return;
    const std::uint32_t entered = mask & ~s->outputs;
    const std::uint32_t left = s->outputs & ~mask;
    s->outputs = mask;
    wl_client* client = wl_resource_get_client(s->resource);
    if (s->destroying || client_dying(client))
        return;
    for (unsigned slot = 0; (entered | left) >> slot; ++slot) {
        const std::uint32_t bit = 1u << slot;
        if (!((entered | left) & bit))
            continue;
        const bool enter = (entered & bit) != 0;
        unsigned sent = 0;
        for (output_res* r = output_resources; r; r = r->next) {
            if (r->client != client || r->slot != slot)
                continue;
            if (enter)
                wl_surface_send_enter(s->resource, r->resource);
            else
                wl_surface_send_leave(s->resource, r->resource);
            ++sent;
        }
        if (surface_debug()) {
            char line[128];
            std::snprintf(line, sizeof(line), "output %s surface=%llu output=%u client=%u resources=%u",
                          enter ? "enter" : "leave", s->id.id(), slot, wire_client_id(client),
                          sent);
            out_log(line);
        }
    }
    // Recomputed only while the surface is on an output: one that left them all keeps
    // rendering for where it was (wlroots does the same).
    auto* om = outputs();
    if (!om || mask == 0)
        return;
    if (wl_resource_get_version(s->resource) < 6) {
        fractional_scale_update(s, lx::compositor::preferred_scale_120(*om, mask));
        return;
    }
    // wp_fractional_scale_v1 first (a client with both uses the fractional value and
    // ignores the integer one), then the wl_surface v6 events.
    if (!s->destroying)
        fractional_scale_update(s, lx::compositor::preferred_scale_120(*om, mask));
    const int scale = lx::compositor::preferred_buffer_scale(*om, mask);
    const unsigned transform = lx::compositor::preferred_buffer_transform(*om, primary);
    bool changed = false;
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
    if (scale != s->pref_scale) {
        s->pref_scale = scale;
        wl_surface_send_preferred_buffer_scale(s->resource, scale);
        changed = true;
    }
    if (transform != s->pref_transform) {
        s->pref_transform = transform;
        wl_surface_send_preferred_buffer_transform(s->resource, transform);
        changed = true;
    }
#else
    (void)scale;
    (void)transform;
#endif
    if (changed && surface_debug()) {
        char line[112];
        std::snprintf(line, sizeof(line), "output preferred surface=%llu scale=%d transform=%u",
                      s->id.id(), s->pref_scale, s->pref_transform);
        out_log(line);
    }
}

void membership_from_rect(surface_obj* s, lx::rect2i rect) {
    auto* om = outputs();
    if (!s || !om)
        return;
    const bool empty = rect.width <= 0 || rect.height <= 0;
    update_membership(s, empty ? 0u : lx::compositor::outputs_for_rect(*om, rect),
                      empty ? -1 : lx::compositor::primary_output_for_rect(*om, rect));
}

/// cursor_manager's surface hook: the cursor surface shown is on the output under the
/// pointer; the one replaced is on none.
void cursor_surface_changed(void*, lx::surface_id old_surface, lx::surface_id new_surface) {
    if (surface_obj* s = surface_by_id(old_surface))
        update_membership(s, 0, -1);
    surface_obj* s = surface_by_id(new_surface);
    if (!s || !g_p0_ctx || !g_p0_ctx->seat)
        return;
    const lx::rect2i at{static_cast<int>(std::floor(g_p0_ctx->seat->pointer_x())),
                        static_cast<int>(std::floor(g_p0_ctx->seat->pointer_y())), 1, 1};
    membership_from_rect(s, at);
}

// ── wl_output ──────────────────────────────────────────────────────────────

output_res* output_from(wl_resource* r) {
    auto* o = r ? static_cast<output_res*>(wl_resource_get_user_data(r)) : nullptr;
    return o && o->live && o->resource == r ? o : nullptr;
}

void output_destroy_resource(wl_resource* r) {
    output_res* o = output_from(r);
    if (!o)
        return;
    for (xdg_output_res* x = xdg_output_resources; x; x = x->next)
        if (x->output == o)
            x->output = nullptr;
    list_unlink(output_resources, o);
    delete o;
}

const struct wl_output_interface output_impl = {
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

#if defined(LUMEN_HAS_PROTOCOL_GLUE)

// ── zxdg_output_manager_v1 ─────────────────────────────────────────────────

const struct zxdg_output_v1_interface xdg_output_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void xdg_output_destroy_resource(wl_resource* r) {
    auto* x = static_cast<xdg_output_res*>(wl_resource_get_user_data(r));
    if (!x || !x->live || x->resource != r)
        return;
    list_unlink(xdg_output_resources, x);
    delete x;
}

const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_xdg_output =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* output) {
            const int version = wl_resource_get_version(resource);
            auto* x = new (std::nothrow) xdg_output_res{};
            wl_resource* res =
                x ? wl_resource_create(client, &zxdg_output_v1_interface, version, id) : nullptr;
            if (!res) {
                delete x;
                wl_client_post_no_memory(client);
                return;
            }
            x->live = true;
            x->resource = res;
            x->output = output_from(output);
            list_push(xdg_output_resources, x);
            wl_resource_set_implementation(res, &xdg_output_impl, x,
                                           xdg_output_destroy_resource);
            // An output that is gone (its wl_output inert) gets an inert xdg_output.
            auto* om = outputs();
            if (!x->output || !om || !om->slot_live(x->output->slot))
                return;
            const auto& o = om->slot_state(x->output->slot);
            send_xdg_output_state(res, o, true);
            // Version 3: the atomic group closes with wl_output.done (wlroots' order).
            if (version >= 3)
                send_output_done(output);
            if (surface_debug()) {
                const lx::rect2i l = logical_rect(o);
                char line[192];
                std::snprintf(line, sizeof(line),
                              "xdg-output create client=%u output=%u version=%d x=%d y=%d w=%d "
                              "h=%d name=%s",
                              wire_client_id(client), x->output->slot, version, l.x, l.y,
                              l.width, l.height, o.name);
                out_log(line);
            }
        },
};

#endif

} // namespace

void output_node_rect(void*, lx::surface_id surface, lx::rect2i rect) {
    membership_from_rect(surface_by_id(surface), rect);
    // An empty rect is the tree dropping the node (unmapped): what the surface had in
    // flight will not be shown, and its presentation feedback says so.
    if (rect.width <= 0 || rect.height <= 0)
        presentation_surface_unmapped(surface);
    // The text input's surface moved: input-method popups follow it.
    text_input_surface_moved(surface);
}

std::uint32_t expected_scale_120(const surface_obj* s) {
    auto* om = outputs();
    if (!om)
        return 120;
    if (s && s->outputs)
        return lx::compositor::preferred_scale_120(*om, s->outputs);
    // A subsurface shows where its parent does.
    if (s && g_p0_ctx && g_p0_ctx->tree) {
        const surface_obj* p = surface_by_id(g_p0_ctx->tree->parent_of(s->id));
        if (p && p->outputs)
            return lx::compositor::preferred_scale_120(*om, p->outputs);
    }
    // Toplevels and layer surfaces map on output 0, the only output until D5; popups on
    // their parent's, which is output 0 as well.
    const int slot = global_slot();
    return slot >= 0 ? lx::compositor::preferred_scale_120(*om, 1u << slot) : 120u;
}

void for_each_output_resource(wl_client* client, unsigned slot, void (*fn)(wl_resource*, void*),
                              void* user) {
    for (output_res* r = output_resources; r; r = r->next)
        if (r->client == client && r->slot == slot && fn)
            fn(r->resource, user);
}

int output_resource_slot(wl_resource* output) {
    const output_res* o = output_from(output);
    auto* om = outputs();
    return o && om && om->slot_live(o->slot) ? static_cast<int>(o->slot) : -1;
}

void install_outputs(p0_protocol_context& ctx) {
    if (ctx.tree)
        ctx.tree->set_rect_hook(&output_node_rect, &ctx);
    if (ctx.cursor)
        ctx.cursor->set_surface_hook(&cursor_surface_changed, &ctx);
}

void bind_output(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_output_interface, version, id);
    if (!res)
        return;
    auto* native = static_cast<wl_resource*>(res.native());
    if (!native)
        return;
    auto* om = outputs();
    const int slot = global_slot();
    // Never without an output: the output manager falls back to HEADLESS-1 in the same
    // refresh that removes the last connector, and the socket opens after the first one.
    output_res* o = om && slot >= 0 ? new (std::nothrow) output_res{} : nullptr;
    if (!o) {
        wl_resource_set_implementation(native, &output_impl, nullptr, nullptr);
        if (om && slot >= 0)
            wl_client_post_no_memory(wl_resource_get_client(native));
        return;
    }
    o->live = true;
    o->resource = native;
    o->client = wl_resource_get_client(native);
    o->slot = static_cast<unsigned>(slot);
    list_push(output_resources, o);
    wl_resource_set_implementation(native, &output_impl, o, output_destroy_resource);

    const auto& st = om->slot_state(o->slot);
    send_output_state(native, st, true);
    send_output_done(native);

    // Clients often bind outputs after mapping: the surfaces already on this output get the
    // enter they missed (wlroots does the same on bind).
    unsigned replayed = 0;
    const std::uint32_t bit = 1u << o->slot;
    for (auto& s : surfaces) {
        if (!s.used || !s.resource || s.destroying || !(s.outputs & bit) ||
            wl_resource_get_client(s.resource) != o->client)
            continue;
        wl_surface_send_enter(s.resource, native);
        ++replayed;
    }
    if (surface_debug()) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "output bind client=%u output=%u version=%d name=%s replayed=%u",
                      client.id(), o->slot, version, st.name, replayed);
        out_log(line);
    }
}

#if defined(LUMEN_HAS_PROTOCOL_GLUE)
void bind_xdg_output_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_output_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&xdg_output_manager_impl, g_p0_ctx, nullptr);
}
#endif

namespace {

void outputs_changed() {
    auto* om = outputs();
    if (!om || !g_p0_ctx)
        return;
    // 1. Membership first, from where each node is now: a surface leaving an output that went
    //    is told on the wl_output objects that still stand for it. Cursor surfaces have no
    //    node; theirs follows the pointer (the cursor hook).
    for (auto& s : surfaces) {
        if (!s.used || !s.resource || !g_p0_ctx->tree || s.role == surface_role::cursor)
            continue;
        membership_from_rect(&s, g_p0_ctx->tree->node_rect(s.id));
    }
    // 2. Every wl_output (and its xdg_outputs) gets the new state, then done. One whose
    //    output went follows the global to the output now behind it (a single wl_output
    //    global until D5), and the surfaces already there get enter on it.
    const int primary = global_slot();
    for (output_res* r = output_resources; r; r = r->next) {
        if (client_dying(r->client))
            continue;
        bool moved = false;
        if (!om->slot_live(r->slot)) {
            if (primary < 0)
                continue;
            r->slot = static_cast<unsigned>(primary);
            moved = true;
        }
        const auto& st = om->slot_state(r->slot);
        // name never changes for an object (v4); description is not re-sent either, as it
        // is derived from the name.
        send_output_state(r->resource, st, false);
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
        for (xdg_output_res* x = xdg_output_resources; x; x = x->next)
            if (x->output == r)
                send_xdg_output_state(x->resource, st, false);
#endif
        send_output_done(r->resource);
        if (!moved)
            continue;
        const std::uint32_t bit = 1u << r->slot;
        for (auto& s : surfaces)
            if (s.used && s.resource && !s.destroying && (s.outputs & bit) &&
                wl_resource_get_client(s.resource) == r->client)
                wl_surface_send_enter(s.resource, r->resource);
    }
    // 3. Power controls of a wl_output that no longer stands for an output are `failed`.
    output_power_outputs_changed();
    // 4. Lock surfaces follow their output's size and origin (a new configure).
    session_lock_outputs_changed();
    // 5. Input-method popups are placed within the new output bounds.
    text_input_outputs_changed();
}

} // namespace

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::notify_outputs_changed(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::outputs_changed();
#else
    (void)ctx;
#endif
}
