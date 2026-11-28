// lumen-probe wlr-toplevels: a bar's client of zwlr_foreign_toplevel_management_v1 for the seat
// check (app-smoke row Q8). Own connection; main() dispatches here.
//
//   lumen-probe wlr-toplevels [--version V] [--watch S] [--for S] [@MS:REQUEST:TITLE ...]
//       Binds the manager (version V, 3 by default) and prints every event as it arrives,
//       `h=N` being the handle's number in arrival order:
//         `toplevel h=N`, `title h=N T`, `app_id h=N A`, `state h=N a,b` (`-`: none),
//         `output_enter h=N OUTPUT`, `output_leave h=N OUTPUT`,
//         `parent h=N -> h=M` / `parent h=N -> null`, `done h=N`, `closed h=N`, `finished`.
//       Each `@MS:REQUEST:TITLE` sends REQUEST on the handle of the window titled TITLE MS
//       milliseconds after that window was listed (the done that closes its opening batch):
//       activate, close, set-minimized, unset-minimized, set-maximized, unset-maximized,
//       set-fullscreen, unset-fullscreen. It prints `request REQUEST h=N` as it goes
//       (`request REQUEST TITLE no-handle` when there is none to send it on).
//       --watch S keeps listening S seconds after the last request went (default 1); --for S
//       bounds the whole run (default 20). A summary line `wlr-toplevels handles=N dones=N
//       closed=N` ends it.

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <wayland-client.h>
#ifdef LUMEN_PROTOCOL_WLR_FOREIGN_TOPLEVEL_MANAGEMENT_UNSTABLE_V1
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#define PROBE_WLR_FOREIGN_TOPLEVEL 1
#endif

int wlr_toplevel_main(int argc, char** argv);

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

struct toplevel_rec {
    zwlr_foreign_toplevel_handle_v1* handle = nullptr;
    std::string title;
    bool listed = false; // its opening batch closed
    bool closed = false;
};

/// One `@MS:REQUEST:TITLE`.
struct action {
    long long after_ms = 0; // once the window titled `title` is listed
    std::string request;
    std::string title;
    long long due_ms = 0; // 0: the window is not listed yet
    bool done = false;
};

struct probe {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    std::vector<output_rec> outputs;
    unsigned ftl_name = 0;
    unsigned ftl_version = 0;
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    std::vector<toplevel_rec> toplevels;
    std::vector<action> actions;
    unsigned dones = 0;
    unsigned closed = 0;
    bool finished = false;
} g;

const wl_output_listener output_listener = {
    [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
       int32_t) {},
    [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    [](void*, wl_output*) {},
    [](void*, wl_output*, int32_t) {},
    [](void*, wl_output* o, const char* name) {
        for (auto& r : g.outputs)
            if (r.output == o)
                r.name = name ? name : "";
    },
    [](void*, wl_output*, const char*) {}};

int index_of(zwlr_foreign_toplevel_handle_v1* h) {
    for (size_t i = 0; i < g.toplevels.size(); ++i)
        if (g.toplevels[i].handle == h)
            return static_cast<int>(i);
    return -1;
}

const char* output_name(wl_output* o) {
    for (auto& r : g.outputs)
        if (r.output == o)
            return r.name.empty() ? "?" : r.name.c_str();
    return "?";
}

#ifdef PROBE_WLR_FOREIGN_TOPLEVEL

std::string state_text(const wl_array* states) {
    static const char* const k_names[] = {"maximized", "minimized", "activated", "fullscreen"};
    std::string s;
    const auto* v = static_cast<const uint32_t*>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        s += (s.empty() ? "" : ",");
        s += v[i] < 4 ? k_names[v[i]] : "?";
    }
    return s.empty() ? "-" : s;
}

/// The window's opening batch closed: the requests timed from its listing are due.
void listed(int index) {
    if (index < 0)
        return;
    g.toplevels[static_cast<size_t>(index)].listed = true;
    const std::string& title = g.toplevels[static_cast<size_t>(index)].title;
    for (action& a : g.actions)
        if (!a.done && a.due_ms == 0 && a.title == title)
            a.due_ms = now_ms() + a.after_ms;
}

const zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    [](void*, zwlr_foreign_toplevel_handle_v1* h, const char* title) {
        const int i = index_of(h);
        if (i >= 0)
            g.toplevels[static_cast<size_t>(i)].title = title ? title : "";
        std::printf("title h=%d %s\n", i, title ? title : "");
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h, const char* app_id) {
        std::printf("app_id h=%d %s\n", index_of(h), app_id ? app_id : "");
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h, wl_output* output) {
        std::printf("output_enter h=%d %s\n", index_of(h), output_name(output));
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h, wl_output* output) {
        std::printf("output_leave h=%d %s\n", index_of(h), output_name(output));
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h, wl_array* states) {
        std::printf("state h=%d %s\n", index_of(h), state_text(states).c_str());
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h) {
        ++g.dones;
        std::printf("done h=%d\n", index_of(h));
        listed(index_of(h));
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h) {
        const int i = index_of(h);
        if (i >= 0)
            g.toplevels[static_cast<size_t>(i)].closed = true;
        ++g.closed;
        std::printf("closed h=%d\n", i);
    },
    [](void*, zwlr_foreign_toplevel_handle_v1* h, zwlr_foreign_toplevel_handle_v1* parent) {
        std::printf("parent h=%d -> %s\n", index_of(h),
                    parent ? std::string("h=" + std::to_string(index_of(parent))).c_str() : "null");
    }};

const zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    [](void*, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* h) {
        g.toplevels.push_back({h, "", false, false});
        zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, nullptr);
        std::printf("toplevel h=%zu\n", g.toplevels.size() - 1);
    },
    [](void*, zwlr_foreign_toplevel_manager_v1*) {
        g.finished = true;
        g.manager = nullptr;
        std::printf("finished\n");
    }};

#endif // PROBE_WLR_FOREIGN_TOPLEVEL

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, wl_seat_interface.name) && !g.seat)
        g.seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (!std::strcmp(iface, wl_output_interface.name) && version >= 4) {
        output_rec o{};
        o.output = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 4));
        wl_output_add_listener(o.output, &output_listener, nullptr);
        g.outputs.push_back(o);
    } else if (!std::strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) {
        g.ftl_name = name;
        g.ftl_version = version;
    }
}
const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};

void on_signal(int) {
    g_stop = 1;
}

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

#ifdef PROBE_WLR_FOREIGN_TOPLEVEL

/// The request of one action, on the handle of the window titled so.
void run_action(const action& a) {
    zwlr_foreign_toplevel_handle_v1* h = nullptr;
    int index = -1;
    for (size_t i = 0; i < g.toplevels.size(); ++i)
        if (!g.toplevels[i].closed && g.toplevels[i].title == a.title) {
            h = g.toplevels[i].handle;
            index = static_cast<int>(i);
        }
    const std::string& k = a.request;
    if (!h) {
        std::printf("request %s %s no-handle\n", k.c_str(), a.title.c_str());
        return;
    }
    wl_output* output = g.outputs.empty() ? nullptr : g.outputs[0].output;
    if (k == "activate")
        zwlr_foreign_toplevel_handle_v1_activate(h, g.seat);
    else if (k == "close")
        zwlr_foreign_toplevel_handle_v1_close(h);
    else if (k == "set-minimized")
        zwlr_foreign_toplevel_handle_v1_set_minimized(h);
    else if (k == "unset-minimized")
        zwlr_foreign_toplevel_handle_v1_unset_minimized(h);
    else if (k == "set-maximized")
        zwlr_foreign_toplevel_handle_v1_set_maximized(h);
    else if (k == "unset-maximized")
        zwlr_foreign_toplevel_handle_v1_unset_maximized(h);
    else if (k == "set-fullscreen")
        zwlr_foreign_toplevel_handle_v1_set_fullscreen(h, output);
    else if (k == "unset-fullscreen")
        zwlr_foreign_toplevel_handle_v1_unset_fullscreen(h);
    else {
        std::printf("request %s unknown\n", k.c_str());
        return;
    }
    std::printf("request %s h=%d\n", k.c_str(), index);
}

int run(int argc, char** argv) {
    unsigned version = 3;
    double watch = 1.0;
    double total = 20.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--version") && i + 1 < argc)
            version = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)
            watch = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--for") && i + 1 < argc)
            total = std::atof(argv[++i]);
        else if (argv[i][0] == '@') {
            // @MS:REQUEST:TITLE — the title may hold colons itself.
            const char* at = argv[i] + 1;
            const char* request = std::strchr(at, ':');
            const char* title = request ? std::strchr(request + 1, ':') : nullptr;
            if (!title) {
                std::fprintf(stderr, "lumen-probe: bad action %s\n", argv[i]);
                return 2;
            }
            action a{};
            a.after_ms = std::atoll(at);
            a.request = std::string(request + 1, title - request - 1);
            a.title = std::string(title + 1);
            g.actions.push_back(a);
        }
    }
    if (!connect_probe() || !g.ftl_name) {
        std::printf("wlr-toplevels unavailable\n");
        return 1;
    }
    g.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
        wl_registry_bind(g.registry, g.ftl_name, &zwlr_foreign_toplevel_manager_v1_interface,
                         version < g.ftl_version ? version : g.ftl_version));
    zwlr_foreign_toplevel_manager_v1_add_listener(g.manager, &manager_listener, nullptr);
    const long long start = now_ms();
    const long long end = start + static_cast<long long>(total * 1000.0);
    const long long watch_ms = static_cast<long long>(watch * 1000.0);
    long long last_request = start;
    while (!g_stop && !g.finished && now_ms() < end) {
        long long next = end;
        for (const action& a : g.actions)
            if (a.due_ms && !a.done && a.due_ms < next)
                next = a.due_ms;
        // In short slices: a request is armed by an event that arrives meanwhile.
        if (!pump_until(std::min(next, now_ms() + 50)))
            return 1;
        bool all_done = true;
        for (action& a : g.actions) {
            if (a.done)
                continue;
            if (a.due_ms && now_ms() >= a.due_ms) {
                a.done = true;
                last_request = now_ms();
                run_action(a);
            } else {
                all_done = false; // still to come: its window is not listed yet
            }
        }
        if (all_done && now_ms() >= last_request + watch_ms)
            break;
    }
    std::printf("wlr-toplevels handles=%zu dones=%u closed=%u\n", g.toplevels.size(), g.dones,
                g.closed);
    return 0;
}

#endif // PROBE_WLR_FOREIGN_TOPLEVEL

} // namespace

int wlr_toplevel_main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef PROBE_WLR_FOREIGN_TOPLEVEL
    return run(argc, argv);
#else
    (void)argc;
    (void)argv;
    std::printf("built without wlr-foreign-toplevel-management-unstable-v1\n");
    return 1;
#endif
}
