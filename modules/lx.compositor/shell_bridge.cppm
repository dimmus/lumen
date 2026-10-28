module;

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
#include "zlm_shell_v1-server-protocol.h"
#endif
#endif

import lx.foundation;
import lx.trace;
import lx.wayland.server;

export module lx.compositor:shell_bridge;

import std;

import :toplevel;
import :toplevel_icon;

namespace {

constexpr lx::trace::category log_shell{"compositor.shell"};

} // namespace

export namespace lx::compositor {

/// zlm_policy_bridge_v1.toplevel_flag bits.
enum toplevel_flag_bits : unsigned {
    toplevel_flag_not_responding = 0x1,
    toplevel_flag_demands_attention = 0x2,
    /// zlm v7: shown on every workspace of its output.
    toplevel_flag_sticky = 0x4,
};
/// The flags the shell is told for `rec`.
[[nodiscard]] unsigned toplevel_flags_of(const toplevel_record& rec);

/// Who else follows the windows the shell is told about (ext-foreign-toplevel-list, screen
/// capture of windows): told of every map, unmap and title/app id/state change, with or
/// without a shell bound.
struct toplevel_observer {
    void (*mapped)(void* user, toplevel_record& rec) = nullptr;
    void (*unmapped)(void* user, toplevel_record& rec) = nullptr;
    void (*changed)(void* user, const toplevel_record& rec) = nullptr;
    /// The keyboard focus moved to `rec` (null: no window). Optional.
    void (*focused)(void* user, const toplevel_record* rec) = nullptr;
    void* user = nullptr;
};

/// Server side of zlm_policy_bridge_v1: the shell's view of window state. UI affinity.
///
/// The compositor executes and the shell observes, so every state the shell may act on is
/// pushed from here: a snapshot when the bridge is created, then deltas. Each mapped
/// toplevel is represented to the shell by a compositor-created `zlm_toplevel_v1` handle
/// (its user data is the `toplevel_id`), never by the client's xdg objects.
class shell_bridge {
public:
    explicit shell_bridge(wayland::server& server);

    /// Records the shell binary path the privilege check matches against.
    [[nodiscard]] lx::result<void> install(const char* shell_binary_path);
    void set_toplevels(toplevel_manager* toplevels);

    /// A shell created its policy bridge. Replaces any previous bridge and sends the
    /// snapshot batch (snapshot_begin … snapshot_done), then `focus_changed(focused)`: the
    /// snapshot carries no focus, and without it a task list could highlight nothing until
    /// the next focus change.
    void on_bridge_bound(void* policy_bridge_resource, lx::toplevel_id focused);
    /// The bridge resource is being destroyed. Handles it introduced become inert.
    void on_bridge_destroyed(void* policy_bridge_resource);
    [[nodiscard]] bool has_bridge() const { return bridge_ != nullptr; }
    /// The `zlm_policy_bridge_v1` resource of the current bridge (null without one) — its
    /// client is the shell.
    [[nodiscard]] void* bridge_resource() const { return bridge_; }

    /// Runtime deltas (compositor → shell). No-ops without a bridge.
    void emit_toplevel_created(toplevel_record& rec);
    void emit_toplevel_destroyed(toplevel_record& rec);
    void emit_toplevel_state(const toplevel_record& rec);
    void emit_toplevel_geometry(const toplevel_record& rec);
    void emit_focus_changed(const toplevel_record* focused);
    void emit_stacking_order_changed(const toplevel_record& rec, unsigned index, bool done);
    /// toplevel_flags (zlm v2): the attention flags of `rec` changed. Bridges bound at
    /// version 1 never see it.
    void emit_toplevel_flags(const toplevel_record& rec);
    /// zlm v6: the icon of `rec` (toplevel_icon, its buffers' memfds, toplevel_icon_done),
    /// its parent (toplevel_parent; null when it has none, or the parent is not mapped), and a
    /// bell rung for it (null: for no window). Bridges below version 6 never see them.
    void emit_toplevel_icon(const toplevel_record& rec);
    void emit_toplevel_parent(const toplevel_record& rec);
    void emit_bell(const toplevel_record* rec);
    /// zlm v7 toplevel_workspace: the shell's own ext_workspace_handle_v1 for `rec`'s
    /// workspace (null: sticky, or the shell has no handle for it). Bridges below version 7
    /// never see it.
    void emit_toplevel_workspace(const toplevel_record& rec);
    /// toplevel_workspace for every window again (the shell bound ext-workspace after the
    /// bridge: its handles exist now).
    void resend_toplevel_workspaces();
    /// The ext_workspace_handle_v1 of workspace `ws` that `client` (`wl_client*`) holds, or
    /// null (installed by the ext-workspace wire).
    using workspace_lookup_fn = void* (*)(void* client, lx::workspace_id ws);
    void set_workspace_lookup(workspace_lookup_fn fn) { workspace_lookup_ = fn; }
    /// The icon store the toplevel records' `icon` ids name (null: icons are never sent).
    void set_icons(const toplevel_icon_table* icons) { icons_ = icons; }
    void set_observer(toplevel_observer observer) { observer_ = observer; }
    /// A second observer: the control socket's event stream (lumen-ctl monitor).
    void set_control_observer(toplevel_observer observer) { control_observer_ = observer; }

    /// Called at the end of every `on_bridge_bound`, with the shell's `wl_client*`.
    void set_bound_handler(void (*bound)(void* user, void* client_native), void* user) {
        bound_ = bound;
        bound_user_ = user;
    }

    /// Bridges bound since start: 1 for the first shell, +1 for each restart (health).
    [[nodiscard]] unsigned binds() const { return binds_; }
    /// The process of the current bridge's client (0: none or unknown).
    [[nodiscard]] int bridge_pid() const { return bridge_ ? bridge_pid_ : 0; }

    /// Decodes a `zlm_toplevel_v1` handle from a request. Null id for an inert handle.
    [[nodiscard]] static lx::toplevel_id toplevel_from_handle(void* zlm_toplevel_resource);

private:
    /// Creates the shell's handle for `rec` on the bridge's client.
    [[nodiscard]] void* create_handle(toplevel_record& rec);
    void detach_all_handles();
    /// The wire toplevel_state of `rec`: `modal` for an effectively modal dialog in the
    /// normal state.
    [[nodiscard]] unsigned wire_state_of(const toplevel_record& rec) const;
    /// The v6 extras after a toplevel is introduced: its icon (when not the default).
    void send_extras(const toplevel_record& rec);

    wayland::server* server_ = nullptr;
    toplevel_manager* toplevels_ = nullptr;
    const toplevel_icon_table* icons_ = nullptr;
    toplevel_observer observer_{};
    toplevel_observer control_observer_{};
    const char* shell_binary_path_ = "lumen-shell";
    void* bridge_ = nullptr;
    unsigned binds_ = 0;
    int bridge_pid_ = 0;
    void (*bound_)(void*, void*) = nullptr;
    void* bound_user_ = nullptr;
    workspace_lookup_fn workspace_lookup_ = nullptr;
};

} // namespace lx::compositor

namespace {

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
/// The handle destructor has only the resource to go on. One compositor per process.
lx::compositor::toplevel_manager* g_handle_toplevels = nullptr;

void handle_destroy_resource(wl_resource* resource) {
    const auto id = lx::compositor::shell_bridge::toplevel_from_handle(resource);
    if (!id || !g_handle_toplevels)
        return;
    if (auto* rec = g_handle_toplevels->find_mut(id); rec && rec->shell_handle == resource)
        rec->shell_handle = nullptr;
}

const struct zlm_toplevel_v1_interface toplevel_handle_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

std::uint32_t wire_state(lx::toplevel_state state) {
    switch (state) {
    case lx::toplevel_state::maximized: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_MAXIMIZED;
    case lx::toplevel_state::fullscreen: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_FULLSCREEN;
    case lx::toplevel_state::minimized: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_MINIMIZED;
    case lx::toplevel_state::tiled: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_TILED;
    case lx::toplevel_state::modal: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_MODAL;
    case lx::toplevel_state::normal:
    default: return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_NORMAL;
    }
}
/// The bridge speaks zlm v7 (toplevel_workspace, the sticky flag).
bool bridge_v7(wl_resource* bridge) {
#if defined(ZLM_POLICY_BRIDGE_V1_TOPLEVEL_WORKSPACE_SINCE_VERSION)
    return bridge &&
           wl_resource_get_version(bridge) >= ZLM_POLICY_BRIDGE_V1_TOPLEVEL_WORKSPACE_SINCE_VERSION;
#else
    (void)bridge;
    return false;
#endif
}
/// The bridge speaks zlm v6 (icons, parents, bell).
bool bridge_v6(wl_resource* bridge) {
#if defined(ZLM_POLICY_BRIDGE_V1_BELL_SINCE_VERSION)
    return bridge && wl_resource_get_version(bridge) >= ZLM_POLICY_BRIDGE_V1_BELL_SINCE_VERSION;
#else
    (void)bridge;
    return false;
#endif
}
/// toplevel_flags to a bridge that knows it (v2), when `flags` is worth sending.
void send_flags(wl_resource* bridge, wl_resource* handle, unsigned flags) {
#if defined(ZLM_POLICY_BRIDGE_V1_TOPLEVEL_FLAGS_SINCE_VERSION)
    // `sticky` is a version 7 bit: an older shell would not know it.
    if (!bridge_v7(bridge))
        flags &= ~static_cast<unsigned>(lx::compositor::toplevel_flag_sticky);
    if (bridge && handle &&
        wl_resource_get_version(bridge) >= ZLM_POLICY_BRIDGE_V1_TOPLEVEL_FLAGS_SINCE_VERSION)
        zlm_policy_bridge_v1_send_toplevel_flags(bridge, handle, flags);
#else
    (void)bridge;
    (void)handle;
    (void)flags;
#endif
}
#endif

} // namespace

unsigned lx::compositor::toplevel_flags_of(const toplevel_record& rec) {
    return (rec.not_responding ? static_cast<unsigned>(toplevel_flag_not_responding) : 0u) |
           (rec.demands_attention ? static_cast<unsigned>(toplevel_flag_demands_attention) : 0u) |
           (rec.sticky ? static_cast<unsigned>(toplevel_flag_sticky) : 0u);
}

lx::compositor::shell_bridge::shell_bridge(wayland::server& server) : server_{&server} {}

lx::result<void> lx::compositor::shell_bridge::install(const char* shell_binary_path) {
    shell_binary_path_ = shell_binary_path ? shell_binary_path : "lumen-shell";
    if (!server_) {
        return lx::make_error(lx::wayland_err::bind_failed, "shell_bridge has no server");
    }
    server_->set_shell_binary_path(shell_binary_path_);
    return {};
}

void lx::compositor::shell_bridge::set_toplevels(toplevel_manager* toplevels) {
    toplevels_ = toplevels;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    g_handle_toplevels = toplevels;
#endif
}

lx::toplevel_id lx::compositor::shell_bridge::toplevel_from_handle(void* resource) {
#if defined(LUMEN_HAS_WAYLAND)
    if (!resource)
        return {};
    return lx::toplevel_id{static_cast<lx::toplevel_id::id_type>(reinterpret_cast<std::uintptr_t>(
        wl_resource_get_user_data(static_cast<wl_resource*>(resource))))};
#else
    (void)resource;
    return {};
#endif
}

void* lx::compositor::shell_bridge::create_handle(toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_)
        return nullptr;
    auto* bridge = static_cast<wl_resource*>(bridge_);
    // id 0: server-allocated, introduced to the client by the new_id argument of the event
    // it is first sent in (toplevel_created / snapshot_toplevel).
    // The handle takes the bridge's version: the family moves together.
    wl_resource* handle = wl_resource_create(wl_resource_get_client(bridge),
                                             &zlm_toplevel_v1_interface,
                                             wl_resource_get_version(bridge), 0);
    if (!handle) {
        wl_client_post_no_memory(wl_resource_get_client(bridge));
        return nullptr;
    }
    wl_resource_set_implementation(handle, &toplevel_handle_impl,
                                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(rec.id.id())),
                                   handle_destroy_resource);
    rec.shell_handle = handle;
    return handle;
#else
    (void)rec;
    return nullptr;
#endif
}

void lx::compositor::shell_bridge::detach_all_handles() {
#if defined(LUMEN_HAS_WAYLAND)
    if (!toplevels_)
        return;
    // The handles stay alive until the shell destroys them; they just stop meaning anything.
    toplevels_->for_each_mut([](toplevel_record& rec) {
        if (!rec.shell_handle)
            return;
        wl_resource_set_user_data(static_cast<wl_resource*>(rec.shell_handle), nullptr);
        rec.shell_handle = nullptr;
    });
#endif
}

void lx::compositor::shell_bridge::on_bridge_bound(void* resource, lx::toplevel_id focused) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (bridge_ && bridge_ != resource) {
        // Only the configured shell binary gets here (the privilege check), so this is a
        // second instance of it: the newest wins and the old one's handles go inert.
        log_shell.warn("bridge replaced: a second shell bound "
                       "zlm_policy_bridge_v1; the previous one goes inert");
        detach_all_handles();
    }
    bridge_ = resource;
    if (!bridge_ || !toplevels_)
        return;
    auto* bridge = static_cast<wl_resource*>(bridge_);
    ++binds_;
    {
        pid_t pid = 0;
        uid_t uid = 0;
        gid_t gid = 0;
        wl_client_get_credentials(wl_resource_get_client(bridge), &pid, &uid, &gid);
        bridge_pid_ = static_cast<int>(pid);
    }

    // zlm_workspace_v1 is deprecated (never implemented): `workspace` stays null and no
    // snapshot_workspace is sent; v7 toplevel_workspace names the shell's ext-workspace
    // handle instead. snapshot_output is skipped too: its wl_output is not nullable and this
    // client may not have bound one.
    zlm_policy_bridge_v1_send_snapshot_begin(bridge);
    // Creation order (toplevel ids only grow), each with its stacking index: a shell lists
    // windows the way they appeared, and a restarted shell then shows the same order it
    // built from deltas before — stacking order would reshuffle its task list. Once per
    // bind, into fixed storage.
    lx::toplevel_id ids[toplevel_manager::k_max];
    unsigned index = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& rec) {
        if (index < toplevel_manager::k_max)
            ids[index++] = rec.id;
    });
    for (unsigned i = 1; i < index; ++i)
        for (unsigned j = i; j > 0 && ids[j].id() < ids[j - 1].id(); --j) {
            const lx::toplevel_id t = ids[j];
            ids[j] = ids[j - 1];
            ids[j - 1] = t;
        }
    for (unsigned i = 0; i < index; ++i) {
        auto* rec = toplevels_->find_mut(ids[i]);
        void* handle = rec ? create_handle(*rec) : nullptr;
        if (handle) {
            zlm_policy_bridge_v1_send_snapshot_toplevel(
                bridge, static_cast<wl_resource*>(handle), nullptr, rec->app_id, rec->title,
                wire_state_of(*rec), rec->geometry.x, rec->geometry.y, rec->geometry.width,
                rec->geometry.height, toplevels_->stacking_index(rec->id));
            if (const unsigned flags = toplevel_flags_of(*rec))
                send_flags(bridge, static_cast<wl_resource*>(handle), flags);
            // v7: its workspace (null while the shell has bound no ext-workspace manager;
            // re-sent once it has).
            emit_toplevel_workspace(*rec);
        }
    }
    // v6: icons, then parents once every handle exists (a parent can come later in the
    // creation order than its child — a window reparented across clients).
    for (unsigned i = 0; i < index; ++i) {
        if (const auto* rec = toplevels_->find(ids[i]); rec && rec->shell_handle) {
            send_extras(*rec);
            if (rec->parent)
                emit_toplevel_parent(*rec);
        }
    }
    zlm_policy_bridge_v1_send_snapshot_done(bridge);
    // A delta valid in v1, sent at once so the shell starts with the focus as it is.
    const toplevel_record* rec = focused ? toplevels_->find(focused) : nullptr;
    zlm_policy_bridge_v1_send_focus_changed(
        bridge, rec && rec->shell_handle ? static_cast<wl_resource*>(rec->shell_handle)
                                         : nullptr);

    {
        unsigned cid = 0;
        if (server_)
            if (auto* conn = server_->find_client_by_native(wl_resource_get_client(bridge)))
                cid = conn->id();
        log_shell.info("bridge bound client={} snapshot={} focus={} pid={} "
                       "binds={}",
                       cid, index, rec && rec->shell_handle ? rec->id.id() : 0ull, bridge_pid_,
                       binds_);
    }
    if (bound_)
        bound_(bound_user_, wl_resource_get_client(bridge));
#else
    (void)resource;
    (void)focused;
#endif
}

void lx::compositor::shell_bridge::on_bridge_destroyed(void* resource) {
    if (!resource || resource != bridge_)
        return;
    detach_all_handles();
    bridge_ = nullptr;
    bridge_pid_ = 0;
    log_shell.info("bridge destroyed");
}

void lx::compositor::shell_bridge::emit_toplevel_created(toplevel_record& rec) {
    if (observer_.mapped)
        observer_.mapped(observer_.user, rec);
    if (control_observer_.mapped)
        control_observer_.mapped(control_observer_.user, rec);
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_ || rec.shell_handle)
        return;
    void* handle = create_handle(rec);
    if (!handle)
        return;
    auto* bridge = static_cast<wl_resource*>(bridge_);
    auto* h = static_cast<wl_resource*>(handle);
    zlm_policy_bridge_v1_send_toplevel_created(bridge, h, nullptr);
    zlm_policy_bridge_v1_send_toplevel_state(bridge, h, rec.app_id, rec.title,
                                             wire_state_of(rec));
    zlm_policy_bridge_v1_send_toplevel_geometry(bridge, h, rec.geometry.x, rec.geometry.y,
                                                rec.geometry.width, rec.geometry.height);
    if (const unsigned flags = toplevel_flags_of(rec))
        send_flags(bridge, h, flags);
    send_extras(rec);
    if (rec.parent)
        emit_toplevel_parent(rec);
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_flags(const toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_ || !rec.shell_handle)
        return;
    send_flags(static_cast<wl_resource*>(bridge_), static_cast<wl_resource*>(rec.shell_handle),
               toplevel_flags_of(rec));
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_destroyed(toplevel_record& rec) {
    if (observer_.unmapped)
        observer_.unmapped(observer_.user, rec);
    if (control_observer_.unmapped)
        control_observer_.unmapped(control_observer_.user, rec);
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!rec.shell_handle)
        return;
    auto* h = static_cast<wl_resource*>(rec.shell_handle);
    if (bridge_)
        zlm_policy_bridge_v1_send_toplevel_destroyed(static_cast<wl_resource*>(bridge_), h);
    // The shell owns its reference and releases it with zlm_toplevel_v1.destroy; until
    // then the handle is inert, so a late request on it is ignored rather than misrouted.
    wl_resource_set_user_data(h, nullptr);
    rec.shell_handle = nullptr;
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_state(const toplevel_record& rec) {
    if (observer_.changed)
        observer_.changed(observer_.user, rec);
    if (control_observer_.changed)
        control_observer_.changed(control_observer_.user, rec);
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_ || !rec.shell_handle)
        return;
    zlm_policy_bridge_v1_send_toplevel_state(static_cast<wl_resource*>(bridge_),
                                             static_cast<wl_resource*>(rec.shell_handle),
                                             rec.app_id, rec.title, wire_state_of(rec));
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_geometry(const toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_ || !rec.shell_handle)
        return;
    zlm_policy_bridge_v1_send_toplevel_geometry(static_cast<wl_resource*>(bridge_),
                                                static_cast<wl_resource*>(rec.shell_handle),
                                                rec.geometry.x, rec.geometry.y,
                                                rec.geometry.width, rec.geometry.height);
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_focus_changed(const toplevel_record* focused) {
    if (control_observer_.focused)
        control_observer_.focused(control_observer_.user, focused);
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_)
        return;
    zlm_policy_bridge_v1_send_focus_changed(
        static_cast<wl_resource*>(bridge_),
        focused ? static_cast<wl_resource*>(focused->shell_handle) : nullptr);
#else
    (void)focused;
#endif
}

void lx::compositor::shell_bridge::emit_stacking_order_changed(const toplevel_record& rec,
                                                               unsigned index, bool done) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    if (!bridge_ || !rec.shell_handle)
        return;
    zlm_policy_bridge_v1_send_stacking_order_changed(static_cast<wl_resource*>(bridge_),
                                                     static_cast<wl_resource*>(rec.shell_handle),
                                                     index, done ? 1u : 0u);
#else
    (void)rec;
    (void)index;
    (void)done;
#endif
}

unsigned lx::compositor::shell_bridge::wire_state_of(const toplevel_record& rec) const {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE)
    // Minimized wins: the window is not on screen whatever xdg state it keeps.
    if (rec.minimized)
        return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_MINIMIZED;
    if (rec.state == lx::toplevel_state::normal && toplevels_ && effective_modal(*toplevels_, rec))
        return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_MODAL;
    // Snapped to a half or a quarter (design-session-and-wm K.3), or in a layout (ADR 012).
    if (rec.state == lx::toplevel_state::normal && (rec.snap || rec.tiled))
        return ZLM_POLICY_BRIDGE_V1_TOPLEVEL_STATE_TILED;
    return wire_state(rec.state);
#else
    (void)rec;
    return 0;
#endif
}

void lx::compositor::shell_bridge::send_extras(const toplevel_record& rec) {
    if (rec.icon)
        emit_toplevel_icon(rec);
}

void lx::compositor::shell_bridge::emit_toplevel_icon(const toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) && \
    defined(ZLM_POLICY_BRIDGE_V1_BELL_SINCE_VERSION)
    auto* bridge = static_cast<wl_resource*>(bridge_);
    if (!bridge_v6(bridge) || !rec.shell_handle)
        return;
    auto* h = static_cast<wl_resource*>(rec.shell_handle);
    const icon_entry* e = icons_ && rec.icon ? icons_->get(rec.icon) : nullptr;
    zlm_policy_bridge_v1_send_toplevel_icon(bridge, h, e ? e->name : "");
    if (e) {
        // libwayland dups each fd into the message: the table keeps its own.
        for (unsigned i = 0; i < e->count; ++i)
            if (e->images[i].fd >= 0)
                zlm_policy_bridge_v1_send_toplevel_icon_buffer(bridge, h, e->images[i].fd,
                                                               e->images[i].size,
                                                               e->images[i].scale);
    }
    zlm_policy_bridge_v1_send_toplevel_icon_done(bridge, h);
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_parent(const toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) && \
    defined(ZLM_POLICY_BRIDGE_V1_BELL_SINCE_VERSION)
    auto* bridge = static_cast<wl_resource*>(bridge_);
    if (!bridge_v6(bridge) || !rec.shell_handle)
        return;
    const toplevel_record* p = toplevels_ && rec.parent ? toplevels_->find(rec.parent) : nullptr;
    zlm_policy_bridge_v1_send_toplevel_parent(
        bridge, static_cast<wl_resource*>(rec.shell_handle),
        p && p->shell_handle ? static_cast<wl_resource*>(p->shell_handle) : nullptr);
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_bell(const toplevel_record* rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) && \
    defined(ZLM_POLICY_BRIDGE_V1_BELL_SINCE_VERSION)
    auto* bridge = static_cast<wl_resource*>(bridge_);
    if (!bridge_v6(bridge))
        return;
    zlm_policy_bridge_v1_send_bell(
        bridge, rec && rec->shell_handle ? static_cast<wl_resource*>(rec->shell_handle) : nullptr);
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::emit_toplevel_workspace(const toplevel_record& rec) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) && \
    defined(ZLM_POLICY_BRIDGE_V1_TOPLEVEL_WORKSPACE_SINCE_VERSION)
    auto* bridge = static_cast<wl_resource*>(bridge_);
    if (!bridge_v7(bridge) || !rec.shell_handle)
        return;
    void* ws = !rec.sticky && rec.workspace && workspace_lookup_
                   ? workspace_lookup_(wl_resource_get_client(bridge), rec.workspace)
                   : nullptr;
    zlm_policy_bridge_v1_send_toplevel_workspace(bridge,
                                                 static_cast<wl_resource*>(rec.shell_handle),
                                                 static_cast<wl_resource*>(ws));
#else
    (void)rec;
#endif
}

void lx::compositor::shell_bridge::resend_toplevel_workspaces() {
    if (!bridge_ || !toplevels_)
        return;
    toplevels_->for_each([&](const toplevel_record& rec) {
        if (rec.mapped)
            emit_toplevel_workspace(rec);
    });
}
