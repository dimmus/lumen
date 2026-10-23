// lumen-probe keyboard: a deterministic client of the seat's keyboard and pointer, for the
// configuration checks (design-session-and-wm T.2, app-smoke group R). Own connection;
// main() dispatches here.
//
//   lumen-probe keyboard [--size WxH] [--title T] [--exit-after S]
//       A window (default 360x200). Prints, as they arrive:
//         `keymap size=N layouts=K names=A|B`   every wl_keyboard.keymap (compiled with
//                                              xkbcommon: K layouts, their names)
//         `repeat rate=R delay=D`              every wl_keyboard.repeat_info
//         `focus in` / `focus out`
//         `key code=C state=S sym=NAME utf8=TEXT layout=L`   what the key produces with the
//                                              client's own xkb state, as a toolkit sees it
//         `modifiers depressed=D latched=L locked=K group=G`
//         `relative DX,DY unaccel DXU,DYU`     relative-pointer-unstable-v1 (accelerated
//                                              and raw motion: the acceleration profile shows)
//         `axis vertical|horizontal V source=S discrete=N`   scrolling (natural scrolling and
//                                              the scroll factor show)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_RELATIVE_POINTER_UNSTABLE_V1)
#include "relative-pointer-unstable-v1-client-protocol.h"
#define PROBE_INPUT_RELATIVE 1
#endif
#if defined(LUMEN_HAS_INPUT)
#include <xkbcommon/xkbcommon.h>
#endif

int input_main(int argc, char** argv);

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct globals {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm = nullptr;
#if defined(PROBE_INPUT_RELATIVE)
    zwp_relative_pointer_manager_v1* relative = nullptr;
#endif
#if defined(LUMEN_HAS_INPUT)
    xkb_context* xkb = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
#endif
} g;

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, wl_compositor_interface.name))
        g.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (!std::strcmp(iface, wl_shm_interface.name))
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (!std::strcmp(iface, wl_seat_interface.name))
        g.seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, version < 7 ? version : 7));
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        g.wm = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, 1));
#if defined(PROBE_INPUT_RELATIVE)
    else if (!std::strcmp(iface, zwp_relative_pointer_manager_v1_interface.name))
        g.relative = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1));
#endif
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

wl_buffer* solid_buffer(int w, int h, uint32_t argb) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-probe-input", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int i = 0; i < w * h; ++i)
            px[i] = argb;
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

// ── keyboard ──

void kb_keymap(void*, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    std::string names;
    unsigned layouts = 0;
#if defined(LUMEN_HAS_INPUT)
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            if (!g.xkb)
                g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            xkb_keymap* km = xkb_keymap_new_from_string(g.xkb, static_cast<const char*>(map),
                                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(map, size);
            if (km) {
                if (g.state)
                    xkb_state_unref(g.state);
                if (g.keymap)
                    xkb_keymap_unref(g.keymap);
                g.keymap = km;
                g.state = xkb_state_new(km);
                layouts = xkb_keymap_num_layouts(km);
                for (unsigned i = 0; i < layouts; ++i) {
                    if (i)
                        names += '|';
                    const char* n = xkb_keymap_layout_get_name(km, i);
                    names += n ? n : "?";
                }
            }
        }
    }
#else
    (void)format;
#endif
    close(fd);
    std::printf("keymap size=%u layouts=%u names=%s\n", size, layouts, names.c_str());
}

void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    char sym[64] = "?";
    char utf8[32] = "";
    unsigned layout = 0;
#if defined(LUMEN_HAS_INPUT)
    if (g.state) {
        const xkb_keysym_t s = xkb_state_key_get_one_sym(g.state, key + 8);
        xkb_keysym_get_name(s, sym, sizeof(sym));
        xkb_state_key_get_utf8(g.state, key + 8, utf8, sizeof(utf8));
        layout = xkb_state_key_get_layout(g.state, key + 8);
    }
#endif
    for (char* c = utf8; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20)
            *c = '?';
    std::printf("key code=%u state=%u sym=%s utf8=%s layout=%u\n", key, state, sym, utf8, layout);
}

void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched,
                  uint32_t locked, uint32_t group) {
#if defined(LUMEN_HAS_INPUT)
    if (g.state)
        xkb_state_update_mask(g.state, depressed, latched, locked, 0, 0, group);
#endif
    std::printf("modifiers depressed=%u latched=%u locked=%u group=%u\n", depressed, latched,
                locked, group);
}

const wl_keyboard_listener keyboard_listener = {
    kb_keymap,
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { std::printf("focus in\n"); },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("focus out\n"); },
    kb_key,
    kb_modifiers,
    [](void*, wl_keyboard*, int32_t rate, int32_t delay) {
        std::printf("repeat rate=%d delay=%d\n", rate, delay);
    }};

// ── pointer ──

unsigned g_axis_source = 0;
int g_discrete[2]{};

const wl_pointer_listener pointer_listener = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) { std::printf("enter\n"); },
    [](void*, wl_pointer*, uint32_t, wl_surface*) { std::printf("leave\n"); },
    [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
        std::printf("button %u %s\n", button, state ? "pressed" : "released");
    },
    [](void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
        std::printf("axis %s %.2f source=%u discrete=%d\n",
                    axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "vertical" : "horizontal",
                    wl_fixed_to_double(value), g_axis_source, g_discrete[axis ? 1 : 0]);
        g_discrete[axis ? 1 : 0] = 0;
    },
    [](void*, wl_pointer*) {},
    [](void*, wl_pointer*, uint32_t source) { g_axis_source = source; },
    [](void*, wl_pointer*, uint32_t, uint32_t) {},
    [](void*, wl_pointer*, uint32_t axis, int32_t discrete) { g_discrete[axis ? 1 : 0] = discrete; },
    nullptr,
    nullptr};

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* top = nullptr;
    uint32_t serial = 0;
    bool configured = false;
} w;

} // namespace

int input_main(int argc, char** argv) {
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });
    int width = 360, height = 200;
    if (const char* s = opt(argc, argv, "--size"))
        (void)std::sscanf(s, "%dx%d", &width, &height);
    const double exit_after = std::atof(opt(argc, argv, "--exit-after", "0"));
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect\n");
        return 2;
    }
    wl_registry* reg = wl_display_get_registry(g.display);
    wl_registry_add_listener(reg, &registry_listener, nullptr);
    (void)wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.wm || !g.seat) {
        std::fprintf(stderr, "lumen-probe: missing globals\n");
        return 2;
    }
    xdg_wm_base_add_listener(g.wm, &wm_listener, nullptr);
    wl_keyboard_add_listener(wl_seat_get_keyboard(g.seat), &keyboard_listener, nullptr);
    wl_pointer* pointer = wl_seat_get_pointer(g.seat);
    wl_pointer_add_listener(pointer, &pointer_listener, nullptr);
#if defined(PROBE_INPUT_RELATIVE)
    if (g.relative) {
        static const zwp_relative_pointer_v1_listener rl = {
            [](void*, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy,
               wl_fixed_t dxu, wl_fixed_t dyu) {
                std::printf("relative %.2f,%.2f unaccel %.2f,%.2f\n", wl_fixed_to_double(dx),
                            wl_fixed_to_double(dy), wl_fixed_to_double(dxu),
                            wl_fixed_to_double(dyu));
            }};
        zwp_relative_pointer_v1_add_listener(
            zwp_relative_pointer_manager_v1_get_relative_pointer(g.relative, pointer), &rl, nullptr);
    }
#endif
    static const xdg_surface_listener xl = {[](void*, xdg_surface*, uint32_t serial) {
        w.serial = serial;
        w.configured = true;
    }};
    static const xdg_toplevel_listener tl = {
        [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
        [](void*, xdg_toplevel*) {
            std::printf("close requested\n");
            g_stop = 1;
        },
        [](void*, xdg_toplevel*, int32_t, int32_t) {},
        [](void*, xdg_toplevel*, wl_array*) {}};
    w.surface = wl_compositor_create_surface(g.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(g.wm, w.surface);
    xdg_surface_add_listener(w.xdg, &xl, nullptr);
    w.top = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.top, &tl, nullptr);
    xdg_toplevel_set_title(w.top, opt(argc, argv, "--title", "lumen-probe keyboard"));
    xdg_toplevel_set_app_id(w.top, "lumen-probe");
    wl_surface_commit(w.surface);
    while (!w.configured && !g_stop)
        if (!pump(1000))
            return 1;
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, solid_buffer(width, height, 0xFF305080u), 0, 0);
    wl_surface_damage_buffer(w.surface, 0, 0, width, height);
    wl_surface_commit(w.surface);
    std::printf("keyboard mapped %dx%d\n", width, height);
    const double start = now_s();
    while (!g_stop) {
        if (!pump(50)) {
            std::fprintf(stderr, "lumen-probe: connection error\n");
            return 1;
        }
        if (exit_after > 0 && now_s() - start >= exit_after)
            break;
    }
    std::printf("exit\n");
    wl_display_disconnect(g.display);
    return 0;
}
