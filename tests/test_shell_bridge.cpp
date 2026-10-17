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
#if __has_include("ext-session-lock-v1-client-protocol.h")
#include "ext-session-lock-v1-client-protocol.h"
#define LUMEN_TEST_SESSION_LOCK 1
#endif
#endif
#endif

import lx.foundation;
import lx.runtime;
import lx.compositor;
import lx.input;
import lx.shell;

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
    check(bridge.has_capabilities() && bridge.compositor_version() == 5,
          "capabilities arrive right after bind (the compositor implements version 5)");
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
    check(v4.compositor_version() == 5 && v4.can_grab_keys() && !v4.has_lock_state(),
          "v4: grabs, no lock state (the compositor reports 5)");
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
