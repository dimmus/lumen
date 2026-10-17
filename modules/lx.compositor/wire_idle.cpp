module;

// ext_idle_notifier_v1 (ext-idle-notify-v1.xml, staging, v2) and zwp_idle_inhibit_manager_v1
// (idle-inhibit-unstable-v1.xml, v1). Part of the protocol wire split; shared declarations
// live in the :wire partition (wire.cppm), the bookkeeping in :idle (pure, tested on its own).
//
// Any client may bind both (sway, niri and KWin expose them alike; same-UID clients are not
// isolated from each other until sandboxing exists — design X1). lumen-shell's idle policy and
// external daemons (swayidle, hypridle) are ordinary clients of the notifier.
//
// Notifications: `get_idle_notification` obeys idle inhibitors, `get_input_idle_notification`
// (v2) counts only input and ignores them. Lumen has one seat, so the wl_seat argument names
// it whichever wl_seat object it is. `idled` and `resumed` alternate by construction (the
// tracker's per-notification flag guards both).
//
// Inhibitors: honoured only while their surface is visible — on screen in the last published
// frame, not covered by an opaque window, on a running display (the visibility pass; the
// compositor evaluates them after each publish). A subsurface counts through its own node.
// An inhibitor whose wl_surface is destroyed stays as an inert object (surface ids are never
// reused, so it simply never becomes visible again).
//
// Protocol errors: neither protocol defines any. A client past its share of the fixed tables
// (16 notifications or inhibitors, 64 in the session) is sent no_memory, logged once.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>
#include <ctime>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if __has_include("ext-idle-notify-v1-server-protocol.h")
#include "ext-idle-notify-v1-server-protocol.h"
#define LUMEN_HAS_IDLE_NOTIFY 1
#endif
#if __has_include("idle-inhibit-unstable-v1-server-protocol.h")
#include "idle-inhibit-unstable-v1-server-protocol.h"
#define LUMEN_HAS_IDLE_INHIBIT 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :idle;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

[[nodiscard]] unsigned long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

[[nodiscard]] bool idle_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void idle_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, "compositor.idle", line);
}

/// The tracker's next deadline may have moved, or inhibition must be looked at again.
void changed() {
    if (g_p0_ctx && g_p0_ctx->idle_changed)
        g_p0_ctx->idle_changed(g_p0_ctx->idle_user);
}

/// A client past its share is refused once per kind with a warning (never silently).
bool g_warned_notify_full = false;
bool g_warned_inhibit_full = false;

#if defined(LUMEN_HAS_IDLE_NOTIFY)

void send_idled(void*, void* resource) {
    auto* r = static_cast<wl_resource*>(resource);
    if (client_dying(wl_resource_get_client(r)))
        return;
    ext_idle_notification_v1_send_idled(r);
    if (idle_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "idle notification idled id=%u",
                      wl_resource_get_id(r));
        idle_log(lx::trace::level::debug, line);
    }
}

void send_resumed(void*, void* resource) {
    auto* r = static_cast<wl_resource*>(resource);
    if (client_dying(wl_resource_get_client(r)))
        return;
    ext_idle_notification_v1_send_resumed(r);
    if (idle_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "idle notification resumed id=%u",
                      wl_resource_get_id(r));
        idle_log(lx::trace::level::debug, line);
    }
}

#endif

[[nodiscard]] lx::compositor::idle_sink sink() {
    lx::compositor::idle_sink k{};
#if defined(LUMEN_HAS_IDLE_NOTIFY)
    k.idled = &send_idled;
    k.resumed = &send_resumed;
#endif
    return k;
}

#if defined(LUMEN_HAS_IDLE_NOTIFY)

/// The notification's destructor: destroy request or client teardown. A removal only moves
/// deadlines later, so the lazily armed timer needs nothing (it fires early, once, at most).
void notification_destroy_resource(wl_resource* r) {
    if (g_p0_ctx && g_p0_ctx->idle)
        g_p0_ctx->idle->remove(r);
}

const struct ext_idle_notification_v1_interface notification_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void create_notification(struct wl_client* client, struct wl_resource* notifier, uint32_t id,
                         uint32_t timeout_ms, struct wl_resource* seat, bool obey) {
    (void)seat; // one seat: every wl_seat object names it
    wl_resource* r = wl_resource_create(client, &ext_idle_notification_v1_interface,
                                        wl_resource_get_version(notifier), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &notification_impl, nullptr,
                                   &notification_destroy_resource);
    auto* t = g_p0_ctx ? g_p0_ctx->idle : nullptr;
    if (!t)
        return; // inert
    if (!t->add(r, client, timeout_ms, obey, monotonic_ns(), sink())) {
        if (!g_warned_notify_full) {
            g_warned_notify_full = true;
            idle_log(lx::trace::level::warn,
                     "idle notification refused: the client's share (16) or the session's "
                     "table (64) is full — no_memory (reported once)");
        }
        wl_client_post_no_memory(client);
        return;
    }
    if (idle_debug()) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "idle notification create id=%u timeout_ms=%u obey_inhibitors=%d "
                      "count=%u inhibited=%d",
                      id, timeout_ms, obey ? 1 : 0, t->count(), t->inhibited() ? 1 : 0);
        idle_log(lx::trace::level::debug, line);
    }
    changed();
}

const struct ext_idle_notifier_v1_interface notifier_impl = {
    // Notifications created from it live on ("All objects created via this interface remain
    // valid").
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_idle_notification =
        [](struct wl_client* client, struct wl_resource* notifier, uint32_t id, uint32_t timeout,
           struct wl_resource* seat) {
            create_notification(client, notifier, id, timeout, seat, true);
        },
    .get_input_idle_notification =
        [](struct wl_client* client, struct wl_resource* notifier, uint32_t id, uint32_t timeout,
           struct wl_resource* seat) {
            create_notification(client, notifier, id, timeout, seat, false);
        },
};

#endif

#if defined(LUMEN_HAS_IDLE_INHIBIT)

void inhibitor_destroy_resource(wl_resource* r) {
    if (g_p0_ctx && g_p0_ctx->inhibitors) {
        const unsigned before = g_p0_ctx->inhibitors->count();
        g_p0_ctx->inhibitors->remove(r);
        if (g_p0_ctx->inhibitors->count() != before) {
            if (idle_debug()) {
                char line[96];
                std::snprintf(line, sizeof(line), "idle inhibitor destroy id=%u count=%u",
                              wl_resource_get_id(r), g_p0_ctx->inhibitors->count());
                idle_log(lx::trace::level::debug, line);
            }
            changed();
        }
    }
}

const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void create_inhibitor(struct wl_client* client, struct wl_resource* manager, uint32_t id,
                      struct wl_resource* surface_res) {
    wl_resource* r = wl_resource_create(client, &zwp_idle_inhibitor_v1_interface,
                                        wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &inhibitor_impl, nullptr, &inhibitor_destroy_resource);
    surface_obj* s = surface_from(surface_res);
    auto* set = g_p0_ctx ? g_p0_ctx->inhibitors : nullptr;
    if (!s || !set)
        return; // inert
    if (!set->add(r, client, s->id)) {
        if (!g_warned_inhibit_full) {
            g_warned_inhibit_full = true;
            idle_log(lx::trace::level::warn,
                     "idle inhibitor refused: the client's share (16) or the session's table "
                     "(64) is full — no_memory (reported once)");
        }
        wl_client_post_no_memory(client);
        return;
    }
    if (idle_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "idle inhibitor create id=%u surface=%llu count=%u",
                      id, static_cast<unsigned long long>(s->id.id()), set->count());
        idle_log(lx::trace::level::debug, line);
    }
    changed();
}

const struct zwp_idle_inhibit_manager_v1_interface inhibit_manager_impl = {
    // Inhibitors created from it live on.
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .create_inhibitor = &create_inhibitor,
};

#endif

} // namespace

#if defined(LUMEN_HAS_IDLE_NOTIFY)
void bind_idle_notifier(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &ext_idle_notifier_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&notifier_impl, g_p0_ctx, nullptr);
}
#endif

#if defined(LUMEN_HAS_IDLE_INHIBIT)
void bind_idle_inhibit_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &zwp_idle_inhibit_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&inhibit_manager_impl, g_p0_ctx, nullptr);
}
#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

lx::compositor::idle_sink lx::compositor::idle_events(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::sink();
#else
    return {};
#endif
}

bool lx::compositor::idle_inhibitor_visible(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.inhibitors || ctx.inhibitors->count() == 0)
        return false;
    // On screen on a lit output (a blanked monitor's windows inhibit nothing).
    return ctx.inhibitors->any(
        [](void* user, lx::surface_id surface) {
            const auto* c = static_cast<const p0_protocol_context*>(user);
            return (p0::surface_shown_mask(surface) & c->lit_mask) != 0;
        },
        &ctx);
#else
    (void)ctx;
    return false;
#endif
}
