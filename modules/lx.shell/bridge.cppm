module;

#include <cstdint>
#include <cstdio>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#include "zlm_shell_v1-client-protocol.h"
#endif

import lx.foundation;
import lx.trace;

export module lx.shell:bridge;

import :task_model;

export namespace lx::shell {

/// Client side of zlm_shell_v1 / zlm_policy_bridge_v1: the shell's window list and its
/// window-management requests. Single-threaded (the shell's one Wayland thread).
///
/// The compositor executes and the shell observes: a snapshot (snapshot_begin …
/// snapshot_done, then a focus_changed), then deltas, all applied to a `task_model`. Each
/// window is a compositor-created zlm_toplevel_v1 the shell never sees an xdg object for;
/// libwayland creates its proxy from the event's new_id and the bridge stores the model
/// slot in the proxy's user data. No Wayland type is exported.
class policy_bridge {
public:
    policy_bridge() = default;
    ~policy_bridge();
    policy_bridge(const policy_bridge&) = delete;
    policy_bridge& operator=(const policy_bridge&) = delete;

    /// Starts listening on a `zlm_shell_v1*` just bound. Call it in the same dispatch as
    /// the bind: `capabilities` follows the bind at once, and libwayland drops events that
    /// reach a proxy with no listener.
    [[nodiscard]] lx::result<void> observe(void* zlm_shell);
    /// Creates the policy bridge on the observed `zlm_shell_v1` (observing it first if
    /// that was not done). The snapshot arrives with the next dispatch.
    [[nodiscard]] lx::result<void> attach(void* zlm_shell);
    /// Destroys the bridge and every handle (the compositor sees `bridge destroyed`).
    void detach();

    [[nodiscard]] const task_model& model() const { return model_; }
    [[nodiscard]] task_model& model() { return model_; }
    [[nodiscard]] bool attached() const { return bridge_ != nullptr; }
    /// zlm_shell_v1.capabilities, once received.
    [[nodiscard]] bool has_capabilities() const { return has_caps_; }
    [[nodiscard]] unsigned capability_flags() const { return caps_; }
    [[nodiscard]] unsigned compositor_version() const { return caps_version_; }

    /// Shell → compositor intent. A stale handle (the window is gone) sends nothing.
    void request_activate(task_handle h);
    void request_raise(task_handle h);
    void request_lower(task_handle h);
    void request_close(task_handle h);
    void request_maximize(task_handle h);
    void request_unmaximize(task_handle h);
    void request_minimize(task_handle h);

    /// Wire events, one per protocol event (defined with the listeners).
    struct events;

private:
    friend struct events;
    [[nodiscard]] void* proxy_of(task_handle h) const;

    task_model model_{};
    void* shell_ = nullptr;  // zlm_shell_v1*, not owned
    void* bridge_ = nullptr; // zlm_policy_bridge_v1*
    unsigned caps_ = 0;
    unsigned caps_version_ = 0;
    bool has_caps_ = false;
};

} // namespace lx::shell

namespace {

constexpr const char* k_bridge_category = "shell.bridge";

[[nodiscard]] bool bridge_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void bridge_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_bridge_category, line);
}

} // namespace

#if defined(LUMEN_SHELL_WAYLAND)

namespace {

[[nodiscard]] unsigned slot_hint(zlm_toplevel_v1* t) {
    return static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(zlm_toplevel_v1_get_user_data(t)));
}

void set_slot(zlm_toplevel_v1* t, lx::shell::task_handle h) {
    zlm_toplevel_v1_set_user_data(
        t, reinterpret_cast<void*>(static_cast<std::uintptr_t>(h ? h.index : ~0u)));
}

void destroy_handle(void* proxy, void*) {
    zlm_toplevel_v1_destroy(static_cast<zlm_toplevel_v1*>(proxy));
}

} // namespace

struct lx::shell::policy_bridge::events {
    static policy_bridge& self(void* data) { return *static_cast<policy_bridge*>(data); }

    static task_handle find(policy_bridge& b, zlm_toplevel_v1* t) {
        return t ? b.model_.find_proxy(slot_hint(t), t) : task_handle{};
    }

    // ── zlm_shell_v1 ──
    static void capabilities(void* data, zlm_shell_v1*, uint32_t flags, uint32_t version) {
        auto& b = self(data);
        b.caps_ = flags;
        b.caps_version_ = version;
        b.has_caps_ = true;
        char line[96];
        std::snprintf(line, sizeof(line), "capabilities flags=0x%x version=%u", flags, version);
        bridge_log(lx::trace::level::info, line);
    }

    // ── zlm_policy_bridge_v1: snapshot ──
    static void snapshot_begin(void* data, zlm_policy_bridge_v1*) {
        self(data).model_.begin_snapshot(destroy_handle, nullptr);
    }
    static void snapshot_workspace(void*, zlm_policy_bridge_v1*, zlm_workspace_v1*, uint32_t,
                                   const char*, uint32_t) {}
    static void snapshot_toplevel(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                  zlm_workspace_v1*, const char* app_id, const char* title,
                                  uint32_t state, int32_t x, int32_t y, int32_t w, int32_t h,
                                  uint32_t stacking_index) {
        auto& b = self(data);
        const task_handle th =
            b.model_.snapshot_add(t, app_id, title, state, {x, y, w, h}, stacking_index);
        if (!th) {
            // Table full: the window cannot be shown, and its handle is not kept.
            bridge_log(lx::trace::level::warn, "snapshot: task table full, window dropped");
            zlm_toplevel_v1_destroy(t);
            return;
        }
        set_slot(t, th);
        if (bridge_debug()) {
            char line[384];
            std::snprintf(line, sizeof(line), "snapshot toplevel handle=%u app_id=%s title=\"%s\"",
                          th.index, app_id ? app_id : "", title ? title : "");
            bridge_log(lx::trace::level::debug, line);
        }
    }
    static void snapshot_output(void*, zlm_policy_bridge_v1*, wl_output*, const char*, int32_t,
                                int32_t, int32_t) {}
    static void snapshot_done(void* data, zlm_policy_bridge_v1*) {
        auto& b = self(data);
        b.model_.end_snapshot();
        char line[64];
        std::snprintf(line, sizeof(line), "snapshot toplevels=%u", b.model_.count());
        bridge_log(lx::trace::level::info, line);
    }

    // ── zlm_policy_bridge_v1: deltas ──
    static void toplevel_created(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                 zlm_workspace_v1*) {
        auto& b = self(data);
        const task_handle th = b.model_.add(t);
        if (!th) {
            bridge_log(lx::trace::level::warn, "add: task table full, window dropped");
            zlm_toplevel_v1_destroy(t);
            return;
        }
        set_slot(t, th);
        if (bridge_debug()) {
            char line[64];
            std::snprintf(line, sizeof(line), "add handle=%u count=%u", th.index,
                          b.model_.count());
            bridge_log(lx::trace::level::debug, line);
        }
    }
    static void toplevel_destroyed(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        const bool removed = b.model_.remove(th);
        if (bridge_debug()) {
            char line[64];
            std::snprintf(line, sizeof(line), "remove handle=%d count=%u",
                          removed ? static_cast<int>(th.index) : -1, b.model_.count());
            bridge_log(lx::trace::level::debug, line);
        }
        // The compositor made the handle inert; the shell releases its reference.
        if (t)
            zlm_toplevel_v1_destroy(t);
    }
    static void toplevel_state(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                               const char* app_id, const char* title, uint32_t state) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        const unsigned changed = b.model_.set_state(th, app_id, title, state);
        if ((changed & task_change_label) && bridge_debug()) {
            const task_entry* e = b.model_.get(th);
            char line[448];
            std::snprintf(line, sizeof(line), "title handle=%u app_id=%s title=\"%s\"",
                          th.index, e ? e->app_id : "", e ? e->title : "");
            bridge_log(lx::trace::level::debug, line);
        }
        if ((changed & task_change_state) && bridge_debug()) {
            char line[64];
            std::snprintf(line, sizeof(line), "state handle=%u state=%u", th.index, state);
            bridge_log(lx::trace::level::debug, line);
        }
    }
    static void toplevel_geometry(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
        // Up to once per frame during a move: stored, never logged.
        auto& b = self(data);
        (void)b.model_.set_geometry(find(b, t), {x, y, w, h});
    }
    static void focus_changed(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t) {
        auto& b = self(data);
        b.model_.set_focus(find(b, t));
        if (bridge_debug()) {
            const task_handle f = b.model_.focused();
            char line[64];
            if (f)
                std::snprintf(line, sizeof(line), "focus handle=%u", f.index);
            else
                std::snprintf(line, sizeof(line), "focus handle=none");
            bridge_log(lx::trace::level::debug, line);
        }
    }
    static void stacking_order_changed(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                       uint32_t index, uint32_t) {
        auto& b = self(data);
        (void)b.model_.set_stacking(find(b, t), index);
    }
    // ── zlm_policy_bridge_v1 v2 ──
    static void toplevel_flags(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                               uint32_t flags) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        if (b.model_.set_flags(th, flags) && bridge_debug()) {
            char line[64];
            std::snprintf(line, sizeof(line), "flags handle=%u flags=%u", th.index, flags);
            bridge_log(lx::trace::level::debug, line);
        }
    }
};

namespace {

const zlm_shell_v1_listener k_shell_listener = {
    .capabilities = lx::shell::policy_bridge::events::capabilities,
};

const zlm_policy_bridge_v1_listener k_bridge_listener = {
    .snapshot_begin = lx::shell::policy_bridge::events::snapshot_begin,
    .snapshot_workspace = lx::shell::policy_bridge::events::snapshot_workspace,
    .snapshot_toplevel = lx::shell::policy_bridge::events::snapshot_toplevel,
    .snapshot_output = lx::shell::policy_bridge::events::snapshot_output,
    .snapshot_done = lx::shell::policy_bridge::events::snapshot_done,
    .toplevel_created = lx::shell::policy_bridge::events::toplevel_created,
    .toplevel_destroyed = lx::shell::policy_bridge::events::toplevel_destroyed,
    .toplevel_state = lx::shell::policy_bridge::events::toplevel_state,
    .toplevel_geometry = lx::shell::policy_bridge::events::toplevel_geometry,
    .focus_changed = lx::shell::policy_bridge::events::focus_changed,
    .stacking_order_changed = lx::shell::policy_bridge::events::stacking_order_changed,
    .toplevel_flags = lx::shell::policy_bridge::events::toplevel_flags,
};

} // namespace

lx::result<void> lx::shell::policy_bridge::observe(void* zlm_shell) {
    if (!zlm_shell)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "no zlm_shell_v1");
    if (shell_ == zlm_shell)
        return {};
    if (zlm_shell_v1_add_listener(static_cast<zlm_shell_v1*>(zlm_shell), &k_shell_listener,
                                  this) != 0)
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "zlm_shell_v1 already has a listener");
    shell_ = zlm_shell;
    return {};
}

lx::result<void> lx::shell::policy_bridge::attach(void* zlm_shell) {
    if (bridge_)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "bridge already attached");
    if (auto observed = observe(zlm_shell); !observed)
        return observed;
    auto* shell = static_cast<zlm_shell_v1*>(zlm_shell);
    zlm_policy_bridge_v1* bridge = zlm_shell_v1_get_policy_bridge(shell);
    if (!bridge)
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "zlm_shell_v1.get_policy_bridge failed");
    zlm_policy_bridge_v1_add_listener(bridge, &k_bridge_listener, this);
    bridge_ = bridge;
    return {};
}

void lx::shell::policy_bridge::detach() {
    if (!bridge_)
        return;
    task_handle all[k_max_tasks];
    const unsigned n = model_.ordered(all, k_max_tasks);
    for (unsigned i = 0; i < n; ++i)
        if (const task_entry* e = model_.get(all[i]); e && e->proxy)
            zlm_toplevel_v1_destroy(static_cast<zlm_toplevel_v1*>(e->proxy));
    model_.clear(); // the proxies are already gone
    zlm_policy_bridge_v1_destroy(static_cast<zlm_policy_bridge_v1*>(bridge_));
    bridge_ = nullptr;
}

void* lx::shell::policy_bridge::proxy_of(task_handle h) const {
    const task_entry* e = model_.get(h);
    return bridge_ && e ? e->proxy : nullptr;
}

#define LX_BRIDGE_REQUEST(name, wire)                                                          \
    void lx::shell::policy_bridge::name(task_handle h) {                                       \
        if (void* p = proxy_of(h))                                                             \
            wire(static_cast<zlm_policy_bridge_v1*>(bridge_), static_cast<zlm_toplevel_v1*>(p)); \
    }
LX_BRIDGE_REQUEST(request_activate, zlm_policy_bridge_v1_request_activate)
LX_BRIDGE_REQUEST(request_raise, zlm_policy_bridge_v1_request_raise)
LX_BRIDGE_REQUEST(request_lower, zlm_policy_bridge_v1_request_lower)
LX_BRIDGE_REQUEST(request_close, zlm_policy_bridge_v1_request_close)
LX_BRIDGE_REQUEST(request_maximize, zlm_policy_bridge_v1_request_maximize)
LX_BRIDGE_REQUEST(request_unmaximize, zlm_policy_bridge_v1_request_unmaximize)
LX_BRIDGE_REQUEST(request_minimize, zlm_policy_bridge_v1_request_minimize)
#undef LX_BRIDGE_REQUEST

lx::shell::policy_bridge::~policy_bridge() { detach(); }

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::policy_bridge::events {};

lx::result<void> lx::shell::policy_bridge::observe(void*) {
    return lx::not_implemented("lx::shell::policy_bridge::observe");
}
lx::result<void> lx::shell::policy_bridge::attach(void*) {
    return lx::not_implemented("lx::shell::policy_bridge::attach");
}
void lx::shell::policy_bridge::detach() {}
void* lx::shell::policy_bridge::proxy_of(task_handle) const { return nullptr; }
void lx::shell::policy_bridge::request_activate(task_handle) {}
void lx::shell::policy_bridge::request_raise(task_handle) {}
void lx::shell::policy_bridge::request_lower(task_handle) {}
void lx::shell::policy_bridge::request_close(task_handle) {}
void lx::shell::policy_bridge::request_maximize(task_handle) {}
void lx::shell::policy_bridge::request_unmaximize(task_handle) {}
void lx::shell::policy_bridge::request_minimize(task_handle) {}
lx::shell::policy_bridge::~policy_bridge() = default;

#endif
