module;

// fifo-v1 and commit-timing-v1 (staging): the display-refresh and target-time constraints
// of a content update, which Mesa's Vulkan WSI uses for FIFO present (set_barrier +
// wait_barrier every frame; a timestamp for present timing). Both only set double-buffered
// flags here; the content-update queue (wire_surface.cpp, `:content_update`) holds updates
// back on them, and the frame clocks release them (design-app-compat-capture §A.3, §A.4):
//   - the barrier set by an applied update clears after the next latch of the surface's
//     pace output, so a waiting update joins the following frame: one update per refresh;
//   - an update with a target time joins the first frame predicted to present no earlier
//     than 0.5 ms before it; its output's clock is armed for that tick.
//   - a surface not on screen ignores both (the protocols allow it; frame callbacks throttle
//     it), and a synchronized subsurface's wait is dropped at merge.
//
// Every error:
//   wp_fifo_manager_v1.already_exists          get_fifo on a surface that has one
//   wp_fifo_v1.surface_destroyed               set_barrier / wait_barrier after the surface
//   wp_commit_timing_manager_v1.commit_timer_exists  get_timer on a surface that has one
//   wp_commit_timer_v1.invalid_timestamp       tv_nsec ≥ 10⁹
//   wp_commit_timer_v1.timestamp_exists        a second set_timestamp before the commit
//   wp_commit_timer_v1.surface_destroyed       set_timestamp after the surface
//
// Also the add-on object plumbing every commit-path protocol shares (`addon_create`).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_FIFO_V1)
#include "fifo-v1-server-protocol.h"
#define LUMEN_HAS_FIFO 1
#endif
#if defined(LUMEN_PROTOCOL_COMMIT_TIMING_V1)
#include "commit-timing-v1-server-protocol.h"
#define LUMEN_HAS_COMMIT_TIMING 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import :surface;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

// ── add-on objects (every commit-path protocol) ─────────────────────────────

surface_addon* addon_create(wl_client* client, const wl_interface* iface, int version,
                            uint32_t id, surface_obj* s, surface_addon** slot, const void* impl,
                            void (*destroy)(wl_resource*)) {
    auto* a = new (std::nothrow) surface_addon{};
    wl_resource* res = a ? wl_resource_create(client, iface, version, id) : nullptr;
    if (!res) {
        delete a;
        wl_client_post_no_memory(client);
        return nullptr;
    }
    a->resource = res;
    a->surface = s;
    a->slot = slot;
    *slot = a;
    wl_resource_set_implementation(res, impl, a, destroy);
    return a;
}

surface_addon* addon_from(wl_resource* resource) {
    return resource ? static_cast<surface_addon*>(wl_resource_get_user_data(resource)) : nullptr;
}

void addon_free(surface_addon* a) {
    if (!a)
        return;
    if (a->surface && a->slot && *a->slot == a)
        *a->slot = nullptr;
    delete a;
}

void surface_addons_destroyed(surface_obj* s) {
    surface_addon* addons[] = {s->fifo,  s->timer,     s->tearing_ctl, s->content_type_ctl,
                               s->alpha_ctl, s->syncobj};
    for (surface_addon* a : addons)
        if (a) {
            a->surface = nullptr;
            a->slot = nullptr;
        }
    s->fifo = s->timer = s->tearing_ctl = s->content_type_ctl = s->alpha_ctl = s->syncobj =
        nullptr;
}

namespace {

void addon_destroy_plain(wl_resource* r) { addon_free(addon_from(r)); }

#if defined(LUMEN_HAS_FIFO)

// ── wp_fifo_v1 ──────────────────────────────────────────────────────────────

const struct wp_fifo_v1_interface fifo_impl = {
    .set_barrier =
        [](struct wl_client*, struct wl_resource* r) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                                       "set_barrier after the wl_surface was destroyed");
                return;
            }
            a->surface->pending.fifo_set = true;
        },
    .wait_barrier =
        [](struct wl_client*, struct wl_resource* r) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                                       "wait_barrier after the wl_surface was destroyed");
                return;
            }
            a->surface->pending.fifo_wait = true;
        },
    // "Surface state changes previously made by this protocol are unaffected."
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct wp_fifo_manager_v1_interface fifo_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_fifo =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->fifo) {
                wl_resource_post_error(r, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
                                       "the surface already has a wp_fifo_v1");
                return;
            }
            (void)addon_create(client, &wp_fifo_v1_interface, wl_resource_get_version(r), id, s,
                               &s->fifo, &fifo_impl, addon_destroy_plain);
        },
};

#endif

#if defined(LUMEN_HAS_COMMIT_TIMING)

// ── wp_commit_timer_v1 ──────────────────────────────────────────────────────

const struct wp_commit_timer_v1_interface timer_impl = {
    .set_timestamp =
        [](struct wl_client*, struct wl_resource* r, uint32_t sec_hi, uint32_t sec_lo,
           uint32_t nsec) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
                                       "set_timestamp after the wl_surface was destroyed");
                return;
            }
            if (nsec >= 1'000'000'000u) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
                                       "tv_nsec is not below one second");
                return;
            }
            surface_pending& p = a->surface->pending;
            if (p.target_ns != 0) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
                                       "a timestamp is already set for this commit");
                return;
            }
            // CLOCK_MONOTONIC, wp_presentation's clock. A target 0 would read as "none": the
            // earliest representable instant is as good as now.
            const unsigned long long sec =
                (static_cast<unsigned long long>(sec_hi) << 32) | sec_lo;
            const unsigned long long ns =
                sec > 18'000'000'000ull ? ~0ull : sec * 1'000'000'000ull + nsec;
            p.target_ns = ns ? ns : 1;
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

const struct wp_commit_timing_manager_v1_interface timing_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_timer =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->timer) {
                wl_resource_post_error(r, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                                       "the surface already has a wp_commit_timer_v1");
                return;
            }
            (void)addon_create(client, &wp_commit_timer_v1_interface, wl_resource_get_version(r),
                               id, s, &s->timer, &timer_impl, addon_destroy_plain);
        },
};

#endif

} // namespace

#if defined(LUMEN_HAS_FIFO)
void bind_fifo_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_fifo_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&fifo_manager_impl, g_p0_ctx, nullptr);
}
#endif

#if defined(LUMEN_HAS_COMMIT_TIMING)
void bind_commit_timing_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wp_commit_timing_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&timing_manager_impl, g_p0_ctx, nullptr);
}
#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
