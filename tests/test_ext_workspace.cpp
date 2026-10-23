// ext-workspace-v1 and the window manager's workspaces (design-session-and-wm W.3–W.6),
// in-process against a real compositor with two virtual outputs: the bind (groups with
// capabilities 0 and output_enter, workspaces with name, coordinates, state, capabilities,
// workspace_enter, one done), output_enter for a wl_output bound later, a window joining the
// active workspace and the trailing empty one appearing, activate (the window hidden and
// `suspended`, shown again), removal order (workspace_leave before removed), an atomic
// commit (assign before activate: the workspace moves to the other output with its window),
// focus memory across switches, minimize (xdg set_minimized) and sticky, inert handles,
// and `stop` → `finished`. That sandboxed clients do not see the global is checked by
// test_window_protocols with the other session-wide globals.
#include <errno.h>
#include <stdio.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) && defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
#include "xdg-shell-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#define LUMEN_TEST_EXT_WORKSPACE 1
#endif
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.compositor;

#if !defined(LUMEN_TEST_EXT_WORKSPACE)

int main() {
    std::printf("SKIP: needs libwayland and ext-workspace-v1's glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-ext-workspace";

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;

void check(const char* name, bool ok, const std::string& detail = {}) {
    ++g_ran;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ",
                detail.c_str());
    if (!ok)
        ++g_failed;
}

// ── the client's view of ext-workspace, and a log of what arrived in order ──

struct ws_state {
    ext_workspace_handle_v1* proxy = nullptr;
    std::string name;
    std::string id;
    unsigned coordinate = ~0u;
    unsigned state = 0;
    unsigned caps = 0;
    ext_workspace_group_handle_v1* group = nullptr;
    bool removed = false;
};
struct group_state {
    ext_workspace_group_handle_v1* proxy = nullptr;
    unsigned caps = ~0u;
    std::vector<wl_output*> outputs;
    bool removed = false;
};

struct client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    std::vector<wl_output*> outputs;
    std::vector<uint32_t> output_names;
    uint32_t ws_name = 0;
    ext_workspace_manager_v1* manager = nullptr;
    std::vector<std::unique_ptr<group_state>> groups;
    std::vector<std::unique_ptr<ws_state>> workspaces;
    std::vector<std::string> log;
    unsigned dones = 0;
    bool finished = false;

    ws_state* ws(ext_workspace_handle_v1* p) {
        for (auto& w : workspaces)
            if (w->proxy == p)
                return w.get();
        return nullptr;
    }
    group_state* group(ext_workspace_group_handle_v1* p) {
        for (auto& g : groups)
            if (g->proxy == p)
                return g.get();
        return nullptr;
    }
    /// The live workspaces of `g` in coordinate order.
    std::vector<ws_state*> of(group_state* g) {
        std::vector<ws_state*> out;
        for (auto& w : workspaces)
            if (!w->removed && g && w->group == g->proxy)
                out.push_back(w.get());
        std::ranges::sort(out, {}, &ws_state::coordinate);
        return out;
    }
    /// "1* 2" for a group (* active).
    std::string names(group_state* g) {
        std::string s;
        for (ws_state* w : of(g)) {
            if (!s.empty())
                s += ' ';
            s += w->name + ((w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) ? "*" : "");
        }
        return s;
    }
    /// The live group whose outputs include `o`.
    group_state* group_of(wl_output* o) {
        for (auto& g : groups)
            if (!g->removed && std::ranges::find(g->outputs, o) != g->outputs.end())
                return g.get();
        return nullptr;
    }
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
    else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        c->outputs.push_back(static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 4)));
        c->output_names.push_back(name);
    } else if (std::strcmp(iface, ext_workspace_manager_v1_interface.name) == 0)
        c->ws_name = name;
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

// wl_output events are not needed.
const wl_output_listener output_listener = {
    [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
       int32_t) {},
    [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    [](void*, wl_output*) {},
    [](void*, wl_output*, int32_t) {},
    [](void*, wl_output*, const char*) {},
    [](void*, wl_output*, const char*) {}};

std::string ws_label(client* c, ext_workspace_handle_v1* w) {
    ws_state* s = c->ws(w);
    return s ? s->name : std::string("?");
}

const ext_workspace_group_handle_v1_listener group_listener = {
    [](void* d, ext_workspace_group_handle_v1* g, uint32_t caps) {
        auto* c = static_cast<client*>(d);
        c->group(g)->caps = caps;
        c->log.push_back("group-caps");
    },
    [](void* d, ext_workspace_group_handle_v1* g, wl_output* o) {
        auto* c = static_cast<client*>(d);
        c->group(g)->outputs.push_back(o);
        c->log.push_back("output-enter");
    },
    [](void* d, ext_workspace_group_handle_v1* g, wl_output* o) {
        auto* c = static_cast<client*>(d);
        auto& v = c->group(g)->outputs;
        v.erase(std::remove(v.begin(), v.end(), o), v.end());
        c->log.push_back("output-leave");
    },
    [](void* d, ext_workspace_group_handle_v1* g, ext_workspace_handle_v1* w) {
        auto* c = static_cast<client*>(d);
        if (ws_state* s = c->ws(w))
            s->group = g;
        c->log.push_back("enter " + ws_label(c, w));
    },
    [](void* d, ext_workspace_group_handle_v1* g, ext_workspace_handle_v1* w) {
        auto* c = static_cast<client*>(d);
        if (ws_state* s = c->ws(w); s && s->group == g)
            s->group = nullptr;
        c->log.push_back("leave " + ws_label(c, w));
    },
    [](void* d, ext_workspace_group_handle_v1* g) {
        auto* c = static_cast<client*>(d);
        c->group(g)->removed = true;
        c->log.push_back("group-removed");
    }};

const ext_workspace_handle_v1_listener ws_listener = {
    [](void* d, ext_workspace_handle_v1* w, const char* id) {
        static_cast<client*>(d)->ws(w)->id = id;
    },
    [](void* d, ext_workspace_handle_v1* w, const char* name) {
        static_cast<client*>(d)->ws(w)->name = name;
    },
    [](void* d, ext_workspace_handle_v1* w, wl_array* a) {
        auto* s = static_cast<client*>(d)->ws(w);
        s->coordinate = a && a->size >= 4 ? *static_cast<uint32_t*>(a->data) : ~0u;
    },
    [](void* d, ext_workspace_handle_v1* w, uint32_t state) {
        static_cast<client*>(d)->ws(w)->state = state;
    },
    [](void* d, ext_workspace_handle_v1* w, uint32_t caps) {
        static_cast<client*>(d)->ws(w)->caps = caps;
    },
    [](void* d, ext_workspace_handle_v1* w) {
        auto* c = static_cast<client*>(d);
        c->ws(w)->removed = true;
        c->log.push_back("removed " + ws_label(c, w));
    }};

const ext_workspace_manager_v1_listener manager_listener = {
    [](void* d, ext_workspace_manager_v1*, ext_workspace_group_handle_v1* g) {
        auto* c = static_cast<client*>(d);
        auto st = std::make_unique<group_state>();
        st->proxy = g;
        c->groups.push_back(std::move(st));
        ext_workspace_group_handle_v1_add_listener(g, &group_listener, c);
        c->log.push_back("group");
    },
    [](void* d, ext_workspace_manager_v1*, ext_workspace_handle_v1* w) {
        auto* c = static_cast<client*>(d);
        auto st = std::make_unique<ws_state>();
        st->proxy = w;
        c->workspaces.push_back(std::move(st));
        ext_workspace_handle_v1_add_listener(w, &ws_listener, c);
        c->log.push_back("workspace");
    },
    [](void* d, ext_workspace_manager_v1*) {
        auto* c = static_cast<client*>(d);
        ++c->dones;
        c->log.push_back("done");
    },
    [](void* d, ext_workspace_manager_v1*) {
        auto* c = static_cast<client*>(d);
        c->finished = true;
        c->log.push_back("finished");
    }};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    if (wl_display_roundtrip(c.display) < 0)
        return false;
    for (wl_output* o : c.outputs)
        wl_output_add_listener(o, &output_listener, nullptr);
    return wl_display_roundtrip(c.display) >= 0;
}

void bind_manager(client& c) {
    c.manager = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(c.registry, c.ws_name, &ext_workspace_manager_v1_interface, 1));
    ext_workspace_manager_v1_add_listener(c.manager, &manager_listener, &c);
}

void settle(client& c) {
    (void)wl_display_roundtrip(c.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)wl_display_roundtrip(c.display);
}

bool protocol_ok(client& c) { return wl_display_get_error(c.display) == 0; }

// ── an application window ──

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
    bool suspended = false;
};

const xdg_surface_listener xdg_listener = {
    [](void* d, xdg_surface*, uint32_t serial) { static_cast<window*>(d)->serial = serial; }};
const xdg_toplevel_listener toplevel_listener = {
    [](void* d, xdg_toplevel*, int32_t, int32_t, wl_array* states) {
        auto* w = static_cast<window*>(d);
        w->suspended = false;
        const auto* s = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
            if (s[i] == XDG_TOPLEVEL_STATE_SUSPENDED)
                w->suspended = true;
    },
    [](void*, xdg_toplevel*) {},
    [](void*, xdg_toplevel*, int32_t, int32_t) {},
    [](void*, xdg_toplevel*, wl_array*) {}};

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test-ws", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void map_window(client& c, window& w, const char* app_id) {
    w.surface = wl_compositor_create_surface(c.compositor);
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, &w);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
    wl_surface_commit(w.surface);
    settle(c);
}

/// Acks the newest configure with a commit (what a client does).
void answer(client& c, window& w) {
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_commit(w.surface);
    settle(c);
}

struct rec_copy {
    lx::toplevel_id id{};
    lx::rect2i geometry{};
    lx::workspace_id workspace{};
    bool hidden = false;
    bool minimized = false;
    bool mapped = false;
};

rec_copy record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    rec_copy out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id())
            out = {r.id, r.geometry, r.workspace, r.hidden, r.minimized, r.mapped};
    });
    return out;
}

lx::toplevel_id focus() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().keyboard_focus();
}

template <typename Fn>
void with_wm(Fn&& fn) {
    std::lock_guard<std::mutex> hold(*g_lock);
    fn(g_comp->windows());
    g_comp->wayland().flush();
}

std::string joined(const std::vector<std::string>& v, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < v.size(); ++i)
        s += (i > from ? ", " : "") + v[i];
    return s;
}

void run_cases() {
    client c{};
    if (!connect(c) || !c.compositor || !c.wm_base || c.outputs.size() != 2 || !c.ws_name) {
        check("connect: two outputs and ext_workspace_manager_v1", false);
        return;
    }
    bind_manager(c);
    settle(c);
    group_state* ga = c.group_of(c.outputs[0]);
    group_state* gb = c.group_of(c.outputs[1]);
    check("bind: a group per output, capabilities 0, output_enter for its wl_output",
          c.groups.size() == 2 && ga && gb && ga != gb && ga->caps == 0 && gb->caps == 0,
          "groups " + std::to_string(c.groups.size()));
    check("bind: one active empty workspace per output (dynamic), activate|assign",
          ga && gb && c.names(ga) == "1*" && c.names(gb) == "1*" &&
              c.of(ga)[0]->caps == (EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
                                    EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN) &&
              c.of(ga)[0]->coordinate == 0 && c.of(ga)[0]->id.empty(),
          (ga ? c.names(ga) : "-") + " | " + (gb ? c.names(gb) : "-"));
    check("bind: everything closed by one done", c.dones == 1 && c.log.back() == "done",
          joined(c.log, 0));
    {
        // The group comes before its workspaces enter it; a workspace's events before its enter.
        const auto first_group = std::ranges::find(c.log, "group");
        const auto first_enter = std::ranges::find_if(
            c.log, [](const std::string& s) { return s.rfind("enter ", 0) == 0; });
        check("bind: groups announced before workspaces enter them", first_group < first_enter);
    }

    // A wl_output bound later: its group says so, with a done.
    std::size_t mark = c.log.size();
    {
        auto* late = static_cast<wl_output*>(
            wl_registry_bind(c.registry, c.output_names[0], &wl_output_interface, 4));
        wl_output_add_listener(late, &output_listener, nullptr);
        settle(c);
        check("a wl_output bound later gets output_enter and done",
              joined(c.log, mark) == "output-enter, done" &&
                  std::ranges::find(ga->outputs, late) != ga->outputs.end(),
              joined(c.log, mark));
    }

    // A window maps on the first output: it joins workspace 1, and the empty last one follows.
    window a{};
    mark = c.log.size();
    map_window(c, a, "ws-a");
    rec_copy ra = record("ws-a");
    check("a window joins the active workspace; a new empty one appears after it",
          ra.mapped && ra.workspace && c.names(ga) == "1* 2" && c.log.back() == "done",
          c.names(ga) + " log: " + joined(c.log, mark));
    window b{};
    map_window(c, b, "ws-b");
    rec_copy rb = record("ws-b");
    check("the second window has the keyboard", focus() == rb.id);
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle(c);
    check("activating the first gives it the keyboard", focus() == ra.id);

    // activate workspace 2 (+ commit): the windows hide, `suspended`, no keyboard.
    ws_state* w2 = c.of(ga)[1];
    ext_workspace_handle_v1_activate(w2->proxy);
    settle(c);
    check("activate without commit changes nothing", c.names(ga) == "1* 2");
    mark = c.log.size();
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);
    answer(c, a);
    ra = record("ws-a");
    check("activate + commit: workspace 2 is active", c.names(ga) == "1 2*", c.names(ga));
    check("its windows are hidden, told they are suspended, and lose the keyboard",
          ra.hidden && a.suspended && !focus(), "hidden " + std::to_string(ra.hidden) +
              " suspended " + std::to_string(a.suspended));
    check("one done for the change", joined(c.log, mark) == "done", joined(c.log, mark));

    // Back to 1: the empty 2 is the trailing one and stays.
    ws_state* w1 = c.of(ga)[0];
    ext_workspace_handle_v1_activate(w1->proxy);
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);
    answer(c, a);
    ra = record("ws-a");
    check("back on 1: the trailing empty workspace stays", c.names(ga) == "1* 2" && !w2->removed,
          c.names(ga));
    check("the windows are shown again, not suspended, and the one focused there has the "
          "keyboard back (focus memory)",
          !ra.hidden && !a.suspended && focus() == ra.id,
          "focus " + std::to_string(focus().id()) + " a " + std::to_string(ra.id.id()));

    // Window b to workspace 2 without following: 2 is occupied, 3 is the trailing one; b
    // back to 1: 2 is empty, inactive and not last — it goes: leave, removed, done.
    lx::workspace_id ws1_id{}, ws2_id{};
    with_wm([&](auto& wm) {
        ws1_id = wm.workspaces().by_index(0, 1);
        ws2_id = wm.workspaces().by_index(0, 2);
        (void)wm.move_to_workspace(rb.id, ws2_id, false, "test");
    });
    settle(c);
    check("b to 2 without following: 2 occupied, a new trailing 3, b hidden",
          c.names(ga) == "1* 2 3" && record("ws-b").hidden, c.names(ga));
    mark = c.log.size();
    with_wm([&](auto& wm) { (void)wm.move_to_workspace(rb.id, ws1_id, false, "test"); });
    settle(c);
    check("removal order: workspace_leave, then removed, then done",
          joined(c.log, mark).find("leave 2, removed 2") != std::string::npos &&
              c.log.back() == "done" && w2->removed,
          joined(c.log, mark));
    rb = record("ws-b");
    check("b is back on 1, shown", !rb.hidden && rb.workspace == ra.workspace, c.names(ga));

    // A request on a removed (inert) handle is ignored, not an error.
    ext_workspace_handle_v1_activate(w2->proxy);
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);
    check("requests on an inert handle are ignored", protocol_ok(c) && c.names(ga) == "1* 2",
          c.names(ga));
    ext_workspace_handle_v1_destroy(w2->proxy);
    w2->proxy = nullptr;

    // Minimize (xdg_toplevel.set_minimized): hidden, the keyboard moves on; activate: back.
    xdg_toplevel_set_minimized(a.toplevel);
    settle(c);
    answer(c, a);
    ra = record("ws-a");
    rb = record("ws-b");
    check("set_minimized: hidden, suspended, the other window has the keyboard",
          ra.minimized && ra.hidden && a.suspended && focus() == rb.id);
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle(c);
    answer(c, a);
    ra = record("ws-a");
    check("activate: unminimized and focused", !ra.minimized && !ra.hidden && focus() == ra.id);

    // Sticky: shown on every workspace of its output.
    with_wm([&](auto& wm) { wm.set_sticky(rb.id, true); });
    settle(c);
    ws_state* w2b = c.of(ga)[1];
    ext_workspace_handle_v1_activate(w2b->proxy);
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);
    ra = record("ws-a");
    rb = record("ws-b");
    check("a sticky window stays shown on another workspace", ra.hidden && !rb.hidden,
          c.names(ga));
    with_wm([&](auto& wm) { wm.set_sticky(rb.id, false); }); // joins workspace 2 here
    settle(c);
    rb = record("ws-b");
    check("unsticking puts it on the active workspace", !rb.hidden && c.names(ga) == "1 2* 3",
          c.names(ga));
    ext_workspace_handle_v1_activate(c.of(ga)[0]->proxy);
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);

    // One commit: assign workspace 1 (with window a) to output B, then activate it there.
    ws_state* moving = c.of(ga)[0];
    mark = c.log.size();
    ext_workspace_handle_v1_activate(moving->proxy); // queued first, run after the assign
    ext_workspace_handle_v1_assign(moving->proxy, gb->proxy);
    ext_workspace_manager_v1_commit(c.manager);
    settle(c);
    answer(c, a);
    ra = record("ws-a");
    lx::rect2i out_b{};
    {
        std::lock_guard<std::mutex> hold(*g_lock);
        out_b = g_comp->windows().output_area(1);
    }
    check("assign + activate in one commit: the workspace is on B and active there",
          moving->group == gb->proxy && (moving->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE),
          c.names(ga) + " | " + c.names(gb) + " log: " + joined(c.log, mark));
    {
        unsigned dones = 0;
        for (std::size_t i = mark; i < c.log.size(); ++i)
            dones += c.log[i] == "done";
        check("the commit is one change: one done", dones == 1, joined(c.log, mark));
    }
    check("its window moved onto B's area and is shown",
          !ra.hidden && ra.geometry.x >= out_b.x && ra.geometry.x < out_b.x + out_b.width,
          "x " + std::to_string(ra.geometry.x) + " B at " + std::to_string(out_b.x));
    check("A shows a workspace of its own", !c.names(ga).empty() &&
                                                c.names(ga).find('*') != std::string::npos,
          c.names(ga));

    // stop: finished, no more events, no error.
    ext_workspace_manager_v1_stop(c.manager);
    settle(c);
    check("stop: finished", c.finished && protocol_ok(c));
    wl_display_disconnect(c.display);

    // A second client binds after all that: it sees the same state.
    client d{};
    if (connect(d) && d.ws_name) {
        bind_manager(d);
        settle(d);
        group_state* db = d.group_of(d.outputs[1]);
        check("a new client sees the same workspaces", db && d.names(db) == c.names(gb),
              (db ? d.names(db) : "-") + " vs " + c.names(gb));
        wl_display_disconnect(d.display);
    } else {
        check("a second client connects", false);
    }
}

} // namespace

int main() {
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 30.0;
    cfg.virtual_outputs = "1280x720,1280x720";
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
