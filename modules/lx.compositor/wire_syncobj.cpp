module;

// linux-drm-syncobj-v1 (staging): explicit synchronization (design-app-compat-capture §A.1,
// S1–S4). A client imports DRM timeline syncobjs and, with every buffer it attaches, sets an
// acquire point (the compositor must not sample before it signals) and a release point (the
// compositor signals it when done with the buffer).
//   - Offered only where it works end to end (`p0_protocol_context::syncobj_supported`:
//     timeline syncobjs, DRM_IOCTL_SYNCOBJ_EVENTFD, and a backend whose composite exports a
//     completion fence) — wlroots and Mutter hide it otherwise, and so does this host
//     (vmwgfx has no syncobjs).
//   - Acquire: the content update is held in the queue until the point has *signalled* —
//     queried first, then an eventfd from DRM_IOCTL_SYNCOBJ_EVENTFD watched by the readiness
//     poller (wire_surface.cpp) — so the composite never waits on a client's GPU work
//     (Mutter, KWin; wlroots waits only for materialization and lets the GPU wait).
//   - Release: a buffer that is never sampled (superseded, dropped, its surface gone) has
//     its point signalled at once; a sampled one when the buffer lifecycle retires it — only
//     once a newer commit superseded it and the last frame that could sample it completed —
//     with the newest composite fence attached (render's `release_fence_mailbox`, published
//     after output composites and after capture copies): the GPU runs them in order, so
//     that fence covers every read. A destroyed wl_buffer still on screen hands its point to
//     the lifecycle record (`syncobj_point_settle`). Nothing waits on the CPU.
//   - Only linux-dmabuf buffers: shm and single-pixel buffers are `unsupported_buffer`
//     (Mutter's rule; shm is copied at apply, a fence on it means nothing).
//
// Every error:
//   wp_linux_drm_syncobj_manager_v1.surface_exists    get_surface for a surface that has one
//   wp_linux_drm_syncobj_manager_v1.invalid_timeline  import_timeline of a non-syncobj fd
//   wp_linux_drm_syncobj_surface_v1.no_surface        set_*_point after the wl_surface
//   wp_linux_drm_syncobj_surface_v1.unsupported_buffer / no_buffer / no_acquire_point /
//     no_release_point / conflicting_points            at commit (`syncobj_precheck`)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_LINUX_DRM_SYNCOBJ_V1)
#include "linux-drm-syncobj-v1-server-protocol.h"
#define LUMEN_HAS_SYNCOBJ 1
#endif
#endif

#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <signal.h>

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import :surface;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

lx::gfx::drm_timeline_table g_timelines{};
/// Live wp_linux_drm_syncobj_surface_v1 objects: while there are none, render publishes no
/// composite fences for release points.
unsigned g_syncobj_surfaces = 0;

void note_syncobj_surfaces() {
    if (g_p0_ctx && g_p0_ctx->release_fences)
        g_p0_ctx->release_fences->set_wanted(g_syncobj_surfaces > 0);
}

void log_timeline_error(const lx::error& err) {
    static unsigned suppressed = 0;
    if (suppressed++ % 600 == 0)
        lx::trace::logger::global().log_error(err, "compositor.surface");
}

} // namespace

lx::gfx::drm_timeline_table* syncobj_timelines() {
    return g_timelines.bound() ? &g_timelines : nullptr;
}

void sync_point_drop(sync_point& p) {
    if (p.timeline)
        g_timelines.unref(p.timeline);
    p = {};
}

void sync_point_signal_now(sync_point& p) {
    if (!p.timeline)
        return;
    if (auto r = g_timelines.signal(p.timeline, p.point); !r)
        log_timeline_error(r.get_error());
    sync_point_drop(p);
}

namespace {

/// Signals `p` once the newest composite fence (render's mailbox) has: the GPU runs
/// composites and capture copies in submission order, so that fence covers every read of
/// a buffer the lifecycle has retired (it is retired only after the last frame that could
/// sample it completed on the render thread).
void sync_point_signal_after_composite(sync_point& p) {
    if (!p.timeline)
        return;
    const int fence =
        g_p0_ctx && g_p0_ctx->release_fences ? g_p0_ctx->release_fences->dup() : -1;
    if (auto r = g_timelines.signal_after(p.timeline, p.point, fence); !r)
        log_timeline_error(r.get_error());
#if !defined(_WIN32)
    if (fence >= 0)
        ::close(fence);
#endif
    sync_point_drop(p);
}

} // namespace

void syncobj_buffer_released(wl_resource* buffer) {
    if (g_syncobj_surfaces == 0 && g_timelines.live() == 0)
        return;
    buffer_obj* b = find_buffer(buffer);
    if (!b || !b->release.timeline)
        return;
    sync_point_signal_after_composite(b->release);
}

void syncobj_point_settle(unsigned timeline, unsigned long long point, bool signal) {
    sync_point p{timeline, point};
    if (signal)
        sync_point_signal_after_composite(p);
    else
        sync_point_drop(p);
}

#if defined(LUMEN_HAS_SYNCOBJ)

namespace {

/// wp_linux_drm_syncobj_timeline_v1: one reference on its table entry. Points set from it
/// hold their own ("timeline points set by this object ... are not unset" by destroying it).
struct timeline_obj {
    wl_resource* resource = nullptr;
    unsigned id = 0;
};

void timeline_destroy_resource(wl_resource* r) {
    auto* t = static_cast<timeline_obj*>(wl_resource_get_user_data(r));
    if (!t)
        return;
    g_timelines.unref(t->id);
    delete t;
}

const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

/// "Any timeline point set by this object ... since the last commit may be discarded."
void syncobj_surface_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    if (a && a->surface) {
        sync_point_drop(a->surface->pending.acquire);
        sync_point_drop(a->surface->pending.release);
    }
    if (a) {
        --g_syncobj_surfaces;
        note_syncobj_surfaces();
    }
    addon_free(a);
}

void set_point(wl_resource* r, wl_resource* timeline_res, uint32_t hi, uint32_t lo,
               bool acquire) {
    surface_addon* a = addon_from(r);
    if (!a || !a->surface) {
        wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                               "timeline point set after the wl_surface was destroyed");
        return;
    }
    auto* t = static_cast<timeline_obj*>(wl_resource_get_user_data(timeline_res));
    if (!t)
        return;
    sync_point& p = acquire ? a->surface->pending.acquire : a->surface->pending.release;
    sync_point_drop(p); // "the new point replaces the old one"
    g_timelines.ref(t->id);
    p.timeline = t->id;
    p.point = (static_cast<unsigned long long>(hi) << 32) | lo;
}

const struct wp_linux_drm_syncobj_surface_v1_interface syncobj_surface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_acquire_point =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* timeline, uint32_t hi,
           uint32_t lo) { set_point(r, timeline, hi, lo, true); },
    .set_release_point =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* timeline, uint32_t hi,
           uint32_t lo) { set_point(r, timeline, hi, lo, false); },
};

const struct wp_linux_drm_syncobj_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_surface =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->syncobj) {
                wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
                                       "the surface already has a syncobj surface object");
                return;
            }
            if (addon_create(client, &wp_linux_drm_syncobj_surface_v1_interface,
                             wl_resource_get_version(r), id, s, &s->syncobj,
                             &syncobj_surface_impl, syncobj_surface_destroy_resource)) {
                ++g_syncobj_surfaces;
                note_syncobj_surfaces();
            }
        },
    .import_timeline =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id, int32_t fd) {
            // The fd is closed by `import` in every path.
            auto imported = g_timelines.import(fd);
            if (!imported) {
                wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
                                       "the fd is not a DRM syncobj");
                return;
            }
            auto* t = new (std::nothrow) timeline_obj{};
            wl_resource* res =
                t ? wl_resource_create(client, &wp_linux_drm_syncobj_timeline_v1_interface,
                                       wl_resource_get_version(r), id)
                  : nullptr;
            if (!res) {
                g_timelines.unref(imported.value());
                delete t;
                wl_client_post_no_memory(client);
                return;
            }
            t->resource = res;
            t->id = imported.value();
            wl_resource_set_implementation(res, &timeline_impl, t, timeline_destroy_resource);
        },
};

} // namespace

void bind_syncobj_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    if (!g_timelines.bound())
        g_timelines.bind(lx::gfx::drm_timeline_ops_for(g_p0_ctx->syncobj_drm_fd));
    auto res =
        wayland::resource::create(client, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
    if (res)
        res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
}

bool syncobj_precheck(surface_obj* s) {
    surface_addon* a = s->syncobj;
    if (!a)
        return true; // its destructor dropped whatever it had set
    const surface_pending& p = s->pending;
    const bool attaching = p.buffer_set && p.buffer;
    if (attaching) {
        const buffer_obj* b = find_buffer(p.buffer);
        if (!b || !b->is_dmabuf) {
            wl_resource_post_error(a->resource,
                                   WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER,
                                   "explicit sync is only supported for linux-dmabuf buffers");
            return false;
        }
    }
    if ((p.acquire.timeline || p.release.timeline) && !attaching) {
        wl_resource_post_error(a->resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
                               "timeline point set without a buffer attached");
        return false;
    }
    if (attaching && !p.acquire.timeline) {
        wl_resource_post_error(a->resource,
                               WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT,
                               "buffer attached without an acquire point");
        return false;
    }
    if (attaching && !p.release.timeline) {
        wl_resource_post_error(a->resource,
                               WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
                               "buffer attached without a release point");
        return false;
    }
    if (attaching && p.acquire.timeline == p.release.timeline &&
        p.acquire.point >= p.release.point) {
        wl_resource_post_error(a->resource,
                               WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
                               "acquire point is not below the release point on one timeline");
        return false;
    }
    return true;
}

#else // no glue: nothing can set points

bool syncobj_precheck(surface_obj*) { return true; }

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
