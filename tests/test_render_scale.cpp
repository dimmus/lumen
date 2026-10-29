// HiDPI supersampling on the wire, against an in-process compositor with one virtual 1280x720
// output at scale 1 and a configuration file of its own (LUMEN_CONFIG):
//   - without a render scale the hints are the scale's (fractional 120, no
//     preferred_buffer_scale);
//   - config.toml `[[output]] render-scale = 2`, applied by a reload, moves both hints of the
//     surfaces on the output (wp_fractional_scale_v1 240, wl_surface.preferred_buffer_scale
//     2) — and nothing else: no wl_output event, the same xdg_output logical size;
//   - a fractional client then renders 2x behind a viewport, an integer client at buffer
//     scale 2: their buffers are 2x, their logical sizes and window geometry unchanged;
//   - a surface created afterwards starts with the render scale's hint;
//   - a later reload changes it again (1.5 by connector name: 180 and ceil 2; 0.5
//     undersampling: 60 and 1), an out-of-range value falls back to the output scale, and
//     removing the entry brings the scale's hints back;
//   - `lumen-ctl outputs`' source (the output manager) reports the render scale.
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
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

const char* const k_socket = "lumen-test-render-scale";

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
    int scale = 0;
    unsigned dones = 0;
    zxdg_output_v1* xo = nullptr;
    int lw = 0, lh = 0;
};

struct surface_info {
    wl_surface* surface = nullptr;
    int buffer_scale = 0;   ///< wl_surface.preferred_buffer_scale (0: never sent)
    unsigned fractional = 0; ///< wp_fractional_scale_v1.preferred_scale (0: never sent)
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wp_fractional_scale_v1* fs = nullptr;
    wp_viewport* viewport = nullptr;
    uint32_t serial = 0;
};

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    zxdg_output_manager_v1* xom = nullptr;
    wp_fractional_scale_manager_v1* fractional = nullptr;
    wp_viewporter* viewporter = nullptr;
    uint32_t output_name = 0;
    uint32_t output_version = 0;
};

void out_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                  const char*, int32_t) {}
void out_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void out_done(void* d, wl_output*) { ++static_cast<output_info*>(d)->dones; }
void out_scale(void* d, wl_output*, int32_t s) { static_cast<output_info*>(d)->scale = s; }
void out_name(void*, wl_output*, const char*) {}
void out_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {out_geometry, out_mode, out_done,
                                            out_scale,    out_name, out_description};

void xo_position(void*, zxdg_output_v1*, int32_t, int32_t) {}
void xo_size(void* d, zxdg_output_v1*, int32_t w, int32_t h) {
    static_cast<output_info*>(d)->lw = w;
    static_cast<output_info*>(d)->lh = h;
}
void xo_done(void*, zxdg_output_v1*) {}
void xo_name(void*, zxdg_output_v1*, const char*) {}
void xo_description(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener xdg_output_listener = {xo_position, xo_size, xo_done, xo_name,
                                                     xo_description};

void surf_enter(void*, wl_surface*, wl_output*) {}
void surf_leave(void*, wl_surface*, wl_output*) {}
void surf_scale(void* d, wl_surface*, int32_t s) { static_cast<surface_info*>(d)->buffer_scale = s; }
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
            wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, 3));
    else if (!std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name))
        c->fractional = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1));
    else if (!std::strcmp(iface, wp_viewporter_interface.name))
        c->viewporter = static_cast<wp_viewporter*>(
            wl_registry_bind(r, name, &wp_viewporter_interface, 1));
    else if (!std::strcmp(iface, wl_output_interface.name)) {
        c->output_name = name;
        c->output_version = version;
    }
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

void on_ping(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
const xdg_wm_base_listener wm_base_listener = {on_ping};

void on_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<surface_info*>(data)->serial = serial;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0 || !c.compositor || !c.wm_base || !c.shm ||
        !c.xom || !c.fractional || !c.viewporter || !c.output_name)
        return false;
    xdg_wm_base_add_listener(c.wm_base, &wm_base_listener, nullptr);
    return true;
}

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-render-scale", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

constexpr int k_w = 300;
constexpr int k_h = 200;

/// A toplevel; `fractional`: with wp_fractional_scale_v1 + a viewport (else it speaks only
/// the integer wl_surface v6 events). Mapped with a 1x buffer.
void open_window(client& c, surface_info& s, bool fractional) {
    s.surface = wl_compositor_create_surface(c.compositor);
    wl_surface_add_listener(s.surface, &surface_listener, &s);
    if (fractional) {
        s.fs = wp_fractional_scale_manager_v1_get_fractional_scale(c.fractional, s.surface);
        wp_fractional_scale_v1_add_listener(s.fs, &fractional_listener, &s);
        s.viewport = wp_viewporter_get_viewport(c.viewporter, s.surface);
    }
    s.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, s.surface);
    xdg_surface_add_listener(s.xdg, &xdg_listener, &s);
    s.toplevel = xdg_surface_get_toplevel(s.xdg);
    wl_surface_commit(s.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(s.xdg, s.serial);
    wl_surface_attach(s.surface, make_buffer(c, k_w, k_h), 0, 0);
    wl_surface_commit(s.surface);
    (void)wl_display_roundtrip(c.display);
}

/// Renders as a client following its hints does: the fractional one at fractional/120 behind
/// a viewport of its logical size, the integer one at its buffer scale.
void redraw(client& c, surface_info& s) {
    if (s.fs) {
        const double scale = (s.fractional ? s.fractional : 120) / 120.0;
        const int bw = static_cast<int>(std::lround(k_w * scale));
        const int bh = static_cast<int>(std::lround(k_h * scale));
        wp_viewport_set_destination(s.viewport, k_w, k_h);
        wl_surface_attach(s.surface, make_buffer(c, bw, bh), 0, 0);
    } else {
        const int scale = s.buffer_scale > 0 ? s.buffer_scale : 1;
        wl_surface_set_buffer_scale(s.surface, scale);
        wl_surface_attach(s.surface, make_buffer(c, k_w * scale, k_h * scale), 0, 0);
    }
    wl_surface_damage_buffer(s.surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(s.surface);
    (void)wl_display_roundtrip(c.display);
}

} // namespace

int main() {
    // The configuration file this compositor reads (and the test rewrites before a reload).
    char dir[] = "/tmp/lumen-render-scale-XXXXXX";
    if (!mkdtemp(dir)) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string config_path = std::string(dir) + "/config.toml";
    const auto write_config = [&](const char* text) {
        std::FILE* f = std::fopen(config_path.c_str(), "w");
        if (!f)
            return false;
        std::fputs(text, f);
        return std::fclose(f) == 0;
    };
    (void)write_config("# no [[output]]\n");
    setenv("LUMEN_CONFIG", config_path.c_str(), 1);

    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.virtual_outputs = "1280x720";
    lx::compositor::compositor comp{cfg};
    if (auto started = comp.start(); !started) {
        std::fprintf(stderr, "compositor start failed\n");
        return 1;
    }
    const std::string output_name = comp.outputs().state(comp.outputs().nth(0)).name;
    std::atomic<bool> pump{true};
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
            pollfd pf{loop_fd, POLLIN, 0};
            (void)poll(&pf, 1, 10);
        }
    });
    // The file changed: `lumen-ctl reload` / SIGHUP, run as the loop would.
    const auto reload = [&](const char* text) {
        (void)write_config(text);
        std::lock_guard<std::mutex> hold(dispatch_lock);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        comp.request_reload();
        comp.run_posted_ui_tasks();
        comp.wayland().flush();
    };
    const auto render_scale = [&] {
        std::lock_guard<std::mutex> hold(dispatch_lock);
        return comp.outputs().state(comp.outputs().nth(0)).effective_render_scale();
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client c{};
    if (!connect(c)) {
        check(false, "the compositor advertises wl_output, xdg-output, fractional scale and "
                     "viewporter");
    } else {
        output_info o{};
        o.output = static_cast<wl_output*>(wl_registry_bind(
            c.registry, c.output_name, &wl_output_interface,
            c.output_version < 4 ? c.output_version : 4));
        wl_output_add_listener(o.output, &output_listener, &o);
        o.xo = zxdg_output_manager_v1_get_xdg_output(c.xom, o.output);
        zxdg_output_v1_add_listener(o.xo, &xdg_output_listener, &o);
        (void)wl_display_roundtrip(c.display);

        surface_info frac{};
        surface_info integer{};
        open_window(c, frac, true);
        open_window(c, integer, false);
        check(frac.fractional == 120 && integer.buffer_scale == 0,
              "no render scale: fractional 120, no preferred_buffer_scale (the scale's)");
        const unsigned dones = o.dones;
        const int lw = o.lw, lh = o.lh;
        check(o.scale == 1 && lw == 1280 && lh == 720, "wl_output scale 1, logical 1280x720");

        // ── render-scale = 2 for every output ──
        reload("[[output]]\nname = \"*\"\nrender-scale = 2\n");
        (void)wl_display_roundtrip(c.display);
        check(render_scale() == 2.0, "the reload set the output's render scale to 2");
        check(frac.fractional == 240 && integer.buffer_scale == 2,
              "render-scale 2: fractional 240, preferred_buffer_scale 2");
        check(o.dones == dones && o.scale == 1 && o.lw == lw && o.lh == lh,
              "no wl_output event: scale 1 and the logical size stay");
        redraw(c, frac);
        redraw(c, integer);
        {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            bool sizes = true, geometry = true;
            unsigned windows = 0;
            comp.toplevels().for_each([&](const lx::compositor::toplevel_record& rec) {
                ++windows;
                const lx::size2i buf = comp.surfaces().buffer_size(rec.surface);
                const lx::size2i logical = comp.surfaces().surface_size(rec.surface);
                sizes = sizes && buf.width == 2 * k_w && buf.height == 2 * k_h &&
                        logical.width == k_w && logical.height == k_h;
                geometry = geometry && rec.geometry.width == k_w && rec.geometry.height == k_h;
            });
            check(windows == 2 && sizes,
                  "both clients' buffers are 600x400 (2x), their surfaces 300x200 (logical)");
            check(geometry, "window geometry unchanged: 300x200");
        }
        // A surface made now starts at the render scale (its first preferred_scale).
        surface_info late{};
        open_window(c, late, true);
        check(late.fractional == 240, "a new fractional surface starts at 240");

        // ── 1.5 by connector name: fractional 180, the integer hint its ceiling ──
        char text[256];
        std::snprintf(text, sizeof(text), "[[output]]\nname = \"%s\"\nrender-scale = 1.5\n",
                      output_name.c_str());
        reload(text);
        (void)wl_display_roundtrip(c.display);
        check(frac.fractional == 180 && integer.buffer_scale == 2 && late.fractional == 180,
              "render-scale 1.5 by connector: fractional 180, preferred_buffer_scale 2");
        redraw(c, frac);
        {
            std::lock_guard<std::mutex> hold(dispatch_lock);
            bool found = false;
            comp.toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
                const lx::size2i buf = comp.surfaces().buffer_size(r.surface);
                if (buf.width == 450 && buf.height == 300 &&
                    comp.surfaces().surface_size(r.surface).width == k_w)
                    found = true;
            });
            check(found, "the fractional client's 1.5x buffer is 450x300, its surface 300x200");
        }

        // ── 0.5: undersampling is honoured ──
        reload("[[output]]\nname = \"*\"\nrender-scale = 0.5\n");
        (void)wl_display_roundtrip(c.display);
        check(frac.fractional == 60 && integer.buffer_scale == 1,
              "render-scale 0.5: fractional 60, preferred_buffer_scale 1");

        // ── out of range: the entry is refused, the output scale is used ──
        reload("[[output]]\nname = \"*\"\nrender-scale = 9\n");
        (void)wl_display_roundtrip(c.display);
        check(frac.fractional == 120 && render_scale() == 1.0,
              "render-scale 9 (outside 0.25..4): refused, fractional back to 120");

        // ── back to 2, then the entry removed ──
        reload("[[output]]\nname = \"*\"\nrender-scale = 2\n");
        (void)wl_display_roundtrip(c.display);
        check(frac.fractional == 240, "render-scale 2 again: 240");
        reload("# no [[output]]\n");
        (void)wl_display_roundtrip(c.display);
        check(frac.fractional == 120 && integer.buffer_scale == 1 && render_scale() == 1.0,
              "entry removed: back to the scale's hints (120, 1)");
        check(o.dones == dones && o.scale == 1,
              "no wl_output event across every render scale change");
    }
    if (c.display)
        wl_display_disconnect(c.display);

    pump.store(false, std::memory_order_release);
    pump_thread.join();
    comp.request_stop();
    std::remove(config_path.c_str());
    ::rmdir(dir);
    std::printf("%d checks, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#endif
