// Protocol conformance: requests that must end in a specific protocol error, and request
// orders that must not. In-process compositor, one libwayland-client connection per case
// (a protocol error is fatal to its connection), asserting wl_display_get_protocol_error.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <utility>

#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#if __has_include(<xkbcommon/xkbcommon.h>)
#include <xkbcommon/xkbcommon.h>
#define LUMEN_TEST_XKB 1
#endif
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "zlm_shell_v1-client-protocol.h"
// wayland-scanner names an argument `namespace`, a C++ keyword (Wayfire renames it the same
// way).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#if defined(LUMEN_PROTOCOL_PRIMARY_SELECTION_UNSTABLE_V1)
#include "primary-selection-unstable-v1-client-protocol.h"
#define LUMEN_TEST_PRIMARY_SELECTION 1
#endif
#if defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1)
#include "xdg-activation-v1-client-protocol.h"
#define LUMEN_TEST_ACTIVATION 1
#endif
#if defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#define LUMEN_TEST_VIEWPORTER 1
#endif
#if defined(LUMEN_PROTOCOL_PRESENTATION_TIME)
#include "presentation-time-client-protocol.h"
#define LUMEN_TEST_PRESENTATION 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_IDLE_NOTIFY_V1) && \
    defined(LUMEN_PROTOCOL_IDLE_INHIBIT_UNSTABLE_V1) && \
    defined(LUMEN_PROTOCOL_WLR_OUTPUT_POWER_MANAGEMENT_UNSTABLE_V1)
#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#define LUMEN_TEST_IDLE 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "ext-session-lock-v1-client-protocol.h"
#define LUMEN_TEST_SESSION_LOCK 1
#endif
#if defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3) && \
    defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2) && \
    defined(LUMEN_PROTOCOL_VIRTUAL_KEYBOARD_UNSTABLE_V1)
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#define LUMEN_TEST_IME 1
#endif
#endif
#endif

import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND) || !defined(LUMEN_HAS_PROTOCOL_GLUE)

int main() {
    std::printf("SKIP: needs libwayland and the generated protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-conformance";

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    zxdg_decoration_manager_v1* deco = nullptr;
    /// Version to bind the decoration manager at (the next connection's).
    uint32_t deco_version = 2;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    uint32_t layer_version = 5;
    wl_data_device_manager* ddm = nullptr;
    /// The wl_output global (not bound by `connect`).
    uint32_t output_name = 0;
    uint32_t output_version = 0;
#if defined(LUMEN_TEST_PRIMARY_SELECTION)
    zwp_primary_selection_device_manager_v1* psm = nullptr;
#endif
#if defined(LUMEN_TEST_ACTIVATION)
    xdg_activation_v1* activation = nullptr;
#endif
#if defined(LUMEN_TEST_VIEWPORTER)
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional = nullptr;
#endif
#if defined(LUMEN_TEST_PRESENTATION)
    wp_presentation* presentation = nullptr;
    /// wp_presentation.clock_id as received (~0u: none yet).
    uint32_t clock_id = ~0u;
#endif
#if defined(LUMEN_TEST_IDLE)
    ext_idle_notifier_v1* idle_notifier = nullptr;
    zwp_idle_inhibit_manager_v1* inhibit_manager = nullptr;
    zwlr_output_power_manager_v1* power_manager = nullptr;
#endif
#if defined(LUMEN_TEST_SESSION_LOCK)
    ext_session_lock_manager_v1* lock_manager = nullptr;
#endif
#if defined(LUMEN_TEST_IME)
    zwp_text_input_manager_v3* ti_manager = nullptr;
    uint32_t ti_version = 0;
    zwp_input_method_manager_v2* im_manager = nullptr;
    zwp_virtual_keyboard_manager_v1* vk_manager = nullptr;
#endif
};

/// Layer-shell version the next `connect` binds.
uint32_t g_layer_version = 5;

/// Decoration manager version the next `connect` binds.
uint32_t g_deco_version = 2;

void on_global(void* data, wl_registry* registry, uint32_t name, const char* iface,
               uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, wl_subcompositor_interface.name) == 0)
        c->subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
        c->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0)
        c->deco = static_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface,
            c->deco_version < version ? c->deco_version : version));
    else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        c->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface,
            c->layer_version < version ? c->layer_version : version));
    else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0)
        c->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
    else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        c->output_name = name;
        c->output_version = version;
    }
#if defined(LUMEN_TEST_PRIMARY_SELECTION)
    else if (std::strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0)
        c->psm = static_cast<zwp_primary_selection_device_manager_v1*>(wl_registry_bind(
            registry, name, &zwp_primary_selection_device_manager_v1_interface, 1));
#endif
#if defined(LUMEN_TEST_ACTIVATION)
    else if (std::strcmp(iface, xdg_activation_v1_interface.name) == 0)
        c->activation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
#endif
#if defined(LUMEN_TEST_VIEWPORTER)
    else if (std::strcmp(iface, wp_viewporter_interface.name) == 0)
        c->viewporter = static_cast<wp_viewporter*>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0)
        c->fractional = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
#endif
#if defined(LUMEN_TEST_PRESENTATION)
    else if (std::strcmp(iface, wp_presentation_interface.name) == 0) {
        static const wp_presentation_listener presentation_listener = {
            [](void* data, wp_presentation*, uint32_t clk) {
                static_cast<client*>(data)->clock_id = clk;
            }};
        c->presentation = static_cast<wp_presentation*>(
            wl_registry_bind(registry, name, &wp_presentation_interface, version < 2 ? version : 2));
        wp_presentation_add_listener(c->presentation, &presentation_listener, c);
    }
#endif
#if defined(LUMEN_TEST_IDLE)
    else if (std::strcmp(iface, ext_idle_notifier_v1_interface.name) == 0)
        c->idle_notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, version < 2 ? version : 2));
    else if (std::strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
        c->inhibit_manager = static_cast<zwp_idle_inhibit_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
    else if (std::strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0)
        c->power_manager = static_cast<zwlr_output_power_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1));
#endif
#if defined(LUMEN_TEST_SESSION_LOCK)
    else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0)
        c->lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1));
#endif
#if defined(LUMEN_TEST_IME)
    else if (std::strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        c->ti_version = version < 2 ? version : 2;
        c->ti_manager = static_cast<zwp_text_input_manager_v3*>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, c->ti_version));
    } else if (std::strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
        c->im_manager = static_cast<zwp_input_method_manager_v2*>(
            wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1));
    else if (std::strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        c->vk_manager = static_cast<zwp_virtual_keyboard_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
#endif
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.deco_version = g_deco_version;
    c.layer_version = g_layer_version;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    return wl_display_roundtrip(c.display) >= 0 && c.compositor && c.subcompositor && c.wm_base;
}

/// The error the connection died of: interface name and code, or "none".
struct outcome {
    const char* interface = "none";
    uint32_t code = 0;
};

outcome finish(client& c) {
    (void)wl_display_roundtrip(c.display);
    outcome o{};
    if (wl_display_get_error(c.display) == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        o.code = wl_display_get_protocol_error(c.display, &iface, &id);
        // A destructor request (wl_surface.destroy) removes the proxy on the client side at
        // once, so libwayland-client reports no interface for an error on it.
        o.interface = iface ? iface->name : "[destroyed]";
    }
    wl_display_disconnect(c.display);
    return o;
}

int g_failed = 0;
int g_ran = 0;

/// Runs `body` on a fresh connection and checks the error it ends in.
void expect(const char* name, const char* interface, uint32_t code,
            const std::function<void(client&)>& body) {
    ++g_ran;
    client c{};
    if (!connect(c)) {
        std::printf("  FAIL  %s — could not connect or bind\n", name);
        ++g_failed;
        return;
    }
    body(c);
    const outcome o = finish(c);
    const bool ok = std::strcmp(o.interface, interface) == 0 && o.code == code;
    std::printf("  %s  %s — got %s/%u, want %s/%u\n", ok ? "PASS" : "FAIL", name, o.interface,
                o.code, interface, code);
    if (!ok)
        ++g_failed;
}

// ── helpers for the xdg cases ──

/// The newest xdg_surface.configure serial, per xdg_surface (user data).
struct configured {
    uint32_t serial = 0;
};
void on_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<configured*>(data)->serial = serial;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

/// An XRGB shm buffer of w x h (contents irrelevant).
wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-conformance", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/// A complete positioner: 40x30 below a 10x10 anchor rect.
xdg_positioner* good_positioner(client& c) {
    xdg_positioner* p = xdg_wm_base_create_positioner(c.wm_base);
    xdg_positioner_set_size(p, 40, 30);
    xdg_positioner_set_anchor_rect(p, 5, 5, 10, 10);
    xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    return p;
}

/// Commits, waits for the configure, acks it and commits a buffer: maps a toplevel or a
/// popup (whose parent is mapped).
void configure_and_map(client& c, wl_surface* s, xdg_surface* xs, configured& cfg, int w, int h) {
    wl_surface_commit(s);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(xs, cfg.serial);
    wl_surface_attach(s, make_buffer(c, w, h), 0, 0);
    wl_surface_commit(s);
    (void)wl_display_roundtrip(c.display);
}

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    configured cfg{};
};

/// A mapped 200x150 toplevel.
void map_window(client& c, window& w) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    configure_and_map(c, w.surface, w.xdg, w.cfg, 200, 150);
}

struct popup {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_popup* role = nullptr;
    configured cfg{};
};

popup make_popup(client& c, xdg_surface* parent) {
    popup p{};
    p.surface = wl_compositor_create_surface(c.compositor);
    p.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, p.surface);
    xdg_surface_add_listener(p.xdg, &xdg_listener, &p.cfg);
    xdg_positioner* pos = good_positioner(c);
    p.role = xdg_surface_get_popup(p.xdg, parent, pos);
    xdg_positioner_destroy(pos);
    return p;
}

// ── helpers for the layer-shell cases ──

void on_layer_configure(void* data, zwlr_layer_surface_v1*, uint32_t serial, uint32_t,
                        uint32_t) {
    static_cast<configured*>(data)->serial = serial;
}
void on_layer_closed(void*, zwlr_layer_surface_v1*) {}
const zwlr_layer_surface_v1_listener layer_listener = {on_layer_configure, on_layer_closed};

/// Listener storage outliving each case's locals: a configure can arrive in `finish`'s
/// roundtrip, after the case's lambda returned.
configured g_layer_cfg[256]{};
unsigned g_layer_cfg_next = 0;

struct layer {
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* role = nullptr;
    configured* cfg = nullptr;
};

/// A top-layer layer surface (not committed yet).
layer make_layer(client& c, uint32_t level = ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
    layer l{};
    l.cfg = &g_layer_cfg[g_layer_cfg_next++ % 256];
    *l.cfg = {};
    l.surface = wl_compositor_create_surface(c.compositor);
    l.role = zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, l.surface, nullptr, level,
                                                   "conformance");
    zwlr_layer_surface_v1_add_listener(l.role, &layer_listener, l.cfg);
    return l;
}

/// A 100x40 panel anchored top/left/right, committed, configured, acked and mapped.
void map_layer(client& c, layer& l) {
    zwlr_layer_surface_v1_set_size(l.role, 0, 40);
    zwlr_layer_surface_v1_set_anchor(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(l.role, 40);
    wl_surface_commit(l.surface);
    (void)wl_display_roundtrip(c.display);
    zwlr_layer_surface_v1_ack_configure(l.role, l.cfg->serial);
    wl_surface_attach(l.surface, make_buffer(c, 100, 40), 0, 0);
    wl_surface_commit(l.surface);
    (void)wl_display_roundtrip(c.display);
}

// ── helpers for the selection cases ──

/// One connection's view of the clipboard and primary selection: every event in arrival
/// order (so "selection before keyboard enter" can be read off), the keyboard enter serial,
/// the current offers and the source's `send`/`cancelled`.
struct clip {
    client c{};
    wl_keyboard* kb = nullptr;
    uint32_t enter_serial = 0;
    wl_data_device* dd = nullptr;
    wl_data_offer* offer = nullptr;  // the current selection offer (null: none)
    std::vector<std::string> offer_mimes;
    wl_data_source* source = nullptr;
    bool cancelled = false;
#if defined(LUMEN_TEST_PRIMARY_SELECTION)
    zwp_primary_selection_device_v1* pd = nullptr;
    zwp_primary_selection_offer_v1* poffer = nullptr;
    std::vector<std::string> poffer_mimes;
    zwp_primary_selection_source_v1* psource = nullptr;
    bool pcancelled = false;
#endif
    /// Payload a source of this client writes on `send`.
    const char* payload = "";
    std::vector<std::string> events;
    window win{};
};

void kb_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void kb_enter(void* data, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
    auto* k = static_cast<clip*>(data);
    k->enter_serial = serial;
    k->events.push_back("enter");
}
void kb_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    static_cast<clip*>(data)->events.push_back("leave");
}
void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kb_listener = {kb_keymap, kb_enter,    kb_leave,
                                          kb_key,    kb_modifiers, kb_repeat};

/// Mimes of an offer not yet named the selection: data_offer is followed by its offers.
std::vector<std::string> g_pending_mimes;

void offer_offer(void* data, wl_data_offer*, const char* mime) {
    static_cast<clip*>(data)->events.push_back(std::string("offer:") + mime);
    g_pending_mimes.emplace_back(mime);
}
void offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void offer_action(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener offer_listener = {offer_offer, offer_source_actions, offer_action};

void dd_data_offer(void* data, wl_data_device*, wl_data_offer* o) {
    static_cast<clip*>(data)->events.push_back("data_offer");
    g_pending_mimes.clear();
    wl_data_offer_add_listener(o, &offer_listener, data);
}
void dd_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
              wl_data_offer*) {}
void dd_leave(void*, wl_data_device*) {}
void dd_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void dd_drop(void*, wl_data_device*) {}
void dd_selection(void* data, wl_data_device*, wl_data_offer* o) {
    auto* k = static_cast<clip*>(data);
    k->events.push_back(o ? "selection" : "selection:nil");
    if (k->offer && k->offer != o)
        wl_data_offer_destroy(k->offer); // as every toolkit does: the old one is done
    k->offer = o;
    k->offer_mimes = o ? g_pending_mimes : std::vector<std::string>{};
}
const wl_data_device_listener dd_listener = {dd_data_offer, dd_enter, dd_leave,
                                             dd_motion,     dd_drop,  dd_selection};

void src_target(void*, wl_data_source*, const char*) {}
void src_send(void* data, wl_data_source*, const char*, int32_t fd) {
    const char* p = static_cast<clip*>(data)->payload;
    (void)!write(fd, p, std::strlen(p));
    close(fd);
}
void src_cancelled(void* data, wl_data_source*) { static_cast<clip*>(data)->cancelled = true; }
void src_dnd_drop_performed(void*, wl_data_source*) {}
void src_dnd_finished(void*, wl_data_source*) {}
void src_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener src_listener = {src_target, src_send, src_cancelled,
                                              src_dnd_drop_performed, src_dnd_finished,
                                              src_action};

#if defined(LUMEN_TEST_PRIMARY_SELECTION)
void poffer_offer(void* data, zwp_primary_selection_offer_v1*, const char* mime) {
    static_cast<clip*>(data)->events.push_back(std::string("poffer:") + mime);
    g_pending_mimes.emplace_back(mime);
}
const zwp_primary_selection_offer_v1_listener poffer_listener = {poffer_offer};
void pd_data_offer(void* data, zwp_primary_selection_device_v1*,
                   zwp_primary_selection_offer_v1* o) {
    static_cast<clip*>(data)->events.push_back("pdata_offer");
    g_pending_mimes.clear();
    zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, data);
}
void pd_selection(void* data, zwp_primary_selection_device_v1*,
                  zwp_primary_selection_offer_v1* o) {
    auto* k = static_cast<clip*>(data);
    k->events.push_back(o ? "pselection" : "pselection:nil");
    if (k->poffer && k->poffer != o)
        zwp_primary_selection_offer_v1_destroy(k->poffer);
    k->poffer = o;
    k->poffer_mimes = o ? g_pending_mimes : std::vector<std::string>{};
}
const zwp_primary_selection_device_v1_listener pd_listener = {pd_data_offer, pd_selection};
void psrc_send(void* data, zwp_primary_selection_source_v1*, const char*, int32_t fd) {
    const char* p = static_cast<clip*>(data)->payload;
    (void)!write(fd, p, std::strlen(p));
    close(fd);
}
void psrc_cancelled(void* data, zwp_primary_selection_source_v1*) {
    static_cast<clip*>(data)->pcancelled = true;
}
const zwp_primary_selection_source_v1_listener psrc_listener = {psrc_send, psrc_cancelled};
#endif

/// Connects with a keyboard and both selection devices, and maps a window — which takes
/// the keyboard (the placeholder focus policy), so `enter_serial` is set.
bool clip_connect(clip& k) {
    if (!connect(k.c) || !k.c.seat || !k.c.ddm)
        return false;
    k.kb = wl_seat_get_keyboard(k.c.seat);
    wl_keyboard_add_listener(k.kb, &kb_listener, &k);
    k.dd = wl_data_device_manager_get_data_device(k.c.ddm, k.c.seat);
    wl_data_device_add_listener(k.dd, &dd_listener, &k);
#if defined(LUMEN_TEST_PRIMARY_SELECTION)
    if (k.c.psm) {
        k.pd = zwp_primary_selection_device_manager_v1_get_device(k.c.psm, k.c.seat);
        zwp_primary_selection_device_v1_add_listener(k.pd, &pd_listener, &k);
    }
#endif
    (void)wl_display_roundtrip(k.c.display);
    map_window(k.c, k.win);
    (void)wl_display_roundtrip(k.c.display);
    return k.enter_serial != 0;
}

/// A clipboard source offering `mimes` (comma-free list), set as the selection.
void clip_set(clip& k, std::initializer_list<const char*> mimes, uint32_t serial) {
    k.source = wl_data_device_manager_create_data_source(k.c.ddm);
    wl_data_source_add_listener(k.source, &src_listener, &k);
    for (const char* m : mimes)
        wl_data_source_offer(k.source, m);
    wl_data_device_set_selection(k.dd, k.source, serial);
    (void)wl_display_roundtrip(k.c.display);
}

/// Reads what the source writes into a pipe passed by `receive` (offer of `reader`),
/// dispatching the source's connection meanwhile. "" on EOF without data.
template <typename Receive>
std::string clip_read(clip& reader, clip& owner, Receive receive) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return "<pipe failed>";
    receive(fds[1]);
    close(fds[1]);
    (void)wl_display_flush(reader.c.display);
    (void)wl_display_roundtrip(reader.c.display);
    (void)wl_display_roundtrip(owner.c.display);
    std::string out;
    char buf[256];
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0)
            break;
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    return out;
}

/// The index of the first event equal to `e` (or the size when absent).
std::size_t index_of(const std::vector<std::string>& v, const char* e, std::size_t from = 0) {
    for (std::size_t i = from; i < v.size(); ++i)
        if (v[i] == e)
            return i;
    return v.size();
}

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

void clip_disconnect(clip& k) {
    if (k.c.display)
        wl_display_disconnect(k.c.display);
    k.c.display = nullptr;
}

// ── helpers for the drag-and-drop cases ──
// Input goes through the compositor's virtual-input seam on this thread, with the pump held
// off (input and protocol handling are UI-affinity work, never concurrent).

lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_dispatch_lock = nullptr;

void inject_move(double x, double y) {
    std::lock_guard<std::mutex> hold(*g_dispatch_lock);
    lx::input::pointer_motion_event m{};
    m.x = x;
    m.y = y;
    g_comp->inject_pointer_motion(m);
    g_comp->wayland().flush();
}

void inject_left(bool pressed) {
    std::lock_guard<std::mutex> hold(*g_dispatch_lock);
    lx::input::pointer_button_event b{};
    b.which = lx::input::button::left;
    b.evdev_code = 0x110;
    b.pressed = pressed;
    g_comp->inject_pointer_button(b);
    g_comp->wayland().flush();
}

void inject_escape() {
    std::lock_guard<std::mutex> hold(*g_dispatch_lock);
    lx::input::key_event k{};
    k.evdev_keycode = 1;
    k.pressed = true;
    g_comp->inject_key(k);
    k.pressed = false;
    g_comp->inject_key(k);
    g_comp->wayland().flush();
}

/// Geometry of the newest mapped toplevel.
lx::rect2i newest_window() {
    std::lock_guard<std::mutex> hold(*g_dispatch_lock);
    lx::rect2i g{};
    unsigned long long newest = 0;
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.mapped && r.id.id() > newest) {
            newest = r.id.id();
            g = r.geometry;
        }
    });
    return g;
}

[[nodiscard]] bool in_rect(lx::rect2i r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

/// One client in a drag: its window, pointer and data device, the drag's source (when it
/// is the origin) and the events both see, in arrival order. As a target it answers
/// `enter` with accept(`accept`) and set_actions(`actions`, `preferred`) when `respond`.
struct dnd_peer {
    client c{};
    window win{};
    lx::rect2i geometry{};
    wl_pointer* ptr = nullptr;
    uint32_t press_serial = 0;
    wl_data_device* dd = nullptr;
    wl_data_source* source = nullptr;
    wl_data_offer* offer = nullptr; // the drag offer of the last enter
    bool respond = true;
    const char* accept = "text/plain";
    uint32_t actions = 3;
    uint32_t preferred = 2;
    const char* payload = "lumen-dnd-conformance";
    std::vector<std::string> events;
};

void dp_ptr_enter(void* d, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    static_cast<dnd_peer*>(d)->events.push_back("ptr-enter");
}
void dp_ptr_leave(void* d, wl_pointer*, uint32_t, wl_surface*) {
    static_cast<dnd_peer*>(d)->events.push_back("ptr-leave");
}
void dp_ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void dp_ptr_button(void* d, wl_pointer*, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
    auto* p = static_cast<dnd_peer*>(d);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        p->press_serial = serial;
    p->events.push_back(state == WL_POINTER_BUTTON_STATE_PRESSED ? "ptr-press" : "ptr-release");
}
void dp_ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
const wl_pointer_listener dp_ptr_listener = {dp_ptr_enter, dp_ptr_leave, dp_ptr_motion,
                                             dp_ptr_button, dp_ptr_axis};

void dp_offer_offer(void*, wl_data_offer*, const char*) {}
void dp_offer_source_actions(void* d, wl_data_offer*, uint32_t a) {
    static_cast<dnd_peer*>(d)->events.push_back("source_actions:" + std::to_string(a));
}
void dp_offer_action(void* d, wl_data_offer*, uint32_t a) {
    static_cast<dnd_peer*>(d)->events.push_back("offer-action:" + std::to_string(a));
}
const wl_data_offer_listener dp_offer_listener = {dp_offer_offer, dp_offer_source_actions,
                                                  dp_offer_action};

void dp_data_offer(void* d, wl_data_device*, wl_data_offer* o) {
    wl_data_offer_add_listener(o, &dp_offer_listener, d);
}
void dp_enter(void* d, wl_data_device*, uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t,
              wl_data_offer* o) {
    auto* p = static_cast<dnd_peer*>(d);
    p->events.push_back(o ? "enter" : "enter:nil");
    p->offer = o;
    if (o && p->respond) {
        wl_data_offer_accept(o, serial, p->accept);
        wl_data_offer_set_actions(o, p->actions, p->preferred);
    }
}
void dp_leave(void* d, wl_data_device*) { static_cast<dnd_peer*>(d)->events.push_back("leave"); }
void dp_motion(void* d, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {
    auto* p = static_cast<dnd_peer*>(d);
    if (p->events.empty() || p->events.back() != "motion")
        p->events.push_back("motion");
}
void dp_drop(void* d, wl_data_device*) { static_cast<dnd_peer*>(d)->events.push_back("drop"); }
void dp_selection(void* d, wl_data_device*, wl_data_offer* o) {
    // The clipboard's own offers: not this test's business.
    if (o && o != static_cast<dnd_peer*>(d)->offer)
        wl_data_offer_destroy(o);
}
const wl_data_device_listener dp_dd_listener = {dp_data_offer, dp_enter, dp_leave,
                                                dp_motion,     dp_drop,  dp_selection};

void dp_src_target(void* d, wl_data_source*, const char* mime) {
    static_cast<dnd_peer*>(d)->events.push_back(std::string("target:") + (mime ? mime : "nil"));
}
void dp_src_send(void* d, wl_data_source*, const char*, int32_t fd) {
    auto* p = static_cast<dnd_peer*>(d);
    p->events.push_back("send");
    (void)!write(fd, p->payload, std::strlen(p->payload));
    close(fd);
}
void dp_src_cancelled(void* d, wl_data_source*) {
    static_cast<dnd_peer*>(d)->events.push_back("cancelled");
}
void dp_src_drop_performed(void* d, wl_data_source*) {
    static_cast<dnd_peer*>(d)->events.push_back("drop_performed");
}
void dp_src_finished(void* d, wl_data_source*) {
    static_cast<dnd_peer*>(d)->events.push_back("finished");
}
void dp_src_action(void* d, wl_data_source*, uint32_t a) {
    static_cast<dnd_peer*>(d)->events.push_back("action:" + std::to_string(a));
}
const wl_data_source_listener dp_src_listener = {dp_src_target,         dp_src_send,
                                                 dp_src_cancelled,      dp_src_drop_performed,
                                                 dp_src_finished,       dp_src_action};

/// Pointer, data device and a mapped window on `p.c` (connected already).
void dnd_setup(dnd_peer& p) {
    p.ptr = wl_seat_get_pointer(p.c.seat);
    wl_pointer_add_listener(p.ptr, &dp_ptr_listener, &p);
    p.dd = wl_data_device_manager_get_data_device(p.c.ddm, p.c.seat);
    wl_data_device_add_listener(p.dd, &dp_dd_listener, &p);
    (void)wl_display_roundtrip(p.c.display);
    map_window(p.c, p.win);
    (void)wl_display_roundtrip(p.c.display);
    p.geometry = newest_window();
}

/// Presses the left button at window-local (x, y) and reads the press serial.
void dnd_press(dnd_peer& p, int x, int y) {
    inject_move(p.geometry.x + x, p.geometry.y + y);
    inject_left(true);
    (void)wl_display_roundtrip(p.c.display);
}

/// A copy|move source offering text/plain, given to start_drag with the press serial
/// (+`serial_skew`), origin the window.
void dnd_start(dnd_peer& p, uint32_t serial_skew = 0, wl_surface* icon = nullptr) {
    p.source = wl_data_device_manager_create_data_source(p.c.ddm);
    wl_data_source_add_listener(p.source, &dp_src_listener, &p);
    wl_data_source_offer(p.source, "text/plain");
    wl_data_source_set_actions(p.source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                             WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_device_start_drag(p.dd, p.source, p.win.surface, icon, p.press_serial + serial_skew);
    (void)wl_display_roundtrip(p.c.display);
    (void)wl_display_roundtrip(p.c.display);
}

/// A drag started from and over the client's own window: its own data device gets `enter`
/// with the drag's offer (no automatic answer).
void self_drag(dnd_peer& p, client& c) {
    p = {};
    p.c = c;
    p.respond = false;
    dnd_setup(p);
    dnd_press(p, 20, 20);
    dnd_start(p);
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& e : v)
        out += (out.empty() ? "" : " ") + e;
    return out;
}

/// `want` appears in `v` in this order (other events may come between).
bool in_order(const std::vector<std::string>& v, std::initializer_list<const char*> want) {
    std::size_t at = 0;
    for (const char* w : want) {
        while (at < v.size() && v[at] != w)
            ++at;
        if (at == v.size())
            return false;
        ++at;
    }
    return true;
}

#if defined(LUMEN_TEST_ACTIVATION)
// ── helpers for the xdg-activation cases ──

void act_done(void* data, xdg_activation_token_v1*, const char* token) {
    *static_cast<std::string*>(data) = token;
}
const xdg_activation_token_v1_listener act_listener = {act_done};

/// A token from `c`: with `serial` (0: no set_serial) and `surface` when given; the text.
std::string issue_token(client& c, uint32_t serial, wl_surface* surface = nullptr) {
    std::string text;
    xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(c.activation);
    xdg_activation_token_v1_add_listener(t, &act_listener, &text);
    if (serial)
        xdg_activation_token_v1_set_serial(t, serial, c.seat);
    if (surface)
        xdg_activation_token_v1_set_surface(t, surface);
    xdg_activation_token_v1_set_app_id(t, "lumen-conformance");
    xdg_activation_token_v1_commit(t);
    (void)wl_display_roundtrip(c.display);
    xdg_activation_token_v1_destroy(t);
    return text;
}
#endif

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    // Protocol handlers run on the UI affinity; this thread stands in for the UI thread.
    std::atomic<bool> pump{true};
    // Held around each dispatch, so a case can read compositor state between dispatches.
    std::mutex dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            // Wait for client traffic without the lock: a dispatch sleeping in epoll while
            // holding it starved every case that reads compositor state or injects input
            // (std::mutex is not fair), stretching the run from ~15 s to over a minute.
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    // Surfaces that have a scene node, i.e. are drawn.
    const auto drawn_surfaces = [&] {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        unsigned n = 0;
        for (unsigned long long id = 1; id < 4096; ++id)
            if (comp.surfaces().node(lx::surface_id{id}))
                ++n;
        return n;
    };
    // Toplevels drawn with a server-side frame right now.
    const auto server_decorated = [&] {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        unsigned n = 0;
        comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
            if (r.decoration_current == lx::compositor::decoration_mode::server)
                ++n;
        });
        return n;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── zlm_shell_v1: only the configured shell binary may bind it ──
    // The compositor's shell is lumen-shell next to its own executable; this test binary is
    // not it, so the bind is refused with the display's implementation error.
    expect("zlm_shell_v1: bind from a binary that is not the configured shell", "wl_display",
           WL_DISPLAY_ERROR_IMPLEMENTATION, [](client& c) {
               struct found {
                   uint32_t name = 0;
               } f{};
               static const wl_registry_listener find_shell = {
                   [](void* data, wl_registry*, uint32_t name, const char* iface, uint32_t) {
                       if (std::strcmp(iface, zlm_shell_v1_interface.name) == 0)
                           static_cast<found*>(data)->name = name;
                   },
                   [](void*, wl_registry*, uint32_t) {}};
               wl_registry* r = wl_display_get_registry(c.display);
               wl_registry_add_listener(r, &find_shell, &f);
               (void)wl_display_roundtrip(c.display);
               if (f.name)
                   (void)wl_registry_bind(r, f.name, &zlm_shell_v1_interface, 1);
           });

    // ── wl_subcompositor.get_subsurface ──
    expect("subsurface: second wl_subsurface for one surface", "wl_subcompositor",
           WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, [](client& c) {
               wl_surface* parent = wl_compositor_create_surface(c.compositor);
               wl_surface* child = wl_compositor_create_surface(c.compositor);
               wl_subcompositor_get_subsurface(c.subcompositor, child, parent);
               wl_subcompositor_get_subsurface(c.subcompositor, child, parent);
           });
    expect("subsurface: surface with another role (xdg_toplevel)", "wl_subcompositor",
           WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, [](client& c) {
               wl_surface* parent = wl_compositor_create_surface(c.compositor);
               wl_surface* top = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, top);
               (void)xdg_surface_get_toplevel(xs);
               wl_subcompositor_get_subsurface(c.subcompositor, top, parent);
           });
    expect("subsurface: parent is the surface itself", "wl_subcompositor",
           WL_SUBCOMPOSITOR_ERROR_BAD_PARENT, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_subcompositor_get_subsurface(c.subcompositor, s, s);
           });
    expect("subsurface: parent is a descendant", "wl_subcompositor",
           WL_SUBCOMPOSITOR_ERROR_BAD_PARENT, [](client& c) {
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               wl_surface* b = wl_compositor_create_surface(c.compositor);
               wl_surface* d = wl_compositor_create_surface(c.compositor);
               wl_subcompositor_get_subsurface(c.subcompositor, b, a); // b under a
               wl_subcompositor_get_subsurface(c.subcompositor, d, b); // d under b
               wl_subcompositor_get_subsurface(c.subcompositor, a, d); // a under its grandchild
           });

    // ── wl_subsurface.place_above / place_below ──
    expect("subsurface: place_above a non-sibling", "wl_subsurface",
           WL_SUBSURFACE_ERROR_BAD_SURFACE, [](client& c) {
               wl_surface* p = wl_compositor_create_surface(c.compositor);
               wl_surface* q = wl_compositor_create_surface(c.compositor);
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               wl_surface* b = wl_compositor_create_surface(c.compositor);
               wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               (void)wl_subcompositor_get_subsurface(c.subcompositor, b, q);
               wl_subsurface_place_above(sa, b);
           });
    expect("subsurface: place_below itself", "wl_subsurface", WL_SUBSURFACE_ERROR_BAD_SURFACE,
           [](client& c) {
               wl_surface* p = wl_compositor_create_surface(c.compositor);
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               wl_subsurface_place_below(sa, a);
           });
    expect("subsurface: place relative to parent and sibling is fine", "none", 0, [](client& c) {
        wl_surface* p = wl_compositor_create_surface(c.compositor);
        wl_surface* a = wl_compositor_create_surface(c.compositor);
        wl_surface* b = wl_compositor_create_surface(c.compositor);
        wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
        wl_subsurface* sb = wl_subcompositor_get_subsurface(c.subcompositor, b, p);
        wl_subsurface_place_below(sa, p);
        wl_subsurface_place_above(sb, a);
        wl_subsurface_set_position(sb, -4, 7);
        wl_subsurface_set_desync(sa);
        wl_surface_commit(b);
        wl_surface_commit(a);
        wl_surface_commit(p);
    });

    // ── wl_surface: scale, transform, offset, size ──
    expect("surface: set_buffer_scale 0", "wl_surface", WL_SURFACE_ERROR_INVALID_SCALE,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_set_buffer_scale(s, 0);
           });
    expect("surface: set_buffer_transform 8", "wl_surface", WL_SURFACE_ERROR_INVALID_TRANSFORM,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_set_buffer_transform(s, 8);
           });
    expect("surface: v5+ attach with a non-zero offset", "wl_surface",
           WL_SURFACE_ERROR_INVALID_OFFSET, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_attach(s, make_buffer(c, 10, 10), 3, 0);
           });
    expect("surface: toplevel buffer not divisible by its scale", "wl_surface",
           WL_SURFACE_ERROR_INVALID_SIZE, [](client& c) {
               window w{};
               w.surface = wl_compositor_create_surface(c.compositor);
               w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
               xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
               w.toplevel = xdg_surface_get_toplevel(w.xdg);
               wl_surface_commit(w.surface);
               (void)wl_display_roundtrip(c.display);
               xdg_surface_ack_configure(w.xdg, w.cfg.serial);
               wl_surface_set_buffer_scale(w.surface, 2);
               wl_surface_attach(w.surface, make_buffer(c, 201, 150), 0, 0);
               wl_surface_commit(w.surface);
           });
    expect("surface: role-less buffer not divisible by its scale is tolerated", "none", 0,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_set_buffer_scale(s, 2);
               wl_surface_attach(s, make_buffer(c, 33, 33), 0, 0);
               wl_surface_commit(s);
           });
    expect("surface: scale 2 and every transform, damage in both spaces", "none", 0,
           [](client& c) {
               for (int t = 0; t < 8; ++t) {
                   wl_surface* s = wl_compositor_create_surface(c.compositor);
                   wl_surface_set_buffer_scale(s, 2);
                   wl_surface_set_buffer_transform(s, t);
                   wl_surface_attach(s, make_buffer(c, 40, 20), 0, 0);
                   wl_surface_damage(s, 0, 0, INT32_MAX, INT32_MAX);
                   wl_surface_damage_buffer(s, 3, 4, 5, 6);
                   wl_surface_commit(s);
                   wl_surface_offset(s, 2, -2);
                   wl_surface_commit(s);
               }
           });

#if defined(LUMEN_TEST_VIEWPORTER)
    // ── wp_viewporter ──
    expect("viewporter: second viewport for a surface", "wp_viewporter",
           WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)wp_viewporter_get_viewport(c.viewporter, s);
               (void)wp_viewporter_get_viewport(c.viewporter, s);
           });
    expect("viewport: source with a negative x", "wp_viewport", WP_VIEWPORT_ERROR_BAD_VALUE,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wp_viewport_set_source(v, wl_fixed_from_int(-1), 0, wl_fixed_from_int(10),
                                      wl_fixed_from_int(10));
           });
    expect("viewport: source with zero width", "wp_viewport", WP_VIEWPORT_ERROR_BAD_VALUE,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wp_viewport_set_source(v, 0, 0, 0, wl_fixed_from_int(10));
           });
    expect("viewport: destination 0x5", "wp_viewport", WP_VIEWPORT_ERROR_BAD_VALUE,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wp_viewport_set_destination(v, 0, 5);
           });
    expect("viewport: destination -1x5 (only -1x-1 unsets)", "wp_viewport",
           WP_VIEWPORT_ERROR_BAD_VALUE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wp_viewport_set_destination(v, -1, 5);
           });
    expect("viewport: fractional source size without a destination", "wp_viewport",
           WP_VIEWPORT_ERROR_BAD_SIZE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_attach(s, make_buffer(c, 40, 40), 0, 0);
               wp_viewport_set_source(v, 0, 0, wl_fixed_from_double(10.5),
                                      wl_fixed_from_int(10));
               wl_surface_commit(s);
           });
    expect("viewport: source past the buffer", "wp_viewport", WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_attach(s, make_buffer(c, 40, 40), 0, 0);
               wp_viewport_set_source(v, wl_fixed_from_int(30), 0, wl_fixed_from_int(11),
                                      wl_fixed_from_int(10));
               wl_surface_commit(s);
           });
    expect("viewport: source past a scale-2, 90-degree buffer", "wp_viewport",
           WP_VIEWPORT_ERROR_OUT_OF_BUFFER, [](client& c) {
               // 80x40 at scale 2 turned by 90 is 20x40 in surface units: 21 wide is out.
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_set_buffer_scale(s, 2);
               wl_surface_set_buffer_transform(s, WL_OUTPUT_TRANSFORM_90);
               wl_surface_attach(s, make_buffer(c, 80, 40), 0, 0);
               wp_viewport_set_source(v, 0, 0, wl_fixed_from_int(21), wl_fixed_from_int(40));
               wl_surface_commit(s);
           });
    expect("viewport: exactly the scale-2, 90-degree buffer is inside", "none", 0,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_set_buffer_scale(s, 2);
               wl_surface_set_buffer_transform(s, WL_OUTPUT_TRANSFORM_90);
               wl_surface_attach(s, make_buffer(c, 80, 40), 0, 0);
               wp_viewport_set_source(v, 0, 0, wl_fixed_from_int(20), wl_fixed_from_int(40));
               wp_viewport_set_destination(v, 33, 17);
               wl_surface_commit(s);
           });
    expect("viewport: request after its wl_surface is gone", "wp_viewport",
           WP_VIEWPORT_ERROR_NO_SURFACE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_destroy(s);
               wp_viewport_set_destination(v, 10, 10);
           });
    expect("viewport: destroyed before the commit — its state goes with it", "none", 0,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_attach(s, make_buffer(c, 40, 40), 0, 0);
               wp_viewport_set_source(v, 0, 0, wl_fixed_from_double(10.5),
                                      wl_fixed_from_int(10));
               wp_viewport_destroy(v);
               wl_surface_commit(s);
               // And a new one may be created.
               (void)wp_viewporter_get_viewport(c.viewporter, s);
           });
    expect("viewport: -1 everywhere unsets the source", "none", 0, [](client& c) {
        wl_surface* s = wl_compositor_create_surface(c.compositor);
        wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
        wl_surface_attach(s, make_buffer(c, 40, 40), 0, 0);
        wp_viewport_set_source(v, 0, 0, wl_fixed_from_int(99), wl_fixed_from_int(99));
        const wl_fixed_t unset = wl_fixed_from_int(-1);
        wp_viewport_set_source(v, unset, unset, unset, unset);
        wp_viewport_set_destination(v, -1, -1);
        wl_surface_commit(s);
    });
    expect("viewport: a viewport surviving its wl_surface may still be destroyed", "none", 0,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, s);
               wl_surface_destroy(s);
               wp_viewport_destroy(v);
           });
    {
        // A toplevel whose buffer is 1.25x its viewport destination maps at the destination
        // size: the window geometry is in surface coordinates, as a fractional-scale client
        // expects.
        unsigned gw = 0, gh = 0;
        expect("viewport: toplevel geometry is the destination size", "none", 0,
               [&](client& c) {
                   window w{};
                   w.surface = wl_compositor_create_surface(c.compositor);
                   wp_viewport* v = wp_viewporter_get_viewport(c.viewporter, w.surface);
                   w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
                   xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
                   w.toplevel = xdg_surface_get_toplevel(w.xdg);
                   xdg_toplevel_set_app_id(w.toplevel, "conformance-viewport");
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   xdg_surface_ack_configure(w.xdg, w.cfg.serial);
                   wp_viewport_set_destination(v, 200, 120);
                   wl_surface_attach(w.surface, make_buffer(c, 250, 150), 0, 0);
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   std::lock_guard<std::mutex> hold(dispatch_lock);
                   comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
                       if (r.app_id && std::strcmp(r.app_id, "conformance-viewport") == 0) {
                           gw = static_cast<unsigned>(r.geometry.width);
                           gh = static_cast<unsigned>(r.geometry.height);
                       }
                   });
               });
        const bool ok = gw == 200 && gh == 120;
        ++g_ran;
        std::printf("  %s  viewport: mapped at the destination size (%ux%u)\n",
                    ok ? "PASS" : "FAIL", gw, gh);
        if (!ok)
            ++g_failed;
    }

    // ── wp_fractional_scale_v1 ──
    expect("fractional scale: second object for a surface", "wp_fractional_scale_manager_v1",
           WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, s);
               (void)wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, s);
           });
    {
        // The first preferred_scale arrives with the object, before the first configure,
        // so the first frame is already drawn at the right scale (120: the test output is
        // at scale 1).
        struct order {
            std::vector<std::string> events;
            uint32_t scale = 0;
        } o{};
        static const wp_fractional_scale_v1_listener frac_listener = {
            [](void* data, wp_fractional_scale_v1*, uint32_t scale) {
                auto* p = static_cast<order*>(data);
                p->scale = scale;
                p->events.emplace_back("preferred");
            }};
        static const xdg_surface_listener order_xdg = {
            [](void* data, xdg_surface* xs, uint32_t serial) {
                static_cast<order*>(data)->events.emplace_back("configure");
                xdg_surface_ack_configure(xs, serial);
            }};
        expect("fractional scale: destroy and recreate is fine", "none", 0, [&](client& c) {
            wl_surface* s = wl_compositor_create_surface(c.compositor);
            wp_fractional_scale_v1* f =
                wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, s);
            wp_fractional_scale_v1_add_listener(f, &frac_listener, &o);
            xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
            xdg_surface_add_listener(xs, &order_xdg, &o);
            (void)xdg_surface_get_toplevel(xs);
            wl_surface_commit(s);
            (void)wl_display_roundtrip(c.display);
            wp_fractional_scale_v1_destroy(f);
            (void)wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, s);
        });
        const bool ok = o.events.size() >= 2 && o.events[0] == "preferred" &&
                        o.events[1] == "configure" && o.scale == 120;
        ++g_ran;
        std::printf("  %s  fractional scale: preferred_scale (%u) before the first configure\n",
                    ok ? "PASS" : "FAIL", o.scale);
        if (!ok)
            ++g_failed;
    }
#endif

#if defined(LUMEN_TEST_PRESENTATION)
    // ── wp_presentation ──
    // No request can raise an error; what matters is the clock and which feedback is
    // discarded. Presented feedback needs a flip (KMS), covered by the presentation probe
    // in app-smoke; here nothing is ever published, so nothing is sampled either.
    {
        struct fb_log {
            std::vector<std::string> events;
        };
        static const wp_presentation_feedback_listener fb_listener = {
            [](void*, struct wp_presentation_feedback*, wl_output*) {},
            [](void* data, struct wp_presentation_feedback* f, uint32_t, uint32_t, uint32_t, uint32_t,
               uint32_t, uint32_t, uint32_t) {
                static_cast<fb_log*>(data)->events.emplace_back("presented");
                wp_presentation_feedback_destroy(f);
            },
            [](void* data, struct wp_presentation_feedback* f) {
                static_cast<fb_log*>(data)->events.emplace_back("discarded");
                wp_presentation_feedback_destroy(f);
            }};
        const auto watch = [](wp_presentation* p, wl_surface* s, fb_log& log) {
            struct wp_presentation_feedback* f = wp_presentation_feedback(p, s);
            wp_presentation_feedback_add_listener(f, &fb_listener, &log);
        };

        uint32_t clock = ~0u;
        expect("presentation: bind", "none", 0, [&](client& c) {
            (void)wl_display_roundtrip(c.display);
            clock = c.clock_id;
        });
        check("presentation: clock_id is CLOCK_MONOTONIC on bind", clock == CLOCK_MONOTONIC,
              "clock_id " + std::to_string(clock));

        fb_log first{}, second{};
        expect("presentation: feedback on a superseded commit", "none", 0, [&](client& c) {
            window w{};
            map_window(c, w);
            watch(c.presentation, w.surface, first);
            wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
            wl_surface_damage(w.surface, 0, 0, 200, 150);
            wl_surface_commit(w.surface);
            watch(c.presentation, w.surface, second);
            wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
            wl_surface_damage(w.surface, 0, 0, 200, 150);
            wl_surface_commit(w.surface);
            (void)wl_display_roundtrip(c.display);
        });
        check("presentation: a commit superseded before it was shown is discarded",
              first.events.size() == 1 && first.events[0] == "discarded" &&
                  second.events.empty(),
              "first " + std::to_string(first.events.size()) + " event(s), second " +
                  std::to_string(second.events.size()));

        fb_log gone{};
        expect("presentation: feedback whose surface is destroyed", "none", 0, [&](client& c) {
            wl_surface* s = wl_compositor_create_surface(c.compositor);
            watch(c.presentation, s, gone); // pending: before any commit
            wl_surface_destroy(s);
            (void)wl_display_roundtrip(c.display);
        });
        check("presentation: a destroyed surface's feedback is discarded",
              gone.events.size() == 1 && gone.events[0] == "discarded",
              std::to_string(gone.events.size()) + " event(s)");

        fb_log cached{}, applied{};
        expect("presentation: synchronized subsurface cache replaced", "none", 0,
               [&](client& c) {
                   window w{};
                   map_window(c, w);
                   wl_surface* a = wl_compositor_create_surface(c.compositor);
                   (void)wl_subcompositor_get_subsurface(c.subcompositor, a, w.surface);
                   watch(c.presentation, a, cached);
                   wl_surface_attach(a, make_buffer(c, 20, 20), 0, 0);
                   wl_surface_commit(a); // cached, waiting for the parent
                   watch(c.presentation, a, applied);
                   wl_surface_attach(a, make_buffer(c, 20, 20), 0, 0);
                   wl_surface_commit(a); // replaces the cached update
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
               });
        check("presentation: a cached update replaced before its parent's commit is discarded",
              cached.events.size() == 1 && cached.events[0] == "discarded" &&
                  applied.events.empty(),
              "replaced " + std::to_string(cached.events.size()) + " event(s), applied " +
                  std::to_string(applied.events.size()));
    }
#endif

    // ── wl_surface.destroy before the role object ──
    expect("surface: destroyed before its wl_subsurface", "[destroyed]",
           WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [](client& c) {
               wl_surface* p = wl_compositor_create_surface(c.compositor);
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               (void)wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               wl_surface_destroy(a);
           });
    expect("surface: destroyed before its xdg_toplevel", "[destroyed]",
           WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               (void)xdg_surface_get_toplevel(xs);
               wl_surface_destroy(s);
           });
    expect("surface: parent destroyed before the child's wl_subsurface is fine", "none", 0,
           [](client& c) {
               wl_surface* p = wl_compositor_create_surface(c.compositor);
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               wl_surface_commit(a); // cached: synchronized to a parent about to vanish
               wl_surface_destroy(p);
               wl_subsurface_set_position(sa, 1, 1); // inert, not an error
               wl_subsurface_destroy(sa);
               wl_surface_destroy(a);
           });
    expect("surface: role object first, then surface, then re-link is fine", "none", 0,
           [](client& c) {
               wl_surface* p = wl_compositor_create_surface(c.compositor);
               wl_surface* a = wl_compositor_create_surface(c.compositor);
               wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               wl_subsurface_destroy(sa);
               // The role stays; a new wl_subsurface for the same surface is allowed.
               sa = wl_subcompositor_get_subsurface(c.subcompositor, a, p);
               wl_subsurface_destroy(sa);
               wl_surface_destroy(a);
               wl_surface_destroy(p);
           });

    // ── content applied before the surface could be drawn ──
    // "A sub-surface becomes mapped, when a non-NULL wl_buffer is applied and the parent
    // surface is mapped" — in either order: content committed while the surface had no
    // role, or while it was unlinked, is shown once it is linked and its parent commits,
    // without a new attach.
    {
        bool drawn_ok = true;
        expect("subsurface: content committed before get_subsurface or re-link is fine", "none",
               0, [&](client& c) {
                   window w{};
                   map_window(c, w);
                   wl_surface* a = wl_compositor_create_surface(c.compositor);
                   wl_surface_attach(a, make_buffer(c, 50, 20), 0, 0);
                   wl_surface_commit(a); // no role yet
                   (void)wl_display_roundtrip(c.display);
                   const unsigned before = drawn_surfaces();
                   wl_subsurface* sa = wl_subcompositor_get_subsurface(c.subcompositor, a, w.surface);
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   drawn_ok = drawn_surfaces() == before + 1;
                   // Unlinked, then new content, then linked again.
                   wl_subsurface_destroy(sa);
                   wl_surface_attach(a, make_buffer(c, 60, 20), 0, 0);
                   wl_surface_commit(a);
                   (void)wl_display_roundtrip(c.display);
                   drawn_ok = drawn_ok && drawn_surfaces() == before;
                   sa = wl_subcompositor_get_subsurface(c.subcompositor, a, w.surface);
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   drawn_ok = drawn_ok && drawn_surfaces() == before + 1;
               });
        ++g_ran;
        std::printf("  %s  subsurface: that content is drawn once linked under a mapped "
                    "parent\n",
                    drawn_ok ? "PASS" : "FAIL");
        if (!drawn_ok)
            ++g_failed;
    }

    // ── xdg_positioner ──
    expect("positioner: set_size 0", "xdg_positioner", XDG_POSITIONER_ERROR_INVALID_INPUT,
           [](client& c) { xdg_positioner_set_size(xdg_wm_base_create_positioner(c.wm_base), 0, 10); });
    expect("positioner: set_anchor_rect with a negative width", "xdg_positioner",
           XDG_POSITIONER_ERROR_INVALID_INPUT, [](client& c) {
               xdg_positioner_set_anchor_rect(xdg_wm_base_create_positioner(c.wm_base), 0, 0, -1, 5);
           });
    expect("positioner: set_anchor out of range", "xdg_positioner",
           XDG_POSITIONER_ERROR_INVALID_INPUT,
           [](client& c) { xdg_positioner_set_anchor(xdg_wm_base_create_positioner(c.wm_base), 9); });
    expect("positioner: set_gravity out of range", "xdg_positioner",
           XDG_POSITIONER_ERROR_INVALID_INPUT,
           [](client& c) { xdg_positioner_set_gravity(xdg_wm_base_create_positioner(c.wm_base), 9); });

    // ── xdg_surface.get_popup ──
    expect("popup: get_popup with an incomplete positioner", "xdg_wm_base",
           XDG_WM_BASE_ERROR_INVALID_POSITIONER, [](client& c) {
               window w{};
               map_window(c, w);
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               xdg_positioner* p = xdg_wm_base_create_positioner(c.wm_base);
               xdg_positioner_set_size(p, 10, 10); // no anchor rect
               (void)xdg_surface_get_popup(xs, w.xdg, p);
           });
    expect("popup: parent is the popup's own xdg_surface", "xdg_wm_base",
           XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               (void)xdg_surface_get_popup(xs, xs, good_positioner(c));
           });
    expect("popup: buffer before the first ack", "xdg_surface",
           XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, [](client& c) {
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               wl_surface_commit(p.surface);
               wl_surface_attach(p.surface, make_buffer(c, 40, 30), 0, 0);
               wl_surface_commit(p.surface);
           });
    expect("popup: ack of a serial never sent", "xdg_surface", XDG_SURFACE_ERROR_INVALID_SERIAL,
           [](client& c) {
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               wl_surface_commit(p.surface);
               (void)wl_display_roundtrip(c.display);
               xdg_surface_ack_configure(p.xdg, p.cfg.serial + 1000);
           });
    expect("popup: maps and is destroyed cleanly", "none", 0, [](client& c) {
        window w{};
        map_window(c, w);
        popup p = make_popup(c, w.xdg);
        configure_and_map(c, p.surface, p.xdg, p.cfg, 40, 30);
        xdg_popup_destroy(p.role);
        xdg_surface_destroy(p.xdg);
        wl_surface_destroy(p.surface);
    });
    expect("popup: grab after map", "xdg_popup", XDG_POPUP_ERROR_INVALID_GRAB, [](client& c) {
        window w{};
        map_window(c, w);
        popup p = make_popup(c, w.xdg);
        configure_and_map(c, p.surface, p.xdg, p.cfg, 40, 30);
        xdg_popup_grab(p.role, c.seat, 1);
    });
    expect("popup: grabbing child of a popup that took no grab", "xdg_wm_base",
           XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, [](client& c) {
               window w{};
               map_window(c, w);
               popup a = make_popup(c, w.xdg);
               configure_and_map(c, a.surface, a.xdg, a.cfg, 40, 30);
               popup b = make_popup(c, a.xdg);
               xdg_popup_grab(b.role, c.seat, 1);
           });
    expect("popup: destroying a popup that has a child popup", "xdg_wm_base",
           XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP, [](client& c) {
               window w{};
               map_window(c, w);
               popup a = make_popup(c, w.xdg);
               configure_and_map(c, a.surface, a.xdg, a.cfg, 40, 30);
               (void)make_popup(c, a.xdg);
               xdg_popup_destroy(a.role);
           });
    expect("popup: xdg_surface destroyed before its xdg_popup", "[destroyed]",
           XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [](client& c) {
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               xdg_surface_destroy(p.xdg);
           });
    expect("popup: wl_surface destroyed before its xdg_popup", "[destroyed]",
           WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [](client& c) {
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               wl_surface_destroy(p.surface);
           });
    expect("popup: a denied grab (stale serial) is popup_done, not an error", "none", 0,
           [](client& c) {
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               xdg_popup_grab(p.role, c.seat, 12345);
               wl_surface_commit(p.surface); // inert after popup_done; no configure owed
               (void)wl_display_roundtrip(c.display);
               xdg_popup_destroy(p.role);
               xdg_surface_destroy(p.xdg);
               wl_surface_destroy(p.surface);
           });

    // ── xdg_wm_base.destroy ──
    expect("wm_base: destroyed with an xdg_surface alive", "[destroyed]",
           XDG_WM_BASE_ERROR_DEFUNCT_SURFACES, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)xdg_wm_base_get_xdg_surface(c.wm_base, s);
               xdg_wm_base_destroy(c.wm_base);
           });
    expect("wm_base: destroyed after its xdg_surfaces is fine", "none", 0, [](client& c) {
        wl_surface* s = wl_compositor_create_surface(c.compositor);
        xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
        xdg_surface_destroy(xs);
        xdg_wm_base_destroy(c.wm_base);
    });

    // ── zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1 ──
    expect("decoration: second decoration object for one toplevel", "zxdg_toplevel_decoration_v1",
           ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               xdg_toplevel* t = xdg_surface_get_toplevel(xs);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, t);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, t);
           });
    expect("decoration: set_mode with an invalid mode", "zxdg_toplevel_decoration_v1",
           ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               xdg_toplevel* t = xdg_surface_get_toplevel(xs);
               zxdg_toplevel_decoration_v1* d =
                   zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, t);
               zxdg_toplevel_decoration_v1_set_mode(d, 3);
           });
    expect("decoration: xdg_toplevel destroyed before its decoration", "zxdg_toplevel_decoration_v1",
           ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED, [](client& c) {
               window w{};
               map_window(c, w);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
               xdg_toplevel_destroy(w.toplevel);
           });
    expect("decoration: v2 object on a mapped toplevel, mode changes, destroy order ok", "none", 0,
           [](client& c) {
               window w{};
               map_window(c, w);
               zxdg_toplevel_decoration_v1* d =
                   zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
               zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
               zxdg_toplevel_decoration_v1_unset_mode(d);
               zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
               (void)wl_display_roundtrip(c.display);
               zxdg_toplevel_decoration_v1_destroy(d);
               wl_surface_commit(w.surface);
               xdg_toplevel_destroy(w.toplevel);
               xdg_surface_destroy(w.xdg);
               wl_surface_destroy(w.surface);
           });
    {
        // The configure answers the policy: server-side when asked (or unset), client-side
        // when the client insists — and it precedes the xdg_surface.configure it belongs to.
        struct seen {
            uint32_t mode = 0;
            uint32_t serial = 0;
            bool order_ok = true;
        };
        static seen got{};
        static const zxdg_toplevel_decoration_v1_listener deco_listener = {
            [](void*, zxdg_toplevel_decoration_v1*, uint32_t mode) {
                got.mode = mode;
                got.serial = 0; // the xdg_surface.configure must come after
            }};
        static const xdg_surface_listener order_listener = {
            [](void*, xdg_surface*, uint32_t serial) { got.serial = serial; }};
        bool ok = false;
        expect("decoration: configure carries the policy's mode", "none", 0, [&](client& c) {
            wl_surface* s = wl_compositor_create_surface(c.compositor);
            xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
            xdg_surface_add_listener(xs, &order_listener, nullptr);
            xdg_toplevel* t = xdg_surface_get_toplevel(xs);
            zxdg_toplevel_decoration_v1* d =
                zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, t);
            zxdg_toplevel_decoration_v1_add_listener(d, &deco_listener, nullptr);
            zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            wl_surface_commit(s);
            (void)wl_display_roundtrip(c.display);
            const bool server = got.mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE &&
                                got.serial != 0;
            // Map, so nothing is in flight, then ask for client-side: answered at once.
            xdg_surface_ack_configure(xs, got.serial);
            wl_surface_attach(s, make_buffer(c, 120, 80), 0, 0);
            wl_surface_commit(s);
            (void)wl_display_roundtrip(c.display);
            got = {};
            zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
            (void)wl_display_roundtrip(c.display);
            const bool client_side = got.mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
                                     got.serial != 0;
            ok = server && client_side;
            zxdg_toplevel_decoration_v1_destroy(d);
            xdg_toplevel_destroy(t);
            xdg_surface_destroy(xs);
            wl_surface_destroy(s);
        });
        ++g_ran;
        std::printf("  %s  decoration: server-side, then client-side on request\n",
                    ok ? "PASS" : "FAIL");
        if (!ok)
            ++g_failed;
    }

    {
        // "Switch back to a mode without any server-side decorations at the next commit" —
        // and a configure that carried server-side, acked after the object is gone, must
        // not turn the frame back on.
        bool ok = false;
        expect("decoration: destroyed with a server-side configure in flight", "none", 0,
               [&](client& c) {
                   window w{};
                   map_window(c, w);
                   zxdg_toplevel_decoration_v1* d =
                       zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
                   (void)wl_display_roundtrip(c.display); // server-side configure, in flight
                   const uint32_t in_flight = w.cfg.serial;
                   zxdg_toplevel_decoration_v1_destroy(d);
                   wl_surface_commit(w.surface);
                   xdg_surface_ack_configure(w.xdg, in_flight);
                   wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   const bool none_after_destroy = server_decorated() == 0;
                   // A new object is configured server-side again, and that does apply.
                   d = zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
                   (void)wl_display_roundtrip(c.display);
                   xdg_surface_ack_configure(w.xdg, w.cfg.serial);
                   wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
                   wl_surface_commit(w.surface);
                   (void)wl_display_roundtrip(c.display);
                   ok = none_after_destroy && w.cfg.serial != in_flight &&
                        server_decorated() == 1;
               });
        ++g_ran;
        std::printf("  %s  decoration: no frame after destroy, even for an older configure\n",
                    ok ? "PASS" : "FAIL");
        if (!ok)
            ++g_failed;
    }

    g_deco_version = 1;
    expect("decoration v1: created on a toplevel that has a buffer", "zxdg_toplevel_decoration_v1",
           ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER, [](client& c) {
               window w{};
               map_window(c, w);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
           });
    expect("decoration v1: buffer attached before the decoration configure",
           "zxdg_toplevel_decoration_v1", ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               xdg_toplevel* t = xdg_surface_get_toplevel(xs);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, t);
               wl_surface_attach(s, make_buffer(c, 50, 50), 0, 0);
           });
    g_deco_version = 2;
    expect("decoration v2: created on a toplevel that has a buffer is fine", "none", 0,
           [](client& c) {
               window w{};
               map_window(c, w);
               (void)zxdg_decoration_manager_v1_get_toplevel_decoration(c.deco, w.toplevel);
               (void)wl_display_roundtrip(c.display);
           });

    // ── zwlr_layer_shell_v1 / zwlr_layer_surface_v1 ──
    expect("layer: surface with another role (xdg_toplevel)", "zwlr_layer_shell_v1",
           ZWLR_LAYER_SHELL_V1_ERROR_ROLE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, s);
               (void)xdg_surface_get_toplevel(xs);
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 2, "x");
           });
    expect("layer: second layer surface for one wl_surface", "zwlr_layer_shell_v1",
           ZWLR_LAYER_SHELL_V1_ERROR_ROLE, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 2, "x");
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 2, "x");
           });
    expect("layer: surface has a buffer attached", "zwlr_layer_shell_v1",
           ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_attach(s, make_buffer(c, 10, 10), 0, 0);
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 2, "x");
           });
    expect("layer: surface has a buffer committed", "zwlr_layer_shell_v1",
           ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED, [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               wl_surface_attach(s, make_buffer(c, 10, 10), 0, 0);
               wl_surface_commit(s);
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 2, "x");
           });
    expect("layer: invalid layer", "zwlr_layer_shell_v1", ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
           [](client& c) {
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, s, nullptr, 4, "x");
           });
    expect("layer: set_layer with an invalid layer", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_layer(l.role, 7);
           });
    expect("layer: invalid anchor", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_anchor(l.role, 16);
           });
    expect("layer: invalid keyboard interactivity", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_keyboard_interactivity(l.role, 3);
           });
    g_layer_version = 3;
    expect("layer v3: on_demand did not exist yet", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_keyboard_interactivity(
                   l.role, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
           });
    g_layer_version = 5;
    expect("layer: on_demand on v4+ is fine", "none", 0, [](client& c) {
        layer l = make_layer(c);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            l.role, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
        map_layer(c, l);
    });
    expect("layer: width 0 without left and right anchors", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_size(l.role, 0, 32);
               zwlr_layer_surface_v1_set_anchor(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
               wl_surface_commit(l.surface);
           });
    expect("layer: height 0 without top and bottom anchors", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_size(l.role, 32, 0);
               zwlr_layer_surface_v1_set_anchor(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
               wl_surface_commit(l.surface);
           });
    expect("layer: exclusive edge that is not a single edge", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_exclusive_edge(l.role,
                                                        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
           });
    expect("layer: exclusive edge not anchored, at commit", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_size(l.role, 50, 50);
               zwlr_layer_surface_v1_set_anchor(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
               zwlr_layer_surface_v1_set_exclusive_edge(l.role,
                                                        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
               wl_surface_commit(l.surface);
           });
    expect("layer: exclusive edge disambiguating a corner is fine", "none", 0, [](client& c) {
        layer l = make_layer(c);
        zwlr_layer_surface_v1_set_size(l.role, 50, 50);
        zwlr_layer_surface_v1_set_anchor(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_exclusive_zone(l.role, 50);
        zwlr_layer_surface_v1_set_exclusive_edge(l.role, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        wl_surface_commit(l.surface);
    });
    expect("layer: buffer attached before the first configure was acked",
           "zwlr_layer_surface_v1", ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
           [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_size(l.role, 50, 50);
               wl_surface_commit(l.surface);
               (void)wl_display_roundtrip(c.display);
               wl_surface_attach(l.surface, make_buffer(c, 50, 50), 0, 0);
               wl_surface_commit(l.surface);
           });
    expect("layer: ack_configure with a serial never sent", "zwlr_layer_surface_v1",
           ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE, [](client& c) {
               layer l = make_layer(c);
               zwlr_layer_surface_v1_set_size(l.role, 50, 50);
               wl_surface_commit(l.surface);
               (void)wl_display_roundtrip(c.display);
               zwlr_layer_surface_v1_ack_configure(l.role, l.cfg->serial + 1000);
           });
    expect("layer: wl_surface destroyed before its layer surface", "[destroyed]",
           WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [](client& c) {
               layer l = make_layer(c);
               wl_surface_destroy(l.surface);
           });
    expect("layer: get_popup on a popup that made its initial commit", "xdg_wm_base",
           XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, [](client& c) {
               layer l = make_layer(c);
               map_layer(c, l);
               window w{};
               map_window(c, w);
               popup p = make_popup(c, w.xdg);
               wl_surface_commit(p.surface);
               (void)wl_display_roundtrip(c.display);
               zwlr_layer_surface_v1_get_popup(l.role, p.role);
           });
    expect("layer: popup without a parent committed before get_popup", "xdg_wm_base",
           XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, [](client& c) {
               popup p = make_popup(c, nullptr);
               wl_surface_commit(p.surface);
           });
    expect("layer: panel maps, a popup of it maps, null buffer unmaps, remaps; teardown in "
           "order",
           "none", 0, [](client& c) {
               layer l = make_layer(c);
               map_layer(c, l);
               popup p = make_popup(c, nullptr);
               zwlr_layer_surface_v1_get_popup(l.role, p.role);
               configure_and_map(c, p.surface, p.xdg, p.cfg, 40, 30);
               xdg_popup_destroy(p.role);
               xdg_surface_destroy(p.xdg);
               wl_surface_destroy(p.surface);
               wl_surface_attach(l.surface, nullptr, 0, 0);
               wl_surface_commit(l.surface);
               (void)wl_display_roundtrip(c.display);
               // Unmapped: back to "right after get_layer_surface" — a new initial commit.
               wl_surface_commit(l.surface);
               (void)wl_display_roundtrip(c.display);
               zwlr_layer_surface_v1_ack_configure(l.role, l.cfg->serial);
               wl_surface_attach(l.surface, make_buffer(c, 100, 40), 0, 0);
               wl_surface_commit(l.surface);
               (void)wl_display_roundtrip(c.display);
               zwlr_layer_surface_v1_destroy(l.role);
               wl_surface_destroy(l.surface);
           });
    expect("layer: disconnect with a mapped panel and its popup alive", "none", 0,
           [](client& c) {
               layer l = make_layer(c);
               map_layer(c, l);
               popup p = make_popup(c, nullptr);
               zwlr_layer_surface_v1_get_popup(l.role, p.role);
               configure_and_map(c, p.surface, p.xdg, p.cfg, 40, 30);
           });

    // ── wl_data_device / primary selection: protocol errors ──
    expect("data device: set_selection with a source already used", "wl_data_device",
           WL_DATA_DEVICE_ERROR_USED_SOURCE, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_source_offer(src, "text/plain");
               wl_data_device_set_selection(dd, src, 1); // refused (serial) but used
               wl_data_device_set_selection(dd, src, 1);
           });
    expect("data device: a drag-and-drop source used as the selection", "wl_data_source",
           WL_DATA_SOURCE_ERROR_INVALID_SOURCE, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
               wl_data_device_set_selection(dd, src, 1);
           });
    expect("data source: set_actions twice", "wl_data_source",
           WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, [](client& c) {
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
               wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
           });
    expect("data source: set_actions with an undefined bit", "wl_data_source",
           WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, [](client& c) {
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_source_set_actions(src, 8);
           });
    expect("data source: set_actions after set_selection", "wl_data_source",
           WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_device_set_selection(dd, src, 1);
               wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
           });
    {
        // An offer is needed: the client sets its own selection and, focused, is offered it.
        static clip k{};
        const auto own_offer = [](client& c) {
            k = {};
            k.c = c;
            k.kb = wl_seat_get_keyboard(c.seat);
            wl_keyboard_add_listener(k.kb, &kb_listener, &k);
            k.dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
            wl_data_device_add_listener(k.dd, &dd_listener, &k);
            window w{};
            map_window(c, w);
            (void)wl_display_roundtrip(c.display);
            clip_set(k, {"text/plain"}, k.enter_serial);
            (void)wl_display_roundtrip(c.display);
        };
        expect("data offer: finish on a selection offer", "wl_data_offer",
               WL_DATA_OFFER_ERROR_INVALID_FINISH, [&](client& c) {
                   own_offer(c);
                   if (k.offer)
                       wl_data_offer_finish(k.offer);
               });
        expect("data offer: set_actions on a selection offer", "wl_data_offer",
               WL_DATA_OFFER_ERROR_INVALID_OFFER, [&](client& c) {
                   own_offer(c);
                   if (k.offer)
                       wl_data_offer_set_actions(k.offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                                 WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
               });
    }

    // ── Clipboard: delivery order, serials, fd passing, ownership ──
    {
        clip a{}, b{};
        a.payload = "lumen-clip-conformance";
        const bool up = clip_connect(a);
        clip_set(a, {"text/plain;charset=utf-8", "UTF8_STRING"}, a.enter_serial);
        const bool a_own_offer = a.offer != nullptr && a.offer_mimes.size() == 2;
        const bool up_b = clip_connect(b); // maps a window: b takes the keyboard
        (void)wl_display_roundtrip(a.c.display);
        const std::size_t i_sel = index_of(b.events, "selection");
        const std::size_t i_enter = index_of(b.events, "enter");
        const std::size_t i_offer = index_of(b.events, "data_offer");
        check("clipboard: the selection reaches a client before its keyboard enter",
              up && up_b && a_own_offer && i_offer < i_sel && i_sel < i_enter &&
                  b.offer_mimes.size() == 2 && b.offer_mimes[0] == "text/plain;charset=utf-8",
              "a offered its own selection=" + std::to_string(a_own_offer) +
                  ", b events: data_offer@" + std::to_string(i_offer) + " selection@" +
                  std::to_string(i_sel) + " enter@" + std::to_string(i_enter) + " mimes=" +
                  std::to_string(b.offer_mimes.size()));

        const std::string got = clip_read(b, a, [&](int fd) {
            wl_data_offer_receive(b.offer, "text/plain;charset=utf-8", fd);
        });
        check("clipboard: receive passes the fd to the owner, data arrives intact",
              got == "lumen-clip-conformance", "read \"" + got + "\"");
        const std::string none = clip_read(b, a, [&](int fd) {
            wl_data_offer_receive(b.offer, "text/html", fd);
        });
        check("clipboard: receive of a type the source did not offer is not forwarded",
              none.empty(), "read \"" + none + "\"");

        // a lost the keyboard to b: its offer is inert, a receive on it reads nothing.
        const std::string inert = clip_read(a, a, [&](int fd) {
            wl_data_offer_receive(a.offer, "UTF8_STRING", fd);
        });
        check("clipboard: an offer goes inert when its client loses the keyboard",
              inert.empty(), "read \"" + inert + "\"");

        // A serial b was never sent: refused, the source cancelled, the selection kept.
        wl_data_source* stale = wl_data_device_manager_create_data_source(b.c.ddm);
        static bool stale_cancelled;
        stale_cancelled = false;
        static const wl_data_source_listener stale_listener = {
            src_target, src_send, [](void*, wl_data_source*) { stale_cancelled = true; },
            src_dnd_drop_performed, src_dnd_finished, src_action};
        wl_data_source_add_listener(stale, &stale_listener, &b);
        wl_data_source_offer(stale, "text/plain");
        wl_data_device_set_selection(b.dd, stale, b.enter_serial + 100000);
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(b.c.display);
        const bool kept = !a.cancelled;
        check("clipboard: set_selection quoting a serial never sent is refused and cancelled",
              stale_cancelled && kept,
              "stale source cancelled=" + std::to_string(stale_cancelled) +
                  ", owner kept the selection=" + std::to_string(kept));
        wl_data_source_destroy(stale);

        // Replacement: b takes the selection with its own enter serial; a is cancelled.
        b.payload = "from-b";
        clip_set(b, {"text/plain"}, b.enter_serial);
        (void)wl_display_roundtrip(a.c.display);
        check("clipboard: a new owner cancels the old source",
              a.cancelled && b.offer && b.offer_mimes.size() == 1,
              "old source cancelled=" + std::to_string(a.cancelled) + ", new offer mimes=" +
                  std::to_string(b.offer_mimes.size()));

        // The owner destroys its source: the focused client is told there is none.
        wl_data_source_destroy(b.source);
        b.source = nullptr;
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(b.c.display);
        check("clipboard: destroying the selection's source delivers selection(NULL)",
              !b.events.empty() && b.events.back() == "selection:nil" && b.offer == nullptr,
              "last event " + (b.events.empty() ? std::string("-") : b.events.back()));
        clip_disconnect(b);
        clip_disconnect(a);
    }

    // ── Clipboard: a button press counts as the triggering event, a configure does not ──
    // Input through the compositor's virtual-input seam, on this thread while the pump is
    // held off (protocol handling and input are UI-affinity work, never concurrent).
    {
        clip k{};
        const bool up = clip_connect(k);
        static uint32_t press_serial;
        press_serial = 0;
        wl_pointer_listener pl{};
        pl.enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {};
        pl.leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {};
        pl.motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {};
        pl.button = [](void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
            if (state == WL_POINTER_BUTTON_STATE_PRESSED)
                press_serial = serial;
        };
        pl.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {};
        wl_pointer* ptr = wl_seat_get_pointer(k.c.seat);
        wl_pointer_add_listener(ptr, &pl, nullptr);
        (void)wl_display_roundtrip(k.c.display);

        // The configure serial was sent to this client, but it is not an input event.
        k.cancelled = false;
        clip_set(k, {"text/plain"}, k.win.cfg.serial);
        (void)wl_display_roundtrip(k.c.display);
        const bool configure_refused = k.cancelled;
        if (k.source)
            wl_data_source_destroy(k.source);
        k.source = nullptr;
        k.cancelled = false;

        lx::rect2i g{};
        {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            unsigned long long newest = 0;
            comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
                if (r.mapped && r.id.id() > newest) {
                    newest = r.id.id();
                    g = r.geometry;
                }
            });
            lx::input::pointer_motion_event m{};
            m.x = g.x + g.width / 2.0;
            m.y = g.y + g.height / 2.0;
            comp.inject_pointer_motion(m);
            lx::input::pointer_button_event b{};
            b.which = lx::input::button::left;
            b.evdev_code = 0x110;
            b.pressed = true;
            comp.inject_pointer_button(b);
            b.pressed = false;
            comp.inject_pointer_button(b);
            comp.wayland().flush();
        }
        (void)wl_display_roundtrip(k.c.display);
        const uint32_t pressed = press_serial;
        clip_set(k, {"text/plain"}, pressed);
        (void)wl_display_roundtrip(k.c.display);
        const bool press_ok = pressed != 0 && !k.cancelled && k.offer != nullptr &&
                              k.offer_mimes.size() == 1;
        check("clipboard: a configure serial is refused; a button-press serial is accepted",
              up && configure_refused && press_ok,
              "configure serial " + std::to_string(k.win.cfg.serial) + " refused=" +
                  std::to_string(configure_refused) + ", press serial " +
                  std::to_string(pressed) + " accepted=" + std::to_string(press_ok));
        clip_disconnect(k);
    }

#if defined(LUMEN_TEST_PRIMARY_SELECTION)
    // ── Primary selection: the same model ──
    {
        clip a{}, b{};
        a.payload = "lumen-primary-conformance";
        const bool up = clip_connect(a) && a.pd;
        a.psource = zwp_primary_selection_device_manager_v1_create_source(a.c.psm);
        zwp_primary_selection_source_v1_add_listener(a.psource, &psrc_listener, &a);
        zwp_primary_selection_source_v1_offer(a.psource, "text/plain;charset=utf-8");
        zwp_primary_selection_device_v1_set_selection(a.pd, a.psource, a.enter_serial);
        (void)wl_display_roundtrip(a.c.display);
        const bool up_b = clip_connect(b) && b.pd;
        const std::size_t i_sel = index_of(b.events, "pselection");
        const std::size_t i_enter = index_of(b.events, "enter");
        check("primary: the selection reaches a client before its keyboard enter",
              up && up_b && index_of(b.events, "pdata_offer") < i_sel && i_sel < i_enter &&
                  b.poffer_mimes.size() == 1,
              "pselection@" + std::to_string(i_sel) + " enter@" + std::to_string(i_enter));
        const std::string got = clip_read(b, a, [&](int fd) {
            zwp_primary_selection_offer_v1_receive(b.poffer, "text/plain;charset=utf-8", fd);
        });
        check("primary: receive passes the fd, data arrives intact",
              got == "lumen-primary-conformance", "read \"" + got + "\"");
        // A reused source has no error code here: refused and cancelled.
        zwp_primary_selection_device_v1_set_selection(a.pd, a.psource, a.enter_serial);
        (void)wl_display_roundtrip(a.c.display);
        check("primary: a reused source is refused and cancelled, not a protocol error",
              a.pcancelled && wl_display_get_error(a.c.display) == 0,
              "cancelled=" + std::to_string(a.pcancelled));
        zwp_primary_selection_source_v1_destroy(a.psource);
        (void)wl_display_roundtrip(a.c.display);
        (void)wl_display_roundtrip(b.c.display);
        check("primary: destroying the source delivers selection(NULL)",
              !b.events.empty() && b.events.back() == "pselection:nil",
              "last event " + (b.events.empty() ? std::string("-") : b.events.back()));
        clip_disconnect(b);
        clip_disconnect(a);
    }
#endif

    // ── Drag-and-drop: protocol errors ──
    g_comp = &comp;
    g_dispatch_lock = &dispatch_lock;
    expect("dnd: start_drag with an icon that has another role", "wl_data_device",
           WL_DATA_DEVICE_ERROR_ROLE, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               window w{};
               map_window(c, w);
               wl_data_device_start_drag(dd, nullptr, w.surface, w.surface, 1);
           });
    expect("dnd: start_drag with a source already used", "wl_data_device",
           WL_DATA_DEVICE_ERROR_USED_SOURCE, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               window w{};
               map_window(c, w);
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_source_offer(src, "text/plain");
               wl_data_device_set_selection(dd, src, 1); // refused (serial) but used
               wl_data_device_start_drag(dd, src, w.surface, nullptr, 1);
           });
    expect("dnd: source set_actions after start_drag", "wl_data_source",
           WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK, [](client& c) {
               wl_data_device* dd = wl_data_device_manager_get_data_device(c.ddm, c.seat);
               window w{};
               map_window(c, w);
               wl_data_source* src = wl_data_device_manager_create_data_source(c.ddm);
               wl_data_device_start_drag(dd, src, w.surface, nullptr, 1); // refused, used
               wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
           });
    {
        static dnd_peer p{};
        expect("dnd: offer set_actions with an undefined bit", "wl_data_offer",
               WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK, [](client& c) {
                   self_drag(p, c);
                   if (p.offer)
                       wl_data_offer_set_actions(p.offer, 8, 0);
                   (void)wl_display_roundtrip(c.display);
                   inject_left(false);
               });
        expect("dnd: offer set_actions with two preferred actions", "wl_data_offer",
               WL_DATA_OFFER_ERROR_INVALID_ACTION, [](client& c) {
                   self_drag(p, c);
                   if (p.offer)
                       wl_data_offer_set_actions(p.offer, 3, 3);
                   (void)wl_display_roundtrip(c.display);
                   inject_left(false);
               });
        expect("dnd: offer finish before the drop", "wl_data_offer",
               WL_DATA_OFFER_ERROR_INVALID_FINISH, [](client& c) {
                   self_drag(p, c);
                   if (p.offer) {
                       wl_data_offer_accept(p.offer, 0, "text/plain");
                       wl_data_offer_set_actions(p.offer, 1, 1);
                       wl_data_offer_finish(p.offer);
                   }
                   (void)wl_display_roundtrip(c.display);
                   inject_left(false);
               });
    }

    // ── Drag-and-drop: one drag across two clients, then the ways one ends without a drop ──
    {
        static dnd_peer a{}, b{};
        a = {};
        b = {};
        a.respond = false; // the origin is a target too while over itself: it declines
        const bool up = connect(a.c) && connect(b.c);
        if (up) {
            dnd_setup(a);
            dnd_setup(b);
        }
        // b mapped last, cascaded from a; the press raises a. A corner of b that a does not
        // cover is where the drop goes.
        dnd_press(a, 5, 5);
        int bx = 0, by = 0;
        for (const auto [cx, cy] : {std::pair{b.geometry.x + b.geometry.width - 6,
                                              b.geometry.y + b.geometry.height - 6},
                                    std::pair{b.geometry.x + 5, b.geometry.y + b.geometry.height - 6},
                                    std::pair{b.geometry.x + b.geometry.width - 6, b.geometry.y + 5}})
            if (!in_rect(a.geometry, cx, cy)) {
                bx = cx;
                by = cy;
                break;
            }
        dnd_start(a);
        (void)wl_display_roundtrip(b.c.display);
        const bool left_origin = in_order(a.events, {"ptr-press", "ptr-leave", "enter"});
        inject_move(bx, by);
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(a.c.display);
        inject_move(bx - 2, by - 1);
        inject_move(bx - 3, by - 2);
        (void)wl_display_roundtrip(b.c.display);
        inject_left(false);
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(a.c.display);
        std::string got;
        if (b.offer) {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) == 0) {
                wl_data_offer_receive(b.offer, "text/plain", fds[1]);
                close(fds[1]);
                (void)wl_display_roundtrip(b.c.display);
                (void)wl_display_roundtrip(a.c.display);
                char buf[128];
                pollfd pf{fds[0], POLLIN, 0};
                while (poll(&pf, 1, 2000) > 0) {
                    const ssize_t n = read(fds[0], buf, sizeof(buf));
                    if (n <= 0)
                        break;
                    got.append(buf, static_cast<std::size_t>(n));
                }
                close(fds[0]);
            }
            wl_data_offer_finish(b.offer);
            (void)wl_display_roundtrip(b.c.display);
            (void)wl_display_roundtrip(a.c.display);
        }
        check("dnd: the origin client gets wl_pointer.leave when its drag starts",
              up && left_origin, "origin events: " + joined(a.events));
        check("dnd: target gets enter, motion, drop, leave; offer source_actions and action",
              // The action follows the target's own accept/set_actions, so it may land on
              // either side of the first motion; the protocol orders neither.
              in_order(b.events, {"source_actions:3", "enter", "offer-action:2", "drop", "leave"}) &&
                  in_order(b.events, {"enter", "motion", "drop"}) &&
                  std::count(b.events.begin(), b.events.end(), "drop") == 1,
              "target events: " + joined(b.events));
        check("dnd: source gets target, action move, drop_performed, send, finished",
              in_order(a.events, {"target:text/plain", "action:2", "drop_performed", "send",
                                  "finished"}) &&
                  std::count(a.events.begin(), a.events.end(), "cancelled") == 0,
              "source events: " + joined(a.events));
        check("dnd: the dropped offer's receive reads the source's data",
              got == "lumen-dnd-conformance", "read \"" + got + "\"");
        // The button releases reached no client: the origin's press was its last button.
        check("dnd: no client gets the release of a drag's button",
              std::count(a.events.begin(), a.events.end(), "ptr-release") == 0 &&
                  std::count(b.events.begin(), b.events.end(), "ptr-release") == 0,
              "origin " + joined(a.events) + " | target " + joined(b.events));

        // A stale serial: refused, the source cancelled, pointer focus untouched.
        a.events.clear();
        b.events.clear();
        wl_data_source_destroy(a.source);
        a.source = nullptr;
        dnd_press(a, 5, 5);
        dnd_start(a, 1000);
        inject_left(false);
        (void)wl_display_roundtrip(a.c.display);
        check("dnd: start_drag quoting a serial that is not the held press is cancelled",
              in_order(a.events, {"ptr-press", "cancelled", "ptr-release"}) &&
                  std::count(a.events.begin(), a.events.end(), "ptr-leave") == 0 &&
                  std::count(a.events.begin(), a.events.end(), "enter") == 0,
              "origin events: " + joined(a.events));

        // Not accepted: b declines, the release over it cancels (leave, no drop).
        wl_data_source_destroy(a.source);
        a.source = nullptr;
        a.events.clear();
        b.events.clear();
        b.accept = nullptr;
        dnd_press(a, 5, 5);
        dnd_start(a);
        inject_move(bx, by);
        (void)wl_display_roundtrip(b.c.display);
        inject_left(false);
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(a.c.display);
        check("dnd: a release over a target that accepted nothing cancels (leave, no drop)",
              in_order(b.events, {"enter", "leave"}) &&
                  std::count(b.events.begin(), b.events.end(), "drop") == 0 &&
                  in_order(a.events, {"cancelled"}) &&
                  std::count(a.events.begin(), a.events.end(), "drop_performed") == 0,
              "target " + joined(b.events) + " | source " + joined(a.events));

        // Escape: the target is left, the source cancelled, Escape reaches no client.
        wl_data_source_destroy(a.source);
        a.source = nullptr;
        a.events.clear();
        b.events.clear();
        b.accept = "text/plain";
        dnd_press(a, 5, 5);
        dnd_start(a);
        inject_move(bx, by);
        (void)wl_display_roundtrip(b.c.display);
        const std::size_t before_escape = a.events.size();
        inject_escape();
        (void)wl_display_roundtrip(b.c.display);
        (void)wl_display_roundtrip(a.c.display);
        // Until the release nobody has the pointer (no `enter` mid-press); then the surface
        // under it gets `enter`.
        const bool still_quiet =
            std::count(a.events.begin() + static_cast<long>(before_escape), a.events.end(),
                       "ptr-enter") == 0;
        inject_left(false);
        (void)wl_display_roundtrip(a.c.display);
        (void)wl_display_roundtrip(b.c.display);
        const bool back = !b.events.empty() && b.events.back() == "ptr-enter";
        check("dnd: Escape cancels (leave, cancelled); the pointer returns at release",
              in_order(b.events, {"enter", "leave"}) &&
                  std::count(b.events.begin(), b.events.end(), "drop") == 0 &&
                  in_order(a.events, {"cancelled"}) && still_quiet && back &&
                  std::count(a.events.begin(), a.events.end(), "ptr-release") == 0,
              "target " + joined(b.events) + " | source " + joined(a.events));

        // The source's client goes mid-drag: the target is left, the compositor carries on.
        wl_data_source_destroy(a.source);
        a.source = nullptr;
        a.events.clear();
        b.events.clear();
        dnd_press(a, 5, 5);
        dnd_start(a);
        inject_move(bx, by);
        (void)wl_display_roundtrip(b.c.display);
        wl_display_disconnect(a.c.display);
        a.c.display = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        (void)wl_display_roundtrip(b.c.display);
        const bool active_after = [&] {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            return comp.windows().drag_active();
        }();
        inject_left(false);
        (void)wl_display_roundtrip(b.c.display);
        check("dnd: the source's client exiting mid-drag ends it; the target is left",
              in_order(b.events, {"enter", "leave"}) && !active_after &&
                  std::count(b.events.begin(), b.events.end(), "drop") == 0,
              "target " + joined(b.events) + ", drag active after=" +
                  std::to_string(active_after));
        if (b.c.display)
            wl_display_disconnect(b.c.display);
    }

#if defined(LUMEN_TEST_ACTIVATION)
    // ── xdg-activation ──
    expect("activation: xdg_activation_token_v1.commit twice", "xdg_activation_token_v1",
           XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED, [](client& c) {
               xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(c.activation);
               xdg_activation_token_v1_commit(t);
               xdg_activation_token_v1_commit(t);
           });
    expect("activation: an unknown token is ignored, not an error", "none", 0, [](client& c) {
        window w{};
        map_window(c, w);
        xdg_activation_v1_activate(c.activation, "deadbeef", w.surface);
        xdg_activation_v1_activate(c.activation, "", w.surface);
    });
    expect("activation: requests after commit are ignored; the token outlives its object",
           "none", 0, [](client& c) {
               std::string text;
               xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(c.activation);
               xdg_activation_token_v1_add_listener(t, &act_listener, &text);
               xdg_activation_token_v1_commit(t);
               xdg_activation_token_v1_set_app_id(t, "late");
               xdg_activation_token_v1_set_serial(t, 1, c.seat);
               (void)wl_display_roundtrip(c.display);
               xdg_activation_token_v1_destroy(t);
               window w{};
               map_window(c, w);
               xdg_activation_v1_activate(c.activation, text.c_str(), w.surface);
           });
    expect("activation: a token whose requesting surface was destroyed is still usable", "none",
           0, [](client& c) {
               window gone{};
               map_window(c, gone);
               const std::string text = issue_token(c, 0, gone.surface);
               xdg_toplevel_destroy(gone.toplevel);
               xdg_surface_destroy(gone.xdg);
               wl_surface_destroy(gone.surface);
               (void)wl_display_roundtrip(c.display);
               window w{};
               map_window(c, w);
               xdg_activation_v1_activate(c.activation, text.c_str(), w.surface);
           });
    expect("activation: activating a popup's surface is ignored", "none", 0, [](client& c) {
        window w{};
        map_window(c, w);
        popup p = make_popup(c, w.xdg);
        configure_and_map(c, p.surface, p.xdg, p.cfg, 40, 30);
        const std::string text = issue_token(c, 0);
        xdg_activation_v1_activate(c.activation, text.c_str(), p.surface);
    });

    // Behaviour, with input injected: a token from a click in the focused window activates
    // another client's window; one presented after the user clicked a third window marks the
    // target demands-attention instead; the shell's focus clears it.
    {
        const auto focus_now = [&] {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            return comp.windows().keyboard_focus();
        };
        const auto attention = [&](lx::toplevel_id id) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            const auto* r = comp.toplevels().find(id);
            return r && r->demands_attention;
        };
        dnd_peer target{}, issuer{}, other{};
        bool ok = connect(target.c) && connect(issuer.c) && connect(other.c) &&
                  target.c.activation && issuer.c.activation;
        if (ok) {
            dnd_setup(target);
            const lx::toplevel_id target_id = focus_now();
            dnd_setup(issuer);
            const lx::toplevel_id issuer_id = focus_now();

            // Granted: the issuer's own click, the issuer focused.
            dnd_press(issuer, 30, 30);
            inject_left(false);
            const std::string t1 = issue_token(issuer.c, issuer.press_serial);
            xdg_activation_v1_activate(target.c.activation, t1.c_str(), target.win.surface);
            (void)wl_display_roundtrip(target.c.display);
            (void)wl_display_roundtrip(target.c.display);
            const bool granted = focus_now() == target_id && !attention(target_id);
            check("activation: a token from the focused client's click focuses the target",
                  granted && t1.size() == 32,
                  "token=" + std::to_string(t1.size()) + " chars, focus=" +
                      std::to_string(focus_now().id()) + " want " +
                      std::to_string(target_id.id()));

            // Single use: the same token again is unknown → urgent (the target is focused, and
            // a focused window is never marked).
            xdg_activation_v1_activate(target.c.activation, t1.c_str(), target.win.surface);
            (void)wl_display_roundtrip(target.c.display);
            check("activation: a spent token does not mark the focused window",
                  focus_now() == target_id && !attention(target_id), "");

            // User moved on: a token from a click in the issuer, then the user clicks a third
            // client's window, then the target presents it. (The issuer is raised first so
            // the click lands on it, not on the target the activation just raised.)
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.windows().raise(issuer_id);
            }
            dnd_press(issuer, 30, 30);
            inject_left(false);
            const std::string t2 = issue_token(issuer.c, issuer.press_serial);
            dnd_setup(other);
            const lx::toplevel_id other_id = focus_now();
            dnd_press(other, 30, 30);
            inject_left(false);
            xdg_activation_v1_activate(target.c.activation, t2.c_str(), target.win.surface);
            (void)wl_display_roundtrip(target.c.display);
            (void)wl_display_roundtrip(target.c.display);
            const bool urgent = focus_now() == other_id && attention(target_id);
            check("activation: after a click in another window the target is marked, not focused",
                  urgent, "focus=" + std::to_string(focus_now().id()) + " want " +
                              std::to_string(other_id.id()) +
                              ", demands_attention=" + std::to_string(attention(target_id)));

            // A token issued by an unfocused client from a serial that is not its press.
            const std::string t3 = issue_token(issuer.c, issuer.press_serial + 1000);
            xdg_activation_v1_activate(issuer.c.activation, t3.c_str(), issuer.win.surface);
            (void)wl_display_roundtrip(issuer.c.display);
            check("activation: a token with a serial never delivered marks, does not focus",
                  focus_now() == other_id && attention(issuer_id), "");

            // Focus by the window manager (the shell's request_activate) clears the mark.
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                comp.windows().activate(target_id);
                comp.wayland().flush();
            }
            check("activation: focusing the marked window clears demands_attention",
                  focus_now() == target_id && !attention(target_id), "");

            // Before map: decided at map. A granted token lets the new window take the
            // keyboard; an unknown one keeps it from doing so (and it opens below the
            // focused window, marked).
            const auto newest_id = [&] {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                lx::toplevel_id id{};
                comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
                    if (r.id.id() > id.id())
                        id = r.id;
                });
                return id;
            };
            const auto stacking = [&](lx::toplevel_id id) {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                return comp.toplevels().stacking_index(id);
            };
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.windows().raise(issuer_id);
            }
            dnd_press(issuer, 30, 30);
            inject_left(false);
            const std::string t4 = issue_token(issuer.c, issuer.press_serial);
            window early{};
            early.surface = wl_compositor_create_surface(target.c.compositor);
            early.xdg = xdg_wm_base_get_xdg_surface(target.c.wm_base, early.surface);
            xdg_surface_add_listener(early.xdg, &xdg_listener, &early.cfg);
            early.toplevel = xdg_surface_get_toplevel(early.xdg);
            xdg_activation_v1_activate(target.c.activation, t4.c_str(), early.surface);
            configure_and_map(target.c, early.surface, early.xdg, early.cfg, 120, 90);
            (void)wl_display_roundtrip(target.c.display);
            const lx::toplevel_id early_id = newest_id();
            check("activation: presented before map, a good token gives the new window the keyboard",
                  focus_now() == early_id && !attention(early_id),
                  "focus=" + std::to_string(focus_now().id()) + " want " +
                      std::to_string(early_id.id()));

            // An unknown token before map proves nothing: the window is treated as one that
            // presented none and takes the keyboard (GTK presents a self-issued token for
            // every new window).
            window plain{};
            plain.surface = wl_compositor_create_surface(target.c.compositor);
            plain.xdg = xdg_wm_base_get_xdg_surface(target.c.wm_base, plain.surface);
            xdg_surface_add_listener(plain.xdg, &xdg_listener, &plain.cfg);
            plain.toplevel = xdg_surface_get_toplevel(plain.xdg);
            xdg_activation_v1_activate(target.c.activation, "deadbeef", plain.surface);
            configure_and_map(target.c, plain.surface, plain.xdg, plain.cfg, 120, 90);
            (void)wl_display_roundtrip(target.c.display);
            const lx::toplevel_id plain_id = newest_id();
            check("activation: presented before map, an unknown token is no reason to withhold "
                  "the keyboard from a new window",
                  plain_id != early_id && focus_now() == plain_id && !attention(plain_id),
                  "focus=" + std::to_string(focus_now().id()) + " want " +
                      std::to_string(plain_id.id()));

            // A launch token the user moved on from (a click in another client after it was
            // issued): the new window opens without the keyboard, marked, just below the
            // focused one.
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.windows().raise(issuer_id);
            }
            dnd_press(issuer, 30, 30);
            inject_left(false);
            const std::string t5 = issue_token(issuer.c, issuer.press_serial);
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.windows().raise(other_id);
            }
            dnd_press(other, 30, 30);
            inject_left(false);
            window refused{};
            refused.surface = wl_compositor_create_surface(target.c.compositor);
            refused.xdg = xdg_wm_base_get_xdg_surface(target.c.wm_base, refused.surface);
            xdg_surface_add_listener(refused.xdg, &xdg_listener, &refused.cfg);
            refused.toplevel = xdg_surface_get_toplevel(refused.xdg);
            xdg_activation_v1_activate(target.c.activation, t5.c_str(), refused.surface);
            configure_and_map(target.c, refused.surface, refused.xdg, refused.cfg, 120, 90);
            (void)wl_display_roundtrip(target.c.display);
            const lx::toplevel_id refused_id = newest_id();
            check("activation: presented before map after the user moved on, the new window "
                  "opens marked, without the keyboard, just below the focused one",
                  refused_id != plain_id && focus_now() == other_id && attention(refused_id) &&
                      stacking(refused_id) + 1 == stacking(other_id),
                  "focus=" + std::to_string(focus_now().id()) + " want " +
                      std::to_string(other_id.id()) + ", marked=" +
                      std::to_string(attention(refused_id)) + ", stacking " +
                      std::to_string(stacking(refused_id)) + " below " +
                      std::to_string(stacking(other_id)));
        } else {
            check("activation: behaviour (three clients bind xdg_activation_v1)", false,
                  "could not connect or bind");
        }
        for (dnd_peer* p : {&target, &issuer, &other})
            if (p->c.display)
                wl_display_disconnect(p->c.display);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    // ── Objects one client hoards never cost another client its connection ──
    // A client that creates objects and never destroys them (a bug, or on purpose) must not
    // make the next client's request fail: no_memory is fatal to the client it is posted
    // to. The counts are the sizes the shared tables once had (64 activation token
    // builders, 128 data sources, 128 data devices, 128 wl_output objects); the hoarder
    // itself is not refused either (wlroots and KWin allocate per request).
    {
        client hoarder{};
        bool up = connect(hoarder) && hoarder.seat && hoarder.ddm && hoarder.output_name;
        if (up) {
#if defined(LUMEN_TEST_ACTIVATION)
            if (hoarder.activation)
                for (int i = 0; i < 64; ++i)
                    (void)xdg_activation_v1_get_activation_token(hoarder.activation);
#endif
            for (int i = 0; i < 128; ++i) {
                (void)wl_data_device_manager_create_data_source(hoarder.ddm);
                (void)wl_data_device_manager_get_data_device(hoarder.ddm, hoarder.seat);
                (void)wl_registry_bind(hoarder.registry, hoarder.output_name,
                                       &wl_output_interface, 1);
            }
            (void)wl_display_roundtrip(hoarder.display);
        }
        const bool hoarder_ok = up && wl_display_get_error(hoarder.display) == 0;

        client victim{};
        up = up && connect(victim) && victim.seat && victim.ddm && victim.output_name;
        std::string token = "-";
        struct out_done {
            unsigned dones = 0;
        } od{};
        if (up) {
#if defined(LUMEN_TEST_ACTIVATION)
            if (victim.activation)
                token = issue_token(victim, 0);
#endif
            (void)wl_data_device_manager_create_data_source(victim.ddm);
            (void)wl_data_device_manager_get_data_device(victim.ddm, victim.seat);
            static const wl_output_listener count_done = {
                [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                   const char*, int32_t) {},
                [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
                [](void* d, wl_output*) { ++static_cast<out_done*>(d)->dones; },
                [](void*, wl_output*, int32_t) {},
                [](void*, wl_output*, const char*) {},
                [](void*, wl_output*, const char*) {}};
            auto* out = static_cast<wl_output*>(
                wl_registry_bind(victim.registry, victim.output_name, &wl_output_interface,
                                 victim.output_version < 4 ? victim.output_version : 4));
            wl_output_add_listener(out, &count_done, &od);
            (void)wl_display_roundtrip(victim.display);
        }
        const int victim_err = up ? wl_display_get_error(victim.display) : -1;
        check("hoarded objects: another client's activation token, data source, data device "
              "and wl_output still work",
              hoarder_ok && victim_err == 0 && !token.empty() && od.dones >= 1,
              "hoarder " + std::string(hoarder_ok ? "alive" : "dead") + ", other client error " +
                  std::to_string(victim_err) + ", token '" + token.substr(0, 8) +
                  "', wl_output.done " + std::to_string(od.dones));
        if (victim.display)
            wl_display_disconnect(victim.display);
        if (hoarder.display)
            wl_display_disconnect(hoarder.display);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── A ping outstanding on an xdg_wm_base the client destroys ──
    // The pong can only come on that object, so the ping is dropped with it: the client is
    // not marked not responding (and its pointer motion muted) for good.
    {
        client c{};
        bool ok = connect(c);
        unsigned pinged = 0; // the client's id, found by its outstanding ping
        if (ok) {
            // Mapping activates the window, which pings its client. No wm_base listener
            // here: the ping is never answered.
            window w{};
            map_window(c, w);
            (void)wl_display_roundtrip(c.display);
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                for (unsigned id = 1; id < 4096 && !pinged; ++id)
                    if (comp.wayland().find_client(id) &&
                        comp.liveness().outstanding(lx::client_id{id}) != 0)
                        pinged = id;
            }
            xdg_toplevel_destroy(w.toplevel);
            xdg_surface_destroy(w.xdg);
            wl_surface_destroy(w.surface);
            xdg_wm_base_destroy(c.wm_base);
            (void)wl_display_roundtrip(c.display);
            ok = wl_display_get_error(c.display) == 0;
        }
        unsigned left = 1;
        bool marked = true;
        if (pinged) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            left = comp.liveness().outstanding(lx::client_id{pinged});
            marked = comp.liveness().not_responding(lx::client_id{pinged});
        }
        check("liveness: destroying the xdg_wm_base a ping went out on drops that ping",
              ok && pinged != 0 && left == 0 && !marked,
              pinged ? "outstanding after destroy " + std::to_string(left) +
                           (marked ? ", not responding" : "")
                     : std::string("no ping went out on map"));
        if (c.display)
            wl_display_disconnect(c.display);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#if defined(LUMEN_TEST_IDLE)
    // ── ext_idle_notifier_v1, zwp_idle_inhibit_manager_v1, zwlr_output_power_manager_v1 ──
    // Neither idle protocol defines an error; what matters is the event order. Timers need
    // the event loop (live: app-smoke group K), so the timing here uses a 0 ms notification,
    // which is idled in the same dispatch and again right after each resume.
    {
        struct idle_log {
            std::vector<std::string> events;
        };
        static const ext_idle_notification_v1_listener idle_listener = {
            [](void* data, ext_idle_notification_v1*) {
                static_cast<idle_log*>(data)->events.emplace_back("idled");
            },
            [](void* data, ext_idle_notification_v1*) {
                static_cast<idle_log*>(data)->events.emplace_back("resumed");
            }};
        const auto joined_events = [](const idle_log& l) {
            std::string out;
            for (const auto& e : l.events)
                out += (out.empty() ? "" : ",") + e;
            return out.empty() ? std::string("(none)") : out;
        };

        idle_log zero{}, input_only{}, long_timeout{}, orphan{};
        bool v2 = false;
        expect("idle: notifications, then input", "none", 0, [&](client& c) {
            if (!c.idle_notifier || !c.seat)
                return;
            v2 = ext_idle_notifier_v1_get_version(c.idle_notifier) >= 2;
            auto* n = ext_idle_notifier_v1_get_idle_notification(c.idle_notifier, 0, c.seat);
            ext_idle_notification_v1_add_listener(n, &idle_listener, &zero);
            auto* l = ext_idle_notifier_v1_get_idle_notification(c.idle_notifier, 60000, c.seat);
            ext_idle_notification_v1_add_listener(l, &idle_listener, &long_timeout);
            ext_idle_notification_v1* i = nullptr;
            if (v2) {
                i = ext_idle_notifier_v1_get_input_idle_notification(c.idle_notifier, 0, c.seat);
                ext_idle_notification_v1_add_listener(i, &idle_listener, &input_only);
            }
            // Notifications outlive their notifier ("All objects created via this interface
            // remain valid").
            auto* o = ext_idle_notifier_v1_get_idle_notification(c.idle_notifier, 0, c.seat);
            ext_idle_notification_v1_add_listener(o, &idle_listener, &orphan);
            ext_idle_notifier_v1_destroy(c.idle_notifier);
            (void)wl_display_roundtrip(c.display);
            inject_move(300, 200);
            (void)wl_display_roundtrip(c.display);
            ext_idle_notification_v1_destroy(n);
            ext_idle_notification_v1_destroy(l);
            ext_idle_notification_v1_destroy(o);
            if (i)
                ext_idle_notification_v1_destroy(i);
            inject_move(310, 210); // destroyed: nothing more may arrive
            (void)wl_display_roundtrip(c.display);
        });
        check("idle: a 0 ms notification is idled at once, resumed by input, idled again",
              joined_events(zero) == "idled,resumed,idled", joined_events(zero));
        check("idle: a 60 s notification sends nothing (and input does not resume it)",
              long_timeout.events.empty(), joined_events(long_timeout));
        check("idle: get_input_idle_notification (v2) behaves the same without inhibitors",
              !v2 || joined_events(input_only) == "idled,resumed,idled",
              v2 ? joined_events(input_only) : std::string("notifier below v2"));
        check("idle: a notification outlives its destroyed notifier",
              joined_events(orphan) == "idled,resumed,idled", joined_events(orphan));

        expect("idle inhibitor: surface destroyed before the inhibitor", "none", 0,
               [](client& c) {
                   if (!c.inhibit_manager)
                       return;
                   wl_surface* s = wl_compositor_create_surface(c.compositor);
                   auto* i = zwp_idle_inhibit_manager_v1_create_inhibitor(c.inhibit_manager, s);
                   wl_surface_destroy(s); // the inhibitor goes inert, not an error
                   (void)wl_display_roundtrip(c.display);
                   zwp_idle_inhibitor_v1_destroy(i);
               });
        expect("idle inhibitor: manager destroyed first, inhibitors live on", "none", 0,
               [](client& c) {
                   if (!c.inhibit_manager)
                       return;
                   window w{};
                   map_window(c, w);
                   auto* i =
                       zwp_idle_inhibit_manager_v1_create_inhibitor(c.inhibit_manager, w.surface);
                   zwp_idle_inhibit_manager_v1_destroy(c.inhibit_manager);
                   (void)wl_display_roundtrip(c.display);
                   zwp_idle_inhibitor_v1_destroy(i);
               });

        struct power_log {
            std::vector<std::string> events;
        };
        static const zwlr_output_power_v1_listener power_listener = {
            [](void* data, zwlr_output_power_v1*, uint32_t mode) {
                static_cast<power_log*>(data)->events.emplace_back(
                    mode == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off");
            },
            [](void* data, zwlr_output_power_v1*) {
                static_cast<power_log*>(data)->events.emplace_back("failed");
            }};
        const auto get_power = [](client& c, power_log* log) -> zwlr_output_power_v1* {
            if (!c.power_manager || !c.output_name)
                return nullptr;
            auto* out = static_cast<wl_output*>(
                wl_registry_bind(c.registry, c.output_name, &wl_output_interface, 1));
            auto* p = zwlr_output_power_manager_v1_get_output_power(c.power_manager, out);
            if (log)
                zwlr_output_power_v1_add_listener(p, &power_listener, log);
            return p;
        };
        expect("output power: set_mode outside {off, on}", "zwlr_output_power_v1",
               ZWLR_OUTPUT_POWER_V1_ERROR_INVALID_MODE, [&](client& c) {
                   if (auto* p = get_power(c, nullptr))
                       zwlr_output_power_v1_set_mode(p, 2);
               });
        power_log first{};
        expect("output power: the mode on creation", "none", 0, [&](client& c) {
            if (get_power(c, &first))
                (void)wl_display_roundtrip(c.display);
        });
        // With KMS (this in-process compositor takes the card when it can) the output is on;
        // headless or nested there is nothing to power, and the object is failed at once.
        check("output power: exactly one event on creation — mode on, or failed without KMS",
              first.events.size() == 1 &&
                  (first.events[0] == "on" || first.events[0] == "failed"),
              first.events.empty() ? std::string("(none)") : first.events[0]);
    }
#endif

#if defined(LUMEN_TEST_SESSION_LOCK)
    // ── ext_session_lock_v1: every error, and the lock's life ──
    // `locked` needs a locked frame on the output: this compositor never lit its CRTC (no
    // render loop runs here), so nothing of the session is visible and a lock with a lock
    // surface on every output is locked at once; without KMS the tick below publishes it.
    {
        struct lock_log {
            unsigned locked = 0;
            unsigned finished = 0;
            uint32_t serial = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            unsigned configures = 0;
        };
        static const ext_session_lock_v1_listener lock_listener = {
            [](void* d, ext_session_lock_v1*) { ++static_cast<lock_log*>(d)->locked; },
            [](void* d, ext_session_lock_v1*) { ++static_cast<lock_log*>(d)->finished; }};
        static const ext_session_lock_surface_v1_listener lock_surface_listener = {
            [](void* d, ext_session_lock_surface_v1*, uint32_t serial, uint32_t w, uint32_t h) {
                auto* l = static_cast<lock_log*>(d);
                l->serial = serial;
                l->width = w;
                l->height = h;
                ++l->configures;
            }};
        unsigned long long tick_frame = 1'000'000;
        const auto tick = [&] {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
            lx::scheduler::frame_tick t{};
            t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            t.delta_seconds = 1.0 / 30.0;
            t.frame_index = static_cast<unsigned>(++tick_frame);
            comp.tick_ui(t);
            comp.wayland().flush();
        };
        const auto state = [&] {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            return comp.session_lock_state();
        };
        const auto logind_unlock = [&] {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            comp.inject_logind_unlock();
            comp.wayland().flush();
        };
        const auto output = [](client& c) {
            return static_cast<wl_output*>(
                wl_registry_bind(c.registry, c.output_name, &wl_output_interface, 1));
        };
        // A lock with one configured lock surface; `commit` maps it at the configured size.
        struct locker {
            ext_session_lock_v1* lock = nullptr;
            wl_surface* surface = nullptr;
            ext_session_lock_surface_v1* ls = nullptr;
            lock_log log{};
        };
        const auto start_lock = [&](client& c, locker& l, bool with_surface) {
            l.lock = ext_session_lock_manager_v1_lock(c.lock_manager);
            ext_session_lock_v1_add_listener(l.lock, &lock_listener, &l.log);
            if (!with_surface)
                return;
            l.surface = wl_compositor_create_surface(c.compositor);
            l.ls = ext_session_lock_v1_get_lock_surface(l.lock, l.surface, output(c));
            ext_session_lock_surface_v1_add_listener(l.ls, &lock_surface_listener, &l.log);
            (void)wl_display_roundtrip(c.display);
        };
        const auto map_lock = [&](client& c, locker& l) {
            ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial);
            wl_surface_attach(l.surface,
                              make_buffer(c, static_cast<int>(l.log.width),
                                          static_cast<int>(l.log.height)),
                              0, 0);
            wl_surface_commit(l.surface);
            (void)wl_display_roundtrip(c.display);
            tick();
            (void)wl_display_roundtrip(c.display);
        };
        const bool up = [&] {
            client probe{};
            const bool ok = connect(probe) && probe.lock_manager && probe.output_name;
            if (probe.display)
                wl_display_disconnect(probe.display);
            return ok;
        }();
        check("session lock: ext_session_lock_manager_v1 is advertised (with a wl_output)", up,
              up ? "" : "missing global or output");

        if (up) {
            expect("session lock: get_lock_surface on a surface with another role",
                   "ext_session_lock_v1", EXT_SESSION_LOCK_V1_ERROR_ROLE, [&](client& c) {
                       window w{};
                       map_window(c, w);
                       locker l{};
                       start_lock(c, l, false);
                       (void)ext_session_lock_v1_get_lock_surface(l.lock, w.surface, output(c));
                   });
            // Each error kills its locker before `locked`: the session is abandoned (fails
            // closed, checked below) and logind's Unlock clears it for the next case.
            logind_unlock();
            expect("session lock: a second lock surface for one output",
                   "ext_session_lock_v1", EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
                   [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       wl_output* o = output(c);
                       (void)ext_session_lock_v1_get_lock_surface(
                           l.lock, wl_compositor_create_surface(c.compositor), o);
                   });
            logind_unlock();
            expect("session lock: get_lock_surface on a surface with a buffer attached",
                   "ext_session_lock_v1", EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
                   [&](client& c) {
                       locker l{};
                       start_lock(c, l, false);
                       wl_surface* s = wl_compositor_create_surface(c.compositor);
                       wl_surface_attach(s, make_buffer(c, 10, 10), 0, 0);
                       (void)ext_session_lock_v1_get_lock_surface(l.lock, s, output(c));
                   });
            logind_unlock();
            expect("session lock: unlock_and_destroy before locked", "[destroyed]",
                   EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK, [&](client& c) {
                       locker l{};
                       start_lock(c, l, false);
                       ext_session_lock_v1_unlock_and_destroy(l.lock);
                   });
            check("session lock: a locker dying of a protocol error before locked leaves the "
                  "session abandoned (fails closed)",
                  state() == lx::compositor::lock_state::abandoned,
                  lx::compositor::lock_state_name(state()));
            logind_unlock();
            check("session lock: logind Unlock ends an abandoned lock",
                  state() == lx::compositor::lock_state::unlocked,
                  lx::compositor::lock_state_name(state()));

            expect("session lock: lock surface committed before the first ack",
                   "ext_session_lock_surface_v1",
                   EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK, [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       wl_surface_attach(l.surface, make_buffer(c, 10, 10), 0, 0);
                       wl_surface_commit(l.surface);
                   });
            logind_unlock();
            expect("session lock: lock surface committed with a null buffer",
                   "ext_session_lock_surface_v1", EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
                   [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial);
                       wl_surface_commit(l.surface);
                   });
            logind_unlock();
            expect("session lock: lock surface not the acked size",
                   "ext_session_lock_surface_v1",
                   EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH, [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial);
                       wl_surface_attach(l.surface, make_buffer(c, 10, 10), 0, 0);
                       wl_surface_commit(l.surface);
                   });
            logind_unlock();
            expect("session lock: ack_configure of a serial never sent",
                   "ext_session_lock_surface_v1", EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
                   [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial + 1000);
                   });
            logind_unlock();
            expect("session lock: ack_configure of a serial already acked",
                   "ext_session_lock_surface_v1", EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
                   [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial);
                       ext_session_lock_surface_v1_ack_configure(l.ls, l.log.serial);
                   });
            logind_unlock();

            // The whole life: configure at the output's size, locked after the locked frame,
            // destroy is then invalid_destroy (the client dies: abandoned). A destructor
            // request removes the proxy at once, so libwayland reports no interface for it.
            lock_log life{};
            expect("session lock: destroy after locked", "[destroyed]",
                   EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY, [&](client& c) {
                       locker l{};
                       start_lock(c, l, true);
                       map_lock(c, l);
                       life = l.log;
                       ext_session_lock_v1_destroy(l.lock);
                   });
            check("session lock: configure at the output's logical size, then locked",
                  life.configures == 1 && life.width > 0 && life.height > 0 && life.locked == 1 &&
                      life.finished == 0,
                  "configure " + std::to_string(life.width) + "x" + std::to_string(life.height) +
                      ", locked " + std::to_string(life.locked));
            check("session lock: the locker died while locked — abandoned, still locked",
                  state() == lx::compositor::lock_state::abandoned,
                  lx::compositor::lock_state_name(state()));

            // A new locker takes an abandoned lock over: locked at once; then a second locker
            // is finished; unlock_and_destroy unlocks.
            locker taker{}, second{};
            expect("session lock: a new locker replaces an abandoned lock, then unlocks", "none",
                   0, [&](client& c) {
                       start_lock(c, taker, false);
                       (void)wl_display_roundtrip(c.display);
                       client other{};
                       if (connect(other) && other.lock_manager) {
                           start_lock(other, second, false);
                           (void)wl_display_roundtrip(other.display);
                           ext_session_lock_v1_destroy(second.lock); // finished: allowed
                           (void)wl_display_roundtrip(other.display);
                           wl_display_disconnect(other.display);
                       }
                       ext_session_lock_v1_unlock_and_destroy(taker.lock);
                       (void)wl_display_roundtrip(c.display);
                   });
            check("session lock: an abandoned lock's replacement is locked at once",
                  taker.log.locked == 1 && taker.log.finished == 0,
                  "locked " + std::to_string(taker.log.locked));
            check("session lock: a second locker while one holds the lock gets finished",
                  second.log.finished == 1 && second.log.locked == 0,
                  "finished " + std::to_string(second.log.finished));
            check("session lock: unlock_and_destroy unlocks",
                  state() == lx::compositor::lock_state::unlocked,
                  lx::compositor::lock_state_name(state()));

            // logind's Unlock while a live locker holds the lock: `finished`, after which
            // destroy is allowed even though `locked` was sent.
            locker held{};
            expect("session lock: logind Unlock finishes a live locker; destroy is then allowed",
                   "none", 0, [&](client& c) {
                       start_lock(c, held, true);
                       map_lock(c, held);
                       logind_unlock();
                       (void)wl_display_roundtrip(c.display);
                       ext_session_lock_v1_destroy(held.lock);
                       (void)wl_display_roundtrip(c.display);
                   });
            check("session lock: locked, then finished by logind",
                  held.log.locked == 1 && held.log.finished == 1 &&
                      state() == lx::compositor::lock_state::unlocked,
                  "locked " + std::to_string(held.log.locked) + ", finished " +
                      std::to_string(held.log.finished));

            // Input while locked: a normal window loses the keyboard at the request, and the
            // lock surface gets it once it has content.
            {
                client app{};
                window w{};
                bool ok = connect(app) && app.seat;
                static unsigned app_enters = 0, app_leaves = 0, lock_enters = 0;
                static wl_surface* lock_surface_ptr = nullptr;
                static const wl_keyboard_listener kl = {
                    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
                    [](void*, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) {
                        if (s && s == lock_surface_ptr)
                            ++lock_enters;
                        else
                            ++app_enters;
                    },
                    [](void*, wl_keyboard*, uint32_t, wl_surface* s) {
                        if (!(s && s == lock_surface_ptr))
                            ++app_leaves;
                    },
                    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {},
                    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
                    [](void*, wl_keyboard*, int32_t, int32_t) {}};
                wl_keyboard* kb = nullptr;
                if (ok) {
                    kb = wl_seat_get_keyboard(app.seat);
                    wl_keyboard_add_listener(kb, &kl, nullptr);
                    map_window(app, w);
                    (void)wl_display_roundtrip(app.display);
                }
                const unsigned enters0 = app_enters;
                locker l{};
                // The locker is the same client here: the lock surface's enter is told apart
                // by its surface.
                if (ok) {
                    start_lock(app, l, true);
                    lock_surface_ptr = l.surface;
                    (void)wl_display_roundtrip(app.display);
                }
                const unsigned leaves_at_request = app_leaves;
                if (ok)
                    map_lock(app, l);
                const bool lock_has_kb = lock_enters == 1;
                if (ok) {
                    ext_session_lock_v1_unlock_and_destroy(l.lock);
                    (void)wl_display_roundtrip(app.display);
                    (void)wl_display_roundtrip(app.display);
                }
                check("session lock: the focused window loses the keyboard at the lock request, "
                      "the lock surface gets it, the window gets it back at unlock",
                      ok && enters0 >= 1 && leaves_at_request >= 1 && lock_has_kb &&
                          app_enters >= enters0 + 1 && l.log.locked == 1,
                      "app enters " + std::to_string(enters0) + "→" +
                          std::to_string(app_enters) + ", leaves " +
                          std::to_string(leaves_at_request) + ", lock enters " +
                          std::to_string(lock_enters));
                if (kb)
                    wl_keyboard_destroy(kb);
                if (app.display)
                    wl_display_disconnect(app.display);
            }
            if (state() != lx::compositor::lock_state::unlocked)
                logind_unlock();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif

#if defined(LUMEN_TEST_IME)
    // ── text-input-v3, input-method-v2, virtual-keyboard-v1: every error, and the relay ──
    {
        static const zwp_input_method_v2_listener quiet_im = {
            [](void*, zwp_input_method_v2*) {}, [](void*, zwp_input_method_v2*) {},
            [](void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {},
            [](void*, zwp_input_method_v2*, uint32_t) {},
            [](void*, zwp_input_method_v2*, uint32_t, uint32_t) {},
            [](void*, zwp_input_method_v2*) {},
            [](void* d, zwp_input_method_v2*) {
                if (d)
                    ++*static_cast<unsigned*>(d);
            }};
        const auto actions = [](client& c, std::initializer_list<uint32_t> list) {
            zwp_text_input_v3* ti = zwp_text_input_manager_v3_get_text_input(c.ti_manager, c.seat);
            wl_array a{};
            wl_array_init(&a);
            for (uint32_t v : list)
                *static_cast<uint32_t*>(wl_array_add(&a, sizeof(uint32_t))) = v;
            zwp_text_input_v3_set_available_actions(ti, &a);
            wl_array_release(&a);
        };
        client probe{};
        const bool up = connect(probe) && probe.ti_manager && probe.im_manager &&
                        probe.vk_manager && probe.ti_version == 2;
        check("ime: zwp_text_input_manager_v3 v2, zwp_input_method_manager_v2 and "
              "zwp_virtual_keyboard_manager_v1 are advertised",
              up, "text input v" + std::to_string(probe.ti_version));
        if (probe.display)
            wl_display_disconnect(probe.display);
        if (up) {
            expect("text input: available actions holding none", "zwp_text_input_v3",
                   ZWP_TEXT_INPUT_V3_ERROR_INVALID_ACTION,
                   [&](client& c) { actions(c, {ZWP_TEXT_INPUT_V3_ACTION_NONE}); });
            expect("text input: an action twice", "zwp_text_input_v3",
                   ZWP_TEXT_INPUT_V3_ERROR_INVALID_ACTION, [&](client& c) {
                       actions(c, {ZWP_TEXT_INPUT_V3_ACTION_SUBMIT, ZWP_TEXT_INPUT_V3_ACTION_SUBMIT});
                   });
            expect("text input: a value that is not an action", "zwp_text_input_v3",
                   ZWP_TEXT_INPUT_V3_ERROR_INVALID_ACTION, [&](client& c) { actions(c, {7}); });
            expect("text input: submit, and an empty list, are fine", "none", 0, [&](client& c) {
                actions(c, {ZWP_TEXT_INPUT_V3_ACTION_SUBMIT});
                actions(c, {});
            });
            expect("input method: popup surface on a surface with another role",
                   "zwp_input_method_v2", ZWP_INPUT_METHOD_V2_ERROR_ROLE, [&](client& c) {
                       zwp_input_method_v2* im =
                           zwp_input_method_manager_v2_get_input_method(c.im_manager, c.seat);
                       wl_surface* s = wl_compositor_create_surface(c.compositor);
                       (void)xdg_surface_get_toplevel(xdg_wm_base_get_xdg_surface(c.wm_base, s));
                       (void)zwp_input_method_v2_get_input_popup_surface(im, s);
                   });
            expect("input method: a second popup object for one surface", "zwp_input_method_v2",
                   ZWP_INPUT_METHOD_V2_ERROR_ROLE, [&](client& c) {
                       zwp_input_method_v2* im =
                           zwp_input_method_manager_v2_get_input_method(c.im_manager, c.seat);
                       wl_surface* s = wl_compositor_create_surface(c.compositor);
                       (void)zwp_input_method_v2_get_input_popup_surface(im, s);
                       (void)zwp_input_method_v2_get_input_popup_surface(im, s);
                   });
            expect("input method: the popup object goes, its surface may take a new one", "none",
                   0, [&](client& c) {
                       zwp_input_method_v2* im =
                           zwp_input_method_manager_v2_get_input_method(c.im_manager, c.seat);
                       wl_surface* s = wl_compositor_create_surface(c.compositor);
                       zwp_input_popup_surface_v2_destroy(
                           zwp_input_method_v2_get_input_popup_surface(im, s));
                       (void)zwp_input_method_v2_get_input_popup_surface(im, s);
                   });
            expect("input method: wl_surface destroyed before its popup object", "[destroyed]",
                   WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT, [&](client& c) {
                       zwp_input_method_v2* im =
                           zwp_input_method_manager_v2_get_input_method(c.im_manager, c.seat);
                       wl_surface* s = wl_compositor_create_surface(c.compositor);
                       (void)zwp_input_method_v2_get_input_popup_surface(im, s);
                       wl_surface_destroy(s);
                   });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            unsigned unavailable_first = 0, unavailable_second = 0;
            expect("input method: a second one on the seat is unavailable (and inert)", "none", 0,
                   [&](client& c) {
                       zwp_input_method_v2* first =
                           zwp_input_method_manager_v2_get_input_method(c.im_manager, c.seat);
                       zwp_input_method_v2_add_listener(first, &quiet_im, &unavailable_first);
                       (void)wl_display_roundtrip(c.display);
                       client other{};
                       if (connect(other) && other.im_manager && other.seat) {
                           zwp_input_method_v2* second =
                               zwp_input_method_manager_v2_get_input_method(other.im_manager,
                                                                            other.seat);
                           zwp_input_method_v2_add_listener(second, &quiet_im, &unavailable_second);
                           (void)wl_display_roundtrip(other.display);
                           // Inert: its requests are accepted and ignored.
                           zwp_input_method_v2_commit_string(second, "x");
                           zwp_input_method_v2_commit(second, 0);
                           (void)wl_display_roundtrip(other.display);
                           wl_display_disconnect(other.display);
                       }
                   });
            check("input method: unavailable is the second one's only event, not the first's",
                  unavailable_first == 0 && unavailable_second == 1,
                  "first " + std::to_string(unavailable_first) + ", second " +
                      std::to_string(unavailable_second));
            expect("virtual keyboard: key before a keymap", "zwp_virtual_keyboard_v1",
                   ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, [&](client& c) {
                       zwp_virtual_keyboard_v1_key(zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
                                                       c.vk_manager, c.seat),
                                                   0, 30, 1);
                   });
            expect("virtual keyboard: modifiers before a keymap", "zwp_virtual_keyboard_v1",
                   ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP, [&](client& c) {
                       zwp_virtual_keyboard_v1_modifiers(
                           zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vk_manager,
                                                                                   c.seat),
                           1, 0, 0, 0);
                   });
            expect("virtual keyboard: a keymap format that is not one", "zwp_virtual_keyboard_v1",
                   1 /* invalid_keymap_format */, [&](client& c) {
                       const int fd = memfd_create("lumen-vk", MFD_CLOEXEC);
                       zwp_virtual_keyboard_v1_keymap(
                           zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vk_manager,
                                                                                   c.seat),
                           5, fd, 0);
                       close(fd);
                   });
            expect("virtual keyboard: a keymap that does not compile leaves it without one",
                   "zwp_virtual_keyboard_v1", ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                   [&](client& c) {
                       const int fd = memfd_create("lumen-vk", MFD_CLOEXEC);
                       const char junk[] = "xkb_keymap { this is not a keymap";
                       (void)!write(fd, junk, sizeof(junk));
                       zwp_virtual_keyboard_v1* vk =
                           zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vk_manager,
                                                                                   c.seat);
                       zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                                      sizeof(junk));
                       close(fd);
                       zwp_virtual_keyboard_v1_key(vk, 0, 30, 1);
                   });
            expect("virtual keyboard: no_keymap format, then keys", "none", 0, [&](client& c) {
                const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                zwp_virtual_keyboard_v1* vk =
                    zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(c.vk_manager, c.seat);
                zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
                close(fd);
                zwp_virtual_keyboard_v1_key(vk, 0, 30, 1);
                zwp_virtual_keyboard_v1_key(vk, 0, 30, 0);
                zwp_virtual_keyboard_v1_modifiers(vk, 0, 0, 0, 0);
            });

            // The relay end to end: an app's text input and an input method on another
            // connection, keys routed to the grab while it is active, text back to the app.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            struct app_log {
                unsigned enters = 0, leaves = 0, dones = 0, last_serial = 0, kb_keys = 0;
                unsigned keymaps = 0;
                std::string preedit, commit;
                unsigned last_kb_key = 0;
            };
            struct im_log {
                unsigned activates = 0, deactivates = 0, dones = 0, grab_keys = 0, grab_mods = 0;
                unsigned keymaps = 0, repeat = 0, last_key = 0, last_state = 0, unavailable = 0;
                std::string surrounding;
                uint32_t hint = 0, purpose = 0;
                int rect_x = 0, rect_y = 0, rect_w = -1, rect_h = -1;
                int keymap_fd = -1;
                uint32_t keymap_size = 0;
            };
            static app_log al{};
            static im_log il{};
            al = {};
            il = {};
            static const zwp_text_input_v3_listener ti_l = {
                [](void*, zwp_text_input_v3*, wl_surface*) { ++al.enters; },
                [](void*, zwp_text_input_v3*, wl_surface*) { ++al.leaves; },
                [](void*, zwp_text_input_v3*, const char* t, int32_t, int32_t) {
                    al.preedit = t ? t : "";
                },
                [](void*, zwp_text_input_v3*, const char* t) { al.commit = t ? t : ""; },
                [](void*, zwp_text_input_v3*, uint32_t, uint32_t) {},
                [](void*, zwp_text_input_v3*, uint32_t serial) {
                    ++al.dones;
                    al.last_serial = serial;
                },
                [](void*, zwp_text_input_v3*, uint32_t, uint32_t) {},
                [](void*, zwp_text_input_v3*, const char*) {},
                [](void*, zwp_text_input_v3*, uint32_t, uint32_t, uint32_t) {}};
            static const zwp_input_method_v2_listener im_l = {
                [](void*, zwp_input_method_v2*) { ++il.activates; },
                [](void*, zwp_input_method_v2*) { ++il.deactivates; },
                [](void*, zwp_input_method_v2*, const char* t, uint32_t, uint32_t) {
                    il.surrounding = t ? t : "";
                },
                [](void*, zwp_input_method_v2*, uint32_t) {},
                [](void*, zwp_input_method_v2*, uint32_t h, uint32_t p) {
                    il.hint = h;
                    il.purpose = p;
                },
                [](void*, zwp_input_method_v2*) { ++il.dones; },
                [](void*, zwp_input_method_v2*) { ++il.unavailable; }};
            static const zwp_input_method_keyboard_grab_v2_listener grab_l = {
                [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, int32_t fd, uint32_t size) {
                    ++il.keymaps;
                    if (il.keymap_fd >= 0)
                        close(il.keymap_fd);
                    il.keymap_fd = fd;
                    il.keymap_size = size;
                },
                [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t key,
                   uint32_t state) {
                    ++il.grab_keys;
                    il.last_key = key;
                    il.last_state = state;
                },
                [](void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t,
                   uint32_t, uint32_t) { ++il.grab_mods; },
                [](void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) { ++il.repeat; }};
            static const zwp_input_popup_surface_v2_listener popup_l = {
                [](void*, zwp_input_popup_surface_v2*, int32_t x, int32_t y, int32_t w, int32_t h) {
                    il.rect_x = x;
                    il.rect_y = y;
                    il.rect_w = w;
                    il.rect_h = h;
                }};
            static const wl_keyboard_listener app_kb = {
                [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
                    ++al.keymaps;
                    close(fd);
                },
                [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
                [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
                [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
                    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                        ++al.kb_keys;
                        al.last_kb_key = key;
                    }
                },
                [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
                [](void*, wl_keyboard*, int32_t, int32_t) {}};
            const auto key = [&](unsigned code, bool pressed) {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                lx::input::key_event k{};
                k.evdev_keycode = code;
                k.pressed = pressed;
                comp.inject_key(k);
                comp.wayland().flush();
            };
            client app{};
            client ime{};
            window w{};
            bool ok = connect(app) && app.seat && app.ti_manager && connect(ime) &&
                      ime.im_manager && ime.vk_manager && ime.seat;
            zwp_text_input_v3* ti = nullptr;
            zwp_input_method_v2* im = nullptr;
            zwp_input_method_keyboard_grab_v2* grab = nullptr;
            wl_keyboard* kb = nullptr;
            const auto both = [&] {
                (void)wl_display_roundtrip(app.display);
                (void)wl_display_roundtrip(ime.display);
                (void)wl_display_roundtrip(app.display);
            };
            if (ok) {
                kb = wl_seat_get_keyboard(app.seat);
                wl_keyboard_add_listener(kb, &app_kb, nullptr);
                map_window(app, w);
                ti = zwp_text_input_manager_v3_get_text_input(app.ti_manager, app.seat);
                zwp_text_input_v3_add_listener(ti, &ti_l, nullptr);
                both();
            }
            check("ime: no text-input focus while no input method is bound", ok && al.enters == 0,
                  "enters " + std::to_string(al.enters));
            if (ok) {
                im = zwp_input_method_manager_v2_get_input_method(ime.im_manager, ime.seat);
                zwp_input_method_v2_add_listener(im, &im_l, nullptr);
                both();
            }
            check("ime: the focused window's text input is entered once an input method binds",
                  ok && al.enters == 1 && il.unavailable == 0,
                  "enters " + std::to_string(al.enters));
            unsigned commits = 0;
            if (ok) {
                zwp_text_input_v3_enable(ti);
                zwp_text_input_v3_set_surrounding_text(ti, "abc", 3, 3);
                zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK,
                                                   ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
                zwp_text_input_v3_set_cursor_rectangle(ti, 20, 30, 2, 18);
                zwp_text_input_v3_commit(ti);
                ++commits;
                both();
            }
            check("ime: enable → activate, surrounding text, content type, done",
                  ok && il.activates == 1 && il.dones == 1 && il.surrounding == "abc" &&
                      il.hint == ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK,
                  "activates " + std::to_string(il.activates) + ", dones " +
                      std::to_string(il.dones) + ", surrounding '" + il.surrounding + "'");
            if (ok) {
                grab = zwp_input_method_v2_grab_keyboard(im);
                zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_l, nullptr);
                both();
            }
            check("ime: the grab gets the seat keymap, repeat info and the modifiers",
                  ok && il.keymaps == 1 && il.keymap_size > 0 && il.repeat == 1 && il.grab_mods >= 1,
                  "keymaps " + std::to_string(il.keymaps) + ", repeat " +
                      std::to_string(il.repeat) + ", mods " + std::to_string(il.grab_mods));
            const unsigned app_keys0 = al.kb_keys;
            if (ok) {
                key(30, true); // KEY_A
                both();
            }
            check("ime: a key press while active goes to the grab, not the window",
                  ok && il.grab_keys == 1 && il.last_key == 30 && al.kb_keys == app_keys0,
                  "grab keys " + std::to_string(il.grab_keys) + ", window keys " +
                      std::to_string(al.kb_keys - app_keys0));
            if (ok) {
                zwp_input_method_v2_set_preedit_string(im, "\xce\xb1", 2, 2); // α
                zwp_input_method_v2_commit(im, il.dones);
                both();
            }
            check("ime: preedit reaches the text input with done(its commit count)",
                  ok && al.preedit == "\xce\xb1" && al.last_serial == commits,
                  "preedit '" + al.preedit + "', serial " + std::to_string(al.last_serial));
            if (ok) {
                zwp_input_method_v2_commit_string(im, "\xce\xb1");
                zwp_input_method_v2_commit(im, il.dones + 7); // stale: dropped
                zwp_input_method_v2_commit(im, il.dones);
                both();
            }
            check("ime: a stale commit is dropped (its commit string never arrives)",
                  ok && al.commit.empty(), "commit '" + al.commit + "'");
            if (ok) {
                zwp_input_method_v2_commit_string(im, "\xce\xb1");
                zwp_input_method_v2_commit(im, il.dones);
                both();
            }
            check("ime: commit_string reaches the text input", ok && al.commit == "\xce\xb1",
                  "commit '" + al.commit + "'");
            // A popup at the cursor: the v2 rectangle applies with the window's next commit.
            wl_surface* ps = nullptr;
            zwp_input_popup_surface_v2* popup = nullptr;
            if (ok) {
                ps = wl_compositor_create_surface(ime.compositor);
                popup = zwp_input_method_v2_get_input_popup_surface(im, ps);
                zwp_input_popup_surface_v2_add_listener(popup, &popup_l, nullptr);
                wl_surface_attach(ps, make_buffer(ime, 120, 40), 0, 0);
                wl_surface_commit(ps);
                both();
            }
            const int before_commit_w = il.rect_w;
            if (ok) {
                wl_surface_commit(w.surface);
                both();
            }
            check("ime: the popup is told the cursor rectangle once the window commits (v2)",
                  ok && before_commit_w == -1 && il.rect_w == 2 && il.rect_h == 18 &&
                      il.rect_x == 0 && il.rect_y == -18,
                  "before " + std::to_string(before_commit_w) + ", rect " +
                      std::to_string(il.rect_x) + "," + std::to_string(il.rect_y) + " " +
                      std::to_string(il.rect_w) + "x" + std::to_string(il.rect_h));
            // The virtual keyboard with the grab's keymap types into the window.
            zwp_virtual_keyboard_v1* vk = nullptr;
            if (ok && il.keymap_fd >= 0) {
                vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(ime.vk_manager,
                                                                             ime.seat);
                zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, il.keymap_fd,
                                               il.keymap_size);
                zwp_virtual_keyboard_v1_key(vk, 0, 48, WL_KEYBOARD_KEY_STATE_PRESSED); // KEY_B
                zwp_virtual_keyboard_v1_key(vk, 0, 48, WL_KEYBOARD_KEY_STATE_RELEASED);
                both();
            }
            check("ime: the input method's virtual keyboard types into the window, not the grab",
                  ok && al.last_kb_key == 48 && il.grab_keys == 1,
                  "window key " + std::to_string(al.last_kb_key) + ", grab keys " +
                      std::to_string(il.grab_keys));
            // The seat's keymap forwarded verbatim switches nothing; another keymap is sent to
            // the window before the virtual key, and the seat's is restored before the next
            // physical key reaches it (below, once the input method is inactive).
            const unsigned keymaps_seat = al.keymaps;
            bool other_layout = false;
#if defined(LUMEN_TEST_XKB)
            if (ok) {
                xkb_context* xc = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                xkb_rule_names names{};
                names.layout = "de";
                xkb_keymap* km = xc ? xkb_keymap_new_from_names(xc, &names,
                                                                XKB_KEYMAP_COMPILE_NO_FLAGS)
                                    : nullptr;
                char* text = km ? xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
                if (text) {
                    const size_t len = std::strlen(text) + 1;
                    const int fd = memfd_create("lumen-vk-de", MFD_CLOEXEC);
                    if (fd >= 0 && write(fd, text, len) == static_cast<ssize_t>(len)) {
                        zwp_virtual_keyboard_v1* vk2 =
                            zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(ime.vk_manager,
                                                                                    ime.seat);
                        zwp_virtual_keyboard_v1_keymap(vk2, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                                       static_cast<uint32_t>(len));
                        zwp_virtual_keyboard_v1_key(vk2, 0, 21, WL_KEYBOARD_KEY_STATE_PRESSED);
                        zwp_virtual_keyboard_v1_key(vk2, 0, 21, WL_KEYBOARD_KEY_STATE_RELEASED);
                        both();
                        other_layout = true;
                    }
                    if (fd >= 0)
                        close(fd);
                    free(text);
                }
                if (km)
                    xkb_keymap_unref(km);
                if (xc)
                    xkb_context_unref(xc);
            }
#endif
            const unsigned keymaps_other = al.keymaps;
            check("ime: the seat's keymap through a virtual keyboard switches nothing; another "
                  "layout is sent to the window before its key",
                  ok && keymaps_seat == 1 && (!other_layout || (keymaps_other == 2 &&
                                                                al.last_kb_key == 21)),
                  "keymaps " + std::to_string(keymaps_seat) + " then " +
                      std::to_string(keymaps_other) + (other_layout ? "" : " (no xkb: skipped)"));
            // Disable: deactivate. The release of KEY_A goes where its press went (the grab).
            if (ok) {
                zwp_text_input_v3_disable(ti);
                zwp_text_input_v3_commit(ti);
                ++commits;
                both();
                key(30, false);
                both();
            }
            check("ime: disable deactivates; the held key's release still goes to the grab",
                  ok && il.deactivates == 1 && il.grab_keys == 2 && il.last_state == 0,
                  "deactivates " + std::to_string(il.deactivates) + ", grab keys " +
                      std::to_string(il.grab_keys));
            const unsigned app_keys1 = al.kb_keys;
            if (ok) {
                key(31, true); // KEY_S
                key(31, false);
                both();
            }
            check("ime: inactive, keys reach the window again (the seat's keymap restored first)",
                  ok && al.kb_keys == app_keys1 + 1 && al.last_kb_key == 31 && il.grab_keys == 2 &&
                      (!other_layout || al.keymaps == keymaps_other + 1),
                  "window keys +" + std::to_string(al.kb_keys - app_keys1) + ", keymaps " +
                      std::to_string(al.keymaps));
            // The input method goes: the text input is left (and entered again by the next).
            if (ok) {
                zwp_text_input_v3_enable(ti);
                zwp_text_input_v3_commit(ti);
                both();
                // Its virtual keyboard and grab go with it.
                wl_display_disconnect(ime.display);
                ime.display = nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                (void)wl_display_roundtrip(app.display);
            }
            check("ime: the input method's client goes — its text input is left",
                  ok && al.leaves == 1, "leaves " + std::to_string(al.leaves));
            (void)vk;
            if (il.keymap_fd >= 0) {
                close(il.keymap_fd);
                il.keymap_fd = -1;
            }
            if (kb)
                wl_keyboard_destroy(kb);
            if (ime.display)
                wl_display_disconnect(ime.display);
            if (app.display)
                wl_display_disconnect(app.display);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
#endif

    // ── Client churn: disconnects out of connection order ──
    // The server's per-client destroy listener must survive its slot being compacted. It
    // used to be indexed by slot: a client disconnecting before a later one moved that
    // later client's slot, the next connection re-initialised a listener still linked into
    // the moved client's destroy signal, and that client's disconnect then removed the wrong
    // slot (or, in longer sequences, spun forever — found by lumen-shell restarts). Checked
    // after every client is gone: the server's table must be empty.
    ++g_ran;
    bool churn_ok = true;
    {
        // Each binds globals, which is what gives a client its server slot.
        // (connect's roundtrip returns before the binds it made are processed: one more.)
        client c[4]{};
        for (int i = 0; i < 3; ++i)
            churn_ok = connect(c[i]) && wl_display_roundtrip(c[i].display) >= 0 && churn_ok;
        if (c[0].display)
            wl_display_disconnect(c[0].display); // a middle slot frees; the last moves into it
        // The server must have seen that disconnect before the next client connects.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        churn_ok = connect(c[3]) && wl_display_roundtrip(c[3].display) >= 0 && churn_ok;
        for (int i = 1; i < 4; ++i)
            if (c[i].display)
                wl_display_disconnect(c[i].display);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    // Pump stopped: the table can be read from here.
    unsigned live = 0;
    for (unsigned id = 1; id < 4096; ++id)
        if (comp.wayland().find_client(id))
            ++live;
    churn_ok = churn_ok && live == 0;
    std::printf("  %s  client churn: out-of-order disconnects leave no client slot behind "
                "(%u live)\n",
                churn_ok ? "PASS" : "FAIL", live);
    if (!churn_ok)
        ++g_failed;
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
