module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
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
    lx::rect2i damage{};
    bool has_damage = false;
    frame_callback frame_cbs[k_max_frame_cbs]{};
    unsigned frame_cb_count = 0;
};

struct surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::surface_id id{};
    lx::client_id client{};
    surface_pending pending{};
    surface_pending current{};
    bool used = false;
    // Role
    wl_resource* xdg_surface = nullptr;
    wl_resource* xdg_toplevel = nullptr;
    lx::toplevel_id toplevel{};
};

struct region_obj {
    wl_resource* resource = nullptr;
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
    uint32_t configure_serial = 1;
    bool configured = false;
    bool used = false;
};

struct xdg_toplevel_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
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

void destroy_buffer(buffer_obj* b) {
    if (!b || !b->used)
        return;
    // Retire the GPU-side texture — but not before the render thread has finished with it.
    // Queue order alone is not enough: a snapshot published while this buffer was still
    // alive can still be waiting to composite, and it references the texture by id. Date
    // the retirement so the render thread can hold it until that snapshot has been drawn.
    //
    // The next frame is the first one guaranteed not to reference it: the surface node is
    // detached on this thread before the next `commit_frame` publishes a draw list. Being
    // one frame conservative costs a frame of texture memory; being one frame early drops
    // a texture out from under a draw.
    if (g_p0_ctx && g_p0_ctx->textures && b->id) {
        texture_update forget{};
        forget.kind = texture_update::op::forget;
        forget.texture_id = g_p0_ctx->surfaces ? g_p0_ctx->surfaces->texture_for(b->id) : 0;
        forget.retire_after_frame = g_p0_ctx->frame_index + 1;
        if (forget.texture_id != 0)
            (void)g_p0_ctx->textures->try_push(forget);
    }
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
    .add = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
    .subtract = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t) {},
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

void surface_damage(struct wl_client*, struct wl_resource* resource, int32_t x, int32_t y,
                    int32_t width, int32_t height) {
    auto* s = find_surface(resource);
    if (!s)
        return;
    s->pending.damage = {x, y, width, height};
    s->pending.has_damage = true;
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
void surface_set_input_region(struct wl_client*, struct wl_resource*, struct wl_resource*) {}

/// A client that keeps committing an unimportable buffer would otherwise log once per
/// frame on the commit hot path. Report the first failure and then periodically.
void log_commit_failure(const lx::error& err) {
    static unsigned suppressed = 0;
    static constexpr unsigned k_report_interval = 600;
    if (suppressed++ % k_report_interval == 0)
        lx::trace::logger::global().log_error(err, "compositor.surface");
}

void surface_commit(struct wl_client*, struct wl_resource* resource) {
    auto* s = find_surface(resource);
    if (!s || !s->ctx || !s->ctx->surfaces)
        return;

    // Apply pending → current
    if (s->pending.buffer_set) {
        s->current.buffer = s->pending.buffer;
        s->pending.buffer_set = false;
    }
    s->current.dx = s->pending.dx;
    s->current.dy = s->pending.dy;
    s->current.scale = s->pending.scale;
    s->current.transform = s->pending.transform;
    if (s->pending.has_damage) {
        s->current.damage = s->pending.damage;
        s->current.has_damage = true;
        s->pending.has_damage = false;
    }
    for (unsigned i = 0; i < s->pending.frame_cb_count; ++i) {
        if (s->pending.frame_cbs[i].used)
            queue_frame_callback(s->pending.frame_cbs[i].resource);
        s->pending.frame_cbs[i] = {};
    }
    s->pending.frame_cb_count = 0;

    // Buffer-less commit: still valid (state-only).
    if (!s->current.buffer) {
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
    if (s->current.has_damage)
        commit.damage = s->current.damage;
    else
        commit.damage = {0, 0, static_cast<int>(buf->width), static_cast<int>(buf->height)};

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
    if (buf->is_shm && img.image_id)
        shm_pixels = stage_shm_pixels(buf, shm, img.image_id, shm_seq, shm_token);

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
    for (auto& t : xdg_toplevels) {
        if (t.used && t.resource == resource) {
            if (t.ctx && t.ctx->toplevels && t.xdg_surface && t.xdg_surface->surface)
                t.ctx->toplevels->on_xdg_unmap(t.xdg_surface->surface->toplevel);
            t = {};
            return;
        }
    }
}

void xdg_surface_destroy_resource(struct wl_resource* resource) {
    for (auto& xs : xdg_surfaces) {
        if (xs.used && xs.resource == resource) {
            if (xs.surface)
                xs.surface->xdg_surface = nullptr;
            xs = {};
            return;
        }
    }
}

const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .set_parent = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .set_title =
        [](struct wl_client*, struct wl_resource* resource, const char* title) {
            for (auto& t : xdg_toplevels) {
                if (!t.used || t.resource != resource)
                    continue;
                std::snprintf(t.title, sizeof(t.title), "%s", title ? title : "");
                if (t.ctx && t.ctx->toplevels && t.xdg_surface && t.xdg_surface->surface)
                    t.ctx->toplevels->on_xdg_title(t.xdg_surface->surface->toplevel, t.title);
                return;
            }
        },
    .set_app_id =
        [](struct wl_client*, struct wl_resource* resource, const char* app_id) {
            for (auto& t : xdg_toplevels) {
                if (!t.used || t.resource != resource)
                    continue;
                std::snprintf(t.app_id, sizeof(t.app_id), "%s", app_id ? app_id : "");
                if (t.ctx && t.ctx->toplevels && t.xdg_surface && t.xdg_surface->surface)
                    t.ctx->toplevels->on_xdg_app_id(t.xdg_surface->surface->toplevel, t.app_id);
                return;
            }
        },
    .show_window_menu = [](struct wl_client*, struct wl_resource*, struct wl_resource*,
                           uint32_t, int32_t, int32_t) {},
    .move = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {},
    .resize = [](struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t,
                 uint32_t) {},
    .set_max_size = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
    .set_min_size = [](struct wl_client*, struct wl_resource*, int32_t, int32_t) {},
    .set_maximized = [](struct wl_client*, struct wl_resource*) {},
    .unset_maximized = [](struct wl_client*, struct wl_resource*) {},
    .set_fullscreen = [](struct wl_client*, struct wl_resource*, struct wl_resource*) {},
    .unset_fullscreen = [](struct wl_client*, struct wl_resource*) {},
    .set_minimized = [](struct wl_client*, struct wl_resource*) {},
};

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy =
        [](struct wl_client*, struct wl_resource* resource) { wl_resource_destroy(resource); },
    .get_toplevel =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            xdg_surface_obj* xs = nullptr;
            for (auto& s : xdg_surfaces)
                if (s.used && s.resource == resource) {
                    xs = &s;
                    break;
                }
            if (!xs || !xs->ctx)
                return;
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
                return;
            }
            wl_resource_set_implementation(top->resource, &xdg_toplevel_impl, top,
                                           xdg_toplevel_destroy_resource);
            if (xs->surface) {
                xs->surface->xdg_toplevel = top->resource;
                if (xs->ctx->toplevels) {
                    auto mapped = xs->ctx->toplevels->on_xdg_map(xs->surface->client,
                                                                 xs->surface->id, "");
                    if (mapped)
                        xs->surface->toplevel = mapped.value().id;
                }
            }
            // Send configure so the client can ack and commit.
            wl_array states{};
            wl_array_init(&states);
            xdg_toplevel_send_configure(top->resource, 0, 0, &states);
            wl_array_release(&states);
            xdg_surface_send_configure(xs->resource, xs->configure_serial);
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
    .set_window_geometry = [](struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t,
                              int32_t) {},
    .ack_configure =
        [](struct wl_client*, struct wl_resource* resource, uint32_t serial) {
            for (auto& xs : xdg_surfaces) {
                if (xs.used && xs.resource == resource && serial >= xs.configure_serial) {
                    xs.configured = true;
                    return;
                }
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
            xs->configure_serial = 1;
            xs->resource = wl_resource_create(client, &xdg_surface_interface,
                                              wl_resource_get_version(resource), id);
            if (!xs->resource) {
                xs->used = false;
                return;
            }
            surf->xdg_surface = xs->resource;
            wl_resource_set_implementation(xs->resource, &xdg_surface_impl, xs,
                                           xdg_surface_destroy_resource);
        },
    .pong = [](struct wl_client*, struct wl_resource*, uint32_t) {},
};

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
    // Minimal US layout; libxkbcommon on the client expands the includes.
    static constexpr char k_keymap[] =
        "xkb_keymap {\n"
        "xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
        "xkb_types { include \"complete\" };\n"
        "xkb_compat { include \"complete\" };\n"
        "xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
        "xkb_geometry { include \"pc(pc105)\" };\n"
        "};\n";

    const int fd = ::memfd_create("lumen-keymap", MFD_CLOEXEC);
    if (fd < 0)
        return cached;

    const auto nbytes = static_cast<uint32_t>(sizeof(k_keymap) - 1u);
    if (::ftruncate(fd, static_cast<off_t>(nbytes)) != 0 ||
        ::write(fd, k_keymap, nbytes) != static_cast<ssize_t>(nbytes) ||
        ::lseek(fd, 0, SEEK_SET) != 0) {
        ::close(fd);
        return cached;
    }

    cached.fd = fd;
    cached.size = nbytes;
#endif
    return cached;
}

const struct wl_seat_interface seat_impl = {
    .get_pointer =
        [](struct wl_client* client, struct wl_resource* resource, uint32_t id) {
            wl_resource* res = wl_resource_create(client, &wl_pointer_interface,
                                                 wl_resource_get_version(resource), id);
            if (!res)
                return;
            static const struct wl_pointer_interface impl = {
                .set_cursor = [](struct wl_client*, struct wl_resource*, uint32_t,
                                 struct wl_resource*, int32_t, int32_t) {},
                .release = [](struct wl_client*,
                              struct wl_resource* r) { wl_resource_destroy(r); },
            };
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
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
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);

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
            wl_resource_set_implementation(res, &impl, nullptr, nullptr);
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
    ctx.server->add_global({.interface_name = "xdg_wm_base",
                            .version = 6,
                            .privileged = false,
                            .interface_desc = &xdg_wm_base_interface},
                           &bind_xdg_wm_base);
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
