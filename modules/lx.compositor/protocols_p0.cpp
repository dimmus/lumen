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
#include <ctime>
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
#include <fcntl.h>
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
import :surface_tree;
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

/// One content update's worth of wl_surface state. Used three times per surface: the
/// pending state requests edit, the cached state a synchronized subsurface accumulates
/// until its parent's state is applied, and the current state.
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
    /// set_input_region / set_opaque_region: the region is copied when the request arrives
    /// (the wl_region may be destroyed right after) and applied with the rest.
    bool input_set = false;
    lx::compositor::input_region input{};
    bool opaque_set = false;
    lx::compositor::input_region opaque{};
};

/// A wl_surface's role. Fixed for the surface's lifetime once assigned (wl_surface: a
/// surface has at most one role), which is what lets a second role request be rejected.
enum class surface_role : unsigned char { none, xdg_toplevel, cursor, subsurface, popup, layer };

struct xdg_surface_obj;
struct xdg_toplevel_obj;
struct subsurface_obj;
struct layer_surface_obj;

struct surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::surface_id id{};
    lx::client_id client{};
    surface_pending pending{};
    /// Every commit merges `pending` in here; `apply_cached` is the only way it becomes
    /// current. A root or desynchronized surface applies it in the same commit, a
    /// synchronized subsurface when its parent's state is applied.
    surface_pending cached{};
    bool has_cached = false;
    /// `buffer` is the live wl_buffer being shown, or null once the client destroyed that
    /// object — the content stays on screen regardless (`has_content`).
    surface_pending current{};
    bool used = false;
    /// The buffer whose content this surface is currently showing. Kept as an id rather
    /// than the `wl_resource*` because the client may destroy the buffer object while the
    /// content stays on screen — which is exactly the case that has to keep working.
    lx::buffer_id current_buffer_id{};
    /// Compositor texture of the current content (0 for an import-cache dma-buf), retired
    /// once nothing shows it.
    unsigned current_texture = 0;
    /// The surface shows content: the last applied attach was non-null.
    bool has_content = false;
    /// The current buffer's format carries no alpha channel.
    bool format_opaque = false;
    surface_role role = surface_role::none;
    /// Back-pointers into the xdg tables. Every destructor nulls the others' pointers to
    /// it, because libwayland destroys a client's objects in id order — wl_surface first —
    /// and a pointer left behind addresses a slot the next surface will reuse.
    xdg_surface_obj* xdg = nullptr;
    lx::toplevel_id toplevel{};
    /// The live wl_subsurface for this surface, or null (none yet, or destroyed).
    subsurface_obj* sub = nullptr;
    /// The live zwlr_layer_surface_v1 for this surface, or null.
    layer_surface_obj* layer = nullptr;
    /// Applied input region (infinite by default) and opaque region (empty by default).
    lx::compositor::input_region input_current{};
    lx::compositor::input_region opaque_current{};
};

/// wl_subsurface. Inert once its surface is gone; a surface whose parent is gone keeps its
/// object (and its role) until the client destroys it.
struct subsurface_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
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

/// Destroy listener for a wl_shm buffer, parallel to `buffers` by index. libwayland owns
/// those resources and their destructor — it frees the wl_shm_buffer and drops its pool
/// reference — so the compositor listens instead of replacing it. (Replacing it, as this
/// file once did, leaked every shm buffer struct and kept its pool mapped for good.)
struct shm_buffer_listener {
    wl_listener listener{}; // first member: the notify casts back to this struct
    struct buffer_obj* owner = nullptr;
};

/// xdg_wm_base, per bound resource: the xdg_surfaces created from it are counted, because
/// destroying it while any is alive is `defunct_surfaces`, and popup errors
/// (invalid_positioner, invalid_popup_parent, not_the_topmost_popup) are posted on it.
struct xdg_wm_base_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    unsigned surfaces = 0;
    bool used = false;
};

struct xdg_popup_obj;
struct decoration_obj;

struct xdg_surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    surface_obj* surface = nullptr;
    xdg_toplevel_obj* toplevel = nullptr;
    xdg_popup_obj* popup = nullptr;
    /// The xdg_wm_base it came from; null once that is gone (only at client teardown).
    xdg_wm_base_obj* wm_base = nullptr;
    bool used = false;
};

/// xdg_positioner: rules only, copied into a popup at get_popup / reposition.
struct positioner_obj {
    wl_resource* resource = nullptr;
    lx::compositor::positioner_rules rules{};
    bool used = false;
};

/// xdg_popup. The record lives in `popup_manager`, keyed by the surface id; this is the wire
/// half, inert once the record is gone.
struct xdg_popup_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
    lx::surface_id surface{};
    bool used = false;
};

struct xdg_toplevel_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
    /// The compositor-side record; null once any of the three objects is gone.
    lx::toplevel_id id{};
    /// The live zxdg_toplevel_decoration_v1 for this toplevel, or null.
    decoration_obj* deco = nullptr;
    char title[256]{};
    char app_id[256]{};
    bool used = false;
};

/// zwlr_layer_surface_v1. The record lives in `layer_shell`, keyed by the surface id; this
/// is the wire half, inert once its wl_surface is gone.
struct layer_surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
};

/// zxdg_toplevel_decoration_v1. Inert once its toplevel is gone (only at client teardown:
/// destroying the toplevel first is `orphaned`).
struct decoration_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_toplevel_obj* toplevel = nullptr;
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

constexpr unsigned k_max_buffers = 512;

surface_obj surfaces[k_max_surfaces]{};
subsurface_obj subsurfaces[k_max_surfaces]{};
region_obj regions[k_max_regions]{};
buffer_obj buffers[k_max_buffers]{};
shm_buffer_listener shm_listeners[k_max_buffers]{};
xdg_surface_obj xdg_surfaces[256]{};
xdg_toplevel_obj xdg_toplevels[256]{};
decoration_obj toplevel_decorations[256]{};
xdg_wm_base_obj xdg_wm_bases[128]{};
positioner_obj positioners[128]{};
xdg_popup_obj xdg_popups[lx::compositor::popup_manager::k_max]{};
layer_surface_obj layer_surfaces[lx::compositor::layer_shell::k_max]{};
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

/// The wl_surface's own record — its resource user data, O(1).
surface_obj* surface_from(wl_resource* res) {
    if (!res)
        return nullptr;
    auto* s = static_cast<surface_obj*>(wl_resource_get_user_data(res));
    return s && s->used && s->resource == res ? s : nullptr;
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

extern p0_protocol_context* g_p0_ctx;

/// The surface record of a surface id, through the surface manager's resource registry.
surface_obj* surface_by_id(lx::surface_id id) {
    if (!g_p0_ctx || !g_p0_ctx->surfaces || !id)
        return nullptr;
    return surface_from(static_cast<wl_resource*>(g_p0_ctx->surfaces->resource(id)));
}

/// X7: a client being torn down has already left the server's table (its destroy signal
/// fires before libwayland destroys its objects, in id order). Destructors must not post
/// protocol errors to it, nor do work on its behalf.
bool client_dying(wl_client* client) {
    return !g_p0_ctx || !g_p0_ctx->server || !client ||
           g_p0_ctx->server->find_client_by_native(client) == nullptr;
}

[[nodiscard]] bool surface_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void surface_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.surface", line);
}

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
    forget.visible_from_frame = g_p0_ctx->building_frame;
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
    // The surface manager's record goes with the object (it owns duplicated plane fds);
    // a surface still showing the content keeps the texture id itself.
    if (g_p0_ctx && g_p0_ctx->surfaces)
        g_p0_ctx->surfaces->forget_buffer(b->id);

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

/// A wl_buffer object is going away (either kind).
void buffer_gone(struct wl_resource* resource, buffer_obj* b) {
    // The tracker may still hold this buffer for an in-flight frame; it must forget the
    // resource now rather than release it after the client freed it.
    if (g_p0_ctx && g_p0_ctx->lifecycle)
        g_p0_ctx->lifecycle->on_buffer_destroyed(resource);
    // Before the references below are dropped: it asks whether a surface still shows it.
    if (b)
        destroy_buffer(b);
    // No surface may keep naming the dead object. A pending or cached attach of it is
    // dropped — its contents are undefined now, and keeping the previous content is the
    // safe reading — and content already on screen stays there without its object.
    for (auto& s : surfaces) {
        if (!s.used)
            continue;
        if (s.pending.buffer == resource) {
            s.pending.buffer = nullptr;
            s.pending.buffer_set = false;
        }
        if (s.cached.buffer == resource) {
            s.cached.buffer = nullptr;
            s.cached.buffer_set = false;
        }
        if (s.current.buffer == resource)
            s.current.buffer = nullptr;
    }
}

/// Destructor of the dma-buf wl_buffers this file creates itself.
void buffer_destroy_resource(struct wl_resource* resource) {
    auto* b = static_cast<buffer_obj*>(wl_resource_get_user_data(resource));
    buffer_gone(resource, b && b->used && b->resource == resource ? b : nullptr);
}

void shm_buffer_destroyed(struct wl_listener* listener, void* data) {
    auto* l = reinterpret_cast<shm_buffer_listener*>(listener);
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    buffer_obj* b = l->owner;
    l->owner = nullptr;
    buffer_gone(static_cast<wl_resource*>(data), b);
}

const struct wl_buffer_interface buffer_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* resource) {
        wl_resource_destroy(resource);
    },
};

/// The record of a wl_buffer: user data for the dma-buf buffers created here, the destroy
/// listener for shm buffers (libwayland's objects). O(1) either way.
buffer_obj* find_buffer(wl_resource* res) {
    if (!res)
        return nullptr;
    if (wl_resource_instance_of(res, &wl_buffer_interface, &buffer_impl)) {
        auto* b = static_cast<buffer_obj*>(wl_resource_get_user_data(res));
        return b && b->used && b->resource == res ? b : nullptr;
    }
    if (wl_listener* l = wl_resource_get_destroy_listener(res, shm_buffer_destroyed))
        return reinterpret_cast<shm_buffer_listener*>(l)->owner;
    return nullptr;
}

/// Starts tracking a wl_shm buffer the first time a surface names it — at attach, so that
/// destroying it before the commit is seen too.
buffer_obj* track_shm_buffer(wl_resource* res, wl_shm_buffer* shm) {
    if (auto* known = find_buffer(res))
        return known;
    if (!shm)
        return nullptr;
    buffer_obj* b = alloc_buffer();
    if (!b)
        return nullptr;
    b->resource = res;
    b->is_shm = true;
    b->id = next_buffer_id();
    b->width = static_cast<unsigned>(wl_shm_buffer_get_width(shm));
    b->height = static_cast<unsigned>(wl_shm_buffer_get_height(shm));
    auto& l = shm_listeners[static_cast<unsigned>(b - buffers)];
    l = {};
    l.owner = b;
    l.listener.notify = shm_buffer_destroyed;
    wl_resource_add_destroy_listener(res, &l.listener);
    return b;
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

// ── wl_surface ─────────────────────────────────────────────────────────────

void apply_cached(surface_obj* s, const char* cause, unsigned depth);
void flush_desync(surface_obj* s, const char* cause);

/// Drops a surface's cached content update without applying it — for a client that is
/// being torn down, whose buffers and callbacks libwayland is destroying anyway.
void discard_cached(surface_obj* s) {
    s->cached = {};
    s->has_cached = false;
}

void surface_destroy_resource(struct wl_resource* resource) {
    auto* s = surface_from(resource);
    if (!s)
        return;
    const bool dying = client_dying(wl_resource_get_client(resource));
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
    default:
        return false;
    }
}

/// Content applied to this surface is imported and kept as its current content, so it is
/// drawn the moment the surface becomes drawable without a new attach: a drawable role, a
/// role-less surface (it may become a subsurface, whose content then maps with its parent:
/// "a non-NULL wl_buffer is applied and the parent surface is mapped", in either order), or
/// a subsurface whose wl_subsurface or parent is gone (a new wl_subsurface may link it
/// again). wlroots keeps every surface's current buffer the same way. Only a cursor, a
/// dismissed popup, a closed layer surface and an xdg_toplevel role without its object
/// release content undrawn.
[[nodiscard]] bool keeps_content(const surface_obj* s) {
    return s->role == surface_role::none || s->role == surface_role::subsurface ||
           drawable_role(s);
}

/// The xdg-shell rules that reject a whole commit, checked on the pending state before
/// anything moves. False when an error was posted.
/// The xdg_wm_base an xdg_surface came from, where popup errors are posted; null only for
/// a client being torn down.
wl_resource* wm_base_resource(const xdg_surface_obj* xs) {
    return xs && xs->wm_base ? xs->wm_base->resource : nullptr;
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

/// The role's part of a commit of a root surface, after its tree has been applied.
void finish_role(surface_obj* s) {
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

p0_protocol_context* g_p0_ctx = nullptr;

void bind_compositor(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wl_compositor_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&compositor_impl, g_p0_ctx, nullptr);
}

// ── wl_subcompositor / wl_subsurface ──────────────────────────────────────
//
// Wire glue only: parentage, cached child state and paint order live in `surface_tree`,
// and the content-update choreography in `surface_commit` / `apply_cached`.

subsurface_obj* subsurface_from(wl_resource* r) {
    auto* o = static_cast<subsurface_obj*>(wl_resource_get_user_data(r));
    return o && o->used && o->resource == r ? o : nullptr;
}

/// The subsurface's surface while it still plays the role; null when inert.
surface_obj* subsurface_surface(wl_resource* r) {
    auto* o = subsurface_from(r);
    return o && o->surface && o->surface->ctx && o->surface->ctx->tree ? o->surface : nullptr;
}

void subsurface_log_sync(const surface_obj* s) {
    if (!surface_debug())
        return;
    const auto* tree = s->ctx->tree;
    char line[128];
    std::snprintf(line, sizeof(line), "subsurface sync surface=%llu mode=%s effective=%s",
                  s->id.id(), tree->sync_flag(s->id) ? "sync" : "desync",
                  tree->effectively_sync(s->id) ? "sync" : "desync");
    surface_log(line);
}

void subsurface_place(wl_resource* resource, wl_resource* sibling_res, bool above) {
    surface_obj* s = subsurface_surface(resource);
    surface_obj* sibling = surface_from(sibling_res);
    if (!s || !sibling)
        return;
    using restack = lx::compositor::surface_tree::restack_error;
    if (s->ctx->tree->place(s->id, sibling->id, above) == restack::bad_surface)
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "place_%s: the reference is neither the parent nor a sibling",
                               above ? "above" : "below");
}

void subsurface_destroy_resource(wl_resource* resource) {
    auto* o = subsurface_from(resource);
    if (!o)
        return;
    surface_obj* s = o->surface;
    *o = {};
    if (!s)
        return;
    s->sub = nullptr;
    // Immediate (wl_subsurface.destroy): the subtree is unmapped and the surface forgets its
    // position and z-order. It keeps the subsurface role; a new wl_subsurface may follow.
    if (auto* tree = s->ctx ? s->ctx->tree : nullptr)
        tree->unlink(s->id);
    // Without a parent nothing synchronizes it any more: content it had cached is applied
    // (buffers released, callbacks answered) — not drawn, since the surface is unmapped.
    if (client_dying(wl_resource_get_client(resource)))
        discard_cached(s);
    else
        flush_desync(s, "unlink");
}

const struct wl_subsurface_interface subsurface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_position =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
            // Double-buffered on the parent: applied when the parent's state is.
            if (surface_obj* s = subsurface_surface(r))
                s->ctx->tree->set_position_pending(s->id, {x, y});
        },
    .place_above =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* sibling) {
            subsurface_place(r, sibling, true);
        },
    .place_below =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* sibling) {
            subsurface_place(r, sibling, false);
        },
    .set_sync =
        [](struct wl_client*, struct wl_resource* r) {
            if (surface_obj* s = subsurface_surface(r)) {
                s->ctx->tree->set_sync(s->id, true);
                subsurface_log_sync(s);
            }
        },
    .set_desync =
        [](struct wl_client*, struct wl_resource* r) {
            surface_obj* s = subsurface_surface(r);
            if (!s)
                return;
            auto* tree = s->ctx->tree;
            const bool was_sync = tree->effectively_sync(s->id);
            tree->set_sync(s->id, false);
            subsurface_log_sync(s);
            // Effectively synchronized → desynchronized: its cached state, and that of
            // descendants it was holding back, applies now (wlroots unlocks it the same way).
            if (was_sync && !tree->effectively_sync(s->id))
                flush_desync(s, "set-desync");
        },
};

void subcompositor_get_subsurface(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t id, struct wl_resource* surface_res,
                                  struct wl_resource* parent_res) {
    surface_obj* s = surface_from(surface_res);
    surface_obj* p = surface_from(parent_res);
    if (!s || !p || !s->ctx || !s->ctx->tree)
        return;
    // "The to-be sub-surface must not already have another role, and it must not have an
    // existing wl_subsurface object."
    if ((s->role != surface_role::none && s->role != surface_role::subsurface) || s->sub) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u already has a role or a wl_subsurface",
                               wl_resource_get_id(surface_res));
        return;
    }
    subsurface_obj* o = nullptr;
    for (auto& slot : subsurfaces)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    using link = lx::compositor::surface_tree::link_error;
    switch (s->ctx->tree->link(s->id, p->id)) {
    case link::none:
        break;
    case link::bad_surface:
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u cannot be a sub-surface",
                               wl_resource_get_id(surface_res));
        return;
    case link::bad_parent:
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                               "parent wl_surface@%u is the surface itself or one of its "
                               "descendants",
                               wl_resource_get_id(parent_res));
        return;
    case link::no_memory:
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res = wl_resource_create(client, &wl_subsurface_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        s->ctx->tree->unlink(s->id);
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->surface = s;
    s->sub = o;
    s->role = surface_role::subsurface;
    wl_resource_set_implementation(res, &subsurface_impl, o, subsurface_destroy_resource);
    if (surface_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "subsurface create surface=%llu parent=%llu root=%llu",
                      s->id.id(), p->id.id(), s->ctx->tree->root_of(s->id).id());
        surface_log(line);
    }
}

const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .get_subsurface = subcompositor_get_subsurface,
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
    // Only at client teardown (the request is `orphaned` otherwise): the decoration object
    // goes inert.
    if (t->deco)
        t->deco->toplevel = nullptr;
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
    // Only reachable with a live toplevel or popup at client disconnect (the destroy
    // request rejects it otherwise). The record cannot outlive its xdg_surface.
    if (xs->toplevel) {
        if (xs->toplevel->id && xs->ctx && xs->ctx->wm)
            xs->ctx->wm->on_destroyed(xs->toplevel->id);
        xs->toplevel->id = {};
        xs->toplevel->xdg_surface = nullptr;
    }
    if (xs->popup) {
        if (xs->popup->surface && xs->ctx && xs->ctx->popups)
            xs->ctx->popups->destroy(xs->popup->surface);
        xs->popup->surface = {};
        xs->popup->xdg_surface = nullptr;
    }
    if (xs->surface) {
        xs->surface->xdg = nullptr;
        xs->surface->toplevel = {};
    }
    if (xs->wm_base && xs->wm_base->surfaces > 0)
        --xs->wm_base->surfaces;
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
        [](struct wl_client* client, struct wl_resource* resource) {
            // "The xdg_toplevel_decoration object must be destroyed before its xdg_toplevel."
            if (auto* t = toplevel_of(resource);
                t && t->deco && t->deco->resource && !client_dying(client)) {
                wl_resource_post_error(t->deco->resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
                                       "xdg_toplevel destroyed before its decoration object");
                return;
            }
            wl_resource_destroy(resource);
        },
    .set_parent = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .set_title =
        [](struct wl_client*, struct wl_resource* resource, const char* title) {
            auto* t = toplevel_of(resource);
            if (!t)
                return;
            std::snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
            if (t->id && t->ctx && t->ctx->toplevels) {
                t->ctx->toplevels->on_xdg_title(t->id, t->title);
                if (t->ctx->wm)
                    t->ctx->wm->on_title_changed(t->id);
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
    .set_maximized =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_maximized(t->id, true, "client");
        },
    .unset_maximized =
        [](struct wl_client*, struct wl_resource* resource) {
            if (auto* t = toplevel_of(resource); t && t->id && t->ctx && t->ctx->wm)
                t->ctx->wm->set_maximized(t->id, false, "client");
        },
    .set_fullscreen = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .unset_fullscreen = [](struct wl_client*, struct wl_resource*) {},
    .set_minimized = [](struct wl_client*, struct wl_resource*) {},
};

xdg_surface_obj* xdg_surface_from(wl_resource* resource) {
    auto* xs = resource ? static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource))
                        : nullptr;
    return xs && xs->used && xs->resource == resource ? xs : nullptr;
}

positioner_obj* positioner_from(wl_resource* resource) {
    auto* p = resource ? static_cast<positioner_obj*>(wl_resource_get_user_data(resource))
                       : nullptr;
    return p && p->used && p->resource == resource ? p : nullptr;
}

xdg_popup_obj* popup_from(wl_resource* resource) {
    auto* o = resource ? static_cast<xdg_popup_obj*>(wl_resource_get_user_data(resource))
                       : nullptr;
    return o && o->used && o->resource == resource ? o : nullptr;
}

// ── xdg_popup ──────────────────────────────────────────────────────────────

void xdg_popup_destroy_resource(struct wl_resource* resource) {
    auto* o = popup_from(resource);
    if (!o)
        return;
    // Unmapped, off the grab stack (the grab returns to the parent popup if it had one),
    // keyboard focus re-resolved — all inside the popup manager.
    if (o->surface && o->ctx && o->ctx->popups)
        o->ctx->popups->destroy(o->surface);
    if (o->xdg_surface)
        o->xdg_surface->popup = nullptr;
    *o = {};
}

const struct xdg_popup_interface xdg_popup_impl = {
    .destroy =
        [](struct wl_client* client, struct wl_resource* resource) {
            // "The only popup you are allowed to destroy at all times is the topmost one."
            auto* o = popup_from(resource);
            if (o && o->surface && o->ctx && o->ctx->popups &&
                o->ctx->popups->has_live_children(o->surface) && !client_dying(client)) {
                if (wl_resource* wm = wm_base_resource(o->xdg_surface)) {
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                                           "xdg_popup destroyed while it has child popups");
                    return;
                }
            }
            wl_resource_destroy(resource);
        },
    .grab =
        [](struct wl_client* client, struct wl_resource* resource, struct wl_resource*,
           uint32_t serial) {
            auto* o = popup_from(resource);
            if (!o || !o->surface || !o->ctx || !o->ctx->wm)
                return;
            using grab = lx::compositor::popup_manager::grab_result;
            switch (o->ctx->wm->request_popup_grab(o->surface, client, serial)) {
            case grab::invalid_grab:
                wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB,
                                       "grab requested on a mapped popup");
                break;
            case grab::invalid_parent:
                // The spec says "an error will be raised" and names none; this is the
                // one that describes it (recorded in docs/architecture.md).
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                           "grabbing popup whose parent popup took no grab");
                break;
            case grab::not_topmost:
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                                           "grabbing popup whose parent is not the topmost "
                                           "grabbing popup");
                break;
            case grab::ok:
            case grab::denied:
                break;
            }
        },
    .reposition =
        [](struct wl_client*, struct wl_resource* resource, struct wl_resource* positioner,
           uint32_t token) {
            auto* o = popup_from(resource);
            if (!o)
                return;
            const positioner_obj* p = positioner_from(positioner);
            if (!p || !p->rules.complete()) {
                if (wl_resource* wm = wm_base_resource(o->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                                           "reposition with an incomplete xdg_positioner");
                return;
            }
            if (o->surface && o->ctx && o->ctx->popups)
                o->ctx->popups->reposition(o->surface, p->rules, token);
        },
};

void popup_send_configure(void* popup_res, void* xdg_surface_res, lx::rect2i rel,
                          unsigned serial, bool repositioned, unsigned token) {
    auto* p = static_cast<wl_resource*>(popup_res);
    auto* xs = static_cast<wl_resource*>(xdg_surface_res);
    if (!p || !xs)
        return;
    if (repositioned && wl_resource_get_version(p) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
        xdg_popup_send_repositioned(p, token);
    xdg_popup_send_configure(p, rel.x, rel.y, rel.width, rel.height);
    xdg_surface_send_configure(xs, serial);
}

void popup_send_done(void* popup_res) {
    if (popup_res)
        xdg_popup_send_popup_done(static_cast<wl_resource*>(popup_res));
}

bool popup_client_alive(void* client) { return !client_dying(static_cast<wl_client*>(client)); }

/// xdg_surface.get_popup.
void xdg_surface_get_popup(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                           struct wl_resource* parent_res, struct wl_resource* positioner_res) {
    auto* xs = xdg_surface_from(resource);
    if (!xs || !xs->ctx || !xs->ctx->popups)
        return;
    if (xs->toplevel || xs->popup) {
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
    wl_resource* wm = wm_base_resource(xs);
    if (surf->role != surface_role::none && surf->role != surface_role::popup) {
        if (wm)
            wl_resource_post_error(wm, XDG_WM_BASE_ERROR_ROLE, "wl_surface already has a role");
        return;
    }
    const positioner_obj* pos = positioner_from(positioner_res);
    if (!pos || !pos->rules.complete()) {
        if (wm)
            wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                                   "get_popup with an incomplete xdg_positioner");
        return;
    }
    using kind = lx::compositor::popup_parent_kind;
    kind parent_kind = kind::none;
    lx::surface_id parent{};
    lx::toplevel_id owner{};
    if (parent_res) {
        // The parent is an xdg_surface with the toplevel or popup role, and not this one.
        const xdg_surface_obj* px = xdg_surface_from(parent_res);
        if (px && px != xs && px->surface && px->toplevel && px->toplevel->id) {
            parent_kind = kind::toplevel;
            parent = px->surface->id;
            owner = px->toplevel->id;
        } else if (px && px != xs && px->surface && px->popup && px->popup->surface) {
            parent_kind = kind::popup;
            parent = px->surface->id;
            if (const auto* pr = xs->ctx->popups->find(parent))
                owner = pr->owner;
        } else {
            if (wm)
                wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                       "popup parent is not an xdg_toplevel or xdg_popup");
            return;
        }
    }
    xdg_popup_obj* o = nullptr;
    for (auto& slot : xdg_popups)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res =
        wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->ctx = xs->ctx;
    o->xdg_surface = xs;
    o->surface = surf->id;
    wl_resource_set_implementation(res, &xdg_popup_impl, o, xdg_popup_destroy_resource);
    xs->popup = o;
    surf->role = surface_role::popup;
    using created = lx::compositor::popup_manager::create_error;
    if (xs->ctx->popups->create(surf->id, surf->client, client, surf->resource, xs->resource,
                                res, parent_kind, parent, owner, pos->rules) != created::none) {
        o->surface = {};
        wl_client_post_no_memory(client);
    }
}

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (xs && (xs->toplevel || xs->popup)) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                                       "xdg_surface destroyed before its role object");
                return;
            }
            wl_resource_destroy(resource);
        },
    .get_toplevel =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs || !xs->used || !xs->ctx)
                return;
            if (xs->toplevel || xs->popup) {
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
                if (wl_resource* wm = wm_base_resource(xs))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_ROLE,
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
            // The surface roots a tree: its subsurfaces are drawn in its container.
            if (xs->ctx->tree && !xs->ctx->tree->make_root(surf->id,
                                                           lx::compositor::root_kind::toplevel)) {
                wl_client_post_no_memory(client);
                return;
            }
        },
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry =
        [](struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y, int32_t width,
           int32_t height) {
            if (width <= 0 || height <= 0) {
                wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                                       "window geometry must have a positive size");
                return;
            }
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (!xs)
                return;
            if (xs->popup && xs->popup->surface && xs->ctx && xs->ctx->popups) {
                if (auto* rec = xs->ctx->popups->find(xs->popup->surface)) {
                    rec->window_geometry_pending = {x, y, width, height};
                    rec->window_geometry_pending_set = true;
                }
                return;
            }
            if (!xs->toplevel)
                return;
            if (auto* rec = record_of(xs->toplevel)) {
                rec->window_geometry_pending = {x, y, width, height};
                rec->window_geometry_pending_set = true;
            }
        },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial) {
            auto* xs = static_cast<xdg_surface_obj*>(wl_resource_get_user_data(resource));
            if (xs && xs->popup) {
                auto* popups = xs->ctx ? xs->ctx->popups : nullptr;
                if (!popups || !xs->popup->surface)
                    return; // inert: the wl_surface is gone
                using ack = lx::compositor::popup_manager::ack_result;
                if (popups->on_ack(xs->popup->surface, serial) == ack::invalid)
                    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                                           "ack_configure with a serial never sent");
                return;
            }
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

// ── xdg_positioner ─────────────────────────────────────────────────────────

positioner_obj* positioner_rules_of(wl_resource* resource) { return positioner_from(resource); }

const struct xdg_positioner_interface positioner_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_size =
        [](struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
            if (w < 1 || h < 1) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                                       "set_size: width and height must be positive");
                return;
            }
            if (auto* p = positioner_rules_of(r)) {
                p->rules.size = {w, h};
                p->rules.size_set = true;
            }
        },
    .set_anchor_rect =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
            if (w < 0 || h < 0) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT,
                                       "set_anchor_rect: negative width or height");
                return;
            }
            if (auto* p = positioner_rules_of(r)) {
                p->rules.anchor_rect = {x, y, w, h};
                p->rules.anchor_rect_set = true;
            }
        },
    .set_anchor =
        [](struct wl_client*, struct wl_resource* r, uint32_t anchor) {
            if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor");
                return;
            }
            if (auto* p = positioner_rules_of(r))
                p->rules.anchor = anchor;
        },
    .set_gravity =
        [](struct wl_client*, struct wl_resource* r, uint32_t gravity) {
            if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) {
                wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity");
                return;
            }
            if (auto* p = positioner_rules_of(r))
                p->rules.gravity = gravity;
        },
    .set_constraint_adjustment =
        [](struct wl_client*, struct wl_resource* r, uint32_t adjustment) {
            // Unknown bits carry no meaning and the spec defines no error for them.
            if (auto* p = positioner_rules_of(r))
                p->rules.adjustment = adjustment & 63u;
        },
    .set_offset =
        [](struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
            if (auto* p = positioner_rules_of(r))
                p->rules.offset = {x, y};
        },
    .set_reactive =
        [](struct wl_client*, struct wl_resource* r) {
            if (auto* p = positioner_rules_of(r))
                p->rules.reactive = true;
        },
    .set_parent_size =
        [](struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
            if (auto* p = positioner_rules_of(r)) {
                p->rules.parent_size = {w, h};
                p->rules.parent_size_set = true;
            }
        },
    .set_parent_configure =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            if (auto* p = positioner_rules_of(r)) {
                p->rules.parent_configure = serial;
                p->rules.parent_configure_set = true;
            }
        },
};

// ── xdg_wm_base ────────────────────────────────────────────────────────────

xdg_wm_base_obj* wm_base_from(wl_resource* r) {
    auto* o = static_cast<xdg_wm_base_obj*>(wl_resource_get_user_data(r));
    return o && o->used && o->resource == r ? o : nullptr;
}

void wm_base_destroy_resource(wl_resource* r) {
    auto* o = wm_base_from(r);
    if (!o)
        return;
    // Only with surfaces alive at client teardown: they forget where to post errors.
    if (o->surfaces > 0)
        for (auto& xs : xdg_surfaces)
            if (xs.used && xs.wm_base == o)
                xs.wm_base = nullptr;
    *o = {};
}

const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) {
            if (const auto* o = wm_base_from(resource); o && o->surfaces > 0) {
                wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                                       "xdg_wm_base destroyed with xdg_surfaces alive");
                return;
            }
            wl_resource_destroy(resource);
        },
    .create_positioner =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            positioner_obj* p = nullptr;
            for (auto& slot : positioners)
                if (!slot.used) {
                    p = &slot;
                    break;
                }
            if (!p) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource* res = wl_resource_create(client, &xdg_positioner_interface,
                                                  wl_resource_get_version(resource), id);
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *p = {};
            p->used = true;
            p->resource = res;
            wl_resource_set_implementation(res, &positioner_impl, p, [](wl_resource* r) {
                if (auto* o = positioner_from(r))
                    *o = {};
            });
        },
    .get_xdg_surface =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* surface_res) {
            auto* base = wm_base_from(resource);
            if (!base)
                return;
            surface_obj* surf = surface_from(surface_res);
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
            xs->ctx = base->ctx;
            xs->surface = surf;
            xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                              wl_resource_get_version(resource), id);
            if (!xs->resource) {
                xs->used = false;
                wl_client_post_no_memory(client);
                return;
            }
            xs->wm_base = base;
            ++base->surfaces;
            surf->xdg = xs;
            wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                           xdg_surface_destroy_resource);
        },
    .pong = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

// ── xdg-decoration ─────────────────────────────────────────────────────────

decoration_obj* decoration_from(wl_resource* r) {
    auto* d = r ? static_cast<decoration_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return d && d->used && d->resource == r ? d : nullptr;
}

void deco_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
}

void decoration_destroy_resource(wl_resource* r) {
    auto* d = decoration_from(r);
    if (!d)
        return;
    if (d->toplevel) {
        d->toplevel->deco = nullptr;
        if (auto* rec = record_of(d->toplevel)) {
            // "Switch back to a mode without any server-side decorations at the next
            // commit": nothing to send it on, so nothing is configured. Every commit while
            // no decoration object exists applies client-side (window_manager), whatever
            // the configure it answers carried, and no later configure says server-side.
            rec->deco_resource = nullptr;
            rec->deco_requested = lx::compositor::decoration_request::unset;
            rec->deco_configure_due = false;
            rec->deco_awaiting_configure = false;
            rec->desired.decoration = lx::compositor::decoration_mode::client;
            rec->last_sent.decoration = lx::compositor::decoration_mode::client;
            if (surface_debug()) {
                char line[64];
                std::snprintf(line, sizeof(line), "decoration destroy id=%llu", rec->id.id());
                deco_log(line);
            }
        }
    }
    *d = {};
}

/// set_mode / unset_mode: what the client prefers; the policy answers with a configure.
void decoration_set_request(wl_resource* resource, lx::compositor::decoration_request req) {
    auto* d = decoration_from(resource);
    auto* rec = d && d->toplevel ? record_of(d->toplevel) : nullptr;
    if (!rec)
        return; // inert
    rec->deco_requested = req;
    if (surface_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "decoration request id=%llu mode=%s", rec->id.id(),
                      req == lx::compositor::decoration_request::server   ? "server"
                      : req == lx::compositor::decoration_request::client ? "client"
                                                                          : "unset");
        deco_log(line);
    }
    if (d->ctx && d->ctx->wm)
        d->ctx->wm->on_decoration_request(rec->id, false);
}

const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_mode =
        [](struct wl_client*, struct wl_resource* r, uint32_t mode) {
            if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
                mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
                wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
                                       "invalid decoration mode %u", mode);
                return;
            }
            decoration_set_request(r, mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                      ? lx::compositor::decoration_request::server
                                      : lx::compositor::decoration_request::client);
        },
    .unset_mode =
        [](struct wl_client*, struct wl_resource* r) {
            decoration_set_request(r, lx::compositor::decoration_request::unset);
        },
};

const struct zxdg_decoration_manager_v1_interface decoration_manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_toplevel_decoration =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource* toplevel_res) {
            const int version = wl_resource_get_version(resource);
            decoration_obj* d = nullptr;
            for (auto& slot : toplevel_decorations)
                if (!slot.used) {
                    d = &slot;
                    break;
                }
            wl_resource* res =
                d ? wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, version, id)
                  : nullptr;
            if (!res) {
                wl_client_post_no_memory(client);
                return;
            }
            *d = {};
            d->used = true;
            d->resource = res;
            d->ctx = g_p0_ctx;
            wl_resource_set_implementation(res, &decoration_impl, d, decoration_destroy_resource);

            // Errors are posted on the new object: the error enum is
            // zxdg_toplevel_decoration_v1's, so that is the interface a client decodes the
            // code against.
            xdg_toplevel_obj* t = toplevel_of(toplevel_res);
            if (t && t->deco) {
                wl_resource_post_error(res, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                                       "xdg_toplevel already has a decoration object");
                return;
            }
            auto* rec = record_of(t);
            if (!t || !rec)
                return; // inert: the toplevel's record is gone
            const surface_obj* surf = t->xdg_surface ? t->xdg_surface->surface : nullptr;
            const bool has_buffer =
                surf && (surf->has_content || (surf->pending.buffer_set && surf->pending.buffer) ||
                         (surf->cached.buffer_set && surf->cached.buffer));
            if (version < 2 && has_buffer) {
                wl_resource_post_error(res, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                                       "xdg_toplevel has a buffer attached or committed");
                return;
            }
            d->toplevel = t;
            t->deco = d;
            rec->deco_resource = res;
            rec->deco_version = static_cast<unsigned>(version);
            rec->deco_requested = lx::compositor::decoration_request::unset;
            // Created again before a commit: the previous mode is retained (v2 text) — the
            // switch back to client-side, decided at commit by the missing object, is
            // called off.
            rec->deco_awaiting_configure = version < 2;
            if (surface_debug()) {
                char line[96];
                std::snprintf(line, sizeof(line), "decoration create id=%llu version=%d",
                              rec->id.id(), version);
                deco_log(line);
            }
            if (g_p0_ctx && g_p0_ctx->wm)
                g_p0_ctx->wm->on_decoration_request(rec->id, true);
        },
};

void bind_decoration_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zxdg_decoration_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&decoration_manager_impl, g_p0_ctx, nullptr);
}

// ── wlr-layer-shell ────────────────────────────────────────────────────────
//
// Wire glue only: records, the configure flow, arrangement, exclusive zones and keyboard
// interactivity live in `layer_shell`; the commit rules in `layer_prechecks`.

layer_surface_obj* layer_surface_from(wl_resource* r) {
    auto* o = r ? static_cast<layer_surface_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return o && o->used && o->resource == r ? o : nullptr;
}

/// The layer record of a zwlr_layer_surface_v1 while its wl_surface lives; null when inert.
lx::compositor::layer_record* layer_record_of(wl_resource* r) {
    auto* o = layer_surface_from(r);
    if (!o || !o->surface || !o->ctx || !o->ctx->layers)
        return nullptr;
    return o->ctx->layers->find(o->surface->id);
}

void layer_surface_destroy_resource(wl_resource* r) {
    auto* o = layer_surface_from(r);
    if (!o)
        return;
    // The role object going away unmaps the surface (its popups first) and frees the
    // record; the wl_surface keeps the layer role, and a new layer surface may follow.
    if (o->surface) {
        if (o->ctx && o->ctx->layers)
            o->ctx->layers->destroy(o->surface->id);
        o->surface->layer = nullptr;
    }
    *o = {};
}

[[nodiscard]] int layer_clamp_size(uint32_t v) {
    return v > (1u << 30) ? (1 << 30) : static_cast<int>(v);
}

const struct zwlr_layer_surface_v1_interface layer_surface_impl = {
    .set_size =
        [](struct wl_client*, struct wl_resource* r, uint32_t width, uint32_t height) {
            if (auto* rec = layer_record_of(r))
                rec->pending.size = {layer_clamp_size(width), layer_clamp_size(height)};
        },
    .set_anchor =
        [](struct wl_client*, struct wl_resource* r, uint32_t anchor) {
            constexpr uint32_t all = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
            if (anchor > all) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                                       "invalid anchor %u", anchor);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.anchor = anchor;
        },
    .set_exclusive_zone =
        [](struct wl_client*, struct wl_resource* r, int32_t zone) {
            if (auto* rec = layer_record_of(r))
                rec->pending.zone = zone;
        },
    .set_margin =
        [](struct wl_client*, struct wl_resource* r, int32_t top, int32_t right, int32_t bottom,
           int32_t left) {
            if (auto* rec = layer_record_of(r)) {
                rec->pending.margin_top = top;
                rec->pending.margin_right = right;
                rec->pending.margin_bottom = bottom;
                rec->pending.margin_left = left;
            }
        },
    .set_keyboard_interactivity =
        [](struct wl_client*, struct wl_resource* r, uint32_t k) {
            // on_demand exists since version 4; anything past it never did.
            if (k > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND ||
                (k == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
                 wl_resource_get_version(r) <
                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION)) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                                       "invalid keyboard interactivity %u", k);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.interactivity = k;
        },
    .get_popup =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* popup_res) {
            auto* rec = layer_record_of(r);
            xdg_popup_obj* po = popup_from(popup_res);
            if (!rec || !po || !po->surface || !po->ctx || !po->ctx->popups)
                return;
            // "This popup should have been created via xdg_surface::get_popup with the
            // parent set to NULL, and this request must be invoked before committing the
            // popup's initial state." The spec names no error; invalid_popup_parent is the
            // one that describes it (recorded in docs/architecture.md).
            if (!po->ctx->popups->set_layer_parent(po->surface, rec->surface)) {
                if (wl_resource* wm = wm_base_resource(po->xdg_surface))
                    wl_resource_post_error(wm, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                           "layer_surface.get_popup on a popup that has a parent "
                                           "or made its initial commit");
            }
        },
    .ack_configure =
        [](struct wl_client*, struct wl_resource* r, uint32_t serial) {
            auto* o = layer_surface_from(r);
            if (!o || !o->surface || !o->ctx || !o->ctx->layers)
                return;
            using ack = lx::compositor::layer_shell::ack_result;
            if (o->ctx->layers->on_ack(o->surface->id, serial) == ack::invalid)
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                                       "ack_configure with a serial never sent");
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_layer =
        [](struct wl_client*, struct wl_resource* r, uint32_t layer) {
            // The spec's enum is zwlr_layer_shell_v1.layer, and so is the error wlroots posts
            // here — on the layer surface.
            if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
                wl_resource_post_error(r, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                                       "invalid layer %u", layer);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.layer = layer;
        },
    .set_exclusive_edge =
        [](struct wl_client*, struct wl_resource* r, uint32_t edge) {
            // One edge or none; whether it is an anchored one is checked at commit.
            if (edge != 0 && (edge > ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT || (edge & (edge - 1)) != 0)) {
                wl_resource_post_error(r, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                                       "exclusive edge %u is not a single edge", edge);
                return;
            }
            if (auto* rec = layer_record_of(r))
                rec->pending.exclusive_edge = edge;
        },
};

void layer_send_configure(void* layer_res, unsigned serial, int width, int height) {
    if (layer_res)
        zwlr_layer_surface_v1_send_configure(static_cast<wl_resource*>(layer_res), serial,
                                             static_cast<uint32_t>(width > 0 ? width : 0),
                                             static_cast<uint32_t>(height > 0 ? height : 0));
}

void layer_send_closed(void* layer_res) {
    if (layer_res)
        zwlr_layer_surface_v1_send_closed(static_cast<wl_resource*>(layer_res));
}

void layer_shell_get_layer_surface(struct wl_client* client, struct wl_resource* resource,
                                   uint32_t id, struct wl_resource* surface_res,
                                   struct wl_resource* /*output: output 0 until D5*/,
                                   uint32_t layer, const char* name_space) {
    surface_obj* s = surface_from(surface_res);
    if (!s || !s->ctx || !s->ctx->layers)
        return;
    // wlroots' order: the layer value, then the role, then the buffer.
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer %u", layer);
        return;
    }
    if ((s->role != surface_role::none && s->role != surface_role::layer) || s->layer) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                               "wl_surface@%u already has another role",
                               wl_resource_get_id(surface_res));
        return;
    }
    // "Creating a layer surface from a wl_surface which has a buffer attached or committed
    // is a client error."
    if (s->has_content || (s->pending.buffer_set && s->pending.buffer)) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                               "wl_surface@%u has a buffer attached or committed",
                               wl_resource_get_id(surface_res));
        return;
    }
    layer_surface_obj* o = nullptr;
    for (auto& slot : layer_surfaces)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource* res = wl_resource_create(client, &zwlr_layer_surface_v1_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    using created = lx::compositor::layer_shell::create_error;
    if (s->ctx->layers->create(s->id, s->client, client, s->resource, res,
                               static_cast<unsigned>(wl_resource_get_version(resource)), layer,
                               name_space) != created::none) {
        wl_resource_destroy(res);
        wl_client_post_no_memory(client);
        return;
    }
    *o = {};
    o->used = true;
    o->resource = res;
    o->ctx = s->ctx;
    o->surface = s;
    s->layer = o;
    s->role = surface_role::layer;
    wl_resource_set_implementation(res, &layer_surface_impl, o, layer_surface_destroy_resource);
}

const struct zwlr_layer_shell_v1_interface layer_shell_impl = {
    .get_layer_surface = layer_shell_get_layer_surface,
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void bind_layer_shell(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &zwlr_layer_shell_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&layer_shell_impl, g_p0_ctx, nullptr);
}

// ── window manager wire ops ────────────────────────────────────────────────

void wm_send_configure(void* toplevel_res, void* xdg_surface_res,
                       const lx::compositor::toplevel_configure& c, lx::size2i bounds,
                       bool initial, void* decoration_res) {
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

    // Maximize only: Lumen has no window menu, fullscreen or minimize yet, and a CSD client
    // hides the buttons it is told the compositor does not honour.
    if (initial && version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
        storage[0] = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
        array.size = sizeof(uint32_t);
        xdg_toplevel_send_wm_capabilities(tl, &array);
        array.size = 0;
    }
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
    if (decoration_res)
        zxdg_toplevel_decoration_v1_send_configure(
            static_cast<wl_resource*>(decoration_res),
            c.decoration == lx::compositor::decoration_mode::server
                ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
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
    xdg_wm_base_obj* o = nullptr;
    for (auto& slot : xdg_wm_bases)
        if (!slot.used) {
            o = &slot;
            break;
        }
    if (!o)
        return;
    auto res = wayland::resource::create(client, &xdg_wm_base_interface, version, id);
    if (!res)
        return;
    *o = {};
    o->used = true;
    o->ctx = g_p0_ctx;
    o->resource = static_cast<wl_resource*>(res.native());
    wl_resource_set_implementation(o->resource, &xdg_wm_base_impl, o, wm_base_destroy_resource);
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

/// The formats the compositor imports, as (fourcc, modifier) — what the legacy modifier
/// events announce to v3 clients, and the format table to v4+ ones.
struct dmabuf_format_entry {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};
constexpr dmabuf_format_entry k_dmabuf_formats[] = {
    {DRM_FORMAT_ARGB8888, 0, 0}, // DRM_FORMAT_MOD_LINEAR
    {DRM_FORMAT_XRGB8888, 0, 0},
};

/// The format table (linux-dmabuf v4): one sealed memfd for the process, duplicated by
/// libwayland on every send. -1 when it cannot be made.
int dmabuf_format_table_fd() {
    static int fd = -2;
    if (fd != -2)
        return fd;
    fd = -1;
#if !defined(_WIN32)
    const int memfd = ::memfd_create("lumen-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0)
        return fd;
    const auto size = static_cast<ssize_t>(sizeof(k_dmabuf_formats));
    if (::write(memfd, k_dmabuf_formats, sizeof(k_dmabuf_formats)) != size) {
        ::close(memfd);
        return fd;
    }
    (void)::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    fd = memfd;
#endif
    return fd;
}

/// zwp_linux_dmabuf_feedback_v1: the format table, the main device, one tranche for it
/// with every format, then done — the sequence the protocol requires. Sending `done`
/// without a format table (as this did) crashes GTK 4 at startup: it reads the table.
void create_dmabuf_feedback(struct wl_client* client, struct wl_resource* resource, uint32_t id) {
    wl_resource* res = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct zwp_linux_dmabuf_feedback_v1_interface impl = {
        .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    };
    wl_resource_set_implementation(res, &impl, nullptr, nullptr);

    const int table = dmabuf_format_table_fd();
    if (table >= 0)
        zwp_linux_dmabuf_feedback_v1_send_format_table(res, table, sizeof(k_dmabuf_formats));

    // Arrays over stack storage, never released (wl_array_add would allocate).
    uint64_t device = g_p0_ctx ? g_p0_ctx->dmabuf_main_device : 0;
    wl_array dev{};
    dev.data = &device;
    dev.size = sizeof(device);
    dev.alloc = sizeof(device);
    zwp_linux_dmabuf_feedback_v1_send_main_device(res, &dev);

    if (table >= 0) {
        uint16_t indices[sizeof(k_dmabuf_formats) / sizeof(k_dmabuf_formats[0])];
        for (uint16_t i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i)
            indices[i] = i;
        wl_array formats{};
        formats.data = indices;
        formats.size = sizeof(indices);
        formats.alloc = sizeof(indices);
        zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(res, &dev);
        zwp_linux_dmabuf_feedback_v1_send_tranche_flags(res, 0);
        zwp_linux_dmabuf_feedback_v1_send_tranche_formats(res, &formats);
        zwp_linux_dmabuf_feedback_v1_send_tranche_done(res);
    }
    zwp_linux_dmabuf_feedback_v1_send_done(res);
}

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
            create_dmabuf_feedback(client, resource, id);
        },
    .get_surface_feedback =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id,
           struct wl_resource*) {
            // One output, one device: a surface's feedback is the default one.
            create_dmabuf_feedback(client, resource, id);
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
    if (!native)
        return;
    // The legacy events: modifier for v3, format before it. From v4 on they "must not be
    // sent" — the client asks for feedback objects instead.
    for (const auto& f : k_dmabuf_formats) {
        if (version >= 4)
            break;
        if (version == 3)
            zwp_linux_dmabuf_v1_send_modifier(native, f.format,
                                              static_cast<uint32_t>(f.modifier >> 32),
                                              static_cast<uint32_t>(f.modifier & 0xFFFFFFFFu));
        else
            zwp_linux_dmabuf_v1_send_format(native, f.format);
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
    surface_obj* surf = surface_res ? surface_from(surface_res) : nullptr;
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

/// Compositor textures (the software cursor, server-side frames): a premultiplied ARGB image
/// padded into a `canvas_w` x `canvas_h` canvas and queued like any shm texture, in the
/// order the active backend samples (native for CPU, RGBA otherwise).
bool stage_image(void* user, unsigned texture_id, const uint32_t* argb, unsigned width,
                 unsigned height, unsigned stride_px, unsigned canvas_w, unsigned canvas_h) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (!ctx || !ctx->textures || !argb || canvas_w == 0 || canvas_h == 0)
        return false;
    const unsigned bytes = canvas_w * canvas_h * 4u;
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
    for (unsigned y = 0; y < canvas_h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(out + static_cast<size_t>(y) * canvas_w * 4u);
        for (unsigned x = 0; x < canvas_w; ++x) {
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
    update.visible_from_frame = ctx->building_frame;
    update.width = canvas_w;
    update.height = canvas_h;
    update.stride = canvas_w * 4u;
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

void retire_image(void*, unsigned texture_id) { retire_texture(texture_id); }

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

/// Every bridge request is logged with what became of it (debug), so a shell's intent can
/// be followed through to the window manager's own lines:
///   inert     the handle no longer names a window (destroyed, or the bridge replaced)
///   unmapped  it names a window that is not on screen
///   ignored   nothing to do (already in that place) or not implemented yet
///   ok        executed
enum class bridge_result : unsigned char { ok, inert, unmapped, ignored };

const char* bridge_result_name(bridge_result r) {
    switch (r) {
    case bridge_result::ok: return "ok";
    case bridge_result::inert: return "inert";
    case bridge_result::unmapped: return "unmapped";
    case bridge_result::ignored: return "ignored";
    }
    return "?";
}

void bridge_log(const char* request, lx::toplevel_id id, bridge_result r) {
    if (!surface_debug())
        return;
    char line[128];
    std::snprintf(line, sizeof(line), "bridge request=%s id=%llu result=%s", request, id.id(),
                  bridge_result_name(r));
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.shell", line);
}

/// Decodes the handle and classifies it; `ok` means "a mapped window, go ahead".
bridge_result bridge_target(wl_resource* handle, lx::toplevel_id& id) {
    id = lx::compositor::shell_bridge::toplevel_from_handle(handle);
    if (!id || !bridge_wm() || !g_p0_ctx->toplevels)
        return bridge_result::inert;
    const auto* rec = g_p0_ctx->toplevels->find(id);
    if (!rec)
        return bridge_result::inert;
    return rec->mapped ? bridge_result::ok : bridge_result::unmapped;
}

/// The common shape: decode, run `fn(wm, id) -> bool` on a mapped window, log.
template<typename Fn>
void bridge_request(const char* request, wl_resource* handle, Fn&& fn) {
    lx::toplevel_id id{};
    bridge_result r = bridge_target(handle, id);
    if (r == bridge_result::ok && !fn(*bridge_wm(), id))
        r = bridge_result::ignored;
    bridge_log(request, id, r);
}

const struct zlm_policy_bridge_v1_interface policy_bridge_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .request_activate =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("activate", toplevel, [](auto& wm, lx::toplevel_id id) {
                wm.activate(id);
                return true;
            });
        },
    .request_raise =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            // Already on top is still the requested outcome: ok, not ignored.
            bridge_request("raise", toplevel, [](auto& wm, lx::toplevel_id id) {
                (void)wm.raise(id);
                return true;
            });
        },
    .request_lower =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("lower", toplevel, [](auto& wm, lx::toplevel_id id) {
                (void)wm.lower(id);
                return true;
            });
        },
    .set_toplevel_stacking_index =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel, uint32_t index) {
            bridge_request("stacking", toplevel, [index](auto& wm, lx::toplevel_id id) {
                (void)wm.restack(id, index);
                return true;
            });
        },
    // Same executor as xdg_toplevel.move/resize and the keybinding; the press serial is
    // checked but may belong to any client (normally the shell's own panel surface).
    .request_move =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource*, uint32_t serial) {
            bridge_request("move", toplevel, [serial](auto& wm, lx::toplevel_id id) {
                return wm.request_shell_grab(id, serial, lx::compositor::grab_mode::move, 0);
            });
        },
    .request_resize =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource*, uint32_t serial, uint32_t edges) {
            // The shell is trusted: an invalid edge is ignored (logged by the WM), not
            // a protocol error that would kill the desktop.
            bridge_request("resize", toplevel, [serial, edges](auto& wm, lx::toplevel_id id) {
                return wm.request_shell_grab(id, serial, lx::compositor::grab_mode::resize,
                                             edges);
            });
        },
    .request_maximize =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("maximize", toplevel, [](auto& wm, lx::toplevel_id id) {
                wm.set_maximized(id, true, "shell");
                return true;
            });
        },
    .request_unmaximize =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("unmaximize", toplevel, [](auto& wm, lx::toplevel_id id) {
                wm.set_maximized(id, false, "shell");
                return true;
            });
        },
    // No compositor-side fullscreen or minimize yet: logged as ignored, never an error.
    .request_fullscreen =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource*) {
            bridge_request("fullscreen", toplevel, [](auto&, lx::toplevel_id) { return false; });
        },
    .request_minimize =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("minimize", toplevel, [](auto&, lx::toplevel_id) { return false; });
        },
    .request_close =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel) {
            bridge_request("close", toplevel, [](auto& wm, lx::toplevel_id id) {
                wm.request_close(id);
                return true;
            });
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
                g_p0_ctx->shell->on_bridge_bound(
                    res, g_p0_ctx->wm ? g_p0_ctx->wm->keyboard_focus() : lx::toplevel_id{});
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
    // "Sent immediately after bind". No optional capability is implemented yet (tiling
    // plugin, workspace drag, explicit sync), so the flags are empty.
    zlm_shell_v1_send_capabilities(static_cast<wl_resource*>(res.native()), 0u, 1u);
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
    {
        lx::compositor::compositor_texture_ops ops{};
        ops.stage = &stage_image;
        ops.retire = &retire_image;
        ops.user = &ctx;
        if (ctx.cursor)
            ctx.cursor->set_texture_ops(ops);
        if (ctx.decorations)
            ctx.decorations->set_texture_ops(ops);
    }
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
    if (ctx.layers) {
        lx::compositor::layer_ops ops{};
        ops.send_configure = &layer_send_configure;
        ops.send_closed = &layer_send_closed;
        ops.next_serial = &wm_next_serial;
        ops.client_alive = &popup_client_alive;
        ops.user = &ctx;
        ctx.layers->set_ops(ops);
    }
    if (ctx.popups) {
        lx::compositor::popup_ops ops{};
        ops.send_configure = &popup_send_configure;
        ops.send_popup_done = &popup_send_done;
        ops.next_serial = &wm_next_serial;
        ops.client_alive = &popup_client_alive;
        ops.user = &ctx;
        ctx.popups->set_ops(ops);
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

    // Server-side decorations, unless the compositor was told to leave every client to
    // decorate itself (`--decorations=client`: exercises client-side frames of clients
    // that have no preference knob).
    if (ctx.server_side_decorations)
        ctx.server->add_global({.interface_name = "zxdg_decoration_manager_v1",
                                .version = 2,
                                .privileged = false,
                                .interface_desc = &zxdg_decoration_manager_v1_interface},
                               &bind_decoration_manager);

    // Open to every client, as in sway, KWin and niri: panels, docks, wallpapers and
    // launchers are ordinary clients.
    if (ctx.layers)
        ctx.server->add_global({.interface_name = "zwlr_layer_shell_v1",
                                .version = 5,
                                .privileged = false,
                                .interface_desc = &zwlr_layer_shell_v1_interface},
                               &bind_layer_shell);

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
