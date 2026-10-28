// Master-stack tiling (ADR 012), in-process against a real compositor with one virtual
// 1280x720 output and no shell (the usable area is the whole output, gaps 8): a workspace
// toggled to master-stack tiles its window, a new window's initial configure already carries
// its tile (T4) and the others reflow, promote / swap / master count / ratio reorder and
// resize the tiles, a floated window leaves the layout and comes back last, a dialog and a
// fixed-size window float, a minimized window gives up its place, a maximized one keeps it
// and returns to its tile, a dragged tile dropped on another swaps with it, an unmapped one
// leaves a gap that closes, and toggling back restores the floating geometry. Clients here
// answer every configure with a buffer of the configured size, as a tiling-aware client does.
#include <errno.h>
#include <stdio.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#define LUMEN_TEST_TILING 1
#endif
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;

#if !defined(LUMEN_TEST_TILING)

int main() {
    std::printf("SKIP: needs libwayland and the xdg-shell glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-tiling";

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 1'000'000;

void check(const char* name, bool ok, const std::string& detail = {}) {
    ++g_ran;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ",
                detail.c_str());
    if (!ok)
        ++g_failed;
}

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

// ── an application window that draws at the size it is configured for ──

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
    uint32_t acked = 0;
    // The newest xdg_toplevel.configure.
    int width = 0;
    int height = 0;
    bool tiled = false; // all four tiled_* states
    bool maximized = false;
    // The very first configure's size.
    int first_width = -1;
    int first_height = -1;
    int drawn_width = 200;
    int drawn_height = 150;
};

const xdg_surface_listener xdg_listener = {
    [](void* d, xdg_surface*, uint32_t serial) { static_cast<window*>(d)->serial = serial; }};
const xdg_toplevel_listener toplevel_listener = {
    [](void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
        auto* win = static_cast<window*>(d);
        win->width = w;
        win->height = h;
        if (win->first_width < 0) {
            win->first_width = w;
            win->first_height = h;
        }
        unsigned tiled = 0;
        win->maximized = false;
        const auto* s = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
            if (s[i] >= XDG_TOPLEVEL_STATE_TILED_LEFT && s[i] <= XDG_TOPLEVEL_STATE_TILED_BOTTOM)
                ++tiled;
            if (s[i] == XDG_TOPLEVEL_STATE_MAXIMIZED)
                win->maximized = true;
        }
        win->tiled = tiled == 4;
    },
    [](void*, xdg_toplevel*) {},
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {}};

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test-tiling", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/// One frame tick of the compositor (configures held back by the single-in-flight rule go).
void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::scheduler::frame_tick t{};
    t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    t.delta_seconds = 1.0 / 30.0;
    t.frame_index = static_cast<unsigned>(++g_frame);
    g_comp->tick_ui(t);
    g_comp->wayland().flush();
}

/// Acks the newest configure and commits a buffer of its size (the drawn size when 0).
void answer(client& c, window& w) {
    xdg_surface_ack_configure(w.xdg, w.serial);
    w.acked = w.serial;
    if (w.width > 0 && w.height > 0) {
        w.drawn_width = w.width;
        w.drawn_height = w.height;
    }
    wl_surface_attach(w.surface, make_buffer(c, w.drawn_width, w.drawn_height), 0, 0);
    wl_surface_commit(w.surface);
}

/// Until nothing is left to answer: round trips, answers, ticks.
void settle(client& c, std::initializer_list<window*> windows) {
    for (int round = 0; round < 8; ++round) {
        (void)wl_display_roundtrip(c.display);
        bool answered = false;
        for (window* w : windows)
            if (w && w->surface && w->serial != w->acked) {
                answer(c, *w);
                answered = true;
            }
        (void)wl_display_roundtrip(c.display);
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)wl_display_roundtrip(c.display);
        if (!answered && round > 0)
            break;
    }
}

void create_window(client& c, window& w, const char* app_id, xdg_toplevel* parent = nullptr,
                   int fixed = 0) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, &w);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    if (parent)
        xdg_toplevel_set_parent(w.toplevel, parent);
    if (fixed > 0) {
        xdg_toplevel_set_min_size(w.toplevel, fixed, fixed);
        xdg_toplevel_set_max_size(w.toplevel, fixed, fixed);
        w.drawn_width = w.drawn_height = fixed;
    }
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
}

void map_window(client& c, window& w, const char* app_id, std::initializer_list<window*> others,
                xdg_toplevel* parent = nullptr, int fixed = 0) {
    create_window(c, w, app_id, parent, fixed);
    answer(c, w);
    std::vector<window*> all(others);
    all.push_back(&w);
    for (int i = 0; i < 3; ++i) {
        (void)wl_display_roundtrip(c.display);
        for (window* o : all)
            if (o->serial != o->acked)
                answer(c, *o);
        tick();
    }
    (void)wl_display_roundtrip(c.display);
}

void destroy_window(window& w) {
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    w = {};
}

struct rec_copy {
    lx::toplevel_id id{};
    lx::rect2i geometry{};
    bool tiled = false;
    bool mapped = false;
};

rec_copy record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    rec_copy out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id())
            out = {r.id, r.geometry, r.tiled, r.mapped};
    });
    return out;
}

template <typename Fn>
auto with_wm(Fn&& fn) {
    std::lock_guard<std::mutex> hold(*g_lock);
    struct flush_after {
        ~flush_after() { g_comp->wayland().flush(); }
    } f;
    return fn(g_comp->windows());
}

std::string rect_text(lx::rect2i r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.width) + "x" +
           std::to_string(r.height);
}

bool at(const char* app_id, lx::rect2i want, std::string& why) {
    const rec_copy r = record(app_id);
    const bool ok = r.mapped && r.geometry.x == want.x && r.geometry.y == want.y &&
                    r.geometry.width == want.width && r.geometry.height == want.height;
    if (!ok)
        why += std::string(why.empty() ? "" : "; ") + app_id + " at " + rect_text(r.geometry) +
               " want " + rect_text(want);
    return ok;
}

/// The layout order of the focused output's workspace, as app ids: "a b c".
std::string order() {
    std::lock_guard<std::mutex> hold(*g_lock);
    auto& wm = g_comp->windows();
    lx::toplevel_id ids[64];
    const unsigned n = wm.tiled_windows(wm.workspaces().active(0), ids);
    std::string s;
    for (unsigned i = 0; i < n; ++i)
        if (const auto* r = g_comp->toplevels().find(ids[i]))
            s += (s.empty() ? "" : " ") + std::string(r->app_id ? r->app_id : "?");
    return s;
}

void pointer_to(double x, double y) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_motion_event m{};
    m.x = x;
    m.y = y;
    g_comp->inject_pointer_motion(m);
    g_comp->wayland().flush();
}

void left_button(bool pressed) {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::input::pointer_button_event b{};
    b.which = lx::input::button::left;
    b.evdev_code = 0x110;
    b.pressed = pressed;
    g_comp->inject_pointer_button(b);
    g_comp->wayland().flush();
}

// The layout of 1280x720, gaps 8, ratio 0.55: the masters' column is 691 wide at x 8, the
// stack's 565 at x 707; a full column is 704 high, half of one 348 (at y 8 and 364).
constexpr lx::rect2i k_full{8, 8, 1264, 704};
constexpr lx::rect2i k_master{8, 8, 691, 704};
constexpr lx::rect2i k_stack{707, 8, 565, 704};
constexpr lx::rect2i k_stack_top{707, 8, 565, 348};
constexpr lx::rect2i k_stack_bottom{707, 364, 565, 348};

void run_cases() {
    client c{};
    c.display = wl_display_connect(k_socket);
    if (!c.display) {
        check("connect", false);
        return;
    }
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    (void)wl_display_roundtrip(c.display);
    if (!c.compositor || !c.wm_base || !c.shm) {
        check("connect: wl_compositor, xdg_wm_base, wl_shm", false);
        return;
    }

    // A floating workspace: nothing tiles.
    window a{};
    map_window(c, a, "a", {});
    const rec_copy a0 = record("a");
    check("floating (the default): a window maps at its own size, not tiled",
          a0.mapped && !a0.tiled && a0.geometry.width == 200 && !a.tiled,
          rect_text(a0.geometry));

    // layout.toggle: the one window fills the usable area less the gaps, tiled states on.
    (void)with_wm([](auto& wm) { return wm.toggle_layout(0, "test"); });
    settle(c, {&a});
    {
        std::string why;
        check("toggled to master-stack: one window takes the whole area, tiled_* states",
              at("a", k_full, why) && a.tiled && record("a").tiled, why);
    }

    // T4: a new window's initial configure already carries its tile; the master reflows.
    window b{};
    map_window(c, b, "b", {&a});
    settle(c, {&a, &b});
    {
        std::string why;
        const bool first = b.first_width == k_stack.width && b.first_height == k_stack.height;
        if (!first)
            why = "b's first configure " + std::to_string(b.first_width) + "x" +
                  std::to_string(b.first_height);
        check("a new window's initial configure is its tile; the master shrinks",
              first && at("a", k_master, why) && at("b", k_stack, why), why);
    }
    window w3{};
    map_window(c, w3, "c", {&a, &b});
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("a third window splits the stack (no pixel lost between the halves)",
              at("a", k_master, why) && at("b", k_stack_top, why) &&
                  at("c", k_stack_bottom, why) && order() == "a b c",
              why + " order " + order());
    }

    // promote: c changes places with the master.
    (void)with_wm([id = record("c").id](auto& wm) { return wm.promote_tiled(id); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("layout.promote: the window changes places with the master",
              order() == "c b a" && at("c", k_master, why) && at("a", k_stack_bottom, why),
              why + " order " + order());
    }
    (void)with_wm([id = record("a").id](auto& wm) { return wm.swap_tiled(id, -1); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("layout.swap-previous: a changes places with its neighbour",
              order() == "c a b" && at("a", k_stack_top, why) && at("b", k_stack_bottom, why),
              why + " order " + order());
    }

    // The master column: two masters, then a wider ratio.
    const bool more = with_wm([](auto& wm) { return wm.adjust_master(0.0, 1); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("layout.master-more: the first two split the master column",
              more && at("c", {8, 8, 691, 348}, why) && at("a", {8, 364, 691, 348}, why) &&
                  at("b", k_stack, why),
              why);
    }
    (void)with_wm([](auto& wm) { return wm.adjust_master(0.0, -1); });
    const bool grow =
        with_wm([](auto& wm) { return wm.adjust_master(lx::compositor::k_master_ratio_step, 0); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        // 0.6 of 1256: 754.
        check("layout.master-grow: the master column widens by 0.05",
              grow && at("c", {8, 8, 754, 704}, why) && at("a", {770, 8, 502, 348}, why), why);
    }
    (void)with_wm(
        [](auto& wm) { return wm.adjust_master(-lx::compositor::k_master_ratio_step, 0); });
    settle(c, {&a, &b, &w3});

    // window.float-toggle: b leaves (the others close the gap), then joins again at the end.
    (void)with_wm([id = record("b").id](auto& wm) { return wm.toggle_floating(id); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("window.float-toggle: the window leaves the layout, the rest reflow",
              !record("b").tiled && !b.tiled && order() == "c a" && at("c", k_master, why) &&
                  at("a", k_stack, why),
              why + " order " + order());
    }
    (void)with_wm([id = record("b").id](auto& wm) { return wm.toggle_floating(id); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("window.float-toggle again: it joins at the end of the order",
              record("b").tiled && order() == "c a b" && at("b", k_stack_bottom, why), why);
    }

    // A dialog (it has a parent) and a fixed-size window float; the layout does not change.
    window d{};
    map_window(c, d, "dialog", {&a, &b, &w3}, w3.toplevel);
    window f{};
    map_window(c, f, "fixed", {&a, &b, &w3, &d}, nullptr, 300);
    settle(c, {&a, &b, &w3, &d, &f});
    {
        std::string why;
        check("a dialog and a fixed-size window float; the tiles stay",
              !record("dialog").tiled && !record("fixed").tiled && d.first_width == 0 &&
                  order() == "c a b" && at("c", k_master, why) && at("a", k_stack_top, why),
              why + " dialog first configure " + std::to_string(d.first_width));
    }
    destroy_window(d);
    destroy_window(f);
    settle(c, {&a, &b, &w3});

    // Minimized: its place goes; back, it goes last.
    with_wm([id = record("a").id](auto& wm) { wm.set_minimized(id, true, "test"); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("minimized: the window gives up its place",
              order() == "c b" && at("c", k_master, why) && at("b", k_stack, why),
              why + " order " + order());
    }
    with_wm([id = record("a").id](auto& wm) { wm.set_minimized(id, false, "test"); });
    settle(c, {&a, &b, &w3});
    check("restored: it comes back at the end of the order", order() == "c b a", order());

    // Maximized: the slot is kept (the others do not move); unmaximized, back in its tile.
    with_wm([id = record("b").id](auto& wm) { wm.set_maximized(id, true, "test"); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("maximized: it fills the area, the other tiles stay where they were",
              b.maximized && record("b").geometry.width == 1280 && at("c", k_master, why) &&
                  at("a", k_stack_bottom, why) && order() == "c b a",
              why);
    }
    with_wm([id = record("b").id](auto& wm) { wm.set_maximized(id, false, "test"); });
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("unmaximized: back in the tile it kept", !b.maximized && b.tiled &&
                                                          at("b", k_stack_top, why),
              why);
    }

    // T6: a tile dragged onto another changes places with it.
    pointer_to(k_master.x + 100, k_master.y + 100);
    left_button(true);
    const bool grabbed = with_wm([id = record("c").id](auto& wm) {
        return wm.begin_grab(id, lx::compositor::grab_mode::move, 0,
                             lx::compositor::grab_source::shell, 0x110);
    });
    pointer_to(k_stack_bottom.x + 200, k_stack_bottom.y + 100);
    tick();
    left_button(false);
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("a tile dropped on another swaps with it",
              grabbed && order() == "a b c" && at("a", k_master, why) &&
                  at("c", k_stack_bottom, why),
              why + " order " + order());
    }
    // Dropped on its own place: it goes back to its tile.
    pointer_to(k_stack_bottom.x + 50, k_stack_bottom.y + 50);
    left_button(true);
    (void)with_wm([id = record("c").id](auto& wm) {
        return wm.begin_grab(id, lx::compositor::grab_mode::move, 0,
                             lx::compositor::grab_source::shell, 0x110);
    });
    pointer_to(k_stack_bottom.x + 80, k_stack_bottom.y + 90);
    tick();
    left_button(false);
    settle(c, {&a, &b, &w3});
    {
        std::string why;
        check("a tile dropped where it was goes back to its tile",
              order() == "a b c" && at("c", k_stack_bottom, why), why);
    }

    // A window that goes leaves a gap the others close.
    destroy_window(b);
    settle(c, {&a, &w3});
    {
        std::string why;
        check("a closed window's place goes",
              order() == "a c" && at("a", k_master, why) && at("c", k_stack, why), why);
    }

    // Back to floating: a gets the geometry it had before it tiled; tiled states go.
    (void)with_wm([](auto& wm) { return wm.toggle_layout(0, "test"); });
    settle(c, {&a, &w3});
    {
        std::string why;
        check("toggled back to floating: the floating geometry returns, tiled states go",
              at("a", a0.geometry, why) && !a.tiled && !w3.tiled && !record("a").tiled &&
                  !record("c").tiled && order().empty(),
              why);
    }

    // [tiling] layout = "master-stack": every workspace starts tiled.
    with_wm([](auto& wm) {
        wm.set_tiling_config({.layout = lx::compositor::workspace_layout::master_stack});
    });
    settle(c, {&a, &w3});
    {
        std::string why;
        check("[tiling] layout = master-stack applies to the workspaces there are",
              order() == "a c" && at("a", k_master, why) && at("c", k_stack, why),
              why + " order " + order());
    }

    destroy_window(a);
    destroy_window(w3);
    settle(c, {});
    check("no protocol error", wl_display_get_error(c.display) == 0);
    wl_display_disconnect(c.display);
}

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.virtual_outputs = "1280x720";
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
                comp.windows().flush_workspaces();
                comp.wayland().flush();
            }
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    run_cases();

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
