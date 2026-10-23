// lumen-probe workspaces / window: deterministic clients for the workspace checks
// (design-session-and-wm T.2, app-smoke group Q). Own connection; main() dispatches here.
//
//   lumen-probe workspaces [--activate NAME] [--assign NAME] [--output CONNECTOR]
//                          [--watch S]
//       An ext-workspace-v1 client. Prints every event as it arrives
//         `group new g=N`, `group g=N output-enter OUTPUT`, `workspace new w=N`,
//         `workspace w=N name=NAME coord=C state=S caps=C`, `enter g=N w=N`, `leave g=N w=N`,
//         `removed w=N`, `group removed g=N`
//       and at every `done` one summary line per group:
//         `ws output=OUTPUT workspaces=1*,2+,3` (* active, ! urgent; the output's wl_output
//         name), then `done n=K`.
//       --activate NAME (on --output's group, else the first group that has one so named)
//       activates it after the first done and commits; --assign NAME moves it to --output's
//       group (assign + commit). --watch S keeps listening S seconds after that (default 1).
//
//   lumen-probe window [--title T] [--app-id A] [--size WxH] [--fullscreen]
//                      [--minimize-after MS] [--min-size WxH] [--keyboard] [--exit-after S]
//       A window (xdg_wm_base v6, solid #4a7ab0) drawn at the size each configure asks for.
//       Prints `configure w=W h=H states=a,b` for every configure (suspended and the tiled
//       states show), `mapped` once, `minimize` when it asks to be minimized, `close` when
//       asked to close. --min-size sets xdg_toplevel.set_min_size. With --keyboard it also
//       prints what its wl_keyboard is told: `keyboard enter keys=N`, `keyboard leave`,
//       `key CODE pressed|released` (the switcher and keyboard-move checks: a cancelled
//       Alt+Tab leaves it focused and keyless).

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
#include "ext-workspace-v1-client-protocol.h"
#define PROBE_EXT_WORKSPACE 1
#endif

int workspace_main(int argc, char** argv);

namespace {

volatile std::sig_atomic_t g_stop = 0;

long long now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

struct output_rec {
    wl_output* output = nullptr;
    std::string name;
};

struct probe {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm = nullptr;
    wl_seat* seat = nullptr;
    std::vector<output_rec> outputs;
#if defined(PROBE_EXT_WORKSPACE)
    ext_workspace_manager_v1* manager = nullptr;
#endif
    unsigned ws_global = 0;
} g;

const wl_output_listener output_listener = {
    [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
       int32_t) {},
    [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    [](void*, wl_output*) {},
    [](void*, wl_output*, int32_t) {},
    [](void* d, wl_output* o, const char* name) {
        for (auto& r : g.outputs)
            if (r.output == o)
                r.name = name ? name : "";
        (void)d;
    },
    [](void*, wl_output*, const char*) {}};

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, wl_compositor_interface.name))
        g.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (!std::strcmp(iface, wl_shm_interface.name))
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (!std::strcmp(iface, xdg_wm_base_interface.name))
        g.wm = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 6 ? version : 6));
    else if (!std::strcmp(iface, wl_seat_interface.name) && !g.seat)
        g.seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (!std::strcmp(iface, wl_output_interface.name) && version >= 4) {
        output_rec o{};
        o.output = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 4));
        wl_output_add_listener(o.output, &output_listener, nullptr);
        g.outputs.push_back(o);
    } else if (!std::strcmp(iface, "ext_workspace_manager_v1"))
        g.ws_global = name;
}
const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};

const char* output_name(wl_output* o) {
    for (auto& r : g.outputs)
        if (r.output == o)
            return r.name.empty() ? "?" : r.name.c_str();
    return "?";
}

void on_signal(int) { g_stop = 1; }

bool connect_probe() {
    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect\n");
        return false;
    }
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, nullptr);
    return wl_display_roundtrip(g.display) >= 0 && wl_display_roundtrip(g.display) >= 0;
}

/// Dispatches until `deadline_ms` (or a signal); false when the connection broke.
bool pump_until(long long deadline_ms) {
    while (!g_stop) {
        const long long left = deadline_ms - now_ms();
        if (left <= 0)
            return true;
        while (wl_display_prepare_read(g.display) != 0)
            if (wl_display_dispatch_pending(g.display) < 0)
                return false;
        wl_display_flush(g.display);
        pollfd pf{wl_display_get_fd(g.display), POLLIN, 0};
        const int r = poll(&pf, 1, static_cast<int>(left > 100 ? 100 : left));
        if (r > 0) {
            if (wl_display_read_events(g.display) < 0)
                return false;
        } else {
            wl_display_cancel_read(g.display);
        }
        if (wl_display_dispatch_pending(g.display) < 0)
            return false;
    }
    return true;
}

#if defined(PROBE_EXT_WORKSPACE)

// ── workspaces ──

struct ws_rec {
    ext_workspace_handle_v1* proxy = nullptr;
    unsigned n = 0;
    std::string name;
    unsigned coord = 0;
    unsigned state = 0;
    unsigned caps = 0;
    int group = -1;
    bool removed = false;
};
struct group_rec {
    ext_workspace_group_handle_v1* proxy = nullptr;
    unsigned n = 0;
    std::vector<wl_output*> outputs;
    bool removed = false;
};
std::vector<group_rec> g_groups;
std::vector<ws_rec> g_ws;
unsigned g_dones = 0;

int group_index(ext_workspace_group_handle_v1* p) {
    for (std::size_t i = 0; i < g_groups.size(); ++i)
        if (g_groups[i].proxy == p)
            return static_cast<int>(i);
    return -1;
}
int ws_index(ext_workspace_handle_v1* p) {
    for (std::size_t i = 0; i < g_ws.size(); ++i)
        if (g_ws[i].proxy == p && !g_ws[i].removed)
            return static_cast<int>(i);
    return -1;
}

std::string caps_text(unsigned c) {
    std::string s;
    if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE)
        s += "activate,";
    if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE)
        s += "deactivate,";
    if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE)
        s += "remove,";
    if (c & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN)
        s += "assign,";
    if (!s.empty())
        s.pop_back();
    return s.empty() ? "-" : s;
}

std::string state_text(unsigned st) {
    std::string s;
    if (st & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
        s += "active,";
    if (st & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT)
        s += "urgent,";
    if (st & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN)
        s += "hidden,";
    if (!s.empty())
        s.pop_back();
    return s.empty() ? "-" : s;
}

void print_ws(const ws_rec& w) {
    std::printf("workspace w=%u name=%s coord=%u state=%s caps=%s\n", w.n, w.name.c_str(), w.coord,
                state_text(w.state).c_str(), caps_text(w.caps).c_str());
}

const ext_workspace_group_handle_v1_listener group_listener = {
    [](void*, ext_workspace_group_handle_v1*, uint32_t) {},
    [](void*, ext_workspace_group_handle_v1* p, wl_output* o) {
        const int i = group_index(p);
        if (i < 0)
            return;
        g_groups[i].outputs.push_back(o);
        std::printf("group g=%u output-enter %s\n", g_groups[i].n, output_name(o));
    },
    [](void*, ext_workspace_group_handle_v1* p, wl_output* o) {
        const int i = group_index(p);
        if (i < 0)
            return;
        auto& v = g_groups[i].outputs;
        for (auto it = v.begin(); it != v.end(); ++it)
            if (*it == o) {
                v.erase(it);
                break;
            }
        std::printf("group g=%u output-leave %s\n", g_groups[i].n, output_name(o));
    },
    [](void*, ext_workspace_group_handle_v1* p, ext_workspace_handle_v1* w) {
        const int gi = group_index(p);
        const int wi = ws_index(w);
        if (gi < 0 || wi < 0)
            return;
        g_ws[wi].group = gi;
        std::printf("enter g=%u w=%u\n", g_groups[gi].n, g_ws[wi].n);
    },
    [](void*, ext_workspace_group_handle_v1* p, ext_workspace_handle_v1* w) {
        const int gi = group_index(p);
        const int wi = ws_index(w);
        if (gi < 0 || wi < 0)
            return;
        if (g_ws[wi].group == gi)
            g_ws[wi].group = -1;
        std::printf("leave g=%u w=%u\n", g_groups[gi].n, g_ws[wi].n);
    },
    [](void*, ext_workspace_group_handle_v1* p) {
        const int i = group_index(p);
        if (i < 0)
            return;
        g_groups[i].removed = true;
        std::printf("group removed g=%u\n", g_groups[i].n);
        ext_workspace_group_handle_v1_destroy(p);
        g_groups[i].proxy = nullptr;
    }};

const ext_workspace_handle_v1_listener ws_listener = {
    [](void*, ext_workspace_handle_v1* p, const char* id) {
        if (const int i = ws_index(p); i >= 0)
            std::printf("workspace w=%u id=%s\n", g_ws[i].n, id);
    },
    [](void*, ext_workspace_handle_v1* p, const char* name) {
        if (const int i = ws_index(p); i >= 0)
            g_ws[i].name = name;
    },
    [](void*, ext_workspace_handle_v1* p, wl_array* a) {
        if (const int i = ws_index(p); i >= 0)
            g_ws[i].coord = a && a->size >= 4 ? *static_cast<uint32_t*>(a->data) : 0;
    },
    [](void*, ext_workspace_handle_v1* p, uint32_t state) {
        if (const int i = ws_index(p); i >= 0)
            g_ws[i].state = state;
    },
    [](void*, ext_workspace_handle_v1* p, uint32_t caps) {
        if (const int i = ws_index(p); i >= 0)
            g_ws[i].caps = caps;
    },
    [](void*, ext_workspace_handle_v1* p) {
        const int i = ws_index(p);
        if (i < 0)
            return;
        g_ws[i].removed = true;
        std::printf("removed w=%u name=%s\n", g_ws[i].n, g_ws[i].name.c_str());
        ext_workspace_handle_v1_destroy(p);
    }};

void print_summary() {
    for (const auto& gr : g_groups) {
        if (gr.removed)
            continue;
        std::vector<const ws_rec*> list;
        for (const auto& w : g_ws)
            if (!w.removed && w.group >= 0 && g_groups[w.group].proxy == gr.proxy)
                list.push_back(&w);
        std::sort(list.begin(), list.end(),
                  [](const ws_rec* a, const ws_rec* b) { return a->coord < b->coord; });
        std::string names;
        for (const ws_rec* w : list) {
            if (!names.empty())
                names += ',';
            names += w->name;
            if (w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
                names += '*';
            if (w->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT)
                names += '!';
        }
        std::printf("ws output=%s workspaces=%s\n",
                    gr.outputs.empty() ? "-" : output_name(gr.outputs.front()),
                    names.empty() ? "-" : names.c_str());
    }
}

const ext_workspace_manager_v1_listener manager_listener = {
    [](void*, ext_workspace_manager_v1*, ext_workspace_group_handle_v1* p) {
        group_rec gr{};
        gr.proxy = p;
        gr.n = static_cast<unsigned>(g_groups.size());
        g_groups.push_back(gr);
        ext_workspace_group_handle_v1_add_listener(p, &group_listener, nullptr);
        std::printf("group new g=%u\n", gr.n);
    },
    [](void*, ext_workspace_manager_v1*, ext_workspace_handle_v1* p) {
        ws_rec w{};
        w.proxy = p;
        w.n = static_cast<unsigned>(g_ws.size());
        g_ws.push_back(w);
        ext_workspace_handle_v1_add_listener(p, &ws_listener, nullptr);
        std::printf("workspace new w=%u\n", w.n);
    },
    [](void*, ext_workspace_manager_v1*) {
        for (const auto& w : g_ws)
            if (!w.removed)
                print_ws(w);
        print_summary();
        ++g_dones;
        std::printf("done n=%u\n", g_dones);
    },
    [](void*, ext_workspace_manager_v1*) { std::printf("finished\n"); }};

/// The live workspace `name` on the group of output `out` ("" any group).
ws_rec* find_ws(const char* name, const char* out) {
    for (auto& w : g_ws) {
        if (w.removed || w.name != name || w.group < 0)
            continue;
        if (!out || !*out)
            return &w;
        for (wl_output* o : g_groups[w.group].outputs)
            if (std::strcmp(output_name(o), out) == 0)
                return &w;
    }
    return nullptr;
}

group_rec* find_group(const char* out) {
    for (auto& gr : g_groups)
        if (!gr.removed)
            for (wl_output* o : gr.outputs)
                if (std::strcmp(output_name(o), out) == 0)
                    return &gr;
    return nullptr;
}

int run_workspaces(int argc, char** argv) {
    const char* activate = nullptr;
    const char* assign = nullptr;
    const char* output = "";
    double watch = 1.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--activate") && i + 1 < argc)
            activate = argv[++i];
        else if (!std::strcmp(argv[i], "--assign") && i + 1 < argc)
            assign = argv[++i];
        else if (!std::strcmp(argv[i], "--output") && i + 1 < argc)
            output = argv[++i];
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)
            watch = std::atof(argv[++i]);
    }
    if (!connect_probe())
        return 1;
    if (!g.ws_global) {
        std::printf("no ext_workspace_manager_v1\n");
        return 1;
    }
    g.manager = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(g.registry, g.ws_global, &ext_workspace_manager_v1_interface, 1));
    ext_workspace_manager_v1_add_listener(g.manager, &manager_listener, nullptr);
    const long long first = now_ms() + 3000;
    while (g_dones == 0 && now_ms() < first && !g_stop)
        if (!pump_until(now_ms() + 50))
            return 1;
    if (g_dones == 0) {
        std::printf("no done\n");
        return 1;
    }
    if (assign) {
        ws_rec* w = find_ws(assign, nullptr);
        group_rec* gr = *output ? find_group(output) : nullptr;
        if (!w || !gr) {
            std::printf("assign: no workspace %s or no group of output %s\n", assign, output);
            return 1;
        }
        ext_workspace_handle_v1_assign(w->proxy, gr->proxy);
        std::printf("request assign w=%u to g=%u\n", w->n, gr->n);
    }
    if (activate) {
        ws_rec* w = find_ws(activate, assign ? "" : output);
        if (!w) {
            std::printf("activate: no workspace %s\n", activate);
            return 1;
        }
        ext_workspace_handle_v1_activate(w->proxy);
        std::printf("request activate w=%u name=%s\n", w->n, w->name.c_str());
    }
    if (assign || activate)
        ext_workspace_manager_v1_commit(g.manager);
    if (!pump_until(now_ms() + static_cast<long long>(watch * 1000.0)))
        return 1;
    ext_workspace_manager_v1_stop(g.manager);
    (void)wl_display_roundtrip(g.display);
    return wl_display_get_error(g.display) ? 1 : 0;
}

#endif // PROBE_EXT_WORKSPACE

// ── window ──

struct window_state {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    int width = 320;
    int height = 200;
    int want_w = 0;
    int want_h = 0;
    bool mapped = false;
    bool configured = false;
    uint32_t serial = 0;
} w;

wl_buffer* make_buffer(int width, int height, uint32_t argb) {
    const int stride = width * 4;
    const int size = stride * height;
    const int fd = memfd_create("lumen-probe-window", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    if (void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(p);
        for (int i = 0; i < width * height; ++i)
            px[i] = argb;
        munmap(p, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    static const wl_buffer_listener release = {[](void*, wl_buffer* b) { wl_buffer_destroy(b); }};
    wl_buffer_add_listener(b, &release, nullptr);
    return b;
}

const xdg_wm_base_listener wm_listener = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }};

const xdg_surface_listener surface_listener = {
    [](void*, xdg_surface* xs, uint32_t serial) {
        w.serial = serial;
        xdg_surface_ack_configure(xs, serial);
        if (w.want_w > 0 && w.want_h > 0) {
            w.width = w.want_w;
            w.height = w.want_h;
        }
        wl_surface_attach(w.surface, make_buffer(w.width, w.height, 0xFF4A7AB0u), 0, 0);
        wl_surface_damage_buffer(w.surface, 0, 0, w.width, w.height);
        wl_surface_commit(w.surface);
        if (!w.mapped) {
            w.mapped = true;
            std::printf("mapped\n");
        }
    }};

const xdg_toplevel_listener toplevel_listener = {
    [](void*, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
        std::string s;
        const auto* v = static_cast<const uint32_t*>(states->data);
        for (std::size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
            static const char* const k_names[] = {"?",          "maximized",  "fullscreen",
                                                  "resizing",   "activated",  "tiled_left",
                                                  "tiled_right", "tiled_top", "tiled_bottom",
                                                  "suspended",  "constrained_left",
                                                  "constrained_right", "constrained_top",
                                                  "constrained_bottom"};
            s += (s.empty() ? "" : ",");
            s += v[i] < sizeof(k_names) / sizeof(k_names[0]) ? k_names[v[i]] : "?";
        }
        w.want_w = width;
        w.want_h = height;
        std::printf("configure w=%d h=%d states=%s\n", width, height, s.empty() ? "-" : s.c_str());
    },
    [](void*, xdg_toplevel*) {
        std::printf("close\n");
        g_stop = 1;
    },
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {}};

const wl_keyboard_listener keyboard_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
        std::printf("keyboard enter keys=%zu\n", keys->size / sizeof(uint32_t));
    },
    [](void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("keyboard leave\n"); },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        std::printf("key %u %s\n", key, state ? "pressed" : "released");
    },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
};

int run_window(int argc, char** argv) {
    const char* title = "lumen-probe window";
    const char* app_id = "lumen-probe";
    int minimize_after = -1;
    double exit_after = 0.0;
    bool fullscreen = false;
    bool keyboard = false;
    int min_w = 0, min_h = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--title") && i + 1 < argc)
            title = argv[++i];
        else if (!std::strcmp(argv[i], "--app-id") && i + 1 < argc)
            app_id = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &w.width, &w.height);
        else if (!std::strcmp(argv[i], "--minimize-after") && i + 1 < argc)
            minimize_after = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--exit-after") && i + 1 < argc)
            exit_after = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--fullscreen"))
            fullscreen = true;
        else if (!std::strcmp(argv[i], "--keyboard"))
            keyboard = true;
        else if (!std::strcmp(argv[i], "--min-size") && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &min_w, &min_h);
    }
    if (!connect_probe() || !g.compositor || !g.shm || !g.wm)
        return 1;
    // wl_seat v1: the keyboard is there from the start on Lumen's seat (capabilities are not
    // waited for).
    wl_keyboard* kb = nullptr;
    if (keyboard && g.seat) {
        kb = wl_seat_get_keyboard(g.seat);
        wl_keyboard_add_listener(kb, &keyboard_listener, nullptr);
    }
    xdg_wm_base_add_listener(g.wm, &wm_listener, nullptr);
    w.surface = wl_compositor_create_surface(g.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(g.wm, w.surface);
    xdg_surface_add_listener(w.xdg, &surface_listener, nullptr);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, nullptr);
    xdg_toplevel_set_title(w.toplevel, title);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    if (min_w > 0 || min_h > 0)
        xdg_toplevel_set_min_size(w.toplevel, min_w, min_h);
    if (fullscreen)
        xdg_toplevel_set_fullscreen(w.toplevel, nullptr);
    wl_surface_commit(w.surface);
    const long long start = now_ms();
    bool minimized = false;
    while (!g_stop) {
        const long long t = now_ms();
        if (exit_after > 0.0 && t - start >= static_cast<long long>(exit_after * 1000.0))
            break;
        if (!minimized && minimize_after >= 0 && w.mapped && t - start >= minimize_after) {
            xdg_toplevel_set_minimized(w.toplevel);
            minimized = true;
            std::printf("minimize\n");
        }
        if (!pump_until(t + 50))
            return 1;
    }
    xdg_toplevel_destroy(w.toplevel);
    xdg_surface_destroy(w.xdg);
    wl_surface_destroy(w.surface);
    if (kb)
        wl_keyboard_destroy(kb);
    wl_display_roundtrip(g.display);
    return 0;
}

} // namespace

int workspace_main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (argc >= 2 && !std::strcmp(argv[1], "window"))
        return run_window(argc, argv);
#if defined(PROBE_EXT_WORKSPACE)
    return run_workspaces(argc, argv);
#else
    std::printf("built without ext-workspace-v1\n");
    return 1;
#endif
}
