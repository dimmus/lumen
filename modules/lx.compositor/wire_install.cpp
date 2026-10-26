module;

// Installs every protocol global, in the order clients see them in the registry, and
// implements the exported entry points of :protocols_p0. Part of the protocol wire split;
// shared declarations live in the :wire partition (wire.cppm).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#if defined(LUMEN_PROTOCOL_PRIMARY_SELECTION_UNSTABLE_V1)
#include "primary-selection-unstable-v1-server-protocol.h"
#define LUMEN_HAS_PRIMARY_SELECTION 1
#endif
#if defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1)
#include "xdg-activation-v1-server-protocol.h"
#define LUMEN_HAS_XDG_ACTIVATION 1
#endif
#if defined(LUMEN_PROTOCOL_VIEWPORTER)
#include "viewporter-server-protocol.h"
#define LUMEN_HAS_VIEWPORTER 1
#endif
#if defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "fractional-scale-v1-server-protocol.h"
#define LUMEN_HAS_FRACTIONAL_SCALE 1
#endif
#if defined(LUMEN_PROTOCOL_PRESENTATION_TIME)
#include "presentation-time-server-protocol.h"
#define LUMEN_HAS_PRESENTATION 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_IDLE_NOTIFY_V1)
#include "ext-idle-notify-v1-server-protocol.h"
#define LUMEN_HAS_IDLE_NOTIFY 1
#endif
#if defined(LUMEN_PROTOCOL_IDLE_INHIBIT_UNSTABLE_V1)
#include "idle-inhibit-unstable-v1-server-protocol.h"
#define LUMEN_HAS_IDLE_INHIBIT 1
#endif
#if defined(LUMEN_PROTOCOL_WLR_OUTPUT_POWER_MANAGEMENT_UNSTABLE_V1)
#include "wlr-output-power-management-unstable-v1-server-protocol.h"
#define LUMEN_HAS_OUTPUT_POWER 1
#endif
#if defined(LUMEN_PROTOCOL_WLR_OUTPUT_MANAGEMENT_UNSTABLE_V1)
#include "wlr-output-management-unstable-v1-server-protocol.h"
#define LUMEN_HAS_OUTPUT_MANAGEMENT 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
#include "ext-workspace-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "ext-session-lock-v1-server-protocol.h"
#define LUMEN_HAS_SESSION_LOCK 1
#endif
#if defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3)
#include "text-input-unstable-v3-server-protocol.h"
#define LUMEN_HAS_TEXT_INPUT 1
#endif
#if defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2)
#include "input-method-unstable-v2-server-protocol.h"
#define LUMEN_HAS_INPUT_METHOD 1
#endif
#if defined(LUMEN_PROTOCOL_VIRTUAL_KEYBOARD_UNSTABLE_V1)
#include "virtual-keyboard-unstable-v1-server-protocol.h"
#define LUMEN_HAS_VIRTUAL_KEYBOARD 1
#endif
#if defined(LUMEN_PROTOCOL_FIFO_V1)
#include "fifo-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_COMMIT_TIMING_V1)
#include "commit-timing-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_TEARING_CONTROL_V1)
#include "tearing-control-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_CONTENT_TYPE_V1)
#include "content-type-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1)
#include "single-pixel-buffer-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_ALPHA_MODIFIER_V1)
#include "alpha-modifier-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_LINUX_DRM_SYNCOBJ_V1)
#include "linux-drm-syncobj-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_XDG_DIALOG_V1)
#include "xdg-dialog-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1)
#include "xdg-toplevel-icon-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1)
#include "xdg-system-bell-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V2)
#include "xdg-foreign-unstable-v2-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V1)
// v1's exporter request is named `export`, a C++ keyword (see wire_foreign.cpp).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define export export_
#include "xdg-foreign-unstable-v1-server-protocol.h"
#undef export
#pragma clang diagnostic pop
#endif
#if defined(LUMEN_PROTOCOL_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1)
#include "keyboard-shortcuts-inhibit-unstable-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_EXT_DATA_CONTROL_V1)
#include "ext-data-control-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_WLR_DATA_CONTROL_UNSTABLE_V1)
#include "wlr-data-control-unstable-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_SECURITY_CONTEXT_V1)
#include "security-context-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_POINTER_CONSTRAINTS_UNSTABLE_V1)
#include "pointer-constraints-unstable-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1)
#include "relative-pointer-unstable-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_POINTER_GESTURES_UNSTABLE_V1)
#include "pointer-gestures-unstable-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1)
#include "ext-foreign-toplevel-list-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1)
#include "ext-image-capture-source-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
#include "ext-image-copy-capture-v1-server-protocol.h"
#endif
#if defined(LUMEN_PROTOCOL_WLR_SCREENCOPY_UNSTABLE_V1)
#include "wlr-screencopy-unstable-v1-server-protocol.h"
#endif
#endif
#include <signal.h>

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
import :content_update;
import :capture_engine;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

p0_protocol_context* g_p0_ctx = nullptr;

namespace {

/// The router's one keyboard-focus-surface hook, shared: the text-input relay follows the
/// focus, and keyboard-shortcuts-inhibit decides which inhibitor is in effect.
void focus_surface_changed(void* user, void* surface, void* client, void* destroyed) {
    text_input_focus_hook(user, surface, client, destroyed);
    shortcuts_inhibit_focus(destroyed ? nullptr : surface);
    // A pointer constraint holds only while its window has the keyboard.
    pointer_constraints_reevaluate();
}

/// The router's pointer-focus hook: a constraint holds only on the pointer-focus surface.
void pointer_focus_changed(void*) { pointer_constraints_reevaluate(); }

/// The window manager's hook before a toplevel record goes: what the wire keyed by it.
void wire_toplevel_destroyed(void* user, lx::toplevel_id id) {
    auto* ctx = static_cast<p0_protocol_context*>(user);
    if (ctx && ctx->toplevels)
        if (auto* rec = ctx->toplevels->find_mut(id))
            toplevel_icon_released(*rec);
    foreign_toplevel_destroyed(id);
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
        if (ctx.wm)
            ctx.wm->set_texture_ops(ops); // the fullscreen backdrop
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
    // Output membership: every node rect change and the cursor surface feed enter/leave;
    // and one wl_output global per output (created here for the outputs known at start,
    // then with the outputs).
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
        ctx.notify_session_ending = &shell_send_session_ending;
        ctx.fire_key_grab = &shell_fire_key_grab;
        ctx.send_shell_config = &shell_send_config;
        ctx.invalidate_seat_keymap = &seat_keymap_invalidate;
        ctx.send_shell_action = &shell_send_action;
        ctx.send_held_action = &shell_send_held_action;
        ctx.send_hold_key = &shell_send_hold_key;
        ctx.send_hold_released = &shell_send_hold_released;
        ctx.send_hold_cancelled = &shell_send_hold_cancelled;
        ctx.notify_lock_state = &shell_send_lock_state;
        ctx.notify_lock_requested = &shell_send_lock_requested;
        ops.last_user_serial = &wm_last_user_serial;
        ops.toplevel_destroyed = &wire_toplevel_destroyed;
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
    if (ctx.shell)
        ctx.shell->set_icons(ctx.icons);
    ctx.server->add_global({.interface_name = "zlm_shell_v1",
                            .version = 7,
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
                                .deny_sandboxed = true,
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
#if defined(LUMEN_HAS_VIEWPORTER)
    // wp_viewporter: per-surface source crop and destination size, applied in the draw
    // (float source box, buffer transform) with every error of the protocol.
    ctx.server->add_global({.interface_name = "wp_viewporter",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_viewporter_interface},
                           &bind_viewporter);
#endif
#if defined(LUMEN_HAS_FRACTIONAL_SCALE)
    // wp_fractional_scale_v1: the preferred scale in 120ths from the outputs a surface is
    // on; a client renders round(w·scale) pixels and sets a viewport destination of w.
    ctx.server->add_global({.interface_name = "wp_fractional_scale_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_fractional_scale_manager_v1_interface},
                           &bind_fractional_scale);
#endif
#if defined(LUMEN_HAS_PRESENTATION)
    // wp_presentation v2: CLOCK_MONOTONIC; feedback presented with the KMS flip's timestamp
    // and vblank sequence, or discarded when superseded, unmapped or never shown.
    if (ctx.presentation)
        ctx.server->add_global({.interface_name = "wp_presentation",
                                .version = 2,
                                .privileged = false,
                                .interface_desc = &wp_presentation_interface},
                               &bind_presentation);
#endif
#if defined(LUMEN_HAS_IDLE_NOTIFY)
    // ext_idle_notifier_v1 v2: idle and resumed per notification, from real input only;
    // `get_idle_notification` obeys visible idle inhibitors, `get_input_idle_notification`
    // does not. lumen-shell's blanking policy and swayidle are its clients.
    if (ctx.idle)
        ctx.server->add_global({.interface_name = "ext_idle_notifier_v1",
                                .version = 2,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &ext_idle_notifier_v1_interface},
                               &bind_idle_notifier);
#endif
#if defined(LUMEN_HAS_IDLE_INHIBIT)
    // zwp_idle_inhibit_manager_v1: honoured while the inhibiting surface is on screen.
    if (ctx.inhibitors)
        ctx.server->add_global({.interface_name = "zwp_idle_inhibit_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zwp_idle_inhibit_manager_v1_interface},
                               &bind_idle_inhibit_manager);
#endif
#if defined(LUMEN_HAS_OUTPUT_POWER)
    // zwlr_output_power_manager_v1: display power (atomic ACTIVE=0, woken by input). Always
    // advertised; without a display to power every object is `failed`.
    ctx.server->add_global({.interface_name = "zwlr_output_power_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .deny_sandboxed = true,
                            .interface_desc = &zwlr_output_power_manager_v1_interface},
                           &bind_output_power_manager);
#endif
#if defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    // zwlr_output_manager_v1 v4 (ADR 008 C1, C5): heads, modes, test and apply with a real
    // TEST_ONLY. Open to every same-UID client that is not sandboxed, as in sway and niri —
    // wlr-randr and kanshi run from a terminal or a user service, not as the shell.
    if (ctx.output_mgmt_kick)
        ctx.server->add_global({.interface_name = "zwlr_output_manager_v1",
                                .version = 4,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &zwlr_output_manager_v1_interface},
                               &bind_output_manager);
#endif
#if defined(LUMEN_HAS_SESSION_LOCK)
    // ext_session_lock_manager_v1: lock surfaces per output; `locked` only once a locked frame
    // is on every output; a locker that dies leaves the session locked (abandoned).
    if (ctx.lock)
        ctx.server->add_global({.interface_name = "ext_session_lock_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &ext_session_lock_manager_v1_interface},
                               &bind_session_lock_manager);
#endif
    // The commit-path protocols (design-app-compat-capture Part A): all ride the
    // content-update queue, so they are offered together; every client, sandboxed or not,
    // may use them.
    if (cu_poll_fd() < 0)
        log_surface.warn("readiness poller unavailable: fences are not waited "
                         "for before applying content updates");
#if defined(LUMEN_PROTOCOL_FIFO_V1)
    // fifo-v1: one update per refresh for a shown surface (Mesa's Vulkan FIFO present).
    ctx.server->add_global({.interface_name = "wp_fifo_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_fifo_manager_v1_interface},
                           &bind_fifo_manager);
#endif
#if defined(LUMEN_PROTOCOL_COMMIT_TIMING_V1)
    // commit-timing-v1: never shown a refresh before its target, the clock armed for it.
    ctx.server->add_global({.interface_name = "wp_commit_timing_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_commit_timing_manager_v1_interface},
                           &bind_commit_timing_manager);
#endif
#if defined(LUMEN_PROTOCOL_TEARING_CONTROL_V1)
    // tearing-control-v1: the hint is kept on every host; async flips only where KMS has
    // them (the output policy, `allow_async`).
    ctx.server->add_global({.interface_name = "wp_tearing_control_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_tearing_control_manager_v1_interface},
                           &bind_tearing_control_manager);
#endif
#if defined(LUMEN_PROTOCOL_CONTENT_TYPE_V1)
    ctx.server->add_global({.interface_name = "wp_content_type_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_content_type_manager_v1_interface},
                           &bind_content_type_manager);
#endif
#if defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1)
    // Solid-colour buffers drawn without a texture.
    ctx.server->add_global({.interface_name = "wp_single_pixel_buffer_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_single_pixel_buffer_manager_v1_interface},
                           &bind_single_pixel_buffer_manager);
#endif
#if defined(LUMEN_PROTOCOL_ALPHA_MODIFIER_V1)
    ctx.server->add_global({.interface_name = "wp_alpha_modifier_v1",
                            .version = 1,
                            .privileged = false,
                            .interface_desc = &wp_alpha_modifier_v1_interface},
                           &bind_alpha_modifier);
#endif
#if defined(LUMEN_PROTOCOL_LINUX_DRM_SYNCOBJ_V1)
    // Explicit sync: only where a client can use it correctly (timeline syncobjs, the
    // eventfd ioctl, a composite that exports its completion) — wlroots and Mutter hide it
    // otherwise. Not on vmwgfx.
    if (ctx.syncobj_supported && ctx.syncobj_drm_fd >= 0)
        ctx.server->add_global({.interface_name = "wp_linux_drm_syncobj_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &wp_linux_drm_syncobj_manager_v1_interface},
                               &bind_syncobj_manager);
#endif
    // text-input-v3 v2 + input-method-v2 + virtual-keyboard-v1: the focused client's text
    // inputs relayed to the seat's one input method (fcitx5, ibus), its candidate popups at
    // the cursor, its keyboard grab and its forwarded keys.
    install_text_input(ctx);
    if (ctx.input) {
        ctx.input->set_focus_surface_hook(&focus_surface_changed, &ctx);
        ctx.input->set_pointer_focus_hook(&pointer_focus_changed, &ctx);
    }
#if defined(LUMEN_HAS_TEXT_INPUT)
    if (ctx.text_input)
        ctx.server->add_global({.interface_name = "zwp_text_input_manager_v3",
                                .version = 2,
                                .privileged = false,
                                .interface_desc = &zwp_text_input_manager_v3_interface},
                               &bind_text_input_manager);
#endif
#if defined(LUMEN_HAS_INPUT_METHOD)
    if (ctx.text_input)
        ctx.server->add_global({.interface_name = "zwp_input_method_manager_v2",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &zwp_input_method_manager_v2_interface},
                               &bind_input_method_manager);
#endif
#if defined(LUMEN_HAS_VIRTUAL_KEYBOARD)
    if (ctx.text_input)
        ctx.server->add_global({.interface_name = "zwp_virtual_keyboard_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &zwp_virtual_keyboard_manager_v1_interface},
                               &bind_virtual_keyboard_manager);
#endif

    // ── Window and session protocols (design-app-compat-capture Part B). Dialogs, icons,
    //    the bell, xdg-foreign and shortcuts inhibition are for every client, sandboxed or
    //    not (a sandboxed app exports its window for its portal dialogs); data control and
    //    the security-context manager are session-wide and hidden from sandboxed clients.
#if defined(LUMEN_PROTOCOL_XDG_DIALOG_V1) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    // xdg-dialog-v1: modal dialogs block their parent (window_manager::set_modal).
    if (ctx.wm)
        ctx.server->add_global({.interface_name = "xdg_wm_dialog_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &xdg_wm_dialog_v1_interface},
                               &bind_wm_dialog);
#endif
#if defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    // xdg-toplevel-icon-v1: icons snapshotted into sealed memfds for the shell's panel.
    if (ctx.icons)
        ctx.server->add_global({.interface_name = "xdg_toplevel_icon_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &xdg_toplevel_icon_manager_v1_interface},
                               &bind_toplevel_icon_manager);
#endif
#if defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1)
    // xdg-system-bell-v1: forwarded to the shell (a visual bell), 4 per second per client.
    if (ctx.shell)
        ctx.server->add_global({.interface_name = "xdg_system_bell_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &xdg_system_bell_v1_interface},
                               &bind_system_bell);
#endif
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
    // xdg-foreign v2 and v1 over one registry: portals parent their dialogs onto app windows.
#if defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V2)
    if (ctx.foreign && ctx.wm) {
        ctx.server->add_global({.interface_name = "zxdg_exporter_v2",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zxdg_exporter_v2_interface},
                               &bind_exporter_v2);
        ctx.server->add_global({.interface_name = "zxdg_importer_v2",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zxdg_importer_v2_interface},
                               &bind_importer_v2);
    }
#endif
#if defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V1)
    if (ctx.foreign && ctx.wm) {
        ctx.server->add_global({.interface_name = "zxdg_exporter_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zxdg_exporter_v1_interface},
                               &bind_exporter_v1);
        ctx.server->add_global({.interface_name = "zxdg_importer_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zxdg_importer_v1_interface},
                               &bind_importer_v1);
    }
#endif
#endif
#if defined(LUMEN_PROTOCOL_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1)
    // keyboard-shortcuts-inhibit: granted while the surface has the keyboard; VT switching,
    // logout, the lock and Super+Escape are never inhibited.
    if (ctx.input)
        ctx.server->add_global(
            {.interface_name = "zwp_keyboard_shortcuts_inhibit_manager_v1",
             .version = 1,
             .privileged = false,
             .interface_desc = &zwp_keyboard_shortcuts_inhibit_manager_v1_interface},
            &bind_shortcuts_inhibit_manager);
#endif
#if defined(LUMEN_PROTOCOL_EXT_DATA_CONTROL_V1)
    // ext-data-control-v1 (and wlr v2 below): clipboard managers, wl-clipboard, cliphist.
    ctx.server->add_global({.interface_name = "ext_data_control_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .deny_sandboxed = true,
                            .interface_desc = &ext_data_control_manager_v1_interface},
                           &bind_ext_data_control_manager);
#endif
#if defined(LUMEN_PROTOCOL_WLR_DATA_CONTROL_UNSTABLE_V1)
    ctx.server->add_global({.interface_name = "zwlr_data_control_manager_v1",
                            .version = 2,
                            .privileged = false,
                            .deny_sandboxed = true,
                            .interface_desc = &zwlr_data_control_manager_v1_interface},
                           &bind_wlr_data_control_manager);
#endif
    // ── Pointer protocols (design-app-compat-capture Part C): for every client, sandboxed
    //    or not — a game or a VM viewer in a sandbox locks the pointer like any other.
#if defined(LUMEN_PROTOCOL_POINTER_CONSTRAINTS_UNSTABLE_V1)
    // pointer-constraints: lock and confine while the window has both foci; Super+Escape
    // releases.
    if (ctx.input && ctx.seat)
        ctx.server->add_global({.interface_name = "zwp_pointer_constraints_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zwp_pointer_constraints_v1_interface},
                               &bind_pointer_constraints);
#endif
#if defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1)
    // relative-pointer: accelerated and unaccelerated deltas, also while locked.
    if (ctx.input)
        ctx.server->add_global({.interface_name = "zwp_relative_pointer_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .interface_desc = &zwp_relative_pointer_manager_v1_interface},
                               &bind_relative_pointer_manager);
#endif
#if defined(LUMEN_PROTOCOL_POINTER_GESTURES_UNSTABLE_V1)
    // pointer-gestures v3: libinput swipe, pinch and hold to the pointer focus.
    if (ctx.input)
        ctx.server->add_global({.interface_name = "zwp_pointer_gestures_v1",
                                .version = 3,
                                .privileged = false,
                                .interface_desc = &zwp_pointer_gestures_v1_interface},
                               &bind_pointer_gestures);
#endif
    // ── Screen capture (design-app-compat-capture Part D): session-wide, hidden from
    //    sandboxed clients (SC2) — they share their screen through the portal.
    install_toplevel_observer(ctx);
#if defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1)
    // ext-foreign-toplevel-list: the window list capture pickers and `grim -T` use.
    if (ctx.toplevels && ctx.shell)
        ctx.server->add_global({.interface_name = "ext_foreign_toplevel_list_v1",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &ext_foreign_toplevel_list_v1_interface},
                               &bind_foreign_toplevel_list);
#endif
#if defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
    // ext-workspace (design-session-and-wm W.4): the workspaces of every output, which the
    // shell's indicator and any bar show and switch. Session-wide: not for sandboxed
    // clients.
    if (ctx.wm)
        ctx.server->add_global({.interface_name = "ext_workspace_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &ext_workspace_manager_v1_interface},
                               &bind_workspace_manager);
#endif
#if defined(LUMEN_PROTOCOL_EXT_IMAGE_CAPTURE_SOURCE_V1) &&                                    \
    defined(LUMEN_PROTOCOL_EXT_IMAGE_COPY_CAPTURE_V1)
    // ext-image-capture-source + ext-image-copy-capture: outputs and windows into client
    // buffers, one copy from the composited frame (wire_capture.cpp).
    if (ctx.capture && ctx.capture->backend != capture_backend::none) {
        ctx.server->add_global(
            {.interface_name = "ext_output_image_capture_source_manager_v1",
             .version = 1,
             .privileged = false,
             .deny_sandboxed = true,
             .interface_desc = &ext_output_image_capture_source_manager_v1_interface},
            &bind_output_capture_source_manager);
#if defined(LUMEN_PROTOCOL_EXT_FOREIGN_TOPLEVEL_LIST_V1)
        if (ctx.capture->backend != capture_backend::vulkan)
            ctx.server->add_global(
                {.interface_name = "ext_foreign_toplevel_image_capture_source_manager_v1",
                 .version = 1,
                 .privileged = false,
                 .deny_sandboxed = true,
                 .interface_desc = &ext_foreign_toplevel_image_capture_source_manager_v1_interface},
                &bind_toplevel_capture_source_manager);
#endif
        ctx.server->add_global({.interface_name = "ext_image_copy_capture_manager_v1",
                                .version = 1,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &ext_image_copy_capture_manager_v1_interface},
                               &bind_image_copy_capture_manager);
#if defined(LUMEN_PROTOCOL_WLR_SCREENCOPY_UNSTABLE_V1)
        // wlr-screencopy v3: the same capture core for tools that predate it (wf-recorder).
        ctx.server->add_global({.interface_name = "zwlr_screencopy_manager_v1",
                                .version = 3,
                                .privileged = false,
                                .deny_sandboxed = true,
                                .interface_desc = &zwlr_screencopy_manager_v1_interface},
                               &bind_screencopy_manager);
#endif
    }
#endif
#if defined(LUMEN_PROTOCOL_SECURITY_CONTEXT_V1)
    // security-context-v1: Flatpak's sockets; their clients are sandboxed (the filter above
    // hides every deny_sandboxed global from them). Hidden from them too: no nesting.
    ctx.server->add_global({.interface_name = "wp_security_context_manager_v1",
                            .version = 1,
                            .privileged = false,
                            .deny_sandboxed = true,
                            .interface_desc = &wp_security_context_manager_v1_interface},
                           &bind_security_context_manager);
#endif
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

lx::result<void> lx::compositor::install_p0_protocols(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!ctx.server) {
        return lx::make_error(lx::wayland_err::bind_failed, "p0 install: null server");
    }
    p0::install_core(ctx);
    p0::bind_shm_direct(ctx);
    p0::bind_capture_guards(ctx);
    return {};
#else
    (void)ctx;
    return lx::make_error(lx::wayland_err::bind_failed,
                          "libwayland-server required for P0 protocols");
#endif
}

void lx::compositor::fire_frame_callbacks(p0_protocol_context& ctx, unsigned frame_time_ms) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::fire_frames(frame_time_ms, false, p0::k_any_output, 0);
#else
    (void)ctx;
    (void)frame_time_ms;
#endif
}

void lx::compositor::fire_visible_frame_callbacks(p0_protocol_context& ctx,
                                                  unsigned frame_time_ms,
                                                  unsigned long long now_ns) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::fire_frames(frame_time_ms, true, p0::k_any_output, now_ns);
#else
    (void)ctx;
    (void)frame_time_ms;
    (void)now_ns;
#endif
}

void lx::compositor::fire_output_frame_callbacks(p0_protocol_context& ctx, unsigned slot,
                                                 unsigned frame_time_ms,
                                                 unsigned long long now_ns) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::fire_frames(frame_time_ms, true, slot, now_ns);
#else
    (void)ctx;
    (void)slot;
    (void)frame_time_ms;
    (void)now_ns;
#endif
}

int lx::compositor::content_updates_poll_fd(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::cu_poll_fd();
#else
    return -1;
#endif
}

void lx::compositor::content_updates_poll(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::cu_poll();
#endif
}

void lx::compositor::content_updates_try(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::cu_try_all("tick");
#endif
}

void lx::compositor::content_updates_latched(p0_protocol_context& ctx, unsigned slot,
                                             unsigned frame) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    p0::cu_after_latch(slot, frame);
#else
    (void)slot;
    (void)frame;
#endif
}

std::uint32_t lx::compositor::content_updates_waiting_outputs(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::cu_waiting_outputs();
#else
    return 0;
#endif
}

unsigned long long lx::compositor::content_updates_next_target(p0_protocol_context& ctx,
                                                               unsigned slot) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::cu_earliest_target(slot);
#else
    (void)slot;
    return 0;
#endif
}

unsigned lx::compositor::content_updates_queued(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::cu_queue().used_count();
#else
    return 0;
#endif
}

bool lx::compositor::frame_callbacks_throttled(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::frames_throttled();
#else
    return false;
#endif
}

bool lx::compositor::frame_callbacks_pending(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::frames_pending();
#else
    return false;
#endif
}

std::uint32_t lx::compositor::frame_callbacks_pending_outputs(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND)
    return p0::frames_pending_outputs();
#else
    return 0;
#endif
}

void lx::compositor::release_wl_buffer_resource(void* resource, void*) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!resource)
        return;
    // Explicit sync first: the release point carries the composite fence, so a client
    // waiting on it rather than on the event still never reuses a buffer the GPU reads.
    p0::syncobj_buffer_released(static_cast<wl_resource*>(resource));
    wl_buffer_send_release(static_cast<wl_resource*>(resource));
#else
    (void)resource;
#endif
}

void lx::compositor::release_sync_point(unsigned timeline, unsigned long long point,
                                        bool signal, void*) {
#if defined(LUMEN_HAS_WAYLAND)
    p0::syncobj_point_settle(timeline, point, signal);
#else
    (void)timeline;
    (void)point;
    (void)signal;
#endif
}
