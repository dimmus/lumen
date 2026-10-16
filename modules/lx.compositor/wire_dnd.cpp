module;

// Drag-and-drop on the wire: wl_data_device.start_drag, the drag's offers and their
// negotiation (wl_data_offer.accept / set_actions / finish, wl_data_source events), and the
// enter / motion / drop / leave sequence a target sees. The grab itself — who is under the
// pointer, when the buttons let go, Escape, the icon following the pointer — is the
// Wayland-free `:dnd` partition, driven by the window manager's pointer path; this unit is
// its `dnd_ops`. Offers come from the shared selection tables (wire_selection.cpp). Part of
// the protocol wire split; shared declarations live in the :wire partition (wire.cppm).
//
// Choices, against the peers (wlroots/sway, Mutter, KWin, Weston, Smithay):
// - start_drag needs the serial of the last button press delivered to that client with the
//   button still held, and no move/resize or drag running (wlroots' pointer-grab rule). The
//   icon takes its role before the serial is checked; the role is permanent (wlroots).
// - drop only when the target accepted a type the source offered and an action other than
//   none was negotiated; then `leave`. Otherwise `leave` and `cancelled` (wlroots, Mutter,
//   Weston). `cancelled` and the DnD events go to version 3+ sources only: for older ones
//   the spec allows `cancelled` on replacement alone.
// - the action: of what both sides accept, the user's (Shift move, Ctrl copy, Alt ask —
//   Mutter's, the spec's advice) before the drop, then the destination's preferred one, then
//   the lowest bit. A side bound below version 3 counts as copy; a version 3 source that never
//   called set_actions too (Weston). The pointer shows the outcome (copy, move, ask,
//   no-drop), as in Mutter and KWin.
// - a version 2 target never sends finish: its source gets dnd_finished at the drop.
// UI affinity only. Per motion: one `motion` per device of the target, nothing else — no
// allocation, no logging. Per target change: one offer per device (the protocol's).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import lx.input;
import :data_transfer;
import :surface_tree;
import :wm;
import :dnd;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

constexpr const char* k_category = "compositor.dnd";
/// Data devices of one client a drag event goes to (a toolkit binds one per seat).
constexpr unsigned k_max_target_devices = 8;

/// The operation in progress. Only one per seat, and Lumen has one seat.
struct drag_state {
    bool active = false;
    /// Null for a drag within one client (start_drag with no source).
    xfer_source* source = nullptr;
    wl_client* origin_client = nullptr;
    /// The client and surface under the pointer that was sent `enter` (null: none).
    wl_client* target_client = nullptr;
    wl_resource* target_surface = nullptr;
    lx::surface_id target_id{};
    /// The offer negotiation follows: the target's first, or the last one to set actions.
    xfer_offer* offer = nullptr;
    /// Index in the source's mime list of the type the target accepted, for the drop log.
    int accepted_mime = -1;
    /// From the held modifiers (`dnd_action_bits`), until the drop.
    unsigned user_action = 0;
};
drag_state drag{};

[[nodiscard]] bool dnd_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void dnd_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, k_category, line);
}

unsigned res_id(wl_resource* r) { return r ? wl_resource_get_id(r) : 0; }

[[nodiscard]] bool v3(wl_resource* r) {
    return r && wl_resource_get_version(r) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION;
}

/// The source can still be told something: live, and its client not being torn down.
[[nodiscard]] bool source_reachable(const xfer_source* s) {
    return s && s->live && s->resource && !client_dying(s->client);
}

[[nodiscard]] unsigned source_actions(const xfer_source* s) {
    return s->actions_set ? s->dnd_actions : lx::compositor::dnd_copy;
}

[[nodiscard]] bool offer_of_drag(const xfer_offer* o) {
    return o && o->live && o->use == offer_use::dnd && o->source && o->source == drag.source;
}

/// Every drag offer of `src` held by `client` goes inert (all of them with a null client).
void inert_drag_offers(xfer_source* src, wl_client* client) {
    if (!src)
        return;
    for (xfer_offer* o = src->offers; o;) {
        xfer_offer* next = o->next;
        if (o->use == offer_use::dnd && (!client || o->client == client))
            xfer_inert_offer(o);
        o = next;
    }
}

/// Sends `leave` to the target's devices (unless its client is going).
void send_leave(wl_client* client) {
    if (!client || client_dying(client))
        return;
    xfer_device* devs[k_max_target_devices];
    const unsigned n = xfer_devices_of(client, xfer_family::clipboard, devs, k_max_target_devices);
    for (unsigned i = 0; i < n; ++i)
        wl_data_device_send_leave(devs[i]->resource);
}

void feedback(unsigned action) {
    if (g_p0_ctx && g_p0_ctx->dnd)
        g_p0_ctx->dnd->set_feedback(drag.target_client != nullptr && drag.offer != nullptr,
                                    action);
}

/// Recomputes the action for offer `o` (null: no target) of source `src` and sends what
/// changed: `action` to the offer (v3) and to the source (v3), the pointer shape.
void renegotiate(xfer_source* src, xfer_offer* o) {
    if (!src)
        return;
    unsigned a = lx::compositor::dnd_none;
    if (o && o->live && o->source == src) {
        const bool offer3 = v3(o->resource);
        a = lx::compositor::negotiate_dnd_action(
            source_actions(src), offer3 ? o->actions : lx::compositor::dnd_copy,
            offer3 ? o->preferred : 0u, src->dropped ? 0u : drag.user_action);
        if (a != o->action) {
            o->action = a;
            if (offer3 && !client_dying(o->client))
                wl_data_offer_send_action(o->resource, a);
        }
    }
    if (a != src->action) {
        src->action = a;
        if (v3(src->resource) && source_reachable(src))
            wl_data_source_send_action(src->resource, a);
        if (dnd_debug()) {
            const bool offer3 = o && v3(o->resource);
            char line[128];
            std::snprintf(line, sizeof(line),
                          "dnd action action=%u source=%u offer=%u preferred=%u user=%u", a,
                          source_actions(src), o ? (offer3 ? o->actions : 1u) : 0u,
                          o && offer3 ? o->preferred : 0u, src->dropped ? 0u : drag.user_action);
            dnd_log(line);
        }
    }
    if (drag.active)
        feedback(a);
}

/// Leaves the current target: `leave` to its devices, its offers inert, the source told
/// nothing is accepted any more.
void leave_target() {
    wl_client* client = drag.target_client;
    if (!client)
        return;
    send_leave(client);
    inert_drag_offers(drag.source, client);
    drag.target_client = nullptr;
    drag.target_surface = nullptr;
    drag.target_id = {};
    drag.offer = nullptr;
    drag.accepted_mime = -1;
    if (xfer_source* src = drag.source) {
        src->accepted = false;
        if (source_reachable(src))
            wl_data_source_send_target(src->resource, nullptr);
    }
}

// ── dnd_ops ────────────────────────────────────────────────────────────────

void op_focus(void*, void* surface_res_ptr, double sx, double sy) {
    if (!drag.active)
        return;
    leave_target();
    auto* surface_res = static_cast<wl_resource*>(surface_res_ptr);
    wl_client* client = surface_res ? wl_resource_get_client(surface_res) : nullptr;
    xfer_device* devs[k_max_target_devices];
    const unsigned n =
        client && !client_dying(client)
            ? xfer_devices_of(client, xfer_family::clipboard, devs, k_max_target_devices)
            : 0u;
    const surface_obj* s = surface_from(surface_res);
    if (n > 0 && g_p0_ctx && g_p0_ctx->seat) {
        drag.target_client = client;
        drag.target_surface = surface_res;
        drag.target_id = s ? s->id : lx::surface_id{};
        const unsigned serial = g_p0_ctx->seat->next_serial();
        const wl_fixed_t fx = wl_fixed_from_double(sx);
        const wl_fixed_t fy = wl_fixed_from_double(sy);
        for (unsigned i = 0; i < n; ++i) {
            xfer_offer* o = nullptr;
            if (drag.source) {
                o = xfer_create_offer(devs[i], drag.source, offer_use::dnd);
                if (o && v3(o->resource))
                    wl_data_offer_send_source_actions(o->resource, source_actions(drag.source));
                if (o && !drag.offer)
                    drag.offer = o;
            }
            wl_data_device_send_enter(devs[i]->resource, serial, surface_res, fx, fy,
                                      o ? o->resource : nullptr);
        }
    }
    if (dnd_debug()) {
        char line[160];
        if (drag.target_client)
            std::snprintf(line, sizeof(line),
                          "dnd focus surface=%llu client=%u sx=%.1f sy=%.1f offer=%u",
                          drag.target_id.id(), wire_client_id(client), sx, sy,
                          res_id(drag.offer ? drag.offer->resource : nullptr));
        else if (surface_res)
            // A surface whose client has no data device: not a target.
            std::snprintf(line, sizeof(line), "dnd focus none surface=%llu client=%u devices=0",
                          s ? s->id.id() : 0ull, wire_client_id(client));
        else
            std::snprintf(line, sizeof(line), "dnd focus none");
        dnd_log(line);
    }
    // A drag without a source has nothing to negotiate: its client draws its own feedback,
    // and the pointer keeps the grab's shape.
    if (drag.source)
        renegotiate(drag.source, drag.offer);
}

void op_motion(void*, unsigned time_ms, double sx, double sy) {
    if (!drag.active || !drag.target_client || client_dying(drag.target_client))
        return;
    // A target that stopped answering pings is not fed motion it cannot read (as for
    // wl_pointer.motion); enter, leave and drop still reach it.
    if (g_p0_ctx && g_p0_ctx->input && g_p0_ctx->input->motion_muted(drag.target_client))
        return;
    xfer_device* devs[k_max_target_devices];
    const unsigned n =
        xfer_devices_of(drag.target_client, xfer_family::clipboard, devs, k_max_target_devices);
    const wl_fixed_t fx = wl_fixed_from_double(sx);
    const wl_fixed_t fy = wl_fixed_from_double(sy);
    for (unsigned i = 0; i < n; ++i)
        wl_data_device_send_motion(devs[i]->resource, time_ms, fx, fy);
}

void op_user_action(void*, unsigned action) {
    if (!drag.active)
        return;
    drag.user_action = action;
    renegotiate(drag.source, drag.offer);
}

/// Ends the wire side of the drag without a drop: `leave`, offers inert, `cancelled`.
void cancel_drag(const char* cause) {
    xfer_source* src = drag.source;
    leave_target();
    inert_drag_offers(src, nullptr);
    if (src && v3(src->resource) && source_reachable(src))
        wl_data_source_send_cancelled(src->resource);
    if (src && dnd_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "dnd cancelled source=%u cause=%s",
                      res_id(src->resource), cause);
        dnd_log(line);
    }
    drag = {};
}

void op_cancel(void*, lx::compositor::dnd_end_reason why) {
    if (drag.active)
        cancel_drag(lx::compositor::dnd_end_name(why));
}

bool op_drop(void*, lx::compositor::dnd_end_reason& why) {
    if (!drag.active)
        return false;
    wl_client* client = drag.target_client;
    xfer_source* src = drag.source;
    xfer_device* devs[k_max_target_devices];
    const unsigned n =
        client && !client_dying(client)
            ? xfer_devices_of(client, xfer_family::clipboard, devs, k_max_target_devices)
            : 0u;
    if (n == 0 || (src && !offer_of_drag(drag.offer))) {
        why = lx::compositor::dnd_end_reason::no_target;
        return false;
    }
    if (src && !src->accepted) {
        why = lx::compositor::dnd_end_reason::not_accepted;
        return false;
    }
    if (src && src->action == lx::compositor::dnd_none) {
        why = lx::compositor::dnd_end_reason::no_action;
        return false;
    }
    for (unsigned i = 0; i < n; ++i)
        wl_data_device_send_drop(devs[i]->resource);
    if (src) {
        src->dropped = true;
        if (v3(src->resource) && source_reachable(src))
            wl_data_source_send_dnd_drop_performed(src->resource);
        const bool ask = src->action == lx::compositor::dnd_ask;
        for (xfer_offer* o = src->offers; o; o = o->next)
            if (o->use == offer_use::dnd)
                o->in_ask = ask;
        // A version 2 target never sends finish: the operation is complete as far as it
        // will ever say (wlroots, Mutter).
        if (!v3(drag.offer->resource)) {
            src->finished = true;
            if (v3(src->resource) && source_reachable(src))
                wl_data_source_send_dnd_finished(src->resource);
        }
    }
    if (dnd_debug()) {
        const char* mime = src && drag.accepted_mime >= 0
                               ? src->mimes.at(static_cast<unsigned>(drag.accepted_mime))
                               : nullptr;
        char line[192];
        std::snprintf(line, sizeof(line), "dnd drop surface=%llu client=%u action=%u mime=%.96s",
                      drag.target_id.id(), wire_client_id(client), src ? src->action : 0u,
                      mime ? mime : "-");
        dnd_log(line);
    }
    // Then `leave` (wlroots, Mutter, Weston); the offer stays live for receive and finish.
    send_leave(client);
    drag = {};
    return true;
}

} // namespace

// ── Requests ───────────────────────────────────────────────────────────────

void dnd_start_drag(xfer_device* dev, wl_resource* source_res, wl_resource* origin_res,
                    wl_resource* icon_res, uint32_t serial) {
    if (!g_p0_ctx)
        return;
    wl_client* client = dev->client;

    // The icon's role first, whatever happens to the drag (wlroots): a surface that has
    // another role is the client's error.
    surface_obj* icon = surface_from(icon_res);
    if (icon_res && !icon)
        return; // destroyed in the same flush
    if (icon && icon->role != surface_role::none && icon->role != surface_role::dnd_icon) {
        wl_resource_post_error(dev->resource, WL_DATA_DEVICE_ERROR_ROLE,
                               "wl_surface@%u already has another role",
                               wl_resource_get_id(icon_res));
        return;
    }
    if (icon) {
        if (g_p0_ctx->tree && !g_p0_ctx->tree->make_root(icon->id, lx::compositor::root_kind::dnd_icon)) {
            // A surface linked as a subsurface has that role already; nothing else gets here.
            wl_resource_post_error(dev->resource, WL_DATA_DEVICE_ERROR_ROLE,
                                   "wl_surface@%u already has another role",
                                   wl_resource_get_id(icon_res));
            return;
        }
        icon->role = surface_role::dnd_icon;
    }

    xfer_source* src = xfer_source_from(source_res);
    if (source_res && !src)
        return;
    if (src && src->used) {
        wl_resource_post_error(dev->resource, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                               "wl_data_source@%u has already been used",
                               res_id(src->resource));
        return;
    }
    if (src) {
        src->used = true;
        src->role = source_role::dnd;
    }

    auto* wm = g_p0_ctx->wm;
    auto* grab = g_p0_ctx->dnd;
    const char* reject = !wm || !grab ? "no-wm" : wm->validate_drag_serial(client, serial);
    if (reject) {
        if (dnd_debug()) {
            char line[96];
            std::snprintf(line, sizeof(line), "dnd start rejected client=%u reason=%s",
                          wire_client_id(client), reject);
            dnd_log(line);
        }
        // The client learns its drag will not happen (a version 2 source has no such
        // event: it was not replaced).
        if (src && v3(src->resource))
            wl_data_source_send_cancelled(src->resource);
        return;
    }

    const surface_obj* origin = surface_from(origin_res);
    drag = {};
    drag.active = true;
    drag.source = src;
    drag.origin_client = client;
    if (dnd_debug()) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "dnd start client=%u source=%u origin=%llu icon=%llu button=%u actions=%u "
                      "mimes=%u",
                      wire_client_id(client), res_id(src ? src->resource : nullptr),
                      origin ? origin->id.id() : 0ull, icon ? icon->id.id() : 0ull,
                      wm->last_press_button(), src ? source_actions(src) : 0u,
                      src ? src->mimes.count() : 0u);
        dnd_log(line);
    }
    const lx::client_id origin_client =
        origin ? origin->client : lx::client_id{wire_client_id(client)};
    grab->begin(origin_client, src == nullptr, icon ? icon->id : lx::surface_id{},
                g_p0_ctx->seat ? g_p0_ctx->seat->pointer_x() : 0.0,
                g_p0_ctx->seat ? g_p0_ctx->seat->pointer_y() : 0.0);
    // The origin's implicit grab ends (leave), and the surface under the pointer — usually the
    // origin itself — becomes the first target at once.
    wm->begin_drag();
}

void dnd_offer_accept(xfer_offer* o, uint32_t, const char* mime) {
    xfer_source* src = o->source;
    if (!src)
        return; // inert: a previous target, or the operation is over
    int index = -1;
    if (mime) {
        for (unsigned i = 0; i < src->mimes.count(); ++i)
            if (std::strcmp(src->mimes.at(i), mime) == 0) {
                index = static_cast<int>(i);
                break;
            }
    }
    const bool accepted = index >= 0;
    // `target` on every request (all versions: it is a version 1 event); how often is the
    // target client's choice.
    if (source_reachable(src))
        wl_data_source_send_target(src->resource, accepted ? mime : nullptr);
    if (offer_of_drag(o))
        drag.accepted_mime = index;
    if (accepted == src->accepted)
        return;
    src->accepted = accepted;
    if (dnd_debug()) {
        char line[160];
        std::snprintf(line, sizeof(line), "dnd accept client=%u mime=%.96s",
                      wire_client_id(o->client), accepted ? mime : "-");
        dnd_log(line);
    }
}

void dnd_offer_set_actions(xfer_offer* o, uint32_t actions, uint32_t preferred) {
    if (!lx::compositor::valid_dnd_mask(actions)) {
        wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
                               "invalid action mask %u", actions);
        return;
    }
    if (!lx::compositor::single_dnd_action(preferred) ||
        (preferred != 0 && (preferred & actions) == 0)) {
        wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                               "invalid preferred action %u for actions %u", preferred,
                               actions);
        return;
    }
    xfer_source* src = o->source;
    if (!src)
        return; // inert
    if (src->dropped) {
        // After the drop only an `ask` is still open, and its answer must be one the source
        // offers ("the destination is expected to perform one last set_actions").
        if (!o->in_ask) {
            wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                                   "set_actions after the drop");
            return;
        }
        if ((preferred & source_actions(src)) == 0) {
            wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                                   "ask answered with action %u", preferred);
            return;
        }
    }
    o->actions = actions;
    o->preferred = preferred;
    if (drag.active && o->client == drag.target_client)
        drag.offer = o;
    renegotiate(src, o);
    if (src->dropped)
        o->in_ask = src->action == lx::compositor::dnd_ask;
}

void dnd_offer_finish(xfer_offer* o) {
    xfer_source* src = o->source;
    if (!src || o->finished)
        return; // inert (a cancelled or replaced operation): nothing left to finish
    if (!src->dropped || !src->accepted) {
        wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                               "premature finish");
        return;
    }
    if (src->action == lx::compositor::dnd_none || src->action == lx::compositor::dnd_ask) {
        wl_resource_post_error(o->resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                               "finish with action %u", src->action);
        return;
    }
    o->finished = true;
    src->finished = true;
    if (v3(src->resource) && source_reachable(src))
        wl_data_source_send_dnd_finished(src->resource);
    if (dnd_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "dnd finish client=%u action=%u",
                      wire_client_id(o->client), src->action);
        dnd_log(line);
    }
    xfer_inert_offer(o);
}

// ── Destruction ────────────────────────────────────────────────────────────

void dnd_offer_destroyed(xfer_offer* o) {
    xfer_source* src = o->source;
    if (!src)
        return;
    if (drag.active && drag.offer == o) {
        // Another device of the same target may still hold one.
        drag.offer = nullptr;
        for (xfer_offer* p = src->offers; p; p = p->next)
            if (p != o && p->use == offer_use::dnd && p->client == drag.target_client) {
                drag.offer = p;
                break;
            }
        renegotiate(src, drag.offer);
        return;
    }
    // After the drop: an offer destroyed without finish — or an `ask` the user dismissed
    // ("expected to perform wl_data_offer.destroy right away") — cancels the operation, once
    // no other offer of the target is left to finish it (wlroots).
    if (!src->dropped || src->finished || o->finished)
        return;
    for (xfer_offer* p = src->offers; p; p = p->next)
        if (p != o && p->use == offer_use::dnd)
            return;
    src->finished = true; // nothing more will come of it
    if (v3(src->resource) && source_reachable(src))
        wl_data_source_send_cancelled(src->resource);
    if (dnd_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "dnd cancelled source=%u cause=%s",
                      res_id(src->resource), o->in_ask ? "ask-dismissed" : "offer-destroyed");
        dnd_log(line);
    }
}

void dnd_source_destroyed(xfer_source* s) {
    if (!drag.active || drag.source != s)
        return; // after the drop its offers just go inert (the caller does that)
    // The target is left; nothing is sent to the source: its resource is being destroyed.
    drag.source = nullptr;
    leave_target();
    inert_drag_offers(s, nullptr);
    drag = {};
    if (g_p0_ctx && g_p0_ctx->dnd)
        g_p0_ctx->dnd->end(lx::compositor::dnd_end_reason::source_destroyed);
}

void dnd_client_destroyed(wl_client* client) {
    // A drag with a source ends when that source is destroyed (its own destructor). One
    // without a source has only its client to end with.
    if (!drag.active || drag.source || drag.origin_client != client)
        return;
    leave_target();
    drag = {};
    if (g_p0_ctx && g_p0_ctx->dnd)
        g_p0_ctx->dnd->end(lx::compositor::dnd_end_reason::client_gone);
}

void install_dnd(p0_protocol_context& ctx) {
    if (!ctx.dnd)
        return;
    lx::compositor::dnd_ops ops{};
    ops.focus = &op_focus;
    ops.motion = &op_motion;
    ops.drop = &op_drop;
    ops.cancel = &op_cancel;
    ops.user_action = &op_user_action;
    ops.user = &ctx;
    ctx.dnd->set_ops(ops);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
