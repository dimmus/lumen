module;

// pointer-constraints-unstable-v1: pointer locks and confinement for games, VM viewers and
// remote desktops (design-app-compat-capture C.3, PC1). The policy is the Wayland-free
// `:pointer_constraints` partition; this unit owns the objects and decides when each holds.
//
// A constraint is active only while its surface has pointer focus, its window has the
// keyboard, the pointer is inside region ∩ input region (to activate; an active one stays
// while the pointer does not leave the surface), and no session lock, VT pause, move,
// resize, drag or popup grab is in progress — Mutter's rule, which the task asks for;
// niri checks focus and region only. Only the pointer-focus surface's constraint can hold,
// so at most one is active for the seat.
//
// While a lock holds the pointer does not move: the compositor restores the position,
// relative motion still goes out (wire_relative_pointer.cpp) and `wl_pointer.motion` never
// does (the router's motion lock). On unlock the pointer is warped to the cursor hint when
// one is set, lies inside the surface, and the surface still has the pointer (sway and
// Mutter warp; a warp makes no relative motion). A confined pointer's motion is clipped
// across the region's rects (`confine_motion`); a new region that leaves it outside warps
// it to the nearest point inside, as the spec allows.
//
// Super+Escape releases the active constraint (`unlocked` / `unconfined`), niri's and
// GNOME's key: a persistent constraint re-arms once the pointer has left the surface or its
// window has lost the keyboard; a oneshot one is spent at its first deactivation, whatever
// ended it, and goes inert so a new constraint may be made for the surface.
//
// Every error:
//   zwp_pointer_constraints_v1.already_constrained  a constraint (active or not, not yet
//                                                    spent) exists for the surface and seat
// A lifetime other than oneshot (1) or persistent (2) is taken as oneshot (the protocol has
// no error for it; wlroots does the same). Constraints are allocated per request, 16 per
// client take effect (more are inert: the protocol does not promise activation), and go
// inert with their surface.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_POINTER_CONSTRAINTS_UNSTABLE_V1)
#include "pointer-constraints-unstable-v1-server-protocol.h"
#define LUMEN_HAS_POINTER_CONSTRAINTS 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import lx.input;
import :surface;
import :surface_tree;
import :session_lock;
import :input_router;
import :wm;
import :popup;
import :pointer_constraints;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_POINTER_CONSTRAINTS)

namespace {

constexpr unsigned k_max_per_client = 16;
constexpr const char* k_category = "compositor.constraint";
/// Commits whose staged region or hint wait for their content update (queued behind a
/// fence, a fifo barrier, a target time). More merge into the newest: applied with a later
/// update, never an earlier one.
constexpr unsigned k_staged = 4;

/// set_region / set_cursor_position_hint state a wl_surface.commit took (double-buffered:
/// it applies with that commit's content update, not with an earlier queued one).
struct staged_state {
    unsigned seq = 0;
    bool region_set = false;
    bool has_region = false;
    lx::compositor::input_region region{};
    bool hint_set = false;
    lx::compositor::constraint_point hint{};
};

struct constraint_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    /// Null: inert — its surface went, the client already had too many, or a oneshot
    /// constraint was spent.
    surface_obj* surface = nullptr;
    bool lock = false;
    bool persistent = false;
    bool active = false;
    /// Released by Super+Escape; cleared once the pointer left the surface or its window
    /// lost the keyboard.
    bool escaped = false;
    /// The region (current, and pending until the surface's next applied update). A null
    /// wl_region is "no region": the input region alone.
    lx::compositor::input_region region{};
    bool has_region = false;
    lx::compositor::input_region pending_region{};
    bool pending_has_region = false;
    bool pending_region_set = false;
    /// set_cursor_position_hint (surface-local), current and pending.
    lx::compositor::constraint_point hint{};
    bool has_hint = false;
    lx::compositor::constraint_point pending_hint{};
    bool pending_hint_set = false;
    /// Committed, waiting for their content update, oldest first.
    staged_state staged[k_staged]{};
    unsigned staged_count = 0;
    /// region ∩ input region ∩ surface box, surface-local.
    lx::compositor::constraint_region effective{};
    /// A content update applied a new region or size: if this confinement is active and the
    /// pointer is now outside, the next evaluation warps it inside (once).
    bool region_changed = false;
    constraint_obj* prev = nullptr;
    constraint_obj* next = nullptr;
};

constraint_obj* constraints = nullptr;
constraint_obj* active = nullptr;
bool in_eval = false;
bool eval_again = false;
/// A constraint's surface applied a content update since the last evaluation.
bool dirty = false;

const char* kind_of(const constraint_obj* c) { return c->lock ? "lock" : "confine"; }

void constraint_log(lx::trace::level lvl, const constraint_obj* c, const char* what) {
    if (lvl == lx::trace::level::debug && !surface_debug())
        return;
    lx::trace::logger::global().logf(lvl, k_category, "pointer {} client={} surface={} {} {}",
                                     kind_of(c), wire_client_id(c->client),
                                     c->surface ? c->surface->id.id() : 0ull,
                                     c->persistent ? "persistent" : "oneshot", what);
}

/// The surface's global origin (logical), from its scene node.
bool surface_origin(const surface_obj* s, double& x, double& y) {
    if (!s || !g_p0_ctx || !g_p0_ctx->surfaces)
        return false;
    const lx::rect2i r = g_p0_ctx->surfaces->node_rect(s->id);
    x = r.x;
    y = r.y;
    return r.width > 0 && r.height > 0;
}

lx::surface_id root_of(const surface_obj* s) {
    if (!s)
        return {};
    return g_p0_ctx && g_p0_ctx->tree ? g_p0_ctx->tree->root_of(s->id) : s->id;
}

void recompute(constraint_obj* c) {
    if (!c->surface) {
        c->effective = {};
        return;
    }
    c->effective = lx::compositor::make_constraint_region(c->has_region ? &c->region : nullptr,
                                                          c->surface->input_current,
                                                          c->surface->surface_size);
}

/// What the seat looks like now, for every constraint.
struct seat_view {
    wl_resource* pointer_focus = nullptr;
    lx::surface_id keyboard_root{};
    wl_resource* keyboard_focus = nullptr;
    bool locked = false;
    bool paused = false;
    bool grab = false;
    double px = 0.0;
    double py = 0.0;
};

seat_view look() {
    seat_view v{};
    p0_protocol_context* ctx = g_p0_ctx;
    if (!ctx)
        return v;
    if (ctx->input) {
        v.pointer_focus = static_cast<wl_resource*>(ctx->input->pointer_focus());
        v.keyboard_focus = static_cast<wl_resource*>(ctx->input->keyboard_focus());
        v.paused = ctx->session_paused || ctx->input->keyboard_suspended();
    }
    if (v.keyboard_focus)
        v.keyboard_root = root_of(surface_from(v.keyboard_focus));
    v.locked = ctx->lock && ctx->lock->state() != lx::compositor::lock_state::unlocked;
    v.grab = (ctx->wm && (ctx->wm->grab_active() || ctx->wm->drag_active())) ||
             (ctx->popups && ctx->popups->grab_active());
    if (ctx->seat) {
        v.px = ctx->seat->pointer_x();
        v.py = ctx->seat->pointer_y();
    }
    return v;
}

bool has_pointer(const constraint_obj* c, const seat_view& v) {
    return c->surface && v.pointer_focus && c->surface->resource == v.pointer_focus;
}

bool window_has_keyboard(const constraint_obj* c, const seat_view& v) {
    if (!c->surface || !v.keyboard_focus)
        return false;
    return c->surface->resource == v.keyboard_focus || root_of(c->surface) == v.keyboard_root;
}

lx::compositor::constraint_conditions conditions(const constraint_obj* c, const seat_view& v) {
    lx::compositor::constraint_conditions in{};
    in.pointer_focus = has_pointer(c, v);
    in.keyboard_focus = window_has_keyboard(c, v);
    double ox = 0.0;
    double oy = 0.0;
    in.pointer_inside = in.pointer_focus && surface_origin(c->surface, ox, oy) &&
                        c->effective.contains(v.px - ox, v.py - oy);
    in.session_locked = v.locked;
    in.paused = v.paused;
    in.grab = v.grab;
    in.escaped = c->escaped;
    in.defunct = !c->surface;
    return in;
}

void send_state(constraint_obj* c, bool on) {
    if (c->lock) {
        if (on)
            zwp_locked_pointer_v1_send_locked(c->resource);
        else
            zwp_locked_pointer_v1_send_unlocked(c->resource);
    } else {
        if (on)
            zwp_confined_pointer_v1_send_confined(c->resource);
        else
            zwp_confined_pointer_v1_send_unconfined(c->resource);
    }
}

void activate(constraint_obj* c) {
    c->active = true;
    c->region_changed = false; // the pointer is inside the region it activates on
    active = c;
    if (c->lock && g_p0_ctx && g_p0_ctx->input)
        g_p0_ctx->input->set_motion_locked(true);
    send_state(c, true);
    constraint_log(lx::trace::level::info, c, "active");
}

/// Ends `c`'s activation. `send`: the object is told (not from its own destructor, not to
/// a dying client). The lock's hint warp happens only while the surface still has the
/// pointer (a deactivation by focus moving elsewhere must not pull the pointer back).
void deactivate(constraint_obj* c, bool send, const char* why) {
    if (!c->active)
        return;
    const seat_view v = look();
    const bool still_pointed = has_pointer(c, v);
    c->active = false;
    if (active == c)
        active = nullptr;
    if (c->lock && g_p0_ctx && g_p0_ctx->input)
        g_p0_ctx->input->set_motion_locked(false);
    if (send && !client_dying(c->client))
        send_state(c, false);
    constraint_log(lx::trace::level::info, c, why);
    surface_obj* s = c->surface;
    const bool warp = c->lock && c->has_hint && s && still_pointed && !s->destroying;
    const lx::compositor::constraint_point hint = c->hint;
    if (!c->persistent)
        c->surface = nullptr; // spent: inert, a new constraint may be made for the surface
    if (!warp || !g_p0_ctx || !g_p0_ctx->warp_pointer)
        return;
    const lx::compositor::constraint_region area =
        lx::compositor::make_constraint_region(nullptr, s->input_current, s->surface_size);
    double ox = 0.0;
    double oy = 0.0;
    if (lx::compositor::constraint_hint_usable(area, hint) && surface_origin(s, ox, oy))
        g_p0_ctx->warp_pointer(g_p0_ctx->warp_user, ox + hint.x, oy + hint.y);
}

} // namespace

void pointer_constraints_reevaluate() {
    if (!constraints || !g_p0_ctx || !g_p0_ctx->input)
        return;
    if (in_eval) {
        eval_again = true; // a warp below moved focus: decided again with the new state
        return;
    }
    in_eval = true;
    dirty = false;
    // A confinement whose region changed under the pointer: back to the nearest point inside
    // (wl_pointer.motion, no relative motion) before anything is decided.
    if (active && !active->lock && active->region_changed && active->surface &&
        g_p0_ctx->seat && g_p0_ctx->warp_pointer) {
        active->region_changed = false;
        double ox = 0.0;
        double oy = 0.0;
        if (surface_origin(active->surface, ox, oy) && !active->effective.empty()) {
            const lx::compositor::constraint_point p{g_p0_ctx->seat->pointer_x() - ox,
                                                     g_p0_ctx->seat->pointer_y() - oy};
            if (!active->effective.contains(p.x, p.y)) {
                const auto q = lx::compositor::nearest_inside(active->effective, p);
                constraint_log(lx::trace::level::debug, active, "warped inside the new region");
                g_p0_ctx->warp_pointer(g_p0_ctx->warp_user, ox + q.x, oy + q.y);
            }
        }
    }
    for (unsigned round = 0; round < 4; ++round) {
        eval_again = false;
        const seat_view v = look();
        for (constraint_obj* c = constraints; c; c = c->next)
            if (c->escaped && !(has_pointer(c, v) && window_has_keyboard(c, v)))
                c->escaped = false; // left, or lost the keyboard: it may hold again
        if (active && !lx::compositor::constraint_wanted(conditions(active, v), true)) {
            const auto in = conditions(active, v);
            deactivate(active, true,
                       in.session_locked    ? "inactive (session locked)"
                       : in.paused          ? "inactive (session paused)"
                       : in.grab            ? "inactive (grab)"
                       : !in.pointer_focus  ? "inactive (pointer focus left)"
                       : !in.keyboard_focus ? "inactive (keyboard focus left)"
                                            : "inactive");
            if (eval_again)
                continue;
        }
        if (!active && v.pointer_focus) {
            for (constraint_obj* c = constraints; c; c = c->next) {
                if (has_pointer(c, v) && !client_dying(c->client) &&
                    lx::compositor::constraint_wanted(conditions(c, v), false)) {
                    activate(c);
                    break;
                }
            }
        }
        if (!eval_again)
            break;
    }
    in_eval = false;
}

namespace {

void constraint_destroy_resource(wl_resource* r) {
    auto* c = static_cast<constraint_obj*>(wl_resource_get_user_data(r));
    if (!c)
        return;
    // Destroying the object ends the constraint ("If applicable, the compositor will unlock
    // the pointer"): the hint warp still applies, nothing is sent to the dying object.
    if (c->active) {
        deactivate(c, false, "destroyed");
        c->surface = nullptr;
    }
    if (c->prev)
        c->prev->next = c->next;
    else
        constraints = c->next;
    if (c->next)
        c->next->prev = c->prev;
    delete c;
    // Another constraint of the surface cannot exist; others may now apply elsewhere.
    pointer_constraints_reevaluate();
}

constraint_obj* constraint_from(wl_resource* r) {
    return static_cast<constraint_obj*>(wl_resource_get_user_data(r));
}

void stage_region(wl_resource* resource, wl_resource* region) {
    constraint_obj* c = constraint_from(resource);
    if (!c)
        return;
    c->pending_region_set = true;
    c->pending_has_region = false;
    if (const auto* r = region ? static_cast<region_obj*>(wl_resource_get_user_data(region))
                               : nullptr) {
        c->pending_region = r->region;
        c->pending_has_region = true;
    }
}

const struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_cursor_position_hint =
        [](struct wl_client*, struct wl_resource* r, wl_fixed_t x, wl_fixed_t y) {
            if (constraint_obj* c = constraint_from(r)) {
                c->pending_hint = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
                c->pending_hint_set = true;
            }
        },
    .set_region = [](struct wl_client*, struct wl_resource* r,
                     struct wl_resource* region) { stage_region(r, region); },
};

const struct zwp_confined_pointer_v1_interface confined_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_region = [](struct wl_client*, struct wl_resource* r,
                     struct wl_resource* region) { stage_region(r, region); },
};

void create_constraint(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                       struct wl_resource* surface_res, struct wl_resource* region,
                       uint32_t lifetime, bool lock) {
    surface_obj* s = surface_from(surface_res);
    unsigned held = 0;
    for (constraint_obj* c = constraints; c; c = c->next) {
        if (s && c->surface == s) {
            // One seat: a constraint of the surface is one of the surface and seat.
            wl_resource_post_error(manager, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                                   "the surface already has a pointer constraint");
            return;
        }
        if (c->client == client && c->surface)
            ++held;
    }
    auto* c = new (std::nothrow) constraint_obj{};
    wl_resource* res = nullptr;
    if (c)
        res = wl_resource_create(client,
                                 lock ? &zwp_locked_pointer_v1_interface
                                      : &zwp_confined_pointer_v1_interface,
                                 wl_resource_get_version(manager), id);
    if (!res) {
        delete c;
        wl_client_post_no_memory(client);
        return;
    }
    c->resource = res;
    c->client = client;
    c->lock = lock;
    c->persistent = lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
    c->surface = held < k_max_per_client ? s : nullptr;
    if (const auto* r = region ? static_cast<region_obj*>(wl_resource_get_user_data(region))
                               : nullptr) {
        c->region = r->region;
        c->has_region = true;
    }
    recompute(c);
    c->next = constraints;
    if (constraints)
        constraints->prev = c;
    constraints = c;
    if (lock)
        wl_resource_set_implementation(res, &locked_impl, c, constraint_destroy_resource);
    else
        wl_resource_set_implementation(res, &confined_impl, c, constraint_destroy_resource);
    constraint_log(lx::trace::level::info, c, c->surface ? "created" : "created inert (limit)");
    pointer_constraints_reevaluate();
}

const struct zwp_pointer_constraints_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .lock_pointer = [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
                       struct wl_resource* surface, struct wl_resource* /*pointer*/,
                       struct wl_resource* region, uint32_t lifetime) {
        create_constraint(client, resource, id, surface, region, lifetime, true);
    },
    .confine_pointer = [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
                          struct wl_resource* surface, struct wl_resource* /*pointer*/,
                          struct wl_resource* region, uint32_t lifetime) {
        create_constraint(client, resource, id, surface, region, lifetime, false);
    },
};

} // namespace

void bind_pointer_constraints(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_pointer_constraints_v1_interface, version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

void pointer_constraints_surface_committed(surface_obj* s, unsigned seq) {
    for (constraint_obj* c = constraints; c; c = c->next) {
        if (c->surface != s || (!c->pending_region_set && !c->pending_hint_set))
            continue;
        // A fifth commit still waiting merges into the newest: it then applies with this
        // later commit's update — late by a frame or two, never early.
        staged_state& st = c->staged_count < k_staged ? c->staged[c->staged_count++]
                                                      : c->staged[k_staged - 1];
        st.seq = seq;
        if (c->pending_region_set) {
            st.region_set = true;
            st.has_region = c->pending_has_region;
            st.region = c->pending_region;
            c->pending_region_set = false;
        }
        if (c->pending_hint_set) {
            st.hint_set = true;
            st.hint = c->pending_hint;
            c->pending_hint_set = false;
        }
    }
}

void pointer_constraints_surface_applied(surface_obj* s, unsigned seq) {
    // State only: the decision (and a warp it may need) waits for the end of the commit
    // request (`pointer_constraints_flush`) or of the loop iteration, never runs in the
    // middle of applying an update.
    for (constraint_obj* c = constraints; c; c = c->next) {
        if (c->surface != s)
            continue;
        // What the commits this update carries staged, in commit order; later commits'
        // state waits for their own updates.
        unsigned kept = 0;
        for (unsigned i = 0; i < c->staged_count; ++i) {
            const staged_state& st = c->staged[i];
            if (seq == 0 || static_cast<int>(st.seq - seq) > 0) {
                c->staged[kept++] = st;
                continue;
            }
            if (st.region_set) {
                c->region = st.region;
                c->has_region = st.has_region;
            }
            if (st.hint_set) {
                c->hint = st.hint;
                c->has_hint = true;
            }
        }
        c->staged_count = kept;
        recompute(c);
        c->region_changed = true;
        dirty = true;
    }
}

void pointer_constraints_flush() {
    if (dirty)
        pointer_constraints_reevaluate();
}

void pointer_constraints_surface_destroyed(surface_obj* s) {
    for (constraint_obj* c = constraints; c; c = c->next) {
        if (c->surface != s)
            continue;
        if (c->active)
            deactivate(c, true, "inactive (surface destroyed)");
        c->surface = nullptr;
        c->effective = {};
    }
}

bool pointer_constraints_escape() {
    constraint_obj* c = active;
    if (!c)
        return false;
    c->escaped = true;
    deactivate(c, true, "released by the user (Super+Escape)");
    pointer_constraints_reevaluate();
    return true;
}

int pointer_constraints_filter(double from_x, double from_y, double& to_x, double& to_y) {
    constraint_obj* c = active;
    if (!c || !c->surface)
        return 0;
    if (c->lock)
        return 1;
    double ox = 0.0;
    double oy = 0.0;
    if (!surface_origin(c->surface, ox, oy) || c->effective.empty())
        return 0;
    const auto r = lx::compositor::confine_motion(c->effective, {from_x - ox, from_y - oy},
                                                  {to_x - ox, to_y - oy});
    to_x = r.x + ox;
    to_y = r.y + oy;
    return 2;
}

int pointer_constraints_active_kind() { return active ? (active->lock ? 1 : 2) : 0; }

#else

void pointer_constraints_reevaluate() {}
void pointer_constraints_surface_committed(surface_obj*, unsigned) {}
void pointer_constraints_surface_applied(surface_obj*, unsigned) {}
void pointer_constraints_flush() {}
void pointer_constraints_surface_destroyed(surface_obj*) {}
bool pointer_constraints_escape() { return false; }
int pointer_constraints_filter(double, double, double&, double&) { return 0; }
int pointer_constraints_active_kind() { return 0; }

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

lx::compositor::pointer_constraint_kind
lx::compositor::pointer_constraint_motion(p0_protocol_context& ctx, double from_x,
                                          double from_y, double& to_x, double& to_y) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    switch (p0::pointer_constraints_filter(from_x, from_y, to_x, to_y)) {
    case 1: return pointer_constraint_kind::locked;
    case 2: return pointer_constraint_kind::confined;
    default: return pointer_constraint_kind::none;
    }
#else
    (void)from_x;
    (void)from_y;
    (void)to_x;
    (void)to_y;
    return pointer_constraint_kind::none;
#endif
}

void lx::compositor::pointer_constraints_update(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::pointer_constraints_reevaluate();
#endif
}

bool lx::compositor::pointer_constraints_release(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::pointer_constraints_escape();
#else
    return false;
#endif
}

lx::compositor::pointer_constraint_kind
lx::compositor::pointer_constraint_active(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    switch (p0::pointer_constraints_active_kind()) {
    case 1: return pointer_constraint_kind::locked;
    case 2: return pointer_constraint_kind::confined;
    default: return pointer_constraint_kind::none;
    }
#else
    return pointer_constraint_kind::none;
#endif
}
