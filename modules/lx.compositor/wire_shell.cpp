module;

// zlm_shell_v1 (privileged) and the zlm_policy_bridge_v1 requests. Part of the protocol
// wire split; shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>
#include <cstdio>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "zlm_shell_v1-server-protocol.h"
#endif
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

} // namespace

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
    // shell_protocol_version: the zlm family's version this compositor implements (2:
    // toplevel_flags).
    zlm_shell_v1_send_capabilities(static_cast<wl_resource*>(res.native()), 0u, 2u);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
