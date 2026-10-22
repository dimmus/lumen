module;

// zwp_linux_dmabuf_v1: buffer params, the format table and feedback. Part of the
// protocol wire split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#endif
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "linux-dmabuf-v1-server-protocol.h"
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

import std;

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
#if defined(LUMEN_HAS_PROTOCOL_GLUE)

namespace lx::compositor::p0 {

namespace {

dmabuf_params_obj dmabuf_params[128]{};

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

} // namespace

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

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_PROTOCOL_GLUE
#endif // LUMEN_HAS_WAYLAND
