// lumen-probe dialog | icon | bell | foreign-export | foreign-import | shortcuts-inhibit |
// data-control | security-context: deterministic clients of the window and session
// protocols (design-app-compat-capture Part B, F.2). Own connection; main() dispatches here.
//
//   lumen-probe dialog [--no-modal] [--exit-after S]
//       A blue 480x320 parent and a yellow 240x140 child (xdg_toplevel.set_parent) made modal
//       with xdg_dialog_v1. Prints `dialog mapped`, then `focus parent|dialog|none` on every
//       keyboard enter/leave and `button parent|dialog` on every press it receives.
//   lumen-probe icon [--name N] [--sizes 16,32] [--exit-after S]
//       A window whose xdg_toplevel_icon_v1 has the name (default lumen-probe-icon) and one
//       generated buffer per size (a coloured square). Prints `icon set name=… buffers=…`.
//   lumen-probe bell [--count N] [--interval MS] [--null] [--exit-after S]
//       A window that rings xdg_system_bell_v1 N times (for its surface, or none with
//       --null). Prints `bell rang N`.
//   lumen-probe foreign-export [--v1] [--exit-after S]
//       A window, exported; prints `handle=H` and, if the import side is told, nothing more.
//   lumen-probe foreign-import --handle H [--v1] [--modal] [--exit-after S]
//       A window imported onto H (set_parent_of; --modal: also an xdg_dialog modal). Prints
//       `imported` or `destroyed`.
//   lumen-probe shortcuts-inhibit [--exit-after S]
//       A window that inhibits shortcuts; prints `inhibit active` / `inhibit inactive` and
//       `key code=N` for every key press it receives.
//   lumen-probe data-control watch|set TEXT|get [--primary] [--wlr] [--count N]
//       [--exit-after S]
//       Clipboard tool over ext-data-control-v1 (zwlr v2 with --wlr): watch prints
//       `selection kind=K mimes=… text=…` per change (up to N), set offers TEXT as
//       text/plain;charset=utf-8 until cancelled (prints `set` / `cancelled`), get prints the
//       current text. `data-control errors` checks used_source and invalid_offer.
//   lumen-probe security-context [--app-id ID]
//       Plays a sandbox engine: a listening socket, create_listener + commit, then a second
//       connection through it. Prints the globals that connection sees, then
//       `security-context hidden=OK|FAIL visible=OK|FAIL nested=OK|FAIL` and, after the
//       close fd hangs up, `security-context closed=OK|FAIL`.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_XDG_DIALOG_V1) && defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1) && \
    defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1) && defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V2) && \
    defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V1) &&                                            \
    defined(LUMEN_PROTOCOL_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1) &&                             \
    defined(LUMEN_PROTOCOL_EXT_DATA_CONTROL_V1) && defined(LUMEN_PROTOCOL_WLR_DATA_CONTROL_UNSTABLE_V1) && \
    defined(LUMEN_PROTOCOL_SECURITY_CONTEXT_V1)
#include "ext-data-control-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "security-context-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#define PROBE_HAS_WINDOW 1
#endif

int window_main(int argc, char** argv);

#if !defined(PROBE_HAS_WINDOW)

int window_main(int, char**) {
    std::fprintf(stderr, "lumen-probe: built without the window/session protocol glue\n");
    return 2;
}

#else

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct globals {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm = nullptr;
    xdg_wm_dialog_v1* dialogs = nullptr;
    xdg_toplevel_icon_manager_v1* icons = nullptr;
    xdg_system_bell_v1* bell = nullptr;
    zxdg_exporter_v2* exporter2 = nullptr;
    zxdg_importer_v2* importer2 = nullptr;
    zxdg_exporter_v1* exporter1 = nullptr;
    zxdg_importer_v1* importer1 = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1* inhibit = nullptr;
    ext_data_control_manager_v1* dc = nullptr;
    zwlr_data_control_manager_v1* wdc = nullptr;
    wp_security_context_manager_v1* sc = nullptr;
    std::vector<std::string> names;
    std::vector<int> icon_sizes;
} g;

template <class T>
T* bind(wl_registry* r, uint32_t name, const wl_interface* i, uint32_t v) {
    return static_cast<T*>(wl_registry_bind(r, name, i, v));
}

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* gl = static_cast<globals*>(data);
    gl->names.emplace_back(iface);
    if (!std::strcmp(iface, wl_compositor_interface.name))
        gl->compositor = bind<wl_compositor>(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!std::strcmp(iface, wl_shm_interface.name))
        gl->shm = bind<wl_shm>(r, name, &wl_shm_interface, 1);
    else if (!std::strcmp(iface, wl_seat_interface.name))
        gl->seat = bind<wl_seat>(r, name, &wl_seat_interface, 1);
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        gl->wm = bind<xdg_wm_base>(r, name, &xdg_wm_base_interface, 1);
    else if (!std::strcmp(iface, xdg_wm_dialog_v1_interface.name))
        gl->dialogs = bind<xdg_wm_dialog_v1>(r, name, &xdg_wm_dialog_v1_interface, 1);
    else if (!std::strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name)) {
        // icon_size … done follow the bind at once: listen in the same dispatch.
        static const xdg_toplevel_icon_manager_v1_listener ml = {
            [](void* d, xdg_toplevel_icon_manager_v1*, int32_t s) {
                static_cast<globals*>(d)->icon_sizes.push_back(s);
            },
            [](void*, xdg_toplevel_icon_manager_v1*) {}};
        gl->icons = bind<xdg_toplevel_icon_manager_v1>(r, name, &xdg_toplevel_icon_manager_v1_interface, 1);
        xdg_toplevel_icon_manager_v1_add_listener(gl->icons, &ml, gl);
    }
    else if (!std::strcmp(iface, xdg_system_bell_v1_interface.name))
        gl->bell = bind<xdg_system_bell_v1>(r, name, &xdg_system_bell_v1_interface, 1);
    else if (!std::strcmp(iface, zxdg_exporter_v2_interface.name))
        gl->exporter2 = bind<zxdg_exporter_v2>(r, name, &zxdg_exporter_v2_interface, 1);
    else if (!std::strcmp(iface, zxdg_importer_v2_interface.name))
        gl->importer2 = bind<zxdg_importer_v2>(r, name, &zxdg_importer_v2_interface, 1);
    else if (!std::strcmp(iface, zxdg_exporter_v1_interface.name))
        gl->exporter1 = bind<zxdg_exporter_v1>(r, name, &zxdg_exporter_v1_interface, 1);
    else if (!std::strcmp(iface, zxdg_importer_v1_interface.name))
        gl->importer1 = bind<zxdg_importer_v1>(r, name, &zxdg_importer_v1_interface, 1);
    else if (!std::strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
        gl->inhibit = bind<zwp_keyboard_shortcuts_inhibit_manager_v1>(
            r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    else if (!std::strcmp(iface, ext_data_control_manager_v1_interface.name))
        gl->dc = bind<ext_data_control_manager_v1>(r, name, &ext_data_control_manager_v1_interface, 1);
    else if (!std::strcmp(iface, zwlr_data_control_manager_v1_interface.name))
        gl->wdc = bind<zwlr_data_control_manager_v1>(r, name, &zwlr_data_control_manager_v1_interface,
                                                     version < 2 ? version : 2);
    else if (!std::strcmp(iface, wp_security_context_manager_v1_interface.name))
        gl->sc = bind<wp_security_context_manager_v1>(r, name, &wp_security_context_manager_v1_interface, 1);
}

const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};
const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

bool connect_to(globals& gl, const char* name) {
    gl.display = wl_display_connect(name);
    if (!gl.display)
        return false;
    gl.registry = wl_display_get_registry(gl.display);
    wl_registry_add_listener(gl.registry, &registry_listener, &gl);
    if (wl_display_roundtrip(gl.display) < 0)
        return false;
    if (gl.wm)
        xdg_wm_base_add_listener(gl.wm, &wm_listener, nullptr);
    return wl_display_roundtrip(gl.display) >= 0;
}

bool pump(int timeout_ms) {
    while (wl_display_prepare_read(g.display) != 0)
        if (wl_display_dispatch_pending(g.display) < 0)
            return false;
    if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(g.display);
        return false;
    }
    pollfd pfd{wl_display_get_fd(g.display), POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) > 0) {
        if (wl_display_read_events(g.display) < 0)
            return false;
    } else {
        wl_display_cancel_read(g.display);
    }
    return wl_display_dispatch_pending(g.display) >= 0;
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

/// Runs the event loop until stopped, `exit_after` seconds (0: forever) or an error.
int run_until(double exit_after) {
    const double end = exit_after > 0 ? now_s() + exit_after : 0;
    while (!g_stop) {
        if (!pump(100)) {
            std::fprintf(stderr, "lumen-probe: connection error\n");
            return 1;
        }
        if (end > 0 && now_s() >= end)
            break;
    }
    return 0;
}

wl_buffer* solid_buffer(int w, int h, uint32_t argb, uint32_t format = WL_SHM_FORMAT_XRGB8888) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-probe-window", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // A 2 px darker border, so windows are told apart on screen.
                const bool edge = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
                px[y * w + x] = edge ? (argb & 0xFF000000u) | ((argb >> 1) & 0x7F7F7Fu) : argb;
            }
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct window {
    const char* tag = "";
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* top = nullptr;
    uint32_t serial = 0;
    bool configured = false;
};

void make_window(window& w, const char* tag, const char* title) {
    static const xdg_surface_listener xl = {[](void* d, xdg_surface*, uint32_t serial) {
        auto* win = static_cast<window*>(d);
        win->serial = serial;
        win->configured = true;
    }};
    static const xdg_toplevel_listener tl = {
        [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
        [](void*, xdg_toplevel*) { g_stop = 1; },
        [](void*, xdg_toplevel*, int32_t, int32_t) {},
        [](void*, xdg_toplevel*, wl_array*) {}};
    w.tag = tag;
    w.surface = wl_compositor_create_surface(g.compositor);
    wl_surface_set_user_data(w.surface, &w);
    w.xdg = xdg_wm_base_get_xdg_surface(g.wm, w.surface);
    xdg_surface_add_listener(w.xdg, &xl, &w);
    w.top = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.top, &tl, &w);
    xdg_toplevel_set_title(w.top, title);
    xdg_toplevel_set_app_id(w.top, "lumen-probe");
}

void map_window(window& w, int width, int height, uint32_t colour) {
    wl_surface_commit(w.surface);
    while (!w.configured && !g_stop)
        if (!pump(1000))
            return;
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, solid_buffer(width, height, colour), 0, 0);
    wl_surface_damage_buffer(w.surface, 0, 0, width, height);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(g.display);
}

const char* tag_of(wl_surface* s) {
    if (!s)
        return "none";
    auto* w = static_cast<window*>(wl_surface_get_user_data(s));
    return w ? w->tag : "?";
}

// ── input reporting ──

void watch_input(bool keys, bool focus, bool buttons) {
    static bool s_keys, s_focus, s_buttons;
    s_keys = keys;
    s_focus = focus;
    s_buttons = buttons;
    static const wl_keyboard_listener kl = {
        [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
        [](void*, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) {
            if (s_focus)
                std::printf("focus %s\n", tag_of(s));
        },
        [](void*, wl_keyboard*, uint32_t, wl_surface*) {
            if (s_focus)
                std::printf("focus none\n");
        },
        [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
            if (s_keys && state == WL_KEYBOARD_KEY_STATE_PRESSED)
                std::printf("key code=%u\n", key);
        },
        [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void*, wl_keyboard*, int32_t, int32_t) {}};
    static wl_surface* pointer_surface = nullptr;
    static const wl_pointer_listener pl = {
        [](void*, wl_pointer*, uint32_t, wl_surface* s, wl_fixed_t, wl_fixed_t) { pointer_surface = s; },
        [](void*, wl_pointer*, uint32_t, wl_surface*) { pointer_surface = nullptr; },
        [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
        [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t state) {
            if (s_buttons && state == WL_POINTER_BUTTON_STATE_PRESSED)
                std::printf("button %s\n", tag_of(pointer_surface));
        },
        [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    if (!g.seat)
        return;
    wl_keyboard_add_listener(wl_seat_get_keyboard(g.seat), &kl, nullptr);
    if (buttons)
        wl_pointer_add_listener(wl_seat_get_pointer(g.seat), &pl, nullptr);
}

// ── options ──

const char* opt(int argc, char** argv, const char* name, const char* fallback = nullptr) {
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name))
            return argv[i + 1];
    return fallback;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], name))
            return true;
    return false;
}
double exit_after(int argc, char** argv) { return std::atof(opt(argc, argv, "--exit-after", "0")); }

// ── modes ──

int mode_dialog(int argc, char** argv) {
    if (!g.dialogs) {
        std::printf("dialog unsupported\n");
        return 1;
    }
    static window parent{}, child{};
    watch_input(false, true, true);
    make_window(parent, "parent", "probe parent");
    map_window(parent, 480, 320, 0xFF2050A0u);
    make_window(child, "dialog", "probe dialog");
    xdg_toplevel_set_parent(child.top, parent.top);
    auto* d = xdg_wm_dialog_v1_get_xdg_dialog(g.dialogs, child.top);
    if (!flag(argc, argv, "--no-modal"))
        xdg_dialog_v1_set_modal(d);
    map_window(child, 240, 140, 0xFFD0B020u);
    std::printf("dialog mapped modal=%d\n", flag(argc, argv, "--no-modal") ? 0 : 1);
    return run_until(exit_after(argc, argv));
}

int mode_icon(int argc, char** argv) {
    if (!g.icons) {
        std::printf("icon unsupported\n");
        return 1;
    }
    static window w{};
    make_window(w, "icon", "probe icon");
    map_window(w, 320, 200, 0xFF208040u);
    const char* name = opt(argc, argv, "--name", "lumen-probe-icon");
    auto* icon = xdg_toplevel_icon_manager_v1_create_icon(g.icons);
    if (name[0])
        xdg_toplevel_icon_v1_set_name(icon, name);
    unsigned buffers = 0;
    std::string list = opt(argc, argv, "--sizes", "16,32");
    for (std::size_t at = 0; at < list.size();) {
        const std::size_t comma = list.find(',', at);
        const int s = std::atoi(list.substr(at, comma - at).c_str());
        if (s > 0) {
            xdg_toplevel_icon_v1_add_buffer(icon, solid_buffer(s, s, 0xFFE04080u, WL_SHM_FORMAT_ARGB8888), 1);
            ++buffers;
        }
        if (comma == std::string::npos)
            break;
        at = comma + 1;
    }
    xdg_toplevel_icon_manager_v1_set_icon(g.icons, w.top, icon);
    xdg_toplevel_icon_v1_destroy(icon);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(g.display);
    std::string pref;
    for (int s : g.icon_sizes)
        pref += (pref.empty() ? "" : ",") + std::to_string(s);
    std::printf("icon set name=%s buffers=%u preferred=%s\n", name, buffers, pref.c_str());
    return run_until(exit_after(argc, argv));
}

int mode_bell(int argc, char** argv) {
    if (!g.bell) {
        std::printf("bell unsupported\n");
        return 1;
    }
    static window w{};
    make_window(w, "bell", "probe bell");
    map_window(w, 320, 200, 0xFF804020u);
    const int count = std::atoi(opt(argc, argv, "--count", "1"));
    const int interval = std::atoi(opt(argc, argv, "--interval", "300"));
    for (int i = 0; i < count && !g_stop; ++i) {
        xdg_system_bell_v1_ring(g.bell, flag(argc, argv, "--null") ? nullptr : w.surface);
        (void)wl_display_flush(g.display);
        const double until = now_s() + interval / 1000.0;
        while (now_s() < until)
            if (!pump(20))
                return 1;
    }
    std::printf("bell rang %d\n", count);
    return run_until(exit_after(argc, argv));
}

int mode_foreign_export(int argc, char** argv) {
    static window w{};
    make_window(w, "exported", "probe exported");
    map_window(w, 480, 320, 0xFF406080u);
    if (flag(argc, argv, "--v1")) {
        static const zxdg_exported_v1_listener l = {
            [](void*, zxdg_exported_v1*, const char* h) { std::printf("handle=%s\n", h); }};
        zxdg_exported_v1_add_listener(zxdg_exporter_v1_export(g.exporter1, w.surface), &l, nullptr);
    } else {
        static const zxdg_exported_v2_listener l = {
            [](void*, zxdg_exported_v2*, const char* h) { std::printf("handle=%s\n", h); }};
        zxdg_exported_v2_add_listener(zxdg_exporter_v2_export_toplevel(g.exporter2, w.surface), &l, nullptr);
    }
    return run_until(exit_after(argc, argv));
}

int mode_foreign_import(int argc, char** argv) {
    const char* handle = opt(argc, argv, "--handle", "");
    static window w{};
    make_window(w, "imported", "probe imported");
    static bool destroyed = false;
    if (flag(argc, argv, "--v1")) {
        static const zxdg_imported_v1_listener l = {[](void*, zxdg_imported_v1*) {
            destroyed = true;
            std::printf("destroyed\n");
        }};
        auto* imp = zxdg_importer_v1_import(g.importer1, handle);
        zxdg_imported_v1_add_listener(imp, &l, nullptr);
        zxdg_imported_v1_set_parent_of(imp, w.surface);
    } else {
        static const zxdg_imported_v2_listener l = {[](void*, zxdg_imported_v2*) {
            destroyed = true;
            std::printf("destroyed\n");
        }};
        auto* imp = zxdg_importer_v2_import_toplevel(g.importer2, handle);
        zxdg_imported_v2_add_listener(imp, &l, nullptr);
        zxdg_imported_v2_set_parent_of(imp, w.surface);
    }
    if (flag(argc, argv, "--modal") && g.dialogs)
        xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(g.dialogs, w.top));
    watch_input(false, true, false);
    map_window(w, 240, 140, 0xFFC08040u);
    (void)wl_display_roundtrip(g.display);
    if (!destroyed)
        std::printf("imported\n");
    return run_until(exit_after(argc, argv));
}

int mode_shortcuts_inhibit(int argc, char** argv) {
    if (!g.inhibit) {
        std::printf("shortcuts-inhibit unsupported\n");
        return 1;
    }
    static window w{};
    make_window(w, "inhibit", "probe shortcuts inhibit");
    watch_input(true, true, false);
    map_window(w, 400, 240, 0xFF603070u);
    static const zwp_keyboard_shortcuts_inhibitor_v1_listener l = {
        [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) { std::printf("inhibit active\n"); },
        [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) { std::printf("inhibit inactive\n"); }};
    auto* inh = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(g.inhibit, w.surface, g.seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inh, &l, nullptr);
    std::printf("inhibit requested\n");
    return run_until(exit_after(argc, argv));
}

// ── data control (ext, or wlr v2 with --wlr; one code path through small adapters) ──

struct dc_offer_state {
    std::vector<std::string> mimes;
};

std::string read_fd(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0)
            break;
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > 4096)
            break;
    }
    return out;
}

struct dc_client {
    bool wlr = false;
    void* device = nullptr;
    void* selection = nullptr;
    void* primary = nullptr;
    void* pending = nullptr;
    dc_offer_state pending_state{};
    dc_offer_state sel_state{};
    dc_offer_state pri_state{};
    unsigned changes = 0;
    bool first_both = false;
};
dc_client g_dc{};

void dc_offer(void*, void* offer, const char* mime) {
    if (offer == g_dc.pending)
        g_dc.pending_state.mimes.emplace_back(mime);
}

std::string dc_receive(void* offer, const char* mime) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return {};
    if (g_dc.wlr)
        zwlr_data_control_offer_v1_receive(static_cast<zwlr_data_control_offer_v1*>(offer), mime, fds[1]);
    else
        ext_data_control_offer_v1_receive(static_cast<ext_data_control_offer_v1*>(offer), mime, fds[1]);
    close(fds[1]);
    (void)wl_display_flush(g.display);
    std::string out = read_fd(fds[0]);
    close(fds[0]);
    return out;
}

const char* text_mime(const dc_offer_state& s) {
    for (const auto& m : s.mimes)
        if (m == "text/plain;charset=utf-8")
            return "text/plain;charset=utf-8";
    for (const auto& m : s.mimes)
        if (m == "text/plain" || m == "UTF8_STRING" || m == "TEXT")
            return m.c_str();
    return nullptr;
}

void dc_selection(void* offer, bool primary) {
    auto& slot = primary ? g_dc.primary : g_dc.selection;
    auto& st = primary ? g_dc.pri_state : g_dc.sel_state;
    if (slot && slot != offer) {
        if (g_dc.wlr)
            zwlr_data_control_offer_v1_destroy(static_cast<zwlr_data_control_offer_v1*>(slot));
        else
            ext_data_control_offer_v1_destroy(static_cast<ext_data_control_offer_v1*>(slot));
    }
    slot = offer;
    st = offer == g_dc.pending ? g_dc.pending_state : dc_offer_state{};
    g_dc.pending = nullptr;
    g_dc.pending_state = {};
    ++g_dc.changes;
}

const ext_data_control_offer_v1_listener ext_offer_l = {
    [](void* d, ext_data_control_offer_v1* o, const char* m) { dc_offer(d, o, m); }};
const ext_data_control_device_v1_listener ext_dev_l = {
    [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
        g_dc.pending = o;
        g_dc.pending_state = {};
        ext_data_control_offer_v1_add_listener(o, &ext_offer_l, nullptr);
    },
    [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* o) { dc_selection(o, false); },
    [](void*, ext_data_control_device_v1*) { std::printf("finished\n"); },
    [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* o) { dc_selection(o, true); }};
const zwlr_data_control_offer_v1_listener wlr_offer_l = {
    [](void* d, zwlr_data_control_offer_v1* o, const char* m) { dc_offer(d, o, m); }};
const zwlr_data_control_device_v1_listener wlr_dev_l = {
    [](void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* o) {
        g_dc.pending = o;
        g_dc.pending_state = {};
        zwlr_data_control_offer_v1_add_listener(o, &wlr_offer_l, nullptr);
    },
    [](void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* o) { dc_selection(o, false); },
    [](void*, zwlr_data_control_device_v1*) { std::printf("finished\n"); },
    [](void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* o) { dc_selection(o, true); }};

std::string g_set_text;
bool g_cancelled = false;
const ext_data_control_source_v1_listener ext_src_l = {
    [](void*, ext_data_control_source_v1*, const char*, int32_t fd) {
        (void)!write(fd, g_set_text.data(), g_set_text.size());
        close(fd);
        std::printf("served\n");
    },
    [](void*, ext_data_control_source_v1*) {
        g_cancelled = true;
        std::printf("cancelled\n");
    }};
const zwlr_data_control_source_v1_listener wlr_src_l = {
    [](void*, zwlr_data_control_source_v1*, const char*, int32_t fd) {
        (void)!write(fd, g_set_text.data(), g_set_text.size());
        close(fd);
        std::printf("served\n");
    },
    [](void*, zwlr_data_control_source_v1*) {
        g_cancelled = true;
        std::printf("cancelled\n");
    }};

void print_selection(bool primary) {
    void* offer = primary ? g_dc.primary : g_dc.selection;
    const dc_offer_state& st = primary ? g_dc.pri_state : g_dc.sel_state;
    std::string mimes;
    for (const auto& m : st.mimes)
        mimes += (mimes.empty() ? "" : ",") + m;
    std::string text;
    if (offer)
        if (const char* m = text_mime(st))
            text = dc_receive(offer, m);
    for (auto& ch : text)
        if (ch == '\n')
            ch = ' ';
    std::printf("selection kind=%s mimes=%s text=%s\n", primary ? "primary" : "clipboard",
                offer ? (mimes.empty() ? "-" : mimes.c_str()) : "none", text.c_str());
}

int dc_error_check(bool used_source) {
    globals sub{};
    if (!connect_to(sub, nullptr) || !sub.dc || !sub.seat)
        return -1;
    auto* dev = ext_data_control_manager_v1_get_data_device(sub.dc, sub.seat);
    auto* src = ext_data_control_manager_v1_create_data_source(sub.dc);
    if (used_source) {
        ext_data_control_source_v1_offer(src, "text/plain");
        ext_data_control_device_v1_set_selection(dev, src);
        ext_data_control_device_v1_set_selection(dev, src);
    } else {
        ext_data_control_device_v1_set_selection(dev, src);
        ext_data_control_source_v1_offer(src, "text/plain");
    }
    (void)wl_display_roundtrip(sub.display);
    int code = -1;
    if (wl_display_get_error(sub.display) == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        code = static_cast<int>(wl_display_get_protocol_error(sub.display, &iface, &id));
        std::printf("data-control %s -> %s/%d\n", used_source ? "used_source" : "invalid_offer",
                    iface ? iface->name : "?", code);
    }
    wl_display_disconnect(sub.display);
    return code;
}

int mode_data_control(int argc, char** argv) {
    const char* what = argc >= 3 ? argv[2] : "watch";
    if (!std::strcmp(what, "errors")) {
        const bool a = dc_error_check(true) == EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE;
        const bool b = dc_error_check(false) == EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER;
        std::printf("data-control errors used_source=%s invalid_offer=%s\n", a ? "OK" : "FAIL",
                    b ? "OK" : "FAIL");
        return a && b ? 0 : 1;
    }
    g_dc.wlr = flag(argc, argv, "--wlr");
    const bool primary = flag(argc, argv, "--primary");
    if ((g_dc.wlr && !g.wdc) || (!g_dc.wlr && !g.dc) || !g.seat) {
        std::printf("data-control unsupported\n");
        return 1;
    }
    if (g_dc.wlr) {
        auto* dev = zwlr_data_control_manager_v1_get_data_device(g.wdc, g.seat);
        zwlr_data_control_device_v1_add_listener(dev, &wlr_dev_l, nullptr);
        g_dc.device = dev;
    } else {
        auto* dev = ext_data_control_manager_v1_get_data_device(g.dc, g.seat);
        ext_data_control_device_v1_add_listener(dev, &ext_dev_l, nullptr);
        g_dc.device = dev;
    }
    (void)wl_display_roundtrip(g.display);
    std::printf("data-control device proto=%s initial_events=%u\n", g_dc.wlr ? "wlr" : "ext", g_dc.changes);
    if (!std::strcmp(what, "get")) {
        print_selection(primary);
        return 0;
    }
    if (!std::strcmp(what, "set")) {
        g_set_text = argc >= 4 ? argv[3] : "";
        if (g_dc.wlr) {
            auto* src = zwlr_data_control_manager_v1_create_data_source(g.wdc);
            zwlr_data_control_source_v1_add_listener(src, &wlr_src_l, nullptr);
            zwlr_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
            zwlr_data_control_source_v1_offer(src, "text/plain");
            auto* dev = static_cast<zwlr_data_control_device_v1*>(g_dc.device);
            if (primary)
                zwlr_data_control_device_v1_set_primary_selection(dev, src);
            else
                zwlr_data_control_device_v1_set_selection(dev, src);
        } else {
            auto* src = ext_data_control_manager_v1_create_data_source(g.dc);
            ext_data_control_source_v1_add_listener(src, &ext_src_l, nullptr);
            ext_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
            ext_data_control_source_v1_offer(src, "text/plain");
            auto* dev = static_cast<ext_data_control_device_v1*>(g_dc.device);
            if (primary)
                ext_data_control_device_v1_set_primary_selection(dev, src);
            else
                ext_data_control_device_v1_set_selection(dev, src);
        }
        (void)wl_display_roundtrip(g.display);
        std::printf("set text=%s\n", g_set_text.c_str());
        const double end = exit_after(argc, argv) > 0 ? now_s() + exit_after(argc, argv) : 0;
        while (!g_stop && !g_cancelled && (end == 0 || now_s() < end))
            if (!pump(100))
                return 1;
        return 0;
    }
    // watch
    const unsigned want = static_cast<unsigned>(std::atoi(opt(argc, argv, "--count", "0")));
    unsigned seen = g_dc.changes;
    print_selection(primary);
    unsigned printed = 0;
    const double end = exit_after(argc, argv) > 0 ? now_s() + exit_after(argc, argv) : 0;
    while (!g_stop && (end == 0 || now_s() < end) && (want == 0 || printed < want)) {
        if (!pump(100))
            return 1;
        if (g_dc.changes != seen) {
            seen = g_dc.changes;
            print_selection(primary);
            ++printed;
        }
    }
    return 0;
}

// ── security context: the probe plays the sandbox engine ──

int mode_security_context(int argc, char** argv) {
    if (!g.sc) {
        std::printf("security-context unsupported\n");
        return 1;
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    char path[108];
    std::snprintf(path, sizeof(path), "%s/lumen-probe-sc-%d", runtime ? runtime : "/tmp", getpid());
    unlink(path);
    const int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (lfd < 0 || bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(lfd, 8) != 0) {
        std::printf("security-context socket failed: %s\n", std::strerror(errno));
        return 1;
    }
    int closefds[2];
    if (pipe2(closefds, O_CLOEXEC) != 0)
        return 1;
    auto* ctx = wp_security_context_manager_v1_create_listener(g.sc, lfd, closefds[0]);
    close(lfd);
    close(closefds[0]);
    wp_security_context_v1_set_sandbox_engine(ctx, "org.lumen.probe");
    wp_security_context_v1_set_app_id(ctx, opt(argc, argv, "--app-id", "org.lumen.Probe"));
    wp_security_context_v1_set_instance_id(ctx, "1");
    wp_security_context_v1_commit(ctx);
    wp_security_context_v1_destroy(ctx);
    (void)wl_display_roundtrip(g.display);
    std::printf("security-context listener %s\n", path);

    globals boxed{};
    if (!connect_to(boxed, path)) {
        std::printf("security-context connect failed\n");
        return 1;
    }
    std::string seen;
    for (const auto& n : boxed.names)
        seen += " " + n;
    std::printf("security-context sandboxed globals:%s\n", seen.c_str());
    const auto has = [&](const char* n) {
        for (const auto& x : boxed.names)
            if (x == n)
                return true;
        return false;
    };
    const char* hidden[] = {"ext_data_control_manager_v1", "zwlr_data_control_manager_v1",
                            "zwlr_layer_shell_v1", "ext_session_lock_manager_v1",
                            "zwlr_output_manager_v1", "zwlr_output_power_manager_v1",
                            "ext_idle_notifier_v1", "zwp_input_method_manager_v2",
                            "zwp_virtual_keyboard_manager_v1", "zlm_shell_v1"};
    const char* visible[] = {"wl_compositor", "xdg_wm_base", "zxdg_exporter_v2", "xdg_wm_dialog_v1",
                             "xdg_toplevel_icon_manager_v1", "xdg_system_bell_v1"};
    bool hidden_ok = true, visible_ok = true;
    for (const char* n : hidden)
        if (has(n)) {
            hidden_ok = false;
            std::printf("security-context leaked %s\n", n);
        }
    for (const char* n : visible)
        if (!has(n)) {
            visible_ok = false;
            std::printf("security-context missing %s\n", n);
        }
    const bool nested_ok = !has("wp_security_context_manager_v1");
    std::printf("security-context hidden=%s visible=%s nested=%s\n", hidden_ok ? "OK" : "FAIL",
                visible_ok ? "OK" : "FAIL", nested_ok ? "OK" : "FAIL");
    wl_display_disconnect(boxed.display);
    // The engine is done: close_fd hangs up, the compositor stops accepting.
    close(closefds[1]);
    (void)wl_display_roundtrip(g.display);
    usleep(100000);
    wl_display* late = wl_display_connect(path);
    std::printf("security-context closed=%s\n", late ? "FAIL" : "OK");
    if (late)
        wl_display_disconnect(late);
    unlink(path);
    return hidden_ok && visible_ok && nested_ok && !late ? 0 : 1;
}

} // namespace

int window_main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    if (!connect_to(g, nullptr) || !g.compositor || !g.wm || !g.shm) {
        std::fprintf(stderr, "lumen-probe: cannot connect or bind the core globals\n");
        return 1;
    }
    const char* m = argv[1];
    int rc = 2;
    if (!std::strcmp(m, "dialog"))
        rc = mode_dialog(argc, argv);
    else if (!std::strcmp(m, "icon"))
        rc = mode_icon(argc, argv);
    else if (!std::strcmp(m, "bell"))
        rc = mode_bell(argc, argv);
    else if (!std::strcmp(m, "foreign-export"))
        rc = mode_foreign_export(argc, argv);
    else if (!std::strcmp(m, "foreign-import"))
        rc = mode_foreign_import(argc, argv);
    else if (!std::strcmp(m, "shortcuts-inhibit"))
        rc = mode_shortcuts_inhibit(argc, argv);
    else if (!std::strcmp(m, "data-control"))
        rc = mode_data_control(argc, argv);
    else if (!std::strcmp(m, "security-context"))
        rc = mode_security_context(argc, argv);
    wl_display_disconnect(g.display);
    return rc;
}

#endif
