module;

// wl_surface and wl_compositor: the double-buffered surface state, the four-step commit
// (role prechecks, pending -> cached, apply_cached, finish_commit), wl_region and frame
// callbacks. Part of the protocol wire split; shared declarations live in the :wire
// partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <time.h>

#if defined(__linux__)
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#endif

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

#include <unistd.h>

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import lx.input;
import lx.scene;
import :surface;
import :surface_tree;
import :toplevel;
import :output;
import :dnd;
import :viewport;
import :popup;
import :layer_shell;
import :content_update;
import :wire;

namespace {

constexpr lx::trace::category log_cursor{"compositor.cursor"};
constexpr lx::trace::category log_render{"compositor.render"};
constexpr lx::trace::category log_wm{"compositor.wm"};

} // namespace

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

surface_obj surfaces[k_max_surfaces]{};

namespace {

region_obj regions[k_max_regions]{};

// Pending frame callbacks across all surfaces (fired after present).
struct pending_frame {
    wl_resource* resource = nullptr;
    /// The surface whose content update queued it: its visibility paces the answer.
    lx::surface_id surface{};
    /// Held back by the throttle (its surface was not on screen at the last answer).
    bool throttled = false;
};
pending_frame g_pending_frames[512]{};
unsigned g_pending_frame_count = 0;
/// Entries of `g_pending_frames` flagged `throttled`.
unsigned g_throttled_frame_count = 0;

/// A surface that is not on screen gets its frame callbacks at most this often (niri's
/// FRAME_CALLBACK_THROTTLE): a window under another, off the output or hidden by the lock
/// stops rendering at the display rate without being starved.
constexpr unsigned long long k_frame_throttle_ns = 995'000'000ull;
/// Readiness-poller keys from here on belong to other units (`cu_poll_add`), never to an
/// update index (the pool has 512).
constexpr unsigned k_poll_foreign_base = 0x10000u;

/// Surfaces with a fifo barrier set: the latch scan is skipped while there are none.
unsigned g_barriers = 0;

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

/// Pixel size of a wl_buffer (shm or dma-buf); false for null or an unknown kind.
bool buffer_dims(wl_resource* buffer, int& w, int& h) {
    if (!buffer)
        return false;
    if (wl_shm_buffer* shm = wl_shm_buffer_get(buffer)) {
        w = wl_shm_buffer_get_width(shm);
        h = wl_shm_buffer_get_height(shm);
        return w > 0 && h > 0;
    }
    if (const buffer_obj* b = find_buffer(buffer)) {
        w = static_cast<int>(b->width);
        h = static_cast<int>(b->height);
        return w > 0 && h > 0;
    }
    return false;
}

lx::compositor::surface_geometry_in geometry_from(const surface_pending& st, int buffer_w,
                                                  int buffer_h) {
    lx::compositor::surface_geometry_in in{};
    in.buffer_w = buffer_w;
    in.buffer_h = buffer_h;
    in.scale = st.scale > 0 ? st.scale : 1;
    in.transform = static_cast<lx::buffer_transform>(st.transform & 7);
    in.has_src = st.viewport.has_src;
    in.src_x = st.viewport.src_x;
    in.src_y = st.viewport.src_y;
    in.src_w = st.viewport.src_w;
    in.src_h = st.viewport.src_h;
    in.has_dst = st.viewport.has_dst;
    in.dst_w = st.viewport.dst_w;
    in.dst_h = st.viewport.dst_h;
    return in;
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

void queue_frame_callback(wl_resource* cb, lx::surface_id surface) {
    if (!cb)
        return;
    if (g_pending_frame_count >= 512) {
        fire_frame_callback_now(cb);
        return;
    }
    pending_frame& e = g_pending_frames[g_pending_frame_count++];
    e.resource = cb;
    e.surface = surface;
    e.throttled = false;
}

// ── frame-callback lists (Q6) ─────────────────────────────────────────────
// Every wl_surface.frame callback not yet applied sits in one node of a shared pool, linked
// into the list of the content update that carries it (pending, cached, or queued). A list
// moves between updates by splicing three words; the callback's user data is its node, so
// its destructor unlinks it in O(1). Applied callbacks move to `g_pending_frames`.

struct frame_cb_node {
    wl_resource* resource = nullptr;
    unsigned next = 0; // index + 1
};
constexpr unsigned k_frame_cb_nodes = 2048;
frame_cb_node g_cb_nodes[k_frame_cb_nodes]{};
unsigned g_cb_free = 0; // index + 1
bool g_cb_ready = false;

[[nodiscard]] unsigned cb_node_alloc() {
    if (!g_cb_ready) {
        g_cb_ready = true;
        for (unsigned i = 0; i < k_frame_cb_nodes; ++i)
            g_cb_nodes[i].next = i + 1 < k_frame_cb_nodes ? i + 2 : 0;
        g_cb_free = 1;
    }
    const unsigned n = g_cb_free;
    if (n)
        g_cb_free = g_cb_nodes[n - 1].next;
    return n;
}

void cb_node_free(unsigned n) {
    g_cb_nodes[n - 1] = {};
    g_cb_nodes[n - 1].next = g_cb_free;
    g_cb_free = n;
}

/// Unlinks the list's oldest node and returns its callback (null when the client already
/// destroyed it).
wl_resource* cb_list_pop(frame_cb_list& l) {
    const unsigned n = l.head;
    if (!n)
        return nullptr;
    wl_resource* cb = g_cb_nodes[n - 1].resource;
    l.head = g_cb_nodes[n - 1].next;
    if (!l.head)
        l.tail = 0;
    --l.count;
    cb_node_free(n);
    if (cb)
        wl_resource_set_user_data(cb, nullptr);
    return cb;
}

void cb_list_push(frame_cb_list& l, wl_resource* cb) {
    // Full: the oldest is answered now rather than dropped (a dropped callback stalls the
    // client for good).
    if (l.count >= k_max_frame_cbs)
        fire_frame_callback_now(cb_list_pop(l));
    const unsigned n = cb_node_alloc();
    if (!n) {
        fire_frame_callback_now(cb);
        return;
    }
    g_cb_nodes[n - 1].resource = cb;
    g_cb_nodes[n - 1].next = 0;
    wl_resource_set_user_data(cb, reinterpret_cast<void*>(static_cast<std::uintptr_t>(n)));
    if (l.tail)
        g_cb_nodes[l.tail - 1].next = n;
    else
        l.head = n;
    l.tail = n;
    ++l.count;
}

/// Appends `from` to `into` (callbacks of an update that never applied accumulate).
void cb_list_splice(frame_cb_list& into, frame_cb_list& from) {
    if (!from.head)
        return;
    if (into.tail)
        g_cb_nodes[into.tail - 1].next = from.head;
    else
        into.head = from.head;
    into.tail = from.tail;
    into.count += from.count;
    from = {};
    while (into.count > k_max_frame_cbs)
        fire_frame_callback_now(cb_list_pop(into));
}

/// Frees the nodes without answering (a surface going away, its client torn down).
void cb_list_forget(frame_cb_list& l) {
    while (l.head)
        (void)cb_list_pop(l);
}

/// libwayland destroys a client's resources on disconnect, so every table holding a
/// wl_resource* must drop it here or the next send touches freed memory.
void frame_callback_destroy_resource(struct wl_resource* resource) {
    // Still in a list: its node forgets it (the list keeps the empty node until it moves).
    const auto n = static_cast<unsigned>(
        reinterpret_cast<std::uintptr_t>(wl_resource_get_user_data(resource)));
    if (n >= 1 && n <= k_frame_cb_nodes && g_cb_nodes[n - 1].resource == resource) {
        g_cb_nodes[n - 1].resource = nullptr;
        return;
    }
    for (unsigned i = 0; i < g_pending_frame_count; ++i) {
        if (g_pending_frames[i].resource == resource)
            g_pending_frames[i].resource = nullptr;
    }
}

} // namespace

void frame_cbs_fire_now(frame_cb_list& list) {
    while (list.head)
        fire_frame_callback_now(cb_list_pop(list));
}

// ── wl_surface ─────────────────────────────────────────────────────────────

namespace {
const surface_pending* queued_tail_state(const surface_obj* s);
} // namespace

lx::compositor::surface_geometry_in pending_geometry(const surface_obj* s) {
    // The buffer the commit leaves in place: newly attached, else one a synchronized
    // update still holds, else the one on screen.
    int w = 0, h = 0;
    const surface_pending& p = s->pending;
    if (p.buffer_set) {
        (void)buffer_dims(p.buffer, w, h);
    } else if (s->has_cached && s->cached.buffer_set) {
        (void)buffer_dims(s->cached.buffer, w, h);
    } else if (const surface_pending* tail = queued_tail_state(s); tail && tail->buffer_set) {
        (void)buffer_dims(tail->buffer, w, h);
    } else if (s->has_content) {
        if (!buffer_dims(s->current.buffer, w, h) && s->ctx && s->ctx->surfaces) {
            // The wl_buffer object is gone; its content (and size) stays on screen.
            const lx::size2i b = s->ctx->surfaces->buffer_size(s->id);
            w = b.width;
            h = b.height;
        }
    }
    // Scale, transform and viewport are sticky in `pending`.
    return geometry_from(p, w, h);
}

lx::compositor::surface_geometry_in current_geometry(const surface_obj* s) {
    int w = 0, h = 0;
    if (s->has_content && !buffer_dims(s->current.buffer, w, h) && s->ctx && s->ctx->surfaces) {
        const lx::size2i b = s->ctx->surfaces->buffer_size(s->id);
        w = b.width;
        h = b.height;
    }
    return geometry_from(s->current, w, h);
}

/// Drops a surface's cached content update without applying it — for a client that is
/// being torn down, whose buffers and callbacks libwayland is destroying anyway.
void discard_cached(surface_obj* s) {
    presentation_drop_cached(s);
    cb_list_forget(s->cached.frame_cbs);
    sync_point_drop(s->cached.acquire);
    sync_point_drop(s->cached.release);
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
    // Queued content updates are discarded — buffers released, release points signalled,
    // frame callbacks answered (a dropped one stalls the client) — and the commit-path
    // add-ons go inert.
    cu_surface_destroyed(s, dying);
    surface_addons_destroyed(s);
    // The dma-buf it shows is released once no submitted frame samples it any more.
    if (s->ctx && s->ctx->lifecycle)
        s->ctx->lifecycle->on_detach(s->id);
    if (s->fifo_barrier) {
        s->fifo_barrier = false;
        --g_barriers;
    }
    cb_list_forget(s->pending.frame_cbs);
    sync_point_drop(s->pending.acquire);
    sync_point_drop(s->pending.release);
    if (s->has_cached) {
        cb_list_forget(s->cached.frame_cbs);
        sync_point_drop(s->cached.acquire);
        if (dying)
            sync_point_drop(s->cached.release);
        else
            sync_point_signal_now(s->cached.release);
    }
    // Its pointer constraints go inert first (an active one is told it ended), so the focus
    // change below re-evaluates without them.
    pointer_constraints_surface_destroyed(s);
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
    // A lock surface: its object goes inert, its output shows the solid colour.
    if (s->lock_surface)
        lock_surface_surface_destroyed(s);
    // An input-method popup: hidden, its object inert.
    if (s->input_popup)
        input_popup_surface_destroyed(s);
    // A drag's icon or target: the drag goes on without it (the icon leaves its layer while
    // the tree still knows the root).
    if (s->ctx && s->ctx->dnd)
        s->ctx->dnd->on_surface_destroyed(s->id);
    // Activation tokens that name it as their requester (by id: nothing dangles).
    activation_surface_destroyed(s->id);
    // Its keyboard-shortcuts inhibitor goes inert.
    shortcuts_inhibit_surface_destroyed(s);
    // Its wp_viewport and wp_fractional_scale_v1 go inert.
    viewport_surface_destroyed(s);
    // Every wp_presentation_feedback of its updates — pending, cached or in flight — is
    // discarded ("its surface destroyed").
    presentation_surface_destroyed(s);
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
    // wl_surface v5: the offset moved to wl_surface.offset; attach must pass 0, 0.
    if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION && (x != 0 || y != 0)) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                               "attach with a non-zero offset (use wl_surface.offset)");
        return;
    }
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
    if (wl_resource_get_version(resource) < WL_SURFACE_OFFSET_SINCE_VERSION) {
        s->pending.dx = x;
        s->pending.dy = y;
    }
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

/// Adds `x, y, width, height` to a commit's accumulated damage. A client may send several
/// damage requests per commit, and keeping only the last one under-reports what changed;
/// they are kept as separate rects (`client_damage`), so two lines far apart upload and
/// repaint as two strips. Formed in 64 bits: INT32_MAX extents are common ("everything",
/// wev, GTK, SDL).
void accumulate_damage(client_damage& acc, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return;
    const lx::rect2i incoming = clip_damage_to_buffer({x, y, width, height}, INT32_MAX, INT32_MAX);
    if (incoming.width <= 0 || incoming.height <= 0)
        return;
    acc.add(incoming);
}

/// wl_surface.damage: surface-local (logical) coordinates.
void surface_damage(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                    int32_t width, int32_t height) {
    if (auto* s = surface_from(resource))
        accumulate_damage(s->pending.damage_surface, x, y, width, height);
}

void surface_frame(struct wl_client* client, struct wl_resource* resource, uint32_t callback_id) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    wl_resource* cb =
        wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (!cb)
        return;
    wl_resource_set_implementation(cb, nullptr, nullptr, frame_callback_destroy_resource);
    cb_list_push(s->pending.frame_cbs, cb);
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
    if (s->pending.input.approximated && log_wm.enabled(lx::trace::level::debug)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            log_wm.debug("input region exceeds 16 rects — over-approximated "
                         "(accepts more input than asked). Reported once.");
        }
    }
}

/// wl_surface.invalid_size on the state the commit produces: without a viewport source,
/// the buffer must be a multiple of the scale. Cursor and role-less surfaces are tolerated
/// with one debug line (wlroots does the same; wayland#194 — cursor themes ship odd sizes).
bool geometry_prechecks(surface_obj* s) {
    const lx::compositor::surface_geometry_in in = pending_geometry(s);
    if (lx::compositor::check_buffer_scale(in) == lx::compositor::surface_size_error::none)
        return true;
    if (s->role == surface_role::none || s->role == surface_role::cursor) {
        if (!s->invalid_size_logged && log_surface.enabled(lx::trace::level::debug)) {
            s->invalid_size_logged = true;
            log_surface.debug("client bug: buffer {}x{} is not divisible by scale {} (surface={}, "
                              "tolerated for this role)",
                              in.buffer_w, in.buffer_h, in.scale, s->id.id());
        }
        return true;
    }
    wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                           "buffer size is not divisible by the buffer scale");
    return false;
}

/// A client that keeps committing an unimportable buffer would otherwise log once per
/// frame on the commit hot path. Report the first failure and then periodically.
void log_commit_failure(const lx::error& err) {
    static unsigned suppressed = 0;
    static constexpr unsigned k_report_interval = 600;
    if (suppressed++ % k_report_interval == 0)
        log_surface.error(err);
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
                s->current.dx, s->current.dy, s->surface_size.width, s->surface_size.height);
            wl_shm_buffer_end_access(shm);
        }
    } else {
        // Recorded trade-off: a dma-buf or non-8888 cursor keeps the previous image —
        // reading one back for a 64x64 plane is not worth a GPU round trip.
        static bool reported = false;
        if (!reported && log_cursor.enabled(lx::trace::level::debug)) {
            reported = true;
            log_cursor.debug("cursor surface buffer is not ARGB/XRGB shm — "
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
        static_cast<unsigned>(stride) / 4u, format == WL_SHM_FORMAT_XRGB8888, 0, 0,
        s->surface_size.width, s->surface_size.height);
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
/// whole surface (the region is surface-local, so it is compared with the logical size).
[[nodiscard]] bool content_opaque(const surface_obj* s) {
    return s->format_opaque ||
           region_covers(s->opaque_current, s->surface_size.width, s->surface_size.height);
}

/// Content that is not opaque as a whole but has an opaque region (a CSD window's body
/// inside its shadow and rounded corners): the region's largest rects clipped to the
/// surface, largest first, for the visibility pass to hide what lies under them. None for an
/// over-approximated region, as `region_covers`.
using opaque_rect_set = lx::rect2i[lx::scene::k_max_opaque_rects];
[[nodiscard]] unsigned partial_opaque_rects(const surface_obj* s, opaque_rect_set& out) {
    const lx::compositor::input_region& region = s->opaque_current;
    const int w = s->surface_size.width;
    const int h = s->surface_size.height;
    if (content_opaque(s) || region.infinite || region.approximated || w <= 0 || h <= 0)
        return 0;
    constexpr unsigned k_keep = lx::scene::k_max_opaque_rects;
    const auto area = [](lx::rect2i r) { return static_cast<long long>(r.width) * r.height; };
    unsigned n = 0;
    for (unsigned i = 0; i < region.count; ++i) {
        const lx::rect2i c = lx::compositor::clip_to(region.rects[i], w, h);
        if (c.width <= 0 || c.height <= 0)
            continue;
        if (n == k_keep && area(c) <= area(out[n - 1]))
            continue;
        // Kept sorted by area, largest first: the smallest makes room.
        unsigned at = n < k_keep ? n++ : n - 1;
        for (; at > 0 && area(out[at - 1]) < area(c); --at)
            out[at] = out[at - 1];
        out[at] = c;
    }
    return n;
}

/// The applied blur region (ext-background-effect-v1) clipped to the surface — "clipped by
/// the compositor to the surface size" — as disjoint rects, each blurred once: a wl_region's
/// rects may overlap, and a pixel under two blur draws would be blurred twice. A region that
/// needs more than `k_max_blur_rects` (or an over-approximated one) is blurred over its
/// bounds — the protocol leaves the blur to compositor policy.
using blur_rect_set = lx::rect2i[lx::scene::surface_node::k_max_blur_rects];
[[nodiscard]] unsigned blur_rects(const surface_obj* s, blur_rect_set& out) {
    const lx::compositor::input_region& region = s->blur_current;
    const int w = s->surface_size.width;
    const int h = s->surface_size.height;
    if (region.infinite || w <= 0 || h <= 0)
        return 0;
    constexpr unsigned k_keep = lx::scene::surface_node::k_max_blur_rects;
    unsigned n = 0;
    bool overflow = region.approximated;
    lx::rect2i bounds{};
    for (unsigned i = 0; i < region.count; ++i) {
        const lx::rect2i c = lx::compositor::clip_to(region.rects[i], w, h);
        if (c.width <= 0 || c.height <= 0)
            continue;
        bounds = bounds.width > 0
                     ? lx::rect2i{std::min(bounds.x, c.x), std::min(bounds.y, c.y),
                                  std::max(bounds.x + bounds.width, c.x + c.width) -
                                      std::min(bounds.x, c.x),
                                  std::max(bounds.y + bounds.height, c.y + c.height) -
                                      std::min(bounds.y, c.y)}
                     : c;
        if (overflow)
            continue;
        // `c` minus every rect kept so far, piece by piece (a cut leaves at most four).
        lx::rect2i pieces[4 * k_keep]{c};
        unsigned count = 1;
        for (unsigned k = 0; k < n && count > 0 && !overflow; ++k) {
            const lx::rect2i o = out[k];
            lx::rect2i next[4 * k_keep]{};
            unsigned m = 0;
            for (unsigned p = 0; p < count && !overflow; ++p) {
                const lx::rect2i r = pieces[p];
                const int x0 = std::max(r.x, o.x), y0 = std::max(r.y, o.y);
                const int x1 = std::min(r.x + r.width, o.x + o.width);
                const int y1 = std::min(r.y + r.height, o.y + o.height);
                lx::rect2i cut[4]{};
                unsigned q = 0;
                if (x1 <= x0 || y1 <= y0) {
                    cut[q++] = r;
                } else {
                    if (y0 > r.y)
                        cut[q++] = {r.x, r.y, r.width, y0 - r.y};
                    if (y1 < r.y + r.height)
                        cut[q++] = {r.x, y1, r.width, r.y + r.height - y1};
                    if (x0 > r.x)
                        cut[q++] = {r.x, y0, x0 - r.x, y1 - y0};
                    if (x1 < r.x + r.width)
                        cut[q++] = {x1, y0, r.x + r.width - x1, y1 - y0};
                }
                if (m + q > 4 * k_keep)
                    overflow = true;
                for (unsigned t = 0; t < q && !overflow; ++t)
                    next[m++] = cut[t];
            }
            for (unsigned t = 0; t < m; ++t)
                pieces[t] = next[t];
            count = m;
        }
        if (n + count > k_keep)
            overflow = true;
        for (unsigned p = 0; p < count && !overflow; ++p)
            out[n++] = pieces[p];
    }
    if (overflow && bounds.width > 0) {
        out[0] = bounds;
        return 1;
    }
    return n;
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
    case surface_role::lock:
        // While its ext_session_lock_surface_v1 lives and belongs to the lock in charge.
        return lock_surface_drawable(s);
    case surface_role::input_popup:
        // While its zwp_input_popup_surface_v2 lives and its input method is the seat's.
        return input_popup_drawable(s);
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
    return true;
}


/// The toplevel's record when `s` is an xdg_toplevel with one.
lx::compositor::toplevel_record* toplevel_rec(surface_obj* s) {
    if (s->role == surface_role::xdg_toplevel && s->toplevel && s->ctx->toplevels)
        return s->ctx->toplevels->find_mut(s->toplevel);
    return nullptr;
}

/// The role state a commit carries (Q4): xdg window geometry, size limits and the acked
/// configure of a toplevel; a popup's window geometry; a layer surface's pending props.
/// Taken at the commit when the update is queued, applied when it is — so a queued buffer is
/// never shown with geometry acked for a later one. (Applied at once on the fast path.)
struct role_snapshot {
    surface_role role = surface_role::none;
    bool min_set = false;
    bool max_set = false;
    lx::size2i min{};
    lx::size2i max{};
    bool wg_set = false;
    lx::rect2i wg{};
    bool acked_unapplied = false;
    lx::compositor::toplevel_configure acked{};
    bool layer_set = false;
    lx::compositor::layer_props layer{};
};

/// Moves the record's pending role state into `snap` (the update is queued).
void take_role_snapshot(surface_obj* s, role_snapshot& snap) {
    snap = {};
    snap.role = s->role;
    if (auto* tl = toplevel_rec(s)) {
        snap.min_set = tl->min_pending_set;
        snap.min = tl->min_pending;
        snap.max_set = tl->max_pending_set;
        snap.max = tl->max_pending;
        snap.wg_set = tl->window_geometry_pending_set;
        snap.wg = tl->window_geometry_pending;
        tl->min_pending_set = tl->max_pending_set = tl->window_geometry_pending_set = false;
        snap.acked_unapplied = tl->acked_unapplied;
        snap.acked = tl->acked;
        tl->acked_unapplied = false;
    } else if (s->role == surface_role::popup && s->ctx->popups) {
        if (auto* rec = s->ctx->popups->find(s->id)) {
            snap.wg_set = rec->window_geometry_pending_set;
            snap.wg = rec->window_geometry_pending;
            rec->window_geometry_pending_set = false;
        }
    } else if (s->role == surface_role::layer && s->ctx->layers && s->layer) {
        if (auto* rec = s->ctx->layers->find(s->id); rec && !rec->closed) {
            snap.layer_set = true;
            snap.layer = rec->pending;
        }
    }
}

/// The pending role state now, set aside while a queued update's snapshot is applied.
struct role_stash {
    role_snapshot pending{};
    bool acked_unapplied = false;
    lx::compositor::toplevel_configure acked{};
};

/// Makes `snap` the record's pending role state and applies it (as the fast path would),
/// keeping what the client has set since in `stash`.
void apply_role_snapshot(surface_obj* s, const role_snapshot& snap, role_stash& stash) {
    if (auto* tl = toplevel_rec(s)) {
        stash.pending.min_set = tl->min_pending_set;
        stash.pending.min = tl->min_pending;
        stash.pending.max_set = tl->max_pending_set;
        stash.pending.max = tl->max_pending;
        stash.pending.wg_set = tl->window_geometry_pending_set;
        stash.pending.wg = tl->window_geometry_pending;
        stash.acked_unapplied = tl->acked_unapplied;
        stash.acked = tl->acked;
        tl->min_pending_set = snap.min_set;
        tl->min_pending = snap.min;
        tl->max_pending_set = snap.max_set;
        tl->max_pending = snap.max;
        tl->window_geometry_pending_set = snap.wg_set;
        tl->window_geometry_pending = snap.wg;
        tl->acked_unapplied = snap.acked_unapplied;
        if (snap.acked_unapplied)
            tl->acked = snap.acked;
        (void)lx::compositor::toplevel_manager::apply_pending_state(*tl); // validated at commit
    } else if (s->role == surface_role::popup && s->ctx->popups) {
        if (auto* rec = s->ctx->popups->find(s->id)) {
            stash.pending.wg_set = rec->window_geometry_pending_set;
            stash.pending.wg = rec->window_geometry_pending;
            rec->window_geometry_pending_set = snap.wg_set;
            rec->window_geometry_pending = snap.wg;
            s->ctx->popups->apply_pending_state(s->id);
        }
    } else if (snap.layer_set && s->ctx->layers && s->layer) {
        if (auto* rec = s->ctx->layers->find(s->id); rec && !rec->closed) {
            stash.pending.layer_set = true;
            stash.pending.layer = rec->pending;
            rec->pending = snap.layer;
            s->ctx->layers->apply_pending(s->id);
        }
    }
}

/// After the queued update's role finisher ran: the client's newer pending state is put
/// back, and a configure it acked since the queued commit is still waiting to be applied.
void restore_role_stash(surface_obj* s, const role_stash& stash) {
    if (auto* tl = toplevel_rec(s)) {
        tl->min_pending_set = stash.pending.min_set;
        tl->min_pending = stash.pending.min;
        tl->max_pending_set = stash.pending.max_set;
        tl->max_pending = stash.pending.max;
        tl->window_geometry_pending_set = stash.pending.wg_set;
        tl->window_geometry_pending = stash.pending.wg;
        if (stash.acked_unapplied) {
            tl->acked = stash.acked;
            tl->acked_unapplied = true;
        }
    } else if (s->role == surface_role::popup && s->ctx->popups) {
        if (auto* rec = s->ctx->popups->find(s->id)) {
            rec->window_geometry_pending_set = stash.pending.wg_set;
            rec->window_geometry_pending = stash.pending.wg;
        }
    } else if (stash.pending.layer_set && s->ctx->layers && s->layer) {
        if (auto* rec = s->ctx->layers->find(s->id); rec && !rec->closed)
            rec->pending = stash.pending.layer;
    }
}

/// The fast path: the commit's role state is applied now (validated by the prechecks).
void apply_role_pending(surface_obj* s) {
    if (auto* tl = toplevel_rec(s))
        (void)lx::compositor::toplevel_manager::apply_pending_state(*tl);
    else if (s->role == surface_role::popup && s->ctx->popups)
        s->ctx->popups->apply_pending_state(s->id);
    else if (s->role == surface_role::layer && s->ctx->layers && s->layer)
        s->ctx->layers->apply_pending(s->id);
}

/// The xdg-shell rules that reject a whole commit, checked on the pending state before
/// anything moves. Validation only: the role state moves with the update (applied now on
/// the fast path, or snapshotted with a queued update). False when an error was posted.
bool role_prechecks(surface_obj* s) {
    if (s->role == surface_role::popup)
        return popup_prechecks(s);
    if (s->role == surface_role::lock)
        return lock_surface_precheck(s);
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
    // min ≤ max on the limits this commit produces (pending over current). A queued
    // update's limits are already on the record's history: they were valid when committed,
    // and a later commit that sets only one of the two is checked against current, as the
    // record applies them.
    const lx::size2i min = tl->min_pending_set ? tl->min_pending : tl->min_size;
    const lx::size2i max = tl->max_pending_set ? tl->max_pending : tl->max_size;
    if ((min.width > 0 && max.width > 0 && min.width > max.width) ||
        (min.height > 0 && max.height > 0 && min.height > max.height)) {
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
void merge_state(surface_obj* s, surface_pending& p, surface_pending& c) {
    if (p.buffer_set) {
        // Not when it is the buffer on screen: a dma-buf there is still being sampled and
        // the lifecycle tracker releases it. An shm buffer was copied and released at its
        // own commit, so a re-attach of it is owed a release of its own.
        if (c.buffer_set && c.buffer && c.buffer != p.buffer &&
            (c.buffer != s->current.buffer || wl_shm_buffer_get(c.buffer)))
            wl_buffer_send_release(c.buffer);
        c.buffer = p.buffer;
        c.buffer_set = true;
        // Explicit sync: the superseded buffer was never sampled, so its release point is
        // signalled now; its acquire point is moot. The new points travel with the update.
        sync_point_signal_now(c.release);
        sync_point_drop(c.acquire);
        c.acquire = p.acquire;
        c.release = p.release;
        p.acquire = {};
        p.release = {};
    }
    if (p.commit_seq)
        c.commit_seq = p.commit_seq;
    c.alpha = p.alpha;
    c.content_type = p.content_type;
    c.tearing = p.tearing;
    c.fifo_set = c.fifo_set || p.fifo_set;
    c.fifo_wait = c.fifo_wait || p.fifo_wait;
    if (p.target_ns > c.target_ns)
        c.target_ns = p.target_ns;
    p.fifo_set = false;
    p.fifo_wait = false;
    p.target_ns = 0;
    c.dx += p.dx;
    c.dy += p.dy;
    c.scale = p.scale;
    c.transform = p.transform;
    c.viewport = p.viewport;
    // Damage of updates that were never applied accumulates, each kind in its own space:
    // converting at apply time uses the geometry of the state that is finally applied.
    c.damage_surface.add_region(p.damage_surface);
    c.damage_buffer.add_region(p.damage_buffer);
    cb_list_splice(c.frame_cbs, p.frame_cbs);
    // Presentation feedback moves with the update; a cached update it replaces is discarded.
    presentation_merge_lists(p.feedback, c.feedback);
    if (p.input_set) {
        c.input = p.input;
        c.input_set = true;
    }
    if (p.opaque_set) {
        c.opaque = p.opaque;
        c.opaque_set = true;
    }
    if (p.blur_set) {
        c.blur = p.blur;
        c.blur_set = true;
    }
    if (p.color_set) {
        c.color = p.color;
        c.color_has = p.color_has;
        c.color_set = true;
    }
    p.buffer = nullptr;
    p.buffer_set = false;
    p.dx = 0;
    p.dy = 0;
    p.damage_surface.clear();
    p.damage_buffer.clear();
    p.input_set = false;
    p.opaque_set = false;
    p.blur_set = false;
    p.color_set = false;
}

void merge_pending_into_cached(surface_obj* s) {
    merge_state(s, s->pending, s->cached);
    s->has_cached = true;
}

/// What one applied update damaged, in both spaces.
struct update_damage {
    bool any = false;
    /// Surface-local (logical): what the scene repaints.
    client_damage surface{};
    /// Buffer pixels: what the texture upload copies.
    client_damage buffer{};
};

/// The import path: hands a newly applied buffer to the surface manager, stages its pixels
/// (shm) or describes the import (dma-buf) for the render thread, and releases or tracks
/// the wl_buffer. Every texture update is stamped with the snapshot that may first show it.
void import_current_buffer(surface_obj* s, const update_damage& damage,
                           const lx::compositor::surface_geometry& geo) {
    buffer_obj* buf = find_buffer(s->current.buffer);
    wl_shm_buffer* shm = wl_shm_buffer_get(s->current.buffer);
    if (!buf && shm)
        buf = track_shm_buffer(s->current.buffer, shm);
    if (!buf) {
        // Not a buffer kind this compositor can show; nothing will ever sample it.
        wl_buffer_send_release(s->current.buffer);
        return;
    }
    if (buf->is_single_pixel) {
        // wp_single_pixel_buffer_v1: a colour, not pixels — the node draws it as a solid
        // rect of its logical size (the viewport destination sizes it). Nothing is uploaded
        // or kept, so the buffer goes straight back.
        s->format_opaque = buf->solid.a >= 1.f;
        s->current_buffer_id = buf->id;
        s->current_texture = 0;
        s->ctx->surfaces->set_solid(s->id, buf->solid, geo.size, content_opaque(s));
        if (log_surface.enabled(lx::trace::level::debug)) {
            log_surface.debug(
                "surface solid surface={} rgba={:.3f},{:.3f},{:.3f},{:.3f} size={}x{} "
                "opaque={}",
                s->id.id(), buf->solid.r, buf->solid.g, buf->solid.b, buf->solid.a, geo.size.width,
                geo.size.height, content_opaque(s) ? 1 : 0);
        }
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
        s->ctx->surfaces->register_dmabuf(buf->id, std::move(desc));
    } else if (buf->is_shm) {
        s->ctx->surfaces->register_shm(buf->id, buf->width, buf->height, shm_format);
    }

    lx::compositor::surface_commit commit{};
    commit.surface = s->id;
    commit.buffer = buf->id;
    commit.client = s->client;
    commit.opaque = content_opaque(s);
    commit.opaque_rect_count = partial_opaque_rects(s, commit.opaque_rects);
    commit.surface_size = geo.size;
    commit.src = geo.src;
    commit.transform = geo.transform;
    // A new buffer with no damage request is treated as fully damaged: strictly the
    // client said nothing changed, but every real client that does this means "all of it".
    const lx::rect2i whole{0, 0, static_cast<int>(buf->width), static_cast<int>(buf->height)};
    commit.damage = damage.any ? damage.surface.bounds()
                               : lx::rect2i{0, 0, geo.size.width, geo.size.height};
    if (damage.any && damage.surface.count > 1)
        commit.damage_rects = damage.surface;
    client_damage buffer_damage = damage.buffer;
    if (!damage.any) {
        buffer_damage.clear();
        buffer_damage.add(whole);
    }

    s->current_buffer_id = buf->id;
    s->current_texture = s->ctx->surfaces->texture_for(buf->id);

    auto result = s->ctx->surfaces->on_commit(std::move(commit));
    if (!result) {
        // The buffer is unusable, so nothing will sample it. Release it immediately:
        // holding it back would starve the client of buffers and stall it forever.
        log_commit_failure(result.get_error());
        buffer_release_now(s->current.buffer);
        return;
    }

    const auto img = s->ctx->surfaces->lookup(s->id);

    const unsigned char* shm_pixels = nullptr;
    unsigned long long shm_token = 0;
    unsigned long long shm_pool_token = 0;
    unsigned shm_direct = 0;
    unsigned shm_stride = 0;
    lx::rect2i shm_region{};
    client_damage shm_rects{};
    if (buf->is_shm && img.image_id) {
        // Buffer age: only what changed since this buffer's texture was last written (plus
        // what its other GL name missed) is copied and uploaded — in buffer pixels, so a
        // scaled, transformed or cropped client keeps partial uploads too.
        s->damage_history.record_rects(buffer_damage);
        if (s->ctx->shm_pool && s->ctx->shm_regions) {
            shm_rects = shm_upload_damage(s->damage_history, buf->upload, s->id, buf->width,
                                          buf->height);
            shm_region = shm_rects.bounds();
        }
        // Read straight from the client's buffer by the render thread when the backend can
        // take its rows as they are; staged (a copy here) otherwise, or when the table is
        // full.
        shm_direct = direct_shm_upload(s->current.buffer, shm, buf, shm_region, shm_pixels,
                                       shm_stride);
        if (!shm_direct)
            shm_pixels = stage_shm_pixels(buf, shm, img.image_id, shm_region, shm_token,
                                          shm_pool_token, &s->damage_history, s->id);
        if (!shm_pixels) {
            // Nothing reached the texture, so the next upload of this buffer cannot assume
            // this one did (the store's slots carry their own stamps; the GL path counts).
            buf->upload = {};
            // The one silent path to a black screen: without staged pixels no texture
            // update is queued, so the texture is never registered, and the draw that
            // references it is skipped every frame with nothing explaining why. Staging
            // legitimately declines when every slot is in flight, so this is rate-limited
            // rather than suppressed — a client that is merely running ahead recovers,
            // one that never stages anything does not.
            static bool warned = false;
            if (!warned) {
                warned = true;
                log_surface.warn(
                    "shm staging returned no buffer — the texture cannot be registered and "
                    "the surface will not be drawn. Reported once.");
            }
        }
    }

    // Register SHM pixels with headless for software blit — only when headless is the
    // present path (the context carries it only then) and the pixels are the whole image.
    const bool whole_image = shm_region.width <= 0 || shm_region.height <= 0 ||
                             (static_cast<unsigned>(shm_region.width) == buf->width &&
                              static_cast<unsigned>(shm_region.height) == buf->height);
    if (buf->is_shm && s->ctx->headless && shm_pixels && img.image_id && whole_image) {
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
            update.shm_token = shm_token;
            update.shm_pool_token = shm_pool_token;
            update.format = shm_format;
            // A partial region is packed at its own width; the whole image at the buffer's.
            const bool partial = shm_region.width > 0 && shm_region.height > 0 &&
                                 (static_cast<unsigned>(shm_region.width) != buf->width ||
                                  static_cast<unsigned>(shm_region.height) != buf->height);
            update.region = partial ? shm_region : lx::rect2i{};
            // Several rects in the region: the upload copies only them.
            if (partial && shm_rects.count > 1)
                update.rects = shm_rects;
            update.stride = (partial ? static_cast<unsigned>(shm_region.width) : buf->width) * 4u;
            if (shm_direct) {
                update.shm_direct = shm_direct;
                update.stride = shm_stride;
            }
        }
        if (update.kind != texture_update::op::none && !s->ctx->textures->try_push(update)) {
            // Nothing will consume the FD now, so it has to be closed here or the
            // compositor leaks one per dropped update.
            if (update.dmabuf_fd >= 0)
                ::close(update.dmabuf_fd);
            if (update.shm_token && s->ctx->shm_store)
                s->ctx->shm_store->release(update.shm_token);
            if (update.shm_pool_token && s->ctx->shm_pool)
                s->ctx->shm_pool->release(update.shm_pool_token);
            if (update.shm_direct && s->ctx->shm_direct) {
                // Releases the buffer; `shm_direct` stays set so it is not released twice.
                s->ctx->shm_direct->drop(update.shm_direct);
            }
            // The texture never received this content, so the next upload of the buffer
            // cannot assume it: restage it whole.
            buf->upload = {};
            log_render.warn("texture update queue full — frame will lag");
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
    } else if (buf->is_shm && !shm_direct) {
        // Copied, so the client may have it back. A direct upload releases it once the
        // render thread has read it (`shm_direct_uploads::retire`).
        wl_buffer_send_release(s->current.buffer);
    }
}

/// cached → current for one surface: the body of a content update. The buffer is applied
/// before all other state (wl_surface.commit).
void apply_core(surface_obj* s) {
    lx::trace::scoped_span span{"wayland", "surface.apply"};
    surface_pending& c = s->cached;
    const bool new_buffer = c.buffer_set;
    if (new_buffer) {
        // The surface stops showing its previous content. If that buffer object is already
        // gone, nothing else will ever reference its texture — destroy_buffer deliberately
        // left it alone while it was still on screen.
        if (s->has_content && !s->current.buffer && s->current_texture)
            retire_texture(s->current_texture);
        if (!s->current.buffer || s->current.buffer != c.buffer) {
            s->current_texture = 0;
            // The dma-buf it showed is superseded: released once the frames that sample it
            // have completed (a re-commit of the same buffer keeps it current).
            if (s->ctx->lifecycle)
                s->ctx->lifecycle->on_detach(s->id);
        }
        s->current.buffer = c.buffer;
        s->has_content = c.buffer != nullptr;
    }
    // The attach/offset delta applies to this content update only (wl_surface.offset).
    s->current.dx = c.dx;
    s->current.dy = c.dy;
    s->current.scale = c.scale;
    s->current.transform = c.transform;
    s->current.viewport = c.viewport;
    // Damage is per update: it describes this change and nothing after it.
    const client_damage surface_dmg = c.damage_surface;
    const client_damage buffer_dmg = c.damage_buffer;
    const bool had_surface_damage = !surface_dmg.empty();
    const bool had_buffer_damage = !buffer_dmg.empty();
    while (c.frame_cbs.head)
        if (wl_resource* cb = cb_list_pop(c.frame_cbs))
            queue_frame_callback(cb, s->id);
    // The commit-path add-ons: the alpha multiplier and content type reach the scene, the
    // tearing hint the output policy; set_barrier raises the fifo barrier, dated with the
    // frame that first shows this update.
    if (c.alpha != s->alpha_current) {
        if (log_surface.enabled(lx::trace::level::debug)) {
            log_surface.debug("surface alpha surface={} multiplier={:.4f}", s->id.id(),
                              static_cast<double>(c.alpha) / 4294967295.0);
        }
        s->alpha_current = c.alpha;
        s->ctx->surfaces->set_alpha(s->id,
                                    static_cast<float>(static_cast<double>(c.alpha) / 4294967295.0));
    }
    if (c.content_type != s->content_type_current) {
        s->content_type_current = c.content_type;
        s->ctx->surfaces->set_content_hint(s->id, static_cast<lx::compositor::content_hint>(
                                                      c.content_type <= 3 ? c.content_type : 0));
        if (log_surface.enabled(lx::trace::level::debug)) {
            static const char* const k_types[] = {"none", "photo", "video", "game"};
            log_surface.debug("surface content type surface={} type={}", s->id.id(),
                              k_types[c.content_type <= 3 ? c.content_type : 0]);
        }
    }
    if (c.tearing != s->tearing_current && log_surface.enabled(lx::trace::level::debug)) {
        log_surface.debug("surface presentation hint surface={} hint={}", s->id.id(),
                          c.tearing == k_tearing_async ? "async" : "vsync");
    }
    s->tearing_current = c.tearing;
    if (c.fifo_set) {
        if (!s->fifo_barrier)
            ++g_barriers;
        s->fifo_barrier = true;
        s->fifo_barrier_frame = s->ctx->building_frame;
    }
    // The acquire point has signalled (the update was held until it did); the release point
    // moves to the buffer, signalled when the buffer lifecycle retires it.
    sync_point_drop(c.acquire);
    if (new_buffer && c.buffer && c.release.timeline) {
        if (buffer_obj* rb = find_buffer(c.buffer); rb && rb->is_dmabuf) {
            // A re-commit of the buffer: the newer point guards it now. On the same timeline
            // a later point signals the older one with it (timelines only move forward), so
            // the older is kept unsignalled until then; on another it is signalled now (one
            // slot per buffer — the spec lets a client reuse a buffer only once no commit
            // holds it, so the newer point still guards it).
            if (rb->release.timeline == c.release.timeline &&
                rb->release.point <= c.release.point)
                sync_point_drop(rb->release);
            else
                sync_point_signal_now(rb->release);
            rb->release = c.release;
            c.release = {};
        }
    }
    sync_point_signal_now(c.release);
    // Its presentation feedback is queued, dated by the snapshot that first shows it; the
    // update it replaces is discarded or superseded.
    presentation_apply(s);
    if (c.input_set) {
        s->input_current = c.input;
        s->ctx->surfaces->set_input_region(s->id, s->input_current);
    }
    const bool opaque_changed = c.opaque_set;
    if (c.opaque_set)
        s->opaque_current = c.opaque;
    const bool blur_changed = c.blur_set;
    if (c.blur_set)
        s->blur_current = c.blur;
    // wp_color_manager_v1: the description reaches the scene as the surface's transform
    // (validated when it was made, so it always has one).
    if (c.color_set && (c.color_has != s->color_has_current ||
                        (c.color_has && !(c.color == s->color_current)))) {
        s->color_has_current = c.color_has;
        s->color_current = c.color_has ? c.color : lx::gfx::color_description{};
        lx::gfx::color_transform t{};
        if (c.color_has && !lx::gfx::make_color_transform(s->color_current, t))
            t = {};
        s->ctx->surfaces->set_color(s->id, t);
        if (log_surface.enabled(lx::trace::level::debug)) {
            log_surface.debug("surface color surface={} description={} tf={} transform={} "
                              "peak={:.3f}",
                              s->id.id(), c.color_has ? 1 : 0,
                              static_cast<int>(s->color_current.tf), t.active ? 1 : 0,
                              static_cast<double>(t.peak));
        }
    }
    c = {};
    s->has_cached = false;

    // The new current state's geometry: logical size, source box, transform (`:viewport`).
    const lx::compositor::surface_geometry_in gin = current_geometry(s);
    const lx::compositor::surface_geometry geo = lx::compositor::compute_geometry(gin);
    const lx::size2i old_size = s->surface_size;
    s->surface_size = geo.size;
    if (log_surface.enabled(lx::trace::level::debug) &&
        (old_size.width != geo.size.width || old_size.height != geo.size.height ||
         s->logged_buffer.width != gin.buffer_w || s->logged_buffer.height != gin.buffer_h)) {
        s->logged_buffer = {gin.buffer_w, gin.buffer_h};
        // On a change only: what the client rendered against what it covers, which says
        // whether its draw is 1:1 at the output scale.
        const auto& vp = s->current.viewport;
        log_surface.debug("surface geometry surface={} buffer={}x{} scale={} transform={} "
                          "src={}{:.2f},{:.2f},{:.2f}x{:.2f} dst={}{}x{} size={}x{}",
                          s->id.id(), gin.buffer_w, gin.buffer_h, gin.scale,
                          static_cast<int>(gin.transform), vp.has_src ? "" : "-", vp.src_x / 256.0,
                          vp.src_y / 256.0, vp.src_w / 256.0, vp.src_h / 256.0,
                          vp.has_dst ? "" : "-", vp.dst_w, vp.dst_h, geo.size.width,
                          geo.size.height);
    }

    // The blur region is clipped to the surface: re-clipped when either changes.
    if (s->ctx->background_blur &&
        (blur_changed || ((old_size.width != geo.size.width || old_size.height != geo.size.height) &&
                          s->blur_current.count > 0))) {
        blur_rect_set rects{};
        const unsigned count = blur_rects(s, rects);
        s->ctx->surfaces->set_blur_region(s->id, {rects, count});
        if (log_surface.enabled(lx::trace::level::debug)) {
            long long area = 0;
            for (unsigned i = 0; i < count; ++i)
                area += static_cast<long long>(rects[i].width) * rects[i].height;
            log_surface.debug("surface blur region surface={} rects={} area={}", s->id.id(),
                              count, area);
        }
    }

    if (s->role == surface_role::cursor) {
        if (new_buffer)
            commit_cursor_surface(s);
        return;
    }

    // State-only update, or a null buffer: there is no new content to import. An update
    // without an attach does not re-read the old buffer — an shm buffer was copied and
    // released, and its storage now belongs to the client again. Its geometry may still
    // change (a new viewport, scale or transform on the content already shown).
    if (!new_buffer || !s->current.buffer) {
        if (s->has_content && !new_buffer) {
            s->ctx->surfaces->set_geometry(s->id, geo.size, geo.src, geo.transform);
            if (opaque_changed || old_size.width != geo.size.width ||
                old_size.height != geo.size.height) {
                opaque_rect_set rects{};
                const unsigned count = partial_opaque_rects(s, rects);
                s->ctx->surfaces->set_opaque(s->id, content_opaque(s), {rects, count});
            }
        }
        return;
    }

    // Each damage kind converted into the other's space (wlroots' surface_update_damage):
    // the scene repaints surface-local damage, the upload copies buffer damage.
    update_damage damage{};
    damage.any = had_surface_damage || had_buffer_damage;
    for (unsigned i = 0; i < surface_dmg.count; ++i) {
        const lx::rect2i& r = surface_dmg.rects[i];
        damage.surface.add(lx::compositor::clip_to(r, geo.size.width, geo.size.height));
        damage.buffer.add(lx::compositor::surface_to_buffer_damage(r, gin));
    }
    for (unsigned i = 0; i < buffer_dmg.count; ++i) {
        const lx::rect2i& r = buffer_dmg.rects[i];
        damage.surface.add(lx::compositor::buffer_to_surface_damage(r, gin));
        damage.buffer.add(lx::compositor::clip_to(r, gin.buffer_w, gin.buffer_h));
    }

    // A role-less surface may still become the cursor: wl_pointer.set_cursor may name it
    // after this commit, and then shows the content committed here. Cursor-sized shm
    // content is kept for that (read before the import below releases the buffer).
    if (s->role == surface_role::none && s->ctx->cursor)
        remember_cursor_candidate(s);
    if (!keeps_content(s)) {
        buffer_release_now(s->current.buffer);
        return;
    }
    import_current_buffer(s, damage, geo);
}

} // namespace

namespace {

// ── content-update queue (design-app-compat-capture §3) ─────────────────────

/// What a queued update carries besides its records: the wl_surface state, the role state
/// (`role_snapshot`) and the tree's cached child state, parallel to `content_queue`.
struct cu_payload {
    surface_pending state{};
    role_snapshot role{};
    lx::compositor::tree_child_state tree{};
    /// The surface's own wl_surface.commit made it (its role finisher runs when applied);
    /// false for one a desync transition made.
    bool root_commit = false;
    /// A sealed record already applied with its DCU.
    bool applied = false;
};

lx::compositor::content_queue g_cu{};
cu_payload g_cu_payload[lx::compositor::k_cu_pool + 1]{};
lx::compositor::readiness_poller g_poller{};
/// The DCU being applied (0: none): sealed children are applied from it.
unsigned g_applying_cu = 0;
/// DMA_BUF_IOCTL_EXPORT_SYNC_FILE is missing (older kernel): implicit readiness is not
/// waited for, the GPU waits implicitly in the import instead.
bool g_implicit_off = false;
bool g_overflow_warned = false;
/// Deadline hints on the implicit fences exported above (UI affinity; off after ENOTTY).
lx::gfx::fence_deadline g_fence_deadline{"client buffer"};

unsigned sealed_record_of(unsigned dcu, unsigned surface) {
    for (unsigned c = g_cu.at(dcu).first_child; c; c = g_cu.at(c).next_sibling)
        if (g_cu.at(c).surface == surface && !g_cu_payload[c].applied)
            return c;
    return 0;
}

const surface_pending* queued_tail_state(const surface_obj* s) {
    const unsigned t = g_cu.tail(surface_slot(s));
    return t ? &g_cu_payload[t].state : nullptr;
}

void apply_sealed(surface_obj* child, unsigned sealed, unsigned depth);

} // namespace

/// Applies `s`'s cached state, then — atomically with it, in the same UI call and so the
/// same snapshot — its children's positions and order and the cached state of every
/// effectively synchronized child, recursively. The only path from cached to current: a
/// queued update is swapped into `cached` and applied through here too.
void apply_cached(surface_obj* s, const char* cause, unsigned depth) {
    const unsigned seq = s->cached.commit_seq;
    apply_core(s);
    auto* tree = s->ctx->tree;
    if (!tree)
        return;
    const lx::size2i size = s->surface_size;
    tree->set_has_buffer(s->id, s->has_content && !s->surface_size.empty());
    if (s->role == surface_role::subsurface && log_surface.enabled(lx::trace::level::debug)) {
        log_surface.debug("subsurface apply surface={} parent={} cause={} buffer={} w={} h={}",
                          s->id.id(), tree->parent_of(s->id).id(), cause, s->has_content ? 1 : 0,
                          size.width, size.height);
    }
    tree->apply_child_state(s->id);
    lx::surface_id kids[lx::compositor::k_tree_max_children + 1];
    const unsigned n =
        tree->sync_children(s->id, kids, lx::compositor::k_tree_max_children + 1);
    for (unsigned i = 0; i < n && depth < lx::compositor::k_tree_max_depth; ++i) {
        surface_obj* child = surface_by_id(kids[i]);
        if (!child)
            continue;
        if (g_applying_cu) {
            // A queued update: the child's state committed with it was sealed into it; what
            // the child has cached now is newer and waits for the parent's next update.
            if (const unsigned sealed = sealed_record_of(g_applying_cu, surface_slot(child)))
                apply_sealed(child, sealed, depth + 1);
        } else if (child->has_cached) {
            apply_cached(child, "parent-commit", depth + 1);
        }
    }
    tree->mark_dirty(s->id);
    // text-input v2: a cursor rectangle staged for this surface applies with its content.
    text_input_surface_applied(s);
    // pointer-constraints: a staged region or cursor hint applies with it too, and the
    // effective region follows the new input region and size.
    pointer_constraints_surface_applied(s, seq);
}

namespace {

/// The role's part of a commit of a root surface, after its tree has been applied.
void finish_role(surface_obj* s) {
    if (s->role == surface_role::lock) {
        lock_surface_committed(s);
        return;
    }
    if (s->role == surface_role::input_popup) {
        // Shown or hidden, and placed at the text input's cursor, by the relay.
        input_popup_committed(s);
        return;
    }
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
        info.size = s->surface_size;
        info.has_buffer = s->has_content && !info.size.empty();
        s->ctx->layers->on_commit(s->id, info);
        return;
    }
    if (s->role == surface_role::popup && s->ctx->popups) {
        const auto* rec = s->ctx->popups->find(s->id);
        if (!rec)
            return;
        lx::compositor::popup_commit_info info{};
        const lx::size2i size = s->surface_size;
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
    info.surface_size = s->surface_size;
    info.has_buffer = s->has_content && !info.surface_size.empty();
    // After apply_cached, so this commit's synchronized subsurfaces are in it.
    if (s->ctx->tree)
        info.tree_bbox = s->ctx->tree->subtree_bbox(s->id);
    else if (info.has_buffer)
        info.tree_bbox = {0, 0, info.surface_size.width, info.surface_size.height};
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

// ── content-update queue: commit, apply, discard ─────────────────────────────

/// What the compositor knows about `s` now (its visibility and pace output's prediction).
lx::compositor::cu_conditions cu_conditions_of(const surface_obj* s) {
    lx::compositor::cu_conditions c{};
    const std::uint32_t shown = surface_shown_mask(s->id) & s->ctx->lit_mask;
    c.shown = shown != 0;
    c.barrier = s->fifo_barrier;
    if (c.shown && s->ctx->cu_predict) {
        const int pace = surface_pace_output(s->id);
        if (pace >= 0)
            c.predicted_ns = s->ctx->cu_predict(s->ctx->cu_user, static_cast<unsigned>(pace));
    }
    return c;
}

/// When the fences of an update of `root` are needed: the start of the repaint that would
/// latch it on its pace output (the dma-fence deadline hint, `cu_latch`); 0 — no hint — when
/// it is not shown, so a hidden window's GPU is not hurried; and none without a `root`. Worked
/// out on first use: an update with nothing pending never needs it.
struct fence_deadline_of {
    const surface_obj* root = nullptr;
    unsigned long long ns = ~0ull;

    unsigned long long get() {
        if (ns != ~0ull)
            return ns;
        ns = 0;
        if (root && root->ctx->cu_latch &&
            (surface_shown_mask(root->id) & root->ctx->lit_mask)) {
            const int pace = surface_pace_output(root->id);
            if (pace >= 0)
                ns = root->ctx->cu_latch(root->ctx->cu_user, static_cast<unsigned>(pace));
        }
        return ns;
    }
};

void log_implicit_off(int err) {
    if (!log_surface.enabled(lx::trace::level::debug))
        return;
    log_surface.debug(
        "dma-buf EXPORT_SYNC_FILE unavailable (errno {}): implicit fences are waited for "
        "by the GPU import, not before applying",
        err);
}

/// An explicit-sync acquire point that has not signalled yet: an eventfd armed with
/// DRM_IOCTL_SYNCOBJ_EVENTFD (-1: it has signalled, or the query failed — nothing better to
/// wait on). `lost`: it has not signalled and no eventfd could be armed. The query carries
/// the deadline hint.
int acquire_wait_fd(const sync_point& p, bool& lost, fence_deadline_of& deadline) {
#if defined(__linux__)
    lx::gfx::drm_timeline_table* tl = syncobj_timelines();
    if (!p.timeline || !tl)
        return -1;
    auto done = tl->signalled(p.timeline, p.point, deadline.get());
    if (!done || done.value())
        return -1;
    const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd >= 0 && tl->notify(p.timeline, p.point, efd))
        return efd;
    if (efd >= 0)
        ::close(efd);
    lost = true;
#else
    (void)p;
    (void)lost;
    (void)deadline;
#endif
    return -1;
}

/// The fence waits `st`'s newly attached buffer needs before it may be sampled, as fds the
/// poller can watch (appended to `fds`, at most `cap`; `acquire[i]` marks an explicit-sync
/// one). Explicit sync: the acquire point — queried first, an eventfd from
/// DRM_IOCTL_SYNCOBJ_EVENTFD only when it has not signalled; one that cannot be watched
/// (no room left in `fds`, no eventfd) sets `lost` — the update must then not apply until
/// it is found signalled. Otherwise, for a dma-buf: its write fences — one poll() per
/// distinct plane (the common, already-signalled case), a sync_file snapshot
/// (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) of those still pending, deadline-hinted; one that does
/// not fit is waited for by the GPU import instead.
unsigned fence_waits(const surface_pending& st, int* fds, bool* acquire, unsigned cap,
                     bool& lost, fence_deadline_of& deadline) {
    if (!st.buffer_set || !st.buffer)
        return 0;
    unsigned n = 0;
#if defined(__linux__)
    if (st.acquire.timeline) {
        if (cap == 0) {
            bool unused = false;
            const int efd = acquire_wait_fd(st.acquire, unused, deadline);
            if (efd >= 0 || unused) {
                if (efd >= 0)
                    ::close(efd);
                lost = true;
            }
            return 0;
        }
        const int efd = acquire_wait_fd(st.acquire, lost, deadline);
        if (efd < 0)
            return 0;
        fds[n] = efd;
        acquire[n] = true;
        return ++n;
    }
    if (g_implicit_off || cap == 0)
        return 0;
    const buffer_obj* buf = find_buffer(st.buffer);
    if (!buf || !buf->is_dmabuf)
        return 0;
    int seen[4] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < buf->dmabuf.plane_count && i < 4 && n < cap; ++i) {
        const int fd = buf->dmabuf.planes[i].fd.get();
        if (fd < 0)
            continue;
        bool dup = false;
        for (unsigned j = 0; j < i; ++j)
            dup = dup || seen[j] == fd;
        seen[i] = fd;
        if (dup)
            continue;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN))
            continue; // every write fence has signalled
        dma_buf_export_sync_file req{};
        req.flags = DMA_BUF_SYNC_READ;
        req.fd = -1;
        if (::ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) != 0 || req.fd < 0) {
            if (errno == ENOTTY || errno == EINVAL) {
                g_implicit_off = true;
                log_implicit_off(errno);
            }
            return n;
        }
        (void)g_fence_deadline.hint(req.fd, deadline.get());
        fds[n] = req.fd;
        acquire[n] = false;
        ++n;
    }
#else
    (void)st;
    (void)fds;
    (void)acquire;
    (void)cap;
    (void)lost;
    (void)deadline;
#endif
    return n;
}

/// Some queued update has `recheck` set (cleared by a scan that finds none).
bool g_recheck_any = false;

/// Hands `nfd` fence fds to the poller as waits of record `cu`. An explicit-sync one it
/// cannot take, or one already `lost`, sets the record's `recheck`: the update then waits
/// until the point is found signalled (`cu_recheck`) — never applied on an unsignalled
/// acquire point. An implicit fence it cannot take is dropped (the GPU import waits for it).
void arm_waits(unsigned cu, const int* fds, const bool* acquire, unsigned nfd, bool lost) {
    lx::compositor::cu_record& rec = g_cu.at(cu);
    for (unsigned i = 0; i < nfd; ++i) {
        if (g_poller.add(fds[i], cu, rec.generation))
            ++rec.waits;
        else if (acquire[i])
            lost = true;
    }
    if (lost) {
        rec.recheck = true;
        g_recheck_any = true;
    }
}

/// The unsignalled explicit-sync acquire points of DCU `root` and the records sealed into
/// it, into `out` (up to `cap`); their count. Query failures count as signalled.
unsigned unsignalled_acquires(unsigned root, sync_point* out, unsigned cap) {
    lx::gfx::drm_timeline_table* tl = syncobj_timelines();
    if (!tl)
        return 0;
    unsigned n = 0;
    const auto check = [&](const surface_pending& st) {
        if (!st.buffer_set || !st.buffer || !st.acquire.timeline || n >= cap)
            return;
        auto done = tl->signalled(st.acquire.timeline, st.acquire.point);
        if (done && !done.value())
            out[n++] = st.acquire;
    };
    check(g_cu_payload[root].state);
    for (unsigned c = g_cu.at(root).first_child; c; c = g_cu.at(c).next_sibling)
        check(g_cu_payload[c].state);
    return n;
}

/// Queued updates whose acquire points had no poller entry: each is queried again (a tick
/// of a lit output runs this while one exists, `cu_waiting_outputs`); found signalled, the
/// update is free of it; still pending, eventfds are armed once the poller has room for
/// all of them.
void cu_recheck() {
    if (!g_recheck_any)
        return;
    bool still = false;
    for (unsigned i = 1; i <= lx::compositor::k_cu_pool; ++i) {
        lx::compositor::cu_record& rec = g_cu.at(i);
        if (!rec.used || !rec.recheck)
            continue;
        sync_point points[lx::compositor::k_cu_per_surface];
        const unsigned n = unsignalled_acquires(i, points, lx::compositor::k_cu_per_surface);
        if (n == 0) {
            rec.recheck = false;
            continue;
        }
        bool armed = g_poller.available() >= n;
        for (unsigned k = 0; armed && k < n; ++k) {
            bool lost = false;
            fence_deadline_of hinted_at_commit{}; // no second hint
            const int efd = acquire_wait_fd(points[k], lost, hinted_at_commit);
            if (efd < 0) {
                armed = !lost; // signalled meanwhile
                continue;
            }
            if (g_poller.add(efd, i, rec.generation))
                ++rec.waits;
            else
                armed = false;
        }
        if (armed)
            rec.recheck = false;
        else
            still = true;
    }
    g_recheck_any = still;
}

/// The effectively synchronized descendants of `s` with cached state, in tree preorder:
/// what a DCU of `s` applies with it.
unsigned sync_descendants(surface_obj* s, surface_obj** out, unsigned cap) {
    auto* tree = s->ctx->tree;
    if (!tree)
        return 0;
    unsigned count = 0;
    tree->for_each_preorder(s->id, [&](lx::surface_id id) {
        if (id == s->id)
            return true;
        if (!tree->effectively_sync(id))
            return false; // a desynchronized subtree applies its own commits
        surface_obj* n = surface_by_id(id);
        if (n && n->has_cached && count < cap)
            out[count++] = n;
        return true;
    });
    return count;
}

void close_fds(const int* fds, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        if (fds[i] >= 0)
            ::close(fds[i]);
}

void warn_overflow() {
    if (g_overflow_warned)
        return;
    g_overflow_warned = true;
    log_surface.warn(
        "content-update queue full (a client far ahead of the display): its newest updates are "
        "merged, superseding frames rather than showing any early. Reported once.");
}

/// `newer`'s role state over `older`'s (two queued updates merged).
void merge_role(role_snapshot& older, const role_snapshot& newer) {
    if (newer.min_set) {
        older.min_set = true;
        older.min = newer.min;
    }
    if (newer.max_set) {
        older.max_set = true;
        older.max = newer.max;
    }
    if (newer.wg_set) {
        older.wg_set = true;
        older.wg = newer.wg;
    }
    if (newer.acked_unapplied) {
        older.acked_unapplied = true;
        older.acked = newer.acked;
    }
    if (newer.layer_set) {
        older.layer_set = true;
        older.layer = newer.layer;
    }
}

/// `newer`'s cached child state over `older`'s.
void merge_tree(lx::compositor::tree_child_state& older,
                const lx::compositor::tree_child_state& newer) {
    for (unsigned i = 0; i < newer.count; ++i) {
        unsigned j = 0;
        while (j < older.count && !(older.ids[j] == newer.ids[i]))
            ++j;
        if (j == older.count) {
            if (older.count >= lx::compositor::k_tree_max_children)
                continue;
            older.ids[older.count++] = newer.ids[i];
        }
        older.pos[j] = newer.pos[i];
    }
    if (newer.order_set) {
        older.order_set = true;
        older.order = newer.order;
    }
}

/// The queue of `s` is full: this commit merges into its newest queued update (Q5), which
/// then carries the newer content and waits for both updates' blockers. Order is kept and
/// nothing shows early; the older update's content is superseded (its feedback discarded).
void merge_into_tail(surface_obj* s, bool root_commit, surface_obj** kids, unsigned nk,
                     const int* fds, const bool* acquire, unsigned nfd, bool lost) {
    const unsigned tail = g_cu.tail(surface_slot(s));
    lx::compositor::cu_record& rec = g_cu.at(tail);
    cu_payload& pl = g_cu_payload[tail];
    if (s->cached.target_ns > rec.target_ns)
        rec.target_ns = s->cached.target_ns;
    merge_state(s, s->cached, pl.state);
    s->cached = {};
    s->has_cached = false;
    if (root_commit) {
        role_snapshot newer{};
        take_role_snapshot(s, newer);
        if (pl.root_commit)
            merge_role(pl.role, newer);
        else
            pl.role = newer;
        pl.root_commit = true;
    }
    auto* tree = s->ctx->tree;
    if (tree) {
        lx::compositor::tree_child_state newer{};
        tree->take_cached_child_state(s->id, newer);
        merge_tree(pl.tree, newer);
    }
    for (unsigned k = 0; k < nk; ++k) {
        surface_obj* kid = kids[k];
        unsigned c = sealed_record_of(tail, surface_slot(kid));
        if (!c) {
            c = g_cu.alloc(surface_slot(kid), s->client.id());
            if (!c)
                continue; // no record left: its state stays cached for the next commit
            g_cu_payload[c] = {};
            g_cu.seal(tail, c);
        }
        cu_payload& cp = g_cu_payload[c];
        merge_state(kid, kid->cached, cp.state);
        kid->cached = {};
        kid->has_cached = false;
        if (tree) {
            lx::compositor::tree_child_state newer{};
            tree->take_cached_child_state(kid->id, newer);
            merge_tree(cp.tree, newer);
        }
    }
    arm_waits(tail, fds, acquire, nfd, lost);
}

/// A commit of `s` (not effectively synchronized): its cached state, with its synchronized
/// descendants' sealed in, becomes a DCU. Applied now when the surface's queue is empty and
/// nothing blocks it (the fast path, today's cost); otherwise queued. True when applied —
/// the caller then finishes the commit (role, scene).
bool commit_dcu(surface_obj* s, bool root_commit, const char* cause) {
    const unsigned slot = surface_slot(s);
    surface_obj* kids[64];
    const unsigned nk = sync_descendants(s, kids, 64);
    int fds[16];
    bool acquire[16];
    bool lost = false;
    fence_deadline_of deadline{s};
    unsigned nfd = fence_waits(s->cached, fds, acquire, 16, lost, deadline);
    for (unsigned k = 0; k < nk; ++k)
        nfd += fence_waits(kids[k]->cached, fds + nfd, acquire + nfd, 16 - nfd, lost, deadline);
    // An unsignalled explicit-sync acquire point among them: the update cannot apply before
    // it signals, whatever else happens.
    bool explicit_wait = lost;
    for (unsigned i = 0; i < nfd; ++i)
        explicit_wait = explicit_wait || acquire[i];
    lx::compositor::cu_record probe{};
    probe.fifo_wait = s->cached.fifo_wait;
    probe.target_ns = s->cached.target_ns;
    const lx::compositor::cu_conditions cond = cu_conditions_of(s);
    const bool blocked = g_cu.depth(slot) > 0 || nfd > 0 || lost ||
                         !lx::compositor::cu_ready(probe, true, cond);
    if (blocked) {
        const unsigned need = 1 + nk;
        const unsigned client = s->client.id();
        const auto room = [&] {
            return g_cu.depth(slot) < lx::compositor::k_cu_per_surface &&
                   lx::compositor::cu_room_for(g_cu.free_count(), g_cu.client_count(client),
                                               need);
        };
        if (!room() && g_cu.depth(slot) > 0) {
            warn_overflow();
            merge_into_tail(s, root_commit, kids, nk, fds, acquire, nfd, lost);
            return false;
        }
        if (room()) {
            const unsigned root = g_cu.alloc(slot, client);
            lx::compositor::cu_record& rec = g_cu.at(root);
            rec.fifo_wait = probe.fifo_wait;
            rec.target_ns = probe.target_ns;
            cu_payload& pl = g_cu_payload[root];
            pl.state = s->cached;
            s->cached = {};
            s->has_cached = false;
            pl.root_commit = root_commit;
            pl.applied = false;
            if (root_commit)
                take_role_snapshot(s, pl.role);
            else
                pl.role = {};
            if (auto* tree = s->ctx->tree)
                tree->take_cached_child_state(s->id, pl.tree);
            for (unsigned k = 0; k < nk; ++k) {
                const unsigned c = g_cu.alloc(surface_slot(kids[k]), client);
                cu_payload& cp = g_cu_payload[c];
                cp.state = kids[k]->cached;
                kids[k]->cached = {};
                kids[k]->has_cached = false;
                cp.root_commit = false;
                cp.applied = false;
                cp.role = {};
                if (auto* tree = s->ctx->tree)
                    tree->take_cached_child_state(kids[k]->id, cp.tree);
                g_cu.seal(root, c);
            }
            arm_waits(root, fds, acquire, nfd, lost);
            g_cu.push(root);
            // A target time on a shown surface: its output's clock is armed for the tick
            // that presents at or after it (an idle desktop wakes only for real deadlines).
            if (rec.target_ns && cond.shown && s->ctx->cu_request_at) {
                const int pace = surface_pace_output(s->id);
                if (pace >= 0)
                    s->ctx->cu_request_at(s->ctx->cu_user, static_cast<unsigned>(pace),
                                          rec.target_ns);
            }
            if (log_surface.enabled(lx::trace::level::debug)) {
                log_surface.debug(
                    "content update queued surface={} cause={} depth={} waits={} fifo={} "
                    "barrier={} target={} sealed={}",
                    s->id.id(), cause, g_cu.depth(slot), rec.waits, rec.fifo_wait ? 1 : 0,
                    s->fifo_barrier ? 1 : 0, rec.target_ns, nk);
            }
            return false;
        }
        // No room and nothing of this surface queued to merge into (the pool or the client cap
        // is spent on its other surfaces).
        close_fds(fds, nfd);
        if (explicit_wait) {
            // Applying it would sample a buffer the client's GPU may still be writing, and
            // there is nowhere for it to wait: the client is past its share of the pool (the
            // reserve keeps room for any client that queues little), so it is the one that
            // goes — wlroots and Mutter answer an allocation failure the same way.
            log_surface.warn(
                "content-update queue full for an update waiting on its acquire point: "
                "client disconnected (no_memory)");
            wl_client_post_no_memory(wl_resource_get_client(s->resource));
            return false;
        }
        // Nothing waits on the client's GPU: this update applies now (fifo and commit-timing
        // are skipped, implicit fences are waited for by the GPU import).
        warn_overflow();
    }
    if (root_commit)
        apply_role_pending(s);
    apply_cached(s, cause, 0);
    return true;
}

/// A sealed child's state, applied with its DCU in place of what the child caches now.
void apply_sealed(surface_obj* child, unsigned sealed, unsigned depth) {
    cu_payload& pl = g_cu_payload[sealed];
    auto* tree = child->ctx->tree;
    const surface_pending stash = child->cached;
    const bool stash_has = child->has_cached;
    child->cached = pl.state;
    child->has_cached = true;
    pl.state = {};
    pl.applied = true;
    lx::compositor::tree_child_state tstash{};
    if (tree) {
        tree->take_cached_child_state(child->id, tstash);
        tree->put_cached_child_state(child->id, pl.tree);
    }
    apply_cached(child, "parent-queued", depth);
    if (tree)
        tree->put_cached_child_state(child->id, tstash);
    child->cached = stash;
    child->has_cached = stash_has;
}

/// Applies a queued DCU (already popped) and frees it: its state is swapped into the
/// surface's `cached` (setting aside whatever the surface caches now), with its role state
/// and the tree's child state, then applied through `apply_cached` like any commit.
void apply_cu(unsigned root) {
    const lx::compositor::cu_record rec = g_cu.at(root);
    surface_obj* s = &surfaces[rec.surface - 1];
    cu_payload& pl = g_cu_payload[root];
    if (s->used) {
        auto* tree = s->ctx->tree;
        const surface_pending stash = s->cached;
        const bool stash_has = s->has_cached;
        s->cached = pl.state;
        s->has_cached = true;
        pl.state = {};
        lx::compositor::tree_child_state tstash{};
        if (tree) {
            tree->take_cached_child_state(s->id, tstash);
            tree->put_cached_child_state(s->id, pl.tree);
        }
        role_stash rstash{};
        if (pl.root_commit)
            apply_role_snapshot(s, pl.role, rstash);
        const unsigned outer = g_applying_cu;
        g_applying_cu = root;
        apply_cached(s, "queued", 0);
        g_applying_cu = outer;
        if (tree)
            tree->put_cached_child_state(s->id, tstash);
        s->cached = stash;
        s->has_cached = stash_has;
        finish_commit(s, pl.root_commit, "queued");
        if (pl.root_commit)
            restore_role_stash(s, rstash);
    }
    for (unsigned c = rec.first_child; c;) {
        const unsigned next = g_cu.at(c).next_sibling;
        cu_payload& cp = g_cu_payload[c];
        // Not applied with it (the child stopped being synchronized meanwhile): dropped
        // like an update of a destroyed parent.
        if (!cp.applied) {
            surface_obj* owner = &surfaces[g_cu.at(c).surface - 1];
            if (owner->used)
                drop_state(owner, cp.state, false);
        }
        cp = {};
        g_cu.release(c);
        c = next;
    }
    (void)g_poller.drop(root);
    pl = {};
    g_cu.release(root);
}

/// Drops a queued DCU and its sealed records unapplied.
void discard_cu(unsigned root, bool dying) {
    const lx::compositor::cu_record rec = g_cu.at(root);
    for (unsigned c = rec.first_child; c;) {
        const unsigned next = g_cu.at(c).next_sibling;
        surface_obj* owner = &surfaces[g_cu.at(c).surface - 1];
        if (owner->used)
            drop_state(owner, g_cu_payload[c].state, dying);
        g_cu_payload[c] = {};
        g_cu.release(c);
        c = next;
    }
    surface_obj* s = &surfaces[rec.surface - 1];
    if (s->used)
        drop_state(s, g_cu_payload[root].state, dying);
    (void)g_poller.drop(root);
    g_cu_payload[root] = {};
    g_cu.release(root);
}

/// Applies `slot`'s queued updates from the head while they are ready.
void cu_try_surface(unsigned slot) {
    for (unsigned guard = 0; guard <= lx::compositor::k_cu_per_surface; ++guard) {
        const unsigned head = g_cu.head(slot);
        if (!head)
            return;
        const surface_obj* s = &surfaces[slot - 1];
        if (!lx::compositor::cu_ready(g_cu.at(head), g_cu.fences_clear(head),
                                      cu_conditions_of(s)))
            return;
        (void)g_cu.pop(slot);
        apply_cu(head);
    }
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
        // A cached SCU of a surface that is no longer synchronized becomes a DCU: applied
        // now, or queued behind its fences like any other.
        if (n && n->has_cached && !tree->effectively_sync(id))
            (void)commit_dcu(n, false, cause);
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

    // Role rules that reject the whole commit come first, before any state moves; then
    // the geometry the commit would produce (D5: checked here, at the client's commit, as
    // wlroots does — the values are fixed by now).
    if (!role_prechecks(s) || !geometry_prechecks(s) || !viewport_precheck(s) ||
        !syncobj_precheck(s))
        return;
    // xdg-toplevel-icon-v1: a staged icon is "applied on the next wl_surface.commit of the
    // toplevel". It only reaches the shell's panel, so it goes now rather than riding a queued
    // content update.
    if (s->role == surface_role::xdg_toplevel)
        toplevel_icon_commit(s);

    // The commit's number travels with its content update; state other objects staged for
    // this commit (a pointer constraint's region) is keyed by it.
    s->pending.commit_seq = ++s->commit_seq ? s->commit_seq : ++s->commit_seq;
    pointer_constraints_surface_committed(s, s->pending.commit_seq);
    merge_pending_into_cached(s);
    auto* tree = s->ctx->tree;
    if (tree)
        tree->cache_child_state(s->id);

    const bool is_sub = s->role == surface_role::subsurface;
    if (tree && tree->effectively_sync(s->id)) {
        // A synchronized content update: it waits for the parent's state to be applied.
        // fifo-v1: "The constraint must be ignored if the surface is a subsurface in
        // synchronized mode."
        s->cached.fifo_wait = false;
        if (log_surface.enabled(lx::trace::level::debug)) {
            log_surface.debug("subsurface commit surface={} mode=sync cached=1", s->id.id());
        }
        return;
    }
    if (is_sub && log_surface.enabled(lx::trace::level::debug)) {
        log_surface.debug("subsurface commit surface={} mode=desync cached=0", s->id.id());
    }
    if (commit_dcu(s, true, is_sub ? "desync-commit" : "commit"))
        finish_commit(s, true, "commit");
    // A constraint region or cursor hint that applied with it takes effect now.
    pointer_constraints_flush();
}

void surface_set_buffer_transform(struct wl_client*, struct wl_resource* resource,
                                  int32_t transform) {
    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "buffer transform is not a wl_output.transform value");
        return;
    }
    if (auto* s = surface_from(resource))
        s->pending.transform = transform;
}

void surface_set_buffer_scale(struct wl_client*, struct wl_resource* resource, int32_t scale) {
    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
                               "buffer scale is not positive");
        return;
    }
    if (auto* s = surface_from(resource))
        s->pending.scale = scale;
}

/// wl_surface.damage_buffer (v4): buffer pixels.
void surface_damage_buffer(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                           int32_t width, int32_t height) {
    if (auto* s = surface_from(resource))
        accumulate_damage(s->pending.damage_buffer, x, y, width, height);
}

void surface_offset(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y) {
    if (auto* s = surface_from(resource)) {
        s->pending.dx = x;
        s->pending.dy = y;
    }
}

/// A role object (wl_subsurface, xdg_toplevel, xdg_popup, zwlr_layer_surface_v1,
/// ext_session_lock_surface_v1, zwp_input_popup_surface_v2) still alive for this surface.
[[nodiscard]] bool has_live_role_object(const surface_obj* s) {
    return s->sub != nullptr || s->layer != nullptr || s->lock_surface != nullptr ||
           s->input_popup != nullptr ||
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
    s->blur_current.reset_empty();   // ext-background-effect-v1: "the initial value is empty"
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

bool surface_visible(lx::surface_id surface, unsigned frame, unsigned slot) {
    if (!g_p0_ctx || !g_p0_ctx->surfaces)
        return false;
    const lx::scene::surface_node* n = g_p0_ctx->surfaces->node(surface);
    return n && n->visible_in(frame, slot);
}

std::uint32_t surface_shown_mask(lx::surface_id surface) {
    if (!g_p0_ctx || !g_p0_ctx->surfaces)
        return 0;
    const lx::scene::surface_node* n = g_p0_ctx->surfaces->node(surface);
    if (!n)
        return 0;
    std::uint32_t mask = 0;
    for (unsigned v = 0; v < 16; ++v)
        if ((g_p0_ctx->published_mask & (1u << v)) && n->visible_in(g_p0_ctx->published_frame[v], v))
            mask |= 1u << v;
    return mask;
}

int surface_pace_output(lx::surface_id surface) {
    const std::uint32_t shown = surface_shown_mask(surface);
    if (shown == 0 || !g_p0_ctx || !g_p0_ctx->outputs)
        return -1;
    if ((shown & (shown - 1)) == 0) {
        // Shown on one output: that one (the common case, no rect needed).
        for (unsigned v = 0; v < 16; ++v)
            if (shown & (1u << v))
                return static_cast<int>(v);
    }
    const lx::rect2i rect = g_p0_ctx->tree ? g_p0_ctx->tree->node_rect(surface) : lx::rect2i{};
    return lx::compositor::pace_output(*g_p0_ctx->outputs, shown, rect);
}

void fire_frames(unsigned frame_time_ms, bool throttle, unsigned slot, unsigned long long now_ns) {
    if (g_pending_frame_count == 0)
        return;
    if (now_ns == 0) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ns = static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
                 static_cast<unsigned long long>(ts.tv_nsec);
    }
    unsigned kept = 0;
    unsigned throttled = 0;
    const unsigned count = g_pending_frame_count;
    for (unsigned i = 0; i < count; ++i) {
        const pending_frame e = g_pending_frames[i];
        // Clear the slot before destroying: wl_resource_destroy runs the destructor, which
        // scans this same table.
        g_pending_frames[i] = {};
        if (!e.resource)
            continue;
        surface_obj* s = surface_by_id(e.surface);
        bool answer = !throttle || !s || s->role == surface_role::cursor;
        bool paced_elsewhere = false;
        if (!answer) {
            // Paced by one output (M8): answered by that output's flip only; a surface on no
            // output falls to the throttle below.
            const int pace = surface_pace_output(e.surface);
            if (pace >= 0) {
                answer = slot == k_any_output || static_cast<unsigned>(pace) == slot;
                paced_elsewhere = !answer;
            }
        }
        // A window being captured counts as shown (CC2): a screencast of a covered window
        // keeps its frames coming at the capturing output's pace, not the throttle's.
        if (!answer && !paced_elsewhere && capture_paces_surface(e.surface))
            answer = true;
        // Not on screen: answered once its surface's throttle interval has passed. A
        // surface answered earlier in this same pass counts as due (`frame_done_ns == now`),
        // so all of one surface's callbacks go together.
        if (!answer && !paced_elsewhere)
            answer = s->frame_done_ns == 0 || s->frame_done_ns == now_ns ||
                     now_ns - s->frame_done_ns >= k_frame_throttle_ns;
        if (!answer) {
            pending_frame& k = g_pending_frames[kept++];
            k = e;
            k.throttled = !paced_elsewhere;
            if (k.throttled)
                ++throttled;
            continue;
        }
        if (s)
            s->frame_done_ns = now_ns;
        wl_callback_send_done(e.resource, frame_time_ms);
        wl_resource_destroy(e.resource);
    }
    g_pending_frame_count = kept;
    g_throttled_frame_count = throttled;
}

bool frames_pending() { return g_pending_frame_count > g_throttled_frame_count; }

std::uint32_t frames_pending_outputs() {
    std::uint32_t mask = 0;
    for (unsigned i = 0; i < g_pending_frame_count; ++i) {
        const pending_frame& e = g_pending_frames[i];
        if (!e.resource || e.throttled)
            continue;
        const surface_obj* s = surface_by_id(e.surface);
        const int pace = s && s->role != surface_role::cursor ? surface_pace_output(e.surface) : -1;
        if (pace < 0)
            return ~0u; // answered by whichever output ticks first
        mask |= 1u << static_cast<unsigned>(pace);
    }
    return mask;
}

bool frames_throttled() { return g_throttled_frame_count > 0; }

// ── content-update queue: entry points ───────────────────────────────────────

void drop_state(surface_obj* s, surface_pending& st, bool dying) {
    if (dying)
        cb_list_forget(st.frame_cbs);
    else
        frame_cbs_fire_now(st.frame_cbs);
    presentation_drop_list(st.feedback);
    sync_point_drop(st.acquire);
    if (dying)
        sync_point_drop(st.release);
    else
        sync_point_signal_now(st.release);
    // Same rule as a superseded cached buffer: not the dma-buf on screen (the lifecycle
    // releases that one), and an shm buffer is owed a release of its own.
    if (!dying && st.buffer_set && st.buffer &&
        (st.buffer != s->current.buffer || wl_shm_buffer_get(st.buffer)))
        wl_buffer_send_release(st.buffer);
    st = {};
}

lx::compositor::content_queue& cu_queue() { return g_cu; }

int cu_poll_fd() {
    if (g_poller.fd() < 0) {
        if (auto opened = g_poller.open(); !opened)
            log_surface.error(opened.get_error());
    }
    return g_poller.fd();
}

void cu_try_all(const char* cause) {
    (void)cause;
    if (g_cu.used_count() == 0)
        return;
    cu_recheck();
    unsigned slots[lx::compositor::k_cu_surfaces];
    const unsigned n = g_cu.queued_surfaces(slots, lx::compositor::k_cu_surfaces);
    for (unsigned i = 0; i < n; ++i)
        cu_try_surface(slots[i]);
}

void cu_poll() {
    using wake = lx::compositor::readiness_poller::wake;
    wake wakes[64];
    bool any = false;
    for (;;) {
        const unsigned n = g_poller.drain(wakes, 64);
        for (unsigned i = 0; i < n; ++i) {
            if (wakes[i].cu >= k_poll_foreign_base) {
                // Another unit's wait (a screen capture's copy fence).
                capture_fence_signalled(wakes[i].cu, wakes[i].generation);
                continue;
            }
            lx::compositor::cu_record& r = g_cu.at(wakes[i].cu);
            if (r.used && r.generation == wakes[i].generation && r.waits > 0) {
                --r.waits;
                any = true;
            }
        }
        if (n < 64)
            break;
    }
    // Entries freed: an update whose acquire point found none may arm one now.
    if (any || g_recheck_any)
        cu_try_all("fence");
}

bool cu_poll_add(int fd, unsigned key, unsigned generation) {
    if (key < k_poll_foreign_base || cu_poll_fd() < 0) {
        if (fd >= 0)
            ::close(fd);
        return false;
    }
    // Other units' waits may use the reserve content updates cannot take.
    return g_poller.add(fd, key, generation, true);
}

void cu_poll_drop(unsigned key) {
    if (key >= k_poll_foreign_base)
        (void)g_poller.drop(key);
}

void cu_after_latch(unsigned slot, unsigned frame) {
    if (g_barriers > 0 && g_p0_ctx) {
        for (auto& s : surfaces) {
            if (!s.used || !s.fifo_barrier)
                continue;
            // "Cleared immediately after the following latching deadline": the frame that
            // first showed the update with set_barrier has latched on the surface's pace
            // output. A surface not on screen does not hold its barrier.
            const bool shown = (surface_shown_mask(s.id) & g_p0_ctx->lit_mask) != 0;
            const int pace = shown ? surface_pace_output(s.id) : -1;
            if (pace >= 0 && (static_cast<unsigned>(pace) != slot ||
                              static_cast<int>(frame - s.fifo_barrier_frame) < 0))
                continue;
            s.fifo_barrier = false;
            --g_barriers;
        }
    }
    cu_try_all("latch");
}

std::uint32_t cu_waiting_outputs() {
    if (g_cu.used_count() == 0 || !g_p0_ctx)
        return 0;
    unsigned slots[lx::compositor::k_cu_surfaces];
    const unsigned n = g_cu.queued_surfaces(slots, lx::compositor::k_cu_surfaces);
    std::uint32_t mask = 0;
    for (unsigned i = 0; i < n; ++i) {
        const surface_obj& s = surfaces[slots[i] - 1];
        const lx::compositor::cu_record& r = g_cu.at(g_cu.head(slots[i]));
        if (g_recheck_any && !g_cu.fences_clear(g_cu.head(slots[i]))) {
            // An acquire point only a query notices: its pace output ticks (any lit one
            // when it is shown on none) until the point signals or an eventfd is armed.
            bool recheck = r.recheck;
            for (unsigned c = r.first_child; c && !recheck; c = g_cu.at(c).next_sibling)
                recheck = g_cu.at(c).recheck;
            if (recheck) {
                const int pace = (surface_shown_mask(s.id) & g_p0_ctx->lit_mask) != 0
                                     ? surface_pace_output(s.id)
                                     : -1;
                if (pace >= 0)
                    mask |= 1u << static_cast<unsigned>(pace);
                else if (g_p0_ctx->lit_mask)
                    mask |= g_p0_ctx->lit_mask & (~g_p0_ctx->lit_mask + 1u);
                continue;
            }
        }
        if (!r.fifo_wait || !s.fifo_barrier)
            continue;
        const int pace = (surface_shown_mask(s.id) & g_p0_ctx->lit_mask) != 0
                             ? surface_pace_output(s.id)
                             : -1;
        if (pace >= 0)
            mask |= 1u << static_cast<unsigned>(pace);
    }
    return mask;
}

unsigned long long cu_earliest_target(unsigned slot) {
    if (g_cu.used_count() == 0 || !g_p0_ctx)
        return 0;
    unsigned slots[lx::compositor::k_cu_surfaces];
    const unsigned n = g_cu.queued_surfaces(slots, lx::compositor::k_cu_surfaces);
    unsigned long long best = 0;
    for (unsigned i = 0; i < n; ++i) {
        const lx::compositor::cu_record& r = g_cu.at(g_cu.head(slots[i]));
        if (!r.target_ns || (best && r.target_ns >= best))
            continue;
        if (surface_pace_output(surfaces[slots[i] - 1].id) == static_cast<int>(slot))
            best = r.target_ns;
    }
    return best;
}

void cu_surface_destroyed(surface_obj* s, bool dying) {
    if (g_cu.used_count() == 0)
        return;
    const unsigned slot = surface_slot(s);
    while (const unsigned root = g_cu.pop(slot))
        discard_cu(root, dying);
    // Its state sealed into an ancestor's queued update: the record must not outlive the
    // slot (the next surface there would receive it).
    for (unsigned i = 1; i <= lx::compositor::k_cu_pool && g_cu.used_count() > 0; ++i) {
        lx::compositor::cu_record& r = g_cu.at(i);
        if (!r.used || r.surface != slot || r.parent == 0)
            continue;
        drop_state(s, g_cu_payload[i].state, dying);
        g_cu_payload[i] = {};
        g_cu.unseal(i);
        g_cu.release(i);
    }
}

void cu_buffer_gone(wl_resource* buffer) {
    if (g_cu.used_count() == 0)
        return;
    for (unsigned i = 1; i <= lx::compositor::k_cu_pool; ++i) {
        if (!g_cu.at(i).used)
            continue;
        surface_pending& st = g_cu_payload[i].state;
        if (st.buffer != buffer)
            continue;
        // As for a cached attach: its contents are undefined now, the update keeps the rest
        // of its state and shows the previous content.
        st.buffer = nullptr;
        st.buffer_set = false;
        sync_point_drop(st.acquire);
        sync_point_signal_now(st.release);
    }
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
