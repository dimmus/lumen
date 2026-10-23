module;

// ext-workspace-v1 (staging) served from the window manager's workspace model (design
// Part W, W.4): one group handle per output, one workspace handle per workspace, for each
// bound manager. The compositor owns the workspaces; the shell's panel indicator is a client
// of this protocol like waybar or any other bar, so what one sees, all see.
//
// - Bind: every group (`capabilities` 0 — dynamic mode makes workspaces by itself, static has
//   a fixed count — then `output_enter` per wl_output the client bound for that output),
//   every workspace (`id` only for a static workspace the configuration names, `name`,
//   1-D `coordinates` — its position, no geometric meaning — `state`, `capabilities`
//   activate | assign; assign only in dynamic mode), `workspace_enter` on its group, `done`.
// - Changes: `workspaces_changed` (after every window-manager operation that changed the
//   model) diffs each manager's handles against the model and sends what differs, closed by
//   one `done` — the protocol's atomicity. A wl_output bound later gets its `output_enter`
//   (and a `done`) at bind.
// - Removal order (the spec): `workspace_leave` from its group, then `removed`; a group
//   loses every workspace before its own `removed`. Removed handles are inert.
// - Requests are queued per manager and run on `commit`: assigns first, then activates
//   (niri's order); deactivate, remove and create_workspace are not advertised and ignored;
//   requests on inert handles and while the session is locked are ignored (W.6). `stop` →
//   `finished`, and the manager is destroyed.
// - Session-wide state, so hidden from sandboxed clients (as ext-foreign-toplevel-list).
// Every handle has a destructor hook: clients disconnect mid-flight.

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
#include "ext-workspace-v1-server-protocol.h"
#define LUMEN_HAS_EXT_WORKSPACE 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :toplevel;
import :shell_bridge;
import :output;
import :wm;
import :workspaces;
import :session_lock;
import :wire;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

#if defined(LUMEN_HAS_EXT_WORKSPACE)

namespace {

constexpr const char* k_category = "compositor.workspace";
/// Requests one commit may carry; more are dropped (logged) — a client that sends hundreds of
/// activations before a commit gets the first ones.
constexpr unsigned k_max_pending = 32;
/// Managers per client and in all: a client binding in a loop gets `finished` at once.
constexpr unsigned k_max_managers_per_client = 4;
constexpr unsigned k_max_managers = 32;

struct manager_obj;

struct group_obj {
    wl_resource* resource = nullptr;
    manager_obj* manager = nullptr;
    unsigned slot = 0;
    /// The model group it stands for (0: inert, removed).
    unsigned serial = 0;
};

struct handle_obj {
    wl_resource* resource = nullptr;
    manager_obj* manager = nullptr;
    /// Empty: inert (removed).
    lx::workspace_id id{};
    // What this handle was last told.
    unsigned long long name_hash = 0;
    unsigned index = ~0u;
    unsigned state = ~0u;
    unsigned caps = ~0u;
    /// The serial of the group it entered (0: none).
    unsigned group_serial = 0;
};

struct pending_request {
    bool assign = false;
    handle_obj* handle = nullptr;
    /// assign: the output of the group named (-1: an inert group).
    int slot = -1;
};

struct manager_obj {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    bool stopped = false;
    bool first_done = false;
    group_obj* groups[k_workspace_groups]{};
    handle_obj* handles[k_max_workspaces]{};
    pending_request pending[k_max_pending]{};
    unsigned pending_count = 0;
    manager_obj* prev = nullptr;
    manager_obj* next = nullptr;
};

manager_obj* g_managers = nullptr;
unsigned g_manager_count = 0;

[[nodiscard]] bool ws_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

unsigned long long text_hash(const char* s) {
    unsigned long long h = 1469598103934665603ull;
    for (const char* p = s ? s : ""; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    return h;
}

window_manager* wm() { return g_p0_ctx ? g_p0_ctx->wm : nullptr; }

[[nodiscard]] unsigned state_of(const workspace_record& w) {
    return (w.active ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0u) |
           (w.urgent ? EXT_WORKSPACE_HANDLE_V1_STATE_URGENT : 0u);
}

[[nodiscard]] unsigned caps_of(const workspace_model& m) {
    // assign moves a workspace between outputs: only where workspaces come and go.
    return EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
           (m.config().dynamic ? EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN : 0u);
}

// ── object lifetimes ──

void handle_destroy_resource(wl_resource* r) {
    auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
    if (!h)
        return;
    if (manager_obj* m = h->manager) {
        for (auto& slot : m->handles)
            if (slot == h)
                slot = nullptr;
        for (unsigned i = 0; i < m->pending_count; ++i)
            if (m->pending[i].handle == h)
                m->pending[i].handle = nullptr;
    }
    delete h;
}

void group_destroy_resource(wl_resource* r) {
    auto* g = static_cast<group_obj*>(wl_resource_get_user_data(r));
    if (!g)
        return;
    if (manager_obj* m = g->manager)
        for (auto& slot : m->groups)
            if (slot == g)
                slot = nullptr;
    delete g;
}

void manager_unlink(manager_obj* m) {
    if (m->prev)
        m->prev->next = m->next;
    else if (g_managers == m)
        g_managers = m->next;
    if (m->next)
        m->next->prev = m->prev;
    m->prev = m->next = nullptr;
    --g_manager_count;
}

void manager_destroy_resource(wl_resource* r) {
    auto* m = static_cast<manager_obj*>(wl_resource_get_user_data(r));
    if (!m)
        return;
    // Its handles are objects of their own (the client destroys them after `finished`):
    // inert from here on.
    for (handle_obj* h : m->handles)
        if (h) {
            h->manager = nullptr;
            h->id = {};
        }
    for (group_obj* g : m->groups)
        if (g) {
            g->manager = nullptr;
            g->serial = 0;
        }
    manager_unlink(m);
    delete m;
}

[[nodiscard]] handle_obj* find_handle(manager_obj* m, lx::workspace_id id) {
    for (handle_obj* h : m->handles)
        if (h && h->id == id)
            return h;
    return nullptr;
}

[[nodiscard]] group_obj* group_by_serial(manager_obj* m, unsigned serial) {
    if (serial == 0)
        return nullptr;
    for (group_obj* g : m->groups)
        if (g && g->serial == serial)
            return g;
    return nullptr;
}

void output_enter_one(wl_resource* output, void* user) {
    ext_workspace_group_handle_v1_send_output_enter(static_cast<group_obj*>(user)->resource,
                                                    output);
}

// ── requests ──

void queue(manager_obj* m, const pending_request& r) {
    if (!m || m->stopped)
        return;
    if (m->pending_count == k_max_pending) {
        if (ws_debug())
            lx::trace::logger::global().log(lx::trace::level::debug, k_category,
                                            "ext-workspace: too many requests before commit; "
                                            "dropped");
        return;
    }
    m->pending[m->pending_count++] = r;
}

const struct ext_workspace_handle_v1_interface handle_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .activate =
        [](struct wl_client*, struct wl_resource* r) {
            auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
            if (h && h->manager && h->id)
                queue(h->manager, {false, h, -1});
        },
    // Not advertised: ignored ("the compositor will ignore requests it doesn't support").
    .deactivate = [](struct wl_client*, struct wl_resource*) {},
    .assign =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* group) {
            auto* h = static_cast<handle_obj*>(wl_resource_get_user_data(r));
            auto* g = group ? static_cast<group_obj*>(wl_resource_get_user_data(group)) : nullptr;
            if (h && h->manager && h->id && g && g->serial)
                queue(h->manager, {true, h, static_cast<int>(g->slot)});
        },
    .remove = [](struct wl_client*, struct wl_resource*) {},
};

const struct ext_workspace_group_handle_v1_interface group_impl = {
    .create_workspace = [](struct wl_client*, struct wl_resource*, const char*) {},
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void run_pending(manager_obj* m) {
    window_manager* w = wm();
    const unsigned n = m->pending_count;
    m->pending_count = 0;
    if (!w || n == 0)
        return;
    // The model does not change behind the lock (W.6): a bar's click there is dropped.
    if (g_p0_ctx->lock && g_p0_ctx->lock->state() != lock_state::unlocked) {
        if (ws_debug())
            lx::trace::logger::global().log(lx::trace::level::debug, k_category,
                                            "ext-workspace: commit while locked; ignored");
        return;
    }
    pending_request reqs[k_max_pending];
    for (unsigned i = 0; i < n; ++i)
        reqs[i] = m->pending[i];
    // Assigns first, then activates (niri): "move workspace 3 here and show it" works in one
    // commit, and every client sees the result as one change (one `done`).
    w->begin_workspace_batch();
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < n; ++i) {
            const pending_request& r = reqs[i];
            if (r.assign != (pass == 0) || !r.handle || !r.handle->id)
                continue;
            if (r.assign) {
                if (r.slot >= 0 && w->workspaces().config().dynamic)
                    (void)w->assign_workspace(r.handle->id, static_cast<unsigned>(r.slot));
            } else {
                (void)w->switch_workspace(r.handle->id, "ext-workspace");
            }
        }
    }
    w->end_workspace_batch();
}

const struct ext_workspace_manager_v1_interface manager_impl = {
    .commit =
        [](struct wl_client*, struct wl_resource* r) {
            auto* m = static_cast<manager_obj*>(wl_resource_get_user_data(r));
            if (m)
                run_pending(m);
        },
    .stop =
        [](struct wl_client*, struct wl_resource* r) {
            auto* m = static_cast<manager_obj*>(wl_resource_get_user_data(r));
            if (!m || m->stopped)
                return;
            m->stopped = true;
            m->pending_count = 0;
            ext_workspace_manager_v1_send_finished(r);
            wl_resource_destroy(r);
        },
};

// ── the diff ──

/// Brings manager `m` in line with the model; true when anything was sent (`done` follows).
bool sync_manager(manager_obj* m, const workspace_model& model) {
    if (!m || m->stopped || !m->resource)
        return false;
    bool sent = false;
    const int version = wl_resource_get_version(m->resource);
    // 1. Workspaces that went or changed group leave their old group (a gone one is removed).
    for (handle_obj*& h : m->handles) {
        if (!h || !h->id)
            continue;
        const workspace_record* w = model.find(h->id);
        const unsigned target = w && w->output >= 0 ? model.group(static_cast<unsigned>(w->output)).serial
                                                    : 0u;
        if (h->group_serial && (!w || h->group_serial != target)) {
            if (group_obj* g = group_by_serial(m, h->group_serial))
                ext_workspace_group_handle_v1_send_workspace_leave(g->resource, h->resource);
            h->group_serial = 0;
            sent = true;
        }
        if (!w) {
            ext_workspace_handle_v1_send_removed(h->resource);
            h->id = {};
            // The object stays the client's (inert); the manager forgets it.
            for (unsigned i = 0; i < m->pending_count; ++i)
                if (m->pending[i].handle == h)
                    m->pending[i].handle = nullptr;
            h->manager = nullptr;
            h = nullptr;
            sent = true;
        }
    }
    // 2. Groups that went are removed (their workspaces left above); new ones announced.
    for (unsigned s = 0; s < k_workspace_groups; ++s) {
        group_obj*& g = m->groups[s];
        const bool live = model.has_group(s);
        if (g && (!live || g->serial != model.group(s).serial)) {
            ext_workspace_group_handle_v1_send_removed(g->resource);
            g->serial = 0;
            g->manager = nullptr;
            g = nullptr;
            sent = true;
        }
        if (live && !g) {
            wl_resource* res = wl_resource_create(m->client, &ext_workspace_group_handle_v1_interface,
                                                  version, 0);
            auto* obj = res ? new (std::nothrow) group_obj{} : nullptr;
            if (!obj) {
                if (res)
                    wl_resource_destroy(res);
                wl_client_post_no_memory(m->client);
                return sent;
            }
            obj->resource = res;
            obj->manager = m;
            obj->slot = s;
            obj->serial = model.group(s).serial;
            wl_resource_set_implementation(res, &group_impl, obj, &group_destroy_resource);
            g = obj;
            ext_workspace_manager_v1_send_workspace_group(m->resource, res);
            ext_workspace_group_handle_v1_send_capabilities(res, 0);
            for_each_output_resource(m->client, s, &output_enter_one, obj);
            sent = true;
        }
    }
    // 3. New workspaces, changed ones, and those that entered a group.
    const unsigned caps = caps_of(model);
    model.for_each([&](const workspace_record& w) {
        handle_obj* h = find_handle(m, w.id);
        if (!h) {
            unsigned free_slot = k_max_workspaces;
            for (unsigned i = 0; i < k_max_workspaces; ++i)
                if (!m->handles[i]) {
                    free_slot = i;
                    break;
                }
            if (free_slot == k_max_workspaces)
                return;
            wl_resource* res =
                wl_resource_create(m->client, &ext_workspace_handle_v1_interface, version, 0);
            h = res ? new (std::nothrow) handle_obj{} : nullptr;
            if (!h) {
                if (res)
                    wl_resource_destroy(res);
                wl_client_post_no_memory(m->client);
                return;
            }
            h->resource = res;
            h->manager = m;
            h->id = w.id;
            wl_resource_set_implementation(res, &handle_impl, h, &handle_destroy_resource);
            m->handles[free_slot] = h;
            ext_workspace_manager_v1_send_workspace(m->resource, res);
            // A stable id for a workspace the configuration names (the output's, by name).
            if (w.named && w.output >= 0) {
                char id[96];
                std::snprintf(id, sizeof(id), "%s/%s",
                              model.group(static_cast<unsigned>(w.output)).output_name, w.name);
                ext_workspace_handle_v1_send_id(res, id);
            }
            sent = true;
        }
        const unsigned long long nh = text_hash(w.name);
        if (h->name_hash != nh) {
            ext_workspace_handle_v1_send_name(h->resource, w.name);
            h->name_hash = nh;
            sent = true;
        }
        if (h->index != w.index) {
            std::uint32_t pos = w.index;
            wl_array a{};
            a.size = sizeof(pos);
            a.alloc = 0;
            a.data = &pos;
            ext_workspace_handle_v1_send_coordinates(h->resource, &a);
            h->index = w.index;
            sent = true;
        }
        const unsigned st = state_of(w);
        if (h->state != st) {
            ext_workspace_handle_v1_send_state(h->resource, st);
            h->state = st;
            sent = true;
        }
        if (h->caps != caps) {
            ext_workspace_handle_v1_send_capabilities(h->resource, caps);
            h->caps = caps;
            sent = true;
        }
        if (h->group_serial == 0 && w.output >= 0) {
            const unsigned serial = model.group(static_cast<unsigned>(w.output)).serial;
            if (group_obj* g = group_by_serial(m, serial)) {
                ext_workspace_group_handle_v1_send_workspace_enter(g->resource, h->resource);
                h->group_serial = serial;
                sent = true;
            }
        }
    });
    return sent;
}

void finish_manager(manager_obj* m, bool sent) {
    if (!sent && m->first_done)
        return;
    ext_workspace_manager_v1_send_done(m->resource);
    if (!m->first_done) {
        m->first_done = true;
        // The shell bound ext-workspace after its policy bridge: toplevel_workspace again,
        // now that it holds a handle of every workspace.
        if (g_p0_ctx && g_p0_ctx->shell && g_p0_ctx->shell->bridge_resource() &&
            wl_resource_get_client(static_cast<wl_resource*>(g_p0_ctx->shell->bridge_resource())) ==
                m->client)
            g_p0_ctx->shell->resend_toplevel_workspaces();
    }
}

void* lookup_handle(void* client, lx::workspace_id id) {
    for (manager_obj* m = g_managers; m; m = m->next) {
        if (m->client != static_cast<wl_client*>(client) || m->stopped)
            continue;
        if (handle_obj* h = find_handle(m, id); h && h->group_serial)
            return h->resource;
    }
    return nullptr;
}

} // namespace

void bind_workspace_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx || !g_p0_ctx->wm)
        return;
    auto res = wayland::resource::create(client, &ext_workspace_manager_v1_interface, version, id);
    if (!res)
        return;
    auto* native = static_cast<wl_resource*>(res.native());
    wl_client* c = wl_resource_get_client(native);
    unsigned mine = 0;
    for (manager_obj* m = g_managers; m; m = m->next)
        if (m->client == c)
            ++mine;
    auto* m = mine < k_max_managers_per_client && g_manager_count < k_max_managers
                  ? new (std::nothrow) manager_obj{}
                  : nullptr;
    if (!m) {
        // Bounded: such a manager is done before it starts.
        wl_resource_set_implementation(native, &manager_impl, nullptr, nullptr);
        ext_workspace_manager_v1_send_finished(native);
        wl_resource_destroy(native);
        return;
    }
    m->resource = native;
    m->client = c;
    m->next = g_managers;
    if (g_managers)
        g_managers->prev = m;
    g_managers = m;
    ++g_manager_count;
    wl_resource_set_implementation(native, &manager_impl, m, &manager_destroy_resource);
    if (g_p0_ctx->shell)
        g_p0_ctx->shell->set_workspace_lookup(&lookup_handle);
    // Outputs known now have their workspaces (start-up before any window).
    g_p0_ctx->wm->sync_workspace_outputs(true);
    (void)sync_manager(m, g_p0_ctx->wm->workspaces());
    finish_manager(m, true);
    if (ws_debug())
        lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                         "ext-workspace bind client={} version={} workspaces={}",
                                         client.id(), version,
                                         g_p0_ctx->wm->workspaces().count());
}

void workspace_output_bound(wl_client* client, wl_resource* output, unsigned slot) {
    for (manager_obj* m = g_managers; m; m = m->next) {
        if (m->client != client || m->stopped || slot >= k_workspace_groups)
            continue;
        if (group_obj* g = m->groups[slot]; g && g->serial) {
            ext_workspace_group_handle_v1_send_output_enter(g->resource, output);
            ext_workspace_manager_v1_send_done(m->resource);
        }
    }
}

void workspaces_sync_all() {
    if (!g_p0_ctx || !g_p0_ctx->wm)
        return;
    const workspace_model& model = g_p0_ctx->wm->workspaces();
    for (manager_obj* m = g_managers; m; m = m->next)
        if (!m->stopped)
            finish_manager(m, sync_manager(m, model));
}

#else // !LUMEN_HAS_EXT_WORKSPACE

void workspace_output_bound(wl_client*, wl_resource*, unsigned) {}
void workspaces_sync_all() {}

#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::workspaces_changed(p0_protocol_context& ctx) {
#if defined(LUMEN_HAS_WAYLAND)
    (void)ctx;
    p0::workspaces_sync_all();
#else
    (void)ctx;
#endif
}
