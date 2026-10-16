module;

// Installs every protocol global, in the order clients see them in the registry, and
// implements the exported entry points of :protocols_p0. Part of the protocol wire split;
// shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "zlm_shell_v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
// wayland-scanner names an argument `namespace`, a C++ keyword; renamed for this header
// only (Wayfire does the same).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif
#if __has_include("primary-selection-unstable-v1-server-protocol.h")
#include "primary-selection-unstable-v1-server-protocol.h"
#define LUMEN_HAS_PRIMARY_SELECTION 1
#endif
#if __has_include("xdg-activation-v1-server-protocol.h")
#include "xdg-activation-v1-server-protocol.h"
#define LUMEN_HAS_XDG_ACTIVATION 1
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

p0_protocol_context* g_p0_ctx = nullptr;

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
    // Output membership: every node rect change and the cursor surface feed enter/leave.
    install_outputs(ctx);
    // Selections: per-client input serials and keyboard focus decide who may set one and
    // who is offered it; client teardown drops what is keyed by its wl_client*.
    ctx.server->set_client_destroyed_hook(&wire_client_destroyed, &ctx);
    if (ctx.input)
        ctx.input->set_focus_client_hook(&xfer_focus_changed, &ctx);
    install_data_device(ctx);
    install_dnd(ctx);
    // v4 adds only wl_data_device_manager.release.
#if defined(WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION)
    constexpr int k_data_device_manager_version = 4;
#else
    constexpr int k_data_device_manager_version = 3;
#endif
    ctx.server->add_global({.interface_name = "wl_data_device_manager",
                            .version = k_data_device_manager_version,
                            .privileged = false,
                            .interface_desc = &wl_data_device_manager_interface},
                           &bind_data_device_manager);
#if defined(LUMEN_HAS_PRIMARY_SELECTION)
    install_primary_selection(ctx);
    ctx.server->add_global({.interface_name = "zwp_primary_selection_device_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &zwp_primary_selection_device_manager_v1_interface},
                           &bind_primary_selection_manager);
#endif
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (ctx.wm) {
        lx::compositor::wm_protocol_ops ops{};
        ops.send_configure = &wm_send_configure;
        ops.next_serial = &wm_next_serial;
        ops.send_close = &wm_send_close;
        ops.last_user_serial = &wm_last_user_serial;
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
                            .version = 2,
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

    // xdg-output v3: logical position and size, name, description; closed by wl_output.done.
    ctx.server->add_global({.interface_name = "zxdg_output_manager_v1",
                            .version = 3,
                            .privileged = false,
                            .interface_desc = &zxdg_output_manager_v1_interface},
                           &bind_xdg_output_manager);
    // xdg_wm_base.ping/pong liveness: pings on activation, presses and the configure
    // watchdog; the not-responding state is shown, never acted on.
    install_liveness(ctx);
#endif
#if defined(LUMEN_HAS_XDG_ACTIVATION)
    // xdg-activation: tokens issued against input serials; presenting one focuses the window
    // or, when the policy refuses (focus-stealing prevention), marks it demands-attention.
    if (ctx.activation && ctx.wm)
        ctx.server->add_global({.interface_name = "xdg_activation_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &xdg_activation_v1_interface},
                               &bind_activation);
#endif
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
