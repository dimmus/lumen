// lumen-shell's side of zlm_policy_bridge_v1 against the compositor's, in one process.
//
// The compositor is configured with this test binary as its shell (`/proc/self/exe`), so
// the privilege check — the peer's /proc/<pid>/exe must be the configured shell — accepts
// the bridge connection. One connection plays the applications (xdg toplevels), another
// the shell (lx::shell::policy_bridge and the panel), and every check reads the shell's
// task model: the snapshot, the focus sent right after it, title/create/destroy deltas,
// and a request_activate that comes back as focus_changed. Version 2: toplevel_flags reach
// a shell bound at v2 (with the snapshot and as deltas) and never one bound at v1. Version 4:
// key grabs — a grabbed combination reaches the shell as `activated` (with a serial noted as
// the shell's key press) and no key event reaches the focused client, refusals for the
// compositor's own bindings, the tap grab, giving a key back, the version gate, and the
// router's forwarded-key rule (no release without a forwarded press; `enter` lists only
// forwarded keys).
#include <stdio.h>
#include <stdlib.h>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#if defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "ext-session-lock-v1-client-protocol.h"
#define LUMEN_TEST_SESSION_LOCK 1
#endif
#if defined(LUMEN_PROTOCOL_XDG_TOPLEVEL_ICON_V1) && defined(LUMEN_PROTOCOL_XDG_DIALOG_V1) && \
    defined(LUMEN_PROTOCOL_XDG_SYSTEM_BELL_V1)
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-system-bell-v1-client-protocol.h"
#define LUMEN_TEST_V6 1
#endif
#endif
#endif
#include <stdint.h>
#include <stddef.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.compositor;
import lx.input;
import lx.shell;
import lx.config;

// From wayland-server.h, which cannot share a translation unit with the client headers.
extern "C" struct wl_client* wl_resource_get_client(struct wl_resource* resource);

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
    bool want_seat = false;
#if defined(LUMEN_TEST_SESSION_LOCK)
    ext_session_lock_manager_v1* lock_manager = nullptr;
#endif
};

/// What the applications' keyboard was told.
struct keyboard_log {
    unsigned keys = 0;
    unsigned last_code = 0;
    unsigned last_state = 99;
    unsigned enters = 0;
    unsigned leaves = 0;
    unsigned enter_keys[8]{};
    unsigned enter_key_count = 0;
    unsigned mods_depressed = 0;
} g_kb;

void kb_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
    ++g_kb.enters;
    g_kb.enter_key_count = 0;
    const auto* k = static_cast<const uint32_t*>(keys->data);
    for (size_t i = 0; i < keys->size / sizeof(uint32_t) && i < 8; ++i)
        g_kb.enter_keys[g_kb.enter_key_count++] = k[i];
}
void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { ++g_kb.leaves; }
void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    ++g_kb.keys;
    g_kb.last_code = key;
    g_kb.last_state = state;
}
void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t, uint32_t, uint32_t) {
    g_kb.mods_depressed = depressed;
}
void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {kb_keymap, kb_enter,     kb_leave,
                                                kb_key,    kb_modifiers, kb_repeat};

unsigned g_activated_serial = 0;
unsigned g_activations = 0;
unsigned g_taps = 0;

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<conn*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (c->want_seat && std::strcmp(iface, wl_seat_interface.name) == 0)
        c->seat = static_cast<wl_seat*>(
            wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
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
#if defined(LUMEN_TEST_SESSION_LOCK)
    else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0)
        c->lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1));
#endif
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
    // zlm v7: a configuration file the compositor hands the shell (and a key it warns about).
    char cfg_dir[] = "/tmp/lumen-test-shell-bridge-XXXXXX";
    std::string cfg_path;
    if (mkdtemp(cfg_dir)) {
        cfg_path = std::string(cfg_dir) + "/config.toml";
        if (FILE* f = std::fopen(cfg_path.c_str(), "w")) {
            std::fputs("[cursor]\nsiez = 30\n[bindings]\n\"Super+Y\" = \"window.close\"\n", f);
            std::fclose(f);
        }
        setenv("LUMEN_CONFIG", cfg_path.c_str(), 1);
    }
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
        const int loop_fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            // Wait for client traffic without the lock: std::mutex is not fair, and a
            // dispatch sleeping in epoll while holding it, then re-locking at once, left the
            // test thread waiting seconds for it.
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
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

    // Virtual keys through the same path libinput's take (UI affinity, under the lock).
    const auto inject = [&](unsigned code, bool pressed) {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::input::key_event ev{};
        ev.evdev_keycode = code;
        ev.pressed = pressed;
        ev.time_ms = 1234;
        comp.inject_key(ev);
        comp.wayland().flush();
    };

    conn app{};
    app.want_seat = true;
    check(connect(app) && app.compositor && app.wm_base && app.shm && app.seat,
          "applications connect");
    window one{}, two{};
    map_window(app, one, "one");
    map_window(app, two, "two"); // mapped last: it has the keyboard

    static lx::shell::policy_bridge bridge;
    conn sh{};
    sh.bridge = &bridge;
    check(connect(sh) && sh.shell, "shell binds zlm_shell_v1 (exe matches the configured path)");
    check(bridge.has_capabilities() && bridge.compositor_version() == 7,
          "capabilities arrive right after bind (the compositor implements version 7)");
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
                                                            .layer_shell = sh.layer_shell},
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
    // Version gate: a shell bound below v4 has no grab_key.
    check(!again.can_grab_keys() &&
              !again.grab_key(0xff0d, lx::shell::policy_bridge::k_mod_super, 0, nullptr, nullptr),
          "a shell bound at v2 cannot grab keys (not_implemented)");
    again.detach();
    zlm_shell_v1_destroy(sh2.shell);
    wl_display_disconnect(sh2.display);

    // ── Version 4: key grabs ──
    static lx::shell::policy_bridge v4;
    conn sh3{};
    sh3.bridge = &v4;
    g_zlm_version = 4;
    check(connect(sh3) && sh3.shell && static_cast<bool>(v4.attach(sh3.shell)),
          "a v4 shell binds");
    (void)wl_display_roundtrip(sh3.display);
    check(v4.compositor_version() == 7 && v4.can_grab_keys() && !v4.has_lock_state(),
          "v4: grabs, no lock state (the compositor reports 7)");
    const auto v_one = find_title(v4.model(), "one renamed");
    const auto v_three = find_title(v4.model(), "three");
    v4.request_activate(v_one);
    (void)wl_display_roundtrip(sh3.display);
    wl_keyboard* kb = wl_seat_get_keyboard(app.seat);
    wl_keyboard_add_listener(kb, &keyboard_listener, nullptr);
    (void)wl_display_roundtrip(app.display);
    check(g_kb.enters == 1 && g_kb.enter_key_count == 0, "the application's keyboard is entered");

    using pb = lx::shell::policy_bridge;
    const auto on_activated = [](unsigned serial, unsigned, void*) {
        g_activated_serial = serial;
        ++g_activations;
    };
    const auto on_tap = [](unsigned, unsigned, void*) { ++g_taps; };
    auto ret = v4.grab_key(0xff0d /* Return */, pb::k_mod_super, 0, on_activated, nullptr);
    auto zap = v4.grab_key(0xff08 /* BackSpace */, pb::k_mod_ctrl | pb::k_mod_alt, 0, nullptr,
                           nullptr);
    auto vt = v4.grab_key(0x1008fe01 /* XF86Switch_VT_1 */, 0, 0, nullptr, nullptr);
    auto tap = v4.grab_key(0xffeb /* Super_L */, 0, pb::k_grab_tap, on_tap, nullptr);
    (void)wl_display_roundtrip(sh3.display);
    check(ret && tap && v4.key_grab_refusal(ret.value()) == 0 &&
              v4.key_grab_refusal(tap.value()) == 0,
          "grab_key: Super+Return and the Super tap are held");
    check(zap && v4.key_grab_refusal(zap.value()) == ZLM_KEY_GRAB_V1_REFUSAL_RESERVED,
          "grab_key: Ctrl+Alt+BackSpace refused (reserved)");
    check(vt && v4.key_grab_refusal(vt.value()) == ZLM_KEY_GRAB_V1_REFUSAL_RESERVED,
          "grab_key: XF86Switch_VT_1 refused (reserved)");

    constexpr unsigned k_meta = 125, k_enter = 28;
    const unsigned keys_before = g_kb.keys;
    inject(k_meta, true);
    inject(k_enter, true);
    (void)wl_display_roundtrip(sh3.display);
    (void)wl_display_roundtrip(app.display);
    wl_client* shell_client =
        wl_resource_get_client(static_cast<wl_resource*>(comp.shell_bridge().bridge_resource()));
    check(g_activations == 1 && g_activated_serial != 0, "Super+Return: activated reaches the shell");
    check(comp.input().serials().last_key_press(shell_client) == g_activated_serial,
          "the activated serial is the shell's last key press (activation tokens may quote it)");
    check(g_kb.keys == keys_before + 1 && g_kb.last_code == k_meta && g_kb.last_state == 1,
          "the focused client got Super's press but not Return's");
    inject(k_enter, false);
    (void)wl_display_roundtrip(app.display);
    check(g_kb.keys == keys_before + 1, "the grabbed key's release is swallowed too");
    // Focus moves away and back while Super is held: `enter` lists only forwarded keys.
    v4.request_activate(v_three);
    (void)wl_display_roundtrip(sh3.display);
    inject(k_enter, true); // grabbed again: consumed, never forwarded
    v4.request_activate(v_one);
    (void)wl_display_roundtrip(sh3.display);
    (void)wl_display_roundtrip(app.display);
    check(g_kb.enter_key_count == 1 && g_kb.enter_keys[0] == k_meta,
          "enter lists the held Super and not the consumed Return");
    check(g_activations == 2, "a second Super+Return activates again");
    inject(k_enter, false);
    inject(k_meta, false);
    (void)wl_display_roundtrip(app.display);
    (void)wl_display_roundtrip(sh3.display);
    check(g_kb.last_code == k_meta && g_kb.last_state == 0 && g_taps == 0,
          "Super's release reaches the client; Super+Return was not a tap");

    // The tap: Super alone fires on release, and the client sees both halves.
    const unsigned keys_tap = g_kb.keys;
    inject(k_meta, true);
    inject(k_meta, false);
    (void)wl_display_roundtrip(sh3.display);
    (void)wl_display_roundtrip(app.display);
    check(g_taps == 1 && g_kb.keys == keys_tap + 2, "Super tap: fired on release, key still forwarded");

    // Giving Return back.
    v4.ungrab_key(ret.value());
    (void)wl_display_roundtrip(sh3.display);
    const unsigned keys_free = g_kb.keys;
    inject(k_meta, true);
    inject(k_enter, true);
    inject(k_enter, false);
    inject(k_meta, false);
    (void)wl_display_roundtrip(app.display);
    (void)wl_display_roundtrip(sh3.display);
    check(g_kb.keys == keys_free + 4 && g_activations == 2,
          "destroying the grab gives Super+Return back to the client");
    // The compositor's own chord is not the shell's: Ctrl+Alt+BackSpace would log out here,
    // which this test does not want; its refusal above is the check.

#if defined(LUMEN_TEST_SESSION_LOCK)
    // ── Version 5: the session lock ──
    {
        static unsigned states[16];
        static unsigned state_count = 0;
        static lx::shell::policy_bridge v5;
        v5.set_lock_handlers(
            [](unsigned state, void*) {
                if (state_count < 16)
                    states[state_count++] = state;
            },
            nullptr, nullptr);
        conn sh5{};
        sh5.bridge = &v5;
        g_zlm_version = 5;
        check(connect(sh5) && sh5.shell && v5.has_lock_state() && state_count == 1 &&
                  states[0] == 0,
              "v5: lock_state unlocked arrives with the bind");
        conn locker{};
        check(connect(locker) && locker.lock_manager, "a locker binds ext_session_lock_manager_v1");
        const unsigned taps_before = g_taps;
        const unsigned enters_before = g_kb.enters;
        const unsigned leaves_before = g_kb.leaves;
        auto* lock = ext_session_lock_manager_v1_lock(locker.lock_manager);
        (void)wl_display_roundtrip(locker.display);
        (void)wl_display_roundtrip(sh5.display);
        (void)wl_display_roundtrip(app.display);
        check(state_count == 2 && states[1] == 1 && v5.lock_state() == 1,
              "v5: lock_state locking at the lock request");
        check(g_kb.leaves == leaves_before + 1,
              "locking: the application loses the keyboard at the request");
        const unsigned keys_locked = g_kb.keys;
        inject(k_meta, true);
        inject(k_meta, false);
        (void)wl_display_roundtrip(app.display);
        (void)wl_display_roundtrip(sh5.display);
        (void)wl_display_roundtrip(sh3.display);
        check(g_taps == taps_before && g_kb.keys == keys_locked,
              "locking: no key reaches the application and the Super tap does not fire");
        // The locker gives up before `locked` (destroy is allowed then): unlocked again.
        ext_session_lock_v1_destroy(lock);
        (void)wl_display_roundtrip(locker.display);
        (void)wl_display_roundtrip(sh5.display);
        (void)wl_display_roundtrip(app.display);
        check(state_count == 3 && states[2] == 0 && wl_display_get_error(locker.display) == 0,
              "v5: lock_state unlocked when the locker gives up before locking");
        check(g_kb.enters == enters_before + 1, "unlocked: the application has the keyboard again");
        wl_display_disconnect(locker.display);
        v5.detach();
        zlm_shell_v1_destroy(sh5.shell);
        wl_display_disconnect(sh5.display);
    }
#endif

#if defined(LUMEN_TEST_V6)
    // ── Version 6: icons (memfds), parents and modal dialogs, the bell, non-inhibitable
    //    grabs ──
    {
        const auto bind = [](conn& c, const wl_interface* iface) -> void* {
            struct found {
                const wl_interface* iface;
                uint32_t name = 0;
            } f{iface};
            static const wl_registry_listener l = {
                [](void* d, wl_registry*, uint32_t name, const char* i, uint32_t) {
                    auto* fd = static_cast<found*>(d);
                    if (std::strcmp(i, fd->iface->name) == 0)
                        fd->name = name;
                },
                [](void*, wl_registry*, uint32_t) {}};
            wl_registry* r = wl_display_get_registry(c.display);
            wl_registry_add_listener(r, &l, &f);
            (void)wl_display_roundtrip(c.display);
            return f.name ? wl_registry_bind(r, f.name, iface, 1) : nullptr;
        };
        static lx::shell::policy_bridge v6;
        static lx::shell::task_handle bells[8];
        static unsigned bell_count = 0;
        v6.set_bell_handler(
            [](lx::shell::task_handle h, void*) {
                if (bell_count < 8)
                    bells[bell_count++] = h;
            },
            nullptr);
        conn sh6{};
        sh6.bridge = &v6;
        g_zlm_version = 6;
        check(connect(sh6) && sh6.shell && v6.has_v6() && static_cast<bool>(v6.attach(sh6.shell)),
              "v6: the shell binds version 6");
        (void)wl_display_roundtrip(sh6.display);
        // The lock key, not inhibitable: accepted.
        auto lock_grab = v6.grab_key(0x6c, lx::shell::policy_bridge::k_mod_super,
                                     lx::shell::policy_bridge::k_grab_not_inhibitable, nullptr,
                                     nullptr);
        (void)wl_display_roundtrip(sh6.display);
        check(lock_grab && v6.key_grab_refusal(lock_grab.value()) == 0,
              "v6: a not_inhibitable grab is accepted");
        auto* icons = static_cast<xdg_toplevel_icon_manager_v1*>(
            bind(app, &xdg_toplevel_icon_manager_v1_interface));
        auto* dialogs = static_cast<xdg_wm_dialog_v1*>(bind(app, &xdg_wm_dialog_v1_interface));
        auto* bell = static_cast<xdg_system_bell_v1*>(bind(app, &xdg_system_bell_v1_interface));
        window main_w{}, dlg{};
        map_window(app, main_w, "with-icon");
        // A 16 px icon with a name; applied by the next commit.
        auto* icon = xdg_toplevel_icon_manager_v1_create_icon(icons);
        xdg_toplevel_icon_v1_set_name(icon, "utilities-terminal");
        {
            const int fd = memfd_create("icon", MFD_CLOEXEC);
            (void)!ftruncate(fd, 16 * 16 * 4);
            auto* px = static_cast<uint32_t*>(mmap(nullptr, 16 * 16 * 4, PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, fd, 0));
            for (int i = 0; i < 256; ++i)
                px[i] = 0xFF00FF00u;
            munmap(px, 16 * 16 * 4);
            wl_shm_pool* pool = wl_shm_create_pool(app.shm, fd, 16 * 16 * 4);
            wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, 16, 16, 64, WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
            close(fd);
            xdg_toplevel_icon_v1_add_buffer(icon, b, 1);
        }
        xdg_toplevel_icon_manager_v1_set_icon(icons, main_w.toplevel, icon);
        xdg_toplevel_icon_v1_destroy(icon);
        wl_surface_commit(main_w.surface);
        (void)wl_display_roundtrip(app.display);
        // A modal dialog of it.
        dlg.surface = wl_compositor_create_surface(app.compositor);
        dlg.xdg = xdg_wm_base_get_xdg_surface(app.wm_base, dlg.surface);
        xdg_surface_add_listener(dlg.xdg, &xdg_listener, &dlg);
        dlg.toplevel = xdg_surface_get_toplevel(dlg.xdg);
        xdg_toplevel_set_title(dlg.toplevel, "the-dialog");
        xdg_toplevel_set_parent(dlg.toplevel, main_w.toplevel);
        xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(dialogs, dlg.toplevel));
        wl_surface_commit(dlg.surface);
        (void)wl_display_roundtrip(app.display);
        xdg_surface_ack_configure(dlg.xdg, dlg.serial);
        wl_surface_attach(dlg.surface, make_buffer(app, 120, 80), 0, 0);
        wl_surface_commit(dlg.surface);
        (void)wl_display_roundtrip(app.display);
        (void)wl_display_roundtrip(sh6.display);
        const auto& m6 = v6.model();
        const auto h_main = find_title(m6, "with-icon");
        const auto h_dlg = find_title(m6, "the-dialog");
        lx::shell::policy_bridge::icon_image images[4];
        const unsigned n_images = v6.icon_images(h_main, images, 4);
        const auto* em = m6.get(h_main);
        check(em && std::strcmp(em->icon_name, "utilities-terminal") == 0 && n_images == 1 &&
                  images[0].size == 16 && images[0].pixels && images[0].pixels[0] == 0xFF00FF00u,
              "v6: the window's icon reaches the shell (name, and the pixels through a memfd)");
        const auto* ed = m6.get(h_dlg);
        check(ed && ed->parent == h_main && ed->state == lx::shell::k_task_state_modal &&
                  lx::shell::task_is_modal_child(m6, *ed),
              "v6: the dialog's parent and modal state reach the shell (the task bar hides it)");
        // The bell, for a window and for none.
        xdg_system_bell_v1_ring(bell, main_w.surface);
        xdg_system_bell_v1_ring(bell, nullptr);
        (void)wl_display_roundtrip(app.display);
        (void)wl_display_roundtrip(sh6.display);
        check(bell_count == 2 && bells[0] == h_main && !bells[1],
              "v6: bells reach the shell with their window (and without one)");
        // A snapshot carries icons and parents too.
        static lx::shell::policy_bridge again;
        conn sh6b{};
        sh6b.bridge = &again;
        check(connect(sh6b) && sh6b.shell && static_cast<bool>(again.attach(sh6b.shell)),
              "v6: a second shell binds");
        (void)wl_display_roundtrip(sh6b.display);
        const auto& m6b = again.model();
        const auto b_main = find_title(m6b, "with-icon");
        const auto b_dlg = find_title(m6b, "the-dialog");
        lx::shell::policy_bridge::icon_image images_b[4];
        check(m6b.get(b_dlg) && m6b.get(b_dlg)->parent == b_main &&
                  again.icon_images(b_main, images_b, 4) == 1 &&
                  std::strcmp(m6b.get(b_main)->icon_name, "utilities-terminal") == 0,
              "v6: the snapshot carries icons and parents");
        again.detach();
        zlm_shell_v1_destroy(sh6b.shell);
        wl_display_disconnect(sh6b.display);
        v6.detach();
        zlm_shell_v1_destroy(sh6.shell);
        wl_display_disconnect(sh6.display);
    }
#endif

    // ── Version 7: the configuration and actions ──
    {
        struct v7_log {
            std::string bytes;
            unsigned generation = 0;
            unsigned done_generation = 0;
            bool applied = false;
            unsigned errors = 99, warnings = 99;
            std::string first_line;
            std::string all_lines;
            std::string action;
            unsigned action_serial = 0;
        };
        static v7_log log7;
        static lx::shell::policy_bridge v7;
        v7.set_config_handlers(
            [](std::string_view bytes, unsigned generation, void*) {
                log7.bytes.assign(bytes);
                log7.generation = generation;
            },
            [](unsigned generation, bool applied, unsigned errors, unsigned warnings,
               std::span<const lx::shell::policy_bridge::config_line> lines, void*) {
                log7.done_generation = generation;
                log7.applied = applied;
                log7.errors = errors;
                log7.warnings = warnings;
                log7.all_lines.clear();
                for (const auto& l : lines)
                    log7.all_lines += l.text + "\n";
                if (!lines.empty())
                    log7.first_line = lines.front().file + ":" + std::to_string(lines.front().line) +
                                      " " + lines.front().text;
            },
            [](const char* action, const char*, unsigned serial, unsigned, void*) {
                log7.action = action;
                log7.action_serial = serial;
            },
            nullptr);
        conn sh7{};
        sh7.bridge = &v7;
        g_zlm_version = 7;
        check(connect(sh7) && sh7.shell && v7.has_v7(), "v7: the shell binds version 7");
        check(log7.generation == 1 && log7.bytes.find("# lumen-layer: " + cfg_path) == 0 &&
                  log7.bytes.find("\"Super+Y\" = \"window.close\"") != std::string::npos,
              "v7: config on bind: the file's bytes behind their layer marker");
        check(log7.done_generation == 1 && log7.applied && log7.errors == 0 &&
                  log7.warnings == 1 &&
                  log7.first_line == cfg_path + ":2 cursor.siez: unknown key (did you mean size?); "
                                                "ignored",
              "v7: config_diagnostic and config_done report the compositor's diagnostics");
        if (log7.warnings == 1 && log7.first_line.find("did you mean") == std::string::npos)
            std::printf("      got: %s\n", log7.first_line.c_str());
        // The parsed bytes give the shell the same configuration.
        lx::config::config_snapshot parsed = lx::config::default_config();
        lx::config::diagnostics d{};
        check(lx::config::parse_config(lx::config::split_layers(log7.bytes), parsed, d, nullptr) &&
                  d.warnings() == 1 && parsed.bindings.size() == lx::config::default_config().bindings.size() + 1,
              "v7: the shell parses the bytes to the compositor's configuration");
        check(static_cast<bool>(v7.attach(sh7.shell)), "v7: get_policy_bridge");
        (void)wl_display_roundtrip(sh7.display);
        // Super+Return: the compositor's binding sends shell.terminal to the v7 shell (not the
        // v4 grab, not the application), with a serial noted as the shell's key press.
        constexpr unsigned k_meta7 = 125, k_enter7 = 28;
        const unsigned activations_before = g_activations;
        const unsigned keys_before7 = g_kb.keys;
        inject(k_meta7, true);
        inject(k_enter7, true);
        inject(k_enter7, false);
        inject(k_meta7, false);
        (void)wl_display_roundtrip(sh7.display);
        (void)wl_display_roundtrip(app.display);
        wl_client* shell7 = wl_resource_get_client(
            static_cast<wl_resource*>(comp.shell_bridge().bridge_resource()));
        check(log7.action == "shell.terminal" && log7.action_serial != 0 &&
                  comp.input().serials().last_key_press(shell7) == log7.action_serial,
              "v7: Super+Return is the action shell.terminal, its serial the shell's key press");
        check(g_activations == activations_before, "v7: the legacy grab is not activated");
        check(g_kb.keys == keys_before7 + 2,
              "v7: the application saw Super's press and release only");
        // A Super tap is shell.launcher.
        log7.action.clear();
        inject(k_meta7, true);
        inject(k_meta7, false);
        (void)wl_display_roundtrip(sh7.display);
        check(log7.action == "shell.launcher", "v7: a Super tap is the action shell.launcher");

        // ── v7: held actions (design-session-and-wm K.1): Alt+Tab holds the keyboard ──
        {
            struct hold_log {
                std::string begin;
                std::vector<std::string> keys;
                unsigned released = 0;
                unsigned cancelled = 0;
            };
            static hold_log hl;
            v7.set_hold_handlers(
                {.begin = [](const char* a, const char*, unsigned, unsigned, void*) { hl.begin = a; },
                 .key = [](unsigned keysym, unsigned mods, const char* binding, unsigned, unsigned,
                           void*) {
                     char b[96];
                     std::snprintf(b, sizeof(b), "%x/%x/%s", keysym, mods, binding);
                     hl.keys.emplace_back(b);
                 },
                 .released = [](unsigned, unsigned, void*) { ++hl.released; },
                 .cancelled = [](void*) { ++hl.cancelled; }});
            constexpr unsigned k_alt = 56, k_tab = 15, k_shift = 42, k_esc = 1, k_a = 30;
            (void)wl_display_roundtrip(app.display); // what came before is counted first
            const unsigned keys0 = g_kb.keys;
            const unsigned leaves0 = g_kb.leaves;
            inject(k_alt, true);
            inject(k_tab, true);
            inject(k_tab, false);
            (void)wl_display_roundtrip(sh7.display);
            check(hl.begin == "switcher.next" && v7.holding(),
                  "v7: Alt+Tab begins the held action switcher.next");
            inject(k_tab, true);
            inject(k_tab, false);
            inject(k_shift, true);
            inject(k_tab, true);
            inject(k_tab, false);
            inject(k_shift, false);
            (void)wl_display_roundtrip(sh7.display);
            check(hl.keys.size() == 2 && hl.keys[0] == "ff09/4/switcher.next" &&
                      hl.keys[1] == "ff09/5/switcher.previous",
                  "v7: Tab and Shift+Tab during the hold reach the shell as keys, with the action "
                  "each is bound to");
            inject(k_alt, false);
            (void)wl_display_roundtrip(sh7.display);
            (void)wl_display_roundtrip(app.display);
            check(hl.released == 1 && !v7.holding(), "v7: letting go of Alt releases the hold");
            check(g_kb.keys == keys0 + 4 && g_kb.leaves == leaves0,
                  "v7: the application saw only Alt's and Shift's presses and releases, and kept "
                  "the keyboard");
            // The shell ends the hold itself (Escape dismissed the switcher): the keys reach the
            // application again at once, though Alt is still down.
            inject(k_alt, true);
            inject(k_tab, true);
            inject(k_tab, false);
            inject(k_esc, true);
            inject(k_esc, false);
            (void)wl_display_roundtrip(sh7.display);
            check(v7.holding() && hl.keys.size() == 3 && hl.keys[2] == "ff1b/4/",
                  "v7: Escape is the hold's too");
            v7.end_hold();
            (void)wl_display_roundtrip(sh7.display);
            (void)wl_display_roundtrip(app.display); // Alt's press, counted before
            const unsigned keys1 = g_kb.keys;
            inject(k_a, true);
            inject(k_a, false);
            inject(k_alt, false);
            (void)wl_display_roundtrip(app.display);
            (void)wl_display_roundtrip(sh7.display);
            check(g_kb.keys == keys1 + 3 && hl.released == 1 && hl.cancelled == 0,
                  "v7: after the shell ended the hold Alt+a and Alt's release reach the "
                  "application, and nothing is released");
            v7.set_hold_handlers({});
        }

        // ── v7: workspace membership (toplevel_workspace), sticky, minimized ──
        {
            const auto& m7 = v7.model();
            const auto t_one = find_title(m7, "three");
            check(t_one && m7.get(t_one)->workspace_known && !m7.get(t_one)->workspace,
                  "v7: toplevel_workspace follows the snapshot: null without an ext-workspace "
                  "manager");
            // The shell binds ext-workspace after its bridge: once that manager's first done
            // is out, every window's workspace comes again — now as the shell's handle.
            static lx::shell::workspace_client ws7;
            ws7.set_removed_handler(
                [](void* proxy, void*) { v7.model().forget_workspace(proxy); }, nullptr);
            struct found_global {
                uint32_t name = 0;
                uint32_t version = 0;
            } fg{};
            static const wl_registry_listener find_ws = {
                [](void* d, wl_registry*, uint32_t name, const char* iface, uint32_t version) {
                    if (std::strcmp(iface, "ext_workspace_manager_v1") == 0)
                        *static_cast<found_global*>(d) = {name, version};
                },
                [](void*, wl_registry*, uint32_t) {}};
            wl_registry* reg7 = wl_display_get_registry(sh7.display);
            wl_registry_add_listener(reg7, &find_ws, &fg);
            (void)wl_display_roundtrip(sh7.display);
            check(fg.name != 0, "v7: ext_workspace_manager_v1 is advertised");
            ws7.bind(reg7, fg.name, fg.version);
            (void)wl_display_roundtrip(sh7.display);
            (void)wl_display_roundtrip(sh7.display);
            const auto* e1 = m7.get(t_one);
            check(ws7.synced() && e1 && e1->workspace && ws7.view().is_active(e1->workspace),
                  "v7: after a late ext-workspace bind each window names its active workspace");
            const auto with_wm = [&](auto&& fn) {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
                lx::toplevel_id id{};
                comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
                    if (r.title && std::strcmp(r.title, "three") == 0)
                        id = r.id;
                });
                fn(comp.windows(), id);
                comp.wayland().flush();
            };
            with_wm([](auto& wm, lx::toplevel_id id) { wm.set_sticky(id, true); });
            (void)wl_display_roundtrip(sh7.display);
            e1 = m7.get(t_one);
            check(e1 && (e1->flags & lx::shell::task_flag_sticky) && !e1->workspace &&
                      e1->workspace_known,
                  "v7: sticky: the flag, and no workspace");
            with_wm([](auto& wm, lx::toplevel_id id) { wm.set_sticky(id, false); });
            (void)wl_display_roundtrip(sh7.display);
            e1 = m7.get(t_one);
            check(e1 && !(e1->flags & lx::shell::task_flag_sticky) && e1->workspace &&
                      ws7.view().is_active(e1->workspace),
                  "v7: not sticky: back on the active workspace");
            with_wm([](auto& wm, lx::toplevel_id id) { wm.set_minimized(id, true, "test"); });
            (void)wl_display_roundtrip(sh7.display);
            e1 = m7.get(t_one);
            check(e1 && e1->state == lx::shell::k_task_state_minimized,
                  "v7: a minimized window's state is minimized");
            with_wm([](auto& wm, lx::toplevel_id id) { wm.activate(id); });
            (void)wl_display_roundtrip(sh7.display);
            e1 = m7.get(t_one);
            check(e1 && e1->state != lx::shell::k_task_state_minimized,
                  "v7: activating it brings it back");
            ws7.destroy();
            wl_registry_destroy(reg7);
        }

        // ── v7: reloads (SIGHUP's path): a keymap that does not compile stays reported in
        // every later generation, and a reload asked for while a generation is busy (its
        // keymap compiling on the worker) is answered by its own generation ──
        if (!cfg_path.empty()) {
            // What systemd would see of the reload protocol (READY=1 per SIGHUP).
            const std::string notify_path = std::string(cfg_dir) + "/notify";
            const int ns = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            sockaddr_un na{};
            na.sun_family = AF_UNIX;
            std::snprintf(na.sun_path, sizeof(na.sun_path), "%s", notify_path.c_str());
            check(ns >= 0 && ::bind(ns, reinterpret_cast<sockaddr*>(&na), sizeof(na)) == 0,
                  "reload: a notify socket");
            setenv("NOTIFY_SOCKET", notify_path.c_str(), 1);
            std::vector<std::string> readies;
            const auto read_notify = [&] {
                char buf[512];
                ssize_t n = 0;
                while ((n = ::recv(ns, buf, sizeof(buf) - 1, 0)) > 0) {
                    buf[n] = '\0';
                    if (std::strstr(buf, "READY=1"))
                        readies.emplace_back(buf);
                }
            };
            const auto write_config = [&](const char* text) {
                if (FILE* f = std::fopen(cfg_path.c_str(), "w")) {
                    std::fputs(text, f);
                    std::fclose(f);
                }
            };
            const auto ui_tasks = [&] {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
                comp.run_posted_ui_tasks();
                comp.wayland().flush();
            };
            const auto wait_generation = [&](unsigned gen) {
                for (int i = 0; i < 400 && log7.done_generation < gen; ++i) {
                    ui_tasks();
                    (void)wl_display_roundtrip(sh7.display);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return log7.done_generation == gen;
            };
            constexpr const char* k_base =
                "[cursor]\nsiez = 30\n[bindings]\n\"Super+Y\" = \"window.close\"\n";
            const std::string bad = std::string(k_base) +
                                    "[input.keyboard]\nlayout = \"lumen-no-such-layout\"\n";
            const unsigned g = log7.done_generation;
            write_config(bad.c_str());
            comp.request_reload();
            ui_tasks(); // generation g+1 starts; its keymap compiles on the worker
            // Saved again while that compiles, and reloaded: queued behind it.
            const std::string bad2 = bad + "[focus]\nmood = 1\n";
            write_config(bad2.c_str());
            comp.request_reload();
            ui_tasks();
            check(wait_generation(g + 2), "reload: the queued reload runs after the busy one");
            read_notify();
            const std::string want1 = "generation " + std::to_string(g + 1);
            const std::string want2 = "generation " + std::to_string(g + 2);
            check(readies.size() == 2 && readies[0].find(want1) != std::string::npos &&
                      readies[1].find(want2) != std::string::npos,
                  "reload: each SIGHUP gets READY=1 from its own generation (the queued one's "
                  "after its files were read)");
            if (readies.size() != 2)
                for (const auto& r : readies)
                    std::printf("      got: %s\n", r.c_str());
            check(log7.applied && log7.errors == 1 && log7.warnings == 2 &&
                      log7.all_lines.find("does not compile") != std::string::npos,
                  "reload: the keymap error is still reported by a generation that did not "
                  "change the layout");
            comp.request_reload();
            check(wait_generation(g + 3) && log7.errors == 1,
                  "reload: a forced reload without changes still reports it");
            write_config(k_base);
            comp.request_reload();
            check(wait_generation(g + 4) && log7.applied && log7.errors == 0 &&
                      log7.warnings == 1,
                  "reload: the layout removed, the error is gone");
            unsetenv("NOTIFY_SOCKET");
            ::close(ns);
            std::remove(notify_path.c_str());
        }
        v7.detach();
        zlm_shell_v1_destroy(sh7.shell);
        wl_display_disconnect(sh7.display);
    }
    if (!cfg_path.empty()) {
        std::remove(cfg_path.c_str());
        ::rmdir(cfg_dir);
    }

    v4.detach();
    zlm_shell_v1_destroy(sh3.shell);
    wl_display_disconnect(sh3.display);
    wl_keyboard_destroy(kb);
    wl_display_disconnect(app.display);
    wl_display_disconnect(app2.display);
    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d checks, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
