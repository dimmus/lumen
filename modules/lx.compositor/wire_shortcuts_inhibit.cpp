module;

// keyboard-shortcuts-inhibit-unstable-v1: a virtual machine, a remote desktop viewer or a
// game asks to receive the keys the desktop would take (design-app-compat-capture B.5, K1).
// Granted without asking, as sway, niri and KWin do (GNOME asks with a dialog): the request
// is for one surface, and only takes effect while that surface has the keyboard.
//
// While an inhibitor is in effect the shell's key grabs and modifier taps are skipped and
// their keys reach the client — except grabs flagged `not_inhibitable` (zlm v6; the shell's
// lock key). The compositor's own ways out are never inhibited: Ctrl+Alt+F<n> (VT switch),
// Ctrl+Alt+Backspace (logout), Escape ending a move/resize/drag, and Super+Escape, which
// toggles the focused surface's inhibitor off (`inactive`) and on again (`active`) — niri's
// and GNOME's key. The session lock gives the keyboard to the lock surface, so nothing is
// inhibited while locked.
//
// Events: `active` every time an inhibitor comes into effect (its surface gains the
// keyboard, the user re-enables it); `inactive` only when the user turns it off — "If the
// surface is destroyed, unmapped, or loses the seat's keyboard focus, the keyboard
// shortcuts inhibitor becomes irrelevant ... but no 'inactive' event is emitted".
//
// Every error:
//   zwp_keyboard_shortcuts_inhibit_manager_v1.already_inhibited  a live inhibitor exists
//                                                                for that surface and seat
// Inhibitors are allocated per request (16 per client take effect; more are inert) and go
// inert with their surface.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1)
#include "keyboard-shortcuts-inhibit-unstable-v1-server-protocol.h"
#define LUMEN_HAS_SHORTCUTS_INHIBIT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :session_lock;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_SHORTCUTS_INHIBIT)

namespace {

constexpr unsigned k_max_per_client = 16;
constexpr const char* k_category = "compositor.inhibit";

struct inhibitor_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    /// Null: inert (its surface went, or the client already had too many).
    surface_obj* surface = nullptr;
    /// The user has not turned it off with Super+Escape.
    bool enabled = true;
    /// In effect now (`active` was sent for this period).
    bool active = false;
    inhibitor_obj* prev = nullptr;
    inhibitor_obj* next = nullptr;
};

inhibitor_obj* inhibitors = nullptr;
/// The surface with the keyboard (the router's focus-surface hook).
wl_resource* focus_surface = nullptr;

bool locked() {
    return g_p0_ctx && g_p0_ctx->lock &&
           g_p0_ctx->lock->state() != lx::compositor::lock_state::unlocked;
}

void inhibit_log(lx::trace::level lvl, const inhibitor_obj* i, const char* what) {
    if (lvl == lx::trace::level::debug && !surface_debug())
        return;
    lx::trace::logger::global().logf(lvl, k_category, "shortcuts inhibitor client={} surface={} {}",
                                     wire_client_id(i->client),
                                     i->surface ? i->surface->id.id() : 0ull, what);
}

/// Sends `active` to the inhibitor that comes into effect; one that leaves it just stops.
void reevaluate() {
    const bool lock = locked();
    for (inhibitor_obj* i = inhibitors; i; i = i->next) {
        const bool effective = i->surface && i->enabled && !lock && focus_surface &&
                               i->surface->resource == focus_surface;
        if (effective && !i->active) {
            i->active = true;
            zwp_keyboard_shortcuts_inhibitor_v1_send_active(i->resource);
            inhibit_log(lx::trace::level::debug, i, "active");
        } else if (!effective && i->active) {
            i->active = false;
            inhibit_log(lx::trace::level::debug, i, "irrelevant");
        }
    }
}

inhibitor_obj* focused_inhibitor() {
    if (!focus_surface)
        return nullptr;
    for (inhibitor_obj* i = inhibitors; i; i = i->next)
        if (i->surface && i->surface->resource == focus_surface)
            return i;
    return nullptr;
}

void inhibitor_destroy_resource(wl_resource* r) {
    auto* i = static_cast<inhibitor_obj*>(wl_resource_get_user_data(r));
    if (!i)
        return;
    if (i->prev)
        i->prev->next = i->next;
    else
        inhibitors = i->next;
    if (i->next)
        i->next->prev = i->prev;
    delete i;
}

const struct zwp_keyboard_shortcuts_inhibitor_v1_interface inhibitor_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void manager_inhibit(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                     struct wl_resource* surface_res, struct wl_resource* /*seat*/) {
    surface_obj* s = surface_from(surface_res);
    unsigned held = 0;
    for (inhibitor_obj* i = inhibitors; i; i = i->next) {
        if (s && i->surface == s) {
            // One seat: an inhibitor of the surface is one of the surface and seat.
            wl_resource_post_error(resource,
                                   ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED,
                                   "shortcuts are already inhibited for this surface");
            return;
        }
        if (i->client == client && i->surface)
            ++held;
    }
    auto* inh = new (std::nothrow) inhibitor_obj{};
    wl_resource* res = inh ? wl_resource_create(client, &zwp_keyboard_shortcuts_inhibitor_v1_interface,
                                                wl_resource_get_version(resource), id)
                           : nullptr;
    if (!res) {
        delete inh;
        wl_client_post_no_memory(client);
        return;
    }
    inh->resource = res;
    inh->client = client;
    inh->surface = held < k_max_per_client ? s : nullptr;
    inh->next = inhibitors;
    if (inhibitors)
        inhibitors->prev = inh;
    inhibitors = inh;
    wl_resource_set_implementation(res, &inhibitor_impl, inh, inhibitor_destroy_resource);
    inhibit_log(lx::trace::level::info, inh, inh->surface ? "created" : "created inert (limit)");
    reevaluate();
}

const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .inhibit_shortcuts = &manager_inhibit,
};

} // namespace

void bind_shortcuts_inhibit_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface,
                                         version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

void shortcuts_inhibit_focus(void* surface) {
    focus_surface = static_cast<wl_resource*>(surface);
    if (inhibitors)
        reevaluate();
}

void shortcuts_inhibit_surface_destroyed(surface_obj* s) {
    if (focus_surface == s->resource)
        focus_surface = nullptr;
    for (inhibitor_obj* i = inhibitors; i; i = i->next) {
        if (i->surface == s) {
            i->surface = nullptr;
            i->active = false;
        }
    }
}

bool shortcuts_inhibit_effective() {
    if (!inhibitors)
        return false;
    const inhibitor_obj* i = focused_inhibitor();
    return i && i->active && i->enabled && !locked();
}

bool shortcuts_inhibit_user_toggle() {
    inhibitor_obj* i = focused_inhibitor();
    if (!i || locked())
        return false;
    if (i->enabled) {
        i->enabled = false;
        i->active = false;
        zwp_keyboard_shortcuts_inhibitor_v1_send_inactive(i->resource);
        inhibit_log(lx::trace::level::info, i, "turned off by the user (Super+Escape)");
    } else {
        i->enabled = true;
        inhibit_log(lx::trace::level::info, i, "turned on by the user (Super+Escape)");
        reevaluate();
    }
    return true;
}

#else

void shortcuts_inhibit_focus(void*) {}
void shortcuts_inhibit_surface_destroyed(surface_obj*) {}
bool shortcuts_inhibit_effective() { return false; }
bool shortcuts_inhibit_user_toggle() { return false; }

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

bool lx::compositor::shortcuts_inhibited(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::shortcuts_inhibit_effective();
#else
    return false;
#endif
}

bool lx::compositor::shortcuts_inhibit_toggle(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::shortcuts_inhibit_user_toggle();
#else
    return false;
#endif
}
