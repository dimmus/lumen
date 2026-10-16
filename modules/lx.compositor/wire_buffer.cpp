module;

// wl_buffer tracking (shm and dma-buf), texture retirement and shm staging, plus the
// staging hook for compositor-drawn textures (cursor, server-side frames). Part of the
// protocol wire split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
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
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

constexpr unsigned k_max_buffers = 512;

buffer_obj buffers[k_max_buffers]{};
shm_buffer_listener shm_listeners[k_max_buffers]{};

} // namespace

buffer_obj* alloc_buffer() {
    for (auto& b : buffers)
        if (!b.used) {
            b = {};
            b.used = true;
            return &b;
        }
    return nullptr;
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

namespace {

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

} // namespace

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

lx::buffer_id next_buffer_id() {
    static unsigned n = 1;
    return lx::buffer_id{n++};
}

namespace {

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

} // namespace

/// Destructor of the dma-buf wl_buffers this file creates itself.
void buffer_destroy_resource(struct wl_resource* resource) {
    auto* b = static_cast<buffer_obj*>(wl_resource_get_user_data(resource));
    buffer_gone(resource, b && b->used && b->resource == resource ? b : nullptr);
}

namespace {

void shm_buffer_destroyed(struct wl_listener* listener, void* data) {
    auto* l = reinterpret_cast<shm_buffer_listener*>(listener);
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    buffer_obj* b = l->owner;
    l->owner = nullptr;
    buffer_gone(static_cast<wl_resource*>(data), b);
}

} // namespace

extern const struct wl_buffer_interface buffer_impl = {
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

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
