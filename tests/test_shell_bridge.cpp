// lumen-shell's side of zlm_policy_bridge_v1 against the compositor's, in one process.
//
// The compositor is configured with this test binary as its shell (`/proc/self/exe`), so
// the privilege check — the peer's /proc/<pid>/exe must be the configured shell — accepts
// the bridge connection. One connection plays the applications (xdg toplevels), another
// the shell (lx::shell::policy_bridge and the panel), and every check reads the shell's
// task model: the snapshot, the focus sent right after it, title/create/destroy deltas,
// and a request_activate that comes back as focus_changed. Version 2: toplevel_flags reach
// a shell bound at v2 (with the snapshot and as deltas) and never one bound at v1.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "zlm_shell_v1-client-protocol.h"
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
import lx.shell;

#if !defined(LUMEN_HAS_WAYLAND) || !defined(LUMEN_HAS_PROTOCOL_GLUE)

int main() {
    std::printf("SKIP: needs libwayland and the generated protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-shell-bridge";
/// zlm_shell_v1 version the next shell connection binds.
uint32_t g_zlm_version = 1;
int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

struct conn {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm_base = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zlm_shell_v1* shell = nullptr;
    lx::shell::policy_bridge* bridge = nullptr; // observes zlm_shell_v1 at bind
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<conn*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        c->layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 4));
    else if (c->bridge && std::strcmp(iface, zlm_shell_v1_interface.name) == 0) {
        c->shell = static_cast<zlm_shell_v1*>(wl_registry_bind(
            r, name, &zlm_shell_v1_interface, g_zlm_version < version ? g_zlm_version : version));
        (void)c->bridge->observe(c->shell);
    }
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool connect(conn& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    wl_registry* r = wl_display_get_registry(c.display);
    wl_registry_add_listener(r, &registry_listener, &c);
    return wl_display_roundtrip(c.display) >= 0 && wl_display_roundtrip(c.display) >= 0;
}

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
};
void on_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<window*>(data)->serial = serial;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

wl_buffer* make_buffer(conn& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-shell-bridge", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void map_window(conn& c, window& w, const char* title) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_set_app_id(w.toplevel, "test-app");
    xdg_toplevel_set_title(w.toplevel, title);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
}

void unmap_window(conn& c, window& w) {
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    (void)wl_display_roundtrip(c.display);
}

/// The shell's entry for `title`, or a null handle.
lx::shell::task_handle find_title(const lx::shell::task_model& m, const char* title) {
    lx::shell::task_handle order[lx::shell::k_max_tasks];
    const unsigned n = m.ordered(order, lx::shell::k_max_tasks);
    for (unsigned i = 0; i < n; ++i)
        if (const auto* e = m.get(order[i]); e && std::strcmp(e->title, title) == 0)
            return order[i];
    return {};
}

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.shell_binary_path = "/proc/self/exe"; // resolved: this test binary
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    std::atomic<bool> pump{true};
    // Held around each dispatch, so the test can change compositor state between them.
    std::mutex dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        while (pump.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            (void)comp.wayland().dispatch(10);
            comp.wayland().flush();
        }
    });
    // The liveness tracker's verdict on the client owning the window titled `title`, as it
    // reaches the window manager (UI affinity: under the dispatch lock).
    const auto set_not_responding = [&](const char* title, bool on) {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::client_id client{};
        comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
            if (r.title && std::strcmp(r.title, title) == 0)
                client = r.client;
        });
        const unsigned n = comp.windows().set_client_not_responding(client, on);
        comp.wayland().flush();
        return n;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    conn app{};
    check(connect(app) && app.compositor && app.wm_base && app.shm, "applications connect");
    window one{}, two{};
    map_window(app, one, "one");
    map_window(app, two, "two"); // mapped last: it has the keyboard

    static lx::shell::policy_bridge bridge;
    conn sh{};
    sh.bridge = &bridge;
    check(connect(sh) && sh.shell, "shell binds zlm_shell_v1 (exe matches the configured path)");
    check(bridge.has_capabilities() && bridge.compositor_version() == 2,
          "capabilities arrive right after bind (the compositor implements version 2)");
    check(static_cast<bool>(bridge.attach(sh.shell)), "get_policy_bridge");
    (void)wl_display_roundtrip(sh.display);
    const auto& model = bridge.model();
    check(model.synced() && !model.in_snapshot() && model.count() == 2,
          "snapshot: two toplevels, complete");
    const auto h_one = find_title(model, "one");
    const auto h_two = find_title(model, "two");
    check(h_one && h_two && std::strcmp(model.get(h_one)->app_id, "test-app") == 0,
          "snapshot carries app_id and title");
    check(model.focused() == h_two, "focus_changed after snapshot_done names the focused one");
    check(model.get(h_one)->geometry.width == 200 && model.get(h_one)->geometry.height == 150,
          "snapshot carries the window geometry");

    // The panel draws the snapshot on its layer surface.
    static lx::shell::panel panel;
    check(sh.layer_shell && static_cast<bool>(panel.create({.compositor = sh.compositor,
                                                            .shm = sh.shm,
                                                            .layer_shell = sh.layer_shell,
                                                            .seat = nullptr,
                                                            .cursor_shape_manager = nullptr},
                                                           bridge)),
          "panel layer surface created");
    (void)wl_display_roundtrip(sh.display); // configure
    panel.after_dispatch();                 // ack + first commit
    (void)wl_display_roundtrip(sh.display);
    const auto& drawn = panel.committed();
    check(drawn.width > 0 && drawn.height == lx::shell::k_panel_height && drawn.count == 2,
          "panel committed a full-width 32 px row of two entries");
    check(drawn.count == 2 && (drawn.cells[1].flags & lx::shell::cell_focused) &&
              !(drawn.cells[0].flags & lx::shell::cell_focused),
          "the focused entry is highlighted");

    // Deltas.
    xdg_toplevel_set_title(one.toplevel, "one renamed");
    (void)wl_display_roundtrip(app.display);
    (void)wl_display_roundtrip(sh.display);
    check(std::strcmp(model.get(h_one)->title, "one renamed") == 0, "title delta");

    bridge.request_activate(h_one);
    (void)wl_display_roundtrip(sh.display);
    check(model.focused() == h_one, "request_activate comes back as focus_changed");

    // Another client's window (flags are per client: liveness is a client's).
    conn app2{};
    check(connect(app2) && app2.compositor, "a second application connects");
    window three{};
    map_window(app2, three, "three");
    (void)wl_display_roundtrip(sh.display);
    const auto h_three = find_title(model, "three");
    check(model.count() == 3 && h_three && model.focused() == h_three,
          "toplevel_created + state + focus for a new window");

    unmap_window(app, two);
    (void)wl_display_roundtrip(sh.display);
    check(model.count() == 2 && !model.get(h_two), "toplevel_destroyed removes the entry");

    // Requests on a handle the shell already dropped send nothing (no protocol error).
    bridge.request_raise(h_two);
    check(wl_display_roundtrip(sh.display) >= 0, "a request on a stale handle is not sent");

    // A shell bound at version 1 never sees toplevel_flags.
    check(set_not_responding("one renamed", true) == 1, "one toplevel marked not responding");
    (void)wl_display_roundtrip(sh.display);
    check(wl_display_get_error(sh.display) == 0 && model.get(h_one)->flags == 0,
          "toplevel_flags is not sent to a v1 bridge");

    // Raise the older window, so stacking order (three, one) differs from creation order.
    bridge.request_activate(h_one);
    (void)wl_display_roundtrip(sh.display);

    // A shell restart: the old connection goes, a new one gets a fresh snapshot.
    panel.destroy();
    bridge.detach();
    zlm_shell_v1_destroy(sh.shell);
    wl_display_disconnect(sh.display);
    static lx::shell::policy_bridge again;
    conn sh2{};
    sh2.bridge = &again;
    g_zlm_version = 2;
    check(connect(sh2) && sh2.shell && static_cast<bool>(again.attach(sh2.shell)),
          "a restarted shell binds again");
    (void)wl_display_roundtrip(sh2.display);
    const auto r_one = find_title(again.model(), "one renamed");
    const auto r_three = find_title(again.model(), "three");
    check(again.model().synced() && again.model().count() == 2 && r_one && r_three &&
              again.model().focused() == r_one,
          "re-snapshot: current windows and focus");
    {
        lx::shell::task_handle order[2];
        const unsigned n = again.model().ordered(order, 2);
        check(n == 2 && order[0] == r_one && order[1] == r_three &&
                  again.model().get(r_one)->stacking == 1 &&
                  again.model().get(r_three)->stacking == 0,
              "re-snapshot lists creation order, each with its stacking index");
    }
    check(again.model().get(r_one)->flags == lx::shell::task_flag_not_responding &&
              again.model().get(r_three)->flags == 0,
          "v2 snapshot: toplevel_flags follows snapshot_toplevel for the flagged window only");
    check(set_not_responding("one renamed", false) == 1, "the client answers again");
    (void)wl_display_roundtrip(sh2.display);
    check(again.model().get(r_one)->flags == 0, "toplevel_flags delta: not responding cleared");
    (void)set_not_responding("three", true);
    (void)wl_display_roundtrip(sh2.display);
    check(again.model().get(r_three)->flags == lx::shell::task_flag_not_responding &&
              again.model().get(r_one)->flags == 0,
          "toplevel_flags delta: set on the right window");
    again.detach();
    zlm_shell_v1_destroy(sh2.shell);
    wl_display_disconnect(sh2.display);
    wl_display_disconnect(app.display);
    wl_display_disconnect(app2.display);
    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d checks, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
