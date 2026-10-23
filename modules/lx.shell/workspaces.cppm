module;

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#if defined(LUMEN_PROTOCOL_EXT_WORKSPACE_V1)
#include "ext-workspace-v1-client-protocol.h"
#define LUMEN_SHELL_EXT_WORKSPACE 1
#endif
#endif

import lx.foundation;
import lx.trace;

export module lx.shell:workspaces;

import std;

export namespace lx::shell {

// ── The workspace indicator's model: an ext-workspace-v1 client (design-session-and-wm W.7) ──
//
// The compositor owns workspaces; the shell shows them like any bar would, through the public
// protocol: one group per output (each with the wl_outputs it covers), its workspaces with a
// name, a 1-D coordinate (their order) and a state. Events build the next state; `done`
// makes it the one shown, so a panel never draws a half-applied change. Pure and Wayland-free
// in `workspace_view` (tested); `workspace_client` feeds it from the wire. Fixed tables: no
// event allocates.

inline constexpr unsigned k_max_shell_workspaces = 64;
inline constexpr unsigned k_max_shell_ws_groups = 16;
inline constexpr unsigned k_ws_group_outputs = 4;
inline constexpr unsigned k_shell_ws_name_bytes = 32;

/// ext_workspace_handle_v1.state bits.
enum shell_ws_state : unsigned {
    shell_ws_active = 1u,
    shell_ws_urgent = 2u,
    shell_ws_hidden = 4u,
};

struct shell_workspace {
    bool used = false;
    /// The ext_workspace_handle_v1 (opaque here).
    void* proxy = nullptr;
    char name[k_shell_ws_name_bytes]{};
    unsigned coordinate = 0;
    unsigned state = 0;
    unsigned caps = 0;
    /// Index of its group (-1: none).
    int group = -1;
    /// Bumped with every visible change (name, state), so a panel repaints one cell.
    unsigned rev = 0;
};

struct shell_ws_group {
    bool used = false;
    void* proxy = nullptr;
    /// The wl_outputs (opaque) it covers.
    void* outputs[k_ws_group_outputs]{};
    unsigned output_count = 0;
};

class workspace_view {
public:
    // ── events ──
    [[nodiscard]] int add_group(void* proxy);
    void output_enter(int group, void* output);
    void output_leave(int group, void* output);
    /// The group is gone (its workspaces left before, per the spec).
    void remove_group(int group);
    [[nodiscard]] int add_workspace(void* proxy);
    void set_name(int ws, const char* name);
    void set_coordinate(int ws, unsigned c);
    void set_state(int ws, unsigned state);
    void set_caps(int ws, unsigned caps);
    void enter(int group, int ws);
    void leave(int group, int ws);
    void remove_workspace(int ws);
    /// ext_workspace_manager_v1.done: what arrived since is the state now.
    void done();
    /// An output went (wl_registry.global_remove): no group names it any more.
    void forget_output(void* output);

    // ── queries ──
    /// The workspaces of the group covering `output` in their order (hidden ones left out);
    /// returns how many were written.
    unsigned of_output(void* output, int* out, unsigned cap) const;
    [[nodiscard]] const shell_workspace* get(int ws) const;
    [[nodiscard]] const shell_ws_group* group(int g) const {
        return g >= 0 && static_cast<unsigned>(g) < k_max_shell_ws_groups && groups_[g].used
                   ? &groups_[g]
                   : nullptr;
    }
    [[nodiscard]] int find(const void* proxy) const;
    [[nodiscard]] int find_group(const void* proxy) const;
    /// The workspace `proxy` is active (false for an unknown one).
    [[nodiscard]] bool is_active(const void* proxy) const;
    /// Changed by a `done` since the last call.
    [[nodiscard]] bool take_changed();
    [[nodiscard]] unsigned generation() const { return generation_; }

private:
    shell_workspace ws_[k_max_shell_workspaces]{};
    shell_ws_group groups_[k_max_shell_ws_groups]{};
    bool dirty_ = false;
    bool changed_ = false;
    unsigned generation_ = 0;
};

/// The wire side: binds ext_workspace_manager_v1 and feeds a `workspace_view`.
class workspace_client {
public:
    workspace_client() = default;
    ~workspace_client();
    workspace_client(const workspace_client&) = delete;
    workspace_client& operator=(const workspace_client&) = delete;

    /// Binds the global (`wl_registry*`, its name and version). The groups and workspaces
    /// arrive with the next dispatch.
    void bind(void* registry, unsigned name, unsigned version);
    void destroy();
    [[nodiscard]] bool bound() const { return manager_ != nullptr; }
    /// Received a `done` at least once.
    [[nodiscard]] bool synced() const { return synced_; }
    [[nodiscard]] const workspace_view& view() const { return view_; }
    [[nodiscard]] workspace_view& view() { return view_; }
    /// Asks the compositor to show workspace `ws` (activate + commit).
    void activate(int ws);
    /// A workspace handle was removed: the task model must forget it (the proxy is destroyed
    /// right after this returns).
    using removed_fn = void (*)(void* proxy, void* user);
    void set_removed_handler(removed_fn fn, void* user) {
        removed_fn_ = fn;
        removed_user_ = user;
    }

    struct events;

private:
    friend struct events;
    void* manager_ = nullptr; // ext_workspace_manager_v1*
    bool synced_ = false;
    workspace_view view_{};
    removed_fn removed_fn_ = nullptr;
    void* removed_user_ = nullptr;
};

} // namespace lx::shell

// ── workspace_view ─────────────────────────────────────────────────────────

int lx::shell::workspace_view::add_group(void* proxy) {
    for (unsigned i = 0; i < k_max_shell_ws_groups; ++i) {
        if (groups_[i].used)
            continue;
        groups_[i] = {};
        groups_[i].used = true;
        groups_[i].proxy = proxy;
        dirty_ = true;
        return static_cast<int>(i);
    }
    return -1;
}

void lx::shell::workspace_view::output_enter(int g, void* output) {
    if (g < 0 || static_cast<unsigned>(g) >= k_max_shell_ws_groups || !groups_[g].used || !output)
        return;
    shell_ws_group& gr = groups_[g];
    for (unsigned i = 0; i < gr.output_count; ++i)
        if (gr.outputs[i] == output)
            return;
    if (gr.output_count < k_ws_group_outputs)
        gr.outputs[gr.output_count++] = output;
    dirty_ = true;
}

void lx::shell::workspace_view::output_leave(int g, void* output) {
    if (g < 0 || static_cast<unsigned>(g) >= k_max_shell_ws_groups || !groups_[g].used)
        return;
    shell_ws_group& gr = groups_[g];
    for (unsigned i = 0; i < gr.output_count; ++i) {
        if (gr.outputs[i] != output)
            continue;
        gr.outputs[i] = gr.outputs[--gr.output_count];
        gr.outputs[gr.output_count] = nullptr;
        dirty_ = true;
        return;
    }
}

void lx::shell::workspace_view::forget_output(void* output) {
    for (unsigned g = 0; g < k_max_shell_ws_groups; ++g)
        if (groups_[g].used)
            output_leave(static_cast<int>(g), output);
    if (dirty_)
        done();
}

void lx::shell::workspace_view::remove_group(int g) {
    if (g < 0 || static_cast<unsigned>(g) >= k_max_shell_ws_groups || !groups_[g].used)
        return;
    for (auto& w : ws_)
        if (w.used && w.group == g)
            w.group = -1;
    groups_[g] = {};
    dirty_ = true;
}

int lx::shell::workspace_view::add_workspace(void* proxy) {
    for (unsigned i = 0; i < k_max_shell_workspaces; ++i) {
        if (ws_[i].used)
            continue;
        ws_[i] = {};
        ws_[i].used = true;
        ws_[i].proxy = proxy;
        dirty_ = true;
        return static_cast<int>(i);
    }
    return -1;
}

void lx::shell::workspace_view::set_name(int w, const char* name) {
    if (const shell_workspace* e = get(w)) {
        (void)e;
        std::snprintf(ws_[w].name, sizeof(ws_[w].name), "%s", name ? name : "");
        ++ws_[w].rev;
        dirty_ = true;
    }
}

void lx::shell::workspace_view::set_coordinate(int w, unsigned c) {
    if (get(w)) {
        ws_[w].coordinate = c;
        dirty_ = true;
    }
}

void lx::shell::workspace_view::set_state(int w, unsigned state) {
    if (get(w) && ws_[w].state != state) {
        ws_[w].state = state;
        ++ws_[w].rev;
        dirty_ = true;
    }
}

void lx::shell::workspace_view::set_caps(int w, unsigned caps) {
    if (get(w))
        ws_[w].caps = caps;
}

void lx::shell::workspace_view::enter(int g, int w) {
    if (get(w) && g >= 0 && static_cast<unsigned>(g) < k_max_shell_ws_groups && groups_[g].used) {
        ws_[w].group = g;
        dirty_ = true;
    }
}

void lx::shell::workspace_view::leave(int g, int w) {
    if (get(w) && ws_[w].group == g) {
        ws_[w].group = -1;
        dirty_ = true;
    }
}

void lx::shell::workspace_view::remove_workspace(int w) {
    if (!get(w))
        return;
    ws_[w] = {};
    dirty_ = true;
}

void lx::shell::workspace_view::done() {
    if (!dirty_)
        return;
    dirty_ = false;
    changed_ = true;
    ++generation_;
}

unsigned lx::shell::workspace_view::of_output(void* output, int* out, unsigned cap) const {
    int group = -1;
    for (unsigned g = 0; g < k_max_shell_ws_groups && group < 0; ++g) {
        if (!groups_[g].used)
            continue;
        for (unsigned i = 0; i < groups_[g].output_count; ++i)
            if (groups_[g].outputs[i] == output)
                group = static_cast<int>(g);
    }
    if (group < 0)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < k_max_shell_workspaces && n < cap; ++i)
        if (ws_[i].used && ws_[i].group == group && !(ws_[i].state & shell_ws_hidden))
            out[n++] = static_cast<int>(i);
    // By coordinate (their order), stable for equal ones.
    std::stable_sort(out, out + n, [&](int a, int b) {
        return ws_[a].coordinate < ws_[b].coordinate;
    });
    return n;
}

const lx::shell::shell_workspace* lx::shell::workspace_view::get(int w) const {
    if (w < 0 || static_cast<unsigned>(w) >= k_max_shell_workspaces || !ws_[w].used)
        return nullptr;
    return &ws_[w];
}

int lx::shell::workspace_view::find(const void* proxy) const {
    if (!proxy)
        return -1;
    for (unsigned i = 0; i < k_max_shell_workspaces; ++i)
        if (ws_[i].used && ws_[i].proxy == proxy)
            return static_cast<int>(i);
    return -1;
}

int lx::shell::workspace_view::find_group(const void* proxy) const {
    if (!proxy)
        return -1;
    for (unsigned i = 0; i < k_max_shell_ws_groups; ++i)
        if (groups_[i].used && groups_[i].proxy == proxy)
            return static_cast<int>(i);
    return -1;
}

bool lx::shell::workspace_view::is_active(const void* proxy) const {
    const shell_workspace* w = get(find(proxy));
    return w && (w->state & shell_ws_active);
}

bool lx::shell::workspace_view::take_changed() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

// ── workspace_client ───────────────────────────────────────────────────────

#if defined(LUMEN_SHELL_EXT_WORKSPACE)

namespace {

constexpr const char* k_ws_category = "shell.workspaces";

} // namespace

// Every proxy's user data is the client (libwayland hands it to the listener); a proxy's
// slot in the view is found by the proxy itself — 64 entries at most, and only per event.
struct lx::shell::workspace_client::events {
    static workspace_client& self(void* data) { return *static_cast<workspace_client*>(data); }

    // ── ext_workspace_manager_v1 ──
    static void workspace_group(void* data, ext_workspace_manager_v1*,
                                ext_workspace_group_handle_v1* g);
    static void workspace(void* data, ext_workspace_manager_v1*, ext_workspace_handle_v1* w);
    static void done(void* data, ext_workspace_manager_v1*) {
        auto& c = self(data);
        c.view_.done();
        if (!c.synced_) {
            c.synced_ = true;
            unsigned n = 0;
            for (int i = 0; i < static_cast<int>(k_max_shell_workspaces); ++i)
                if (c.view_.get(i))
                    ++n;
            lx::trace::logger::global().logf(lx::trace::level::info, k_ws_category,
                                             "ext-workspace: {} workspaces", n);
        }
    }
    static void finished(void* data, ext_workspace_manager_v1* m) {
        auto& c = self(data);
        if (c.manager_ == m)
            c.manager_ = nullptr;
        ext_workspace_manager_v1_destroy(m);
    }

    // ── ext_workspace_group_handle_v1 ──
    static void group_capabilities(void*, ext_workspace_group_handle_v1*, uint32_t) {}
    static void group_output_enter(void* data, ext_workspace_group_handle_v1* g, wl_output* o) {
        auto& c = self(data);
        c.view_.output_enter(c.view_.find_group(g), o);
    }
    static void group_output_leave(void* data, ext_workspace_group_handle_v1* g, wl_output* o) {
        auto& c = self(data);
        c.view_.output_leave(c.view_.find_group(g), o);
    }
    static void group_workspace_enter(void* data, ext_workspace_group_handle_v1* g,
                                      ext_workspace_handle_v1* w) {
        auto& c = self(data);
        c.view_.enter(c.view_.find_group(g), c.view_.find(w));
    }
    static void group_workspace_leave(void* data, ext_workspace_group_handle_v1* g,
                                      ext_workspace_handle_v1* w) {
        auto& c = self(data);
        c.view_.leave(c.view_.find_group(g), c.view_.find(w));
    }
    static void group_removed(void* data, ext_workspace_group_handle_v1* g) {
        auto& c = self(data);
        c.view_.remove_group(c.view_.find_group(g));
        ext_workspace_group_handle_v1_destroy(g);
    }

    // ── ext_workspace_handle_v1 ──
    static void ws_id(void*, ext_workspace_handle_v1*, const char*) {}
    static void ws_name(void* data, ext_workspace_handle_v1* w, const char* name) {
        auto& c = self(data);
        c.view_.set_name(c.view_.find(w), name);
    }
    static void ws_coordinates(void* data, ext_workspace_handle_v1* w, wl_array* coords) {
        unsigned c = 0;
        if (coords && coords->size >= sizeof(std::uint32_t))
            c = *static_cast<const std::uint32_t*>(coords->data);
        auto& v = self(data).view_;
        v.set_coordinate(v.find(w), c);
    }
    static void ws_state(void* data, ext_workspace_handle_v1* w, uint32_t state) {
        auto& v = self(data).view_;
        v.set_state(v.find(w), state);
    }
    static void ws_capabilities(void* data, ext_workspace_handle_v1* w, uint32_t caps) {
        auto& v = self(data).view_;
        v.set_caps(v.find(w), caps);
    }
    static void ws_removed(void* data, ext_workspace_handle_v1* w) {
        auto& c = self(data);
        if (c.removed_fn_)
            c.removed_fn_(w, c.removed_user_);
        c.view_.remove_workspace(c.view_.find(w));
        ext_workspace_handle_v1_destroy(w);
    }
};

namespace {

const ext_workspace_manager_v1_listener k_manager_listener = {
    .workspace_group = lx::shell::workspace_client::events::workspace_group,
    .workspace = lx::shell::workspace_client::events::workspace,
    .done = lx::shell::workspace_client::events::done,
    .finished = lx::shell::workspace_client::events::finished,
};
const ext_workspace_group_handle_v1_listener k_group_listener = {
    .capabilities = lx::shell::workspace_client::events::group_capabilities,
    .output_enter = lx::shell::workspace_client::events::group_output_enter,
    .output_leave = lx::shell::workspace_client::events::group_output_leave,
    .workspace_enter = lx::shell::workspace_client::events::group_workspace_enter,
    .workspace_leave = lx::shell::workspace_client::events::group_workspace_leave,
    .removed = lx::shell::workspace_client::events::group_removed,
};
const ext_workspace_handle_v1_listener k_ws_listener = {
    .id = lx::shell::workspace_client::events::ws_id,
    .name = lx::shell::workspace_client::events::ws_name,
    .coordinates = lx::shell::workspace_client::events::ws_coordinates,
    .state = lx::shell::workspace_client::events::ws_state,
    .capabilities = lx::shell::workspace_client::events::ws_capabilities,
    .removed = lx::shell::workspace_client::events::ws_removed,
};

} // namespace

void lx::shell::workspace_client::events::workspace_group(void* data, ext_workspace_manager_v1*,
                                                          ext_workspace_group_handle_v1* g) {
    auto& c = self(data);
    if (c.view_.add_group(g) < 0) {
        ext_workspace_group_handle_v1_destroy(g); // more groups than outputs can be: ignored
        return;
    }
    ext_workspace_group_handle_v1_add_listener(g, &k_group_listener, &c);
}

void lx::shell::workspace_client::events::workspace(void* data, ext_workspace_manager_v1*,
                                                    ext_workspace_handle_v1* w) {
    auto& c = self(data);
    if (c.view_.add_workspace(w) < 0) {
        ext_workspace_handle_v1_destroy(w);
        return;
    }
    ext_workspace_handle_v1_add_listener(w, &k_ws_listener, &c);
}

void lx::shell::workspace_client::bind(void* registry, unsigned name, unsigned version) {
    if (manager_ || !registry)
        return;
    auto* m = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(static_cast<wl_registry*>(registry), name,
                         &ext_workspace_manager_v1_interface, version < 1 ? version : 1));
    ext_workspace_manager_v1_add_listener(m, &k_manager_listener, this);
    manager_ = m;
}

void lx::shell::workspace_client::activate(int ws) {
    const shell_workspace* w = view_.get(ws);
    if (!manager_ || !w || !w->proxy)
        return;
    ext_workspace_handle_v1_activate(static_cast<ext_workspace_handle_v1*>(w->proxy));
    ext_workspace_manager_v1_commit(static_cast<ext_workspace_manager_v1*>(manager_));
    lx::trace::logger::global().logf(lx::trace::level::info, k_ws_category,
                                     "activate workspace {}", w->name);
}

void lx::shell::workspace_client::destroy() {
    for (int i = 0; i < static_cast<int>(k_max_shell_workspaces); ++i)
        if (const shell_workspace* w = view_.get(i); w && w->proxy) {
            ext_workspace_handle_v1_destroy(static_cast<ext_workspace_handle_v1*>(w->proxy));
            view_.remove_workspace(i);
        }
    for (int g = 0; g < static_cast<int>(k_max_shell_ws_groups); ++g)
        if (const shell_ws_group* gr = view_.group(g); gr && gr->proxy) {
            ext_workspace_group_handle_v1_destroy(static_cast<ext_workspace_group_handle_v1*>(gr->proxy));
            view_.remove_group(g);
        }
    if (manager_) {
        ext_workspace_manager_v1_stop(static_cast<ext_workspace_manager_v1*>(manager_));
        // `finished` would destroy it; at teardown nothing dispatches any more.
        ext_workspace_manager_v1_destroy(static_cast<ext_workspace_manager_v1*>(manager_));
    }
    manager_ = nullptr;
    synced_ = false;
}

#else

struct lx::shell::workspace_client::events {};
void lx::shell::workspace_client::bind(void*, unsigned, unsigned) {}
void lx::shell::workspace_client::activate(int) {}
void lx::shell::workspace_client::destroy() { manager_ = nullptr; }

#endif

lx::shell::workspace_client::~workspace_client() { destroy(); }
