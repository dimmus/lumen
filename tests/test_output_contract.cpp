// Output contracts on the wire, against an in-process compositor and its first output —
// the virtual HEADLESS-1 (1920x1080 at 60 Hz) without a KMS device; on a host with one the
// unprivileged test process still enumerates its connectors ("Virtual-1" on vmwgfx), so the
// expected values are read from the compositor's output manager:
//   - wl_output v4 describes it (geometry, mode, scale, name, description, done);
//   - zxdg_output_v1 v3 gives the logical rect, name and description, closed by
//     wl_output.done; v2 is closed by zxdg_output_v1.done instead;
//   - a mapped toplevel gets wl_surface.enter, a null attach (unmap) leave;
//   - a wl_output bound after mapping replays the enter;
//   - subsurfaces and popups get enter;
//   - at scale 1 no preferred_buffer_scale / _transform is sent (they equal the defaults);
//   - an output change seen by a frame tick (here: a transform) is sent to every bound
//     wl_output (geometry … done) and as preferred_buffer_transform to the v6 surfaces on it.
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
#endif
#endif

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

const char* const k_socket = "lumen-test-output-contract";

/// The compositor's first output, as the wire must describe it.
struct expected_output {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0, refresh = 0;
} g_want;

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

/// What one wl_output object was told.
struct output_info {
    wl_output* output = nullptr;
    uint32_t global_name = 0;
    int x = -1, y = -1, w = 0, h = 0, refresh = 0, scale = 0, transform = -1;
    std::string make, model, name, description;
    unsigned dones = 0;
};

struct xdg_output_info {
    zxdg_output_v1* xo = nullptr;
    int x = -1, y = -1, w = 0, h = 0;
    std::string name, description;
    unsigned dones = 0;
};

/// Per surface: enter/leave in arrival order ("+" enter, "-" leave) and preferred events.
struct surface_info {
    wl_surface* surface = nullptr;
    std::string events;
    int preferred_scale = 0;
    int preferred_transform = -1;
};

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    zxdg_output_manager_v1* xom = nullptr;
    uint32_t output_name = 0;
    uint32_t output_version = 0;
    uint32_t xom_version = 3;
};

void out_geometry(void* d, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                  const char* make, const char* model, int32_t transform) {
    auto* o = static_cast<output_info*>(d);
    o->x = x;
    o->y = y;
    o->transform = transform;
    o->make = make;
    o->model = model;
}
void out_mode(void* d, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    auto* o = static_cast<output_info*>(d);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->w = w;
        o->h = h;
        o->refresh = refresh;
    }
}
void out_done(void* d, wl_output*) { ++static_cast<output_info*>(d)->dones; }
void out_scale(void* d, wl_output*, int32_t s) { static_cast<output_info*>(d)->scale = s; }
void out_name(void* d, wl_output*, const char* n) { static_cast<output_info*>(d)->name = n; }
void out_description(void* d, wl_output*, const char* s) {
    static_cast<output_info*>(d)->description = s;
}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done,
                                            out_scale,    out_name, out_description};

void xo_position(void* d, zxdg_output_v1*, int32_t x, int32_t y) {
    auto* o = static_cast<xdg_output_info*>(d);
    o->x = x;
    o->y = y;
}
void xo_size(void* d, zxdg_output_v1*, int32_t w, int32_t h) {
    auto* o = static_cast<xdg_output_info*>(d);
    o->w = w;
    o->h = h;
}
void xo_done(void* d, zxdg_output_v1*) { ++static_cast<xdg_output_info*>(d)->dones; }
void xo_name(void* d, zxdg_output_v1*, const char* n) { static_cast<xdg_output_info*>(d)->name = n; }
void xo_description(void* d, zxdg_output_v1*, const char* s) {
    static_cast<xdg_output_info*>(d)->description = s;
}
const zxdg_output_v1_listener xdg_output_listener = {xo_position, xo_size, xo_done, xo_name,
                                                     xo_description};

void surf_enter(void* d, wl_surface*, wl_output*) { static_cast<surface_info*>(d)->events += '+'; }
void surf_leave(void* d, wl_surface*, wl_output*) { static_cast<surface_info*>(d)->events += '-'; }
void surf_scale(void* d, wl_surface*, int32_t s) {
    static_cast<surface_info*>(d)->preferred_scale = s;
}
void surf_transform(void* d, wl_surface*, uint32_t t) {
    static_cast<surface_info*>(d)->preferred_transform = static_cast<int>(t);
}
const wl_surface_listener surface_listener = {surf_enter, surf_leave, surf_scale, surf_transform};

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (!std::strcmp(iface, wl_compositor_interface.name))
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6));
    else if (!std::strcmp(iface, wl_subcompositor_interface.name))
        c->subcompositor = static_cast<wl_subcompositor*>(
            wl_registry_bind(r, name, &wl_subcompositor_interface, 1));
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        c->wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (!std::strcmp(iface, wl_shm_interface.name))
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (!std::strcmp(iface, zxdg_output_manager_v1_interface.name))
        c->xom = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(
            r, name, &zxdg_output_manager_v1_interface,
            c->xom_version < version ? c->xom_version : version));
    else if (!std::strcmp(iface, wl_output_interface.name)) {
        c->output_name = name;
        c->output_version = version;
    }
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

void on_ping(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
const xdg_wm_base_listener wm_base_listener = {on_ping};

bool connect(client& c, uint32_t xom_version = 3) {
    c.xom_version = xom_version;
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0 || !c.compositor || !c.wm_base || !c.shm)
        return false;
    xdg_wm_base_add_listener(c.wm_base, &wm_base_listener, nullptr);
    return true;
}

void bind_output(client& c, output_info& o, uint32_t version = 4) {
    o.output = static_cast<wl_output*>(wl_registry_bind(
        c.registry, c.output_name, &wl_output_interface,
        version < c.output_version ? version : c.output_version));
    wl_output_add_listener(o.output, &output_listener, &o);
}

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-output-contract", MFD_CLOEXEC);
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
};
void on_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<configured*>(data)->serial = serial;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

struct window {
    surface_info info{};
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    configured cfg{};
};

/// A mapped 300x200 toplevel with a surface listener.
void map_window(client& c, window& w) {
    w.info.surface = wl_compositor_create_surface(c.compositor);
    wl_surface_add_listener(w.info.surface, &surface_listener, &w.info);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.info.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w.cfg);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    wl_surface_commit(w.info.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.cfg.serial);
    wl_surface_attach(w.info.surface, make_buffer(c, 300, 200), 0, 0);
    wl_surface_commit(w.info.surface);
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
    {
        const auto st = comp.outputs().state(comp.outputs().nth(0));
        g_want = {st.name, st.geometry.x, st.geometry.y, st.geometry.width, st.geometry.height,
                  static_cast<int>(st.refresh_mhz)};
        std::printf("  output under test: %s %dx%d+%d+%d @%d mHz\n", g_want.name.c_str(),
                    g_want.w, g_want.h, g_want.x, g_want.y, g_want.refresh);
    }
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
    // One frame tick on this thread (UI affinity while the pump is held off): what notices an
    // output change and tells clients.
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
    const auto set_transform = [&](unsigned transform) {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        comp.outputs().set_transform(comp.outputs().nth(0), transform);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── wl_output and xdg-output v3 ──
    {
        client c{};
        const bool up = connect(c) && c.output_name != 0 && c.xom;
        check(up, "the compositor advertises wl_output and zxdg_output_manager_v1");
        if (up) {
            output_info o{};
            bind_output(c, o);
            (void)wl_display_roundtrip(c.display);
            check(!g_want.name.empty() && o.name == g_want.name && o.description == g_want.name,
                  "wl_output v4 name and description: the output's (connector) name");
            check(o.w == g_want.w && o.h == g_want.h && o.refresh == g_want.refresh &&
                      o.refresh > 0 && o.scale == 1 && o.x == g_want.x && o.y == g_want.y,
                  "wl_output mode and refresh are the output's, scale 1, at its position");
            check(o.make == "Lumen" && o.model == g_want.name && o.dones == 1,
                  "wl_output geometry make=Lumen model=<name>, one done");

            xdg_output_info x{};
            x.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
            zxdg_output_v1_add_listener(x.xo, &xdg_output_listener, &x);
            (void)wl_display_roundtrip(c.display);
            check(x.x == g_want.x && x.y == g_want.y && x.w == g_want.w && x.h == g_want.h,
                  "zxdg_output_v1 logical position and size (the mode at scale 1)");
            check(x.name == g_want.name && x.description == g_want.name,
                  "zxdg_output_v1 v3 name and description");
            check(x.dones == 0 && o.dones == 2,
                  "zxdg_output_v1 v3 is closed by wl_output.done, not its own done");
        }
        wl_display_disconnect(c.display);
    }

    // ── xdg-output v2: its own done ──
    {
        client c{};
        if (connect(c, 2) && c.xom) {
            output_info o{};
            bind_output(c, o);
            (void)wl_display_roundtrip(c.display);
            xdg_output_info x{};
            x.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
            zxdg_output_v1_add_listener(x.xo, &xdg_output_listener, &x);
            (void)wl_display_roundtrip(c.display);
            check(x.dones == 1 && o.dones == 1 && x.name == g_want.name,
                  "zxdg_output_v1 v2 gets zxdg_output_v1.done (and wl_output.done is not resent)");
        } else {
            check(false, "connect for the xdg-output v2 case");
        }
        wl_display_disconnect(c.display);
    }

    // ── enter on map, leave on unmap; nothing preferred at scale 1 ──
    {
        client c{};
        if (connect(c)) {
            output_info o{};
            bind_output(c, o);
            (void)wl_display_roundtrip(c.display);
            window w{};
            map_window(c, w);
            check(w.info.events == "+", "a mapped toplevel gets wl_surface.enter once");
            check(w.info.preferred_scale == 0 && w.info.preferred_transform == -1,
                  "v6 surface at scale 1: no preferred_buffer_scale/_transform (the defaults)");
            // An output change: the next tick tells every wl_output and moves the preferred
            // transform of the surfaces on it.
            const unsigned dones = o.dones;
            set_transform(1); // 90°
            tick();
            (void)wl_display_roundtrip(c.display);
            check(o.transform == 1 && o.dones == dones + 1 && w.info.preferred_transform == 1,
                  "output change: wl_output geometry(transform 90) + done, and "
                  "preferred_buffer_transform(90) to the surface on it");
            set_transform(0);
            tick();
            tick(); // unchanged: nothing more is sent
            (void)wl_display_roundtrip(c.display);
            check(o.transform == 0 && o.dones == dones + 2 && w.info.preferred_transform == 0 &&
                      w.info.events == "+",
                  "back to normal: one more done, preferred_buffer_transform(normal), no "
                  "enter/leave churn");
            // Subsurface with content: enter as it maps (with the parent's next commit).
            surface_info sub{};
            sub.surface = wl_compositor_create_surface(c.compositor);
            wl_surface_add_listener(sub.surface, &surface_listener, &sub);
            wl_subsurface* ss =
                wl_subcompositor_get_subsurface(c.subcompositor, sub.surface, w.info.surface);
            wl_subsurface_set_position(ss, 10, 10);
            wl_surface_attach(sub.surface, make_buffer(c, 40, 40), 0, 0);
            wl_surface_commit(sub.surface);
            wl_surface_commit(w.info.surface);
            (void)wl_display_roundtrip(c.display);
            check(sub.events == "+", "a mapped subsurface gets wl_surface.enter");
            // A popup of the window.
            surface_info pop{};
            pop.surface = wl_compositor_create_surface(c.compositor);
            wl_surface_add_listener(pop.surface, &surface_listener, &pop);
            xdg_surface* pxs = xdg_wm_base_get_xdg_surface(c.wm_base, pop.surface);
            configured pcfg{};
            xdg_surface_add_listener(pxs, &xdg_listener, &pcfg);
            xdg_positioner* pos = xdg_wm_base_create_positioner(c.wm_base);
            xdg_positioner_set_size(pos, 60, 40);
            xdg_positioner_set_anchor_rect(pos, 5, 5, 10, 10);
            xdg_popup* popup = xdg_surface_get_popup(pxs, w.xdg, pos);
            xdg_positioner_destroy(pos);
            wl_surface_commit(pop.surface);
            (void)wl_display_roundtrip(c.display);
            xdg_surface_ack_configure(pxs, pcfg.serial);
            wl_surface_attach(pop.surface, make_buffer(c, 60, 40), 0, 0);
            wl_surface_commit(pop.surface);
            (void)wl_display_roundtrip(c.display);
            check(pop.events == "+", "a mapped popup gets wl_surface.enter");
            xdg_popup_destroy(popup);
            xdg_surface_destroy(pxs);
            wl_surface_destroy(pop.surface);
            // Unmap the window: leave for it and its subsurface.
            wl_surface_attach(w.info.surface, nullptr, 0, 0);
            wl_surface_commit(w.info.surface);
            (void)wl_display_roundtrip(c.display);
            check(w.info.events == "+-" && sub.events == "+-",
                  "a null attach (unmap) sends leave to the window and its subsurface");
        } else {
            check(false, "connect for the enter/leave case");
        }
        wl_display_disconnect(c.display);
    }

    // ── a wl_output bound after mapping replays the enter ──
    {
        client c{};
        if (connect(c)) {
            window w{};
            map_window(c, w);
            const bool none_yet = w.info.events.empty();
            output_info o{};
            bind_output(c, o);
            (void)wl_display_roundtrip(c.display);
            check(none_yet && w.info.events == "+",
                  "no enter without a bound wl_output; a late bind replays it");
            // A second wl_output object of the same output gets its own enter.
            output_info o2{};
            bind_output(c, o2, 3);
            (void)wl_display_roundtrip(c.display);
            check(w.info.events == "++" && o2.dones == 1 && o2.name.empty(),
                  "a second wl_output (v3: no name) gets its own enter replay");
        } else {
            check(false, "connect for the late-bind case");
        }
        wl_display_disconnect(c.display);
    }

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
