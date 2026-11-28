// zwlr_foreign_toplevel_management_v1 (wlr-foreign-toplevel-management-unstable-v1, served at
// version 3) and the window manager, in-process against a real compositor with two virtual
// outputs: the bind (one handle per mapped window, bottom to top; each handle's opening batch
// of title, app_id, state, output_enter, parent and done; a child listed before its parent),
// a window mapped later, changes (title, app id, each state bit, an output change by a
// workspace switch, a move and an assign, a parent that maps and unmaps), activated following
// the focus decision, closed and the inert handle, stop → finished, versions 1 and 2 (no
// parent event, no fullscreen state before their versions), a wl_output bound later, the
// requests (activate, close, minimize, maximize, fullscreen — seen through the window client's
// configures and the compositor's own state), set_rectangle and invalid_rectangle, the session
// lock, and that no event names an object the client destroyed. That sandboxed clients do not
// see the global is checked by test_window_protocols with the other session-wide globals.
#include <errno.h>
#include <stdio.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE) &&                                                            \
    defined(LUMEN_PROTOCOL_WLR_FOREIGN_TOPLEVEL_MANAGEMENT_UNSTABLE_V1)
#include "xdg-shell-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#define LUMEN_TEST_WLR_FOREIGN_TOPLEVEL 1
#endif
#include <stdint.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.compositor;

#ifndef LUMEN_TEST_WLR_FOREIGN_TOPLEVEL

int main() {
    std::printf("SKIP: needs libwayland and wlr-foreign-toplevel-management's glue\n");
    return 0;
}

#else

namespace {

const char* const k_socket = "lumen-test-foreign-toplevel";
/// The protocol's `state` values.
constexpr unsigned st_maximized = 0;
constexpr unsigned st_minimized = 1;
constexpr unsigned st_activated = 2;
constexpr unsigned st_fullscreen = 3;

int g_failed = 0;
int g_ran = 0;
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;

void check(const char* name, bool ok, const std::string& detail = {}) {
    ++g_ran;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ",
                detail.c_str());
    std::fflush(stdout); // the compositor's own lines go to stderr: keep the two readable
    if (!ok)
        ++g_failed;
}

// ── a client's view of the protocol, and a log of what arrived in order ──

/// One event: `text` as printed (`state h=1 activated`), `brief` for its handle (`state
/// activated`), and which handle (`~0u`: the manager's own).
struct logged {
    std::string text;
    std::string brief;
    unsigned handle = ~0u;
    const char* name = "";
};

struct tl {
    zwlr_foreign_toplevel_handle_v1* proxy = nullptr;
    std::string title;
    std::string app_id;
    /// The `state` values of the last state event, as bits (1 << value).
    unsigned state = 0;
    std::vector<wl_output*> outputs;
    zwlr_foreign_toplevel_handle_v1* parent = nullptr;
    bool closed = false;
    unsigned dones = 0;
};

struct client {
    wl_display* display = nullptr;
    /// Proxies this client created beyond the globals; disconnect destroys them
    /// (libwayland 1.26 frees no remaining proxy at wl_display_disconnect).
    std::vector<wl_proxy*> created;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    /// A surface of its own (set_rectangle's).
    wl_surface* surface = nullptr;
    bool bind_outputs = true;
    std::vector<wl_output*> outputs;
    std::vector<uint32_t> output_names;
    uint32_t ftl_name = 0;
    uint32_t ftl_version = 0;
    uint32_t lock_name = 0;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    /// Null once `finished` destroyed its proxy.
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    bool finished = false;
    std::vector<std::unique_ptr<tl>> toplevels;
    std::vector<logged> log;
    unsigned parent_events = 0;
    /// state events whose array carried the fullscreen state (version 2's).
    unsigned fullscreen_states = 0;

    tl* at(unsigned h) const { return h < toplevels.size() ? toplevels[h].get() : nullptr; }
    /// The handle of the window titled `title` (null: none, or its handle went).
    tl* titled(const char* title) {
        for (auto& t : toplevels)
            if (t->proxy && t->title == title)
                return t.get();
        return nullptr;
    }
    unsigned index_of(const tl* t) const {
        for (unsigned i = 0; i < toplevels.size(); ++i)
            if (toplevels[i].get() == t)
                return i;
        return ~0u;
    }
    int output_index(wl_output* o) const {
        for (unsigned i = 0; i < outputs.size(); ++i)
            if (outputs[i] == o)
                return static_cast<int>(i);
        return -1;
    }
};

unsigned index_of(const client& c, zwlr_foreign_toplevel_handle_v1* p) {
    for (unsigned i = 0; i < c.toplevels.size(); ++i)
        if (c.toplevels[i]->proxy == p)
            return i;
    return ~0u;
}

std::string state_text(unsigned bits) {
    std::string s;
    static const char* const k_names[] = {"maximized", "minimized", "activated", "fullscreen"};
    for (unsigned v = 0; v < 4; ++v)
        if (bits & (1u << v)) {
            if (!s.empty())
                s += ',';
            s += k_names[v];
        }
    return s.empty() ? "-" : s;
}

void note(client& c, const char* name, zwlr_foreign_toplevel_handle_v1* p,
          const std::string& arg = {}) {
    logged e;
    e.handle = p ? index_of(c, p) : ~0u;
    e.name = name;
    e.brief = arg.empty() ? std::string(name) : std::string(name) + " " + arg;
    e.text = arg.empty() ? std::format("{} h={}", name, e.handle)
                         : std::format("{} h={} {}", name, e.handle, arg);
    c.log.push_back(std::move(e));
}

const zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, const char* title) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p))) {
            t->title = title ? title : "";
            note(*c, "title", p, t->title);
        }
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, const char* app_id) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p))) {
            t->app_id = app_id ? app_id : "";
            note(*c, "app_id", p, t->app_id);
        }
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, wl_output* output) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p))) {
            t->outputs.push_back(output);
            note(*c, "output_enter", p, std::to_string(c->output_index(output)));
        }
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, wl_output* output) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p))) {
            std::erase(t->outputs, output);
            note(*c, "output_leave", p, std::to_string(c->output_index(output)));
        }
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, wl_array* states) {
        auto* c = static_cast<client*>(d);
        unsigned bits = 0;
        const auto* v = static_cast<const uint32_t*>(states->data);
        for (std::size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
            bits |= 1u << v[i];
            if (v[i] == st_fullscreen)
                ++c->fullscreen_states;
        }
        if (tl* t = c->at(index_of(*c, p))) {
            t->state = bits;
            note(*c, "state", p, state_text(bits));
        }
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p)))
            ++t->dones;
        note(*c, "done", p);
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p) {
        auto* c = static_cast<client*>(d);
        if (tl* t = c->at(index_of(*c, p)))
            t->closed = true;
        note(*c, "closed", p);
    },
    [](void* d, zwlr_foreign_toplevel_handle_v1* p, zwlr_foreign_toplevel_handle_v1* parent) {
        auto* c = static_cast<client*>(d);
        ++c->parent_events;
        if (tl* t = c->at(index_of(*c, p)))
            t->parent = parent;
        note(*c, "parent", p, parent ? "-> h=" + std::to_string(index_of(*c, parent)) : "-> null");
    }};

const zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    [](void* d, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* h) {
        auto* c = static_cast<client*>(d);
        auto t = std::make_unique<tl>();
        t->proxy = h;
        c->toplevels.push_back(std::move(t));
        // Server-pushed proxies are this client's to destroy too; track them so
        // disconnect owns whatever the case did not destroy.
        c->created.push_back(reinterpret_cast<wl_proxy*>(h));
        zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, c);
        note(*c, "toplevel", h);
    },
    [](void* d, zwlr_foreign_toplevel_manager_v1*) {
        auto* c = static_cast<client*>(d);
        c->finished = true;
        // A destructor event: libwayland destroys the proxy once this returns.
        c->manager = nullptr;
        c->log.push_back({"finished", "finished", ~0u, "finished"});
    }};

const wl_output_listener output_listener = {
    [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
       int32_t) {},
    [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    [](void*, wl_output*) {},
    [](void*, wl_output*, int32_t) {},
    [](void*, wl_output*, const char*) {},
    [](void*, wl_output*, const char*) {}};

/// Binds the wl_output global `name` (the registry's number, kept by `output_names`).
void bind_output(client& c, wl_registry* r, uint32_t name) {
    wl_output* o = static_cast<wl_output*>(wl_registry_bind(r, name, &wl_output_interface, 4));
    wl_output_add_listener(o, &output_listener, nullptr);
    c.created.push_back(reinterpret_cast<wl_proxy*>(o));
    c.outputs.push_back(o);
}

void on_global(void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    auto* c = static_cast<client*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0)
        c->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 4));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        c->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 6 ? version : 6));
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        c->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !c->seat)
        c->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, wl_output_interface.name) == 0 && version >= 4) {
        c->output_names.push_back(name); // in output order: the globals are created in it
        if (c->bind_outputs)
            bind_output(*c, r, name);
    } else if (std::strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        c->ftl_name = name;
        c->ftl_version = version;
    } else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0)
        c->lock_name = name;
}
const wl_registry_listener registry_listener = {on_global, [](void*, wl_registry*, uint32_t) {}};

bool connect(client& c) {
    c.display = wl_display_connect(k_socket);
    if (!c.display)
        return false;
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    return wl_display_roundtrip(c.display) >= 0 && wl_display_roundtrip(c.display) >= 0;
}

/// Takes a proxy off the tracked list after its typed destroy request ran (a second
/// wl_proxy_destroy would be UB).
void untrack(client& c, wl_proxy* p) {
    if (auto it = std::find(c.created.begin(), c.created.end(), p); it != c.created.end())
        c.created.erase(it);
}

void disconnect(client& c) {
    (void)wl_display_dispatch_pending(c.display);
    for (auto it = c.created.rbegin(); it != c.created.rend(); ++it) {
        if (std::find(it + 1, c.created.rend(), *it) != c.created.rend())
            continue; // tracked twice: destroy once, at its oldest position
        wl_proxy_destroy(*it);
    }
    c.created.clear();
    if (c.manager)
        wl_proxy_destroy(reinterpret_cast<wl_proxy*>(c.manager));
    if (c.registry)
        wl_registry_destroy(c.registry);
    wl_display_disconnect(c.display);
}

/// The manager, bound now (the toplevel events start at the bind), at `version`.
void bind_manager(client& c, int version) {
    c.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(
        wl_registry_bind(c.registry, c.ftl_name, &zwlr_foreign_toplevel_manager_v1_interface,
                         static_cast<uint32_t>(version)));
    zwlr_foreign_toplevel_manager_v1_add_listener(c.manager, &manager_listener, &c);
}

/// The client destroys a handle it was given (not the window: that stays mapped).
void destroy_handle(client& c, tl& t) {
    untrack(c, reinterpret_cast<wl_proxy*>(t.proxy));
    zwlr_foreign_toplevel_handle_v1_destroy(t.proxy);
    t.proxy = nullptr;
}

void settle(client& c) {
    if (!c.display)
        return;
    (void)wl_display_roundtrip(c.display);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)wl_display_roundtrip(c.display);
}

/// Reads what arrived since the last settle: one roundtrip.
void drain(client& c) {
    if (c.display)
        (void)wl_display_roundtrip(c.display);
}

template <typename... Cs>
void settle_all(Cs*... cs) {
    (settle(*cs), ...);
}

template <typename... Cs>
void drain_all(Cs*... cs) {
    (drain(*cs), ...);
}

bool protocol_ok(client& c) {
    return wl_display_get_error(c.display) == 0;
}

// ── the log, read per handle ──

std::string joined(const std::vector<logged>& v, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < v.size(); ++i)
        s += (i > from ? ", " : "") + v[i].text;
    return s;
}

/// Handle `h`'s events since `mark`, as "title Alpha 2, done".
std::string per(const client& c, const tl* t, std::size_t mark) {
    const unsigned h = c.index_of(t);
    std::string s;
    bool first = true;
    for (std::size_t i = mark; i < c.log.size(); ++i) {
        if (c.log[i].handle != h)
            continue;
        if (!first)
            s += ", ";
        s += c.log[i].brief;
        first = false;
    }
    return s;
}

/// Handle `h`'s events since `mark`, by name only: "title, done".
std::string names(const client& c, const tl* t, std::size_t mark) {
    const unsigned h = c.index_of(t);
    std::string s;
    bool first = true;
    for (std::size_t i = mark; i < c.log.size(); ++i) {
        if (c.log[i].handle != h)
            continue;
        if (!first)
            s += ", ";
        s += c.log[i].name;
        first = false;
    }
    return s;
}

/// Where in the log handle `t`'s first event named `name` since `mark` came (npos: never).
std::size_t position(const client& c, const tl* t, const char* name, std::size_t mark) {
    const unsigned h = c.index_of(t);
    for (std::size_t i = mark; i < c.log.size(); ++i)
        if (c.log[i].handle == h && std::strcmp(c.log[i].name, name) == 0)
            return i;
    return std::string::npos;
}

/// Every event of every handle since `mark` is closed by one done of that handle before
/// anything else of it arrives; a `closed` is that handle's last. Nothing else is sent.
bool batches_closed(const client& c, std::size_t mark) {
    std::vector<unsigned> handles;
    for (std::size_t i = mark; i < c.log.size(); ++i)
        if (c.log[i].handle != ~0u && std::ranges::find(handles, c.log[i].handle) == handles.end())
            handles.push_back(c.log[i].handle);
    for (const unsigned h : handles) {
        std::vector<const char*> seq;
        for (std::size_t i = mark; i < c.log.size(); ++i)
            if (c.log[i].handle == h)
                seq.push_back(c.log[i].name);
        std::size_t i = 0;
        if (i < seq.size() && std::strcmp(seq[i], "toplevel") == 0)
            ++i;
        while (i < seq.size()) {
            if (std::strcmp(seq[i], "closed") == 0)
                return i + 1 == seq.size();
            std::size_t j = i;
            while (j < seq.size() && std::strcmp(seq[j], "done") != 0) {
                if (std::strcmp(seq[j], "closed") == 0 || std::strcmp(seq[j], "toplevel") == 0)
                    return false;
                ++j;
            }
            if (j == seq.size() || j == i)
                return false; // a batch left open, or a done that closed nothing
            i = j + 1;
        }
    }
    return true;
}

// ── an application window ──

struct window {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t serial = 0;
    /// The newest configure acked.
    uint32_t acked = 0;
    /// The size drawn, and the size the newest configure asked for (0: the client's choice).
    int width = 200;
    int height = 150;
    int want_w = 0;
    int want_h = 0;
    bool suspended = false;
    bool activated = false;
    bool maximized = false;
    bool fullscreen = false;
    unsigned configures = 0;
    unsigned closes = 0;
    bool mapped = false;
};

const xdg_surface_listener xdg_listener = {
    [](void* d, xdg_surface*, uint32_t serial) { static_cast<window*>(d)->serial = serial; }};
const xdg_toplevel_listener toplevel_listener = {
    [](void* d, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
        auto* w = static_cast<window*>(d);
        w->suspended = w->activated = w->maximized = w->fullscreen = false;
        w->want_w = width;
        w->want_h = height;
        const auto* s = static_cast<const uint32_t*>(states->data);
        for (std::size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
            if (s[i] == XDG_TOPLEVEL_STATE_SUSPENDED)
                w->suspended = true;
            if (s[i] == XDG_TOPLEVEL_STATE_ACTIVATED)
                w->activated = true;
            if (s[i] == XDG_TOPLEVEL_STATE_MAXIMIZED)
                w->maximized = true;
            if (s[i] == XDG_TOPLEVEL_STATE_FULLSCREEN)
                w->fullscreen = true;
        }
        ++w->configures;
    },
    [](void* d, xdg_toplevel*) { ++static_cast<window*>(d)->closes; },
    [](void*, xdg_toplevel*, int32_t, int32_t) {}, [](void*, xdg_toplevel*, wl_array*) {}};

wl_buffer* make_buffer(client& c, int w, int h) {
    const int stride = w * 4;
    const int size = stride * h;
    const int fd = memfd_create("lumen-test-ftl", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, size);
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    c.created.push_back(reinterpret_cast<wl_proxy*>(b));
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/// The window's objects and its initial configure. Not mapped.
void create_window(client& c, window& w, const char* app_id, const char* title) {
    w = {};
    w.surface = wl_compositor_create_surface(c.compositor);
    c.created.push_back(reinterpret_cast<wl_proxy*>(w.surface));
    w.xdg = xdg_wm_base_get_xdg_surface(c.wm_base, w.surface);
    c.created.push_back(reinterpret_cast<wl_proxy*>(w.xdg));
    xdg_surface_add_listener(w.xdg, &xdg_listener, &w);
    w.toplevel = xdg_surface_get_toplevel(w.xdg);
    c.created.push_back(reinterpret_cast<wl_proxy*>(w.toplevel));
    xdg_toplevel_add_listener(w.toplevel, &toplevel_listener, &w);
    xdg_toplevel_set_app_id(w.toplevel, app_id);
    xdg_toplevel_set_title(w.toplevel, title);
    wl_surface_commit(w.surface);
    (void)wl_display_roundtrip(c.display);
}

void map_window(client& c, window& w) {
    xdg_surface_ack_configure(w.xdg, w.serial);
    wl_surface_attach(w.surface, make_buffer(c, 200, 150), 0, 0);
    wl_surface_commit(w.surface);
    w.mapped = true;
    settle(c);
}

void map_window(client& c, window& w, const char* app_id, const char* title) {
    create_window(c, w, app_id, title);
    map_window(c, w);
}

/// Acks the newest configure with a commit (what a client does), drawn at the size it asked
/// for. The configure is read first: one the compositor sent since this client last
/// dispatched is the one meant.
void answer(client& c, window& w) {
    (void)wl_display_roundtrip(c.display);
    xdg_surface_ack_configure(w.xdg, w.serial);
    w.acked = w.serial;
    if (w.want_w > 0 && w.want_h > 0 && (w.want_w != w.width || w.want_h != w.height)) {
        w.width = w.want_w;
        w.height = w.want_h;
        wl_surface_attach(w.surface, make_buffer(c, w.width, w.height), 0, 0);
    }
    wl_surface_commit(w.surface);
    settle(c);
}

/// The null-buffer commit: the window unmaps.
void unmap_window(client& c, window& w) {
    wl_surface_attach(w.surface, nullptr, 0, 0);
    wl_surface_commit(w.surface);
    w.mapped = false;
    settle(c);
}

// ── the compositor's own state ──

/// The window manager sends the configures it can: the frame tick's job in the compositor,
/// and this harness has no tick.
void flush_configures() {
    std::lock_guard<std::mutex> hold(*g_lock);
    g_comp->windows().flush_configures(std::chrono::steady_clock::now().time_since_epoch().count());
    g_comp->wayland().flush();
}

/// Answers every configure the window manager has for the window: the one in flight, then
/// those it held back behind it (one is in flight at a time) — the frame tick's work, which
/// this harness does not do — until none is left.
void answer_all(client& c, window& w) {
    for (unsigned i = 0; i < 8; ++i) {
        flush_configures();
        (void)wl_display_roundtrip(c.display);
        if (w.serial == w.acked)
            break; // nothing is left to answer
        answer(c, w);
    }
}

struct rec_copy {
    lx::toplevel_id id{};
    lx::rect2i geometry{};
    lx::workspace_id workspace{};
    lx::toplevel_id parent{};
    bool hidden = false;
    bool minimized = false;
    bool mapped = false;
    lx::toplevel_state state = lx::toplevel_state::normal;
};

/// The newest record of a window with this app id.
rec_copy record(const char* app_id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    rec_copy out{};
    g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r) {
        if (r.app_id && std::strcmp(r.app_id, app_id) == 0 && r.id.id() > out.id.id())
            out = {r.id,     r.geometry,  r.workspace, r.parent,
                   r.hidden, r.minimized, r.mapped,    r.state};
    });
    return out;
}

lx::toplevel_id focus() {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().keyboard_focus();
}

/// The window is the top of the stacking order.
bool on_top(lx::toplevel_id id) {
    std::lock_guard<std::mutex> hold(*g_lock);
    const auto& t = g_comp->toplevels();
    return id && t.stacking_index(id) + 1 == t.mapped_count();
}

bool above(lx::toplevel_id id, lx::toplevel_id other) {
    std::lock_guard<std::mutex> hold(*g_lock);
    const auto& t = g_comp->toplevels();
    return t.stacking_index(other) < t.stacking_index(id);
}

lx::rect2i output_area(int slot) {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().output_area(slot);
}

lx::rect2i usable_area(int slot) {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().usable_area(slot);
}

lx::workspace_id workspace_at(unsigned slot, unsigned index_1based) {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().workspaces().by_index(slot, index_1based);
}

template <typename Fn>
void with_wm(Fn&& fn) {
    std::lock_guard<std::mutex> hold(*g_lock);
    fn(g_comp->windows());
    g_comp->wayland().flush();
}

std::string rect_text(lx::rect2i r) {
    return std::format("{},{},{}x{}", r.x, r.y, r.width, r.height);
}

bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

const ext_session_lock_v1_listener lock_listener = {[](void*, ext_session_lock_v1*) {},  // locked
                                                    [](void*, ext_session_lock_v1*) {}}; // finished

// ── the cases' clients and windows ──

/// `b3` binds before any window maps; `c3` (version 3), `v1` (1) and `v2` (2) bind once two
/// are mapped — so handle 0 of every bar is alpha and handle 1 is beta.
struct scene {
    client app, b3, c3, v1, v2, d3, late;
    window a, b, p, p2, k, n;
    tl* ba = nullptr; // c3's handle of alpha
    tl* ca = nullptr; // c3's handle of kiddo
};

template <typename Fn>
void for_bars(scene& s, Fn&& fn) {
    for (client* c : {&s.c3, &s.b3, &s.v1, &s.v2})
        fn(*c);
}

/// Every client of the scene reads what it was sent — twice over. A request leaves its
/// client only when that client dispatches, and what it did reaches the others after that:
/// a bar's request reaches the window client in the second pass, the window client's request
/// the bars in the first.
void settle_scene(scene& s) {
    settle_all(&s.app, &s.c3, &s.b3, &s.v1, &s.v2, &s.d3, &s.late);
    drain_all(&s.app, &s.c3, &s.b3, &s.v1, &s.v2, &s.d3, &s.late);
}

/// Every bar's handle `h` got exactly `events` since its own `mark`.
bool bars_agree(const scene& s, const std::size_t marks[4], unsigned h, const std::string& events) {
    unsigned i = 0;
    for (const client* c : {&s.c3, &s.b3, &s.v1, &s.v2}) {
        const tl* t = c->at(h);
        if (!t || per(*c, t, marks[i]) != events)
            return false;
        ++i;
    }
    return true;
}

void take_marks(const scene& s, std::size_t marks[4]) {
    unsigned i = 0;
    for (const client* c : {&s.c3, &s.b3, &s.v1, &s.v2})
        marks[i++] = c->log.size();
}

std::string bars_text(const scene& s, const std::size_t marks[4], unsigned h) {
    std::string out;
    unsigned i = 0;
    for (const client* c : {&s.c3, &s.b3, &s.v1, &s.v2}) {
        out += (i ? " | " : "") + (c->at(h) ? per(*c, c->at(h), marks[i]) : std::string("?"));
        ++i;
    }
    return out;
}

/// `per`, as above, on one bar.
std::string got(const client& c, const tl* t, std::size_t mark) {
    return per(c, t, mark);
}

/// The clients, and the bind with two windows mapped.
bool open_scene(scene& s) {
    if (!connect(s.b3) || s.b3.ftl_version != 3 || s.b3.outputs.size() != 2) {
        check("connect: zwlr_foreign_toplevel_manager_v1 is advertised (version 3), two outputs",
              false);
        return false;
    }
    bind_manager(s.b3, 3);
    settle(s.b3);
    check("bind with no window mapped: no event, no error", s.b3.log.empty() && protocol_ok(s.b3),
          joined(s.b3.log, 0));
    if (!connect(s.app) || !s.app.compositor || !s.app.wm_base || !s.app.shm) {
        check("the window client connects", false);
        return false;
    }

    std::size_t mark = s.b3.log.size();
    map_window(s.app, s.a, "alpha", "Alpha");
    settle(s.b3);
    tl* ba = s.b3.titled("Alpha");
    check("a window mapped later: toplevel, title, app_id, state, output_enter, parent (null), "
          "done — then, as a change, the activated state the focus decision gives it",
          ba && per(s.b3, ba, mark) ==
                    "toplevel, title Alpha, app_id alpha, state -, output_enter 0, parent -> null, "
                    "done, state activated, done",
          joined(s.b3.log, mark));
    check("... output_enter names the client's wl_output of the window's output, once",
          ba && ba->outputs == std::vector<wl_output*>{s.b3.outputs[0]},
          ba ? std::to_string(ba->outputs.size()) + " output(s)" : "no handle");
    check("... activated is the window manager's focus decision", focus() == record("alpha").id,
          "focus " + std::to_string(focus().id()));

    mark = s.b3.log.size();
    map_window(s.app, s.b, "beta", "Beta");
    settle(s.b3);
    tl* bb = s.b3.titled("Beta");
    check("a second window mapped later: the same batch",
          bb && per(s.b3, bb, mark) ==
                    "toplevel, title Beta, app_id beta, state -, output_enter 0, parent -> null, "
                    "done, state activated, done",
          joined(s.b3.log, mark));
    check("... the window that had the keyboard loses activated: one state event, one done",
          ba && per(s.b3, ba, mark) == "state -, done", per(s.b3, ba, mark));

    if (!connect(s.c3) || !connect(s.v1) || !connect(s.v2)) {
        check("three bars connect", false);
        return false;
    }
    bind_manager(s.c3, 3);
    bind_manager(s.v1, 1);
    bind_manager(s.v2, 2);
    settle_scene(s);
    s.ba = s.c3.titled("Alpha");
    tl* cb = s.c3.titled("Beta");
    const std::string opening =
        "toplevel h=0, title h=0 Alpha, app_id h=0 alpha, state h=0 -, output_enter h=0 0, "
        "toplevel h=1, title h=1 Beta, app_id h=1 beta, state h=1 activated, output_enter h=1 0, ";
    check("bind: one toplevel event per mapped window, bottom to top (the stacking order)",
          s.c3.toplevels.size() == 2 && s.ba == s.c3.at(0) && cb == s.c3.at(1) &&
              above(record("beta").id, record("alpha").id),
          joined(s.c3.log, 0));
    check("bind: each opening batch carries title, app_id, state and its output_enter",
          s.ba && cb &&
              names(s.c3, s.ba, 0) ==
                  "toplevel, title, app_id, state, output_enter, parent, done" &&
              names(s.c3, cb, 0) == "toplevel, title, app_id, state, output_enter, parent, done" &&
              s.ba->outputs == std::vector<wl_output*>{s.c3.outputs[0]} &&
              cb->outputs == std::vector<wl_output*>{s.c3.outputs[0]},
          joined(s.c3.log, 0));
    check("bind: parents once every handle of the list exists, then one done each — a handle's "
          "batch is closed only when it is whole",
          joined(s.c3.log, 0) ==
              opening + "parent h=0 -> null, done h=0, parent h=1 -> null, done h=1",
          joined(s.c3.log, 0));
    check("versions 1 and 2 bind the same list, without a parent event",
          s.v1.toplevels.size() == 2 && s.v2.toplevels.size() == 2 && s.v1.parent_events == 0 &&
              s.v2.parent_events == 0 && joined(s.v1.log, 0) == opening + "done h=0, done h=1" &&
              joined(s.v2.log, 0) == opening + "done h=0, done h=1",
          joined(s.v1.log, 0) + " || " + joined(s.v2.log, 0));
    return true;
}

/// What changes: title, app id, each state bit, and the output by a switch, a move and an
/// assign.
void change_cases(scene& s) {
    client& c = s.c3;
    rec_copy ra = record("alpha");
    rec_copy rb = record("beta");
    std::size_t marks[4];
    take_marks(s, marks);

    xdg_toplevel_set_title(s.a.toplevel, "Alpha 2");
    settle_scene(s);
    check("a title change: the title and one done — on every bound client, the other window "
          "untouched",
          bars_agree(s, marks, 0, "title Alpha 2, done") && bars_agree(s, marks, 1, ""),
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));
    take_marks(s, marks);
    xdg_toplevel_set_app_id(s.a.toplevel, "alpha.x");
    settle_scene(s);
    check("an app_id change: the app_id and one done",
          bars_agree(s, marks, 0, "app_id alpha.x, done"), bars_text(s, marks, 0));
    take_marks(s, marks);
    xdg_toplevel_set_title(s.a.toplevel, "Alpha 2");
    settle_scene(s);
    check("the same title again sends nothing",
          bars_agree(s, marks, 0, "") && bars_agree(s, marks, 1, ""), bars_text(s, marks, 0));
    // The id again as it was: the cases look windows up by it.
    xdg_toplevel_set_app_id(s.a.toplevel, "alpha");
    settle_scene(s);

    // activated: the focus decision.
    take_marks(s, marks);
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle_scene(s);
    check("activated moves with the focus decision: the window that gains it and the one that "
          "loses it, one state event and one done each",
          bars_agree(s, marks, 0, "state activated, done") &&
              bars_agree(s, marks, 1, "state -, done") && focus() == ra.id,
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));

    // maximized: the state the client draws for, so it changes with the commit that answers
    // the configure.
    take_marks(s, marks);
    with_wm([&](auto& wm) { wm.set_maximized(ra.id, true, "test"); });
    settle_scene(s);
    check("maximized: nothing is told before the window answered its configure",
          bars_agree(s, marks, 0, ""), bars_text(s, marks, 0));
    answer_all(s.app, s.a);
    settle_scene(s);
    check("maximized: the state event and one done; the window fills the usable area",
          bars_agree(s, marks, 0, "state maximized,activated, done") &&
              record("alpha").state == lx::toplevel_state::maximized &&
              same_rect(record("alpha").geometry, usable_area(0)),
          bars_text(s, marks, 0) + " geometry " + rect_text(record("alpha").geometry));
    take_marks(s, marks);
    with_wm([&](auto& wm) { wm.set_maximized(ra.id, false, "test"); });
    answer_all(s.app, s.a);
    settle_scene(s);
    check("unmaximized: the state event and one done",
          bars_agree(s, marks, 0, "state activated, done") &&
              record("alpha").state == lx::toplevel_state::normal,
          bars_text(s, marks, 0));

    // minimized: no output_leave with it.
    take_marks(s, marks);
    xdg_toplevel_set_minimized(s.a.toplevel);
    settle_scene(s);
    check("minimized: the window is told minimized while it still has the keyboard, then the "
          "keyboard moves on — two changes, each one state event closed by one done",
          bars_agree(s, marks, 0, "state minimized,activated, done, state minimized, done") &&
              bars_agree(s, marks, 1, "state activated, done") && record("alpha").minimized &&
              focus() == rb.id,
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));
    check("... minimizing sends no output_leave: a bar keeps the button that restores the window",
          c.at(0)->outputs == std::vector<wl_output*>{c.outputs[0]} &&
              c.at(1)->outputs == std::vector<wl_output*>{c.outputs[0]});
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_unset_minimized(s.ba->proxy);
    settle_scene(s);
    check("unset_minimized: unminimized, not activated; one state event and one done",
          bars_agree(s, marks, 0, "state -, done") && !record("alpha").minimized,
          bars_text(s, marks, 0));

    // fullscreen: a version 2 state.
    take_marks(s, marks);
    with_wm([&](auto& wm) { wm.set_fullscreen(rb.id, true, -1, "test"); });
    answer_all(s.app, s.b);
    settle_scene(s);
    check("fullscreen: the state event carries fullscreen (version 2's state) and one done",
          got(s.c3, c.at(1), marks[0]) == "state activated,fullscreen, done" &&
              got(s.v2, s.v2.at(1), marks[3]) == "state activated,fullscreen, done" &&
              record("beta").state == lx::toplevel_state::fullscreen &&
              same_rect(record("beta").geometry, output_area(0)),
          got(s.c3, c.at(1), marks[0]) + " || v2: " + got(s.v2, s.v2.at(1), marks[3]) +
              " geometry " + rect_text(record("beta").geometry));
    check("fullscreen: a version 1 client is sent no event its version lacks — no state at all "
          "here, its state did not change",
          got(s.v1, s.v1.at(1), marks[2]).empty() && s.v1.fullscreen_states == 0 &&
              protocol_ok(s.v1),
          got(s.v1, s.v1.at(1), marks[2]));
    take_marks(s, marks);
    with_wm([&](auto& wm) { wm.set_fullscreen(rb.id, false, -1, "test"); });
    answer_all(s.app, s.b);
    settle_scene(s);
    check("unfullscreen: the state event and one done",
          got(s.c3, c.at(1), marks[0]) == "state activated, done" &&
              got(s.v1, s.v1.at(1), marks[2]).empty() &&
              record("beta").state == lx::toplevel_state::normal,
          got(s.c3, c.at(1), marks[0]));

    // The output: a workspace switch shows and hides the workspace's windows.
    const lx::workspace_id ws1 = record("alpha").workspace; // output 0's workspace 1, both on it
    const lx::workspace_id ws2 = workspace_at(0, 2);        // the empty one after it
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.switch_workspace(ws2, "test"); });
    settle_scene(s);
    check("a workspace switch: its windows leave the output (output_leave, one done); the one "
          "with the keyboard loses activated in the same batch",
          bars_agree(s, marks, 0, "output_leave 0, done") &&
              bars_agree(s, marks, 1, "state -, output_leave 0, done") && !focus() &&
              c.at(0)->outputs.empty() && c.at(1)->outputs.empty(),
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));
    check("... nothing else was sent: no title, no state for the window that had none",
          names(c, c.at(0), marks[0]) == "output_leave, done", names(c, c.at(0), marks[0]));
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.switch_workspace(ws1, "test"); });
    settle_scene(s);
    check("back: output_enter again, and the keyboard returns to the window focused there",
          bars_agree(s, marks, 0, "output_enter 0, done") &&
              bars_agree(s, marks, 1, "state activated, output_enter 0, done"),
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));

    // The output: a move to the other output's workspace.
    const lx::workspace_id other = workspace_at(1, 1); // output 1's active workspace
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.move_to_workspace(ra.id, other, false, "test"); });
    settle_scene(s);
    check("a move to the other output's workspace: output_leave, output_enter, one done — the "
          "enter names that output's wl_output",
          bars_agree(s, marks, 0, "output_leave 0, output_enter 1, done") &&
              bars_agree(s, marks, 1, "") && record("alpha").workspace == other &&
              c.at(0)->outputs == std::vector<wl_output*>{c.outputs[1]},
          "alpha: " + bars_text(s, marks, 0));
    check("... and the window is shown there", !record("alpha").hidden);

    // A client that bound no wl_output: its handles are pending on their outputs.
    s.late.bind_outputs = false;
    const bool late_ok = connect(s.late) && s.late.ftl_name;
    if (late_ok) {
        bind_manager(s.late, 3);
        settle(s.late);
    }
    check("a client that bound no wl_output gets its handles without an output_enter",
          late_ok && s.late.toplevels.size() == 2 && s.late.at(0) && s.late.at(1) &&
              std::ranges::none_of(
                  s.late.log,
                  [](const logged& e) { return std::strcmp(e.name, "output_enter") == 0; }),
          joined(s.late.log, 0));
    std::size_t lmark = s.late.log.size();
    tl* lb = s.late.titled("Beta");    // it is below alpha now: alpha was raised
    tl* la = s.late.titled("Alpha 2"); // on output 1
    if (late_ok)
        bind_output(s.late, s.late.registry, s.late.output_names[0]);
    settle(s.late);
    check("a wl_output bound later: the handles on that output get their output_enter, each "
          "closed by a done; the others nothing",
          late_ok && lb && la && got(s.late, lb, lmark) == "output_enter 0, done" &&
              got(s.late, la, lmark).empty(),
          joined(s.late.log, lmark));
    lmark = s.late.log.size();
    if (late_ok)
        bind_output(s.late, s.late.registry, s.late.output_names[1]);
    settle(s.late);
    check("... and the other output's handles with the second wl_output",
          late_ok && lb && la && got(s.late, la, lmark) == "output_enter 1, done" &&
              got(s.late, lb, lmark).empty() && protocol_ok(s.late),
          joined(s.late.log, lmark));

    // Back, then an assign of the whole workspace to the other output.
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.move_to_workspace(ra.id, ws1, false, "test"); });
    settle_scene(s);
    check("... and back", bars_agree(s, marks, 0, "output_leave 1, output_enter 0, done"),
          bars_text(s, marks, 0));
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.assign_workspace(ws1, 1); });
    settle_scene(s);
    check("an assign of the workspace to the other output: its windows leave theirs (it is not "
          "the active one there yet); then the keyboard leaves them — two changes, each closed "
          "by one done",
          bars_agree(s, marks, 0, "output_leave 0, done") &&
              bars_agree(s, marks, 1, "output_leave 0, done, state -, done") &&
              record("alpha").workspace == ws1,
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));
    take_marks(s, marks);
    with_wm([&](auto& wm) { (void)wm.switch_workspace(ws1, "test"); });
    settle_scene(s);
    check("... and enter it once it is that output's active workspace",
          bars_agree(s, marks, 0, "output_enter 1, done") &&
              bars_agree(s, marks, 1, "state activated, output_enter 1, done"),
          "alpha: " + bars_text(s, marks, 0) + " || beta: " + bars_text(s, marks, 1));
    // The workspace returns to output 0, and is shown there again.
    with_wm([&](auto& wm) { (void)wm.assign_workspace(ws1, 0); });
    settle_scene(s);
    with_wm([&](auto& wm) { (void)wm.switch_workspace(ws1, "test"); });
    settle_scene(s);
    check("the workspace back on output 0 and shown: both windows entered there again",
          c.at(0)->outputs == std::vector<wl_output*>{c.outputs[0]} &&
              c.at(1)->outputs == std::vector<wl_output*>{c.outputs[0]} &&
              !record("alpha").hidden && !record("beta").hidden && focus() == rb.id,
          rect_text(record("alpha").geometry));
}

/// The parent: one that maps after its child, one that unmaps, and a list where the child
/// comes first.
void parent_cases(scene& s) {
    client& c = s.c3;
    std::size_t mark = c.log.size();
    // The parent's objects exist, but it is not mapped.
    create_window(s.app, s.p, "papa", "Papa");
    create_window(s.app, s.k, "kiddo", "Kiddo");
    xdg_toplevel_set_parent(s.k.toplevel, s.p.toplevel);
    map_window(s.app, s.k);
    settle_scene(s);
    s.ca = c.titled("Kiddo");
    check("a child of a parent that is not mapped yet: its opening batch carries parent null",
          s.ca && got(c, s.ca, mark) ==
                      "toplevel, title Kiddo, app_id kiddo, state -, output_enter 0, parent -> "
                      "null, done, state activated, done",
          joined(c.log, mark));
    const rec_copy rk = record("kiddo");

    // The parent maps after the child.
    mark = c.log.size();
    map_window(s.app, s.p);
    settle_scene(s);
    tl* cp = c.titled("Papa");
    check("the parent maps after the child: the child is told its parent (that handle), then "
          "done",
          cp && got(c, s.ca, mark) ==
                    "parent -> h=" + std::to_string(c.index_of(cp)) + ", done, state -, done",
          joined(c.log, mark));
    check("... the parent's own batch says it has none",
          cp && got(c, cp, mark) ==
                    "toplevel, title Papa, app_id papa, state -, output_enter 0, parent -> null, "
                    "done, state activated, done",
          cp ? got(c, cp, mark) : "no handle");
    check("... the child is below its parent in the stacking order (it mapped first)",
          above(record("papa").id, rk.id));

    // A client binding now lists the child before its parent.
    if (!connect(s.d3) || !s.d3.ftl_name) {
        check("a fourth bar connects", false);
        return;
    }
    bind_manager(s.d3, 3);
    settle(s.d3);
    tl* dk = s.d3.titled("Kiddo");
    tl* dp = s.d3.titled("Papa");
    check("bind: a child listed before its parent gets parent(that handle) before that batch's "
          "done",
          dk && dp && s.d3.index_of(dk) < s.d3.index_of(dp) &&
              got(s.d3, dk, 0) ==
                  "toplevel, title Kiddo, app_id kiddo, state -, output_enter 0, parent -> h=" +
                      std::to_string(s.d3.index_of(dp)) + ", done" &&
              position(s.d3, dk, "done", 0) > position(s.d3, dp, "toplevel", 0),
          joined(s.d3.log, 0));

    // The parent unmaps: the child is managed as though it had none.
    mark = c.log.size();
    unmap_window(s.app, s.p);
    settle_scene(s);
    check("the parent unmaps: closed for it, and the child is told parent null (one event, "
          "one done) — the keyboard that was its parent's goes to it",
          cp && got(c, cp, mark) == "closed" &&
              got(c, s.ca, mark).rfind("parent -> null, done", 0) == 0,
          "parent: " + got(c, cp, mark) + " || child: " + got(c, s.ca, mark));
}

bool workspace_is_active(lx::workspace_id ws) {
    std::lock_guard<std::mutex> hold(*g_lock);
    return g_comp->windows().workspaces().is_active(ws);
}

/// A fresh client sends set_rectangle on its first handle: true when that killed it with
/// invalid_rectangle on zwlr_foreign_toplevel_handle_v1.
bool dies_of_invalid_rectangle(int x, int y, int width, int height) {
    client e{};
    bool invalid = false;
    if (connect(e) && e.ftl_name) {
        bind_manager(e, 3);
        settle(e);
        e.surface = wl_compositor_create_surface(e.compositor);
        e.created.push_back(reinterpret_cast<wl_proxy*>(e.surface));
        if (tl* t = e.at(0)) {
            zwlr_foreign_toplevel_handle_v1_set_rectangle(t->proxy, e.surface, x, y, width, height);
            (void)wl_display_roundtrip(e.display);
            const wl_interface* iface = nullptr;
            uint32_t id = 0;
            const uint32_t code = wl_display_get_protocol_error(e.display, &iface, &id);
            invalid = wl_display_get_error(e.display) != 0 &&
                      iface == &zwlr_foreign_toplevel_handle_v1_interface &&
                      code == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE;
        }
        disconnect(e);
    }
    return invalid;
}

/// Nothing arrived for handle `h` since `mark`.
bool nothing_since(const client& c, unsigned h, std::size_t mark) {
    for (std::size_t i = mark; i < c.log.size(); ++i)
        if (c.log[i].handle == h)
            return false;
    return true;
}

/// Handle `t` got nothing but state events since `mark`, each batch closed by one done.
bool states_only(const client& c, const tl* t, std::size_t mark) {
    const std::string s = names(c, t, mark);
    unsigned i = 0;
    for (const char* p = s.c_str(); *p;) {
        const char* comma = std::strstr(p, ", ");
        const std::string token(p, comma ? static_cast<std::size_t>(comma - p) : std::strlen(p));
        if (token != (i % 2 == 0 ? "state" : "done"))
            return false;
        ++i;
        if (!comma)
            break;
        p = comma + 2;
    }
    return i != 0 && i % 2 == 0;
}

/// The requests, as the zlm policy bridge's: seen through the window client's configures and
/// the compositor's own state.
void request_cases(scene& s) {
    client& c = s.c3;
    rec_copy ra = record("alpha");
    const lx::workspace_id ws2 = workspace_at(0, 2);
    tl* cb = c.titled("Beta");
    std::size_t marks[4];
    settle_all(&s.app, &c);
    c.surface = wl_compositor_create_surface(c.compositor);
    c.created.push_back(reinterpret_cast<wl_proxy*>(c.surface));

    // activate: on a minimized window of a workspace that is not shown.
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle_scene(s);
    check("output 0 has an empty workspace 2 to switch to", ws2 != lx::workspace_id{});
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_set_minimized(s.ba->proxy);
    settle_scene(s);
    check("set_minimized: the window is minimized — told in state events, each closed by one "
          "done (it had the keyboard: minimized first, then the keyboard moving on)",
          record("alpha").minimized && states_only(c, s.ba, marks[0]), got(c, s.ba, marks[0]));
    with_wm([&](auto& wm) { (void)wm.switch_workspace(ws2, "test"); });
    settle_scene(s);
    check("... the workspace switched away: the window is hidden and its handle left the "
          "output",
          record("alpha").hidden && c.at(0)->outputs.empty(),
          std::format("hidden {} outputs {}", record("alpha").hidden ? 1 : 0,
                      c.at(0)->outputs.size()));

    // The request of a task-list click, as the shell's request_activate.
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_activate(s.ba->proxy, c.seat);
    settle_scene(s);
    const bool active = workspace_is_active(ra.workspace);
    const bool raised = on_top(ra.id);
    // The window answers its configures, the one carrying activated included.
    answer_all(s.app, s.a);
    settle_scene(s);
    ra = record("alpha");
    check("activate: the window is unminimized, its workspace is switched to and shown, it has "
          "the keyboard and it is raised",
          !ra.minimized && !ra.hidden && active && focus() == ra.id && raised,
          std::format("minimized {} hidden {} workspace active {} focus {} top {}",
                      ra.minimized ? 1 : 0, ra.hidden ? 1 : 0, active ? 1 : 0, focus() == ra.id,
                      raised ? 1 : 0));
    check("... its client's configure carries activated", s.a.activated && !s.a.suspended,
          std::format("activated {} suspended {}", s.a.activated, s.a.suspended));
    check("... the handle is told what changed, each batch closed by one done",
          got(c, s.ba, marks[0]) == "state -, done, output_enter 0, done, state activated, done",
          got(c, s.ba, marks[0]));

    // close: xdg_toplevel.close to the window's client.
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_close(cb->proxy);
    settle_scene(s);
    check("close: the window's client is asked to close (xdg_toplevel.close); the handle stays "
          "until the window goes",
          s.b.closes == 1 && record("beta").mapped && bars_agree(s, marks, 1, ""),
          "closes " + std::to_string(s.b.closes) + " " + bars_text(s, marks, 1));
    take_marks(s, marks);
    unmap_window(s.app, s.b);
    settle_scene(s);
    check("... and when it does: closed, the handle's last event",
          bars_agree(s, marks, 1, "closed"), bars_text(s, marks, 1));

    // set_maximized / unset_maximized.
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_set_maximized(s.ba->proxy);
    settle_scene(s);
    check("set_maximized: the window is asked first (its configure carries maximized); nothing "
          "is told before it drew it",
          s.a.maximized && bars_agree(s, marks, 0, ""), bars_text(s, marks, 0));
    answer_all(s.app, s.a);
    settle_scene(s);
    check("... then the state event and one done; the window fills the usable area",
          bars_agree(s, marks, 0, "state maximized,activated, done") &&
              record("alpha").state == lx::toplevel_state::maximized &&
              same_rect(record("alpha").geometry, usable_area(0)),
          bars_text(s, marks, 0) + " geometry " + rect_text(record("alpha").geometry) + " usable " +
              rect_text(usable_area(0)));
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_unset_maximized(s.ba->proxy);
    settle_scene(s);
    answer_all(s.app, s.a);
    settle_scene(s);
    check("unset_maximized: the normal state again — the state event and one done",
          bars_agree(s, marks, 0, "state activated, done") &&
              record("alpha").state == lx::toplevel_state::normal,
          bars_text(s, marks, 0));

    // set_fullscreen(output) / unset_fullscreen: on the other output.
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(s.ba->proxy, c.outputs[1]);
    settle_scene(s);
    answer_all(s.app, s.a);
    settle_scene(s);
    check("set_fullscreen(output): fullscreen on that output — the state event, output_leave, "
          "output_enter, one done — and its geometry is the output's",
          got(s.c3, s.ba, marks[0]) ==
                  "state activated,fullscreen, output_leave 0, output_enter 1, done" &&
              got(s.v1, s.v1.at(0), marks[2]) == "output_leave 0, output_enter 1, done" &&
              record("alpha").state == lx::toplevel_state::fullscreen &&
              same_rect(record("alpha").geometry, output_area(1)),
          got(s.c3, s.ba, marks[0]) + " || v1: " + got(s.v1, s.v1.at(0), marks[2]) + " geometry " +
              rect_text(record("alpha").geometry) + " output " + rect_text(output_area(1)));
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(s.ba->proxy);
    settle_scene(s);
    answer_all(s.app, s.a);
    settle_scene(s);
    check("unset_fullscreen: the normal state and its own output again — the state event, "
          "output_leave, output_enter, one done",
          got(s.c3, s.ba, marks[0]) == "state activated, output_leave 1, output_enter 0, done" &&
              record("alpha").state == lx::toplevel_state::normal,
          got(s.c3, s.ba, marks[0]));

    // set_rectangle: accepted, and the connection goes on.
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(s.ba->proxy, c.surface, 10, 20, 30, 40);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(s.ba->proxy, c.surface, 0, 0, 0, 0);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(s.ba->proxy, c.surface, 5, 6, 7, 8);
    settle(c);
    check("set_rectangle: a positive rectangle, and width = height = 0, are accepted — no "
          "event, no error",
          protocol_ok(c) && got(c, s.ba, marks[0]).empty(), got(c, s.ba, marks[0]));
    xdg_toplevel_set_title(s.a.toplevel, "Alpha 6");
    settle_scene(s);
    check("... the connection goes on", got(c, s.ba, marks[0]) == "title Alpha 6, done",
          got(c, s.ba, marks[0]));

    // set_rectangle: invalid_rectangle kills the client that sent it.
    const int bad[][4] = {{0, 0, -1, 10}, {0, 0, 10, -5}, {0, 0, 0, 10}, {0, 0, 10, 0}};
    const char* const bad_names[] = {"a negative width", "a negative height",
                                     "width 0 with a height", "height 0 with a width"};
    for (unsigned i = 0; i < 4; ++i)
        check(bad_names[i], dies_of_invalid_rectangle(bad[i][0], bad[i][1], bad[i][2], bad[i][3]),
              std::format("set_rectangle {} {} {} {}", bad[i][0], bad[i][1], bad[i][2], bad[i][3]));
}

/// Behind the session lock every request changes nothing; the events report what the
/// compositor itself does.
void lock_cases(scene& s) {
    client& c = s.c3;
    rec_copy ra = record("alpha");
    with_wm([&](auto& wm) { wm.set_minimized(ra.id, false, "test"); });
    const rec_copy rk = record("kiddo");
    with_wm([&](auto& wm) { wm.activate(rk.id); }); // kiddo has the keyboard, alpha does not
    settle_scene(s);
    tl* ck = c.titled("Kiddo");
    client lk{};
    if (!connect(lk) || !lk.lock_name || !ck || !s.ba) {
        check("a locker connects", false);
        return;
    }
    lk.lock_manager = static_cast<ext_session_lock_manager_v1*>(
        wl_registry_bind(lk.registry, lk.lock_name, &ext_session_lock_manager_v1_interface, 1));
    lk.created.push_back(reinterpret_cast<wl_proxy*>(lk.lock_manager));
    auto* lock = ext_session_lock_manager_v1_lock(lk.lock_manager);
    lk.created.push_back(reinterpret_cast<wl_proxy*>(lock));
    ext_session_lock_v1_add_listener(lock, &lock_listener, nullptr);
    settle_scene(s);
    settle(lk);

    std::size_t mark = c.log.size();
    const lx::toplevel_id focus_before = focus();
    const unsigned closes_before = s.a.closes;
    const unsigned configures_before = s.a.configures;
    zwlr_foreign_toplevel_handle_v1_activate(s.ba->proxy, c.seat);
    zwlr_foreign_toplevel_handle_v1_set_minimized(ck->proxy);
    zwlr_foreign_toplevel_handle_v1_unset_minimized(ck->proxy);
    zwlr_foreign_toplevel_handle_v1_set_maximized(s.ba->proxy);
    zwlr_foreign_toplevel_handle_v1_unset_maximized(s.ba->proxy);
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(s.ba->proxy, c.outputs[1]);
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(s.ba->proxy);
    zwlr_foreign_toplevel_handle_v1_close(s.ba->proxy);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(s.ba->proxy, c.surface, 0, 0, 0, 10);
    settle_scene(s);
    ra = record("alpha");
    check("while the session is locked every request changes nothing — the keyboard, the "
          "windows, their clients (no close, no configure) — and none is an error",
          protocol_ok(c) && focus() == focus_before && !record("kiddo").minimized &&
              ra.state == lx::toplevel_state::normal && !ra.minimized &&
              s.a.closes == closes_before && s.a.configures == configures_before &&
              c.log.size() == mark,
          std::format("focus moved {} closes {}->{} configures {}->{} events {}",
                      focus() != focus_before, closes_before, s.a.closes, configures_before,
                      s.a.configures, c.log.size() - mark));

    // The compositor itself still acts, and the events report it.
    mark = c.log.size();
    with_wm([&](auto& wm) { wm.set_minimized(rk.id, true, "test"); });
    settle_scene(s);
    check("... the events keep reporting what the compositor itself does",
          record("kiddo").minimized && states_only(c, ck, mark), got(c, ck, mark));

    // Unlocked: the requests act again.
    ext_session_lock_v1_destroy(lock);
    untrack(lk, reinterpret_cast<wl_proxy*>(lock));
    lock = nullptr;
    settle_scene(s);
    settle(lk);
    with_wm([&](auto& wm) { wm.set_minimized(rk.id, false, "test"); });
    settle_scene(s);
    mark = c.log.size();
    zwlr_foreign_toplevel_handle_v1_set_minimized(s.ba->proxy);
    settle_scene(s);
    check("unlocked: a request changes the window again",
          record("alpha").minimized && states_only(c, s.ba, mark), got(c, s.ba, mark));
    disconnect(lk);
}

/// Objects the client destroyed are named no more; stop; the inert handle after closed.
void lifetime_cases(scene& s) {
    client& c = s.c3;
    rec_copy ra = record("alpha");
    std::size_t mark = c.log.size();
    std::size_t marks[4];

    // A parent of a mapped child, whose handle the client then destroys.
    create_window(s.app, s.p2, "papa2", "Papa 2");
    map_window(s.app, s.p2);
    settle_scene(s);
    tl* c2 = c.titled("Papa 2");
    mark = c.log.size();
    xdg_toplevel_set_parent(s.k.toplevel, s.p2.toplevel);
    settle_scene(s);
    check("a parent set on a mapped child: parent (its handle) and one done",
          c2 && got(c, s.ca, mark) == "parent -> h=" + std::to_string(c.index_of(c2)) + ", done",
          got(c, s.ca, mark));
    destroy_handle(c, *c2);
    mark = c.log.size();
    unmap_window(s.app, s.p2);
    settle_scene(s);
    check("a parent whose handle the client destroyed, unmapping: the child is told parent null "
          "— nothing names the destroyed handle, no error",
          protocol_ok(c) && got(c, s.ca, mark).rfind("parent -> null, done", 0) == 0,
          got(c, s.ca, mark));

    // The client destroys its wl_output of output 1; a window goes fullscreen there.
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle_scene(s);
    wl_output* gone = c.outputs[1];
    untrack(c, reinterpret_cast<wl_proxy*>(gone));
    wl_output_release(gone);
    c.outputs[1] = nullptr;
    settle_scene(s);
    mark = c.log.size();
    with_wm([&](auto& wm) { wm.set_fullscreen(ra.id, true, 1, "test"); });
    settle_scene(s);
    answer_all(s.app, s.a);
    settle_scene(s);
    check("a window entering an output whose wl_output the client destroyed: no output_enter "
          "names it (no error); the rest of the change is told",
          protocol_ok(c) &&
              got(c, s.ba, mark) == "state activated,fullscreen, output_leave 0, done",
          got(c, s.ba, mark) + " err " + std::to_string(wl_display_get_error(c.display)));
    mark = c.log.size();
    with_wm([&](auto& wm) { wm.set_fullscreen(ra.id, false, -1, "test"); });
    settle_scene(s);
    answer_all(s.app, s.a);
    settle_scene(s);
    check("... and leaving it: no output_leave names it; the enter on the output it still "
          "holds is",
          protocol_ok(c) && got(c, s.ba, mark) == "state activated, output_enter 0, done",
          got(c, s.ba, mark));

    // stop: finished, the last event of the manager.
    std::size_t dmark = s.d3.log.size();
    const unsigned handles = s.d3.toplevels.size();
    zwlr_foreign_toplevel_manager_v1_stop(s.d3.manager);
    settle(s.d3);
    check("stop: finished — the manager's last event — and no error",
          s.d3.finished && s.d3.manager == nullptr && protocol_ok(s.d3) &&
              joined(s.d3.log, dmark) == "finished",
          joined(s.d3.log, dmark));
    map_window(s.app, s.n, "nova", "Nova");
    settle_scene(s);
    check("stop: no further toplevel event reaches that client; another manager still gets the "
          "window",
          s.d3.toplevels.size() == handles && !s.d3.titled("Nova") && c.titled("Nova"),
          std::format("handles {} -> {}", handles, s.d3.toplevels.size()));
    with_wm([&](auto& wm) { wm.activate(ra.id); });
    settle_scene(s);
    tl* da = s.d3.titled("Alpha 6");
    dmark = s.d3.log.size();
    zwlr_foreign_toplevel_handle_v1_set_minimized(da->proxy);
    settle_scene(s);
    check("... the handles it made are the client's objects and go on working: a request on "
          "them still acts, without an error",
          record("alpha").minimized && protocol_ok(s.d3) && states_only(s.d3, da, dmark),
          got(s.d3, da, dmark));
    with_wm([&](auto& wm) { wm.set_minimized(ra.id, false, "test"); });
    settle_scene(s);

    // A window that unmaps: closed, and the handle is inert.
    tl* cn = c.titled("Nova");
    if (!cn) {
        check("the new window has a handle", false);
        return;
    }
    const unsigned hn = c.index_of(cn);
    take_marks(s, marks);
    unmap_window(s.app, s.n);
    settle_scene(s);
    check("a window that unmaps: closed, on every manager — the last event of its handle",
          bars_agree(s, marks, hn, "closed"), bars_text(s, marks, hn));
    ra = record("alpha");
    const lx::toplevel_id focus_before = focus();
    const unsigned closes_before = s.a.closes;
    const unsigned configures_before = s.a.configures;
    take_marks(s, marks);
    zwlr_foreign_toplevel_handle_v1_set_maximized(cn->proxy);
    zwlr_foreign_toplevel_handle_v1_unset_maximized(cn->proxy);
    zwlr_foreign_toplevel_handle_v1_set_minimized(cn->proxy);
    zwlr_foreign_toplevel_handle_v1_unset_minimized(cn->proxy);
    zwlr_foreign_toplevel_handle_v1_activate(cn->proxy, c.seat);
    zwlr_foreign_toplevel_handle_v1_close(cn->proxy);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(cn->proxy, c.surface, 0, 0, 100, 100);
    zwlr_foreign_toplevel_handle_v1_set_rectangle(cn->proxy, c.surface, -1, 0, 10, 10);
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(cn->proxy, c.outputs[0]);
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(cn->proxy);
    settle_scene(s);
    check("closed: the handle is inert — every request but destroy changes nothing, and none "
          "is an error (an invalid rectangle included)",
          protocol_ok(c) && focus() == focus_before && !record("alpha").minimized &&
              s.a.closes == closes_before && s.a.configures == configures_before &&
              bars_agree(s, marks, hn, ""),
          std::format("focus moved {} closes {}->{} configures {}->{}", focus() != focus_before,
                      closes_before, s.a.closes, configures_before, s.a.configures));
    destroy_handle(c, *cn);
    settle(c);
    check("... destroy works, and the connection stays up", protocol_ok(c));

    // A handle the client destroyed is sent nothing more.
    if (!s.ba) {
        check("alpha's handle is still there", false);
        return;
    }
    const unsigned gone_index = c.index_of(s.ba);
    destroy_handle(c, *s.ba);
    s.ba = nullptr;
    mark = c.log.size();
    xdg_toplevel_set_title(s.a.toplevel, "Alpha 7");
    with_wm([&](auto& wm) { wm.set_minimized(ra.id, true, "test"); });
    settle(c);
    check("a handle the client destroyed: nothing more is sent for it, and no error",
          protocol_ok(c) && nothing_since(c, gone_index, mark));
    with_wm([&](auto& wm) { wm.set_minimized(ra.id, false, "test"); });
    settle_scene(s);
}

void run_cases() {
    scene s{};
    if (open_scene(s)) {
        change_cases(s);
        parent_cases(s);
        request_cases(s);
        lock_cases(s);
        lifetime_cases(s);
        bool closed = true;
        bool up = true;
        for (client* c : {&s.c3, &s.b3, &s.v1, &s.v2, &s.d3, &s.late}) {
            closed = batches_closed(*c, 0) && closed;
            up = protocol_ok(*c) && up;
        }
        check("no event of a handle is left open: each is closed by one done of that handle "
              "before anything else of it arrives",
              closed);
        check("versions: a client is sent no event its version lacks — parent is version 3's, "
              "the fullscreen state version 2's",
              s.c3.parent_events > 0 && s.v1.parent_events == 0 && s.v2.parent_events == 0 &&
                  s.v1.fullscreen_states == 0 && s.v2.fullscreen_states > 0 &&
                  s.c3.fullscreen_states > 0,
              std::format("parent events v3 {} v2 {} v1 {}; fullscreen states v3 {} v2 {} v1 {}",
                          s.c3.parent_events, s.v2.parent_events, s.v1.parent_events,
                          s.c3.fullscreen_states, s.v2.fullscreen_states, s.v1.fullscreen_states));
        check("every client's connection is up", up);
    }
    for (client* c : {&s.c3, &s.b3, &s.v1, &s.v2, &s.d3, &s.late})
        if (c->display)
            disconnect(*c);
    if (s.app.display)
        disconnect(s.app);
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
