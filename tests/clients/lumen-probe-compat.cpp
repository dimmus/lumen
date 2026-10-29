// lumen-probe timing | solid | tearing: deterministic clients of the commit-path protocols
// (design-app-compat-capture Part A, F.2). Separate from the rest of lumen-probe (own
// connection and window); main() dispatches here.
//
//   lumen-probe timing [--frames N] [--timed N] [--step MS] [--size WxH] [--exit-after S]
//       fifo-v1: N updates committed back to back with set_barrier + wait_barrier from a
//       pool of four buffers (what Mesa's FIFO swapchain does) — each must be presented, one
//       per refresh. Then commit-timing-v1: `--timed` updates, each with a target `--step`
//       ms ahead (the client commits early, the compositor holds the update). Prints
//         fifo frame=I presented=T            (or discarded)
//         timed frame=I target=T presented=P delta_ms=D
//         timing summary fifo_frames=N fifo_presented=P fifo_discarded=D dt_min_ms=…
//           dt_mean_ms=… refresh_ms=… timed_frames=M timed_presented=K early=E late=L
//           delta_min_ms=… delta_max_ms=…
//   lumen-probe solid [--rgba R,G,B,A] [--alpha F] [--size WxH] [--content-type T]
//       [--exit-after S]
//       A toplevel whose content is a wp_single_pixel_buffer_v1 colour (0..255 per channel,
//       premultiplied), sized by a wp_viewport destination, faded by wp_alpha_modifier_v1
//       (0..1), typed by wp_content_type_v1 (none|photo|video|game). Prints `solid mapped`.
//   lumen-probe tearing [--exit-after S]
//       A fullscreen single-pixel window with wp_tearing_control_v1 hint async. Prints
//       `tearing mapped hint=async`.
//   lumen-probe blur [--size WxH] [--region X,Y,W,H] [--exit-after S]
//       ext-background-effect-v1: a window of 4 px black and white vertical stripes with a
//       transparent subsurface over `--region` (default: the middle half) whose blur region
//       is all of it — what shows there is the stripes blurred, everything else sharp.
//       Prints `blur mapped caps=C size=WxH region=X,Y,W,H` (C: the manager's capabilities).

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_FIFO_V1) && defined(LUMEN_PROTOCOL_COMMIT_TIMING_V1) && \
    defined(LUMEN_PROTOCOL_PRESENTATION_TIME) && defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1) && \
    defined(LUMEN_PROTOCOL_ALPHA_MODIFIER_V1) && defined(LUMEN_PROTOCOL_CONTENT_TYPE_V1) && \
    defined(LUMEN_PROTOCOL_TEARING_CONTROL_V1) && defined(LUMEN_PROTOCOL_VIEWPORTER)
#include "alpha-modifier-v1-client-protocol.h"
#include "commit-timing-v1-client-protocol.h"
#include "content-type-v1-client-protocol.h"
#include "fifo-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#define PROBE_HAS_COMPAT 1
#endif
#if defined(PROBE_HAS_COMPAT) && defined(LUMEN_PROTOCOL_EXT_BACKGROUND_EFFECT_V1)
#include "ext-background-effect-v1-client-protocol.h"
#define PROBE_HAS_BLUR 1
#endif

int compat_main(int argc, char** argv);

#if !defined(PROBE_HAS_COMPAT)

int compat_main(int, char**) {
    std::fprintf(stderr, "lumen-probe: built without the commit-path protocol glue\n");
    return 2;
}

#else

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct globals {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm = nullptr;
    wp_fifo_manager_v1* fifo = nullptr;
    wp_commit_timing_manager_v1* timing = nullptr;
    wp_presentation* presentation = nullptr;
    wp_single_pixel_buffer_manager_v1* spb = nullptr;
    wp_alpha_modifier_v1* alpha = nullptr;
    wp_content_type_manager_v1* content = nullptr;
    wp_tearing_control_manager_v1* tearing = nullptr;
    wp_viewporter* viewporter = nullptr;
    wl_subcompositor* subcompositor = nullptr;
#if defined(PROBE_HAS_BLUR)
    ext_background_effect_manager_v1* effect = nullptr;
    uint32_t effect_caps = 0;
#endif
    uint32_t clock_id = ~0u;
} g;

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

template <class T>
T* bind(wl_registry* r, uint32_t name, const wl_interface* i, uint32_t v) {
    return static_cast<T*>(wl_registry_bind(r, name, i, v));
}

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, wl_compositor_interface.name))
        g.compositor = bind<wl_compositor>(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!std::strcmp(iface, wl_shm_interface.name))
        g.shm = bind<wl_shm>(r, name, &wl_shm_interface, 1);
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        g.wm = bind<xdg_wm_base>(r, name, &xdg_wm_base_interface, 1);
    else if (!std::strcmp(iface, wp_fifo_manager_v1_interface.name))
        g.fifo = bind<wp_fifo_manager_v1>(r, name, &wp_fifo_manager_v1_interface, 1);
    else if (!std::strcmp(iface, wp_commit_timing_manager_v1_interface.name))
        g.timing = bind<wp_commit_timing_manager_v1>(r, name, &wp_commit_timing_manager_v1_interface, 1);
    else if (!std::strcmp(iface, wp_presentation_interface.name)) {
        static const wp_presentation_listener l = {
            [](void*, wp_presentation*, uint32_t clk) { g.clock_id = clk; }};
        g.presentation = bind<wp_presentation>(r, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener(g.presentation, &l, nullptr);
    } else if (!std::strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name))
        g.spb = bind<wp_single_pixel_buffer_manager_v1>(
            r, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    else if (!std::strcmp(iface, wp_alpha_modifier_v1_interface.name))
        g.alpha = bind<wp_alpha_modifier_v1>(r, name, &wp_alpha_modifier_v1_interface, 1);
    else if (!std::strcmp(iface, wp_content_type_manager_v1_interface.name))
        g.content = bind<wp_content_type_manager_v1>(r, name, &wp_content_type_manager_v1_interface, 1);
    else if (!std::strcmp(iface, wp_tearing_control_manager_v1_interface.name))
        g.tearing = bind<wp_tearing_control_manager_v1>(
            r, name, &wp_tearing_control_manager_v1_interface, 1);
    else if (!std::strcmp(iface, wp_viewporter_interface.name))
        g.viewporter = bind<wp_viewporter>(r, name, &wp_viewporter_interface, 1);
    else if (!std::strcmp(iface, wl_subcompositor_interface.name))
        g.subcompositor = bind<wl_subcompositor>(r, name, &wl_subcompositor_interface, 1);
#if defined(PROBE_HAS_BLUR)
    else if (!std::strcmp(iface, ext_background_effect_manager_v1_interface.name)) {
        static const ext_background_effect_manager_v1_listener l = {
            [](void*, ext_background_effect_manager_v1*, uint32_t caps) { g.effect_caps = caps; }};
        g.effect = bind<ext_background_effect_manager_v1>(
            r, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(g.effect, &l, nullptr);
    }
#endif
}

const wl_registry_listener registry_listener = {on_global,
                                                [](void*, wl_registry*, uint32_t) {}};

const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

/// Dispatches with a timeout (ms); false on a connection error.
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

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* top = nullptr;
    uint32_t serial = 0;
    bool configured = false;
    int width = 0;
    int height = 0;
};

window g_win{};

void make_window(const char* title, int w, int h, bool fullscreen) {
    static const xdg_surface_listener xl = {[](void* d, xdg_surface*, uint32_t serial) {
        auto* win = static_cast<window*>(d);
        win->serial = serial;
        win->configured = true;
    }};
    static const xdg_toplevel_listener tl = {
        [](void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
            auto* win = static_cast<window*>(d);
            if (w > 0 && h > 0) {
                win->width = w;
                win->height = h;
            }
        },
        [](void*, xdg_toplevel*) { g_stop = 1; },
        [](void*, xdg_toplevel*, int32_t, int32_t) {},
        [](void*, xdg_toplevel*, wl_array*) {}};
    g_win.width = w;
    g_win.height = h;
    g_win.surface = wl_compositor_create_surface(g.compositor);
    g_win.xdg = xdg_wm_base_get_xdg_surface(g.wm, g_win.surface);
    xdg_surface_add_listener(g_win.xdg, &xl, &g_win);
    g_win.top = xdg_surface_get_toplevel(g_win.xdg);
    xdg_toplevel_add_listener(g_win.top, &tl, &g_win);
    xdg_toplevel_set_title(g_win.top, title);
    xdg_toplevel_set_app_id(g_win.top, "lumen-probe");
    if (fullscreen)
        xdg_toplevel_set_fullscreen(g_win.top, nullptr);
    wl_surface_commit(g_win.surface);
    while (!g_win.configured && !g_stop)
        if (!pump(1000))
            break;
    xdg_surface_ack_configure(g_win.xdg, g_win.serial);
}

// ── shm buffers with release tracking ──────────────────────────────────────

struct shm_buffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    bool busy = false;
};

void make_buffers(shm_buffer* out, unsigned n, int w, int h) {
    static const wl_buffer_listener bl = {
        [](void* d, wl_buffer*) { static_cast<shm_buffer*>(d)->busy = false; }};
    const size_t stride = static_cast<size_t>(w) * 4u;
    const size_t one = stride * static_cast<size_t>(h);
    const int fd = memfd_create("lumen-probe-compat", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(one * n)) != 0)
        return;
    void* mem = mmap(nullptr, one * n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(one * n));
    for (unsigned i = 0; i < n; ++i) {
        out[i].buffer = wl_shm_pool_create_buffer(pool, static_cast<int>(one * i), w, h,
                                                  static_cast<int>(stride), WL_SHM_FORMAT_XRGB8888);
        out[i].pixels = reinterpret_cast<uint32_t*>(static_cast<char*>(mem) + one * i);
        wl_buffer_add_listener(out[i].buffer, &bl, &out[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
}

void fill(shm_buffer& b, int w, int h, uint32_t colour) {
    for (int i = 0; i < w * h; ++i)
        b.pixels[i] = colour;
}

// ── presentation feedback ──────────────────────────────────────────────────

struct feedback {
    int frame = 0;
    bool done = false;
    bool presented = false;
    double t = 0.0;
    unsigned refresh_ns = 0;
};

struct wp_presentation_feedback* request_feedback(feedback* f) {
    static const wp_presentation_feedback_listener fl = {
        [](void*, struct wp_presentation_feedback*, wl_output*) {},
        [](void* d, struct wp_presentation_feedback* fb, uint32_t hi, uint32_t lo, uint32_t nsec,
           uint32_t refresh, uint32_t, uint32_t, uint32_t) {
            auto* f = static_cast<feedback*>(d);
            f->done = true;
            f->presented = true;
            f->t = static_cast<double>((static_cast<uint64_t>(hi) << 32) | lo) + nsec / 1e9;
            f->refresh_ns = refresh;
            wp_presentation_feedback_destroy(fb);
        },
        [](void* d, struct wp_presentation_feedback* fb) {
            static_cast<feedback*>(d)->done = true;
            wp_presentation_feedback_destroy(fb);
        }};
    struct wp_presentation_feedback* fb = wp_presentation_feedback(g.presentation, g_win.surface);
    wp_presentation_feedback_add_listener(fb, &fl, f);
    return fb;
}

struct options {
    int region[4] = {-1, -1, -1, -1};
    int frames = 120;
    int timed = 20;
    double step_ms = 55.0;
    int w = 320;
    int h = 240;
    double exit_after = 30.0;
    unsigned rgba[4] = {0, 255, 0, 255};
    double alpha = 1.0;
    unsigned content_type = WP_CONTENT_TYPE_V1_TYPE_NONE;
};

int run_timing(const options& o) {
    if (!g.fifo || !g.timing || !g.presentation) {
        std::printf("timing missing globals fifo=%d timing=%d presentation=%d\n", g.fifo != nullptr,
                    g.timing != nullptr, g.presentation != nullptr);
        return 1;
    }
    make_window("lumen-probe-timing", o.w, o.h, false);
    const int w = g_win.width, h = g_win.height;
    shm_buffer bufs[4]{};
    make_buffers(bufs, 4, w, h);
    wp_fifo_v1* fifo = wp_fifo_manager_v1_get_fifo(g.fifo, g_win.surface);
    wp_commit_timer_v1* timer = wp_commit_timing_manager_v1_get_timer(g.timing, g_win.surface);

    // Map with a first frame and let it show.
    fill(bufs[0], w, h, 0xFF404040u);
    bufs[0].busy = true;
    wl_surface_attach(g_win.surface, bufs[0].buffer, 0, 0);
    wl_surface_damage_buffer(g_win.surface, 0, 0, w, h);
    wl_surface_commit(g_win.surface);
    const double settle = now_s() + 0.5;
    while (now_s() < settle && !g_stop)
        pump(50);

    // Phase 1 — FIFO: commit as soon as a buffer is free; never wait for a frame callback.
    std::vector<feedback> fifo_fb(static_cast<size_t>(o.frames));
    for (int i = 0; i < o.frames && !g_stop; ++i) {
        shm_buffer* b = nullptr;
        const double deadline = now_s() + 2.0;
        while (!b && !g_stop && now_s() < deadline) {
            for (auto& c : bufs)
                if (!c.busy) {
                    b = &c;
                    break;
                }
            if (!b)
                pump(20);
        }
        if (!b) {
            std::printf("timing stalled frame=%d (no buffer released in 2 s)\n", i);
            break;
        }
        fill(*b, w, h, (i & 1) ? 0xFF2060A0u : 0xFFA06020u);
        b->busy = true;
        fifo_fb[static_cast<size_t>(i)].frame = i;
        request_feedback(&fifo_fb[static_cast<size_t>(i)]);
        wp_fifo_v1_set_barrier(fifo);
        wp_fifo_v1_wait_barrier(fifo);
        wl_surface_attach(g_win.surface, b->buffer, 0, 0);
        wl_surface_damage_buffer(g_win.surface, 0, 0, w, h);
        wl_surface_commit(g_win.surface);
        wl_display_flush(g.display);
        pump(0);
    }
    {
        const double deadline = now_s() + 3.0;
        auto all_done = [&] {
            for (const auto& f : fifo_fb)
                if (!f.done)
                    return false;
            return true;
        };
        while (!all_done() && now_s() < deadline && !g_stop)
            pump(50);
    }
    int presented = 0, discarded = 0;
    double dt_min = 1e9, dt_sum = 0.0, last = 0.0;
    int dts = 0;
    unsigned refresh_ns = 0;
    for (const auto& f : fifo_fb) {
        if (!f.done)
            continue;
        if (!f.presented) {
            ++discarded;
            std::printf("fifo frame=%d discarded\n", f.frame);
            continue;
        }
        ++presented;
        refresh_ns = f.refresh_ns ? f.refresh_ns : refresh_ns;
        std::printf("fifo frame=%d presented=%.6f\n", f.frame, f.t);
        if (last > 0.0) {
            const double dt = f.t - last;
            dt_min = dt < dt_min ? dt : dt_min;
            dt_sum += dt;
            ++dts;
        }
        last = f.t;
    }

    // Phase 2 — commit timing: each update targets `step` ahead and is committed at once.
    std::vector<feedback> timed_fb(static_cast<size_t>(o.timed));
    std::vector<double> targets(static_cast<size_t>(o.timed));
    int early = 0, late = 0, timed_presented = 0;
    double dmin = 1e9, dmax = -1e9;
    const double period = refresh_ns ? refresh_ns / 1e9 : 1.0 / 60.0;
    for (int i = 0; i < o.timed && !g_stop; ++i) {
        shm_buffer* b = nullptr;
        const double deadline = now_s() + 2.0;
        while (!b && !g_stop && now_s() < deadline) {
            for (auto& c : bufs)
                if (!c.busy) {
                    b = &c;
                    break;
                }
            if (!b)
                pump(20);
        }
        if (!b)
            break;
        fill(*b, w, h, (i & 1) ? 0xFF20A060u : 0xFF6020A0u);
        b->busy = true;
        const double target = now_s() + o.step_ms / 1000.0;
        targets[static_cast<size_t>(i)] = target;
        const auto sec = static_cast<uint64_t>(target);
        const auto nsec = static_cast<uint32_t>((target - static_cast<double>(sec)) * 1e9);
        timed_fb[static_cast<size_t>(i)].frame = i;
        request_feedback(&timed_fb[static_cast<size_t>(i)]);
        wp_commit_timer_v1_set_timestamp(timer, static_cast<uint32_t>(sec >> 32),
                                         static_cast<uint32_t>(sec), nsec);
        wl_surface_attach(g_win.surface, b->buffer, 0, 0);
        wl_surface_damage_buffer(g_win.surface, 0, 0, w, h);
        wl_surface_commit(g_win.surface);
        const double wait_until = now_s() + 2.0;
        while (!timed_fb[static_cast<size_t>(i)].done && now_s() < wait_until && !g_stop)
            pump(20);
        const feedback& f = timed_fb[static_cast<size_t>(i)];
        if (!f.presented) {
            std::printf("timed frame=%d target=%.6f discarded\n", i, target);
            continue;
        }
        ++timed_presented;
        const double delta = f.t - target;
        dmin = delta < dmin ? delta : dmin;
        dmax = delta > dmax ? delta : dmax;
        // Never a refresh early (0.5 ms of jitter), at most a refresh (+1 ms) late.
        if (delta < -0.0005)
            ++early;
        if (delta > period + 0.001)
            ++late;
        std::printf("timed frame=%d target=%.6f presented=%.6f delta_ms=%.3f\n", i, target, f.t,
                    delta * 1e3);
    }
    std::printf("timing summary fifo_frames=%d fifo_presented=%d fifo_discarded=%d "
                "dt_min_ms=%.3f dt_mean_ms=%.3f refresh_ms=%.3f timed_frames=%d timed_presented=%d "
                "early=%d late=%d delta_min_ms=%.3f delta_max_ms=%.3f clock_id=%u\n",
                o.frames, presented, discarded, dts ? dt_min * 1e3 : 0.0,
                dts ? dt_sum / dts * 1e3 : 0.0, period * 1e3, o.timed, timed_presented, early,
                late, timed_presented ? dmin * 1e3 : 0.0, timed_presented ? dmax * 1e3 : 0.0,
                g.clock_id);
    const double end = now_s() + 0.2;
    while (now_s() < end && !g_stop)
        pump(50);
    return 0;
}

int run_solid(const options& o, bool tearing_mode) {
    if (!g.spb || !g.viewporter || (!tearing_mode && (!g.alpha || !g.content)) ||
        (tearing_mode && !g.tearing)) {
        std::printf("solid missing globals\n");
        return 1;
    }
    make_window(tearing_mode ? "lumen-probe-tearing" : "lumen-probe-solid", o.w, o.h,
                tearing_mode);
    wp_viewport* vp = wp_viewporter_get_viewport(g.viewporter, g_win.surface);
    wp_viewport_set_destination(vp, g_win.width, g_win.height);
    if (tearing_mode) {
        wp_tearing_control_v1* t =
            wp_tearing_control_manager_v1_get_tearing_control(g.tearing, g_win.surface);
        wp_tearing_control_v1_set_presentation_hint(t, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    } else {
        wp_alpha_modifier_surface_v1* a = wp_alpha_modifier_v1_get_surface(g.alpha, g_win.surface);
        const double clamped = o.alpha < 0.0 ? 0.0 : (o.alpha > 1.0 ? 1.0 : o.alpha);
        wp_alpha_modifier_surface_v1_set_multiplier(
            a, static_cast<uint32_t>(std::llround(clamped * 4294967295.0)));
        wp_content_type_v1* ct =
            wp_content_type_manager_v1_get_surface_content_type(g.content, g_win.surface);
        wp_content_type_v1_set_content_type(ct, o.content_type);
    }
    auto scale = [](unsigned v) { return static_cast<uint32_t>((v > 255 ? 255 : v) * 0x01010101u); };
    wl_buffer* px = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        g.spb, scale(o.rgba[0]), scale(o.rgba[1]), scale(o.rgba[2]), scale(o.rgba[3]));
    wl_surface_attach(g_win.surface, px, 0, 0);
    wl_surface_damage_buffer(g_win.surface, 0, 0, 1, 1);
    wl_surface_commit(g_win.surface);
    wl_display_roundtrip(g.display);
    if (tearing_mode)
        std::printf("tearing mapped hint=async size=%dx%d\n", g_win.width, g_win.height);
    else
        std::printf("solid mapped rgba=%u,%u,%u,%u alpha=%.3f size=%dx%d\n", o.rgba[0], o.rgba[1],
                    o.rgba[2], o.rgba[3], o.alpha, g_win.width, g_win.height);
    const double end = now_s() + o.exit_after;
    while (!g_stop && now_s() < end)
        if (!pump(100))
            break;
    return 0;
}

#if defined(PROBE_HAS_BLUR)
/// One shm buffer of `w` × `h` in `format`, filled by `px(x, y)`.
template <typename Fn>
wl_buffer* shm_image(int w, int h, uint32_t format, Fn px) {
    const size_t stride = static_cast<size_t>(w) * 4u;
    const size_t size = stride * static_cast<size_t>(h);
    const int fd = memfd_create("lumen-probe-blur", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return nullptr;
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    auto* p = static_cast<uint32_t*>(mem);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = px(x, y);
    munmap(mem, size);
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(size));
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, static_cast<int>(stride), format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

int run_blur(const options& o) {
    if (!g.effect || !g.subcompositor) {
        std::printf("blur missing globals\n");
        return 1;
    }
    make_window("lumen-probe-blur", o.w, o.h, false);
    const int w = g_win.width;
    const int h = g_win.height;
    int rx = o.region[0], ry = o.region[1], rw = o.region[2], rh = o.region[3];
    if (rw <= 0 || rh <= 0) {
        rx = w / 4;
        ry = h / 4;
        rw = w / 2;
        rh = h / 2;
    }
    wl_surface* child = wl_compositor_create_surface(g.compositor);
    wl_subsurface* sub = wl_subcompositor_get_subsurface(g.subcompositor, child, g_win.surface);
    wl_subsurface_set_position(sub, rx, ry);
    ext_background_effect_surface_v1* effect =
        ext_background_effect_manager_v1_get_background_effect(g.effect, child);
    wl_region* region = wl_compositor_create_region(g.compositor);
    wl_region_add(region, 0, 0, rw, rh);
    ext_background_effect_surface_v1_set_blur_region(effect, region);
    wl_region_destroy(region);
    wl_buffer* clear = shm_image(rw, rh, WL_SHM_FORMAT_ARGB8888, [](int, int) { return 0u; });
    wl_surface_attach(child, clear, 0, 0);
    wl_surface_damage_buffer(child, 0, 0, rw, rh);
    wl_surface_commit(child); // synchronized: applied with the parent's commit
    wl_buffer* stripes = shm_image(w, h, WL_SHM_FORMAT_XRGB8888, [](int x, int) {
        return (x / 4) % 2 ? 0xFFFFFFFFu : 0xFF000000u;
    });
    wl_surface_attach(g_win.surface, stripes, 0, 0);
    wl_surface_damage_buffer(g_win.surface, 0, 0, w, h);
    wl_surface_commit(g_win.surface);
    wl_display_roundtrip(g.display);
    std::printf("blur mapped caps=%u size=%dx%d region=%d,%d,%d,%d\n", g.effect_caps, w, h, rx,
                ry, rw, rh);
    std::fflush(stdout);
    const double end = now_s() + o.exit_after;
    while (!g_stop && now_s() < end)
        if (!pump(100))
            break;
    return 0;
}
#endif

} // namespace

int compat_main(int argc, char** argv) {
    options o{};
    const char* mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--frames") && v) { o.frames = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--timed") && v) { o.timed = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--step") && v) { o.step_ms = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--size") && v && std::sscanf(v, "%dx%d", &o.w, &o.h) == 2) ++i;
        else if (!std::strcmp(a, "--exit-after") && v) { o.exit_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--region") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.region[0], &o.region[1], &o.region[2],
                             &o.region[3]) == 4)
            ++i;
        else if (!std::strcmp(a, "--alpha") && v) { o.alpha = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--rgba") && v &&
                 std::sscanf(v, "%u,%u,%u,%u", &o.rgba[0], &o.rgba[1], &o.rgba[2], &o.rgba[3]) == 4)
            ++i;
        else if (!std::strcmp(a, "--content-type") && v) {
            o.content_type = !std::strcmp(v, "photo")   ? WP_CONTENT_TYPE_V1_TYPE_PHOTO
                             : !std::strcmp(v, "video") ? WP_CONTENT_TYPE_V1_TYPE_VIDEO
                             : !std::strcmp(v, "game")  ? WP_CONTENT_TYPE_V1_TYPE_GAME
                                                        : WP_CONTENT_TYPE_V1_TYPE_NONE;
            ++i;
        } else {
            std::fprintf(stderr, "lumen-probe %s: unknown option %s\n", mode, a);
            return 2;
        }
    }
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect\n");
        return 1;
    }
    wl_registry* r = wl_display_get_registry(g.display);
    wl_registry_add_listener(r, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.wm) {
        std::fprintf(stderr, "lumen-probe: core globals missing\n");
        return 1;
    }
    xdg_wm_base_add_listener(g.wm, &wm_listener, nullptr);
    int rc = 2;
    if (!std::strcmp(mode, "timing"))
        rc = run_timing(o);
    else if (!std::strcmp(mode, "solid"))
        rc = run_solid(o, false);
    else if (!std::strcmp(mode, "tearing"))
        rc = run_solid(o, true);
#if defined(PROBE_HAS_BLUR)
    else if (!std::strcmp(mode, "blur"))
        rc = run_blur(o);
#endif
    if (wl_display_get_error(g.display))
        std::printf("protocol error %d\n", wl_display_get_error(g.display));
    wl_display_disconnect(g.display);
    return rc;
}

#endif
