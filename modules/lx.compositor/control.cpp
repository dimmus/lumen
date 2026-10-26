module;

// io.lumen.Compositor over sd-varlink (the `:control` partition's bodies). Kept out of the
// partition so that no sd-* type, and none of these bodies, reaches the module interface.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// The sd-event loop needs any libsystemd (the user bus attaches to it); the Varlink server
// needs sd-varlink and sd-json, public since systemd 257 (LUMEN_HAS_VARLINK). Without them
// the loop still runs and the socket is not_implemented.
#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-event.h>
#endif
#if defined(LUMEN_HAS_VARLINK)
#include <systemd/sd-json.h>
#include <systemd/sd-varlink-idl.h>
#include <systemd/sd-varlink.h>
#endif

module lx.compositor;

import std;
import lx.foundation;
import lx.health;
import lx.trace;
import :control;

namespace lx::compositor {

namespace {

// The interface. sd-varlink validates every call's parameters against it (a bad call gets
// org.varlink.service.InvalidParameter before any handler runs) and serves
// org.varlink.service.GetInterfaceDescription from it.
constexpr const char* k_idl = R"IDL(# The Lumen compositor's control interface: the running session's state and health, and
# actions on it. Served on $XDG_RUNTIME_DIR/<display>.varlink to the session's own user.
interface io.lumen.Compositor

# One subsystem of the compositor.
type Part (name: string, level: string, implemented: bool, detail: ?string)

# One process of the session (compositor, shell). pid is absent while not running.
type Process (role: string, pid: ?int, connected: bool, binds: int, level: string)

type Memory (
  level: string,
  pressure: string,
  rss_kib: int,
  import_cache_used: int,
  import_cache_capacity: int,
  staging_bytes: int,
  staging_budget: int
)

# Frame pacing of one output. missed_recent counts misses among the last recent_frames
# presented frames (at most 128).
type Speed (
  output: string,
  level: string,
  refresh_mhz: int,
  presented: int,
  missed: int,
  missed_recent: int,
  recent_frames: int,
  last_tick_ui_us: int,
  last_tick_render_us: int,
  ui_over_budget: int,
  render_over_budget: int
)

type Health (
  level: string,
  uptime_ms: int,
  backend: string,
  parts: []Part,
  processes: []Process,
  memory: Memory,
  speed: []Speed,
  config_generation: int,
  config_errors: int,
  config_warnings: int
)

type Output (
  name: string,
  description: string,
  identifier: string,
  enabled: bool,
  x: int,
  y: int,
  width: int,
  height: int,
  mode_width: int,
  mode_height: int,
  refresh_mhz: int,
  scale: float,
  transform: string,
  active_workspace: ?string
)

# index: 1-based position on its output (what workspace.switch N takes).
type Workspace (id: int, output: string, index: int, name: string, active: bool, urgent: bool, windows: int)

type Window (
  id: int,
  app_id: string,
  title: string,
  pid: ?int,
  output: ?string,
  workspace: ?int,
  state: string,
  focused: bool,
  minimized: bool,
  sticky: bool,
  x: int,
  y: int,
  width: int,
  height: int
)

type Diagnostic (severity: string, file: string, line: int, key: string, message: string)

method GetVersion() -> (version: string, zlm_version: int, backend: string)

method GetHealth() -> (health: Health)

method ListOutputs() -> (outputs: []Output)

method ListWorkspaces() -> (workspaces: []Workspace)

method ListWindows() -> (windows: []Window)

# Re-reads the configuration file.
method Reload() -> (generation: int, applied: bool, diagnostics: []Diagnostic)

# Runs a named action (the names key bindings use), e.g. session.logout.
method RunAction(action: string, argument: ?string) -> ()

# Logs out: every window is asked to close, then the compositor stops (5 s at most).
method Logout() -> ()

# Call with 'more': one reply per change. The first reply is the current focus. Events:
# focus, window-opened, window-closed, window-changed, workspace, workspace-removed, health,
# config.
method Subscribe() -> (event: string, window: ?Window, workspace: ?Workspace, level: ?string, generation: ?int)

error UnknownAction (action: string)

error InvalidArgument (action: string, message: string)

error ShellNotConnected (action: string)

error SessionLocked (action: string)

# Subscribe: every subscriber slot is taken.
error TooManySubscribers ()
)IDL";

#if defined(LUMEN_HAS_VARLINK)

constexpr lx::trace::category log_control{"compositor.control"};

// ── JSON (the function API: sd-json's builder macros are C compound literals) ───────────

struct json {
    sd_json_variant* v = nullptr;
    json() = default;
    json(const json&) = delete;
    json& operator=(const json&) = delete;
    ~json() { sd_json_variant_unref(v); }
    void str(const char* k, const char* s) { (void)sd_json_variant_set_field_string(&v, k, s ? s : ""); }
    void num(const char* k, long long i) { (void)sd_json_variant_set_field_integer(&v, k, i); }
    void unum(const char* k, unsigned long long u) {
        (void)sd_json_variant_set_field_integer(
            &v, k, u > 0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFll : static_cast<long long>(u));
    }
    void flag(const char* k, bool b) { (void)sd_json_variant_set_field_boolean(&v, k, b ? 1 : 0); }
    void real(const char* k, double d) {
        sd_json_variant* r = nullptr;
        if (sd_json_variant_new_real(&r, d) >= 0)
            (void)sd_json_variant_set_field(&v, k, r);
        sd_json_variant_unref(r);
    }
    /// Sets `k` to `child` (an object or array); `child` keeps its own reference.
    void set(const char* k, const json& child) {
        if (child.v)
            (void)sd_json_variant_set_field(&v, k, child.v);
    }
    /// `k` = [] when `child` was never appended to.
    void set_array(const char* k, const json& child) {
        if (child.v) {
            (void)sd_json_variant_set_field(&v, k, child.v);
        } else {
            sd_json_variant* empty = nullptr;
            if (sd_json_variant_new_array(&empty, nullptr, 0) >= 0)
                (void)sd_json_variant_set_field(&v, k, empty);
            sd_json_variant_unref(empty);
        }
    }
    void append(const json& element) {
        if (element.v)
            (void)sd_json_variant_append_array(&v, element.v);
    }
    /// An empty object (a reply with no fields).
    void ensure_object() {
        if (!v)
            (void)sd_json_variant_new_object(&v, nullptr, 0);
    }
};

const char* param_string(sd_json_variant* params, const char* key) {
    sd_json_variant* f = params ? sd_json_variant_by_key(params, key) : nullptr;
    return f && sd_json_variant_is_string(f) ? sd_json_variant_string(f) : nullptr;
}

void window_json(json& o, const control_window& w) {
    o.num("id", w.id);
    o.str("app_id", w.app_id);
    o.str("title", w.title);
    if (w.pid > 0)
        o.num("pid", w.pid);
    if (w.output[0])
        o.str("output", w.output);
    if (w.workspace)
        o.num("workspace", w.workspace);
    o.str("state", w.state);
    o.flag("focused", w.focused);
    o.flag("minimized", w.minimized);
    o.flag("sticky", w.sticky);
    o.num("x", w.x);
    o.num("y", w.y);
    o.num("width", w.width);
    o.num("height", w.height);
}

void workspace_json(json& o, const control_workspace& w) {
    o.num("id", w.id);
    o.str("output", w.output);
    o.num("index", w.index);
    o.str("name", w.name);
    o.flag("active", w.active);
    o.flag("urgent", w.urgent);
    o.num("windows", w.windows);
}

void health_json(json& h, const lx::health::health_report& r) {
    h.str("level", lx::health::level_name(r.overall));
    h.unum("uptime_ms", r.uptime_ms);
    h.str("backend", r.backend);
    json parts;
    for (const auto& p : r.parts) {
        json o;
        o.str("name", p.name);
        o.str("level", lx::health::level_name(p.lvl));
        o.flag("implemented", p.implemented);
        if (!p.detail.empty())
            o.str("detail", p.detail.c_str());
        parts.append(o);
    }
    h.set_array("parts", parts);
    json procs;
    for (const auto& p : r.processes) {
        json o;
        o.str("role", p.role);
        if (p.pid > 0)
            o.num("pid", p.pid);
        o.flag("connected", p.connected);
        o.num("binds", p.binds);
        o.str("level", lx::health::level_name(p.lvl));
        procs.append(o);
    }
    h.set_array("processes", procs);
    json mem;
    mem.str("level", lx::health::level_name(r.memory.lvl));
    mem.str("pressure", r.memory.pressure);
    mem.unum("rss_kib", r.memory.rss_kib);
    mem.num("import_cache_used", r.memory.import_cache_used);
    mem.num("import_cache_capacity", r.memory.import_cache_capacity);
    mem.unum("staging_bytes", r.memory.staging_bytes);
    mem.unum("staging_budget", r.memory.staging_budget);
    h.set("memory", mem);
    json speed;
    for (const auto& s : r.speed) {
        json o;
        o.str("output", s.output.c_str());
        o.str("level", lx::health::level_name(s.lvl));
        o.num("refresh_mhz", s.refresh_mhz);
        o.unum("presented", s.presented);
        o.unum("missed", s.missed);
        o.num("missed_recent", s.missed_recent);
        o.num("recent_frames", s.recent_frames);
        o.num("last_tick_ui_us", s.last_tick_ui_us);
        o.num("last_tick_render_us", s.last_tick_render_us);
        o.unum("ui_over_budget", s.ui_over_budget);
        o.unum("render_over_budget", s.render_over_budget);
        speed.append(o);
    }
    h.set_array("speed", speed);
    h.num("config_generation", r.config_generation);
    h.num("config_errors", r.config_errors);
    h.num("config_warnings", r.config_warnings);
}

/// "io.lumen.Compositor.<name>" in a static buffer per name (error ids must outlive the
/// call only; these are literals).
constexpr const char* k_err_unknown_action = "io.lumen.Compositor.UnknownAction";
constexpr const char* k_err_invalid_argument = "io.lumen.Compositor.InvalidArgument";
constexpr const char* k_err_shell = "io.lumen.Compositor.ShellNotConnected";
constexpr const char* k_err_locked = "io.lumen.Compositor.SessionLocked";
constexpr const char* k_err_subscribers = "io.lumen.Compositor.TooManySubscribers";

#endif // LUMEN_HAS_VARLINK

} // namespace

const char* control_interface_idl() { return k_idl; }

#if defined(LUMEN_HAS_LOGIND)

int control_server::fd() const {
    return event_ ? sd_event_get_fd(static_cast<sd_event*>(event_)) : -1;
}

namespace {
/// Runs the sd-event loop until it has nothing to do. Never blocks.
void run_event_loop(void* event) {
    auto* e = static_cast<sd_event*>(event);
    if (!e)
        return;
    // Bounded: each run dispatches one source; a flood of work still yields to the UI loop.
    for (unsigned i = 0; i < 256; ++i) {
        const int r = sd_event_run(e, 0);
        if (r <= 0)
            break;
    }
}
} // namespace

#endif

#if defined(LUMEN_HAS_VARLINK)

struct control_server::impl {
    static control_server* self(sd_varlink* link) {
        return static_cast<control_server*>(sd_varlink_get_userdata(link));
    }

    static int get_version(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        control_version v{};
        if (s && s->host_.version)
            s->host_.version(s->host_.user, v);
        json o;
        o.str("version", v.version);
        o.num("zlm_version", v.zlm_version);
        o.str("backend", v.backend);
        return sd_varlink_reply(link, o.v);
    }

    static int get_health(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        if (!s || !s->host_.health)
            return sd_varlink_error(link, "org.varlink.service.MethodNotImplemented", nullptr);
        unsigned slot = k_max_pending_health;
        for (unsigned i = 0; i < k_max_pending_health; ++i) {
            if (!s->health_[i].link) {
                slot = i;
                break;
            }
        }
        if (slot == k_max_pending_health)
            return sd_varlink_error_errno(link, -EBUSY);
        pending_health& p = s->health_[slot];
        p.link = sd_varlink_ref(link);
        p.generation = s->health_generation_++ & 0xFFFFFFu;
        const unsigned token = slot | (p.generation << 8);
        p.owner = s;
        sd_event_source* timer = nullptr;
        if (sd_event_add_time_relative(static_cast<sd_event*>(s->event_), &timer,
                                       CLOCK_MONOTONIC,
                                       static_cast<std::uint64_t>(health_timeout_ms) * 1000ull,
                                       0, &impl::health_timer, &p) >= 0)
            p.timer = timer;
        s->host_.health(s->host_.user, token);
        return 0;
    }

    static int health_timer(sd_event_source* src, std::uint64_t, void* u) {
        auto* p = static_cast<pending_health*>(u);
        auto* s = p ? static_cast<control_server*>(p->owner) : nullptr;
        if (!s)
            return 0;
        if (p->timer == src) {
            sd_event_source_disable_unref(src);
            p->timer = nullptr;
        }
        const auto slot = static_cast<unsigned>(p - s->health_);
        if (p->link && s->host_.health_timeout)
            s->host_.health_timeout(s->host_.user, slot | (p->generation << 8));
        return 0;
    }

    static int list_outputs(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        control_output outs[16];
        const unsigned n = s && s->host_.outputs ? s->host_.outputs(s->host_.user, outs, 16) : 0;
        json arr;
        for (unsigned i = 0; i < n && i < 16; ++i) {
            const control_output& o = outs[i];
            json j;
            j.str("name", o.name);
            j.str("description", o.description);
            j.str("identifier", o.identifier);
            j.flag("enabled", o.enabled);
            j.num("x", o.x);
            j.num("y", o.y);
            j.num("width", o.width);
            j.num("height", o.height);
            j.num("mode_width", o.mode_width);
            j.num("mode_height", o.mode_height);
            j.num("refresh_mhz", o.refresh_mhz);
            j.real("scale", o.scale);
            j.str("transform", o.transform);
            if (o.active_workspace[0])
                j.str("active_workspace", o.active_workspace);
            arr.append(j);
        }
        json r;
        r.set_array("outputs", arr);
        return sd_varlink_reply(link, r.v);
    }

    static int list_workspaces(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t,
                               void*) {
        control_server* s = self(link);
        // At most 64 (the model's table): on the stack.
        control_workspace ws[64];
        const unsigned n =
            s && s->host_.workspaces ? s->host_.workspaces(s->host_.user, ws, 64) : 0;
        json arr;
        for (unsigned i = 0; i < n && i < 64; ++i) {
            json j;
            workspace_json(j, ws[i]);
            arr.append(j);
        }
        json r;
        r.set_array("workspaces", arr);
        return sd_varlink_reply(link, r.v);
    }

    static int list_windows(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        // A window list is rare and bounded: one heap block per request, never per frame.
        constexpr unsigned k_cap = 256;
        auto windows = std::make_unique<control_window[]>(k_cap);
        const unsigned n =
            s && s->host_.windows ? s->host_.windows(s->host_.user, windows.get(), k_cap) : 0;
        json arr;
        for (unsigned i = 0; i < n && i < k_cap; ++i) {
            json j;
            window_json(j, windows[i]);
            arr.append(j);
        }
        json r;
        r.set_array("windows", arr);
        return sd_varlink_reply(link, r.v);
    }

    static int reload(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        if (!s || !s->host_.reload)
            return sd_varlink_error(link, "org.varlink.service.MethodNotImplemented", nullptr);
        unsigned slot = k_max_pending_reload;
        for (unsigned i = 0; i < k_max_pending_reload; ++i) {
            if (!s->reload_[i].link) {
                slot = i;
                break;
            }
        }
        if (slot == k_max_pending_reload)
            return sd_varlink_error_errno(link, -EBUSY);
        pending_reload& p = s->reload_[slot];
        p.link = sd_varlink_ref(link);
        p.generation = s->reload_generation_++ & 0xFFFFFFu;
        s->host_.reload(s->host_.user, slot | (p.generation << 8));
        return 0;
    }

    static int run_action(sd_varlink* link, sd_json_variant* params, sd_varlink_method_flags_t,
                          void*) {
        control_server* s = self(link);
        const char* action = param_string(params, "action");
        const char* argument = param_string(params, "argument");
        if (!action)
            return sd_varlink_error_invalid_parameter_name(link, "action");
        const char* message = "";
        const control_action_result res =
            s && s->host_.run_action ? s->host_.run_action(s->host_.user, action, argument, message)
                                     : control_action_result::unknown_action;
        json e;
        e.str("action", action);
        switch (res) {
        case control_action_result::ok: {
            json r;
            r.ensure_object();
            return sd_varlink_reply(link, r.v);
        }
        case control_action_result::unknown_action:
            return sd_varlink_error(link, k_err_unknown_action, e.v);
        case control_action_result::invalid_argument:
            e.str("message", message);
            return sd_varlink_error(link, k_err_invalid_argument, e.v);
        case control_action_result::shell_not_connected:
            return sd_varlink_error(link, k_err_shell, e.v);
        case control_action_result::session_locked:
            return sd_varlink_error(link, k_err_locked, e.v);
        }
        return sd_varlink_error(link, k_err_unknown_action, e.v);
    }

    static int logout(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t, void*) {
        control_server* s = self(link);
        // Answered before the logout starts: the reply must not race the shutdown.
        json r;
        r.ensure_object();
        const int ret = sd_varlink_reply(link, r.v);
        if (s && s->host_.logout)
            s->host_.logout(s->host_.user, "lumen-ctl");
        return ret;
    }

    static int subscribe(sd_varlink* link, sd_json_variant*, sd_varlink_method_flags_t flags,
                         void*) {
        control_server* s = self(link);
        if (!(flags & SD_VARLINK_METHOD_MORE))
            return sd_varlink_error(link, "org.varlink.service.ExpectedMore", nullptr);
        subscriber* free_slot = nullptr;
        for (auto& sub : s->subscribers_)
            if (!sub.link) {
                free_slot = &sub;
                break;
            }
        if (!free_slot)
            return sd_varlink_error(link, k_err_subscribers, nullptr);
        free_slot->link = sd_varlink_ref(link);
        free_slot->backlog = 0;
        free_slot->stalled = false;
        // The first event is the current focus: a monitor starts from the present state.
        control_window w{};
        const bool has = s->host_.focused && s->host_.focused(s->host_.user, w);
        json o;
        o.str("event", "focus");
        if (has) {
            json win;
            window_json(win, w);
            o.set("window", win);
        }
        return sd_varlink_notify(link, o.v);
    }

    static void on_disconnect(sd_varlink_server*, sd_varlink* link, void* userdata) {
        auto* s = static_cast<control_server*>(userdata);
        if (!s)
            return;
        for (auto& sub : s->subscribers_) {
            if (sub.link == link) {
                sd_varlink_unref(link);
                sub = {};
            }
        }
        for (auto& p : s->health_) {
            if (p.link == link) {
                if (p.timer)
                    sd_event_source_disable_unref(static_cast<sd_event_source*>(p.timer));
                sd_varlink_unref(link);
                p.link = nullptr;
                p.timer = nullptr;
            }
        }
        for (auto& p : s->reload_) {
            if (p.link == link) {
                sd_varlink_unref(link);
                p.link = nullptr;
            }
        }
    }

    /// Sends one stream event to every subscriber. One whose socket was full at the last
    /// dispatch (it does not read) accumulates backlog and is dropped at the cap. Output
    /// merely queued since then is not backlog: nothing is written until the next dispatch,
    /// so a burst within one UI iteration is no sign of a client that does not read.
    static void broadcast(control_server* s, const json& o) {
        for (auto& sub : s->subscribers_) {
            if (!sub.link)
                continue;
            auto* link = static_cast<sd_varlink*>(sub.link);
            if (sub.stalled)
                ++sub.backlog;
            if (sub.backlog >= k_max_backlog || sd_varlink_notify(link, o.v) < 0) {
                log_control.info("subscriber dropped: not reading");
                sub.link = nullptr;
                sub.backlog = 0;
                sub.stalled = false;
                (void)sd_varlink_close(link);
                sd_varlink_unref(link);
                continue;
            }
        }
        s->kicked_ = true;
    }
};

control_server::~control_server() { close(); }

lx::result<void> control_server::open(control_host host) {
    if (event_)
        return {};
    host_ = host;
    sd_event* e = nullptr;
    if (sd_event_new(&e) < 0 || !e)
        return lx::make_error(lx::io_err::not_found, "sd_event_new failed");
    event_ = e;

    sd_varlink_server* s = nullptr;
    const auto flags = static_cast<sd_varlink_server_flags_t>(
        SD_VARLINK_SERVER_MYSELF_ONLY | SD_VARLINK_SERVER_INHERIT_USERDATA);
    if (sd_varlink_server_new(&s, flags) < 0 || !s) {
        close();
        return lx::make_error(lx::io_err::not_found, "sd_varlink_server_new failed");
    }
    server_ = s;
    (void)sd_varlink_server_set_description(s, "lumen-compositor");
    (void)sd_varlink_server_set_info(s, "Lumen", "lumen-compositor", "0.3.0",
                                     "https://github.com/dimmus/lumen");
    sd_varlink_server_set_userdata(s, this);
    (void)sd_varlink_server_set_connections_max(s, k_max_connections);

    sd_varlink_interface* iface = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    if (sd_varlink_idl_parse(k_idl, &line, &column, &iface) < 0 || !iface) {
        log_control.error("interface definition does not parse at {}:{}", line, column);
        close();
        return lx::make_error(lx::io_err::not_found, "control interface does not parse");
    }
    interface_ = iface;
    int r = sd_varlink_server_add_interface(s, iface);
    if (r >= 0)
        r = sd_varlink_server_bind_method_many(
            s, "io.lumen.Compositor.GetVersion", &impl::get_version,
            "io.lumen.Compositor.GetHealth", &impl::get_health,
            "io.lumen.Compositor.ListOutputs", &impl::list_outputs,
            "io.lumen.Compositor.ListWorkspaces", &impl::list_workspaces,
            "io.lumen.Compositor.ListWindows", &impl::list_windows,
            "io.lumen.Compositor.Reload", &impl::reload,
            "io.lumen.Compositor.RunAction", &impl::run_action,
            "io.lumen.Compositor.Logout", &impl::logout,
            "io.lumen.Compositor.Subscribe", &impl::subscribe);
    if (r >= 0)
        r = sd_varlink_server_bind_disconnect(s, &impl::on_disconnect);
    if (r >= 0)
        r = sd_varlink_server_attach_event(s, e, 0);
    if (r < 0) {
        close();
        return lx::make_error(lx::io_err::not_found, "control server set-up failed");
    }
    return {};
}

lx::result<void> control_server::listen(const char* path) {
    if (!server_ || !path || !*path)
        return lx::invalid_argument("control_server::listen: not open or no path");
    if (std::strlen(path) >= sizeof(path_))
        return lx::invalid_argument("control socket path too long");
    // Ours by the display's lock: a file left by a compositor that died is stale.
    (void)::unlink(path);
    const int r = sd_varlink_server_listen_address(static_cast<sd_varlink_server*>(server_), path,
                                                   0600);
    if (r < 0)
        return lx::make_error(lx::io_err::permission_denied, "cannot listen on the control socket");
    std::snprintf(path_, sizeof(path_), "%s", path);
    kicked_ = true;
    return {};
}

lx::result<void> control_server::add_connection(int fd, const control_peer* peer) {
    if (!server_ || fd < 0)
        return lx::invalid_argument("control_server::add_connection");
    sd_varlink* link = nullptr;
    int r = 0;
    if (peer) {
        ucred cred{};
        cred.uid = static_cast<uid_t>(peer->uid);
        cred.gid = static_cast<gid_t>(peer->gid);
        cred.pid = static_cast<pid_t>(peer->pid);
        r = sd_varlink_server_add_connection_pair(static_cast<sd_varlink_server*>(server_), fd,
                                                  fd, &cred, &link);
    } else {
        r = sd_varlink_server_add_connection(static_cast<sd_varlink_server*>(server_), fd, &link);
    }
    if (r < 0)
        return lx::make_error(lx::io_err::permission_denied, "control connection refused");
    kicked_ = true;
    return {};
}

void control_server::dispatch() {
    kicked_ = false;
    run_event_loop(event_);
    // Whatever a subscriber still has unwritten now could not be written: its socket is
    // full. Until a later dispatch empties it, the events it gets are backlog.
    for (auto& sub : subscribers_) {
        if (!sub.link)
            continue;
        const int events = sd_varlink_get_events(static_cast<sd_varlink*>(sub.link));
        sub.stalled = events >= 0 && (events & EPOLLOUT);
        if (!sub.stalled)
            sub.backlog = 0;
    }
}

void control_server::finish_health(unsigned token, const lx::health::health_report& report) {
    const unsigned slot = token & 0xFFu;
    if (slot >= k_max_pending_health)
        return;
    pending_health& p = health_[slot];
    if (!p.link || p.generation != (token >> 8))
        return;
    if (p.timer)
        sd_event_source_disable_unref(static_cast<sd_event_source*>(p.timer));
    p.timer = nullptr;
    auto* link = static_cast<sd_varlink*>(p.link);
    p.link = nullptr;
    json h;
    health_json(h, report);
    json r;
    r.set("health", h);
    (void)sd_varlink_reply(link, r.v);
    sd_varlink_unref(link);
    kicked_ = true;
}

void control_server::finish_reload(unsigned token, unsigned generation, bool applied,
                                   std::span<const control_diagnostic> diagnostics) {
    const unsigned slot = token & 0xFFu;
    if (slot >= k_max_pending_reload)
        return;
    pending_reload& p = reload_[slot];
    if (!p.link || p.generation != (token >> 8))
        return;
    auto* link = static_cast<sd_varlink*>(p.link);
    p.link = nullptr;
    json r;
    r.num("generation", generation);
    r.flag("applied", applied);
    json arr;
    for (const control_diagnostic& d : diagnostics) {
        json o;
        o.str("severity", d.severity);
        o.str("file", d.file);
        o.num("line", d.line);
        o.str("key", d.key);
        o.str("message", d.message);
        arr.append(o);
    }
    r.set_array("diagnostics", arr);
    (void)sd_varlink_reply(link, r.v);
    sd_varlink_unref(link);
    kicked_ = true;
}

void control_server::publish_focus(const control_window* focused) {
    if (subscribers() == 0)
        return;
    json o;
    o.str("event", "focus");
    if (focused) {
        json w;
        window_json(w, *focused);
        o.set("window", w);
    }
    impl::broadcast(this, o);
}

void control_server::publish_window(const char* event, const control_window& w) {
    if (subscribers() == 0)
        return;
    json o;
    o.str("event", event);
    json win;
    window_json(win, w);
    o.set("window", win);
    impl::broadcast(this, o);
}

void control_server::publish_workspace(const char* event, const control_workspace& w) {
    if (subscribers() == 0)
        return;
    json o;
    o.str("event", event);
    json ws;
    workspace_json(ws, w);
    o.set("workspace", ws);
    impl::broadcast(this, o);
}

void control_server::publish_health(lx::health::level level) {
    if (subscribers() == 0)
        return;
    json o;
    o.str("event", "health");
    o.str("level", lx::health::level_name(level));
    impl::broadcast(this, o);
}

void control_server::publish_config(unsigned generation) {
    if (subscribers() == 0)
        return;
    json o;
    o.str("event", "config");
    o.num("generation", generation);
    impl::broadcast(this, o);
}

unsigned control_server::subscribers() const {
    unsigned n = 0;
    for (const auto& s : subscribers_)
        if (s.link)
            ++n;
    return n;
}

void control_server::close() {
    for (auto& p : reload_) {
        if (p.link)
            sd_varlink_unref(static_cast<sd_varlink*>(p.link));
        p = {};
    }
    for (auto& p : health_) {
        if (p.timer)
            sd_event_source_disable_unref(static_cast<sd_event_source*>(p.timer));
        if (p.link)
            sd_varlink_unref(static_cast<sd_varlink*>(p.link));
        p = {};
    }
    for (auto& sub : subscribers_) {
        if (sub.link)
            sd_varlink_unref(static_cast<sd_varlink*>(sub.link));
        sub = {};
    }
    if (server_) {
        auto* s = static_cast<sd_varlink_server*>(server_);
        (void)sd_varlink_server_shutdown(s);
        (void)sd_varlink_server_detach_event(s);
        sd_varlink_server_unref(s);
        server_ = nullptr;
    }
    if (interface_) {
        sd_varlink_interface_free(static_cast<sd_varlink_interface*>(interface_));
        interface_ = nullptr;
    }
    if (event_) {
        sd_event_unref(static_cast<sd_event*>(event_));
        event_ = nullptr;
    }
    if (path_[0]) {
        (void)::unlink(path_);
        path_[0] = '\0';
    }
    kicked_ = false;
}

#else // !LUMEN_HAS_VARLINK

struct control_server::impl {};

control_server::~control_server() { close(); }
#if defined(LUMEN_HAS_LOGIND)
// libsystemd < 257: the sd-event loop alone, for the user bus (the session environment).
lx::result<void> control_server::open(control_host host) {
    if (event_)
        return {};
    host_ = host;
    sd_event* e = nullptr;
    if (sd_event_new(&e) < 0 || !e)
        return lx::make_error(lx::io_err::not_found, "sd_event_new failed");
    event_ = e;
    return {};
}
void control_server::close() {
    if (event_) {
        sd_event_unref(static_cast<sd_event*>(event_));
        event_ = nullptr;
    }
    kicked_ = false;
}
void control_server::dispatch() {
    kicked_ = false;
    run_event_loop(event_);
}
#else
lx::result<void> control_server::open(control_host host) {
    host_ = host;
    return lx::not_implemented("lx::compositor::control_server::open");
}
int control_server::fd() const { return -1; }
void control_server::dispatch() { kicked_ = false; }
void control_server::close() {}
#endif
lx::result<void> control_server::listen(const char*) {
    return lx::not_implemented("lx::compositor::control_server::listen");
}
lx::result<void> control_server::add_connection(int, const control_peer*) {
    return lx::not_implemented("lx::compositor::control_server::add_connection");
}
void control_server::finish_health(unsigned, const lx::health::health_report&) {}
void control_server::finish_reload(unsigned, unsigned, bool, std::span<const control_diagnostic>) {}
void control_server::publish_focus(const control_window*) {}
void control_server::publish_window(const char*, const control_window&) {}
void control_server::publish_workspace(const char*, const control_workspace&) {}
void control_server::publish_health(lx::health::level) {}
void control_server::publish_config(unsigned) {}
unsigned control_server::subscribers() const { return 0; }

#endif

} // namespace lx::compositor
