module;

#include <stdint.h>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#include "zlm_shell_v1-client-protocol.h"
#endif
#include <time.h>

import lx.foundation;
import lx.trace;

export module lx.shell:bridge;

import std;

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
    /// zlm_shell_v1 v3: whether the compositor can end the session on request.
    [[nodiscard]] bool can_logout() const;
    /// Asks the compositor to close every window and end the session.
    void request_logout();
    /// The shell asked for the session to end: losing the connection is then expected.
    [[nodiscard]] bool logout_requested() const { return logout_requested_; }

    // ── Key grabs (zlm_shell_v1 v4) ──
    /// zlm_key_grab_v1.modifier / .flag bits.
    static constexpr unsigned k_mod_shift = 0x1, k_mod_ctrl = 0x2, k_mod_alt = 0x4,
                              k_mod_super = 0x8;
    static constexpr unsigned k_grab_tap = 0x1;
    static constexpr unsigned k_max_key_grabs = 8;
    /// The combination was pressed (or the modifier tapped). `serial` is an input serial
    /// delivered to the shell — what xdg_activation_token_v1.set_serial wants.
    using key_grab_fn = void (*)(unsigned serial, unsigned time_ms, void* user);
    /// The compositor takes key grabs (bound at version 4 or later).
    [[nodiscard]] bool can_grab_keys() const;
    /// zlm_shell_v1.grab_key: the slot of the new grab. Below version 4, not_implemented
    /// (logged once at info: the keybindings are then simply absent).
    [[nodiscard]] lx::result<unsigned> grab_key(unsigned keysym, unsigned modifiers,
                                                unsigned flags, key_grab_fn fn, void* user);
    /// Gives the combination back to clients.
    void ungrab_key(unsigned slot);
    /// zlm_key_grab_v1.refusal for the grab in `slot` (0: held, or not answered yet).
    [[nodiscard]] unsigned key_grab_refusal(unsigned slot) const;
    /// Times the grab in `slot` was activated.
    [[nodiscard]] unsigned key_grab_activations(unsigned slot) const;

    // ── Session lock (zlm_shell_v1 v5) ──
    /// zlm_shell_v1.lock_state (0 unlocked, 1 locking, 2 locked, 3 abandoned) and
    /// lock_requested (0 logind, 1 sleep).
    using lock_state_fn = void (*)(unsigned state, void* user);
    using lock_requested_fn = void (*)(unsigned reason, void* user);
    void set_lock_handlers(lock_state_fn state, lock_requested_fn requested, void* user) {
        lock_state_fn_ = state;
        lock_requested_fn_ = requested;
        lock_user_ = user;
    }
    /// The compositor reports the session lock (bound at version 5 or later).
    [[nodiscard]] bool has_lock_state() const;
    [[nodiscard]] unsigned lock_state() const { return lock_state_; }

    // ── zlm v6: icons, parents, bell, capture state, non-inhibitable grabs ──
    /// zlm_key_grab_v1.flag.not_inhibitable: the grab fires also while a client inhibits
    /// shortcuts (the lock key). Only sent to a compositor of version 6 or later.
    static constexpr unsigned k_grab_not_inhibitable = 0x2;
    /// Bound at version 6 or later.
    [[nodiscard]] bool has_v6() const;
    /// One pixel buffer of a window's icon (premultiplied ARGB8888, `size`², read-only, valid
    /// until the task's icon changes or the task goes).
    struct icon_image {
        const std::uint32_t* pixels = nullptr;
        int size = 0;
        int scale = 1;
    };
    static constexpr unsigned k_icon_images = 4;
    /// The pixel buffers of `h`'s icon (the name is in the task entry); how many were written.
    unsigned icon_images(task_handle h, icon_image* out, unsigned cap) const;
    /// zlm_policy_bridge_v1.bell: `h` is the window (a null handle: no window in
    /// particular).
    using bell_fn = void (*)(task_handle h, void* user);
    void set_bell_handler(bell_fn fn, void* user) {
        bell_fn_ = fn;
        bell_user_ = user;
    }
    /// zlm_shell_v1.capture_state: some client is capturing the screen.
    [[nodiscard]] bool capture_active() const { return capture_active_; }

    /// Wire events, one per protocol event (defined with the listeners).
    struct events;

private:
    friend struct events;
    [[nodiscard]] void* proxy_of(task_handle h) const;

    struct key_grab {
        void* proxy = nullptr; // zlm_key_grab_v1*
        policy_bridge* owner = nullptr;
        key_grab_fn fn = nullptr;
        void* user = nullptr;
        unsigned keysym = 0;
        unsigned refusal = 0;
        unsigned activations = 0;
    };
    void destroy_key_grabs();

    /// The mapped icon memfds of each task slot, and the group being received (between
    /// toplevel_icon and toplevel_icon_done).
    struct icon_maps {
        icon_image images[k_icon_images]{};
        std::size_t lengths[k_icon_images]{};
        unsigned count = 0;
    };
    static void unmap_icons(icon_maps& m);
    icon_maps icons_[k_max_tasks]{};
    icon_maps icon_pending_{};
    task_handle icon_pending_task_{};
    char icon_pending_name_[k_task_icon_name_bytes]{};
    bell_fn bell_fn_ = nullptr;
    void* bell_user_ = nullptr;
    bool capture_active_ = false;

    task_model model_{};
    key_grab grabs_[k_max_key_grabs]{};
    bool told_no_grabs_ = false;
    void* shell_ = nullptr;  // zlm_shell_v1*, not owned
    void* bridge_ = nullptr; // zlm_policy_bridge_v1*
    unsigned caps_ = 0;
    unsigned caps_version_ = 0;
    bool has_caps_ = false;
    bool logout_requested_ = false;
    unsigned lock_state_ = 0;
    lock_state_fn lock_state_fn_ = nullptr;
    lock_requested_fn lock_requested_fn_ = nullptr;
    void* lock_user_ = nullptr;
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

template <typename... Args>
void bridge_log(lx::trace::level lvl, std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lvl, k_bridge_category, fmt, std::forward<Args>(args)...);
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

const char* refusal_name(unsigned r) {
    switch (r) {
    case ZLM_KEY_GRAB_V1_REFUSAL_RESERVED: return "reserved by the compositor";
    case ZLM_KEY_GRAB_V1_REFUSAL_TAKEN: return "already taken";
    case ZLM_KEY_GRAB_V1_REFUSAL_INVALID: return "invalid";
    default: return "unknown";
    }
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
        bridge_log(lx::trace::level::info, "capabilities flags=0x{:x} version={}", flags, version);
    }
    static void session_ending(void* data, zlm_shell_v1*) {
        // Whoever started the logout, the disconnect that follows is the session ending.
        self(data).logout_requested_ = true;
        bridge_log(lx::trace::level::info, "session ending");
    }
    static void lock_state(void* data, zlm_shell_v1*, uint32_t state) {
        auto& b = self(data);
        const bool changed = b.lock_state_ != state;
        b.lock_state_ = state;
        if (changed || bridge_debug()) {
            static constexpr const char* k_names[] = {"unlocked", "locking", "locked",
                                                      "abandoned"};
            bridge_log(lx::trace::level::info, "session lock {}", state < 4 ? k_names[state] : "?");
        }
        if (b.lock_state_fn_)
            b.lock_state_fn_(state, b.lock_user_);
    }
    static void capture_state(void* data, zlm_shell_v1*, uint32_t active) {
        auto& b = self(data);
        if (b.capture_active_ == (active != 0))
            return;
        b.capture_active_ = active != 0;
        bridge_log(lx::trace::level::info, active ? "screen capture active" : "screen capture ended");
    }
    static void lock_requested(void* data, zlm_shell_v1*, uint32_t reason) {
        auto& b = self(data);
        bridge_log(lx::trace::level::info, reason == ZLM_SHELL_V1_LOCK_REASON_SLEEP
                                               ? "lock requested (the system is going to sleep)"
                                               : "lock requested (logind)");
        if (b.lock_requested_fn_)
            b.lock_requested_fn_(reason, b.lock_user_);
    }

    // ── zlm_key_grab_v1 (v4); `data` is the grab's slot ──
    static void key_grab_activated(void* data, zlm_key_grab_v1*, uint32_t serial,
                                   uint32_t time) {
        auto* g = static_cast<key_grab*>(data);
        ++g->activations;
        if (bridge_debug()) {
            bridge_log(lx::trace::level::debug, "key grab activated keysym=0x{:x} serial={}",
                       g->keysym, serial);
        }
        if (g->fn)
            g->fn(serial, time, g->user);
    }
    static void key_grab_refused(void* data, zlm_key_grab_v1*, uint32_t reason) {
        auto* g = static_cast<key_grab*>(data);
        g->refusal = reason;
        bridge_log(lx::trace::level::warn, "key grab keysym=0x{:x} refused: {}", g->keysym,
                   refusal_name(reason));
    }

    // ── zlm_policy_bridge_v1: snapshot ──
    static void snapshot_begin(void* data, zlm_policy_bridge_v1*) {
        auto& b = self(data);
        b.model_.begin_snapshot(destroy_handle, nullptr);
        for (auto& m : b.icons_)
            unmap_icons(m);
        unmap_icons(b.icon_pending_);
        b.icon_pending_task_ = {};
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
            bridge_log(lx::trace::level::debug,
                       "snapshot toplevel handle={} app_id={} title=\"{}\"", th.index,
                       app_id ? app_id : "", title ? title : "");
        }
    }
    static void snapshot_output(void*, zlm_policy_bridge_v1*, wl_output*, const char*, int32_t,
                                int32_t, int32_t) {}
    static void snapshot_done(void* data, zlm_policy_bridge_v1*) {
        auto& b = self(data);
        b.model_.end_snapshot();
        bridge_log(lx::trace::level::info, "snapshot toplevels={}", b.model_.count());
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
            bridge_log(lx::trace::level::debug, "add handle={} count={}", th.index,
                       b.model_.count());
        }
    }
    static void toplevel_destroyed(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        if (b.model_.get(th))
            unmap_icons(b.icons_[th.index]);
        const bool removed = b.model_.remove(th);
        if (bridge_debug()) {
            bridge_log(lx::trace::level::debug, "remove handle={} count={}",
                       removed ? static_cast<int>(th.index) : -1, b.model_.count());
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
            bridge_log(lx::trace::level::debug, "title handle={} app_id={} title=\"{}\"", th.index,
                       e ? e->app_id : "", e ? e->title : "");
        }
        if ((changed & task_change_state) && bridge_debug()) {
            bridge_log(lx::trace::level::debug, "state handle={} state={}", th.index, state);
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
            bridge_log(lx::trace::level::debug, "flags handle={} flags={}", th.index, flags);
        }
    }
    // ── zlm_policy_bridge_v1 v6 ──
    static void toplevel_icon(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                              const char* name) {
        auto& b = self(data);
        unmap_icons(b.icon_pending_);
        b.icon_pending_task_ = find(b, t);
        (void)copy_utf8(b.icon_pending_name_, sizeof(b.icon_pending_name_), name);
    }
    static void toplevel_icon_buffer(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                     int32_t fd, int32_t size, int32_t scale) {
        auto& b = self(data);
        icon_maps& m = b.icon_pending_;
        // Mapped read-only for as long as the icon is shown: the compositor sealed it, so it
        // can neither change nor shrink under the mapping.
        const std::size_t len =
            size > 0 && size <= 1024 ? static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u : 0;
        if (len && m.count < k_icon_images && find(b, t) == b.icon_pending_task_) {
            void* p = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                m.images[m.count] = {static_cast<const std::uint32_t*>(p), size, scale > 0 ? scale : 1};
                m.lengths[m.count] = len;
                ++m.count;
            }
        }
        ::close(fd);
    }
    static void toplevel_icon_done(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        if (!b.model_.get(th) || !(th == b.icon_pending_task_)) {
            unmap_icons(b.icon_pending_);
            b.icon_pending_task_ = {};
            return;
        }
        unmap_icons(b.icons_[th.index]);
        b.icons_[th.index] = b.icon_pending_;
        b.icon_pending_ = {};
        b.icon_pending_task_ = {};
        (void)b.model_.set_icon(th, b.icon_pending_name_);
        bridge_log(lx::trace::level::info, "icon handle={} name={} buffers={}", th.index,
                   b.icon_pending_name_[0] ? b.icon_pending_name_ : "-", b.icons_[th.index].count);
    }
    static void toplevel_parent(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t,
                                zlm_toplevel_v1* parent) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        const task_handle ph = find(b, parent);
        if (b.model_.set_parent(th, ph) && bridge_debug()) {
            bridge_log(lx::trace::level::debug, "parent handle={} parent={}", th.index,
                       ph ? static_cast<int>(ph.index) : -1);
        }
    }
    static void bell(void* data, zlm_policy_bridge_v1*, zlm_toplevel_v1* t) {
        auto& b = self(data);
        const task_handle th = find(b, t);
        {
            bridge_log(lx::trace::level::info, "bell handle={}",
                       th ? static_cast<int>(th.index) : -1);
        }
        if (b.bell_fn_)
            b.bell_fn_(th, b.bell_user_);
    }
};

namespace {

const zlm_key_grab_v1_listener k_key_grab_listener = {
    .activated = lx::shell::policy_bridge::events::key_grab_activated,
    .refused = lx::shell::policy_bridge::events::key_grab_refused,
};

const zlm_shell_v1_listener k_shell_listener = {
    .capabilities = lx::shell::policy_bridge::events::capabilities,
    .session_ending = lx::shell::policy_bridge::events::session_ending,
    .lock_state = lx::shell::policy_bridge::events::lock_state,
    .lock_requested = lx::shell::policy_bridge::events::lock_requested,
    .capture_state = lx::shell::policy_bridge::events::capture_state,
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
    .toplevel_icon = lx::shell::policy_bridge::events::toplevel_icon,
    .toplevel_icon_buffer = lx::shell::policy_bridge::events::toplevel_icon_buffer,
    .toplevel_icon_done = lx::shell::policy_bridge::events::toplevel_icon_done,
    .toplevel_parent = lx::shell::policy_bridge::events::toplevel_parent,
    .bell = lx::shell::policy_bridge::events::bell,
};

} // namespace

lx::result<void> lx::shell::policy_bridge::observe(void* zlm_shell) {
    if (!zlm_shell)
        return lx::invalid_argument("no zlm_shell_v1");
    if (shell_ == zlm_shell)
        return {};
    if (zlm_shell_v1_add_listener(static_cast<zlm_shell_v1*>(zlm_shell), &k_shell_listener,
                                  this) != 0)
        return lx::make_error(lx::wayland_err::bind_failed, "zlm_shell_v1 already has a listener");
    shell_ = zlm_shell;
    return {};
}

lx::result<void> lx::shell::policy_bridge::attach(void* zlm_shell) {
    if (bridge_)
        return lx::invalid_argument("bridge already attached");
    if (auto observed = observe(zlm_shell); !observed)
        return observed;
    auto* shell = static_cast<zlm_shell_v1*>(zlm_shell);
    zlm_policy_bridge_v1* bridge = zlm_shell_v1_get_policy_bridge(shell);
    if (!bridge)
        return lx::make_error(lx::wayland_err::bind_failed,
                              "zlm_shell_v1.get_policy_bridge failed");
    zlm_policy_bridge_v1_add_listener(bridge, &k_bridge_listener, this);
    bridge_ = bridge;
    return {};
}

void lx::shell::policy_bridge::detach() {
    destroy_key_grabs();
    if (!bridge_)
        return;
    task_handle all[k_max_tasks];
    const unsigned n = model_.ordered(all, k_max_tasks);
    for (unsigned i = 0; i < n; ++i)
        if (const task_entry* e = model_.get(all[i]); e && e->proxy)
            zlm_toplevel_v1_destroy(static_cast<zlm_toplevel_v1*>(e->proxy));
    model_.clear(); // the proxies are already gone
    for (auto& m : icons_)
        unmap_icons(m);
    unmap_icons(icon_pending_);
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

bool lx::shell::policy_bridge::can_logout() const {
    return shell_ && wl_proxy_get_version(static_cast<wl_proxy*>(shell_)) >=
                         ZLM_SHELL_V1_REQUEST_LOGOUT_SINCE_VERSION;
}

void lx::shell::policy_bridge::request_logout() {
    if (!can_logout() || logout_requested_)
        return;
    logout_requested_ = true;
    zlm_shell_v1_request_logout(static_cast<zlm_shell_v1*>(shell_));
}

bool lx::shell::policy_bridge::has_lock_state() const {
    return shell_ && wl_proxy_get_version(static_cast<wl_proxy*>(shell_)) >=
                         ZLM_SHELL_V1_LOCK_STATE_SINCE_VERSION;
}

bool lx::shell::policy_bridge::has_v6() const {
    return shell_ && wl_proxy_get_version(static_cast<wl_proxy*>(shell_)) >=
                         ZLM_SHELL_V1_CAPTURE_STATE_SINCE_VERSION;
}

bool lx::shell::policy_bridge::can_grab_keys() const {
    return shell_ && wl_proxy_get_version(static_cast<wl_proxy*>(shell_)) >=
                         ZLM_SHELL_V1_GRAB_KEY_SINCE_VERSION;
}

lx::result<unsigned> lx::shell::policy_bridge::grab_key(unsigned keysym, unsigned modifiers,
                                                        unsigned flags, key_grab_fn fn,
                                                        void* user) {
    if (!can_grab_keys()) {
        if (!told_no_grabs_) {
            told_no_grabs_ = true;
            bridge_log(lx::trace::level::info,
                       "compositor has no key grabs (v{}): no shell keybindings",
                       shell_ ? wl_proxy_get_version(static_cast<wl_proxy*>(shell_)) : 0u);
        }
        return lx::not_implemented("lx::shell::policy_bridge::grab_key");
    }
    for (unsigned i = 0; i < k_max_key_grabs; ++i) {
        key_grab& g = grabs_[i];
        if (g.proxy)
            continue;
        zlm_key_grab_v1* proxy =
            zlm_shell_v1_grab_key(static_cast<zlm_shell_v1*>(shell_), keysym, modifiers, flags);
        if (!proxy)
            return lx::make_error(lx::wayland_err::bind_failed, "zlm_shell_v1.grab_key failed");
        g = {};
        g.proxy = proxy;
        g.owner = this;
        g.fn = fn;
        g.user = user;
        g.keysym = keysym;
        zlm_key_grab_v1_add_listener(proxy, &k_key_grab_listener, &g);
        return i;
    }
    return lx::invalid_argument("key grab table full");
}

void lx::shell::policy_bridge::ungrab_key(unsigned slot) {
    if (slot >= k_max_key_grabs || !grabs_[slot].proxy)
        return;
    zlm_key_grab_v1_destroy(static_cast<zlm_key_grab_v1*>(grabs_[slot].proxy));
    grabs_[slot] = {};
}

unsigned lx::shell::policy_bridge::key_grab_refusal(unsigned slot) const {
    return slot < k_max_key_grabs ? grabs_[slot].refusal : 0u;
}

unsigned lx::shell::policy_bridge::key_grab_activations(unsigned slot) const {
    return slot < k_max_key_grabs ? grabs_[slot].activations : 0u;
}

void lx::shell::policy_bridge::destroy_key_grabs() {
    for (unsigned i = 0; i < k_max_key_grabs; ++i)
        ungrab_key(i);
}

lx::shell::policy_bridge::~policy_bridge() { detach(); }

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::policy_bridge::events {};
bool lx::shell::policy_bridge::has_v6() const { return false; }

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
bool lx::shell::policy_bridge::can_logout() const { return false; }
void lx::shell::policy_bridge::request_logout() {}
bool lx::shell::policy_bridge::can_grab_keys() const { return false; }
bool lx::shell::policy_bridge::has_lock_state() const { return false; }
lx::result<unsigned> lx::shell::policy_bridge::grab_key(unsigned, unsigned, unsigned, key_grab_fn,
                                                        void*) {
    return lx::not_implemented("lx::shell::policy_bridge::grab_key");
}
void lx::shell::policy_bridge::ungrab_key(unsigned) {}
unsigned lx::shell::policy_bridge::key_grab_refusal(unsigned) const { return 0; }
unsigned lx::shell::policy_bridge::key_grab_activations(unsigned) const { return 0; }
void lx::shell::policy_bridge::destroy_key_grabs() {}
lx::shell::policy_bridge::~policy_bridge() = default;

#endif

void lx::shell::policy_bridge::unmap_icons(icon_maps& m) {
#if !defined(_WIN32)
    for (unsigned i = 0; i < m.count; ++i)
        if (m.images[i].pixels)
            ::munmap(const_cast<std::uint32_t*>(m.images[i].pixels), m.lengths[i]);
#endif
    m = {};
}

unsigned lx::shell::policy_bridge::icon_images(task_handle h, icon_image* out, unsigned cap) const {
    if (!model_.get(h) || !out)
        return 0;
    const icon_maps& m = icons_[h.index];
    unsigned n = 0;
    for (unsigned i = 0; i < m.count && n < cap; ++i)
        out[n++] = m.images[i];
    return n;
}
