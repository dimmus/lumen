module;

// xdg-toplevel-icon-v1 (staging): a window's icon as an XDG icon-theme name and/or wl_shm
// pixel buffers (design-app-compat-capture B.2, I1). The compositor does not draw icons: at
// set_icon it snapshots the icon — the name and up to four buffers, copied into sealed
// memfds (`toplevel_icon_table`) — stages it on the toplevel, and the toplevel's next commit
// makes it current and sends it to the shell (zlm_policy_bridge_v1 v6 toplevel_icon …). The
// shell resolves the name in its icon theme and falls back to the pixels.
//
// Every error:
//   xdg_toplevel_icon_v1.invalid_buffer  add_buffer of a buffer not backed by wl_shm, or not
//                                        square
//   xdg_toplevel_icon_v1.immutable       set_name / add_buffer after the icon was set
//   xdg_toplevel_icon_v1.no_buffer       a buffer added to the icon destroyed before the icon
//                                        was set (its pixels were never read)
// A buffer destroyed after set_icon is not an error here: its pixels were copied, and
// wlroots does not enforce no_buffer at all — posting it then would only kill clients whose
// teardown order differs (an icon object destroyed after its buffers). Formats other than
// ARGB8888/XRGB8888 are accepted, as the spec requires, and not forwarded (the name, or the
// app's desktop entry, stands in).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1)
#include "xdg-shell-server-protocol.h"
#include "xdg-toplevel-icon-v1-server-protocol.h"
#define LUMEN_HAS_TOPLEVEL_ICON 1
#endif
#endif

module lx.compositor;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :toplevel;
import :toplevel_icon;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_TOPLEVEL_ICON)

namespace {

constexpr unsigned k_icon_buffers = 8;

struct icon_obj;

/// One wl_buffer added to an icon object, watched for its destruction. Never moved while
/// linked (the listener is in the buffer's destroy signal list).
struct icon_buf {
    wl_listener destroy{}; // first member: the notify casts back to this struct
    icon_obj* owner = nullptr;
    wl_resource* buffer = nullptr;
    int size = 0;
    int scale = 1;
};

/// xdg_toplevel_icon_v1. Allocated per request, freed with its resource.
struct icon_obj {
    wl_resource* resource = nullptr;
    char name[lx::compositor::k_icon_name_bytes]{};
    icon_buf bufs[k_icon_buffers]{};
    /// Given to set_icon: no further change ("immutable from this point").
    bool immutable = false;
};

void unwatch(icon_buf& b) {
    if (!b.buffer)
        return;
    wl_list_remove(&b.destroy.link);
    b = {};
}

void buffer_destroyed(wl_listener* listener, void*) {
    auto* b = reinterpret_cast<icon_buf*>(listener);
    icon_obj* icon = b->owner;
    wl_resource* buffer = b->buffer;
    unwatch(*b);
    if (!icon || icon->immutable || !buffer)
        return;
    wl_client* client = wl_resource_get_client(icon->resource);
    if (!client_dying(client))
        wl_resource_post_error(icon->resource, XDG_TOPLEVEL_ICON_V1_ERROR_NO_BUFFER,
                               "a buffer of the icon was destroyed before the icon was set");
}

void icon_destroy_resource(wl_resource* r) {
    auto* icon = static_cast<icon_obj*>(wl_resource_get_user_data(r));
    if (!icon)
        return;
    for (auto& b : icon->bufs)
        unwatch(b);
    delete icon;
}

icon_obj* icon_from(wl_resource* r) {
    return r ? static_cast<icon_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

const struct xdg_toplevel_icon_v1_interface icon_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_name =
        [](struct wl_client*, struct wl_resource* r, const char* name) {
            icon_obj* icon = icon_from(r);
            if (!icon)
                return;
            if (icon->immutable) {
                wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                                       "the icon was already set on a toplevel");
                return;
            }
            std::snprintf(icon->name, sizeof(icon->name), "%s", name ? name : "");
        },
    .add_buffer =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* buffer, int32_t scale) {
            icon_obj* icon = icon_from(r);
            if (!icon)
                return;
            if (icon->immutable) {
                wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                                       "the icon was already set on a toplevel");
                return;
            }
            wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
            if (!shm) {
                wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
                                       "the buffer is not backed by wl_shm");
                return;
            }
            const int w = wl_shm_buffer_get_width(shm);
            const int h = wl_shm_buffer_get_height(shm);
            if (w != h || w <= 0) {
                wl_resource_post_error(r, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
                                       "the buffer is not square");
                return;
            }
            const int sc = scale > 0 ? scale : 1;
            // "If this icon instance already has a buffer of the same size and scale from a
            // previous 'add_buffer' request, data from the last request overrides" it.
            icon_buf* slot = nullptr;
            for (auto& b : icon->bufs) {
                if (b.buffer && b.size == w && b.scale == sc) {
                    unwatch(b);
                    slot = &b;
                    break;
                }
            }
            for (auto& b : icon->bufs) {
                if (slot)
                    break;
                if (!b.buffer)
                    slot = &b;
            }
            if (!slot)
                return; // eight sizes already: more cannot improve what the shell picks
            slot->owner = icon;
            slot->buffer = buffer;
            slot->size = w;
            slot->scale = sc;
            slot->destroy.notify = &buffer_destroyed;
            wl_resource_add_destroy_listener(buffer, &slot->destroy);
        },
};

/// Snapshots `icon` into the table: 0 for the default icon (null, or neither a name nor a
/// usable buffer).
unsigned snapshot(icon_obj* icon) {
    auto* table = g_p0_ctx ? g_p0_ctx->icons : nullptr;
    if (!icon || !table)
        return 0;
    lx::compositor::icon_candidate cand[k_icon_buffers]{};
    wl_shm_buffer* shm[k_icon_buffers]{};
    unsigned n = 0;
    for (auto& b : icon->bufs) {
        if (!b.buffer)
            continue;
        wl_shm_buffer* s = wl_shm_buffer_get(b.buffer);
        if (!s)
            continue;
        const uint32_t format = wl_shm_buffer_get_format(s);
        if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
            continue;
        cand[n] = {b.size, b.scale};
        shm[n] = s;
        ++n;
    }
    if (icon->name[0] == '\0' && n == 0)
        return 0;
    const unsigned id = table->create(icon->name);
    if (!id)
        return 0;
    unsigned picked[lx::compositor::k_icon_max_images]{};
    const unsigned keep = lx::compositor::select_icon_images(cand, n, table->budget_left(), picked);
    for (unsigned i = 0; i < keep; ++i) {
        wl_shm_buffer* s = shm[picked[i]];
        wl_shm_buffer_begin_access(s);
        (void)table->add_image(id, wl_shm_buffer_get_data(s),
                               static_cast<unsigned>(wl_shm_buffer_get_stride(s)),
                               cand[picked[i]].size, cand[picked[i]].scale,
                               wl_shm_buffer_get_format(s) == WL_SHM_FORMAT_XRGB8888);
        wl_shm_buffer_end_access(s);
    }
    const auto* e = table->get(id);
    if (e && e->name[0] == '\0' && e->count == 0) {
        table->unref(id);
        return 0;
    }
    return id;
}

void manager_set_icon(struct wl_client*, struct wl_resource*, struct wl_resource* toplevel_res,
                      struct wl_resource* icon_res) {
    icon_obj* icon = icon_from(icon_res);
    if (icon)
        icon->immutable = true;
    lx::compositor::toplevel_record* rec = record_of(toplevel_of(toplevel_res));
    if (!rec || !g_p0_ctx->icons)
        return;
    const unsigned id = snapshot(icon);
    if (rec->icon_pending_set)
        g_p0_ctx->icons->unref(rec->icon_pending);
    rec->icon_pending = id;
    rec->icon_pending_set = true;
    if (surface_debug()) {
        const auto* e = g_p0_ctx->icons->get(id);
        char line[384];
        std::snprintf(line, sizeof(line), "icon staged toplevel=%llu name=%s buffers=%u",
                      rec->id.id(), e ? e->name : "", e ? e->count : 0u);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.icon", line);
    }
}

const struct xdg_toplevel_icon_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .create_icon =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            auto* icon = new (std::nothrow) icon_obj{};
            wl_resource* res = icon ? wl_resource_create(client, &xdg_toplevel_icon_v1_interface,
                                                         wl_resource_get_version(r), id)
                                    : nullptr;
            if (!res) {
                delete icon;
                wl_client_post_no_memory(client);
                return;
            }
            icon->resource = res;
            wl_resource_set_implementation(res, &icon_impl, icon, icon_destroy_resource);
        },
    .set_icon = &manager_set_icon,
};

} // namespace

void bind_toplevel_icon_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res =
        wayland::resource::create(client, &xdg_toplevel_icon_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
    // The panel draws 16 px icons, a task switcher 32 (surface-local; clients add their own
    // scale's buffers).
    auto* native = static_cast<wl_resource*>(res.native());
    xdg_toplevel_icon_manager_v1_send_icon_size(native, 16);
    xdg_toplevel_icon_manager_v1_send_icon_size(native, 32);
    xdg_toplevel_icon_manager_v1_send_done(native);
}

#endif // LUMEN_HAS_TOPLEVEL_ICON

void toplevel_icon_commit(surface_obj* s) {
    if (!s || s->role != surface_role::xdg_toplevel || !s->toplevel || !s->ctx ||
        !s->ctx->toplevels || !s->ctx->icons)
        return;
    auto* rec = s->ctx->toplevels->find_mut(s->toplevel);
    if (!rec || !rec->icon_pending_set)
        return;
    s->ctx->icons->unref(rec->icon);
    rec->icon = rec->icon_pending;
    rec->icon_pending = 0;
    rec->icon_pending_set = false;
    if (rec->mapped && s->ctx->shell)
        s->ctx->shell->emit_toplevel_icon(*rec);
    if (surface_debug()) {
        const auto* e = s->ctx->icons->get(rec->icon);
        char line[384];
        std::snprintf(line, sizeof(line), "icon applied toplevel=%llu name=%s buffers=%u",
                      rec->id.id(), e ? e->name : "", e ? e->count : 0u);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.icon", line);
    }
}

void toplevel_icon_released(lx::compositor::toplevel_record& rec) {
    if (!g_p0_ctx || !g_p0_ctx->icons)
        return;
    g_p0_ctx->icons->unref(rec.icon);
    if (rec.icon_pending_set)
        g_p0_ctx->icons->unref(rec.icon_pending);
    rec.icon = 0;
    rec.icon_pending = 0;
    rec.icon_pending_set = false;
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
