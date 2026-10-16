module;

// wl_surface and wl_compositor: the double-buffered surface state, the four-step commit
// (role prechecks, pending -> cached, apply_cached, finish_commit), wl_region and frame
// callbacks. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <ctime>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
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

#if !defined(_WIN32)
#include <unistd.h>
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
import :dnd;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

surface_obj surfaces[k_max_surfaces]{};

namespace {

region_obj regions[k_max_regions]{};

// Pending frame callbacks across all surfaces (fired after present).
struct pending_frame {
    wl_resource* resource = nullptr;
};
pending_frame g_pending_frames[512]{};
unsigned g_pending_frame_count = 0;

surface_obj* alloc_surface() {
    for (auto& s : surfaces)
        if (!s.used) {
            s = {};
            s.used = true;
            return &s;
        }
    return nullptr;
}

lx::surface_id next_surface_id() {
    static unsigned n = 1;
    return lx::surface_id{n++};
}

lx::client_id client_handle(wayland::client_connection& c) {
    return lx::client_id{c.id()};
}

// ── wl_region ──────────────────────────────────────────────────────────────

const struct wl_region_interface region_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .add =
        [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t w,
           int32_t h) {
            if (auto* r = static_cast<region_obj*>(wl_resource_get_user_data(resource)))
                r->region.add({x, y, w, h});
        },
    .subtract =
        [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t w,
           int32_t h) {
            if (auto* r = static_cast<region_obj*>(wl_resource_get_user_data(resource)))
                r->region.subtract({x, y, w, h});
        },
};

void region_destroy_resource(struct wl_resource* resource) {
    for (auto& r : regions) {
        if (r.used && r.resource == resource) {
            r = {};
            return;
        }
    }
}

// ── wl_callback (frame) ────────────────────────────────────────────────────

[[nodiscard]] unsigned now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned>(static_cast<unsigned long long>(ts.tv_sec) * 1000ull +
                                 static_cast<unsigned long long>(ts.tv_nsec) / 1'000'000ull);
}

/// Answers a frame callback now. Done rather than dropped: a callback that is never
/// answered stalls its client for good.
void fire_frame_callback_now(wl_resource* cb) {
    if (!cb)
        return;
    wl_callback_send_done(cb, now_ms());
    wl_resource_destroy(cb);
}

void queue_frame_callback(wl_resource* cb) {
    if (!cb)
        return;
    if (g_pending_frame_count >= 512) {
        fire_frame_callback_now(cb);
        return;
    }
    g_pending_frames[g_pending_frame_count++].resource = cb;
}

/// libwayland destroys a client's resources on disconnect, so every table holding a
/// wl_resource* must drop it here or the next send touches freed memory.
void frame_callback_destroy_resource(struct wl_resource* resource) {
    for (unsigned i = 0; i < g_pending_frame_count; ++i) {
        if (g_pending_frames[i].resource == resource)
            g_pending_frames[i].resource = nullptr;
    }
    for (auto& s : surfaces) {
        if (!s.used)
            continue;
        for (unsigned i = 0; i < s.pending.frame_cb_count; ++i) {
            if (s.pending.frame_cbs[i].resource == resource)
                s.pending.frame_cbs[i] = {};
        }
        for (unsigned i = 0; i < s.cached.frame_cb_count; ++i) {
            if (s.cached.frame_cbs[i].resource == resource)
                s.cached.frame_cbs[i] = {};
        }
        for (unsigned i = 0; i < s.current.frame_cb_count; ++i) {
            if (s.current.frame_cbs[i].resource == resource)
                s.current.frame_cbs[i] = {};
        }
    }
}

} // namespace

// ── wl_surface ─────────────────────────────────────────────────────────────

/// Drops a surface's cached content update without applying it — for a client that is
/// being torn down, whose buffers and callbacks libwayland is destroying anyway.
void discard_cached(surface_obj* s) {
    s->cached = {};
    s->has_cached = false;
}

namespace {

void surface_destroy_resource(struct wl_resource* resource) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    const bool dying = client_dying(wl_resource_get_client(resource));
    // The unmaps below change its node rects: nothing more is sent to this object (an
    // event to a destroyed object would reach the client as one for a dead id).
    s->destroying = true;
    // Drop input focus without a `leave`: the surface is going, and focus must not keep
    // pointing at it for the focus change the unmap below makes.
    if (s->ctx && s->ctx->input)
        s->ctx->input->remove(resource);
    if (s->toplevel && s->ctx && s->ctx->wm)
        s->ctx->wm->on_destroyed(s->toplevel);
    // A popup's record goes with whichever of its objects goes first; its tree root is
    // still in the tree here, so the unmap is damaged properly.
    if (s->role == surface_role::popup && s->ctx && s->ctx->popups)
        s->ctx->popups->destroy(s->id);
    // Likewise a layer surface (its popups are dismissed and orphaned first).
    if (s->role == surface_role::layer && s->ctx && s->ctx->layers)
        s->ctx->layers->destroy(s->id);
    if (s->layer) {
        s->layer->surface = nullptr;
        s->layer = nullptr;
    }
    if (s->role == surface_role::cursor && s->ctx && s->ctx->cursor)
        s->ctx->cursor->on_surface_destroyed(s->id);
    // A drag's icon or target: the drag goes on without it (the icon leaves its layer while
    // the tree still knows the root).
    if (s->ctx && s->ctx->dnd)
        s->ctx->dnd->on_surface_destroyed(s->id);
    // Activation tokens that name it as their requester (by id: nothing dangles).
    activation_surface_destroyed(s->id);
    if (s->xdg) {
        s->xdg->surface = nullptr;
        if (s->xdg->toplevel)
            s->xdg->toplevel->id = {};
        if (s->xdg->popup)
            s->xdg->popup->surface = {};
    }
    if (s->sub) {
        s->sub->surface = nullptr;
        s->sub = nullptr;
    }
    // The tree forgets the surface: it leaves its parent's orders, everything it drew is
    // unmapped, and its children become orphans ("if the parent wl_surface object is
    // destroyed, the sub-surface is unmapped"). An orphan has no parent to synchronize
    // with, so content it had cached is applied now, like a desync transition.
    lx::surface_id orphans[lx::compositor::k_tree_max_children + 1];
    unsigned orphan_count = 0;
    if (auto* tree = s->ctx ? s->ctx->tree : nullptr) {
        if (const auto* kids = tree->order_pending(s->id)) {
            for (unsigned i = 0; i < kids->count; ++i)
                if (!(kids->ids[i] == s->id) && tree->parent_of(kids->ids[i]) == s->id)
                    orphans[orphan_count++] = kids->ids[i];
        }
        tree->remove_surface(s->id);
    }
    // Last chance to retire content whose buffer object is already gone: nothing else will
    // reference its texture now. A live buffer is retired when its object is destroyed.
    if (s->has_content && !s->current.buffer && s->current_texture)
        retire_texture(s->current_texture);
    if (s->ctx && s->ctx->surfaces)
        s->ctx->surfaces->destroy(s->id);
    *s = {};
    for (unsigned i = 0; i < orphan_count; ++i) {
        surface_obj* child = surface_by_id(orphans[i]);
        if (!child)
            continue;
        if (dying)
            discard_cached(child);
        else
            flush_desync(child, "parent-destroyed");
    }
}

void surface_attach(struct wl_client*, struct wl_resource* resource, struct wl_resource* buffer,
                    int32_t x, int32_t y) {
    auto* s = surface_from(resource);
    if (!s)
        return;
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
    // xdg-decoration v1: "any attempts by a client to attach or manipulate a buffer prior to
    // the first xdg_toplevel_decoration.configure event must also be treated as errors".
    if (buffer && s->toplevel && s->ctx && s->ctx->toplevels) {
        const auto* rec = s->ctx->toplevels->find(s->toplevel);
        if (rec && rec->deco_resource && rec->deco_awaiting_configure) {
            wl_resource_post_error(static_cast<wl_resource*>(rec->deco_resource),
                                   ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                                   "buffer attached before the first decoration configure");
            return;
        }
    }
#endif
    // Tracked from the attach on, so a buffer destroyed before the commit is noticed.
    if (buffer)
        (void)track_shm_buffer(buffer, wl_shm_buffer_get(buffer));
    s->pending.buffer = buffer;
    s->pending.buffer_set = true;
    s->pending.dx = x;
    s->pending.dy = y;
}

// Clips client damage to its buffer. "Everything" is conventionally x=0, y=0 with
// INT32_MAX extents (wev, GTK and SDL all send it), so the sum x + width is formed in 64
// bits. Passed through unclipped, that rectangle reached FB_DAMAGE_CLIPS as the node's
// bounds and the frame's damage, and the kernel rejected the whole atomic commit with
// EINVAL — one lost frame on every such commit.
lx::rect2i clip_damage_to_buffer(lx::rect2i damage, int width, int height) {
    const long long x0 = damage.x > 0 ? damage.x : 0;
    const long long y0 = damage.y > 0 ? damage.y : 0;
    long long x1 = static_cast<long long>(damage.x) + damage.width;
    long long y1 = static_cast<long long>(damage.y) + damage.height;
    if (x1 > width)
        x1 = width;
    if (y1 > height)
        y1 = height;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

void surface_damage(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                    int32_t width, int32_t height) {
    auto* s = surface_from(resource);
    if (!s || width <= 0 || height <= 0)
        return;
    // Accumulate: a client may send several damage requests per commit, and keeping only
    // the last one under-reports what changed. A bbox union is enough here — the client's
    // rects all land inside one window, and the window's damage is split from the others'
    // in the scene's region. Formed in 64 bits: INT32_MAX extents are common.
    const lx::rect2i incoming = clip_damage_to_buffer({x, y, width, height}, INT32_MAX, INT32_MAX);
    if (incoming.width <= 0 || incoming.height <= 0)
        return;
    if (!s->pending.has_damage) {
        s->pending.damage = incoming;
        s->pending.has_damage = true;
        return;
    }
    const lx::rect2i& d = s->pending.damage;
    const int x0 = d.x < incoming.x ? d.x : incoming.x;
    const int y0 = d.y < incoming.y ? d.y : incoming.y;
    const long long x1 = std::max(static_cast<long long>(d.x) + d.width,
                                  static_cast<long long>(incoming.x) + incoming.width);
    const long long y1 = std::max(static_cast<long long>(d.y) + d.height,
                                  static_cast<long long>(incoming.y) + incoming.height);
    s->pending.damage = {x0, y0, static_cast<int>(std::min<long long>(x1 - x0, INT32_MAX)),
                         static_cast<int>(std::min<long long>(y1 - y0, INT32_MAX))};
}

void surface_frame(struct wl_client* client, struct wl_resource* resource, uint32_t callback_id) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    wl_resource* cb =
        wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (!cb)
        return;
    wl_resource_set_destructor(cb, frame_callback_destroy_resource);
    if (s->pending.frame_cb_count < k_max_frame_cbs) {
        s->pending.frame_cbs[s->pending.frame_cb_count].resource = cb;
        s->pending.frame_cbs[s->pending.frame_cb_count].used = true;
        ++s->pending.frame_cb_count;
    }
}

void surface_set_opaque_region(struct wl_client*, struct wl_resource* resource,
                               struct wl_resource* region) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    // Same rules as the input region, except that null means empty, not infinite.
    if (const auto* r = region ? static_cast<region_obj*>(wl_resource_get_user_data(region))
                               : nullptr) {
        s->pending.opaque = r->region;
    } else {
        s->pending.opaque = {};
        s->pending.opaque.reset_empty();
    }
    s->pending.opaque_set = true;
}

void surface_set_input_region(struct wl_client*, struct wl_resource* resource,
                              struct wl_resource* region) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    // Copied now, applied on commit (wl_surface: double-buffered; the wl_region may be
    // destroyed immediately after this request). Null means infinite.
    if (const auto* r = region ? static_cast<region_obj*>(wl_resource_get_user_data(region))
                               : nullptr)
        s->pending.input = r->region;
    else
        s->pending.input = {};
    s->pending.input_set = true;
    if (s->pending.input.approximated &&
        lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.wm",
                                            "input region exceeds 16 rects — over-approximated "
                                            "(accepts more input than asked). Reported once.");
        }
    }
}

/// A client that keeps committing an unimportable buffer would otherwise log once per
/// frame on the commit hot path. Report the first failure and then periodically.
void log_commit_failure(const lx::error& err) {
    static unsigned suppressed = 0;
    static constexpr unsigned k_report_interval = 600;
    if (suppressed++ % k_report_interval == 0)
        lx::trace::logger::global().log_error(err, "compositor.surface");
}

/// A cursor-role surface's new content. The pixels are copied out at once (the cursor
/// manager keeps its own image), so the buffer goes straight back to the client.
void commit_cursor_surface(surface_obj* s) {
    auto* cursor = s->ctx ? s->ctx->cursor : nullptr;
    wl_resource* buffer = s->current.buffer;
    if (!cursor) {
        if (buffer)
            wl_buffer_send_release(buffer);
        return;
    }
    if (!buffer) {
        cursor->on_client_surface_commit(s->id, nullptr, 0, 0, 0, false, s->current.dx,
                                         s->current.dy);
        return;
    }
    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    const uint32_t format = shm ? wl_shm_buffer_get_format(shm) : 0;
    if (shm && (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888)) {
        const int width = wl_shm_buffer_get_width(shm);
        const int height = wl_shm_buffer_get_height(shm);
        const int stride = wl_shm_buffer_get_stride(shm);
        if (width > 0 && height > 0 && stride >= width * 4) {
            wl_shm_buffer_begin_access(shm);
            cursor->on_client_surface_commit(
                s->id, static_cast<const uint32_t*>(wl_shm_buffer_get_data(shm)),
                static_cast<unsigned>(width), static_cast<unsigned>(height),
                static_cast<unsigned>(stride) / 4u, format == WL_SHM_FORMAT_XRGB8888,
                s->current.dx, s->current.dy);
            wl_shm_buffer_end_access(shm);
        }
    } else {
        // Recorded trade-off: a dma-buf or non-8888 cursor keeps the previous image —
        // reading one back for a 64x64 plane is not worth a GPU round trip.
        static bool reported = false;
        if (!reported && lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
            reported = true;
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.cursor",
                                            "cursor surface buffer is not ARGB/XRGB shm — "
                                            "keeping the previous image");
        }
    }
    wl_buffer_send_release(buffer);
}

void remember_cursor_candidate(surface_obj* s) {
    wl_shm_buffer* shm = wl_shm_buffer_get(s->current.buffer);
    if (!shm)
        return;
    const uint32_t format = wl_shm_buffer_get_format(shm);
    const int width = wl_shm_buffer_get_width(shm);
    const int height = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    constexpr int k_max = static_cast<int>(lx::compositor::k_cursor_max_px);
    if ((format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) || width <= 0 ||
        height <= 0 || width > k_max || height > k_max || stride < width * 4)
        return;
    wl_shm_buffer_begin_access(shm);
    s->ctx->cursor->on_client_surface_commit(
        s->id, static_cast<const uint32_t*>(wl_shm_buffer_get_data(shm)),
        static_cast<unsigned>(width), static_cast<unsigned>(height),
        static_cast<unsigned>(stride) / 4u, format == WL_SHM_FORMAT_XRGB8888, 0, 0);
    wl_shm_buffer_end_access(shm);
}

/// No-alpha formats: the content covers its extents, and drawing it may skip blending.
[[nodiscard]] bool fourcc_is_opaque(lx::fourcc f) {
    switch (f) {
    case 0x34325258u: // XR24 XRGB8888
    case 0x34324258u: // XB24 XBGR8888
    case 0x34325852u: // RX24 RGBX8888
    case 0x34325842u: // BX24 BGRX8888
    case 0x34324752u: // RG24 RGB888
    case 0x34324742u: // BG24 BGR888
    case 0x36314752u: // RG16 RGB565
    case 0x36314742u: // BG16 BGR565
    case 0x30335258u: // XR30 XRGB2101010
    case 0x30334258u: // XB30 XBGR2101010
        return true;
    default:
        return false;
    }
}

/// True when one rect of `region` spans the whole buffer. An over-approximated region
/// never counts: claiming opacity the client did not promise draws garbage.
[[nodiscard]] bool region_covers(const lx::compositor::input_region& region, int width,
                                 int height) {
    if (width <= 0 || height <= 0 || region.approximated)
        return false;
    if (region.infinite)
        return true;
    for (unsigned i = 0; i < region.count; ++i) {
        const lx::rect2i& r = region.rects[i];
        if (r.x <= 0 && r.y <= 0 && static_cast<long long>(r.x) + r.width >= width &&
            static_cast<long long>(r.y) + r.height >= height)
            return true;
    }
    return false;
}

/// Opaque content, for blending: a format without alpha, or an opaque region covering the
/// whole buffer. Sub-rect opaque regions (split draws) are a follow-up.
[[nodiscard]] bool content_opaque(const surface_obj* s, int width, int height) {
    return s->format_opaque || region_covers(s->opaque_current, width, height);
}

/// Content on this surface is drawn (once mapped), so a buffer must be imported and staged
/// when it is applied — whether or not the surface is mapped yet: a subsurface committed
/// before its parent maps has to show the moment the parent does. See `keeps_content` for
/// the surfaces whose content is kept although they are not drawable now.
[[nodiscard]] bool drawable_role(const surface_obj* s) {
    switch (s->role) {
    case surface_role::xdg_toplevel:
        return static_cast<bool>(s->toplevel);
    case surface_role::subsurface:
        // Linked and parented: an orphan (its parent was destroyed) is never drawn again.
        return s->sub != nullptr && s->ctx->tree && s->ctx->tree->linked(s->id) &&
               static_cast<bool>(s->ctx->tree->parent_of(s->id));
    case surface_role::popup:
        // Until popup_done: a dismissed popup's commits release their buffers undrawn.
        return s->ctx->popups && s->ctx->popups->drawable(s->id);
    case surface_role::layer:
        // Until `closed` (and only while its zwlr_layer_surface_v1 lives).
        return s->layer && s->ctx->layers && s->ctx->layers->drawable(s->id);
    case surface_role::dnd_icon:
        // While the drag it was given to runs.
        return s->ctx->dnd && s->ctx->dnd->active() && s->ctx->dnd->icon() == s->id;
    default:
        return false;
    }
}

/// Content applied to this surface is imported and kept as its current content, so it is
/// drawn the moment the surface becomes drawable without a new attach: a drawable role, a
/// role-less surface (it may become a subsurface, whose content then maps with its parent:
/// "a non-NULL wl_buffer is applied and the parent surface is mapped", in either order), or
/// a subsurface whose wl_subsurface or parent is gone (a new wl_subsurface may link it
/// again), or a drag icon, which may be committed before or after start_drag (and given to
/// a later drag again). wlroots keeps every surface's current buffer the same way. Only a
/// cursor, a dismissed popup, a closed layer surface and an xdg_toplevel role without its
/// object release content undrawn.
[[nodiscard]] bool keeps_content(const surface_obj* s) {
    return s->role == surface_role::none || s->role == surface_role::subsurface ||
           s->role == surface_role::dnd_icon || drawable_role(s);
}


/// xdg-shell rules that reject a popup's commit, checked on the pending state.
bool popup_prechecks(surface_obj* s) {
    auto* popups = s->ctx->popups;
    const auto* rec = popups ? popups->find(s->id) : nullptr;
    if (!rec || rec->dismissed)
        return true; // inert: its buffers are released undrawn
    if (!rec->initial_commit_seen &&
        rec->parent_kind == lx::compositor::popup_parent_kind::none) {
        // A popup created without a parent must be given one (zwlr_layer_surface_v1
        // .get_popup) before its initial commit.
        if (wl_resource* wm = wm_base_resource(s->xdg))
            wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                   "popup committed without a parent");
        return false;
    }
    if (s->pending.buffer_set && s->pending.buffer && !rec->ever_acked) {
        wl_resource_post_error(static_cast<wl_resource*>(rec->xdg_surface_res),
                               XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                               "buffer attached before the first configure was acked");
        return false;
    }
    popups->apply_pending_state(s->id);
    return true;
}

/// zwlr_layer_surface_v1's commit rules, checked on the pending state, on the layer surface.
bool layer_prechecks(surface_obj* s) {
    auto* layers = s->ctx->layers;
    auto* rec = layers && s->layer ? layers->find(s->id) : nullptr;
    if (!rec || rec->closed)
        return true; // inert: "further changes to the surface will be ignored"
    auto* res = static_cast<wl_resource*>(rec->layer_res);
    switch (lx::compositor::validate_layer_state(rec->pending)) {
    case lx::compositor::layer_state_error::invalid_size:
        wl_resource_post_error(res, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                               "width or height is 0 without both opposite anchors");
        return false;
    case lx::compositor::layer_state_error::invalid_exclusive_edge:
        wl_resource_post_error(res, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                               "exclusive edge is not one of the anchored edges");
        return false;
    case lx::compositor::layer_state_error::none:
        break;
    }
    // "Any attempts by a client to attach or manipulate a buffer prior to the first
    // layer_surface.configure call must also be treated as errors" — wlroots reads it as
    // "before a configure was acked", which is when a buffer may be attached.
    if (s->pending.buffer_set && s->pending.buffer && !rec->ever_acked) {
        wl_resource_post_error(res, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                               "buffer attached before a configure was acked");
        return false;
    }
    layers->apply_pending(s->id);
    return true;
}


/// The xdg-shell rules that reject a whole commit, checked on the pending state before
/// anything moves. False when an error was posted.
bool role_prechecks(surface_obj* s) {
    if (s->role == surface_role::popup)
        return popup_prechecks(s);
    if (s->role == surface_role::layer)
        return layer_prechecks(s);
    lx::compositor::toplevel_record* tl = nullptr;
    if (s->role == surface_role::xdg_toplevel && s->toplevel && s->ctx->toplevels)
        tl = s->ctx->toplevels->find_mut(s->toplevel);
    if (!tl)
        return true;
    const bool attaching = s->pending.buffer_set && s->pending.buffer;
    if (attaching && !tl->ever_acked) {
        wl_resource_post_error(static_cast<wl_resource*>(tl->xdg_surface_resource),
                               XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                               "buffer attached before the first configure was acked");
        return false;
    }
    if (!lx::compositor::toplevel_manager::apply_pending_state(*tl)) {
        wl_resource_post_error(static_cast<wl_resource*>(tl->xdg_toplevel_resource),
                               XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "min_size is larger than max_size");
        return false;
    }
    return true;
}

/// pending → cached, the content-update queue with no constraints: later state replaces
/// earlier, damage and frame callbacks accumulate, and a buffer superseded before it was
/// ever used goes straight back to the client.
void merge_pending_into_cached(surface_obj* s) {
    surface_pending& p = s->pending;
    surface_pending& c = s->cached;
    if (p.buffer_set) {
        // Not when it is the buffer on screen: a dma-buf there is still being sampled and
        // the lifecycle tracker releases it. An shm buffer was copied and released at its
        // own commit, so a re-attach of it is owed a release of its own.
        if (c.buffer_set && c.buffer && c.buffer != p.buffer &&
            (c.buffer != s->current.buffer || wl_shm_buffer_get(c.buffer)))
            wl_buffer_send_release(c.buffer);
        c.buffer = p.buffer;
        c.buffer_set = true;
    }
    c.dx += p.dx;
    c.dy += p.dy;
    c.scale = p.scale;
    c.transform = p.transform;
    if (p.has_damage) {
        if (!c.has_damage) {
            c.damage = p.damage;
            c.has_damage = true;
        } else {
            const lx::rect2i& d = c.damage;
            const lx::rect2i& n = p.damage;
            const int x0 = d.x < n.x ? d.x : n.x;
            const int y0 = d.y < n.y ? d.y : n.y;
            const long long x1 = std::max(static_cast<long long>(d.x) + d.width,
                                          static_cast<long long>(n.x) + n.width);
            const long long y1 = std::max(static_cast<long long>(d.y) + d.height,
                                          static_cast<long long>(n.y) + n.height);
            c.damage = {x0, y0, static_cast<int>(std::min<long long>(x1 - x0, INT32_MAX)),
                        static_cast<int>(std::min<long long>(y1 - y0, INT32_MAX))};
        }
    }
    for (unsigned i = 0; i < p.frame_cb_count; ++i) {
        frame_callback cb = p.frame_cbs[i];
        p.frame_cbs[i] = {};
        if (!cb.used || !cb.resource)
            continue;
        if (c.frame_cb_count >= k_max_frame_cbs) {
            // Full: the oldest is answered now rather than dropped (a dropped callback
            // stalls the client for good).
            const frame_callback oldest = c.frame_cbs[0];
            for (unsigned j = 1; j < c.frame_cb_count; ++j)
                c.frame_cbs[j - 1] = c.frame_cbs[j];
            c.frame_cbs[--c.frame_cb_count] = {};
            if (oldest.used)
                fire_frame_callback_now(oldest.resource);
        }
        c.frame_cbs[c.frame_cb_count++] = cb;
    }
    p.frame_cb_count = 0;
    if (p.input_set) {
        c.input = p.input;
        c.input_set = true;
    }
    if (p.opaque_set) {
        c.opaque = p.opaque;
        c.opaque_set = true;
    }
    p.buffer = nullptr;
    p.buffer_set = false;
    p.dx = 0;
    p.dy = 0;
    p.damage = {};
    p.has_damage = false;
    p.input_set = false;
    p.opaque_set = false;
    s->has_cached = true;
}

/// The import path: hands a newly applied buffer to the surface manager, stages its pixels
/// (shm) or describes the import (dma-buf) for the render thread, and releases or tracks
/// the wl_buffer. Every texture update is stamped with the snapshot that may first show it.
void import_current_buffer(surface_obj* s, bool had_damage, lx::rect2i pending_damage) {
    buffer_obj* buf = find_buffer(s->current.buffer);
    wl_shm_buffer* shm = wl_shm_buffer_get(s->current.buffer);
    if (!buf && shm)
        buf = track_shm_buffer(s->current.buffer, shm);
    if (!buf) {
        // Not a buffer kind this compositor can show; nothing will ever sample it.
        wl_buffer_send_release(s->current.buffer);
        return;
    }

    // Staging is keyed by texture id, which only exists after on_commit below, so the
    // format is resolved here and the pixel copy waits.
    const lx::fourcc shm_format = buf->is_shm ? staged_shm_format(shm)
                                              : static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
    const lx::fourcc client_format =
        buf->is_shm ? shm_format_to_fourcc(wl_shm_buffer_get_format(shm)) : buf->dmabuf.format;
    s->format_opaque = fourcc_is_opaque(client_format);

    if (buf->is_dmabuf) {
        // Duplicate plane metadata for the surface table; keep FDs owned by buffer_obj.
        lx::gfx::dmabuf_desc desc{};
        desc.width = buf->dmabuf.width;
        desc.height = buf->dmabuf.height;
        desc.format = buf->dmabuf.format;
        desc.plane_count = buf->dmabuf.plane_count;
        for (unsigned i = 0; i < desc.plane_count && i < 4; ++i) {
            // Dup FD so surface_manager owns a copy for import.
            const int src = buf->dmabuf.planes[i].fd.get();
            if (src >= 0)
                desc.planes[i].fd.reset(::dup(src));
            desc.planes[i].offset = buf->dmabuf.planes[i].offset;
            desc.planes[i].stride = buf->dmabuf.planes[i].stride;
            desc.planes[i].modifier = buf->dmabuf.planes[i].modifier;
        }
        s->ctx->surfaces->register_dmabuf(buf->id, static_cast<lx::gfx::dmabuf_desc&&>(desc));
    } else if (buf->is_shm) {
        s->ctx->surfaces->register_shm(buf->id, buf->width, buf->height, shm_format);
    }

    lx::compositor::surface_commit commit{};
    commit.surface = s->id;
    commit.buffer = buf->id;
    commit.client = s->client;
    commit.opaque =
        content_opaque(s, static_cast<int>(buf->width), static_cast<int>(buf->height));
    // A new buffer with no damage request is treated as fully damaged: strictly the
    // client said nothing changed, but every real client that does this means "all of it".
    if (had_damage)
        commit.damage = clip_damage_to_buffer(pending_damage, static_cast<int>(buf->width),
                                              static_cast<int>(buf->height));
    else
        commit.damage = {0, 0, static_cast<int>(buf->width), static_cast<int>(buf->height)};

    s->current_buffer_id = buf->id;
    s->current_texture = s->ctx->surfaces->texture_for(buf->id);

    auto result = s->ctx->surfaces->on_commit(static_cast<lx::compositor::surface_commit&&>(commit));
    if (!result) {
        // The buffer is unusable, so nothing will sample it. Release it immediately:
        // holding it back would starve the client of buffers and stall it forever.
        log_commit_failure(result.get_error());
        wl_buffer_send_release(s->current.buffer);
        return;
    }

    const auto img = s->ctx->surfaces->lookup(s->id);

    const unsigned char* shm_pixels = nullptr;
    unsigned long long shm_seq = 0;
    unsigned long long shm_token = 0;
    if (buf->is_shm && img.image_id) {
        shm_pixels = stage_shm_pixels(buf, shm, img.image_id, shm_seq, shm_token);
        if (!shm_pixels) {
            // The one silent path to a black screen: without staged pixels no texture
            // update is queued, so the texture is never registered, and the draw that
            // references it is skipped every frame with nothing explaining why. Staging
            // legitimately declines when every slot is in flight, so this is rate-limited
            // rather than suppressed — a client that is merely running ahead recovers,
            // one that never stages anything does not.
            static bool warned = false;
            if (!warned) {
                warned = true;
                lx::trace::logger::global().log(
                    lx::trace::level::warn, "compositor.surface",
                    "shm staging returned no buffer — the texture cannot be registered and "
                    "the surface will not be drawn. Reported once.");
            }
        }
    }

    // Register SHM pixels with headless for software blit.
    if (buf->is_shm && s->ctx->headless && shm_pixels && img.image_id) {
        s->ctx->headless->register_texture(img.image_id, buf->width, buf->height, shm_pixels);
    }

    // Describe the GPU-side work for the render thread; it owns all Vulkan recording.
    if (s->ctx->textures && img.image_id) {
        texture_update update{};
        update.texture_id = img.image_id;
        update.width = buf->width;
        update.height = buf->height;
        update.format = img.format;
        update.visible_from_frame = s->ctx->building_frame;
        if (buf->is_dmabuf) {
            update.kind = texture_update::op::dmabuf;
            update.vk_image = img.vk_image;
            update.vk_memory = img.vk_memory;
            // Hand the render thread its own FD: the GL backend imports there, and the
            // client's plane FD belongs to buffer_obj, which may be destroyed first.
            if (buf->dmabuf.plane_count > 0) {
                const int plane_fd = buf->dmabuf.planes[0].fd.get();
                if (plane_fd >= 0)
                    update.dmabuf_fd = ::dup(plane_fd);
                update.dmabuf_offset = buf->dmabuf.planes[0].offset;
                update.dmabuf_stride = buf->dmabuf.planes[0].stride;
                update.dmabuf_modifier = buf->dmabuf.planes[0].modifier;
                update.format = buf->dmabuf.format;
            }
        } else if (buf->is_shm && shm_pixels) {
            update.kind = texture_update::op::shm;
            update.pixels = shm_pixels;
            update.shm_seq = shm_seq;
            update.shm_token = shm_token;
            update.format = shm_format;
            update.stride = buf->width * 4u;
        }
        if (update.kind != texture_update::op::none && !s->ctx->textures->try_push(update)) {
            // Nothing will consume the FD now, so it has to be closed here or the
            // compositor leaks one per dropped update.
            if (update.dmabuf_fd >= 0)
                ::close(update.dmabuf_fd);
            if (update.shm_token && s->ctx->shm_store)
                s->ctx->shm_store->release(update.shm_token);
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.render",
                                            "texture update queue full — frame will lag");
        }
    }

    // dmabuf buffers stay alive until the render thread finishes sampling; SHM pixels
    // are copied at commit time so the client buffer can be released immediately.
    if (buf->is_dmabuf && s->ctx->lifecycle) {
        lx::gfx::import_cache_key key{};
        key.client = s->client;
        key.buffer = buf->id;
        if (buf->dmabuf.plane_count > 0)
            key.modifier = buf->dmabuf.planes[0].modifier;
        (void)s->ctx->lifecycle->on_import(s->client, s->id, buf->id, key, s->current.buffer);
        s->ctx->lifecycle->on_attach(s->id);
    } else if (buf->is_shm) {
        wl_buffer_send_release(s->current.buffer);
    }
}

/// cached → current for one surface: the body of a content update. The buffer is applied
/// before all other state (wl_surface.commit).
void apply_core(surface_obj* s) {
    surface_pending& c = s->cached;
    const bool new_buffer = c.buffer_set;
    if (new_buffer) {
        // The surface stops showing its previous content. If that buffer object is already
        // gone, nothing else will ever reference its texture — destroy_buffer deliberately
        // left it alone while it was still on screen.
        if (s->has_content && !s->current.buffer && s->current_texture)
            retire_texture(s->current_texture);
        if (!s->current.buffer || s->current.buffer != c.buffer)
            s->current_texture = 0;
        s->current.buffer = c.buffer;
        s->has_content = c.buffer != nullptr;
    }
    // The attach/offset delta applies to this content update only (wl_surface.offset).
    s->current.dx = c.dx;
    s->current.dy = c.dy;
    s->current.scale = c.scale;
    s->current.transform = c.transform;
    // Damage is per update: it describes this change and nothing after it.
    const bool had_damage = c.has_damage;
    const lx::rect2i pending_damage = c.damage;
    for (unsigned i = 0; i < c.frame_cb_count; ++i) {
        const frame_callback cb = c.frame_cbs[i];
        c.frame_cbs[i] = {};
        if (cb.used)
            queue_frame_callback(cb.resource);
    }
    c.frame_cb_count = 0;
    if (c.input_set) {
        s->input_current = c.input;
        s->ctx->surfaces->set_input_region(s->id, s->input_current);
    }
    const bool opaque_changed = c.opaque_set;
    if (c.opaque_set)
        s->opaque_current = c.opaque;
    c = {};
    s->has_cached = false;

    if (s->role == surface_role::cursor) {
        if (new_buffer)
            commit_cursor_surface(s);
        return;
    }

    // State-only update, or a null buffer: there is no new content to import. An update
    // without an attach does not re-read the old buffer — an shm buffer was copied and
    // released, and its storage now belongs to the client again.
    if (!new_buffer || !s->current.buffer) {
        if (opaque_changed && s->has_content) {
            const lx::size2i size = s->ctx->surfaces->buffer_size(s->id);
            s->ctx->surfaces->set_opaque(s->id, content_opaque(s, size.width, size.height));
        }
        return;
    }

    // A role-less surface may still become the cursor: wl_pointer.set_cursor may name it
    // after this commit, and then shows the content committed here. Cursor-sized shm
    // content is kept for that (read before the import below releases the buffer).
    if (s->role == surface_role::none && s->ctx->cursor)
        remember_cursor_candidate(s);
    if (!keeps_content(s)) {
        wl_buffer_send_release(s->current.buffer);
        return;
    }
    import_current_buffer(s, had_damage, pending_damage);
}

} // namespace

/// Applies `s`'s cached state, then — atomically with it, in the same UI call and so the
/// same snapshot — its children's positions and order and the cached state of every
/// effectively synchronized child, recursively. The only path from cached to current, so
/// a later commit-constraint system (fences, transactions) can hold updates back here.
void apply_cached(surface_obj* s, const char* cause, unsigned depth) {
    apply_core(s);
    auto* tree = s->ctx->tree;
    if (!tree)
        return;
    const lx::size2i size = s->ctx->surfaces->buffer_size(s->id);
    tree->set_has_buffer(s->id, s->has_content && !size.empty());
    if (s->role == surface_role::subsurface && surface_debug()) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "subsurface apply surface=%llu parent=%llu cause=%s buffer=%d w=%d h=%d",
                      s->id.id(), tree->parent_of(s->id).id(), cause, s->has_content ? 1 : 0,
                      size.width, size.height);
        surface_log(line);
    }
    tree->apply_child_state(s->id);
    lx::surface_id kids[lx::compositor::k_tree_max_children + 1];
    const unsigned n =
        tree->sync_children(s->id, kids, lx::compositor::k_tree_max_children + 1);
    for (unsigned i = 0; i < n && depth < lx::compositor::k_tree_max_depth; ++i) {
        surface_obj* child = surface_by_id(kids[i]);
        if (child && child->has_cached)
            apply_cached(child, "parent-commit", depth + 1);
    }
    tree->mark_dirty(s->id);
}

namespace {

/// The role's part of a commit of a root surface, after its tree has been applied.
void finish_role(surface_obj* s) {
    if (s->role == surface_role::dnd_icon) {
        // The offset moves the icon relative to the hotspot; the tree sync that follows
        // places its nodes (and maps them, the first time it has content).
        if (s->ctx->dnd)
            s->ctx->dnd->on_icon_commit(s->id, s->has_content, s->current.dx, s->current.dy);
        return;
    }
    if (s->role == surface_role::layer && s->ctx->layers) {
        if (!s->layer)
            return;
        lx::compositor::layer_commit_info info{};
        info.size = s->ctx->surfaces->buffer_size(s->id);
        info.has_buffer = s->has_content && !info.size.empty();
        s->ctx->layers->on_commit(s->id, info);
        return;
    }
    if (s->role == surface_role::popup && s->ctx->popups) {
        const auto* rec = s->ctx->popups->find(s->id);
        if (!rec)
            return;
        lx::compositor::popup_commit_info info{};
        const lx::size2i size = s->ctx->surfaces->buffer_size(s->id);
        info.has_buffer = s->has_content && !size.empty();
        const lx::rect2i bbox = s->ctx->tree ? s->ctx->tree->subtree_bbox(s->id)
                                             : lx::rect2i{0, 0, size.width, size.height};
        info.window_geometry = lx::compositor::effective_window_geometry(
            rec->window_geometry_set, rec->window_geometry_requested, bbox);
        s->ctx->popups->on_commit(s->id, info);
        return;
    }
    if (s->role != surface_role::xdg_toplevel || !s->toplevel || !s->ctx->wm ||
        !s->ctx->toplevels)
        return;
    auto* tl = s->ctx->toplevels->find_mut(s->toplevel);
    if (!tl)
        return;
    if (!tl->initial_commit_seen) {
        // The initial commit carries no buffer (rejected by the prechecks) and asks for
        // the first configure, which the window manager sends right away.
        s->ctx->wm->on_initial_commit(s->toplevel);
        return;
    }
    lx::compositor::toplevel_commit_info info{};
    info.buffer_size = s->ctx->surfaces->buffer_size(s->id);
    info.has_buffer = s->has_content && !info.buffer_size.empty();
    // After apply_cached, so this commit's synchronized subsurfaces are in it.
    if (s->ctx->tree)
        info.tree_bbox = s->ctx->tree->subtree_bbox(s->id);
    else if (info.has_buffer)
        info.tree_bbox = {0, 0, info.buffer_size.width, info.buffer_size.height};
    s->ctx->wm->on_toplevel_commit(s->toplevel, info);
}

/// Ends a commit (or a desync flush) that applied state somewhere in a tree: the role
/// finisher when the root itself committed, then the root's scene, so that every node,
/// origin and paint position of this update lands in the same snapshot.
void finish_commit(surface_obj* s, bool root_committed, const char* cause) {
    auto* tree = s->ctx->tree;
    const lx::surface_id root = tree ? tree->root_of(s->id) : s->id;
    if (root_committed && root == s->id)
        finish_role(s);
    if (tree)
        tree->sync_scene(root, cause);
}

} // namespace

/// A surface stopped being effectively synchronized (set_desync, its wl_subsurface or its
/// parent went away): "all SCUs ... become DCUs", and with no constraints that means they
/// apply now — top-down, so a parent's state is current before its children's.
void flush_desync(surface_obj* s, const char* cause) {
    auto* tree = s->ctx ? s->ctx->tree : nullptr;
    if (!tree)
        return;
    tree->for_each_preorder(s->id, [&](lx::surface_id id) {
        surface_obj* n = surface_by_id(id);
        if (n && n->has_cached && !tree->effectively_sync(id))
            apply_cached(n, cause, 0);
        // Always descend: a desynchronized grandchild may hold state that was only cached
        // because this surface was synchronized.
        return true;
    });
    finish_commit(s, false, cause);
}

namespace {

void surface_commit(struct wl_client*, struct wl_resource* resource) {
    auto* s = surface_from(resource);
    if (!s || !s->ctx || !s->ctx->surfaces)
        return;

    // Role rules that reject the whole commit come first, before any state moves.
    if (!role_prechecks(s))
        return;

    merge_pending_into_cached(s);
    auto* tree = s->ctx->tree;
    if (tree)
        tree->cache_child_state(s->id);

    const bool is_sub = s->role == surface_role::subsurface;
    if (tree && tree->effectively_sync(s->id)) {
        // A synchronized content update: it waits for the parent's state to be applied.
        if (surface_debug()) {
            char line[128];
            std::snprintf(line, sizeof(line), "subsurface commit surface=%llu mode=sync cached=1",
                          s->id.id());
            surface_log(line);
        }
        return;
    }
    if (is_sub && surface_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "subsurface commit surface=%llu mode=desync cached=0",
                      s->id.id());
        surface_log(line);
    }
    apply_cached(s, is_sub ? "desync-commit" : "commit", 0);
    finish_commit(s, true, "commit");
}

void surface_set_buffer_transform(struct wl_client*, struct wl_resource* resource,
                                  int32_t transform) {
    if (auto* s = surface_from(resource))
        s->pending.transform = transform;
}

void surface_set_buffer_scale(struct wl_client*, struct wl_resource* resource, int32_t scale) {
    if (auto* s = surface_from(resource))
        s->pending.scale = scale > 0 ? scale : 1;
}

void surface_damage_buffer(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                           int32_t width, int32_t height) {
    surface_damage(nullptr, resource, x, y, width, height);
}

void surface_offset(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y) {
    if (auto* s = surface_from(resource)) {
        s->pending.dx = x;
        s->pending.dy = y;
    }
}

/// A role object (wl_subsurface, xdg_toplevel, xdg_popup, zwlr_layer_surface_v1) still
/// alive for this surface.
[[nodiscard]] bool has_live_role_object(const surface_obj* s) {
    return s->sub != nullptr || s->layer != nullptr ||
           (s->xdg && (s->xdg->toplevel || s->xdg->popup));
}

const struct wl_surface_interface surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            // wl_surface: "they must destroy this role object before the wl_surface,
            // otherwise a defunct_role_object error is sent". (Only the request: libwayland
            // tearing a client down destroys in id order, wl_surface first, and that is not
            // the client's doing.)
            if (const auto* s = surface_from(resource); s && has_live_role_object(s)) {
                wl_resource_post_error(resource, WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                                       "wl_surface destroyed before its role object");
                return;
            }
            wl_resource_destroy(resource);
        },
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
#if defined(WL_SURFACE_GET_RELEASE_SINCE_VERSION)
    .get_release =
        [](struct wl_client* client, struct wl_resource*, uint32_t callback) {
            // Optional v7 release callback — create and immediately complete unused.
            wl_resource* cb = wl_resource_create(client, &wl_callback_interface, 1, callback);
            if (cb) {
                wl_callback_send_done(cb, 0);
                wl_resource_destroy(cb);
            }
        },
#endif
};

// ── wl_compositor ──────────────────────────────────────────────────────────

void compositor_create_surface(struct wl_client* client, struct wl_resource* resource,
                               uint32_t id) {
    auto* ctx = static_cast<p0_protocol_context*>(wl_resource_get_user_data(resource));
    if (!ctx || !ctx->server)
        return;
    auto* conn = ctx->server->find_client_by_native(client);
    if (!conn)
        conn = ctx->server->ensure_client(client);
    if (!conn)
        return;

    surface_obj* s = alloc_surface();
    if (!s) {
        wl_client_post_no_memory(client);
        return;
    }
    s->resource = wl_resource_create(client, &wl_surface_interface,
                                     wl_resource_get_version(resource), id);
    if (!s->resource) {
        s->used = false;
        wl_client_post_no_memory(client);
        return;
    }
    s->ctx = ctx;
    s->id = next_surface_id();
    s->client = client_handle(*conn);
    s->opaque_current.reset_empty(); // wl_surface: the initial opaque region is empty
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_destroy_resource);
    // Registered for O(1) id → resource lookups (subsurface trees name surfaces by id) and
    // as a tree member. Both tables match this one's size, so this only fails when the
    // surface tables disagree — refuse the surface rather than half-track it.
    const bool registered = (!ctx->surfaces || ctx->surfaces->register_surface(s->id, s->resource)) &&
                            (!ctx->tree || ctx->tree->add_surface(s->id));
    if (!registered) {
        wl_client_post_no_memory(client);
        return;
    }
}

void compositor_create_region(struct wl_client* client, struct wl_resource* resource,
                              uint32_t id) {
    region_obj* r = nullptr;
    for (auto& slot : regions)
        if (!slot.used) {
            r = &slot;
            break;
        }
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    r->used = true;
    r->region.reset_empty();
    r->resource = wl_resource_create(client, &wl_region_interface,
                                     wl_resource_get_version(resource), id);
    if (!r->resource) {
        r->used = false;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r->resource, &region_impl, r, region_destroy_resource);
}

const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
#if defined(WL_COMPOSITOR_RELEASE_SINCE_VERSION)
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
#endif
};

} // namespace

void bind_compositor(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_compositor_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&compositor_impl, g_p0_ctx, nullptr);
}

void fire_frames(unsigned frame_time_ms) {
    for (unsigned i = 0; i < g_pending_frame_count; ++i) {
        auto* cb = g_pending_frames[i].resource;
        // Clear the slot before destroying: wl_resource_destroy runs the destructor, which
        // scans this same table.
        g_pending_frames[i] = {};
        if (cb) {
            wl_callback_send_done(cb, frame_time_ms);
            wl_resource_destroy(cb);
        }
    }
    g_pending_frame_count = 0;
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
