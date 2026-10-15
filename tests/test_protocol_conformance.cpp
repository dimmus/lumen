// Protocol conformance: requests that must end in a specific protocol error, and request
// orders that must not. In-process compositor, one libwayland-client connection per case
// (a protocol error is fatal to its connection), asserting wl_display_get_protocol_error.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

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
#endif
#endif

import lx.foundation;
import lx.runtime;
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
