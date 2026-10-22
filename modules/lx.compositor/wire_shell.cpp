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
import :keybindings;
import :input_router;
import :session_lock;
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
    // Fullscreen on the output asked for (null: the window's own). No minimize yet: logged
    // as ignored, never an error.
    .request_fullscreen =
        [](struct wl_client*, struct wl_resource*, struct wl_resource* toplevel,
           struct wl_resource* output) {
            const int slot = output ? output_resource_slot(output) : -1;
            bridge_request("fullscreen", toplevel, [slot](auto& wm, lx::toplevel_id id) {
                wm.set_fullscreen(id, true, slot, "shell");
                return true;
            });
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

// ── zlm_key_grab_v1 (v4) ───────────────────────────────────────────────────
//
// The grab table lives in `keybindings` (Wayland-free); the resource is its owner key, and
// its destructor gives the combination back — so a dead shell's grabs go with its
// connection, and the compositor's own bindings never depended on them.

const struct zlm_key_grab_v1_interface key_grab_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void key_grab_destroyed(struct wl_resource* r) {
    if (g_p0_ctx && g_p0_ctx->keys)
        g_p0_ctx->keys->remove_grab(r);
}

const char* refusal_name(lx::compositor::key_grab_refusal r) {
    switch (r) {
    case lx::compositor::key_grab_refusal::none: return "none";
    case lx::compositor::key_grab_refusal::reserved: return "reserved";
    case lx::compositor::key_grab_refusal::taken: return "taken";
    case lx::compositor::key_grab_refusal::invalid: return "invalid";
    }
    return "?";
}

void shell_grab_key(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                    uint32_t keysym, uint32_t modifiers, uint32_t flags) {
    wl_resource* res = wl_resource_create(client, &zlm_key_grab_v1_interface,
                                          wl_resource_get_version(resource), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &key_grab_impl, g_p0_ctx, &key_grab_destroyed);
    lx::compositor::key_grab_refusal refusal = lx::compositor::key_grab_refusal::reserved;
    if (g_p0_ctx && g_p0_ctx->keys)
        refusal = g_p0_ctx->keys->add_grab(keysym, modifiers, flags, res);
    if (refusal != lx::compositor::key_grab_refusal::none)
        zlm_key_grab_v1_send_refused(res, static_cast<uint32_t>(refusal));
    if (surface_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "grab_key keysym=0x%x mods=0x%x flags=0x%x result=%s",
                      keysym, modifiers, flags, refusal_name(refusal));
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.shell", line);
    }
}

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
    .request_logout =
        [](struct wl_client*, struct wl_resource*) {
            if (g_p0_ctx && g_p0_ctx->request_logout)
                g_p0_ctx->request_logout(g_p0_ctx->logout_user, "shell");
        },
    .grab_key = &shell_grab_key,
};

} // namespace

namespace {
/// The bound shells (normally one), told when the session is ending.
constexpr unsigned k_max_shell_res = 4;
wl_resource* shell_resources[k_max_shell_res]{};
/// zlm_shell_v1.capture_state (v6) as last sent: screen capture in progress.
bool capture_active = false;
} // namespace

void shell_send_session_ending() {
    for (auto* r : shell_resources)
        if (r && wl_resource_get_version(r) >= ZLM_SHELL_V1_SESSION_ENDING_SINCE_VERSION)
            zlm_shell_v1_send_session_ending(r);
}

void shell_send_lock_state(unsigned state) {
    for (auto* r : shell_resources)
        if (r && wl_resource_get_version(r) >= ZLM_SHELL_V1_LOCK_STATE_SINCE_VERSION)
            zlm_shell_v1_send_lock_state(r, state);
}

void shell_send_capture_state(bool active) {
    if (capture_active == active)
        return;
    capture_active = active;
    for (auto* r : shell_resources)
        if (r && wl_resource_get_version(r) >= ZLM_SHELL_V1_CAPTURE_STATE_SINCE_VERSION)
            zlm_shell_v1_send_capture_state(r, active ? 1u : 0u);
}

bool shell_send_lock_requested(unsigned reason) {
    bool told = false;
    for (auto* r : shell_resources) {
        if (r && wl_resource_get_version(r) >= ZLM_SHELL_V1_LOCK_REQUESTED_SINCE_VERSION) {
            zlm_shell_v1_send_lock_requested(r, reason);
            told = true;
        }
    }
    return told;
}

void shell_fire_key_grab(void* grab_resource, unsigned time_ms) {
    auto* res = static_cast<wl_resource*>(grab_resource);
    if (!res || !g_p0_ctx || !g_p0_ctx->seat)
        return;
    // An input serial, recorded as a key press delivered to the shell: the shell may quote
    // it to xdg_activation_token_v1.set_serial for the window it launches, and the focus-
    // stealing policy accepts it as the shell's last press.
    const unsigned serial = g_p0_ctx->seat->next_serial();
    if (g_p0_ctx->input)
        g_p0_ctx->input->serials().note(wl_resource_get_client(res), serial,
                                        lx::compositor::input_serial_kind::key_press);
    zlm_key_grab_v1_send_activated(res, serial, time_ms);
    if (surface_debug()) {
        char line[64];
        std::snprintf(line, sizeof(line), "grab activated serial=%u", serial);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.shell", line);
    }
}

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
    res.set_implementation(&shell_impl, g_p0_ctx, [](void* r) {
        for (auto*& s : shell_resources)
            if (s == r)
                s = nullptr;
    });
    for (auto*& s : shell_resources) {
        if (!s) {
            s = static_cast<wl_resource*>(res.native());
            break;
        }
    }
    // "Sent immediately after bind". No optional capability is implemented yet (tiling
    // plugin, workspace drag, explicit sync), so the flags are empty.
    // shell_protocol_version: the zlm family's version this compositor implements (2:
    // toplevel_flags; 3: request_logout; 4: grab_key; 5: lock_state, lock_requested; 6:
    // icons, parents, bell, capture_state, not_inhibitable grabs).
    auto* native = static_cast<wl_resource*>(res.native());
    zlm_shell_v1_send_capabilities(native, 0u, 6u);
    if (wl_resource_get_version(native) >= ZLM_SHELL_V1_CAPTURE_STATE_SINCE_VERSION)
        zlm_shell_v1_send_capture_state(native, capture_active ? 1u : 0u);
    // "Sent on bind and on every change": a shell restarted behind a lock learns it at once.
    if (wl_resource_get_version(native) >= ZLM_SHELL_V1_LOCK_STATE_SINCE_VERSION)
        zlm_shell_v1_send_lock_state(
            native, g_p0_ctx->lock ? lx::compositor::shell_lock_state(g_p0_ctx->lock->state()) : 0u);
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
