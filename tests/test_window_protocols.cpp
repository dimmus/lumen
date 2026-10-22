// The window and session protocols of design-app-compat-capture Part B, in-process against a
// real compositor (one libwayland-client connection per case; a protocol error is fatal to
// its connection): xdg-dialog-v1 (modal policy: focus, clicks, stacking, centring),
// xdg-toplevel-icon-v1 (every error, the memfd snapshot), xdg-system-bell-v1,
// xdg-foreign v2 and v1 (one registry, set_parent_of across clients, `destroyed`),
// keyboard-shortcuts-inhibit (active / inactive, Super+Escape, the keys reaching the client),
// ext-data-control-v1 and zwlr-data-control v2 (selections both ways, errors), and
// security-context-v1 (every error, a client on the listener sees no session-wide global,
// close_fd stops accepting, listening outlives the creator).
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_XDG_DIALOG_V1) &&             \
    defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1) && defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1) && \
    defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V2) &&                                          \
    defined(LUMEN_PROTOCOL_XDG_FOREIGN_UNSTABLE_V1) &&                                          \
    defined(LUMEN_PROTOCOL_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1) &&                           \
    defined(LUMEN_PROTOCOL_EXT_DATA_CONTROL_V1) &&                                              \
    defined(LUMEN_PROTOCOL_WLR_DATA_CONTROL_UNSTABLE_V1) &&                                     \
    defined(LUMEN_PROTOCOL_SECURITY_CONTEXT_V1)
#include "xdg-shell-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "security-context-v1-client-protocol.h"
#define LUMEN_TEST_PART_B 1
#endif
#endif

import lx.foundation;
import lx.runtime;
import lx.input;
import lx.compositor;

#if !defined(LUMEN_TEST_PART_B)

int main() {
    std::printf("SKIP: needs libwayland and the Part B protocols' glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-window-protocols";

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_data_device_manager* ddm = nullptr;
    std::vector<std::string> globals;
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    c->globals.emplace_back(iface);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
        c->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0)
        c->ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(r, name, &wl_data_device_manager_interface, 3));
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool connect(client& c, const char* socket = k_socket) {
    c.display = wl_display_connect(socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    return wl_display_roundtrip(c.display) >= 0 && wl_display_roundtrip(c.display) >= 0;
}

bool has_global(const client& c, const char* iface) {
    for (const auto& g : c.globals)
        if (g == iface)
            return true;
    return false;
}

/// Binds the global of `iface` at `version` (null when it is not advertised).
void* bind_global(client& c, const wl_interface* iface, uint32_t version) {
    struct found {
        const wl_interface* iface;
        uint32_t name = 0;
        uint32_t version = 0;
    } f{iface};
    static const wl_registry_listener listener = {
        [](void* data, wl_registry*, uint32_t name, const char* i, uint32_t v) {
            auto* fd = static_cast<found*>(data);
            if (std::strcmp(i, fd->iface->name) == 0) {
                fd->name = name;
                fd->version = v;
            }
        },
        [](void*, wl_registry*, uint32_t) {}};
    wl_registry* r = wl_display_get_registry(c.display);
    wl_registry_add_listener(r, &listener, &f);
    (void)wl_display_roundtrip(c.display);
    if (!f.name)
        return nullptr;
    return wl_registry_bind(r, f.name, iface, version < f.version ? version : f.version);
}

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
        o.interface = iface ? iface->name : "[destroyed]";
    }
    wl_display_disconnect(c.display);
    c.display = nullptr;
    return o;
}

void expect(const char* name, const char* interface, uint32_t code,
            const std::function<void(client&)>& body) {
    ++g_ran;
    client c{};
    if (!connect(c)) {
        std::printf("  FAIL  %s — could not connect\n", name);
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

void check(const char* name, bool ok, const std::string& detail) {
    ++g_ran;
    std::printf("  %s  %s — %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

/// An shm buffer of w x h filled with `argb`.
wl_buffer* make_buffer(client& c, int w, int h, uint32_t argb = 0xFF336699u,
                       uint32_t format = WL_SHM_FORMAT_ARGB8888) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int i = 0; i < w * h; ++i)
            px[i] = argb;
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct configured {
    uint32_t serial = 0;
};
const xdg_surface_listener xdg_listener = {
    [](void* data, xdg_surface*, uint32_t serial) { static_cast<configured*>(data)->serial = serial; }};

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    configured cfg{};
};

/// Creates the toplevel role (not committed): set_parent / app id / dialog go before the map.
void make_window(client& c, window& w, const char* app_id) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
}

void map_made(client& c, window& w, int width, int height) {
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.cfg.serial);
    wl_surface_attach(w.surface, make_buffer(c, width, height), 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
}

void map_window(client& c, window& w, const char* app_id, int width = 200, int height = 150) {
    make_window(c, w, app_id);
    map_made(c, w, width, height);
}

/// The record of the newest toplevel with `app_id` (a copy, read under the dispatch lock).
lx::compositor::toplevel_record record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::compositor::toplevel_record out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id()) {
            out.id = r.id;
            out.geometry = r.geometry;
            out.parent = r.parent;
            out.modal = r.modal;
            out.mapped = r.mapped;
            out.icon = r.icon;
        }
    });
    return out;
}

void inject_key(unsigned code, bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::key_event k{};
    k.evdev_keycode = code;
    k.pressed = pressed;
    g_comp->inject_key(k);
    g_comp->wayland().flush();
}

void inject_move(double x, double y) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_motion_event m{};
    m.x = x;
    m.y = y;
    g_comp->inject_pointer_motion(m);
    g_comp->wayland().flush();
}

void inject_left(bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_button_event b{};
    b.which = lx::input::button::left;
    b.evdev_code = 0x110;
    b.pressed = pressed;
    g_comp->inject_pointer_button(b);
    g_comp->wayland().flush();
}

void settle(client& c) {
    (void)wl_display_roundtrip(c.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)wl_display_roundtrip(c.display);
}

// ── keyboard and pointer bookkeeping ──

struct keys {
    unsigned enters = 0;
    std::vector<unsigned> pressed;
    unsigned buttons = 0;
    uint32_t enter_serial = 0;
};
const wl_keyboard_listener kb_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void* d, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
        static_cast<keys*>(d)->enters++;
        static_cast<keys*>(d)->enter_serial = serial;
    },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
    [](void* d, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            static_cast<keys*>(d)->pressed.push_back(key);
    },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_keyboard*, int32_t, int32_t) {}};
const wl_pointer_listener ptr_listener = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_pointer*, uint32_t, wl_surface*) {},
    [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    [](void* d, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {
        static_cast<keys*>(d)->buttons++;
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

// ── xdg-dialog-v1 ──

void dialog_cases() {
    expect("dialog: second xdg_dialog_v1 for one toplevel", "xdg_wm_dialog_v1",
           XDG_WM_DIALOG_V1_ERROR_ALREADY_USED, [](client& c) {
               auto* wm = static_cast<xdg_wm_dialog_v1*>(bind_global(c, &xdg_wm_dialog_v1_interface, 1));
               window w{};
               make_window(c, w, "dialog-twice");
               (void)xdg_wm_dialog_v1_get_xdg_dialog(wm, w.toplevel);
               (void)xdg_wm_dialog_v1_get_xdg_dialog(wm, w.toplevel);
           });
    expect("dialog: a new xdg_dialog_v1 after the first was destroyed", "none", 0, [](client& c) {
        auto* wm = static_cast<xdg_wm_dialog_v1*>(bind_global(c, &xdg_wm_dialog_v1_interface, 1));
        window w{};
        make_window(c, w, "dialog-again");
        xdg_dialog_v1_destroy(xdg_wm_dialog_v1_get_xdg_dialog(wm, w.toplevel));
        auto* d = xdg_wm_dialog_v1_get_xdg_dialog(wm, w.toplevel);
        xdg_dialog_v1_set_modal(d);
        // The toplevel goes first: the dialog is inert, not an error.
        xdg_toplevel_destroy(w.toplevel);
        xdg_dialog_v1_unset_modal(d);
    });

    // The policy: a modal child blocks its parent.
    client c{};
    keys k{};
    if (!connect(c) || !c.seat) {
        check("dialog: modal policy", false, "connect");
        return;
    }
    auto* wm = static_cast<xdg_wm_dialog_v1*>(bind_global(c, &xdg_wm_dialog_v1_interface, 1));
    auto* kb = wl_seat_get_keyboard(c.seat);
    wl_keyboard_add_listener(kb, &kb_listener, &k);
    auto* ptr = wl_seat_get_pointer(c.seat);
    wl_pointer_add_listener(ptr, &ptr_listener, &k);
    window parent{}, dialog{};
    map_window(c, parent, "modal-parent", 400, 300);
    make_window(c, dialog, "modal-dialog");
    xdg_toplevel_set_parent(dialog.toplevel, parent.toplevel);
    auto* d = xdg_wm_dialog_v1_get_xdg_dialog(wm, dialog.toplevel);
    xdg_dialog_v1_set_modal(d);
    map_made(c, dialog, 160, 100);
    settle(c);
    const auto p = record("modal-parent");
    const auto m = record("modal-dialog");
    const auto focus_is = [&](lx::toplevel_id id) {
        std::lock_guard<std::mutex> hold(*g_lock);
        return g_comp->windows().keyboard_focus() == id;
    };
    const auto above = [&](lx::toplevel_id a, lx::toplevel_id b) {
        std::lock_guard<std::mutex> hold(*g_lock);
        return g_comp->toplevels().stacking_index(a) > g_comp->toplevels().stacking_index(b);
    };
    {
        const int pcx = p.geometry.x + p.geometry.width / 2, pcy = p.geometry.y + p.geometry.height / 2;
        const int mcx = m.geometry.x + m.geometry.width / 2, mcy = m.geometry.y + m.geometry.height / 2;
        const bool centred = std::abs(pcx - mcx) <= 1 && std::abs(pcy - mcy) <= 1;
        check("dialog: a child maps centred on its parent", centred,
              "parent centre " + std::to_string(pcx) + "," + std::to_string(pcy) + " dialog centre " +
                  std::to_string(mcx) + "," + std::to_string(mcy));
    }
    check("dialog: the modal dialog has the keyboard and is above its parent",
          m.modal && m.parent == p.id && focus_is(m.id) && above(m.id, p.id),
          "modal " + std::to_string(m.modal) + " focus on dialog " + std::to_string(focus_is(m.id)));
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        g_comp->windows().activate(p.id); // the shell's request_activate, activation
    }
    check("dialog: activating the parent focuses the dialog, the parent stays below it",
          focus_is(m.id) && above(m.id, p.id), "focus on dialog " + std::to_string(focus_is(m.id)));
    // A click on the parent (a corner the dialog does not cover): consumed, dialog focused.
    const unsigned buttons_before = k.buttons;
    inject_move(p.geometry.x + 5, p.geometry.y + 5);
    inject_left(true);
    inject_left(false);
    settle(c);
    check("dialog: a click on the blocked parent is consumed and focuses the dialog",
          k.buttons == buttons_before && focus_is(m.id) && above(m.id, p.id),
          "button events " + std::to_string(k.buttons - buttons_before));
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        (void)g_comp->windows().lower(m.id);
    }
    check("dialog: lowering the dialog lowers the family, the dialog stays above",
          above(m.id, p.id), "");
    xdg_dialog_v1_unset_modal(d);
    settle(c);
    inject_left(true);
    inject_left(false);
    settle(c);
    check("dialog: after unset_modal a click on the parent reaches it and focuses it",
          k.buttons >= buttons_before + 2 && focus_is(p.id),
          "button events " + std::to_string(k.buttons - buttons_before));
    xdg_dialog_v1_set_modal(d);
    settle(c);
    check("dialog: set_modal again takes the keyboard from the focused parent", focus_is(m.id), "");
    // The dialog unmaps: the parent gets the keyboard back.
    wl_surface_attach(dialog.surface, nullptr, 0, 0);
    wl_surface_commit(dialog.surface);
    settle(c);
    check("dialog: the dialog unmapped, the parent takes focus again", focus_is(p.id), "");
    wl_display_disconnect(c.display);
}

// ── xdg-toplevel-icon-v1 ──

void icon_cases() {
    expect("icon: a non-square buffer", "xdg_toplevel_icon_v1",
           XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER, [](client& c) {
               auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
                   bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
               auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
               xdg_toplevel_icon_v1_add_buffer(icon, make_buffer(c, 32, 16), 1);
           });
    expect("icon: set_name after set_icon", "xdg_toplevel_icon_v1",
           XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, [](client& c) {
               auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
                   bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
               window w{};
               make_window(c, w, "icon-immutable");
               auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
               xdg_toplevel_icon_v1_set_name(icon, "foot");
               xdg_toplevel_icon_manager_v1_set_icon(m, w.toplevel, icon);
               xdg_toplevel_icon_v1_set_name(icon, "other");
           });
    expect("icon: add_buffer after set_icon", "xdg_toplevel_icon_v1",
           XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE, [](client& c) {
               auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
                   bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
               window w{};
               make_window(c, w, "icon-immutable2");
               auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
               xdg_toplevel_icon_manager_v1_set_icon(m, w.toplevel, icon);
               xdg_toplevel_icon_v1_add_buffer(icon, make_buffer(c, 16, 16), 1);
           });
    expect("icon: a buffer destroyed before the icon was set", "xdg_toplevel_icon_v1",
           XDG_TOPLEVEL_ICON_V1_ERROR_NO_BUFFER, [](client& c) {
               auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
                   bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
               auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
               wl_buffer* b = make_buffer(c, 16, 16);
               xdg_toplevel_icon_v1_add_buffer(icon, b, 1);
               wl_buffer_destroy(b);
           });
    expect("icon: buffers destroyed after set_icon are no error (copied)", "none", 0, [](client& c) {
        auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
            bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
        window w{};
        make_window(c, w, "icon-copied");
        auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
        wl_buffer* b = make_buffer(c, 16, 16);
        xdg_toplevel_icon_v1_add_buffer(icon, b, 1);
        xdg_toplevel_icon_manager_v1_set_icon(m, w.toplevel, icon);
        wl_buffer_destroy(b);
        xdg_toplevel_icon_v1_destroy(icon);
    });

    // The snapshot: name + buffers in sealed memfds, applied by the commit.
    client c{};
    if (!connect(c)) {
        check("icon: snapshot", false, "connect");
        return;
    }
    struct sizes {
        std::vector<int> v;
        bool done = false;
    } s{};
    static const xdg_toplevel_icon_manager_v1_listener ml = {
        [](void* d, xdg_toplevel_icon_manager_v1*, int32_t size) { static_cast<sizes*>(d)->v.push_back(size); },
        [](void* d, xdg_toplevel_icon_manager_v1*) { static_cast<sizes*>(d)->done = true; }};
    auto* m = static_cast<xdg_toplevel_icon_manager_v1*>(
        bind_global(c, &xdg_toplevel_icon_manager_v1_interface, 1));
    xdg_toplevel_icon_manager_v1_add_listener(m, &ml, &s);
    window w{};
    map_window(c, w, "icon-app");
    auto* icon = xdg_toplevel_icon_manager_v1_create_icon(m);
    xdg_toplevel_icon_v1_set_name(icon, "utilities-terminal");
    xdg_toplevel_icon_v1_add_buffer(icon, make_buffer(c, 32, 32, 0xFF112233u), 1);
    xdg_toplevel_icon_v1_add_buffer(icon, make_buffer(c, 16, 16, 0x00445566u, WL_SHM_FORMAT_XRGB8888), 1);
    xdg_toplevel_icon_manager_v1_set_icon(m, w.toplevel, icon);
    xdg_toplevel_icon_v1_destroy(icon);
    settle(c);
    const unsigned before_commit = record("icon-app").icon;
    wl_surface_commit(w.surface);
    settle(c);
    const auto rec = record("icon-app");
    std::string detail = "sizes";
    for (int v : s.v)
        detail += " " + std::to_string(v);
    bool ok = s.done && before_commit == 0 && rec.icon != 0;
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        const auto* e = g_comp->toplevel_icons().get(rec.icon);
        ok = ok && e && std::strcmp(e->name, "utilities-terminal") == 0 && e->count == 2;
        if (e) {
            detail += std::string(" name ") + e->name + " images " + std::to_string(e->count);
            for (unsigned i = 0; i < e->count; ++i) {
                const int edge = e->images[i].size;
                void* p = mmap(nullptr, static_cast<std::size_t>(edge) * edge * 4, PROT_READ, MAP_SHARED,
                               e->images[i].fd, 0);
                const int seals = fcntl(e->images[i].fd, F_GET_SEALS);
                const bool sealed = seals >= 0 && (seals & F_SEAL_WRITE) && (seals & F_SEAL_SHRINK);
                uint32_t first = p != MAP_FAILED ? *static_cast<uint32_t*>(p) : 0;
                if (p != MAP_FAILED)
                    munmap(p, static_cast<std::size_t>(edge) * edge * 4);
                // XRGB becomes opaque ARGB.
                const uint32_t want = edge == 32 ? 0xFF112233u : 0xFF445566u;
                ok = ok && sealed && first == want;
                detail += " [" + std::to_string(edge) + "px sealed " + std::to_string(sealed) +
                          " px " + std::to_string(first == want) + "]";
            }
        }
    }
    check("icon: set_icon snapshots name and buffers into sealed memfds, applied at commit", ok,
          detail);
    // Reset with null: the default icon after the next commit.
    xdg_toplevel_icon_manager_v1_set_icon(m, w.toplevel, nullptr);
    wl_surface_commit(w.surface);
    settle(c);
    check("icon: set_icon(null) resets to the default", record("icon-app").icon == 0, "");
    wl_display_disconnect(c.display);
}

// ── xdg-system-bell-v1 ──

void bell_cases() {
    expect("bell: ring with and without a surface, faster than the rate limit", "none", 0,
           [](client& c) {
               auto* b = static_cast<xdg_system_bell_v1*>(bind_global(c, &xdg_system_bell_v1_interface, 1));
               window w{};
               map_window(c, w, "bell-app");
               for (int i = 0; i < 10; ++i)
                   xdg_system_bell_v1_ring(b, i % 2 ? w.surface : nullptr);
           });
}

// ── xdg-foreign ──

struct exported_handle {
    std::string handle;
};
const zxdg_exported_v2_listener exported_v2_listener = {
    [](void* d, zxdg_exported_v2*, const char* h) { static_cast<exported_handle*>(d)->handle = h; }};
const zxdg_exported_v1_listener exported_v1_listener = {
    [](void* d, zxdg_exported_v1*, const char* h) { static_cast<exported_handle*>(d)->handle = h; }};
const zxdg_imported_v2_listener imported_v2_listener = {
    [](void* d, zxdg_imported_v2*) { *static_cast<int*>(d) += 1; }};

void foreign_cases() {
    expect("foreign v2: export of a surface that is not a toplevel", "zxdg_exporter_v2",
           ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE, [](client& c) {
               auto* e = static_cast<zxdg_exporter_v2*>(bind_global(c, &zxdg_exporter_v2_interface, 1));
               (void)zxdg_exporter_v2_export_toplevel(e, wl_compositor_create_surface(c.compositor));
           });
    expect("foreign v2: set_parent_of a surface that is not a toplevel", "zxdg_imported_v2",
           ZXDG_IMPORTED_V2_ERROR_INVALID_SURFACE, [](client& c) {
               auto* i = static_cast<zxdg_importer_v2*>(bind_global(c, &zxdg_importer_v2_interface, 1));
               auto* imp = zxdg_importer_v2_import_toplevel(i, "0123456789abcdef0123456789abcdef");
               zxdg_imported_v2_set_parent_of(imp, wl_compositor_create_surface(c.compositor));
           });
    expect("foreign v1: a non-toplevel export is no error (its handle imports as destroyed)", "none",
           0, [](client& c) {
               auto* e = static_cast<zxdg_exporter_v1*>(bind_global(c, &zxdg_exporter_v1_interface, 1));
               (void)zxdg_exporter_v1_export(e, wl_compositor_create_surface(c.compositor));
           });

    client a{}, b{};
    if (!connect(a) || !connect(b)) {
        check("foreign: across clients", false, "connect");
        return;
    }
    // An unknown handle: destroyed at once.
    auto* bi = static_cast<zxdg_importer_v2*>(bind_global(b, &zxdg_importer_v2_interface, 1));
    int unknown_destroyed = 0;
    auto* bogus = zxdg_importer_v2_import_toplevel(bi, "ffffffffffffffffffffffffffffffff");
    zxdg_imported_v2_add_listener(bogus, &imported_v2_listener, &unknown_destroyed);
    settle(b);
    check("foreign: an unknown handle is destroyed immediately", unknown_destroyed == 1, "");

    auto* ae = static_cast<zxdg_exporter_v2*>(bind_global(a, &zxdg_exporter_v2_interface, 1));
    window aw{}, bw{};
    map_window(a, aw, "foreign-parent", 400, 300);
    exported_handle h{};
    auto* exp = zxdg_exporter_v2_export_toplevel(ae, aw.surface);
    zxdg_exported_v2_add_listener(exp, &exported_v2_listener, &h);
    settle(a);
    bool hex = h.handle.size() == 32;
    for (char ch : h.handle)
        hex = hex && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    check("foreign: the handle is 32 hex digits (128 random bits)", hex, h.handle);
    make_window(b, bw, "foreign-child");
    int destroyed = 0;
    auto* imp = zxdg_importer_v2_import_toplevel(bi, h.handle.c_str());
    zxdg_imported_v2_add_listener(imp, &imported_v2_listener, &destroyed);
    zxdg_imported_v2_set_parent_of(imp, bw.surface);
    auto* wm = static_cast<xdg_wm_dialog_v1*>(bind_global(b, &xdg_wm_dialog_v1_interface, 1));
    xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(wm, bw.toplevel));
    map_made(b, bw, 160, 100);
    settle(b);
    const auto pa = record("foreign-parent");
    const auto cb = record("foreign-child");
    lx::toplevel_id blocker{};
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        blocker = g_comp->windows().modal_blocker(pa.id);
    }
    check("foreign: set_parent_of parents across clients; a modal child blocks the other client's window",
          cb.parent == pa.id && blocker == cb.id,
          "parent " + std::to_string(cb.parent.id()) + " want " + std::to_string(pa.id.id()));
    // v1 export, v2 import: one registry.
    auto* ae1 = static_cast<zxdg_exporter_v1*>(bind_global(a, &zxdg_exporter_v1_interface, 1));
    exported_handle h1{};
    auto* exp1 = zxdg_exporter_v1_export(ae1, aw.surface);
    zxdg_exported_v1_add_listener(exp1, &exported_v1_listener, &h1);
    settle(a);
    int destroyed1 = 0;
    auto* imp1 = zxdg_importer_v2_import_toplevel(bi, h1.handle.c_str());
    zxdg_imported_v2_add_listener(imp1, &imported_v2_listener, &destroyed1);
    settle(b);
    check("foreign: a v1 export imports over v2", destroyed1 == 0 && h1.handle.size() == 32, h1.handle);
    // The exported object goes: the import is told, the relation undone.
    zxdg_exported_v2_destroy(exp);
    settle(a);
    settle(b);
    check("foreign: destroying the export sends destroyed and unparents the child",
          destroyed == 1 && record("foreign-child").parent == lx::toplevel_id{},
          "destroyed " + std::to_string(destroyed));
    // The window behind the v1 export goes: that import is told too.
    xdg_toplevel_destroy(aw.toplevel);
    settle(a);
    settle(b);
    check("foreign: destroying the exported window sends destroyed", destroyed1 == 1, "");
    wl_display_disconnect(a.display);
    wl_display_disconnect(b.display);
}

// ── keyboard-shortcuts-inhibit ──

struct inhibit_events {
    unsigned active = 0;
    unsigned inactive = 0;
};
const zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    [](void* d, zwp_keyboard_shortcuts_inhibitor_v1*) { static_cast<inhibit_events*>(d)->active++; },
    [](void* d, zwp_keyboard_shortcuts_inhibitor_v1*) { static_cast<inhibit_events*>(d)->inactive++; }};

void inhibit_cases() {
    expect("inhibit: a second inhibitor for one surface", "zwp_keyboard_shortcuts_inhibit_manager_v1",
           ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED, [](client& c) {
               auto* m = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                   bind_global(c, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
               wl_surface* s = wl_compositor_create_surface(c.compositor);
               (void)zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, c.seat);
               (void)zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, c.seat);
           });
    client c{};
    keys k{};
    if (!connect(c) || !c.seat) {
        check("inhibit: active/inactive", false, "connect");
        return;
    }
    auto* kb = wl_seat_get_keyboard(c.seat);
    wl_keyboard_add_listener(kb, &kb_listener, &k);
    auto* m = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
        bind_global(c, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
    window w{};
    map_window(c, w, "inhibit-app");
    inhibit_events ev{};
    auto* inh = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, w.surface, c.seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inh, &inhibitor_listener, &ev);
    settle(c);
    check("inhibit: the focused surface's inhibitor is active at once", ev.active == 1,
          "active " + std::to_string(ev.active));
    // Super+Escape: the compositor's, never inhibited — the inhibitor goes inactive.
    constexpr unsigned k_meta = 125, k_esc = 1, k_a = 30;
    k.pressed.clear();
    inject_key(k_meta, true);
    inject_key(k_esc, true);
    inject_key(k_esc, false);
    inject_key(k_meta, false);
    settle(c);
    bool esc_seen = false, meta_seen = false;
    for (unsigned key : k.pressed) {
        esc_seen = esc_seen || key == k_esc;
        meta_seen = meta_seen || key == k_meta;
    }
    check("inhibit: Super+Escape turns it off (inactive); Escape never reaches the client",
          ev.inactive == 1 && !esc_seen && meta_seen,
          "inactive " + std::to_string(ev.inactive) + " esc seen " + std::to_string(esc_seen));
    inject_key(k_meta, true);
    inject_key(k_esc, true);
    inject_key(k_esc, false);
    inject_key(k_meta, false);
    settle(c);
    check("inhibit: Super+Escape again turns it back on (active)", ev.active == 2, "active " + std::to_string(ev.active));
    // With it active, ordinary keys still go to the client (nothing else changes for it).
    k.pressed.clear();
    inject_key(k_a, true);
    inject_key(k_a, false);
    settle(c);
    check("inhibit: keys reach the inhibiting client", k.pressed.size() == 1 && k.pressed[0] == k_a, "");
    // Its surface loses the keyboard (another window): no inactive (the spec), and nothing is
    // inhibited; regaining it sends active again.
    window other{};
    map_window(c, other, "inhibit-other");
    settle(c);
    const unsigned inactive_before = ev.inactive;
    {
        const lx::toplevel_id id = record("inhibit-app").id;
        std::lock_guard<std::mutex> hold(*g_lock);
        g_comp->windows().activate(id);
    }
    settle(c);
    check("inhibit: focus away sends nothing, focus back sends active",
          ev.inactive == inactive_before && ev.active == 3, "active " + std::to_string(ev.active));
    // Super+Escape on a surface without an inhibitor is the client's key.
    {
        const lx::toplevel_id id = record("inhibit-other").id;
        std::lock_guard<std::mutex> hold(*g_lock);
        g_comp->windows().activate(id);
    }
    settle(c);
    k.pressed.clear();
    inject_key(k_meta, true);
    inject_key(k_esc, true);
    inject_key(k_esc, false);
    inject_key(k_meta, false);
    settle(c);
    esc_seen = false;
    for (unsigned key : k.pressed)
        esc_seen = esc_seen || key == k_esc;
    check("inhibit: without an inhibitor Super+Escape reaches the client", esc_seen, "");
    wl_display_disconnect(c.display);
}

// ── data control ──

struct dc_state {
    std::vector<std::string> events;
    ext_data_control_offer_v1* selection = nullptr;
    ext_data_control_offer_v1* primary = nullptr;
    zwlr_data_control_offer_v1* wlr_primary = nullptr;
    unsigned wlr_primary_events = 0;
    const char* payload = "";
    bool cancelled = false;
};
const ext_data_control_offer_v1_listener dc_offer_listener = {
    [](void* d, ext_data_control_offer_v1*, const char* m) {
        static_cast<dc_state*>(d)->events.push_back(std::string("offer:") + m);
    }};
const ext_data_control_device_v1_listener dc_device_listener = {
    [](void* d, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
        static_cast<dc_state*>(d)->events.push_back("data_offer");
        ext_data_control_offer_v1_add_listener(o, &dc_offer_listener, d);
    },
    [](void* d, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
        auto* s = static_cast<dc_state*>(d);
        s->events.push_back(o ? "selection" : "selection:nil");
        s->selection = o;
    },
    [](void* d, ext_data_control_device_v1*) { static_cast<dc_state*>(d)->events.push_back("finished"); },
    [](void* d, ext_data_control_device_v1*, ext_data_control_offer_v1* o) {
        auto* s = static_cast<dc_state*>(d);
        s->events.push_back(o ? "primary" : "primary:nil");
        s->primary = o;
    }};
const ext_data_control_source_v1_listener dc_source_listener = {
    [](void* d, ext_data_control_source_v1*, const char*, int32_t fd) {
        const char* p = static_cast<dc_state*>(d)->payload;
        (void)!write(fd, p, std::strlen(p));
        close(fd);
    },
    [](void* d, ext_data_control_source_v1*) { static_cast<dc_state*>(d)->cancelled = true; }};

struct app_state {
    wl_data_offer* offer = nullptr;
    const char* payload = "";
    bool cancelled = false;
};
const wl_data_offer_listener app_offer_listener = {[](void*, wl_data_offer*, const char*) {},
                                                   [](void*, wl_data_offer*, uint32_t) {},
                                                   [](void*, wl_data_offer*, uint32_t) {}};
const wl_data_device_listener app_dd_listener = {
    [](void*, wl_data_device*, wl_data_offer* o) { wl_data_offer_add_listener(o, &app_offer_listener, nullptr); },
    [](void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {},
    [](void*, wl_data_device*) {},
    [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_data_device*) {},
    [](void* d, wl_data_device*, wl_data_offer* o) { static_cast<app_state*>(d)->offer = o; }};
const wl_data_source_listener app_src_listener = {
    [](void*, wl_data_source*, const char*) {},
    [](void* d, wl_data_source*, const char*, int32_t fd) {
        const char* p = static_cast<app_state*>(d)->payload;
        (void)!write(fd, p, std::strlen(p));
        close(fd);
    },
    [](void* d, wl_data_source*) { static_cast<app_state*>(d)->cancelled = true; },
    [](void*, wl_data_source*) {},
    [](void*, wl_data_source*) {},
    [](void*, wl_data_source*, uint32_t) {}};

/// Reads what arrives on a pipe whose write end `receive` hands over, dispatching both.
template <typename Receive>
std::string read_pipe(client& reader, client& owner, Receive receive) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return "<pipe>";
    receive(fds[1]);
    close(fds[1]);
    (void)wl_display_roundtrip(reader.display);
    (void)wl_display_roundtrip(owner.display);
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

void data_control_cases() {
    expect("data control: set_selection with a used source", "ext_data_control_device_v1",
           EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE, [](client& c) {
               auto* m = static_cast<ext_data_control_manager_v1*>(
                   bind_global(c, &ext_data_control_manager_v1_interface, 1));
               auto* dev = ext_data_control_manager_v1_get_data_device(m, c.seat);
               auto* src = ext_data_control_manager_v1_create_data_source(m);
               ext_data_control_source_v1_offer(src, "text/plain");
               ext_data_control_device_v1_set_selection(dev, src);
               ext_data_control_device_v1_set_primary_selection(dev, src);
           });
    expect("data control: offer after the source was used", "ext_data_control_source_v1",
           EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER, [](client& c) {
               auto* m = static_cast<ext_data_control_manager_v1*>(
                   bind_global(c, &ext_data_control_manager_v1_interface, 1));
               auto* dev = ext_data_control_manager_v1_get_data_device(m, c.seat);
               auto* src = ext_data_control_manager_v1_create_data_source(m);
               ext_data_control_device_v1_set_selection(dev, src);
               ext_data_control_source_v1_offer(src, "text/plain");
           });
    expect("data control (wlr v2): set_primary_selection with a used source",
           "zwlr_data_control_device_v1", ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE, [](client& c) {
               auto* m = static_cast<zwlr_data_control_manager_v1*>(
                   bind_global(c, &zwlr_data_control_manager_v1_interface, 2));
               auto* dev = zwlr_data_control_manager_v1_get_data_device(m, c.seat);
               auto* src = zwlr_data_control_manager_v1_create_data_source(m);
               zwlr_data_control_device_v1_set_primary_selection(dev, src);
               zwlr_data_control_device_v1_set_selection(dev, src);
           });

    // A focused application and a clipboard tool.
    client app{}, tool{};
    app_state as{};
    keys k{};
    if (!connect(app) || !connect(tool) || !app.seat || !tool.seat || !app.ddm) {
        check("data control: selections", false, "connect");
        return;
    }
    auto* akb = wl_seat_get_keyboard(app.seat);
    wl_keyboard_add_listener(akb, &kb_listener, &k);
    auto* add = wl_data_device_manager_get_data_device(app.ddm, app.seat);
    wl_data_device_add_listener(add, &app_dd_listener, &as);
    window aw{};
    map_window(app, aw, "dc-app");
    settle(app);
    dc_state ts{};
    auto* m = static_cast<ext_data_control_manager_v1*>(
        bind_global(tool, &ext_data_control_manager_v1_interface, 1));
    auto* dev = ext_data_control_manager_v1_get_data_device(m, tool.seat);
    ext_data_control_device_v1_add_listener(dev, &dc_device_listener, &ts);
    settle(tool);
    const bool both_at_bind =
        ts.events.size() >= 2 && ts.events[0] == "selection:nil" && ts.events[1] == "primary:nil";
    check("data control: both selections are sent when the device is created", both_at_bind,
          ts.events.empty() ? "none" : ts.events[0]);
    // The tool sets the clipboard (wl-copy): the focused app is offered it and reads it.
    auto* src = ext_data_control_manager_v1_create_data_source(m);
    ext_data_control_source_v1_add_listener(src, &dc_source_listener, &ts);
    ext_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
    ts.payload = "from-wl-copy";
    ext_data_control_device_v1_set_selection(dev, src);
    settle(tool);
    settle(app);
    std::string got;
    if (as.offer)
        got = read_pipe(app, tool, [&](int fd) {
            wl_data_offer_receive(as.offer, "text/plain;charset=utf-8", fd);
        });
    check("data control: a tool's selection reaches the focused application without a serial",
          got == "from-wl-copy", "read \"" + got + "\"");
    // The app copies (Ctrl+C): the tool hears of it and reads it (cliphist's store); the
    // tool's source is cancelled.
    auto* asrc = wl_data_device_manager_create_data_source(app.ddm);
    wl_data_source_add_listener(asrc, &app_src_listener, &as);
    wl_data_source_offer(asrc, "text/plain");
    as.payload = "from-app";
    ts.events.clear();
    wl_data_device_set_selection(add, asrc, k.enter_serial);
    settle(app);
    settle(tool);
    std::string watched;
    if (ts.selection)
        watched = read_pipe(tool, app, [&](int fd) {
            ext_data_control_offer_v1_receive(ts.selection, "text/plain", fd);
        });
    check("data control: an application's copy reaches the tool, whose source is cancelled",
          watched == "from-app" && ts.cancelled,
          "read \"" + watched + "\" cancelled " + std::to_string(ts.cancelled));
    // wlr v2 at bind: primary_selection is sent too.
    dc_state ws{};
    static const zwlr_data_control_device_v1_listener wl = {
        [](void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {},
        [](void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {},
        [](void*, zwlr_data_control_device_v1*) {},
        [](void* d, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {
            static_cast<dc_state*>(d)->wlr_primary_events++;
        }};
    auto* wm = static_cast<zwlr_data_control_manager_v1*>(
        bind_global(tool, &zwlr_data_control_manager_v1_interface, 2));
    auto* wdev = zwlr_data_control_manager_v1_get_data_device(wm, tool.seat);
    zwlr_data_control_device_v1_add_listener(wdev, &wl, &ws);
    settle(tool);
    check("data control (wlr v2): primary_selection at bind", ws.wlr_primary_events == 1, "");
    // The owner exits: the selection is cleared, the tool told (a manager may take it over).
    ts.events.clear();
    wl_display_disconnect(app.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    settle(tool);
    bool cleared = false;
    for (const auto& e : ts.events)
        cleared = cleared || e == "selection:nil";
    check("data control: the owner's exit clears the selection for the tool", cleared, "");
    wl_display_disconnect(tool.display);
}

// ── security-context-v1 ──

void security_cases() {
    expect("security context: a listen fd that is not a listening socket",
           "wp_security_context_manager_v1", WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD,
           [](client& c) {
               auto* m = static_cast<wp_security_context_manager_v1*>(
                   bind_global(c, &wp_security_context_manager_v1_interface, 1));
               int p[2];
               if (pipe(p) != 0)
                   return;
               (void)wp_security_context_manager_v1_create_listener(m, p[0], p[1]);
               close(p[0]);
               close(p[1]);
           });
    const auto listening = [] {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/lumen-test-sc-%d-%ld", getpid(),
                      static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000));
        unlink(a.sun_path);
        (void)bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        (void)listen(fd, 8);
        return std::pair<int, std::string>{fd, a.sun_path};
    };
    expect("security context: set_app_id twice", "wp_security_context_v1",
           WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET, [&](client& c) {
               auto* m = static_cast<wp_security_context_manager_v1*>(
                   bind_global(c, &wp_security_context_manager_v1_interface, 1));
               auto [fd, path] = listening();
               int p[2];
               (void)!pipe(p);
               auto* ctx = wp_security_context_manager_v1_create_listener(m, fd, p[0]);
               close(fd);
               close(p[0]);
               wp_security_context_v1_set_app_id(ctx, "org.example.App");
               wp_security_context_v1_set_app_id(ctx, "org.example.App");
               unlink(path.c_str());
               close(p[1]);
           });
    expect("security context: empty metadata", "wp_security_context_v1",
           WP_SECURITY_CONTEXT_V1_ERROR_INVALID_METADATA, [&](client& c) {
               auto* m = static_cast<wp_security_context_manager_v1*>(
                   bind_global(c, &wp_security_context_manager_v1_interface, 1));
               auto [fd, path] = listening();
               int p[2];
               (void)!pipe(p);
               auto* ctx = wp_security_context_manager_v1_create_listener(m, fd, p[0]);
               close(fd);
               close(p[0]);
               wp_security_context_v1_set_sandbox_engine(ctx, "");
               unlink(path.c_str());
               close(p[1]);
           });
    expect("security context: a request after commit", "wp_security_context_v1",
           WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED, [&](client& c) {
               auto* m = static_cast<wp_security_context_manager_v1*>(
                   bind_global(c, &wp_security_context_manager_v1_interface, 1));
               auto [fd, path] = listening();
               int p[2];
               (void)!pipe(p);
               auto* ctx = wp_security_context_manager_v1_create_listener(m, fd, p[0]);
               close(fd);
               close(p[0]);
               wp_security_context_v1_commit(ctx);
               wp_security_context_v1_set_instance_id(ctx, "1");
               unlink(path.c_str());
               close(p[1]); // stops the listener the commit registered
           });

    // A sandbox engine's listener: its clients are sandboxed.
    client creator{};
    if (!connect(creator)) {
        check("security context: sandboxed client", false, "connect");
        return;
    }
    auto* m = static_cast<wp_security_context_manager_v1*>(
        bind_global(creator, &wp_security_context_manager_v1_interface, 1));
    auto [fd, path] = listening();
    int p[2];
    (void)!pipe(p);
    auto* ctx = wp_security_context_manager_v1_create_listener(m, fd, p[0]);
    close(fd); // "closing listen_fd and close_fd remains the only valid operation"
    close(p[0]);
    wp_security_context_v1_set_sandbox_engine(ctx, "org.flatpak");
    wp_security_context_v1_set_app_id(ctx, "org.example.Sandboxed");
    wp_security_context_v1_set_instance_id(ctx, "42");
    wp_security_context_v1_commit(ctx);
    wp_security_context_v1_destroy(ctx);
    settle(creator);
    // Listening outlives the creator.
    wl_display_disconnect(creator.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client boxed{};
    const bool connected = connect(boxed, path.c_str());
    const char* hidden[] = {"ext_data_control_manager_v1", "zwlr_data_control_manager_v1",
                            "wp_security_context_manager_v1", "zwlr_layer_shell_v1",
                            "ext_session_lock_manager_v1", "zwlr_output_manager_v1",
                            "ext_idle_notifier_v1", "zwp_input_method_manager_v2",
                            "zwp_virtual_keyboard_manager_v1", "zwlr_output_power_manager_v1",
                            "zlm_shell_v1"};
    const char* visible[] = {"wl_compositor", "xdg_wm_base", "zxdg_exporter_v2", "zxdg_importer_v2",
                             "xdg_wm_dialog_v1", "xdg_toplevel_icon_manager_v1", "xdg_system_bell_v1",
                             "zwp_keyboard_shortcuts_inhibit_manager_v1"};
    std::string leaked, missing;
    for (const char* g : hidden)
        if (connected && has_global(boxed, g))
            leaked += std::string(" ") + g;
    for (const char* g : visible)
        if (connected && !has_global(boxed, g))
            missing += std::string(" ") + g;
    bool sandboxed = false;
    if (connected) {
        std::lock_guard<std::mutex> hold(*g_lock);
        for (unsigned id = 1; id < 4096; ++id) {
            auto* conn = g_comp->wayland().find_client(id);
            if (conn && conn->context() &&
                std::strcmp(conn->context()->app_id, "org.example.Sandboxed") == 0 &&
                std::strcmp(conn->context()->sandbox_engine, "org.flatpak") == 0)
                sandboxed = conn->sandboxed();
        }
    }
    check("security context: a client on the listener is sandboxed and sees no session-wide global",
          connected && leaked.empty() && missing.empty() && sandboxed,
          std::string(connected ? "" : "no connection") + (leaked.empty() ? "" : " leaked:" + leaked) +
              (missing.empty() ? "" : " missing:" + missing) + " context " + std::to_string(sandboxed));
    if (connected)
        wl_display_disconnect(boxed.display);
    // close_fd hangs up: the compositor stops accepting (and closes its listen fd).
    close(p[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client late{};
    const bool late_connected = connect(late, path.c_str());
    check("security context: after close_fd hangs up no client is accepted", !late_connected, "");
    if (late.display)
        wl_display_disconnect(late.display);
    unlink(path.c_str());
}

void globals_case() {
    client c{};
    if (!connect(c)) {
        check("globals", false, "connect");
        return;
    }
    const char* want[] = {"xdg_wm_dialog_v1", "xdg_toplevel_icon_manager_v1", "xdg_system_bell_v1",
                          "zxdg_exporter_v2", "zxdg_importer_v2", "zxdg_exporter_v1",
                          "zxdg_importer_v1", "zwp_keyboard_shortcuts_inhibit_manager_v1",
                          "ext_data_control_manager_v1", "zwlr_data_control_manager_v1",
                          "wp_security_context_manager_v1"};
    std::string missing;
    for (const char* g : want)
        if (!has_global(c, g))
            missing += std::string(" ") + g;
    check("every Part B global is advertised to an unsandboxed client", missing.empty(),
          missing.empty() ? "all 11" : "missing:" + missing);
    wl_display_disconnect(c.display);
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
    std::atomic<bool> pump{true};
    std::mutex dispatch_lock;
    g_comp = &comp;
    g_lock = &dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int loop_fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    globals_case();
    dialog_cases();
    icon_cases();
    bell_cases();
    foreign_cases();
    inhibit_cases();
    data_control_cases();
    security_cases();

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
