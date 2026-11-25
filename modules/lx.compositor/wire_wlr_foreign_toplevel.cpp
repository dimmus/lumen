module;

// wlr-foreign-toplevel-management-unstable-v1 (wlr-protocols: zwlr_foreign_toplevel_manager_v1 and
// zwlr_foreign_toplevel_handle_v1, served at version 3): the window list and the window
// requests of shell chrome and waybar-class bars — title, app id, state, outputs and parent of
// every mapped window, and activate, close, minimize, maximize and fullscreen on them. It sits
// next to ext-foreign-toplevel-list (the list the capture pickers use) and changes nothing of
// zlm_shell_v1, which stays the shell's own channel.
//
// - Bind: one handle per mapped window, bottom to top; each handle's opening batch carries
//   title, app_id, state, one output_enter per wl_output the client bound for the window's
//   output, parent (null without a mapped one) and ends with done. A child that comes before
//   its parent in the list gets its parent once every handle of the batch exists, before that
//   batch's done.
// - A window that maps gets the same batch, from the shell bridge's list observer.
// - Changes: the observer's `changed`, `focused` and `updated`, and the workspace and outputs
//   hooks, compare each handle with what it was last told — title and app id by hash, the
//   state bits, the output, the parent — and send only what differs, closed by one done.
// - Output: the output whose active workspace lists the window; a sticky or fullscreen one,
//   the one it is on. Minimizing sends no output_leave: a bar keeps the button that restores
//   the window. A wl_output bound later gets the output_enter it missed, and a done.
// - activated: the window the window manager's focus decision names (what focus_changed tells
//   the shell); none while no window has it.
// - Unmap: closed, and the handle is inert from then on (its requests are ignored).
// - stop: finished, and the manager object goes. Its handles are the client's objects and go
//   on working.
// - Requests do what the zlm policy bridge's do. Behind the session lock every request
//   changes nothing. set_rectangle keeps its rectangle as a hint; a negative width or height,
//   or exactly one of them zero, is invalid_rectangle.
// - Session-wide, so hidden from sandboxed clients (SC2), as ext-foreign-toplevel-list is.
//
// Nothing here is polled or timed: every entry returns at once while no manager is bound, and
// the handles live in their manager's fixed-capacity table, grown a block at a time — a change
// batch allocates nothing.

#include <stdint.h>

#include <wayland-server.h>
#ifdef LUMEN_PROTOCOL_WLR_FOREIGN_TOPLEVEL_MANAGEMENT_UNSTABLE_V1
#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"
#define LUMEN_HAS_WLR_FOREIGN_TOPLEVEL 1
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :toplevel;
import :shell_bridge;
import :workspaces;
import :session_lock;
import :wire;

namespace lx::compositor::wire {

#ifdef LUMEN_HAS_WLR_FOREIGN_TOPLEVEL

namespace {

constexpr lx::trace::category log_ftl{"compositor.foreign_toplevel"};

/// Managers per client and in all: a client binding in a loop gets `finished` at once.
constexpr unsigned k_max_managers_per_client = 4;
constexpr unsigned k_max_managers = 32;
/// One handle per toplevel record the compositor has (`toplevel_manager::k_max`). A client
/// that never destroys the handles of closed windows runs out: a window mapped then is not
/// announced to it (logged) until it destroys some.
constexpr unsigned k_max_handles = toplevel_manager::k_max;
/// What the window manager logs as the source of these requests.
constexpr char k_source[] = "wlr-toplevel";
/// `handle_obj::state` before any `state` event went out (no real state has every bit).
constexpr unsigned k_no_state = ~0u;
/// `handle_obj::output` before any output event: neither an output nor "none" (-1).
constexpr int k_no_output_sent = -2;
/// The protocol's `state` values, as the places of `handle_obj::state`.
constexpr unsigned st_maximized = 0;
constexpr unsigned st_minimized = 1;
constexpr unsigned st_activated = 2;
constexpr unsigned st_fullscreen = 3;
constexpr unsigned k_states = 4;

struct manager_obj;

/// zwlr_foreign_toplevel_handle_v1, in its manager's table. `resource` null: a free slot.
/// `toplevel` empty with one: an inert handle — its window unmapped, so nothing more is sent
/// to it and its requests are ignored — until the client destroys it.
struct handle_obj {
    wl_resource* resource = nullptr;
    lx::toplevel_id toplevel{};
    manager_obj* manager = nullptr;
    /// What this handle was last told.
    unsigned long long title_hash = 0;
    unsigned long long app_id_hash = 0;
    unsigned state = k_no_state;
    /// The output slot of the last output_enter.
    int output = k_no_output_sent;
    /// The mapped parent of the last `parent` event (`parent_told` false: none sent yet).
    lx::toplevel_id parent{};
    bool parent_told = false;
    /// set_rectangle's hint: the bar's surface and the rectangle in its local coordinates
    /// (empty: none — "Setting width=height=0 removes the already-set rectangle"; the last
    /// one set wins). Kept for the window manager, which has no use for it yet.
    lx::surface_id rect_surface{};
    lx::rect2i rect{};
};

/// zwlr_foreign_toplevel_manager_v1 and its handles' table: one allocation per bind, the
/// table's blocks as windows need them. Freed with the resource — or, while handles of it are
/// alive (the client's objects, which go on working after `finished`), with the last of them.
struct manager_obj {
    /// Null once the manager is gone (stop, its client): nothing more is announced on it.
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    /// Handles in use, the inert ones too: they are the client's until it destroys them.
    unsigned live = 0;
    /// Set once a window was skipped for a full table, to log that once.
    bool overflow_logged = false;
    lx::sparse_slots<handle_obj, k_max_handles> handles{};
    manager_obj* prev = nullptr;
    manager_obj* next = nullptr;
};

/// Every table that is still referenced, newest first.
manager_obj* g_tables = nullptr;
/// Of them, the managers that still announce.
unsigned g_manager_count = 0;
/// The window the handles were last told is activated.
lx::toplevel_id g_activated{};

/// 64-bit FNV-1a: what a handle was last told, without keeping the strings.
unsigned long long text_hash(const char* s) {
    unsigned long long h = 1469598103934665603ull;
    for (const char* p = s ? s : ""; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    return h;
}

window_manager* wm() {
    return g_ctx ? g_ctx->wm : nullptr;
}

/// The output a window is shown on, for this protocol: a fullscreen one, the output it is
/// fullscreen on; else the output whose active workspace lists it (a sticky window: the one
/// it is on). -1 on none — unmapped, its workspace not shown, its output gone.
[[nodiscard]] int shown_output(const toplevel_record& rec) {
    auto* om = g_ctx ? g_ctx->outputs : nullptr;
    window_manager* w = wm();
    if (!rec.mapped || !om || !w)
        return -1;
    const auto live = [om](int slot) {
        return slot >= 0 && om->slot_live(static_cast<unsigned>(slot));
    };
    if (rec.state == lx::toplevel_state::fullscreen && live(rec.fullscreen_output))
        return rec.fullscreen_output;
    if (rec.sticky) {
        const int slot = w->window_output(rec);
        return live(slot) ? slot : -1;
    }
    const workspace_record* ws = rec.workspace ? w->workspaces().find(rec.workspace) : nullptr;
    return ws && ws->active && live(ws->output) ? ws->output : -1;
}

/// The mapped parent of `rec`: its parent while that one is mapped, else none — the relation
/// zlm's toplevel_parent tells the shell too.
[[nodiscard]] lx::toplevel_id mapped_parent(const toplevel_record& rec) {
    if (!rec.parent || !g_ctx || !g_ctx->toplevels)
        return {};
    const toplevel_record* p = g_ctx->toplevels->find(rec.parent);
    return p && p->mapped ? p->id : lx::toplevel_id{};
}

/// The state bits of `rec` as a handle bound at `version` may be told them: the fullscreen
/// state is version 2's. `activated` is the window manager's focus decision.
[[nodiscard]] unsigned state_bits(const toplevel_record& rec, int version) {
    unsigned bits = 0;
    if (rec.state == lx::toplevel_state::maximized)
        bits |= 1u << st_maximized;
    if (rec.minimized)
        bits |= 1u << st_minimized;
    if (wm() && rec.id == wm()->keyboard_focus())
        bits |= 1u << st_activated;
    if (version >= 2 && rec.state == lx::toplevel_state::fullscreen)
        bits |= 1u << st_fullscreen;
    return bits;
}

/// Behind the session lock nothing changes (W.6, ext-workspace's rule for its commit).
[[nodiscard]] bool session_locked() {
    return g_ctx && g_ctx->lock && g_ctx->lock->state() != lock_state::unlocked;
}

/// `h` is an object still (not destroyed, its client not being torn down).
[[nodiscard]] bool tellable(const handle_obj& h) {
    return h.resource && !client_dying(wl_resource_get_client(h.resource));
}

// ── object lifetimes ──

void manager_free(manager_obj* m) {
    if (m->prev)
        m->prev->next = m->next;
    else if (g_tables == m)
        g_tables = m->next;
    if (m->next)
        m->next->prev = m->prev;
    delete m;
}

void handle_destroy_resource(wl_resource* r) {
    auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
    if (!h)
        return;
    manager_obj* m = h->manager;
    *h = {};
    // The manager went before its last handle did (stop): its table waited for this.
    if (m && --m->live == 0 && !m->resource)
        manager_free(m);
}

void manager_destroy_resource(wl_resource* r) {
    auto* m = static_cast<manager_obj*>(wl_resource_get_user_data(r));
    if (!m)
        return;
    // Its handles are the client's objects and go on working: the table stays for them.
    m->resource = nullptr;
    --g_manager_count;
    if (m->live == 0)
        manager_free(m);
}

// ── events ──

void send_state(wl_resource* r, unsigned bits) {
    std::uint32_t values[k_states];
    unsigned n = 0;
    for (unsigned b = 0; b < k_states; ++b)
        if (bits & (1u << b))
            values[n++] = b;
    wl_array a{};
    a.size = n * sizeof(std::uint32_t);
    a.alloc = 0;
    a.data = values;
    zwlr_foreign_toplevel_handle_v1_send_state(r, &a);
}

/// What `for_each_output_resource` was told, and how often.
struct output_notes {
    wl_resource* handle = nullptr;
    unsigned sent = 0;
};

void output_enter_one(wl_resource* output, void* user) {
    auto* n = static_cast<output_notes*>(user);
    zwlr_foreign_toplevel_handle_v1_send_output_enter(n->handle, output);
    ++n->sent;
}

void output_leave_one(wl_resource* output, void* user) {
    auto* n = static_cast<output_notes*>(user);
    zwlr_foreign_toplevel_handle_v1_send_output_leave(n->handle, output);
    ++n->sent;
}

// ── the diff: what a handle is told of its window ──

/// title and app_id: always for a handle just opened, else when they differ from what it was
/// told.
[[nodiscard]] bool sync_texts(handle_obj& h, const toplevel_record& rec, bool initial) {
    const unsigned long long th = text_hash(rec.title);
    const unsigned long long ah = text_hash(rec.app_id);
    bool sent = false;
    if (initial || th != h.title_hash) {
        zwlr_foreign_toplevel_handle_v1_send_title(h.resource, rec.title ? rec.title : "");
        h.title_hash = th;
        sent = true;
    }
    if (initial || ah != h.app_id_hash) {
        zwlr_foreign_toplevel_handle_v1_send_app_id(h.resource, rec.app_id ? rec.app_id : "");
        h.app_id_hash = ah;
        sent = true;
    }
    return sent;
}

/// state, then the output: output_leave for the one it left, output_enter for the one it is
/// on — each on every wl_output the client bound for that output.
[[nodiscard]] bool sync_state_output(handle_obj& h, const toplevel_record& rec) {
    bool sent = false;
    const unsigned bits = state_bits(rec, wl_resource_get_version(h.resource));
    if (bits != h.state) {
        send_state(h.resource, bits);
        h.state = bits;
        sent = true;
    }
    const int output = shown_output(rec);
    if (output != h.output) {
        output_notes notes{h.resource, 0};
        wl_client* client = wl_resource_get_client(h.resource);
        if (h.output >= 0)
            for_each_output_resource(client, static_cast<unsigned>(h.output), &output_leave_one,
                                     &notes);
        if (output >= 0)
            for_each_output_resource(client, static_cast<unsigned>(output), &output_enter_one,
                                     &notes);
        // On with no event when the client bound no wl_output for that output yet: the
        // output's next bind sends the enter (wlr_toplevel_output_bound).
        h.output = output;
        sent = sent || notes.sent != 0;
    }
    return sent;
}

/// The client's handle of `toplevel` to name as `h`'s parent: one of `h`'s own table when it
/// has one, else the client's first. Null when the client has none (never announced to it, or
/// destroyed by it: "no event is emitted when the parent handle is destroyed by the client").
[[nodiscard]] wl_resource* parent_handle(const handle_obj& h, lx::toplevel_id toplevel) {
    if (!toplevel || !h.manager)
        return nullptr;
    for (const handle_obj& p : h.manager->handles)
        if (p.toplevel == toplevel)
            return p.resource;
    for (const manager_obj* m = g_tables; m; m = m->next) {
        if (m->client != h.manager->client)
            continue;
        for (const handle_obj& p : m->handles)
            if (p.toplevel == toplevel)
                return p.resource;
    }
    return nullptr;
}

/// parent (version 3): the window's mapped parent, when it differs from what `h` was told.
[[nodiscard]] bool sync_parent(handle_obj& h, const toplevel_record& rec) {
    if (wl_resource_get_version(h.resource) < 3)
        return false;
    const lx::toplevel_id parent = mapped_parent(rec);
    if (h.parent_told && h.parent == parent)
        return false;
    zwlr_foreign_toplevel_handle_v1_send_parent(h.resource, parent_handle(h, parent));
    h.parent = parent;
    h.parent_told = true;
    return true;
}

/// A live handle follows its window: everything that differs from what it was told, then one
/// done when anything went out. False when nothing did, or the client is being torn down.
[[nodiscard]] bool sync_handle(handle_obj& h, const toplevel_record& rec) {
    if (!tellable(h))
        return false;
    bool sent = sync_texts(h, rec, false);
    sent = sync_state_output(h, rec) || sent;
    sent = sync_parent(h, rec) || sent;
    if (sent)
        zwlr_foreign_toplevel_handle_v1_send_done(h.resource);
    return sent;
}

/// Every live handle of window `id` follows it.
void sync_id(lx::toplevel_id id) {
    if (!id || !g_tables || !g_ctx || !g_ctx->toplevels)
        return;
    const toplevel_record* rec = g_ctx->toplevels->find(id);
    if (!rec)
        return;
    for (manager_obj* m = g_tables; m; m = m->next)
        for (handle_obj& h : m->handles)
            if (h.toplevel == id)
                (void)sync_handle(h, *rec);
}

// ── requests ──

/// A request's window: the handle's, while it is mapped and the session is not locked. Null
/// for the rest, each logged: an inert handle (its window closed), and anything behind the
/// lock (nothing changes there, W.6).
[[nodiscard]] const toplevel_record* request_target(wl_resource* r, const char* request) {
    auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
    if (!h)
        return nullptr;
    // An inert handle names no window left (id 0 in the log).
    const toplevel_record* rec =
        h->toplevel && g_ctx && g_ctx->toplevels ? g_ctx->toplevels->find(h->toplevel) : nullptr;
    if (rec && rec->mapped && !session_locked())
        return rec;
    if (log_ftl.enabled(lx::trace::level::debug))
        log_ftl.debug("wlr-toplevel request={} id={} result={}", request, h->toplevel.id(),
                      !rec           ? "inert"
                      : !rec->mapped ? "unmapped"
                                     : "locked");
    return nullptr;
}

/// The common shape, as the zlm bridge's requests: the window, then `fn(window_manager, id)`.
template <typename Fn>
void window_request(wl_resource* r, const char* request, Fn&& fn) {
    const toplevel_record* rec = request_target(r, request);
    if (rec && wm())
        fn(*wm(), rec->id);
}

const struct zwlr_foreign_toplevel_handle_v1_interface handle_impl = {
    .set_maximized =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "set_maximized", [](auto& w, lx::toplevel_id id) {
                w.set_maximized(id, true, k_source);
            });
        },
    .unset_maximized =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "unset_maximized", [](auto& w, lx::toplevel_id id) {
                w.set_maximized(id, false, k_source);
            });
        },
    .set_minimized =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "set_minimized", [](auto& w, lx::toplevel_id id) {
                w.set_minimized(id, true, k_source);
            });
        },
    .unset_minimized =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "unset_minimized", [](auto& w, lx::toplevel_id id) {
                w.set_minimized(id, false, k_source);
            });
        },
    // One seat: the object is accepted and decides nothing. This is an explicit choice of
    // the user's, as the shell's request_activate is (a task-list click), so the window
    // manager raises it and gives it the keyboard.
    .activate =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource*) {
            window_request(r, "activate", [](auto& w, lx::toplevel_id id) { w.activate(id); });
        },
    .close =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "close", [](auto& w, lx::toplevel_id id) { w.request_close(id); });
        },
    .set_rectangle =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* surface, int32_t x,
           int32_t y, int32_t width, int32_t height) {
            // An inert handle ignores everything but destroy — and raises nothing.
            if (!request_target(r, "set_rectangle"))
                return;
            // Both dimensions or neither: width = height = 0 removes the rectangle.
            if (width < 0 || height < 0 || (width == 0) != (height == 0)) {
                wl_resource_post_error(
                    r, ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_ERROR_INVALID_RECTANGLE,
                    "invalid rectangle: a negative width or height, or exactly one of them zero");
                return;
            }
            auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
            const surface_obj* s = surface ? surface_from(surface) : nullptr;
            h->rect_surface = s ? s->id : lx::surface_id{};
            h->rect = width > 0 ? lx::rect2i{x, y, width, height} : lx::rect2i{};
            if (log_ftl.enabled(lx::trace::level::debug))
                log_ftl.debug("wlr-toplevel rectangle id={} surface={} x={} y={} w={} h={}",
                              h->toplevel.id(), h->rect_surface.id(), x, y, width, height);
        },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    // Fullscreen on the output asked for (null, or one that is gone: the window's own).
    .set_fullscreen =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* output) {
            const int slot = output ? output_resource_slot(output) : -1;
            window_request(r, "set_fullscreen", [slot](auto& w, lx::toplevel_id id) {
                w.set_fullscreen(id, true, slot, k_source);
            });
        },
    .unset_fullscreen =
        [](struct wl_client*, struct wl_resource* r) {
            window_request(r, "unset_fullscreen", [](auto& w, lx::toplevel_id id) {
                w.set_fullscreen(id, false, -1, k_source);
            });
        },
};

// ── announcing a window ──

/// A handle of `rec` on `m`: the toplevel event. Null when the table is full (logged once) or
/// the resource could not be made (no_memory was posted).
[[nodiscard]] handle_obj* open(manager_obj& m, const toplevel_record& rec) {
    // The first free slot: the blocks before it are the only ones touched.
    handle_obj* h = nullptr;
    for (unsigned i = 0; i < k_max_handles && !h; ++i)
        if (!m.handles[i].resource)
            h = &m.handles[i];
    if (!h) {
        if (log_ftl.enabled(lx::trace::level::debug) && !m.overflow_logged) {
            m.overflow_logged = true;
            log_ftl.debug("wlr-toplevel: a client holds {} handles; no new window is announced "
                          "to it until it destroys some",
                          k_max_handles);
        }
        return nullptr;
    }
    wl_resource* res = wl_resource_create(m.client, &zwlr_foreign_toplevel_handle_v1_interface,
                                          wl_resource_get_version(m.resource), 0);
    if (!res) {
        wl_client_post_no_memory(m.client);
        return nullptr;
    }
    *h = {};
    h->resource = res;
    h->toplevel = rec.id;
    h->manager = &m;
    ++m.live;
    wl_resource_set_implementation(res, &handle_impl, h, &handle_destroy_resource);
    zwlr_foreign_toplevel_manager_v1_send_toplevel(m.resource, res);
    return h;
}

/// A window mapped: toplevel, then its opening batch (title, app id, state, output_enter,
/// parent) closed by done.
void announce(manager_obj& m, const toplevel_record& rec) {
    if (handle_obj* h = open(m, rec)) {
        (void)sync_texts(*h, rec, true);
        (void)sync_state_output(*h, rec);
        (void)sync_parent(*h, rec);
        zwlr_foreign_toplevel_handle_v1_send_done(h->resource);
    }
}

const struct zwlr_foreign_toplevel_manager_v1_interface manager_impl = {
    .stop =
        [](struct wl_client*, struct wl_resource* r) {
            // `finished` is a destructor event: the object goes with it.
            zwlr_foreign_toplevel_manager_v1_send_finished(r);
            wl_resource_destroy(r);
        },
};

// ── the shell bridge's list observer ──

void observe_mapped(void*, toplevel_record& rec) {
    if (!g_tables || !rec.mapped)
        return;
    for (manager_obj* m = g_tables; m; m = m->next)
        if (m->resource && !client_dying(m->client))
            announce(*m, rec);
}

void observe_unmapped(void*, toplevel_record& rec) {
    if (!g_tables)
        return;
    for (manager_obj* m = g_tables; m; m = m->next)
        for (handle_obj& h : m->handles)
            if (h.toplevel == rec.id) {
                if (tellable(h))
                    zwlr_foreign_toplevel_handle_v1_send_closed(h.resource);
                h.toplevel = {}; // inert until the client destroys it
                h.rect_surface = {};
                h.rect = {};
            }
}

void observe_changed(void*, const toplevel_record& rec) {
    if (rec.mapped)
        sync_id(rec.id);
}

void observe_updated(void*, const toplevel_record& rec) {
    if (rec.mapped)
        sync_id(rec.id);
}

void observe_focused(void*, const toplevel_record* rec) {
    if (!g_tables)
        return;
    // The window told activated loses it, the one named now gains it.
    const lx::toplevel_id was = g_activated;
    g_activated = rec ? rec->id : lx::toplevel_id{};
    if (was == g_activated)
        return;
    sync_id(was);
    sync_id(g_activated);
}

} // namespace

void bind_wlr_foreign_toplevel_manager(wayland::client_connection& client, unsigned id,
                                       int version) {
    if (!g_ctx || !g_ctx->toplevels || !g_ctx->wm)
        return;
    auto res =
        wayland::resource::create(client, &zwlr_foreign_toplevel_manager_v1_interface, version, id);
    if (!res)
        return;
    auto* native = static_cast<wl_resource*>(res.native());
    if (!native)
        return;
    wl_client* c = wl_resource_get_client(native);
    unsigned mine = 0;
    for (manager_obj* m = g_tables; m; m = m->next)
        if (m->resource && m->client == c)
            ++mine;
    auto* m = mine < k_max_managers_per_client && g_manager_count < k_max_managers
                  ? new (std::nothrow) manager_obj{}
                  : nullptr;
    if (!m) {
        // Bounded: such a manager is done before it starts.
        wl_resource_set_implementation(native, &manager_impl, nullptr, nullptr);
        zwlr_foreign_toplevel_manager_v1_send_finished(native);
        wl_resource_destroy(native);
        return;
    }
    m->resource = native;
    m->client = c;
    m->next = g_tables;
    if (g_tables)
        g_tables->prev = m;
    g_tables = m;
    ++g_manager_count;
    wl_resource_set_implementation(native, &manager_impl, m, &manager_destroy_resource);
    // The list is told `activated` from the keyboard focus, and the observer records nothing
    // while no manager is bound: the tracker starts here, or the window that has the keyboard
    // now keeps `activated` after the next focus change.
    g_activated = wm()->keyboard_focus();
    // 1. Every window mapped now, bottom to top: toplevel, title, app id, state, outputs.
    g_ctx->toplevels->for_each_bottom_up([&](const toplevel_record& rec) {
        if (handle_obj* h = open(*m, rec)) {
            (void)sync_texts(*h, rec, true);
            (void)sync_state_output(*h, rec);
        }
    });
    // 2. Their parents, once every handle of the list exists: a child listed before its
    //    parent names it. 3. One done each: its batch is whole.
    for (handle_obj& h : m->handles)
        if (h.toplevel) {
            if (const toplevel_record* rec = g_ctx->toplevels->find(h.toplevel))
                (void)sync_parent(h, *rec);
            zwlr_foreign_toplevel_handle_v1_send_done(h.resource);
        }
    if (log_ftl.enabled(lx::trace::level::debug))
        log_ftl.debug("wlr-toplevel bind client={} version={} windows={}", client.id(), version,
                      m->live);
}

void install_wlr_toplevel_observer(protocol_context& ctx) {
    if (!ctx.shell)
        return;
    g_activated = {};
    toplevel_observer o{};
    o.mapped = &observe_mapped;
    o.unmapped = &observe_unmapped;
    o.changed = &observe_changed;
    o.focused = &observe_focused;
    o.updated = &observe_updated;
    ctx.shell->set_list_observer(o);
}

void wlr_toplevels_sync_all() {
    if (!g_tables || !g_ctx || !g_ctx->toplevels)
        return;
    for (manager_obj* m = g_tables; m; m = m->next) {
        if (m->live == 0)
            continue;
        for (handle_obj& h : m->handles)
            if (h.toplevel)
                if (const toplevel_record* rec = g_ctx->toplevels->find(h.toplevel))
                    (void)sync_handle(h, *rec);
    }
}

void wlr_toplevel_output_bound(wl_client* client, wl_resource* output, unsigned slot) {
    if (!g_tables)
        return;
    for (manager_obj* m = g_tables; m; m = m->next) {
        if (m->client != client || m->live == 0)
            continue;
        for (handle_obj& h : m->handles) {
            if (!h.toplevel || h.output != static_cast<int>(slot) || !tellable(h))
                continue;
            zwlr_foreign_toplevel_handle_v1_send_output_enter(h.resource, output);
            zwlr_foreign_toplevel_handle_v1_send_done(h.resource);
        }
    }
}

#else // !LUMEN_HAS_WLR_FOREIGN_TOPLEVEL

void install_wlr_toplevel_observer(protocol_context&) {}
void wlr_toplevels_sync_all() {}
void wlr_toplevel_output_bound(wl_client*, wl_resource*, unsigned) {}

#endif // LUMEN_HAS_WLR_FOREIGN_TOPLEVEL

} // namespace lx::compositor::wire
