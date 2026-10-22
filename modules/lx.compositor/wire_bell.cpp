module;

// xdg-system-bell-v1 (staging): a client rings the bell (a terminal's `\a`, a toolkit's
// error beep), for one of its surfaces or for none (design-app-compat-capture B.3, B1). The
// compositor resolves the window — the toplevel owning the surface's tree, a popup's
// toplevel, nothing for other roles — and forwards it to the shell (zlm_policy_bridge_v1 v6
// `bell`), which gives the feedback: a visual flash, as Mutter flashes the window or the
// whole screen (meta_bell_notify). The compositor itself plays no sound.
//
// Bounded: a token bucket of 4 rings per second per client (a `yes $'\a'` in a terminal must
// neither strobe the screen nor flood the shell's connection), and nothing while the
// session is locked (a bell rung behind the lock would reveal which window rang it).
// The protocol defines no errors.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1)
#include "xdg-system-bell-v1-server-protocol.h"
#define LUMEN_HAS_SYSTEM_BELL 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :surface;
import :surface_tree;
import :toplevel;
import :popup;
import :session_lock;
import :wire;

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_SYSTEM_BELL)

namespace lx::compositor::p0 {

namespace {

constexpr unsigned k_bell_burst = 4;
constexpr unsigned long long k_bell_refill_ns = 250'000'000ull; // 4 per second

/// One token bucket per recent client (lx client ids are never reused); the least recently
/// rung slot is taken over by a new client.
struct bell_bucket {
    unsigned client = 0;
    unsigned tokens = 0;
    unsigned long long refilled_ns = 0;
    unsigned long long used_ns = 0;
};
bell_bucket buckets[16]{};
unsigned dropped_logged_client = 0;

unsigned long long now_ns() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
}

/// Takes a token of `client`'s bucket; false when it is empty.
bool take_token(unsigned client, unsigned long long now) {
    bell_bucket* b = nullptr;
    bell_bucket* oldest = &buckets[0];
    for (auto& e : buckets) {
        if (e.client == client) {
            b = &e;
            break;
        }
        if (e.used_ns < oldest->used_ns)
            oldest = &e;
    }
    if (!b) {
        b = oldest;
        *b = {client, k_bell_burst, now, now};
    }
    if (now > b->refilled_ns) {
        const unsigned long long add = (now - b->refilled_ns) / k_bell_refill_ns;
        if (add > 0) {
            b->tokens = add >= k_bell_burst || b->tokens + add >= k_bell_burst
                            ? k_bell_burst
                            : b->tokens + static_cast<unsigned>(add);
            b->refilled_ns += add * k_bell_refill_ns;
        }
    }
    b->used_ns = now;
    if (b->tokens == 0)
        return false;
    --b->tokens;
    return true;
}

/// The toplevel a bell for `surface` names: the window whose tree it is in, a popup's
/// window; null for a layer surface, a cursor, a lock surface or no surface at all.
const lx::compositor::toplevel_record* bell_window(wl_resource* surface_res) {
    surface_obj* s = surface_from(surface_res);
    if (!s || !g_p0_ctx->toplevels)
        return nullptr;
    lx::surface_id root = g_p0_ctx->tree ? g_p0_ctx->tree->root_of(s->id) : s->id;
    if (!root)
        root = s->id;
    if (const auto* rec = g_p0_ctx->toplevels->find_by_surface(root))
        return rec;
    if (g_p0_ctx->popups)
        if (const auto* p = g_p0_ctx->popups->find(root); p && p->owner)
            return g_p0_ctx->toplevels->find(p->owner);
    return nullptr;
}

void bell_ring(struct wl_client* client, struct wl_resource*, struct wl_resource* surface_res) {
    if (!g_p0_ctx || !g_p0_ctx->shell)
        return;
    const unsigned cid = wire_client_id(client);
    const char* result = "sent";
    const lx::compositor::toplevel_record* rec = bell_window(surface_res);
    if (g_p0_ctx->lock && g_p0_ctx->lock->state() != lx::compositor::lock_state::unlocked)
        result = "locked";
    else if (!take_token(cid, now_ns()))
        result = "rate-limited";
    else
        g_p0_ctx->shell->emit_bell(rec && rec->mapped ? rec : nullptr);
    // One line per ring at debug; a client that hits the limit is noted once at info.
    if (surface_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "bell client=%u toplevel=%llu result=%s", cid,
                      rec ? rec->id.id() : 0ull, result);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.bell", line);
    } else if (result[0] == 'r' && dropped_logged_client != cid) {
        dropped_logged_client = cid;
        char line[96];
        std::snprintf(line, sizeof(line), "bell client=%u rate-limited (4 per second)", cid);
        lx::trace::logger::global().log(lx::trace::level::info, "compositor.bell", line);
    }
}

const struct xdg_system_bell_v1_interface bell_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .ring = &bell_ring,
};

} // namespace

void bind_system_bell(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &xdg_system_bell_v1_interface, version, id);
    if (res)
        res.set_implementation(&bell_impl, g_p0_ctx, nullptr);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND && LUMEN_HAS_SYSTEM_BELL
