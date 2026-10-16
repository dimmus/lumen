// Protocol conformance: requests that must end in a specific protocol error, and request
// orders that must not. In-process compositor, one libwayland-client connection per case
// (a protocol error is fatal to its connection), asserting wl_display_get_protocol_error.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
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
#if __has_include("primary-selection-unstable-v1-client-protocol.h")
#include "primary-selection-unstable-v1-client-protocol.h"
#define LUMEN_TEST_PRIMARY_SELECTION 1
#endif
#if __has_include("xdg-activation-v1-client-protocol.h")
#include "xdg-activation-v1-client-protocol.h"
#define LUMEN_TEST_ACTIVATION 1
#endif
#endif
#endif

import lx.foundation;
import lx.runtime;
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
        while (pump.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            (void)comp.wayland().dispatch(10);
            comp.wayland().flush();
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
              in_order(b.events, {"source_actions:3", "enter", "offer-action:2", "motion",
                                  "drop", "leave"}) &&
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
