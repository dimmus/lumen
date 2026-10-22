// lumen-probe constraints | gestures: deterministic clients of the pointer protocols
// (design-app-compat-capture Part C, F.2). Own connection; main() dispatches here.
//
//   lumen-probe constraints [--confine] [--oneshot] [--region X,Y,W,H] [--hint X,Y]
//       [--size WxH] [--destroy-after S] [--exit-after S]
//       A window (default 480x320) that asks for a pointer lock (or with --confine a
//       confinement), persistent unless --oneshot, with an optional region and (lock) cursor
//       hint, and a relative pointer. Prints `constraints mapped WxH`, then on every event:
//       `enter X,Y`, `leave`, `motion X,Y` (surface-local), `locked` / `unlocked` /
//       `confined` / `unconfined`, `relative DX,DY unaccel DXU,DYU t=US`, `focus in` /
//       `focus out` (keyboard). --destroy-after destroys the constraint after S seconds
//       (`constraint destroyed`): a lock with a hint then warps the pointer there.
//   lumen-probe gestures [--exit-after S]
//       A window with swipe, pinch and hold objects (v3). Prints `gestures bound version=N`
//       and each event: `swipe begin fingers=N`, `swipe update DX,DY`, `swipe end
//       cancelled=C`, likewise pinch (with scale and rotation) and hold.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_POINTER_CONSTRAINTS_UNSTABLE_V1) && \
    defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1) &&     \
    defined(LUMEN_PROTOCOL_POINTER_GESTURES_UNSTABLE_V1)
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#define PROBE_HAS_POINTER 1
#endif

int pointer_main(int argc, char** argv);

#if !defined(PROBE_HAS_POINTER)

int pointer_main(int, char**) {
    std::fprintf(stderr, "lumen-probe: built without the pointer protocol glue\n");
    return 2;
}

#else

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct globals {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_relative_pointer_manager_v1* relative = nullptr;
    zwp_pointer_gestures_v1* gestures = nullptr;
    uint32_t gestures_version = 0;
} g;

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, wl_compositor_interface.name))
        g.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (!std::strcmp(iface, wl_shm_interface.name))
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (!std::strcmp(iface, wl_seat_interface.name))
        g.seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        g.wm = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
    else if (!std::strcmp(iface, zwp_pointer_constraints_v1_interface.name))
        g.constraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1));
    else if (!std::strcmp(iface, zwp_relative_pointer_manager_v1_interface.name))
        g.relative = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1));
    else if (!std::strcmp(iface, zwp_pointer_gestures_v1_interface.name)) {
        g.gestures_version = version < 3 ? version : 3;
        g.gestures = static_cast<zwp_pointer_gestures_v1*>(
            wl_registry_bind(r, name, &zwp_pointer_gestures_v1_interface, g.gestures_version));
    }
}
const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};
const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

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

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

const char* opt(int argc, char** argv, const char* name, const char* fallback = nullptr) {
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name))
            return argv[i + 1];
    return fallback;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], name))
            return true;
    return false;
}

wl_buffer* solid_buffer(int w, int h, uint32_t argb) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-probe-pointer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool edge = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
                px[y * w + x] = edge ? 0xFF202020u : argb;
            }
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* top = nullptr;
    uint32_t serial = 0;
    bool configured = false;
} w;

bool map_window(int width, int height, uint32_t colour, const char* title) {
    static const xdg_surface_listener xl = {[](void*, xdg_surface*, uint32_t serial) {
        w.serial = serial;
        w.configured = true;
    }};
    static const xdg_toplevel_listener tl = {
        [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
        [](void*, xdg_toplevel*) { g_stop = 1; },
        [](void*, xdg_toplevel*, int32_t, int32_t) {},
        [](void*, xdg_toplevel*, wl_array*) {}};
    w.surface = wl_compositor_create_surface(g.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(g.wm, w.surface);
    xdg_surface_add_listener(w.xdg, &xl, nullptr);
    w.top = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.top, &tl, nullptr);
    xdg_toplevel_set_title(w.top, title);
    xdg_toplevel_set_app_id(w.top, "lumen-probe");
    wl_surface_commit(w.surface);
    while (!w.configured && !g_stop)
        if (!pump(1000))
            return false;
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, solid_buffer(width, height, colour), 0, 0);
    wl_surface_damage_buffer(w.surface, 0, 0, width, height);
    wl_surface_commit(w.surface);
    return wl_display_roundtrip(g.display) >= 0;
}

void watch_pointer_and_keyboard(wl_pointer* pointer) {
    static const wl_pointer_listener pl = {
        [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
            std::printf("enter %.2f,%.2f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
        },
        [](void*, wl_pointer*, uint32_t, wl_surface*) { std::printf("leave\n"); },
        [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            std::printf("motion %.2f,%.2f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
        },
        [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
            std::printf("button %u %s\n", button, state ? "pressed" : "released");
        },
        [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
        [](void*, wl_pointer*) {},
        [](void*, wl_pointer*, uint32_t) {},
        [](void*, wl_pointer*, uint32_t, uint32_t) {},
        [](void*, wl_pointer*, uint32_t, int32_t) {},
        nullptr,
        nullptr};
    static const wl_keyboard_listener kl = {
        [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
        [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { std::printf("focus in\n"); },
        [](void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("focus out\n"); },
        [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void*, wl_keyboard*, int32_t, int32_t) {}};
    wl_pointer_add_listener(pointer, &pl, nullptr);
    wl_keyboard_add_listener(wl_seat_get_keyboard(g.seat), &kl, nullptr);
}

int run(double exit_after, double destroy_after, void (*destroy)()) {
    const double start = now_s();
    bool destroyed = false;
    while (!g_stop) {
        if (!pump(50)) {
            std::fprintf(stderr, "lumen-probe: connection error\n");
            return 1;
        }
        const double t = now_s() - start;
        if (destroy && !destroyed && destroy_after > 0 && t >= destroy_after) {
            destroy();
            destroyed = true;
            std::printf("constraint destroyed\n");
        }
        if (exit_after > 0 && t >= exit_after)
            break;
    }
    return 0;
}

zwp_locked_pointer_v1* g_lock = nullptr;
zwp_confined_pointer_v1* g_confine = nullptr;

int mode_constraints(int argc, char** argv) {
    if (!g.constraints || !g.relative || !g.seat) {
        std::printf("constraints unsupported\n");
        return 1;
    }
    int width = 480, height = 320;
    if (const char* s = opt(argc, argv, "--size"))
        (void)std::sscanf(s, "%dx%d", &width, &height);
    const bool confine = flag(argc, argv, "--confine");
    const uint32_t lifetime = flag(argc, argv, "--oneshot") ? ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
                                                            : ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
    wl_pointer* pointer = wl_seat_get_pointer(g.seat);
    watch_pointer_and_keyboard(pointer);
    static const zwp_relative_pointer_v1_listener rl = {
        [](void*, zwp_relative_pointer_v1*, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
           wl_fixed_t dxu, wl_fixed_t dyu) {
            std::printf("relative %.2f,%.2f unaccel %.2f,%.2f t=%llu\n", wl_fixed_to_double(dx),
                        wl_fixed_to_double(dy), wl_fixed_to_double(dxu), wl_fixed_to_double(dyu),
                        (static_cast<unsigned long long>(hi) << 32) | lo);
        }};
    zwp_relative_pointer_v1_add_listener(
        zwp_relative_pointer_manager_v1_get_relative_pointer(g.relative, pointer), &rl, nullptr);
    if (!map_window(width, height, confine ? 0xFF2E7D32u : 0xFF1565C0u,
                    confine ? "probe confine" : "probe lock"))
        return 1;
    std::printf("constraints mapped %dx%d\n", width, height);
    wl_region* region = nullptr;
    if (const char* s = opt(argc, argv, "--region")) {
        int x = 0, y = 0, rw = 0, rh = 0;
        if (std::sscanf(s, "%d,%d,%d,%d", &x, &y, &rw, &rh) == 4) {
            region = wl_compositor_create_region(g.compositor);
            wl_region_add(region, x, y, rw, rh);
        }
    }
    if (confine) {
        static const zwp_confined_pointer_v1_listener cl = {
            [](void*, zwp_confined_pointer_v1*) { std::printf("confined\n"); },
            [](void*, zwp_confined_pointer_v1*) { std::printf("unconfined\n"); }};
        g_confine = zwp_pointer_constraints_v1_confine_pointer(g.constraints, w.surface, pointer,
                                                               region, lifetime);
        zwp_confined_pointer_v1_add_listener(g_confine, &cl, nullptr);
    } else {
        static const zwp_locked_pointer_v1_listener ll = {
            [](void*, zwp_locked_pointer_v1*) { std::printf("locked\n"); },
            [](void*, zwp_locked_pointer_v1*) { std::printf("unlocked\n"); }};
        g_lock = zwp_pointer_constraints_v1_lock_pointer(g.constraints, w.surface, pointer, region,
                                                         lifetime);
        zwp_locked_pointer_v1_add_listener(g_lock, &ll, nullptr);
        if (const char* s = opt(argc, argv, "--hint")) {
            double hx = 0, hy = 0;
            if (std::sscanf(s, "%lf,%lf", &hx, &hy) == 2) {
                zwp_locked_pointer_v1_set_cursor_position_hint(g_lock, wl_fixed_from_double(hx),
                                                               wl_fixed_from_double(hy));
                wl_surface_commit(w.surface);
            }
        }
    }
    if (region)
        wl_region_destroy(region);
    std::printf("constraint requested %s %s\n", confine ? "confine" : "lock",
                lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT ? "oneshot" : "persistent");
    return run(std::atof(opt(argc, argv, "--exit-after", "0")),
               std::atof(opt(argc, argv, "--destroy-after", "0")), [] {
                   if (g_lock)
                       zwp_locked_pointer_v1_destroy(g_lock);
                   if (g_confine)
                       zwp_confined_pointer_v1_destroy(g_confine);
                   g_lock = nullptr;
                   g_confine = nullptr;
               });
}

int mode_gestures(int argc, char** argv) {
    if (!g.gestures || !g.seat) {
        std::printf("gestures unsupported\n");
        return 1;
    }
    wl_pointer* pointer = wl_seat_get_pointer(g.seat);
    watch_pointer_and_keyboard(pointer);
    static const zwp_pointer_gesture_swipe_v1_listener sl = {
        [](void*, zwp_pointer_gesture_swipe_v1*, uint32_t, uint32_t, wl_surface*, uint32_t f) {
            std::printf("swipe begin fingers=%u\n", f);
        },
        [](void*, zwp_pointer_gesture_swipe_v1*, uint32_t, wl_fixed_t dx, wl_fixed_t dy) {
            std::printf("swipe update %.2f,%.2f\n", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
        },
        [](void*, zwp_pointer_gesture_swipe_v1*, uint32_t, uint32_t, int32_t c) {
            std::printf("swipe end cancelled=%d\n", c);
        }};
    static const zwp_pointer_gesture_pinch_v1_listener pl = {
        [](void*, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, wl_surface*, uint32_t f) {
            std::printf("pinch begin fingers=%u\n", f);
        },
        [](void*, zwp_pointer_gesture_pinch_v1*, uint32_t, wl_fixed_t dx, wl_fixed_t dy,
           wl_fixed_t scale, wl_fixed_t rot) {
            std::printf("pinch update %.2f,%.2f scale=%.3f rotation=%.2f\n", wl_fixed_to_double(dx),
                        wl_fixed_to_double(dy), wl_fixed_to_double(scale), wl_fixed_to_double(rot));
        },
        [](void*, zwp_pointer_gesture_pinch_v1*, uint32_t, uint32_t, int32_t c) {
            std::printf("pinch end cancelled=%d\n", c);
        }};
    zwp_pointer_gesture_swipe_v1_add_listener(zwp_pointer_gestures_v1_get_swipe_gesture(g.gestures, pointer),
                                              &sl, nullptr);
    zwp_pointer_gesture_pinch_v1_add_listener(zwp_pointer_gestures_v1_get_pinch_gesture(g.gestures, pointer),
                                              &pl, nullptr);
    if (g.gestures_version >= 3) {
        static const zwp_pointer_gesture_hold_v1_listener hl = {
            [](void*, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t, wl_surface*, uint32_t f) {
                std::printf("hold begin fingers=%u\n", f);
            },
            [](void*, zwp_pointer_gesture_hold_v1*, uint32_t, uint32_t, int32_t c) {
                std::printf("hold end cancelled=%d\n", c);
            }};
        zwp_pointer_gesture_hold_v1_add_listener(zwp_pointer_gestures_v1_get_hold_gesture(g.gestures, pointer),
                                                 &hl, nullptr);
    }
    if (!map_window(400, 260, 0xFF6A1B9Au, "probe gestures"))
        return 1;
    std::printf("gestures bound version=%u\n", g.gestures_version);
    return run(std::atof(opt(argc, argv, "--exit-after", "0")), 0, nullptr);
}

} // namespace

int pointer_main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect\n");
        return 1;
    }
    wl_registry* r = wl_display_get_registry(g.display);
    wl_registry_add_listener(r, &registry_listener, nullptr);
    if (wl_display_roundtrip(g.display) < 0 || !g.compositor || !g.wm || !g.shm) {
        std::fprintf(stderr, "lumen-probe: cannot bind the core globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(g.wm, &wm_listener, nullptr);
    int rc = 2;
    if (!std::strcmp(argv[1], "constraints"))
        rc = mode_constraints(argc, argv);
    else if (!std::strcmp(argv[1], "gestures"))
        rc = mode_gestures(argc, argv);
    wl_display_disconnect(g.display);
    return rc;
}

#endif
