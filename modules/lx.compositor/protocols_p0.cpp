module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#include <wayland-server-protocol.h>
#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#endif
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "zlm_shell_v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#endif
#endif

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if !defined(DRM_FORMAT_ARGB8888)
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_XRGB8888 0x34325258u
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import lx.input;
import :surface;
import :toplevel;
import :output;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {
namespace {

constexpr unsigned k_max_surfaces = 256;
constexpr unsigned k_max_frame_cbs = 64;
constexpr unsigned k_max_regions = 128;

struct frame_callback {
    wl_resource* resource = nullptr;
    bool used = false;
};

struct surface_pending {
    wl_resource* buffer = nullptr;
    bool buffer_set = false;
    int dx = 0;
    int dy = 0;
    int scale = 1;
    int32_t transform = 0;
    /// Union of every damage / damage_buffer since the last commit, surface-local. Both
    /// requests are treated alike: scale is 1 and the transform normal, so the two
    /// coordinate spaces coincide.
    lx::rect2i damage{};
    bool has_damage = false;
    frame_callback frame_cbs[k_max_frame_cbs]{};
    unsigned frame_cb_count = 0;
};

/// A wl_surface's role. Fixed for the surface's lifetime once assigned (wl_surface: a
/// surface has at most one role), which is what lets a second role request be rejected.
enum class surface_role : unsigned char { none, xdg_toplevel, cursor, subsurface, popup };

struct xdg_surface_obj;
struct xdg_toplevel_obj;

struct surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::surface_id id{};
    lx::client_id client{};
    surface_pending pending{};
    surface_pending current{};
    bool used = false;
    /// The buffer whose content this surface is currently showing. Kept as an id rather
    /// than the `wl_resource*` because the client may destroy the buffer object while the
    /// content stays on screen — which is exactly the case that has to keep working.
    lx::buffer_id current_buffer_id{};
    surface_role role = surface_role::none;
    /// Back-pointers into the xdg tables. Every destructor nulls the others' pointers to
    /// it, because libwayland destroys a client's objects in id order — wl_surface first —
    /// and a pointer left behind addresses a slot the next surface will reuse.
    xdg_surface_obj* xdg = nullptr;
    lx::toplevel_id toplevel{};
    /// wl_surface.set_input_region: the region is copied when the request arrives (the
    /// wl_region may be destroyed right after) and applied on commit. Infinite by default.
    lx::compositor::input_region input_pending{};
    bool input_pending_set = false;
    lx::compositor::input_region input_current{};
};

struct region_obj {
    wl_resource* resource = nullptr;
    lx::compositor::input_region region{};
    bool used = false;
};

struct buffer_obj {
    wl_resource* resource = nullptr;
    bool is_shm = false;
    bool is_dmabuf = false;
    lx::buffer_id id{};
    unsigned width = 0;
    unsigned height = 0;
    lx::gfx::dmabuf_desc dmabuf{};
    bool used = false;
};

struct xdg_wm_base_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
};

struct xdg_surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    surface_obj* surface = nullptr;
    xdg_toplevel_obj* toplevel = nullptr;
    bool used = false;
};

struct xdg_toplevel_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
    /// The compositor-side record; null once any of the three objects is gone.
    lx::toplevel_id id{};
    char title[256]{};
    char app_id[256]{};
    bool used = false;
};

struct dmabuf_params_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::gfx::dmabuf_desc desc{};
    bool used = false;
};

struct seat_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
};

struct data_device_manager_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
};

struct data_source_obj {
    wl_resource* resource = nullptr;
    char mime[16][128]{};
    unsigned mime_count = 0;
};

struct data_device_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    wl_resource* selection = nullptr;
};

// ── Storage ────────────────────────────────────────────────────────────────

surface_obj surfaces[k_max_surfaces]{};
region_obj regions[k_max_regions]{};
buffer_obj buffers[512]{};
xdg_surface_obj xdg_surfaces[256]{};
xdg_toplevel_obj xdg_toplevels[256]{};
dmabuf_params_obj dmabuf_params[128]{};
data_source_obj data_sources[64]{};
data_device_obj data_devices[64]{};

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

surface_obj* find_surface(wl_resource* res) {
    for (auto& s : surfaces)
        if (s.used && s.resource == res)
            return &s;
    return nullptr;
}

buffer_obj* alloc_buffer() {
    for (auto& b : buffers)
        if (!b.used) {
            b = {};
            b.used = true;
            return &b;
        }
    return nullptr;
}

buffer_obj* find_buffer(wl_resource* res) {
    if (!res)
        return nullptr;
    for (auto& b : buffers)
        if (b.used && b.resource == res)
            return &b;
    return nullptr;
}

extern p0_protocol_context* g_p0_ctx;

/// Queues a dated retirement for a texture.
///
/// Dated, because queue order alone is not enough: a snapshot published while the texture
/// was alive can still be waiting to composite, and it references the texture by id. Being
/// one frame conservative costs a frame of texture memory; being one frame early drops a
/// texture out from under a draw.
void retire_texture(unsigned texture_id) {
    if (!g_p0_ctx || !g_p0_ctx->textures || texture_id == 0)
        return;
    texture_update forget{};
    forget.kind = texture_update::op::forget;
    forget.texture_id = texture_id;
    forget.retire_after_frame = g_p0_ctx->frame_index + 1;
    (void)g_p0_ctx->textures->try_push(forget);
}

/// True while some surface is still showing this buffer.
bool buffer_is_displayed(struct wl_resource* buffer_resource) {
    if (!buffer_resource)
        return false;
    for (auto& s : surfaces) {
        if (s.used && s.current.buffer == buffer_resource)
            return true;
    }
    return false;
}

void destroy_buffer(buffer_obj* b) {
    if (!b || !b->used)
        return;

    // Destroying a wl_buffer does not take the content off the screen. The protocol is
    // explicit that a surface keeps displaying what it was given until a new buffer is
    // committed, and clients rely on it — the usual pattern is attach, commit, destroy the
    // buffer object immediately, because the compositor has either copied the pixels (shm)
    // or holds its own reference (dma-buf, via a duplicated fd).
    //
    // Retiring here regardless is what made a window render for about a second and then
    // vanish: the texture was dropped while the scene was still drawing it, and every
    // frame after that skipped the draw. The retirement belongs where the surface stops
    // referencing the content — a new commit, or the surface being destroyed.
    const unsigned texture_id =
        g_p0_ctx && g_p0_ctx->surfaces ? g_p0_ctx->surfaces->texture_for(b->id) : 0;
    if (texture_id != 0 && !buffer_is_displayed(b->resource))
        retire_texture(texture_id);

    b->dmabuf = {};
    *b = {};
}

/// wl_shm numbers ARGB8888 and XRGB8888 0 and 1 rather than by fourcc; every other format
/// in the enum is already a DRM fourcc.
lx::fourcc shm_format_to_fourcc(uint32_t shm_format) {
    switch (shm_format) {
    case WL_SHM_FORMAT_ARGB8888:
        return static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    case WL_SHM_FORMAT_XRGB8888:
        return static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    default:
        return static_cast<lx::fourcc>(shm_format);
    }
}

/// Channel order of the rows `stage_shm_pixels` will produce, which is not always the
/// client's: the Vulkan path samples RGBA textures, so it pays for a swizzle.
lx::fourcc staged_shm_format(wl_shm_buffer* shm) {
    if (!shm)
        return static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
    if (g_p0_ctx && g_p0_ctx->shm_native_format)
        return shm_format_to_fourcc(wl_shm_buffer_get_format(shm));
    return static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
}

/// Copies the client's shm pixels into compositor-owned staging. Must run on every commit:
/// a client is free to redraw into the same wl_buffer.
///
/// Which staging is used depends on how the render thread consumes it — see
/// `shm_pixel_store`. Exactly one of `seq_out` / `token_out` is set.
const unsigned char* stage_shm_pixels(buffer_obj* buf, wl_shm_buffer* shm, unsigned texture_id,
                                      unsigned long long& seq_out, unsigned long long& token_out) {
    seq_out = 0;
    token_out = 0;
    if (!buf || !shm || !g_p0_ctx)
        return nullptr;

    const int stride = wl_shm_buffer_get_stride(shm);
    const uint32_t fmt = wl_shm_buffer_get_format(shm);
    if (buf->width == 0 || buf->height == 0 || stride <= 0)
        return nullptr;

    const unsigned dst_stride = buf->width * 4u;
    const unsigned bytes = dst_stride * buf->height;

    unsigned char* out = nullptr;
    if (g_p0_ctx->shm_store)
        out = g_p0_ctx->shm_store->acquire(texture_id, bytes, token_out);
    else if (g_p0_ctx->shm_staging)
        out = g_p0_ctx->shm_staging->acquire(bytes, seq_out);
    if (!out)
        return nullptr;

    const bool native = g_p0_ctx->shm_native_format;

    wl_shm_buffer_begin_access(shm);
    const auto* data = static_cast<const unsigned char*>(wl_shm_buffer_get_data(shm));

    if (native) {
        // Nothing to reorder — a contiguous client buffer is one memcpy for the whole
        // frame, and a padded one is a memcpy per row.
        if (static_cast<unsigned>(stride) == dst_stride) {
            std::memcpy(out, data, bytes);
        } else {
            for (unsigned y = 0; y < buf->height; ++y) {
                std::memcpy(out + static_cast<size_t>(y) * dst_stride,
                            data + static_cast<size_t>(y) * static_cast<unsigned>(stride),
                            dst_stride);
            }
        }
        wl_shm_buffer_end_access(shm);
        return out;
    }

    const bool swap_rb = fmt == WL_SHM_FORMAT_ARGB8888 || fmt == WL_SHM_FORMAT_XRGB8888;
    const uint32_t alpha_fill = fmt == WL_SHM_FORMAT_XRGB8888 ? 0xFF000000u : 0u;
    for (unsigned y = 0; y < buf->height; ++y) {
        const auto* src_row = reinterpret_cast<const uint32_t*>(
            data + static_cast<size_t>(y) * static_cast<unsigned>(stride));
        auto* dst_row = reinterpret_cast<uint32_t*>(out + static_cast<size_t>(y) * dst_stride);
        for (unsigned x = 0; x < buf->width; ++x) {
            // Word-at-a-time so the loop vectorises; the byte-wise form did not.
            const uint32_t p = src_row[x];
            dst_row[x] = swap_rb ? (((p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) |
                                     ((p & 0x000000FFu) << 16)) |
                                    alpha_fill)
                                 : p;
        }
    }
    wl_shm_buffer_end_access(shm);
    return out;
}

lx::surface_id next_surface_id() {
    static unsigned n = 1;
    return lx::surface_id{n++};
}

lx::buffer_id next_buffer_id() {
    static unsigned n = 1;
    return lx::buffer_id{n++};
}

lx::client_id client_handle(wayland::client_connection& c) {
    return lx::client_id{c.id()};
}

// ── wl_buffer ──────────────────────────────────────────────────────────────

void buffer_destroy_resource(struct wl_resource* resource) {
    // The tracker may still hold this buffer for an in-flight frame; it must forget the
    // resource now rather than release it after the client freed it.
    if (g_p0_ctx && g_p0_ctx->lifecycle)
        g_p0_ctx->lifecycle->on_buffer_destroyed(resource);
    if (auto* b = find_buffer(resource))
        destroy_buffer(b);
}

const struct wl_buffer_interface buffer_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* resource) {
        wl_resource_destroy(resource);
    },
};

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

void queue_frame_callback(wl_resource* cb) {
    if (!cb || g_pending_frame_count >= 512)
        return;
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
        for (unsigned i = 0; i < s.current.frame_cb_count; ++i) {
            if (s.current.frame_cbs[i].resource == resource)
                s.current.frame_cbs[i] = {};
        }
    }
}

// ── wl_surface ─────────────────────────────────────────────────────────────

void surface_destroy_resource(struct wl_resource* resource) {
    auto* s = find_surface(resource);
    if (!s)
        return;
    // Drop input focus without a `leave`: the surface is going, and focus must not keep
    // pointing at it for the focus change the unmap below makes.
    if (s->ctx && s->ctx->input)
        s->ctx->input->remove(resource);
    if (s->toplevel && s->ctx && s->ctx->wm)
        s->ctx->wm->on_destroyed(s->toplevel);
    if (s->role == surface_role::cursor && s->ctx && s->ctx->cursor)
        s->ctx->cursor->on_surface_destroyed(s->id);
    if (s->xdg) {
        s->xdg->surface = nullptr;
        if (s->xdg->toplevel)
            s->xdg->toplevel->id = {};
    }
    // Last chance to retire the content this surface was showing: with the surface gone,
    // nothing else will ever reference its texture.
    if (s->ctx && s->ctx->surfaces && s->current_buffer_id)
        retire_texture(s->ctx->surfaces->texture_for(s->current_buffer_id));
    if (s->ctx && s->ctx->surfaces)
        s->ctx->surfaces->destroy(s->id);
    *s = {};
}

void surface_attach(struct wl_client*, struct wl_resource* resource, struct wl_resource* buffer,
                    int32_t x, int32_t y) {
    auto* s = find_surface(resource);
    if (!s)
        return;
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
    auto* s = find_surface(resource);
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
    auto* s = find_surface(resource);
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

void surface_set_opaque_region(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void surface_set_input_region(struct wl_client*, struct wl_resource* resource,
                              struct wl_resource* region) {
    auto* s = find_surface(resource);
    if (!s)
        return;
    // Copied now, applied on commit (wl_surface: double-buffered; the wl_region may be
    // destroyed immediately after this request). Null means infinite.
    if (const auto* r = region ? static_cast<region_obj*>(wl_resource_get_user_data(region))
                               : nullptr)
        s->input_pending = r->region;
    else
        s->input_pending = {};
    s->input_pending_set = true;
    if (s->input_pending.approximated &&
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

/// Hands a toplevel's commit to the window manager once the surface manager has the
/// content — on every exit of `surface_commit` past the protocol checks, including a
/// state-only commit and a buffer that failed to import, since an ack or a window
/// geometry change can ride either.
struct toplevel_commit_finisher {
    surface_obj* surface = nullptr;
    lx::rect2i old_node_rect{};

    ~toplevel_commit_finisher() {
        if (!surface || !surface->toplevel || !surface->ctx || !surface->ctx->wm ||
            !surface->ctx->surfaces)
            return;
        lx::compositor::toplevel_commit_info info{};
        info.buffer_size = surface->ctx->surfaces->buffer_size(surface->id);
        info.has_buffer = surface->current.buffer != nullptr && !info.buffer_size.empty();
        info.old_node_rect = old_node_rect;
        surface->ctx->wm->on_toplevel_commit(surface->toplevel, info);
    }
};

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

void surface_commit(struct wl_client*, struct wl_resource* resource) {
    auto* s = find_surface(resource);
    if (!s || !s->ctx || !s->ctx->surfaces)
        return;

    // xdg-shell rules that reject the whole commit come first, before any state moves.
    lx::compositor::toplevel_record* tl = nullptr;
    if (s->role == surface_role::xdg_toplevel && s->toplevel && s->ctx->toplevels)
        tl = s->ctx->toplevels->find_mut(s->toplevel);
    if (tl) {
        const bool attaching = s->pending.buffer_set && s->pending.buffer;
        if (attaching && !tl->ever_acked) {
            wl_resource_post_error(static_cast<wl_resource*>(tl->xdg_surface_resource),
                                   XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                                   "buffer attached before the first configure was acked");
            return;
        }
        if (!lx::compositor::toplevel_manager::apply_pending_state(*tl)) {
            wl_resource_post_error(static_cast<wl_resource*>(tl->xdg_toplevel_resource),
                                   XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                                   "min_size is larger than max_size");
            return;
        }
    }

    const bool new_buffer = s->pending.buffer_set;

    // Apply pending → current
    if (s->pending.buffer_set) {
        // The surface is about to stop showing its previous content. If that buffer object
        // is already gone, this is the point its texture becomes unreachable and can be
        // retired — destroy_buffer deliberately left it alone while it was still on screen.
        if (s->current.buffer && s->current.buffer != s->pending.buffer &&
            !find_buffer(s->current.buffer)) {
            if (s->ctx && s->ctx->surfaces)
                retire_texture(s->ctx->surfaces->texture_for(s->current_buffer_id));
        }
        s->current.buffer = s->pending.buffer;
        s->pending.buffer_set = false;
    }
    // The attach/offset delta applies to this commit only (wl_surface.offset).
    s->current.dx = s->pending.dx;
    s->current.dy = s->pending.dy;
    s->pending.dx = 0;
    s->pending.dy = 0;
    s->current.scale = s->pending.scale;
    s->current.transform = s->pending.transform;
    // Damage is per commit: it describes this commit's change and nothing after it.
    const bool had_damage = s->pending.has_damage;
    const lx::rect2i pending_damage = s->pending.damage;
    s->pending.has_damage = false;
    s->pending.damage = {};
    for (unsigned i = 0; i < s->pending.frame_cb_count; ++i) {
        if (s->pending.frame_cbs[i].used)
            queue_frame_callback(s->pending.frame_cbs[i].resource);
        s->pending.frame_cbs[i] = {};
    }
    s->pending.frame_cb_count = 0;
    if (s->input_pending_set) {
        s->input_pending_set = false;
        s->input_current = s->input_pending;
        // Only a drawn role is hit-tested; a surface that takes the toplevel role later
        // hands its current region over at get_toplevel.
        if (s->role == surface_role::xdg_toplevel)
            s->ctx->surfaces->set_input_region(s->id, s->input_current);
    }

    if (s->role == surface_role::cursor) {
        if (new_buffer)
            commit_cursor_surface(s);
        return;
    }

    if (tl && !tl->initial_commit_seen) {
        // The initial commit carries no buffer (rejected above) and asks for the first
        // configure, which the window manager sends right away.
        if (s->ctx->wm)
            s->ctx->wm->on_initial_commit(s->toplevel);
        return;
    }

    toplevel_commit_finisher finisher{};
    if (tl) {
        finisher.surface = s;
        finisher.old_node_rect = s->ctx->surfaces->node_rect(s->id);
    }

    // State-only commit, or a null buffer: there is no new content to import. A commit
    // without an attach does not re-read the old buffer — an shm buffer was copied and
    // released, and its storage now belongs to the client again.
    if (!new_buffer || !s->current.buffer) {
        return;
    }

    // Only a toplevel is drawn. Content on any other surface — roleless, a cursor surface,
    // a subsurface or popup (both still stubs) — would be imported and staged for nothing,
    // so the buffer goes straight back to the client instead.
    if (!tl) {
        // A role-less surface may still become the cursor: wl_pointer.set_cursor may name it
        // after this commit, and then shows the content committed here. Cursor-sized shm
        // content is kept for that; anything else is not a cursor candidate.
        if (s->role == surface_role::none && s->ctx->cursor)
            remember_cursor_candidate(s);
        wl_buffer_send_release(s->current.buffer);
        return;
    }

    buffer_obj* buf = find_buffer(s->current.buffer);
    wl_shm_buffer* shm = wl_shm_buffer_get(s->current.buffer);

    // SHM buffer may not be in our table yet — create on the fly from wl_shm_buffer.
    if (!buf && shm) {
        buf = alloc_buffer();
        if (!buf)
            return;
        buf->resource = s->current.buffer;
        buf->is_shm = true;
        buf->id = next_buffer_id();
        buf->width = static_cast<unsigned>(wl_shm_buffer_get_width(shm));
        buf->height = static_cast<unsigned>(wl_shm_buffer_get_height(shm));
        wl_resource_set_destructor(s->current.buffer, buffer_destroy_resource);
    }

    if (!buf)
        return;

    // Staging is keyed by texture id, which only exists after on_commit below, so the
    // format is resolved here and the pixel copy waits.
    const lx::fourcc shm_format = buf->is_shm ? staged_shm_format(shm)
                                              : static_cast<lx::fourcc>(lx::pixel_format::rgba8888);

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
    // A new buffer with no damage request is treated as fully damaged: strictly the
    // client said nothing changed, but every real client that does this means "all of it".
    if (had_damage)
        commit.damage = clip_damage_to_buffer(pending_damage, static_cast<int>(buf->width),
                                              static_cast<int>(buf->height));
    else
        commit.damage = {0, 0, static_cast<int>(buf->width), static_cast<int>(buf->height)};

    s->current_buffer_id = buf->id;

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

void surface_set_buffer_transform(struct wl_client*, struct wl_resource* resource,
                                  int32_t transform) {
    if (auto* s = find_surface(resource))
        s->pending.transform = transform;
}

void surface_set_buffer_scale(struct wl_client*, struct wl_resource* resource, int32_t scale) {
    if (auto* s = find_surface(resource))
        s->pending.scale = scale > 0 ? scale : 1;
}

void surface_damage_buffer(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                           int32_t width, int32_t height) {
    surface_damage(nullptr, resource, x, y, width, height);
}

void surface_offset(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y) {
    if (auto* s = find_surface(resource)) {
        s->pending.dx = x;
        s->pending.dy = y;
    }
}

const struct wl_surface_interface surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
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
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_destroy_resource);
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

p0_protocol_context* g_p0_ctx = nullptr;

void bind_compositor(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_compositor_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&compositor_impl, g_p0_ctx, nullptr);
}

// ── wl_subcompositor (minimal) ─────────────────────────────────────────────

const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .get_subsurface =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* /*surface*/, struct wl_resource* /*parent*/) {
            // Create a stub subsurface resource so clients don't error.
            wl_resource* res = wl_resource_create(client, &wl_subsurface_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct wl_subsurface_interface impl = {
                .destroy = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
                .set_position = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
                .place_above = [](struct wl_client*, struct wl_resource*,
                                  struct wl_resource*) {},
                .place_below = [](struct wl_client*, struct wl_resource*,
                                  struct wl_resource*) {},
                .set_sync = [](struct wl_client*, struct wl_resource*) {},
                .set_desync = [](struct wl_client*, struct wl_resource*) {},
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
        },
};

void bind_subcompositor(wayland::client_connection& client, unsigned id, int version) {
    auto res = wayland::resource::create(client, &wl_subcompositor_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&subcompositor_impl, nullptr, nullptr);
}

#if defined(LUMEN_HAS_PROTOCOL_GLUE)

// ── xdg_wm_base ────────────────────────────────────────────────────────────

void xdg_toplevel_destroy_resource(struct wl_resource* resource) {
    auto* t = static_cast<xdg_toplevel_obj*>(wl_resource_get_user_data(resource));
    if (!t || !t->used || t->resource != resource)
        return;
    // Destroying the role object unmaps the window and ends the record (xdg-shell).
    if (t->id && t->ctx && t->ctx->wm)
        t->ctx->wm->on_destroyed(t->id);
    if (t->xdg_surface) {
        t->xdg_surface->toplevel = nullptr;
        if (t->xdg_surface->surface)
            t->xdg_surface->surface->toplevel = {};
    }
    *t = {};
}

void xdg_surface_destroy_resource(struct wl_resource* resource) {
    auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
    if (!xs || !xs->used || xs->resource != resource)
        return;
    // Only reachable with a live toplevel at client disconnect (the destroy request
    // rejects it otherwise). The record cannot outlive its xdg_surface.
    if (xs->toplevel) {
        if (xs->toplevel->id && xs->ctx && xs->ctx->wm)
            xs->ctx->wm->on_destroyed(xs->toplevel->id);
        xs->toplevel->id = {};
        xs->toplevel->xdg_surface = nullptr;
    }
    if (xs->surface) {
        xs->surface->xdg = nullptr;
        xs->surface->toplevel = {};
    }
    *xs = {};
}

xdg_toplevel_obj* toplevel_of(wl_resource* resource) {
    auto* t = static_cast<xdg_toplevel_obj*>(wl_resource_get_user_data(resource));
    return t && t->used ? t : nullptr;
}

lx::compositor::toplevel_record* record_of(xdg_toplevel_obj* t) {
    if (!t || !t->id || !t->ctx || !t->ctx->toplevels)
        return nullptr;
    return t->ctx->toplevels->find_mut(t->id);
}

/// set_min_size / set_max_size: double-buffered, applied on the next wl_surface.commit.
void toplevel_set_size_limit(wl_resource* resource, int32_t width, int32_t height, bool max) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "negative size limit");
        return;
    }
    auto* rec = record_of(toplevel_of(resource));
    if (!rec)
        return;
    if (max) {
        rec->max_pending = {width, height};
        rec->max_pending_set = true;
    } else {
        rec->min_pending = {width, height};
        rec->min_pending_set = true;
    }
}

const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .set_parent = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .set_title =
        [](struct wl_client*, struct wl_resource* resource, const char* title) {
            auto* t = toplevel_of(resource);
            if (!t)
                return;
            std::snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
            if (t->id && t->ctx && t->ctx->toplevels) {
                t->ctx->toplevels->on_xdg_title(t->id, t->title);
                if (auto* rec = record_of(t); rec && rec->mapped && t->ctx->shell)
                    t->ctx->shell->emit_toplevel_state(*rec);
            }
        },
    .set_app_id =
        [](struct wl_client*, struct wl_resource* resource, const char* app_id) {
            auto* t = toplevel_of(resource);
            if (!t)
                return;
            std::snprintf(t->app_id, sizeof(t->app_id), "%s", app_id ? app_id : "");
            if (t->id && t->ctx && t->ctx->toplevels) {
                t->ctx->toplevels->on_xdg_app_id(t->id, t->app_id);
                if (auto* rec = record_of(t); rec && rec->mapped && t->ctx->shell)
                    t->ctx->shell->emit_toplevel_state(*rec);
            }
        },
    .show_window_menu = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                           uint32_t, int32_t, int32_t) {},
    .move =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial) {
            auto* t = toplevel_of(resource);
            if (!t || !t->id || !t->ctx || !t->ctx->wm)
                return;
            (void)t->ctx->wm->request_client_grab(t->id, client, serial,
                                                  lx::compositor::grab_mode::move, 0);
        },
    .resize =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial, uint32_t edges) {
            if (!lx::compositor::valid_resize_edges(edges)) {
                wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
                                       "invalid resize edge %u", edges);
                return;
            }
            auto* t = toplevel_of(resource);
            if (!t || !t->id || !t->ctx || !t->ctx->wm)
                return;
            (void)t->ctx->wm->request_client_grab(t->id, client, serial,
                                                  lx::compositor::grab_mode::resize, edges);
        },
    .set_max_size =
        [](struct wl_client*, struct wl_resource* resource, int32_t width, int32_t height) {
            toplevel_set_size_limit(resource, width, height, true);
        },
    .set_min_size =
        [](struct wl_client*, struct wl_resource* resource, int32_t width, int32_t height) {
            toplevel_set_size_limit(resource, width, height, false);
        },
    .set_maximized = [](struct wl_client*, struct wl_resource*) {},
    .unset_maximized = [](struct wl_client*, struct wl_resource*) {},
    .set_fullscreen = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .unset_fullscreen = [](struct wl_client*, struct wl_resource*) {},
    .set_minimized = [](struct wl_client*, struct wl_resource*) {},
};

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (xs && xs->toplevel) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                                       "xdg_surface destroyed before its xdg_toplevel");
                return;
            }
            wl_resource_destroy(resource);
        },
    .get_toplevel =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs || !xs->used || !xs->ctx)
                return;
            if (xs->toplevel) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                                       "xdg_surface already has a role object");
                return;
            }
            surface_obj* surf = xs->surface;
            if (!surf) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                                       "the wl_surface is gone");
                return;
            }
            // A surface keeps its role for life: once a toplevel, only ever a toplevel.
            if (surf->role != surface_role::none && surf->role != surface_role::xdg_toplevel) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                                       "wl_surface already has another role");
                return;
            }
            xdg_toplevel_obj* top = nullptr;
            for (auto& t : xdg_toplevels)
                if (!t.used) {
                    top = &t;
                    break;
                }
            if (!top) {
                wl_client_post_no_memory(client);
                return;
            }
            *top = {};
            top->used = true;
            top->ctx = xs->ctx;
            top->xdg_surface = xs;
            top->resource = wl_resource_create(client, &xdg_toplevel_interface,
                                               wl_resource_get_version(resource), id);
            if (!top->resource) {
                top->used = false;
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(top->resource, &xdg_toplevel_impl, top,
                                           xdg_toplevel_destroy_resource);
            xs->toplevel = top;
            surf->role = surface_role::xdg_toplevel;

            // No configure and no focus here: the client still has to make its initial
            // commit, and the first configure answers that (xdg-shell). Focus is taken when
            // the window maps, not when it is merely announced.
            if (xs->ctx->toplevels) {
                auto created = xs->ctx->toplevels->create(surf->client, surf->id,
                                                          surf->resource, xs->resource,
                                                          top->resource);
                if (!created) {
                    wl_client_post_no_memory(client);
                    return;
                }
                top->id = created.value();
                surf->toplevel = top->id;
            }
            if (!surf->input_current.infinite && xs->ctx->surfaces)
                xs->ctx->surfaces->set_input_region(surf->id, surf->input_current);
        },
    .get_popup =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource*, struct wl_resource*) {
            // Stub popup so clients don't die.
            wl_resource* res = wl_resource_create(client, &xdg_popup_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct xdg_popup_interface impl = {
                .destroy = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
                .grab = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                           uint32_t) {},
                .reposition = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                                 uint32_t) {},
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
        },
    .set_window_geometry =
        [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t width,
           int32_t height) {
            if (width <= 0 || height <= 0) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                                       "window geometry must have a positive size");
                return;
            }
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs || !xs->toplevel)
                return;
            if (auto* rec = record_of(xs->toplevel)) {
                rec->window_geometry_pending = {x, y, width, height};
                rec->window_geometry_pending_set = true;
            }
        },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs || !xs->toplevel) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                       "ack_configure on an xdg_surface without a role");
                return;
            }
            auto* rec = record_of(xs->toplevel);
            if (!rec)
                return;
            using ack = lx::compositor::toplevel_manager::ack_result;
            const ack result = lx::compositor::toplevel_manager::on_ack(*rec, serial);
            if (result == ack::invalid) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                                       "ack_configure with a serial never sent");
                return;
            }
            if (result == ack::ok &&
                lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
                char line[96];
                std::snprintf(line, sizeof(line), "ack id=%llu serial=%u", rec->id.id(), serial);
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.wm", line);
            }
        },
};

const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .create_positioner =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &xdg_positioner_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct xdg_positioner_interface impl = {
                .destroy = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
                .set_size = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
                .set_anchor_rect = [](struct wl_client*, struct wl_resource*, int32_t, int32_t,
                                      int32_t, int32_t) {},
                .set_anchor = [](struct wl_client*, struct wl_resource*, uint32_t) {},
                .set_gravity = [](struct wl_client*, struct wl_resource*, uint32_t) {},
                .set_constraint_adjustment = [](struct wl_client*, struct wl_resource*,
                                                uint32_t) {},
                .set_offset = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
                .set_reactive = [](struct wl_client*, struct wl_resource*) {},
                .set_parent_size = [](struct wl_client*, struct wl_resource*, int32_t,
                                      int32_t) {},
                .set_parent_configure = [](struct wl_client*, struct wl_resource*, uint32_t) {},
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
        },
    .get_xdg_surface =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* surface_res) {
            auto* ctx = static_cast<p0_protocol_context*>(wl_resource_get_user_data(resource));
            surface_obj* surf = find_surface(surface_res);
            if (!surf) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                                       "unknown surface");
                return;
            }
            // One xdg_surface per wl_surface, and none on a surface with a non-xdg role
            // (a cursor, a subsurface) — the same rule wlroots applies.
            if (surf->xdg || (surf->role != surface_role::none &&
                              surf->role != surface_role::xdg_toplevel &&
                              surf->role != surface_role::popup)) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                                       "wl_surface already has a role");
                return;
            }
            xdg_surface_obj* xs = nullptr;
            for (auto& s : xdg_surfaces)
                if (!s.used) {
                    xs = &s;
                    break;
                }
            if (!xs) {
                wl_client_post_no_memory(client);
                return;
            }
            *xs = {};
            xs->used = true;
            xs->ctx = ctx;
            xs->surface = surf;
            xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                              wl_resource_get_version(resource), id);
            if (!xs->resource) {
                xs->used = false;
                wl_client_post_no_memory(client);
                return;
            }
            surf->xdg = xs;
            wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                           xdg_surface_destroy_resource);
        },
    .pong = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

// ── window manager wire ops ────────────────────────────────────────────────

void wm_send_configure(void* toplevel_res, void* xdg_surface_res,
                       const lx::compositor::toplevel_configure& c, lx::size2i bounds,
                       bool initial) {
    auto* tl = static_cast<wl_resource*>(toplevel_res);
    auto* xs = static_cast<wl_resource*>(xdg_surface_res);
    if (!tl || !xs)
        return;
    const int version = wl_resource_get_version(tl);

    // A wl_array over stack storage, never released: wl_array_add would reallocate per
    // configure, and a resize sends one per frame.
    uint32_t storage[8];
    wl_array array{};
    array.data = storage;
    array.alloc = sizeof(storage);
    array.size = 0;

    // An empty capability list: Lumen has no window menu, maximize or minimize yet, and a
    // CSD client hides the buttons it is told the compositor does not honour.
    if (initial && version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        xdg_toplevel_send_wm_capabilities(tl, &array);
    if (bounds.width > 0 && bounds.height > 0 &&
        version >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
        xdg_toplevel_send_configure_bounds(tl, bounds.width, bounds.height);

    unsigned n = 0;
    if (c.states & lx::compositor::tl_maximized)
        storage[n++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (c.states & lx::compositor::tl_fullscreen)
        storage[n++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (c.states & lx::compositor::tl_resizing)
        storage[n++] = XDG_TOPLEVEL_STATE_RESIZING;
    if (c.states & lx::compositor::tl_activated)
        storage[n++] = XDG_TOPLEVEL_STATE_ACTIVATED;
    array.size = n * sizeof(uint32_t);
    xdg_toplevel_send_configure(tl, c.size.width, c.size.height, &array);
    xdg_surface_send_configure(xs, c.serial);
}

void wm_send_close(void* toplevel_res) {
    if (toplevel_res)
        xdg_toplevel_send_close(static_cast<wl_resource*>(toplevel_res));
}

unsigned wm_next_serial(void* user) {
    // The seat's counter, not wl_display_next_serial: every input event already draws from
    // it, and one serial space is what lets a move/resize request's serial be checked
    // against the press that allowed it without colliding with a configure serial.
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (ctx && ctx->seat)
        return ctx->seat->next_serial();
    if (ctx && ctx->server && ctx->server->native_display())
        return wl_display_next_serial(static_cast<wl_display*>(ctx->server->native_display()));
    return 0;
}

void bind_xdg_wm_base(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &xdg_wm_base_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&xdg_wm_base_impl, g_p0_ctx, nullptr);
}

// ── zwp_linux_dmabuf_v1 ────────────────────────────────────────────────────

void params_destroy_resource(struct wl_resource* resource) {
    for (auto& p : dmabuf_params) {
        if (p.used && p.resource == resource) {
            p.desc = {};
            p = {};
            return;
        }
    }
}

void params_create_common(struct wl_client* client, struct wl_resource* resource, uint32_t buffer_id,
                          int32_t width, int32_t height, uint32_t format, uint32_t flags,
                          bool immed) {
    (void)flags;
    dmabuf_params_obj* p = nullptr;
    for (auto& slot : dmabuf_params)
        if (slot.used && slot.resource == resource) {
            p = &slot;
            break;
        }
    if (!p || !p->ctx)
        return;
    if (width <= 0 || height <= 0 || p->desc.plane_count == 0) {
        if (immed)
            wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                                   "invalid dmabuf params");
        else
            zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }
    p->desc.width = static_cast<unsigned>(width);
    p->desc.height = static_cast<unsigned>(height);
    p->desc.format = static_cast<lx::fourcc>(format);

    buffer_obj* buf = alloc_buffer();
    if (!buf) {
        wl_client_post_no_memory(client);
        return;
    }
    buf->is_dmabuf = true;
    buf->id = next_buffer_id();
    buf->width = p->desc.width;
    buf->height = p->desc.height;
    // Move plane FDs into buffer.
    buf->dmabuf = static_cast<lx::gfx::dmabuf_desc&&>(p->desc);
    p->desc = {};

    buf->resource = wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (!buf->resource) {
        destroy_buffer(buf);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(buf->resource, &buffer_impl, buf, buffer_destroy_resource);

    if (immed) {
        // create_immed: buffer is ready immediately (no event).
    } else {
        zwp_linux_buffer_params_v1_send_created(resource, buf->resource);
    }
}

const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .add =
        [](struct wl_client*, struct wl_resource* resource, int32_t fd, uint32_t plane_idx,
           uint32_t offset, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
            dmabuf_params_obj* p = nullptr;
            for (auto& slot : dmabuf_params)
                if (slot.used && slot.resource == resource) {
                    p = &slot;
                    break;
                }
            if (!p || plane_idx >= 4) {
                if (fd >= 0)
                    ::close(fd);
                return;
            }
            auto& plane = p->desc.planes[plane_idx];
            plane.fd.reset(fd);
            plane.offset = offset;
            plane.stride = stride;
            plane.modifier = (static_cast<unsigned long long>(modifier_hi) << 32) | modifier_lo;
            if (plane_idx + 1 > p->desc.plane_count)
                p->desc.plane_count = plane_idx + 1;
        },
    .create =
        [](struct wl_client* client, struct wl_resource* resource, int32_t width, int32_t height,
           uint32_t format, uint32_t flags) {
            // create sends created/failed events; buffer id is the new_id in older versions —
            // in v1 create has no new_id, it sends the buffer via event. Actually looking at
            // the protocol: create(width, height, format, flags) → created(buffer) event where
            // buffer is a new_id in the event. libwayland handles new_id in events differently.
            // For params.create the signature is without buffer id — the created event carries
            // a new_id. We'll use create with id 0 and let send_created allocate… Actually
            // zwp_linux_buffer_params_v1_send_created takes wl_resource* buffer which must
            // already exist. So we create with id=0 (server-allocated).
            params_create_common(client, resource, 0, width, height, format, flags, false);
        },
    .create_immed =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t buffer_id,
           int32_t width, int32_t height, uint32_t format, uint32_t flags) {
            params_create_common(client, resource, buffer_id, width, height, format, flags, true);
        },
    .set_sampling_device =
        [](struct wl_client*, struct wl_resource*, struct wl_array*) {},
};

const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .create_params =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* ctx = static_cast<p0_protocol_context*>(wl_resource_get_user_data(resource));
            dmabuf_params_obj* p = nullptr;
            for (auto& slot : dmabuf_params)
                if (!slot.used) {
                    p = &slot;
                    break;
                }
            if (!p) {
                wl_client_post_no_memory(client);
                return;
            }
            *p = {};
            p->used = true;
            p->ctx = ctx;
            p->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                             wl_resource_get_version(resource), id);
            if (!p->resource) {
                p->used = false;
                return;
            }
            wl_resource_set_implementation(p->resource, &params_impl, p, params_destroy_resource);
        },
    .get_default_feedback =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            // Minimal feedback object so clients probing formats don't error.
            wl_resource* res =
                wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                   wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct zwp_linux_dmabuf_feedback_v1_interface impl = {
                .destroy = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
            // Immediate done so client proceeds.
            zwp_linux_dmabuf_feedback_v1_send_done(res);
        },
    .get_surface_feedback =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource*) {
            wl_resource* res =
                wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                   wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct zwp_linux_dmabuf_feedback_v1_interface impl = {
                .destroy = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
            zwp_linux_dmabuf_feedback_v1_send_done(res);
        },
};

void bind_linux_dmabuf(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&dmabuf_impl, g_p0_ctx, nullptr);
    auto* native = static_cast<wl_resource*>(res.native());
    // Advertise common formats (legacy format/modifier events for v3 clients).
    if (version >= 3 && native) {
        zwp_linux_dmabuf_v1_send_modifier(native, DRM_FORMAT_ARGB8888, 0, 0); // LINEAR lo/hi
        zwp_linux_dmabuf_v1_send_modifier(native, DRM_FORMAT_XRGB8888, 0, 0);
    } else if (native) {
        zwp_linux_dmabuf_v1_send_format(native, DRM_FORMAT_ARGB8888);
        zwp_linux_dmabuf_v1_send_format(native, DRM_FORMAT_XRGB8888);
    }
}

#endif // LUMEN_HAS_PROTOCOL_GLUE

// ── wl_seat ────────────────────────────────────────────────────────────────

/// libwayland dups the keymap FD when marshalling — `-1` always fails with EBADF.
/// Cache one sealed memfd for the process lifetime; each send_keymap dups it again.
struct keymap_source {
    int fd = -1;
    uint32_t size = 0;
};

[[nodiscard]] keymap_source& shared_keymap() {
    static keymap_source cached{};
    if (cached.fd >= 0)
        return cached;

#if !defined(_WIN32)
    // The real compiled keymap, from the same xkb state the compositor tracks modifiers
    // with. It used to be a hardcoded US string, which meant every client saw a US layout
    // whatever the user had configured, and the compositor's own idea of the keyboard did
    // not exist at all.
    //
    // Compiled once and cached: it is identical for every client, and the fd is duped by
    // libwayland on each send.
    // The seat's keymap, not a second one compiled here: clients must be told about the
    // same layout the compositor tracks modifiers with, or the two disagree the moment the
    // configured layout is not US.
    static lx::input::keyboard_keymap fallback{};
    lx::input::keyboard_keymap* keymap =
        (g_p0_ctx && g_p0_ctx->seat && g_p0_ctx->seat->keymap().valid())
            ? &g_p0_ctx->seat->keymap()
            : &fallback;
    if (!keymap->valid()) {
        if (auto compiled = keymap->compile(); !compiled) {
            lx::trace::logger::global().log_error(compiled.get_error(), "compositor.seat");
            return cached;
        }
    }

    unsigned size = 0;
    auto fd = keymap->keymap_fd(size);
    if (!fd) {
        lx::trace::logger::global().log_error(fd.get_error(), "compositor.seat");
        return cached;
    }

    cached.fd = std::move(fd).value().release();
    cached.size = size;
#endif
    return cached;
}

// ── pointer cursor ─────────────────────────────────────────────────────────

/// A cursor request counts only from the client holding pointer focus, quoting an `enter`
/// serial it was sent for that focus (wl_pointer.set_cursor, cursor-shape-v1). Anything
/// else — a stale request racing a focus change, another client — is ignored, not an error.
bool cursor_request_allowed(wl_client* client, uint32_t serial) {
    return g_p0_ctx && g_p0_ctx->input && g_p0_ctx->cursor &&
           g_p0_ctx->input->pointer_focus_client() == client &&
           g_p0_ctx->input->is_pointer_enter_serial(serial);
}

void pointer_set_cursor(struct wl_client* client, struct wl_resource* pointer, uint32_t serial,
                        struct wl_resource* surface_res, int32_t hot_x, int32_t hot_y) {
    surface_obj* surf = surface_res ? find_surface(surface_res) : nullptr;
    if (surface_res && !surf)
        return;
    if (surf) {
        // The surface becomes a cursor for life — and only a role-less one can.
        if (surf->role != surface_role::none && surf->role != surface_role::cursor) {
            wl_resource_post_error(pointer, WL_POINTER_ERROR_ROLE,
                                   "wl_surface already has another role");
            return;
        }
        surf->role = surface_role::cursor;
    }
    if (!cursor_request_allowed(client, serial))
        return;
    g_p0_ctx->cursor->set_client_surface(surf ? surf->id : lx::surface_id{}, hot_x, hot_y);
}

#if defined(LUMEN_HAS_PROTOCOL_GLUE)
/// wp_cursor_shape_device_v1 objects for wl_pointers. Fixed table: a client has one per
/// pointer, and the pointer's destructor makes its devices inert.
struct shape_device_obj {
    wl_resource* resource = nullptr;
    wl_resource* pointer = nullptr;
    bool used = false;
};
shape_device_obj shape_devices[64]{};

void shape_devices_drop_pointer(wl_resource* pointer) {
    for (auto& d : shape_devices)
        if (d.used && d.pointer == pointer)
            d.pointer = nullptr;
}

const struct wp_cursor_shape_device_v1_interface shape_device_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_shape =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t serial,
           uint32_t shape) {
            if (!wp_cursor_shape_device_v1_shape_is_valid(shape,
                                                          wl_resource_get_version(resource))) {
                wl_resource_post_error(resource, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                                       "invalid cursor shape");
                return;
            }
            // Null user data: a tablet-tool device (no tablet support) — inert.
            auto* dev = static_cast<shape_device_obj*>(wl_resource_get_user_data(resource));
            if (!dev || !dev->used || !dev->pointer)
                return;
            if (!cursor_request_allowed(client, serial))
                return;
            g_p0_ctx->cursor->set_client_shape(static_cast<lx::compositor::cursor_shape>(shape));
        },
};

const struct wp_cursor_shape_manager_v1_interface shape_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_pointer =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* pointer) {
            shape_device_obj* dev = nullptr;
            for (auto& d : shape_devices)
                if (!d.used) {
                    dev = &d;
                    break;
                }
            if (!dev) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource* res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *dev = {};
            dev->used = true;
            dev->resource = res;
            dev->pointer = pointer;
            wl_resource_set_implementation(res, &shape_device_impl, dev, [](wl_resource* r) {
                auto* d = static_cast<shape_device_obj*>(wl_resource_get_user_data(r));
                if (d && d->resource == r)
                    *d = {};
            });
        },
    .get_tablet_tool_v2 =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource*) {
            wl_resource* res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &shape_device_impl, nullptr, nullptr);
        },
};

void bind_cursor_shape(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_cursor_shape_manager_v1_interface, version,
                                         id);
    if (!res)
        return;
    res.set_implementation(&shape_manager_impl, g_p0_ctx, nullptr);
}
#endif

/// The cursor manager's texture hook: a premultiplied ARGB image padded into a fixed
/// `canvas` square and queued like any shm texture, in the order the active backend
/// samples (native for CPU, RGBA otherwise).
bool stage_compositor_argb(void* user, unsigned texture_id, const uint32_t* argb, unsigned width,
                           unsigned height, unsigned stride_px, unsigned canvas) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (!ctx || !ctx->textures || !argb || canvas == 0)
        return false;
    const unsigned bytes = canvas * canvas * 4u;
    unsigned long long seq = 0;
    unsigned long long token = 0;
    unsigned char* out = nullptr;
    if (ctx->shm_store)
        out = ctx->shm_store->acquire(texture_id, bytes, token);
    else if (ctx->shm_staging)
        out = ctx->shm_staging->acquire(bytes, seq);
    if (!out)
        return false;
    const bool native = ctx->shm_native_format;
    for (unsigned y = 0; y < canvas; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(out + static_cast<size_t>(y) * canvas * 4u);
        for (unsigned x = 0; x < canvas; ++x) {
            const uint32_t p =
                (y < height && x < width) ? argb[static_cast<size_t>(y) * stride_px + x] : 0u;
            row[x] = native ? p
                            : ((p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) |
                               ((p & 0x000000FFu) << 16));
        }
    }
    texture_update update{};
    update.kind = texture_update::op::shm;
    update.texture_id = texture_id;
    update.width = canvas;
    update.height = canvas;
    update.stride = canvas * 4u;
    update.format = native ? static_cast<lx::fourcc>(lx::pixel_format::argb8888)
                           : static_cast<lx::fourcc>(lx::pixel_format::rgba8888);
    update.pixels = out;
    update.shm_seq = seq;
    update.shm_token = token;
    if (!ctx->textures->try_push(update)) {
        if (token && ctx->shm_store)
            ctx->shm_store->release(token);
        return false;
    }
    return true;
}

const struct wl_seat_interface seat_impl = {
    .get_pointer =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &wl_pointer_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct wl_pointer_interface impl = {
                .set_cursor = pointer_set_cursor,
                .release = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            // Unregister on destroy, or the router keeps sending into freed resources.
            wl_resource_set_implementation(res, &impl, nullptr, [](wl_resource* r) {
                if (g_p0_ctx && g_p0_ctx->input)
                    g_p0_ctx->input->remove(r);
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
                shape_devices_drop_pointer(r);
#endif
            });
            if (g_p0_ctx && g_p0_ctx->input && g_p0_ctx->seat)
                g_p0_ctx->input->add_pointer(res, *g_p0_ctx->seat);
        },
    .get_keyboard =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &wl_keyboard_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct wl_keyboard_interface impl = {
                .release = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            wl_resource_set_implementation(res, &impl, nullptr, [](wl_resource* r) {
                if (g_p0_ctx && g_p0_ctx->input)
                    g_p0_ctx->input->remove(r);
            });

            const keymap_source& keymap = shared_keymap();
            if (keymap.fd < 0) {
                // Cannot marshal keymap without a real FD; drop the object rather than
                // poisoning the client connection with dup(-1).
                wl_resource_destroy(res);
                return;
            }
            wl_keyboard_send_keymap(res, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap.fd,
                                   keymap.size);
            if (wl_resource_get_version(res) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
                wl_keyboard_send_repeat_info(res, 40, 400);

            // Registered only after the keymap is sent: a client that receives `enter`
            // before it knows the layout cannot interpret the keys it is told are held.
            if (g_p0_ctx && g_p0_ctx->input && g_p0_ctx->seat)
                g_p0_ctx->input->add_keyboard(res, *g_p0_ctx->seat);
        },
    .get_touch =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &wl_touch_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct wl_touch_interface impl = {
                .release = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            wl_resource_set_implementation(res, &impl, nullptr, [](wl_resource* r) {
                if (g_p0_ctx && g_p0_ctx->input)
                    g_p0_ctx->input->remove(r);
            });
            if (g_p0_ctx && g_p0_ctx->input)
                g_p0_ctx->input->add_touch(res);
        },
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

void bind_seat(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_seat_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&seat_impl, g_p0_ctx, nullptr);
    auto* native = static_cast<wl_resource*>(res.native());
    if (native) {
        uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
        // Only advertise keyboard when keymap marshalling can succeed.
        if (shared_keymap().fd >= 0)
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        wl_seat_send_capabilities(native, caps);
        if (version >= 2)
            wl_seat_send_name(native, "default");
    }
}

// ── zlm_shell_v1 (privileged) ──────────────────────────────────────────────
//
// The seam docs/architecture.md is built around: the shell is an ordinary Wayland client
// except that it may bind this, and window-management intent flows over the policy bridge
// while the compositor keeps ownership of the state. Both sides were scaffolded and the
// global was never advertised, so lumen-shell connected and found nothing to bind.
//
// Advertised to every client but bound only by one that passes the privilege check —
// rather than hidden from the registry — so a rejected bind is a protocol error the shell
// can report, instead of a global that mysteriously does not exist.

// Requests name toplevels by the compositor-created zlm_toplevel_v1 handle, whose user data
// is the toplevel_id (shell_bridge::toplevel_from_handle); an inert handle decodes to 0.
// The bodies that are still empty are the ones whose compositor-side action does not exist
// yet (maximize/fullscreen/minimize, workspace objects, rule
// persistence) — they are declared here so the shell can bind and drive what does work,
// rather than failing at bind. Every request has a body: libwayland calls the slot
// unconditionally, so a missing one is a NULL call.
lx::compositor::window_manager* bridge_wm() { return g_p0_ctx ? g_p0_ctx->wm : nullptr; }

const struct zlm_policy_bridge_v1_interface policy_bridge_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .request_activate =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            if (auto* wm = bridge_wm())
                wm->activate(lx::compositor::shell_bridge::toplevel_from_handle(toplevel));
        },
    .request_raise =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            if (auto* wm = bridge_wm())
                (void)wm->raise(lx::compositor::shell_bridge::toplevel_from_handle(toplevel));
        },
    .request_lower =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            if (auto* wm = bridge_wm())
                (void)wm->lower(lx::compositor::shell_bridge::toplevel_from_handle(toplevel));
        },
    .set_toplevel_stacking_index =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel, uint32_t index) {
            if (auto* wm = bridge_wm())
                (void)wm->restack(lx::compositor::shell_bridge::toplevel_from_handle(toplevel),
                                  index);
        },
    // Same executor as xdg_toplevel.move/resize and the keybinding; the press serial is
    // checked but may belong to any client (normally the shell's own panel surface).
    .request_move =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource*, uint32_t serial) {
            if (auto* wm = bridge_wm())
                (void)wm->request_shell_grab(
                    lx::compositor::shell_bridge::toplevel_from_handle(toplevel), serial,
                    lx::compositor::grab_mode::move, 0);
        },
    .request_resize =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource*, uint32_t serial, uint32_t edges) {
            // The shell is trusted: an invalid edge is ignored (logged by the WM), not
            // a protocol error that would kill the desktop.
            if (auto* wm = bridge_wm())
                (void)wm->request_shell_grab(
                    lx::compositor::shell_bridge::toplevel_from_handle(toplevel), serial,
                    lx::compositor::grab_mode::resize, edges);
        },
    .request_maximize = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .request_unmaximize = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .request_fullscreen = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                             struct wl_resource*) {},
    .request_minimize = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .request_close =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            if (auto* wm = bridge_wm())
                wm->request_close(lx::compositor::shell_bridge::toplevel_from_handle(toplevel));
        },
};

const struct zlm_workspace_manager_v1_interface workspace_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .create_workspace = [](struct wl_client*, struct wl_resource*, uint32_t, const char*) {},
    .destroy_workspace = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .set_active = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .reorder_workspace =
        [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {},
};

const struct zlm_window_rules_v1_interface window_rules_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .reload_rules = [](struct wl_client*, struct wl_resource*) {},
    .set_workspace_rule =
        [](struct wl_client*, struct wl_resource*, const char*, uint32_t) {},
    .set_monitor_rule = [](struct wl_client*, struct wl_resource*, const char*, const char*) {},
    .set_geometry_rule = [](struct wl_client*, struct wl_resource*, const char*, int32_t,
                            int32_t, int32_t, int32_t) {},
    .set_state_rule = [](struct wl_client*, struct wl_resource*, const char*, uint32_t) {},
    .set_decoration_rule = [](struct wl_client*, struct wl_resource*, const char*, uint32_t) {},
};

const struct zlm_shell_v1_interface shell_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_workspace_manager =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &zlm_workspace_manager_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res)
                return;
            wl_resource_set_implementation(res, &workspace_manager_impl, g_p0_ctx, nullptr);
        },
    .get_policy_bridge =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &zlm_policy_bridge_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res)
                return;
            wl_resource_set_implementation(res, &policy_bridge_impl, g_p0_ctx,
                                           [](struct wl_resource* r) {
                                               if (g_p0_ctx && g_p0_ctx->shell)
                                                   g_p0_ctx->shell->on_bridge_destroyed(r);
                                           });
            // The shell's first act is to learn the current state — ADR-004 §5's
            // session-startup path. Emitted on bridge creation rather than on shell bind,
            // because the bridge is the object the events are addressed to.
            if (g_p0_ctx && g_p0_ctx->shell)
                g_p0_ctx->shell->on_bridge_bound(res);
        },
    .get_window_rules =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &zlm_window_rules_v1_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res)
                return;
            wl_resource_set_implementation(res, &window_rules_impl, g_p0_ctx, nullptr);
        },
};

void bind_shell(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx || !g_p0_ctx->server)
        return;

    // Privilege is checked at bind, against the connecting peer's credentials — not at
    // advertisement. A client that is not the shell gets a protocol error naming the
    // reason, which is debuggable; silently omitting the global is not.
    if (!g_p0_ctx->server->allow_privileged_global(client.credentials(), "zlm_shell_v1")) {
        auto reject = wayland::resource::create(client, &zlm_shell_v1_interface, version, id);
        if (reject)
            reject.post_error(0, "zlm_shell_v1 is restricted to the session shell");
        return;
    }

    auto res = wayland::resource::create(client, &zlm_shell_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&shell_impl, g_p0_ctx, nullptr);
}

// ── wl_output ──────────────────────────────────────────────────────────────

const struct wl_output_interface output_impl = {
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

void bind_output(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_output_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&output_impl, g_p0_ctx, nullptr);
    auto* native = static_cast<wl_resource*>(res.native());
    if (!native)
        return;

    int w = 1920, h = 1080;
    if (g_p0_ctx->outputs && g_p0_ctx->outputs->count() > 0) {
        const auto first = g_p0_ctx->outputs->state(g_p0_ctx->outputs->nth(0));
        if (first.geometry.width > 0 && first.geometry.height > 0) {
            w = first.geometry.width;
            h = first.geometry.height;
        }
    }
    wl_output_send_geometry(native, 0, 0, w, h, WL_OUTPUT_SUBPIXEL_UNKNOWN, "lumen", "headless",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(native, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, w, h, 60000);
    if (version >= 2)
        wl_output_send_scale(native, 1);
    if (version >= 4) {
        wl_output_send_name(native, "LUMEN-0");
        wl_output_send_description(native, "Lumen output");
    }
    wl_output_send_done(native);
}

// ── wl_data_device_manager ─────────────────────────────────────────────────

const struct wl_data_source_interface data_source_impl = {
    .offer =
        [](struct wl_client*, struct wl_resource* resource, const char* mime) {
            for (auto& s : data_sources) {
                if (s.resource != resource)
                    continue;
                if (s.mime_count < 16 && mime) {
                    std::snprintf(s.mime[s.mime_count], sizeof(s.mime[0]), "%s", mime);
                    ++s.mime_count;
                }
                return;
            }
        },
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .set_actions = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

const struct wl_data_device_interface data_device_impl = {
    .start_drag = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                     struct wl_resource*, struct wl_resource*, uint32_t) {},
    .set_selection =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* source,
           uint32_t) {
            for (auto& d : data_devices) {
                if (d.resource == resource) {
                    d.selection = source;
                    return;
                }
            }
        },
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
};

const struct wl_data_device_manager_interface data_device_manager_impl = {
    .create_data_source =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            data_source_obj* s = nullptr;
            for (auto& slot : data_sources)
                if (!slot.resource) {
                    s = &slot;
                    break;
                }
            if (!s) {
                wl_client_post_no_memory(client);
                return;
            }
            *s = {};
            s->resource = wl_resource_create(client, &wl_data_source_interface,
                                             wl_resource_get_version(resource), id);
            if (!s->resource)
                return;
            wl_resource_set_implementation(s->resource, &data_source_impl, s,
                                           [](struct wl_resource* r) {
                                               for (auto& slot : data_sources)
                                                   if (slot.resource == r) {
                                                       slot = {};
                                                       return;
                                                   }
                                           });
        },
    .get_data_device =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* /*seat*/) {
            auto* ctx = static_cast<p0_protocol_context*>(wl_resource_get_user_data(resource));
            data_device_obj* d = nullptr;
            for (auto& slot : data_devices)
                if (!slot.resource) {
                    d = &slot;
                    break;
                }
            if (!d) {
                wl_client_post_no_memory(client);
                return;
            }
            *d = {};
            d->ctx = ctx;
            d->resource = wl_resource_create(client, &wl_data_device_interface,
                                             wl_resource_get_version(resource), id);
            if (!d->resource)
                return;
            wl_resource_set_implementation(d->resource, &data_device_impl, d,
                                           [](struct wl_resource* r) {
                                               for (auto& slot : data_devices)
                                                   if (slot.resource == r) {
                                                       slot = {};
                                                       return;
                                                   }
                                           });
        },
#if defined(WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION)
    .release =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
#endif
};

void bind_data_device_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &wl_data_device_manager_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&data_device_manager_impl, g_p0_ctx, nullptr);
}

} // namespace

void install_core(p0_protocol_context& ctx) {
    g_p0_ctx = &ctx;
    if (!ctx.server)
        return;
    if (ctx.cursor)
        ctx.cursor->set_stage_fn(&stage_compositor_argb, &ctx);
    ctx.server->add_global({.interface_name = "wl_compositor",
                            .version = 6,
                            .privileged = false,
                            .interface_desc = &wl_compositor_interface},
                           &bind_compositor);
    ctx.server->add_global({.interface_name = "wl_subcompositor",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wl_subcompositor_interface},
                           &bind_subcompositor);
    ctx.server->add_global({.interface_name = "wl_seat",
                            .version = 9,
                            .privileged = false,
                            .interface_desc = &wl_seat_interface},
                           &bind_seat);
    ctx.server->add_global({.interface_name = "wl_output",
                            .version = 4,
                            .privileged = false,
                            .interface_desc = &wl_output_interface},
                           &bind_output);
    ctx.server->add_global({.interface_name = "wl_data_device_manager",
                            .version = 3,
                            .privileged = false,
                            .interface_desc = &wl_data_device_manager_interface},
                           &bind_data_device_manager);
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (ctx.wm) {
        lx::compositor::wm_protocol_ops ops{};
        ops.send_configure = &wm_send_configure;
        ops.next_serial = &wm_next_serial;
        ops.send_close = &wm_send_close;
        ops.user = &ctx;
        ctx.wm->set_protocol_ops(ops);
    }
    ctx.server->add_global({.interface_name = "xdg_wm_base",
                            .version = 6,
                            .privileged = false,
                            .interface_desc = &xdg_wm_base_interface},
                           &bind_xdg_wm_base);
    ctx.server->add_global({.interface_name = "zlm_shell_v1",
                            .version = 1,
                            .privileged = true,
                            .interface_desc = &zlm_shell_v1_interface},
                           bind_shell);

    ctx.server->add_global({.interface_name = "wp_cursor_shape_manager_v1",
                            .version = 2,
                            .privileged = false,
                            .interface_desc = &wp_cursor_shape_manager_v1_interface},
                           &bind_cursor_shape);

    ctx.server->add_global({.interface_name = "zwp_linux_dmabuf_v1",
                            .version = 5,
                            .privileged = false,
                            .interface_desc = &zwp_linux_dmabuf_v1_interface},
                           &bind_linux_dmabuf);
#endif
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

lx::result<void> lx::compositor::install_p0_protocols(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.server) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "p0 install: null server");
    }
    p0::install_core(ctx);
    return {};
#else
    (void)ctx;
    return lx::make_error(lx::error_domain::wayland,
                          static_cast<int>(lx::wayland_err::bind_failed),
                          "libwayland-server required for P0 protocols");
#endif
}

void lx::compositor::fire_frame_callbacks(p0_protocol_context& ctx, unsigned frame_time_ms) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::fire_frames(frame_time_ms);
#else
    (void)ctx;
    (void)frame_time_ms;
#endif
}

void lx::compositor::release_wl_buffer_resource(void* resource, void*) {
#if defined(LUMEN_HAS_WAYLAND)
    if (resource)
        wl_buffer_send_release(static_cast<wl_resource*>(resource));
#else
    (void)resource;
#endif
}
