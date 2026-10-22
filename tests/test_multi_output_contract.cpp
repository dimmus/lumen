// Multi-output contracts on the wire (ADR 008 Part A), against an in-process compositor with
// two virtual outputs (`config::virtual_outputs`: no card is opened): HEADLESS-1 800x600 at
// scale 1 and HEADLESS-2 640x480 at scale 1.5, laid out left to right.
//   - one wl_output global per output, each describing its own output (name, mode, scale
//     rounded up to an integer, position) and one zxdg_output_v1 logical rect each;
//   - a toplevel maps on the output the pointer is on (the first) and enters only it;
//   - a layer surface asked for the second output is configured to that output's logical
//     width, enters it only, and gets its scales (preferred_buffer_scale 2, fractional 180);
//   - moving an output re-sends its wl_output/xdg-output group (geometry, position, done) to
//     every bound object of that output only.
#include <stdio.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
// wayland-scanner names an argument `namespace`, a C++ keyword.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#include "fractional-scale-v1-client-protocol.h"
#endif
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.scheduler;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND) || !defined(LUMEN_HAS_PROTOCOL_GLUE)

int main() {
    std::printf("SKIP: needs libwayland and the generated protocol glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-multi-output";

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
    uint32_t version = 0;
    int x = -1, y = -1, w = 0, h = 0, scale = 0;
    std::string name;
    unsigned dones = 0;
    zxdg_output_v1* xo = nullptr;
    int lx = -1, ly = -1, lw = 0, lh = 0;
};

struct surface_info {
    wl_surface* surface = nullptr;
    /// "+N" enter / "-N" leave of output index N, in arrival order.
    std::string events;
    int preferred_scale = 0;
    unsigned fractional = 0;
};

constexpr unsigned k_outs = 4;

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    zxdg_output_manager_v1* xom = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    wp_fractional_scale_manager_v1* fractional = nullptr;
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
void out_scale(void* d, wl_output*, int32_t s) { static_cast<output_info*>(d)->scale = s; }
void out_name(void* d, wl_output*, const char* n) { static_cast<output_info*>(d)->name = n; }
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done,
                                            out_scale,    out_name, out_description};

void xo_position(void* d, zxdg_output_v1*, int32_t x, int32_t y) {
    auto* o = static_cast<output_info*>(d);
    o->lx = x;
    o->ly = y;
}
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

void surf_enter(void* d, wl_surface*, wl_output* o) {
    static_cast<surface_info*>(d)->events += "+" + std::to_string(index_of(o));
}
void surf_leave(void* d, wl_surface*, wl_output* o) {
    static_cast<surface_info*>(d)->events += "-" + std::to_string(index_of(o));
}
void surf_scale(void* d, wl_surface*, int32_t s) {
    static_cast<surface_info*>(d)->preferred_scale = s;
}
void surf_transform(void*, wl_surface*, uint32_t) {}
const wl_surface_listener surface_listener = {surf_enter, surf_leave, surf_scale, surf_transform};

void frac_preferred(void* d, wp_fractional_scale_v1*, uint32_t s) {
    static_cast<surface_info*>(d)->fractional = s;
}
const wp_fractional_scale_v1_listener fractional_listener = {frac_preferred};

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
    else if (!std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name))
        c->fractional = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1));
    else if (!std::strcmp(iface, wl_output_interface.name) && c->out_count < k_outs) {
        c->outs[c->out_count].global = name;
        c->outs[c->out_count].version = version;
        ++c->out_count;
    }
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
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
    for (unsigned i = 0; i < c.out_count; ++i) {
        output_info& o = c.outs[i];
        o.output = static_cast<wl_output*>(wl_registry_bind(
            c.registry, o.global, &wl_output_interface, o.version < 4 ? o.version : 4));
        wl_output_add_listener(o.output, &output_listener, &o);
        if (c.xom) {
            o.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
            zxdg_output_v1_add_listener(o.xo, &xdg_output_listener, &o);
        }
    }
    return wl_display_roundtrip(c.display) >= 0;
}

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-multi-output", MFD_CLOEXEC);
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
}
void layer_closed(void* data, zwlr_layer_surface_v1*) { static_cast<configured*>(data)->closed = true; }
const zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.virtual_outputs = "800x600,640x480@1.5";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    auto& outs = comp.outputs();
    check(outs.count() == 2, "two virtual outputs");
    if (outs.count() != 2) {
        std::printf("%d cases, %d failed\n", g_ran, g_failed);
        return 1;
    }
    const auto s0 = outs.state(outs.nth(0));
    const auto s1 = outs.state(outs.nth(1));
    const lx::rect2i l0 = s0.logical_rect();
    const lx::rect2i l1 = s1.logical_rect();
    std::printf("  outputs: %s %dx%d logical %dx%d+%d+%d, %s %dx%d logical %dx%d+%d+%d\n",
                s0.name, s0.mode.width, s0.mode.height, l0.width, l0.height, l0.x, l0.y, s1.name,
                s1.mode.width, s1.mode.height, l1.width, l1.height, l1.x, l1.y);

    std::atomic<bool> pump{true};
    std::mutex dispatch_lock;
    std::thread pump_thread([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        while (pump.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            (void)comp.wayland().dispatch(10);
            comp.wayland().flush();
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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c{};
    const bool up = connect(c);
    check(up && c.out_count == 2 && c.xom && c.layer_shell,
          "two wl_output globals, xdg-output and layer shell advertised");
    if (up && c.out_count == 2) {
        const output_info& a = c.outs[0];
        const output_info& b = c.outs[1];
        check(a.name == s0.name && b.name == s1.name && a.name != b.name,
              "each wl_output names its own output");
        check(a.w == 800 && a.h == 600 && b.w == 640 && b.h == 480,
              "each wl_output's current mode is its own");
        check(a.scale == 1 && b.scale == 2,
              "wl_output.scale: 1, and 2 for the 1.5 output (integer ceiling)");
        check(a.x == l0.x && b.x == l1.x && b.x == l0.x + l0.width && a.y == 0 && b.y == 0,
              "geometry positions: left to right, the second at the first's right edge");
        check(a.lx == l0.x && a.lw == l0.width && a.lh == l0.height && b.lx == l1.x &&
                  b.lw == l1.width && b.lh == l1.height && b.lw == 427 && b.lh == 320,
              "zxdg_output_v1 logical rects (640x480 at 1.5 = 427x320)");
        // wl_output's group, then xdg-output v3's (closed by another wl_output.done).
        check(a.dones == 2 && b.dones == 2, "each output's groups are closed by its own done");

        // ── a toplevel maps on the pointer's output (the first) and enters only it ──
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
        wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
        wl_surface_commit(w.surface);
        (void)wl_display_roundtrip(c.display);
        tick();
        (void)wl_display_roundtrip(c.display);
        check(w.events == "+0" && w.preferred_scale == 0,
              "a toplevel maps on the pointer's output: enter output 0 only, scale 1 (unsent)");

        // ── a layer surface on the second output ──
        if (c.layer_shell) {
            surface_info ls{};
            ls.surface = wl_compositor_create_surface(c.compositor);
            wl_surface_add_listener(ls.surface, &surface_listener, &ls);
            wp_fractional_scale_v1* fs = nullptr;
            if (c.fractional) {
                fs = wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, ls.surface);
                wp_fractional_scale_v1_add_listener(fs, &fractional_listener, &ls);
            }
            configured lcfg{};
            zwlr_layer_surface_v1* layer = zwlr_layer_shell_v1_get_layer_surface(
                c.layer_shell, ls.surface, b.output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "panel");
            zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &lcfg);
            zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(layer, 0, 30);
            zwlr_layer_surface_v1_set_exclusive_zone(layer, 30);
            wl_surface_commit(ls.surface);
            (void)wl_display_roundtrip(c.display);
            check(lcfg.w == static_cast<unsigned>(l1.width) && lcfg.h == 30,
                  "a layer surface on the second output is as wide as that output (logical)");
            zwlr_layer_surface_v1_ack_configure(layer, lcfg.serial);
            wl_surface_set_buffer_scale(ls.surface, 2);
            wl_surface_attach(ls.surface, make_buffer(c, static_cast<int>(lcfg.w) * 2, 60), 0, 0);
            wl_surface_commit(ls.surface);
            (void)wl_display_roundtrip(c.display);
            tick();
            (void)wl_display_roundtrip(c.display);
            check(ls.events == "+1", "the layer surface enters the second output only");
            check(ls.preferred_scale == 2 && (!c.fractional || ls.fractional == 180),
                  "its preferred scales are the second output's: buffer scale 2, fractional 180");
            check(w.events == "+0", "the toplevel on the first output is unaffected");

            // ── moving an output re-sends that output's group only ──
            const unsigned da = a.dones;
            const unsigned db = b.dones;
            {
                // The first output pinned where it is (an auto-placed output would follow the
                // explicit one to its right, sway's rule), the second below it.
                std::lock_guard<std::mutex> hold(dispatch_lock);
                outs.set_position(outs.nth(0), {l0.x, l0.y});
                outs.set_position(outs.nth(1), {0, l0.height});
            }
            tick();
            (void)wl_display_roundtrip(c.display);
            check(b.x == 0 && b.y == l0.height && b.lx == 0 && b.ly == l0.height &&
                      b.dones == db + 1,
                  "a moved output: geometry and xdg-output position re-sent, one done");
            check(a.dones == da, "the other output's objects hear nothing");
            check(lcfg.w == static_cast<unsigned>(l1.width) && !lcfg.closed,
                  "the layer surface stays on its output across the move");
            zwlr_layer_surface_v1_destroy(layer);
            if (fs)
                wp_fractional_scale_v1_destroy(fs);
            wl_surface_destroy(ls.surface);
        }
        xdg_toplevel_destroy(tl);
        xdg_surface_destroy(xs);
        wl_surface_destroy(w.surface);
        (void)wl_display_roundtrip(c.display);
    }
    if (c.display)
        wl_display_disconnect(c.display);

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
