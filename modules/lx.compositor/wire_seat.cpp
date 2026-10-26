module;

// wl_seat, wl_pointer (set_cursor), wl_keyboard (keymap) and wl_touch. Event delivery
// lives in input_router. Part of the protocol wire split; shared declarations live in
// the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
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

namespace lx::compositor::p0 {

namespace {

// ── wl_seat ────────────────────────────────────────────────────────────────

/// libwayland dups the keymap FD when marshalling — `-1` always fails with EBADF.
/// Cache one sealed memfd for the process lifetime; each send_keymap dups it again.
struct keymap_source {
    int fd = -1;
    uint32_t size = 0;
};

/// The cached keymap fd: replaced when a configuration reload compiles a new keymap
/// (`seat_keymap_invalidate`); resources it was sent to hold their own dups.
[[nodiscard]] keymap_source& shared_keymap_slot() {
    static keymap_source cached{};
    return cached;
}

[[nodiscard]] keymap_source& shared_keymap() {
    keymap_source& cached = shared_keymap_slot();
    if (cached.fd >= 0)
        return cached;

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
    return cached;
}

} // namespace

void seat_keymap_invalidate() {
    keymap_source& k = shared_keymap_slot();
    if (k.fd >= 0)
        ::close(k.fd);
    k = keymap_source{};
}

bool seat_keymap(int& fd, unsigned& size) {
    const keymap_source& k = shared_keymap();
    fd = k.fd;
    size = k.size;
    return k.fd >= 0;
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

namespace {

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
                wl_keyboard_send_repeat_info(res, g_p0_ctx ? g_p0_ctx->repeat_rate : 25,
                                             g_p0_ctx ? g_p0_ctx->repeat_delay : 600);

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

} // namespace

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

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
