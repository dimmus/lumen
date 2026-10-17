// Hotplug on the wire (ADR 008 Part B), against an in-process compositor whose display is
// virtual (`config::virtual_outputs`, no card opened) and whose connectors are fake scans
// fed through `compositor::simulate_connector_scan` — the same diff, policy, output plans,
// wl_output lifecycle and window migration a udev uevent drives:
//   - a monitor connects: a new wl_output global with its name, mode and position, laid out
//     after the existing output; a window opened under the pointer there enters it;
//   - an identical scan changes nothing and tells nobody (§B.7);
//   - its preferred mode changes (the VMware window resize): wl_output.mode, the xdg-output
//     size and the layer surface's configure follow;
//   - it disconnects: the window moves to the remaining output (enter there, leave the gone
//     one, before the global goes), the layer surface is `closed`, the global is removed,
//     and a bind racing the removal gets an inert wl_output, no error (H4);
//   - it comes back: a new global, and the window returns to its place there (H5);
//   - a monitor plugged and unplugged before its pipeline is up never appears, and comes up
//     when plugged again (the cancelled add freed its slot).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif
#endif

#include "scan_builder.hpp"

import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.drm;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND) || !defined(LUMEN_HAS_PROTOCOL_GLUE)

int main() {
    std::printf("SKIP: needs libwayland and the generated protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-hotplug";

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

struct output_info {
    wl_output* output = nullptr;
    uint32_t global = 0;
    bool removed = false;
    int x = -1, y = -1, w = 0, h = 0;
    std::string name;
    unsigned dones = 0;
    zxdg_output_v1* xo = nullptr;
    int lw = 0, lh = 0;
};

constexpr unsigned k_outs = 8;

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    zxdg_output_manager_v1* xom = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    output_info outs[k_outs]{};
    unsigned out_count = 0;
};

client* g_client = nullptr;

int index_of(wl_output* o) {
    for (unsigned i = 0; g_client && i < g_client->out_count; ++i)
        if (g_client->outs[i].output == o)
            return static_cast<int>(i);
    return -1;
}

int index_named(const char* name) {
    for (unsigned i = 0; g_client && i < g_client->out_count; ++i)
        if (!g_client->outs[i].removed && g_client->outs[i].name == name)
            return static_cast<int>(i);
    return -1;
}

void out_geometry(void* d, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                  const char*, const char*, int32_t) {
    auto* o = static_cast<output_info*>(d);
    o->x = x;
    o->y = y;
}
void out_mode(void* d, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
    auto* o = static_cast<output_info*>(d);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->w = w;
        o->h = h;
    }
}
void out_done(void* d, wl_output*) { ++static_cast<output_info*>(d)->dones; }
void out_scale(void*, wl_output*, int32_t) {}
void out_name(void* d, wl_output*, const char* n) { static_cast<output_info*>(d)->name = n; }
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done,
                                            out_scale,    out_name, out_description};

void xo_position(void*, zxdg_output_v1*, int32_t, int32_t) {}
void xo_size(void* d, zxdg_output_v1*, int32_t w, int32_t h) {
    auto* o = static_cast<output_info*>(d);
    o->lw = w;
    o->lh = h;
}
void xo_done(void*, zxdg_output_v1*) {}
void xo_name(void*, zxdg_output_v1*, const char*) {}
void xo_description(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener xdg_output_listener = {xo_position, xo_size, xo_done, xo_name,
                                                     xo_description};

struct surface_info {
    wl_surface* surface = nullptr;
    std::string events;
};
void surf_enter(void* d, wl_surface*, wl_output* o) {
    static_cast<surface_info*>(d)->events += "+" + std::to_string(index_of(o));
}
void surf_leave(void* d, wl_surface*, wl_output* o) {
    static_cast<surface_info*>(d)->events += "-" + std::to_string(index_of(o));
}
void surf_scale(void*, wl_surface*, int32_t) {}
void surf_transform(void*, wl_surface*, uint32_t) {}
const wl_surface_listener surface_listener = {surf_enter, surf_leave, surf_scale, surf_transform};

void bind_output(client& c, output_info& o, uint32_t name) {
    o.global = name;
    o.output = static_cast<wl_output*>(wl_registry_bind(c.registry, name, &wl_output_interface, 4));
    wl_output_add_listener(o.output, &output_listener, &o);
    if (c.xom) {
        o.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
        zxdg_output_v1_add_listener(o.xo, &xdg_output_listener, &o);
    }
}

std::string g_removed_names;

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (!std::strcmp(iface, wl_compositor_interface.name))
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6));
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (!std::strcmp(iface, wl_shm_interface.name))
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (!std::strcmp(iface, zxdg_output_manager_v1_interface.name))
        c->xom = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 3 ? version : 3));
    else if (!std::strcmp(iface, zwlr_layer_shell_v1_interface.name))
        c->layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1));
    else if (!std::strcmp(iface, wl_output_interface.name) && c->out_count < k_outs) {
        // Bound as it is announced (later ones too: hotplug).
        output_info& o = c->outs[c->out_count++];
        o.global = name;
        if (c->compositor)
            bind_output(*c, o, name);
    }
}
void on_global_remove(void* data, wl_registry*, uint32_t name) {
    auto* c = static_cast<client*>(data);
    for (unsigned i = 0; i < c->out_count; ++i) {
        if (c->outs[i].global == name && !c->outs[i].removed) {
            c->outs[i].removed = true;
            g_removed_names += c->outs[i].name + ";";
        }
    }
}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

void on_ping(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
const xdg_wm_base_listener wm_base_listener = {on_ping};

bool connect(client& c) {
    g_client = &c;
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0 || !c.compositor || !c.wm_base || !c.shm)
        return false;
    xdg_wm_base_add_listener(c.wm_base, &wm_base_listener, nullptr);
    for (unsigned i = 0; i < c.out_count; ++i)
        if (!c.outs[i].output)
            bind_output(c, c.outs[i], c.outs[i].global);
    return wl_display_roundtrip(c.display) >= 0;
}

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-hotplug", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct configured {
    uint32_t serial = 0;
    unsigned w = 0, h = 0;
    unsigned count = 0;
    bool closed = false;
};
void on_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<configured*>(data)->serial = serial;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

void layer_configure(void* data, zwlr_layer_surface_v1*, uint32_t serial, uint32_t w, uint32_t h) {
    auto* c = static_cast<configured*>(data);
    c->serial = serial;
    c->w = w;
    c->h = h;
    ++c->count;
}
void layer_closed(void* data, zwlr_layer_surface_v1*) { static_cast<configured*>(data)->closed = true; }
const zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.virtual_outputs = "800x600";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    auto& outs = comp.outputs();

    std::atomic<bool> pump{true};
    std::mutex dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(dispatch_lock);
                (void)comp.wayland().dispatch(10);
                comp.wayland().flush();
            }
            // std::mutex is not fair: without a pause outside the lock the test thread can
            // wait seconds for it.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    unsigned long long frame = 0;
    const auto tick = [&] {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::scheduler::frame_tick t{};
        t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        t.delta_seconds = 1.0 / 30.0;
        t.frame_index = static_cast<unsigned>(++frame);
        comp.tick_ui(t);
    };
    const auto ui = [&](auto fn) {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        fn();
    };
    // Render answers output plans on its own thread; the answers run on UI here.
    const auto settle = [&](auto done) {
        for (int i = 0; i < 200; ++i) {
            bool ok = false;
            ui([&] {
                comp.run_posted_ui_tasks();
                ok = done();
            });
            if (ok)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c{};
    const bool up = connect(c);
    check(up && c.out_count == 1 && c.layer_shell && c.xom, "start: one wl_output (HEADLESS-1)");
    if (!up) {
        pump.store(false);
        pump_thread.join();
        return 1;
    }

    using scan_builder::connector;
    using scan_builder::mode;
    lx::drm::connector_scan scan{};
    connector(scan, 44, "Virtual-1", true, {mode(1024, 768), mode(640, 480)});
    connector(scan, 53, "Virtual-2", false);

    // ── connect ──
    ui([&] { comp.simulate_connector_scan(scan); });
    const bool added = settle([&] { return outs.count() == 2; });
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    const int v1 = index_named("Virtual-1");
    check(added && v1 >= 0, "a connected monitor becomes an output with a new wl_output global");
    if (v1 >= 0) {
        const output_info& o = c.outs[v1];
        check(o.w == 1024 && o.h == 768 && o.x == 800 && o.y == 0 && o.lw == 1024 && o.dones >= 1,
              "its wl_output: preferred mode 1024x768, laid out after HEADLESS-1 (x=800)");
    }

    // ── identical scan: nothing ──
    {
        unsigned gen = 0;
        unsigned dones = v1 >= 0 ? c.outs[v1].dones : 0;
        ui([&] { gen = outs.generation(); });
        ui([&] { comp.simulate_connector_scan(scan); });
        (void)settle([] { return true; });
        (void)wl_display_roundtrip(c.display);
        unsigned gen2 = 0;
        ui([&] { gen2 = outs.generation(); });
        check(gen2 == gen && (v1 < 0 || c.outs[v1].dones == dones),
              "an identical scan changes nothing and tells nobody");
    }

    // ── a window under the pointer on the new output, a layer surface on it ──
    ui([&] {
        lx::input::pointer_motion_event m{};
        m.x = 800 + 512;
        m.y = 384;
        comp.inject_pointer_motion(m);
    });
    surface_info w{};
    w.surface = wl_compositor_create_surface(c.compositor);
    wl_surface_add_listener(w.surface, &surface_listener, &w);
    configured wcfg{};
    xdg_surface* xs = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(xs, &xdg_listener, &wcfg);
    xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(xs, wcfg.serial);
    wl_surface_attach(w.surface, make_buffer(c, 300, 200), 0, 0);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    tick();
    (void)wl_display_roundtrip(c.display);
    check(v1 >= 0 && w.events == "+" + std::to_string(v1),
          "a window opened with the pointer on it enters the new output only");
    lx::rect2i home{};
    ui([&] {
        comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) { home = r.geometry; });
    });

    configured lcfg{};
    zwlr_layer_surface_v1* layer = nullptr;
    surface_info ls{};
    if (v1 >= 0) {
        ls.surface = wl_compositor_create_surface(c.compositor);
        layer = zwlr_layer_shell_v1_get_layer_surface(c.layer_shell, ls.surface, c.outs[v1].output,
                                                      ZWLR_LAYER_SHELL_V1_LAYER_TOP, "panel");
        zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &lcfg);
        zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(layer, 0, 24);
        wl_surface_commit(ls.surface);
        (void)wl_display_roundtrip(c.display);
        zwlr_layer_surface_v1_ack_configure(layer, lcfg.serial);
        wl_surface_attach(ls.surface, make_buffer(c, static_cast<int>(lcfg.w), 24), 0, 0);
        wl_surface_commit(ls.surface);
        (void)wl_display_roundtrip(c.display);
        tick();
        (void)wl_display_roundtrip(c.display);
        check(lcfg.w == 1024 && lcfg.h == 24, "a layer surface on it is as wide as it");
    }

    // ── its preferred mode changes (the VM window resize) ──
    connector(scan, 44, "Virtual-1", true, {mode(1280, 720), mode(1024, 768), mode(640, 480)});
    ui([&] { comp.simulate_connector_scan(scan); });
    (void)settle([&] { return outs.slot_state(1).mode.width == 1280; });
    tick();
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    if (v1 >= 0)
        check(c.outs[v1].w == 1280 && c.outs[v1].h == 720 && c.outs[v1].lw == 1280 &&
                  c.outs[v1].lh == 720,
              "a new preferred mode: wl_output.mode and the xdg-output size follow (1280x720)");
    check(lcfg.w == 1280 && !lcfg.closed, "the layer surface is configured to the new width");

    // ── it disconnects ──
    const std::string before = w.events;
    connector(scan, 44, "Virtual-1", false);
    ui([&] { comp.simulate_connector_scan(scan); });
    const bool removed = settle([&] { return outs.count() == 1; });
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    check(removed && v1 >= 0 && c.outs[v1].removed, "unplugged: its wl_output global is removed");
    check(w.events == before + "+0-" + std::to_string(v1),
          "the window enters the remaining output, then leaves the gone one (before the global goes)");
    check(lcfg.closed, "its layer surface is closed");
    lx::rect2i moved{};
    ui([&] {
        comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) { moved = r.geometry; });
    });
    check(moved.x >= 0 && moved.x + moved.width <= 800 && moved.y >= 0 && moved.y + moved.height <= 600,
          "the window is inside the remaining output");
    // A bind racing the removal (the client had not seen global_remove yet): inert, no error.
    if (v1 >= 0) {
        auto* late = static_cast<wl_output*>(
            wl_registry_bind(c.registry, c.outs[v1].global, &wl_output_interface, 4));
        const int r = wl_display_roundtrip(c.display);
        wl_output_release(late);
        check(r >= 0 && wl_display_get_error(c.display) == 0,
              "binding the removed wl_output global gives an inert object, no error");
    }

    // ── it comes back: a new global, the window goes home ──
    connector(scan, 44, "Virtual-1", true, {mode(1280, 720), mode(1024, 768)});
    ui([&] { comp.simulate_connector_scan(scan); });
    const bool back = settle([&] { return outs.count() == 2; });
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    tick();
    (void)wl_display_roundtrip(c.display);
    const int v1b = index_named("Virtual-1");
    check(back && v1b >= 0 && v1b != v1, "plugged in again: a new wl_output global");
    lx::rect2i again{};
    ui([&] {
        comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) { again = r.geometry; });
    });
    check(again.x == home.x && again.y == home.y,
          "the window returns to its place on that output (home)");
    check(v1b >= 0 && w.events.find("+" + std::to_string(v1b)) != std::string::npos,
          "and enters it");

    // ── plugged and unplugged before its pipeline is up: the add is cancelled ──
    connector(scan, 53, "Virtual-2", true, {mode(800, 600)});
    ui([&] {
        comp.simulate_connector_scan(scan);
        connector(scan, 53, "Virtual-2", false);
        comp.simulate_connector_scan(scan);
    });
    for (int i = 0; i < 20; ++i) {
        ui([&] { comp.run_posted_ui_tasks(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)wl_display_roundtrip(c.display);
    check(outs.count() == 2 && index_named("Virtual-2") < 0,
          "plugged and unplugged at once: no output, no global");
    connector(scan, 53, "Virtual-2", true, {mode(800, 600)});
    ui([&] { comp.simulate_connector_scan(scan); });
    const bool v2 = settle([&] { return outs.count() == 3; });
    (void)wl_display_roundtrip(c.display);
    (void)wl_display_roundtrip(c.display);
    check(v2 && index_named("Virtual-2") >= 0, "and plugged in again: it comes up");

    if (layer)
        zwlr_layer_surface_v1_destroy(layer);
    if (ls.surface)
        wl_surface_destroy(ls.surface);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(w.surface);
    (void)wl_display_roundtrip(c.display);
    wl_display_disconnect(c.display);

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
