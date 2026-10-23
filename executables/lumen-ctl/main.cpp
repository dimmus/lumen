// lumen-ctl: the control tool of a running Lumen session (design-session-and-wm S.8).
//
// It speaks Varlink (interface io.lumen.Compositor) to the compositor's control socket,
// $XDG_RUNTIME_DIR/<display>.varlink, and reads the session's systemd units from the user
// manager. `varlinkctl introspect $LUMEN_SOCKET` shows the same interface.
//
//   lumen-ctl [--socket PATH] [--json] COMMAND [ARGS]
//
//   status      health in one screen, plus the session's units (active state, restarts)
//               exit 0 ok, 1 degraded, 2 critical, 3 not running
//   health      the full health report (every part, process, output)
//   version     compositor version, zlm_shell_v1 version, composite backend
//   outputs     the outputs: name, mode, position, scale
//   windows     the windows: id, app id, title, state, output, geometry
//   workspaces  every output's workspaces (output, position, name, active, windows)
//   reload      re-read the configuration; prints what was wrong (exit 1: rejected)
//   config check [FILE]
//               validate the configuration (or FILE) offline, keymap included
//   config defaults
//               print the documented default configuration
//   action NAME [ARG]
//               run an action by name (the names key bindings use); action --list lists them
//   logout      log out: every window is asked to close, then the session ends
//   monitor     one line per change (focus, windows, health, configuration) until ^C
//
// The socket: --socket, else $LUMEN_SOCKET, else $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY.varlink,
// else $XDG_RUNTIME_DIR/lumen-0.varlink. Only the session's own user may connect.

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

int lumen_ctl_config(int argc, char** argv);
int lumen_ctl_list_actions();

namespace {

bool g_json = false;

void usage(FILE* out) {
    std::fputs(
        "usage: lumen-ctl [--socket PATH] [--json] COMMAND [ARGS]\n"
        "  status            health summary and the session's units (exit 0 ok, 1 degraded,\n"
        "                    2 critical, 3 not running)\n"
        "  health            the full health report\n"
        "  version           versions and the composite backend\n"
        "  outputs           outputs\n"
        "  windows           windows\n"
        "  workspaces        workspaces\n"
        "  reload            re-read the configuration and print its problems\n"
        "  config check [FILE]  validate the configuration offline (no session needed)\n"
        "  config defaults   print the documented default configuration\n"
        "  action NAME [ARG] run an action (lumen-ctl action --list lists them)\n"
        "  logout            log out of the session\n"
        "  monitor           print changes as they happen (^C to stop)\n"
        "The socket: --socket, $LUMEN_SOCKET, or $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY.varlink.\n",
        out);
}

std::string socket_path(const char* given) {
    if (given && *given)
        return given;
    if (const char* s = std::getenv("LUMEN_SOCKET"); s && *s)
        return s;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime && *runtime ? runtime : "/run/user/" + std::to_string(getuid());
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display && *display && display[0] != '/')
        return dir + "/" + display + ".varlink";
    return dir + "/lumen-0.varlink";
}

// ── JSON accessors ───────────────────────────────────────────────────────────

sd_json_variant* field(sd_json_variant* v, const char* key) {
    return v ? sd_json_variant_by_key(v, key) : nullptr;
}
const char* str(sd_json_variant* v, const char* key, const char* fallback = "") {
    sd_json_variant* f = field(v, key);
    return f && sd_json_variant_is_string(f) ? sd_json_variant_string(f) : fallback;
}
long long num(sd_json_variant* v, const char* key, long long fallback = 0) {
    sd_json_variant* f = field(v, key);
    return f && sd_json_variant_is_integer(f) ? static_cast<long long>(sd_json_variant_integer(f))
           : f && sd_json_variant_is_unsigned(f)
               ? static_cast<long long>(sd_json_variant_unsigned(f))
               : fallback;
}
double real(sd_json_variant* v, const char* key, double fallback = 0.0) {
    sd_json_variant* f = field(v, key);
    return f && sd_json_variant_is_number(f) ? sd_json_variant_real(f) : fallback;
}
bool flag(sd_json_variant* v, const char* key) {
    sd_json_variant* f = field(v, key);
    return f && sd_json_variant_is_boolean(f) && sd_json_variant_boolean(f);
}
std::size_t count(sd_json_variant* arr) {
    return arr && sd_json_variant_is_array(arr) ? sd_json_variant_elements(arr) : 0;
}

void dump(sd_json_variant* v) {
    (void)sd_json_variant_dump(v,
                               static_cast<sd_json_format_flags_t>(SD_JSON_FORMAT_PRETTY_AUTO |
                                                                   SD_JSON_FORMAT_COLOR_AUTO |
                                                                   SD_JSON_FORMAT_NEWLINE),
                               stdout, nullptr);
}

// ── Varlink ──────────────────────────────────────────────────────────────────

struct connection {
    sd_varlink* link = nullptr;
    ~connection() { sd_varlink_flush_close_unref(link); }
};

/// Connects; prints why not and returns false.
bool connect(connection& c, const std::string& path, bool quiet = false) {
    const int r = sd_varlink_connect_address(&c.link, path.c_str());
    if (r < 0) {
        if (!quiet)
            std::fprintf(stderr, "lumen-ctl: no compositor on %s (%s)\n", path.c_str(),
                         std::strerror(-r));
        return false;
    }
    (void)sd_varlink_set_description(c.link, "lumen-ctl");
    (void)sd_varlink_set_relative_timeout(c.link, 5'000'000);
    return true;
}

/// Calls `method`: true on success, with the reply's parameters (borrowed from the link;
/// null for a reply without any) in `reply`; false with the error printed.
bool call(connection& c, const char* method, sd_json_variant* params,
          sd_json_variant** reply_out = nullptr) {
    sd_json_variant* reply = nullptr;
    const char* error_id = nullptr;
    char full[128];
    std::snprintf(full, sizeof(full), "io.lumen.Compositor.%s", method);
    const int r = sd_varlink_call(c.link, full, params, &reply, &error_id);
    if (r < 0) {
        std::fprintf(stderr, "lumen-ctl: %s failed: %s\n", method, std::strerror(-r));
        return false;
    }
    if (error_id) {
        const char* msg = str(reply, "message", nullptr);
        std::fprintf(stderr, "lumen-ctl: %s: %s%s%s\n", method, error_id, msg ? ": " : "",
                     msg ? msg : "");
        return false;
    }
    if (reply_out)
        *reply_out = reply;
    return true;
}

/// `call` for methods that answer with data: the parameters, or null on failure.
sd_json_variant* fetch(connection& c, const char* method) {
    sd_json_variant* reply = nullptr;
    return call(c, method, nullptr, &reply) && reply ? reply : nullptr;
}

// ── systemd units (status) ───────────────────────────────────────────────────

struct unit_state {
    bool loaded = false;
    std::string active, sub;
    unsigned restarts = 0;
    unsigned pid = 0;
    bool service = false;
};

unit_state read_unit(sd_bus* bus, const char* name) {
    unit_state u{};
    char* path = nullptr;
    if (sd_bus_path_encode("/org/freedesktop/systemd1/unit", name, &path) < 0)
        return u;
    char* load = nullptr;
    if (sd_bus_get_property_string(bus, "org.freedesktop.systemd1", path,
                                   "org.freedesktop.systemd1.Unit", "LoadState", nullptr,
                                   &load) >= 0 &&
        load) {
        u.loaded = std::strcmp(load, "loaded") == 0;
    }
    std::free(load);
    if (u.loaded) {
        char* s = nullptr;
        if (sd_bus_get_property_string(bus, "org.freedesktop.systemd1", path,
                                       "org.freedesktop.systemd1.Unit", "ActiveState", nullptr,
                                       &s) >= 0 &&
            s)
            u.active = s;
        std::free(s);
        s = nullptr;
        if (sd_bus_get_property_string(bus, "org.freedesktop.systemd1", path,
                                       "org.freedesktop.systemd1.Unit", "SubState", nullptr,
                                       &s) >= 0 &&
            s)
            u.sub = s;
        std::free(s);
        const std::size_t n = std::strlen(name);
        u.service = n > 8 && std::strcmp(name + n - 8, ".service") == 0;
        if (u.service) {
            std::uint32_t v = 0;
            if (sd_bus_get_property_trivial(bus, "org.freedesktop.systemd1", path,
                                            "org.freedesktop.systemd1.Service", "NRestarts",
                                            nullptr, 'u', &v) >= 0)
                u.restarts = v;
            v = 0;
            if (sd_bus_get_property_trivial(bus, "org.freedesktop.systemd1", path,
                                            "org.freedesktop.systemd1.Service", "MainPID",
                                            nullptr, 'u', &v) >= 0)
                u.pid = v;
        }
    }
    std::free(path);
    return u;
}

constexpr const char* k_units[] = {
    "lumen-compositor.service", "lumen-shell.service",          "graphical-session.target",
    "xdg-desktop-autostart.target", "xdg-desktop-portal.service",
    "xdg-desktop-portal-lumen.service", "xdg-desktop-portal-wlr.service",
    "xdg-desktop-portal-gtk.service",
};

/// The units as a JSON object (for --json), or printed.
void units(sd_json_variant** json_out) {
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0 || !bus)
        return;
    for (const char* name : k_units) {
        const unit_state u = read_unit(bus, name);
        if (!u.loaded)
            continue;
        if (json_out) {
            sd_json_variant* o = nullptr;
            (void)sd_json_variant_set_field_string(&o, "active", u.active.c_str());
            (void)sd_json_variant_set_field_string(&o, "sub", u.sub.c_str());
            if (u.service) {
                (void)sd_json_variant_set_field_unsigned(&o, "restarts", u.restarts);
                (void)sd_json_variant_set_field_unsigned(&o, "pid", u.pid);
            }
            (void)sd_json_variant_set_field(json_out, name, o);
            sd_json_variant_unref(o);
        } else {
            char extra[64] = "";
            if (u.service && u.pid)
                std::snprintf(extra, sizeof(extra), " pid %u", u.pid);
            if (u.service && u.restarts)
                std::snprintf(extra + std::strlen(extra), sizeof(extra) - std::strlen(extra),
                              ", restarted %u×", u.restarts);
            std::printf("  %-34s %s (%s)%s\n", name, u.active.c_str(), u.sub.c_str(), extra);
        }
    }
    sd_bus_flush_close_unref(bus);
}

// ── Commands ─────────────────────────────────────────────────────────────────

int level_exit(const char* level) {
    if (std::strcmp(level, "ok") == 0)
        return 0;
    if (std::strcmp(level, "degraded") == 0)
        return 1;
    return 2;
}

std::string duration(long long ms) {
    long long s = ms / 1000;
    char buf[64];
    if (s >= 3600)
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", s / 3600, (s / 60) % 60);
    else if (s >= 60)
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
    else
        std::snprintf(buf, sizeof(buf), "%llds", s);
    return buf;
}

void print_health(sd_json_variant* h, bool full) {
    sd_json_variant* parts = field(h, "parts");
    for (std::size_t i = 0; i < count(parts); ++i) {
        sd_json_variant* p = sd_json_variant_by_index(parts, i);
        const char* level = str(p, "level");
        if (!full && std::strcmp(level, "ok") == 0)
            continue;
        std::printf("  %-13s %-9s %s\n", str(p, "name"), level, str(p, "detail"));
    }
    sd_json_variant* procs = field(h, "processes");
    for (std::size_t i = 0; i < count(procs); ++i) {
        sd_json_variant* p = sd_json_variant_by_index(procs, i);
        const long long pid = num(p, "pid", 0);
        char pidbuf[32] = "not running";
        if (pid > 0)
            std::snprintf(pidbuf, sizeof(pidbuf), "pid %lld", pid);
        std::printf("  %-13s %-9s %s, bound %lld×\n", str(p, "role"), str(p, "level"), pidbuf,
                    num(p, "binds"));
    }
    sd_json_variant* m = field(h, "memory");
    std::printf("  %-13s %-9s %lld MiB resident, pressure %s, staging %lld KiB",
                "memory", str(m, "level"), num(m, "rss_kib") / 1024, str(m, "pressure"),
                num(m, "staging_bytes") / 1024);
    if (num(m, "import_cache_capacity") > 0)
        std::printf(", import cache %lld/%lld", num(m, "import_cache_used"),
                    num(m, "import_cache_capacity"));
    std::printf("\n");
    sd_json_variant* speed = field(h, "speed");
    for (std::size_t i = 0; i < count(speed); ++i) {
        sd_json_variant* s = sd_json_variant_by_index(speed, i);
        std::printf("  %-13s %-9s %.2f Hz, %lld presented, %lld missed (%lld of the last %lld)",
                    str(s, "output"), str(s, "level"),
                    static_cast<double>(num(s, "refresh_mhz")) / 1000.0, num(s, "presented"),
                    num(s, "missed"), num(s, "missed_recent"), num(s, "recent_frames"));
        if (full)
            std::printf("; tick ui %lld us, render %lld us; over budget ui %lld, render %lld",
                        num(s, "last_tick_ui_us"), num(s, "last_tick_render_us"),
                        num(s, "ui_over_budget"), num(s, "render_over_budget"));
        std::printf("\n");
    }
}

int cmd_status(connection& c, bool full) {
    sd_json_variant* v = fetch(c, "GetVersion");
    if (!v)
        return 3;
    std::string version = str(v, "version");
    std::string backend = str(v, "backend");
    long long zlm = num(v, "zlm_version");
    sd_json_variant* r = fetch(c, "GetHealth");
    if (!r)
        return 3;
    sd_json_variant* h = field(r, "health");
    const char* level = str(h, "level", "unknown");
    if (g_json) {
        sd_json_variant* out = nullptr;
        (void)sd_json_variant_set_field_string(&out, "version", version.c_str());
        (void)sd_json_variant_set_field_integer(&out, "zlm_version", zlm);
        (void)sd_json_variant_set_field(&out, "health", h);
        sd_json_variant* u = nullptr;
        if (!full)
            units(&u);
        if (u)
            (void)sd_json_variant_set_field(&out, "units", u);
        dump(out);
        sd_json_variant_unref(u);
        sd_json_variant_unref(out);
        return level_exit(level);
    }
    std::printf("Lumen %s (zlm v%lld, %s composite), up %s: %s\n", version.c_str(), zlm,
                backend.c_str(), duration(num(h, "uptime_ms")).c_str(), level);
    print_health(h, full);
    std::printf("configuration: generation %lld, %lld error(s), %lld warning(s)%s\n",
                num(h, "config_generation"), num(h, "config_errors"), num(h, "config_warnings"),
                num(h, "config_errors") + num(h, "config_warnings") > 0
                    ? " (lumen-ctl reload lists them)"
                    : "");
    if (!full) {
        std::printf("units:\n");
        units(nullptr);
    }
    return level_exit(level);
}

int cmd_list(connection& c, const char* method, const char* key) {
    sd_json_variant* r = fetch(c, method);
    if (!r)
        return 1;
    sd_json_variant* arr = field(r, key);
    if (g_json) {
        dump(arr);
        return 0;
    }
    const std::size_t n = count(arr);
    for (std::size_t i = 0; i < n; ++i) {
        sd_json_variant* e = sd_json_variant_by_index(arr, i);
        if (std::strcmp(key, "outputs") == 0) {
            std::printf("%s  %lldx%lld@%.3f Hz  at %lld,%lld  logical %lldx%lld  scale %g  %s"
                        "  workspace %s\n"
                        "    %s\n",
                        str(e, "name"), num(e, "mode_width"), num(e, "mode_height"),
                        static_cast<double>(num(e, "refresh_mhz")) / 1000.0, num(e, "x"),
                        num(e, "y"), num(e, "width"), num(e, "height"), real(e, "scale", 1.0),
                        str(e, "transform"), str(e, "active_workspace", "-"),
                        str(e, "description"));
        } else if (std::strcmp(key, "windows") == 0) {
            // The workspace by its id (`lumen-ctl workspaces` names it); sticky windows have
            // none.
            char ws[24];
            if (flag(e, "sticky"))
                std::snprintf(ws, sizeof(ws), "sticky");
            else if (const long long id = num(e, "workspace"); id > 0)
                std::snprintf(ws, sizeof(ws), "ws %lld", id);
            else
                std::snprintf(ws, sizeof(ws), "-");
            std::printf("%4lld %c %-24s %-10s %-10s %-7s %lldx%lld+%lld+%lld  %s\n", num(e, "id"),
                        flag(e, "focused") ? '*' : ' ', str(e, "app_id"),
                        flag(e, "minimized") ? "minimized" : str(e, "state"),
                        str(e, "output", "-"), ws, num(e, "width"), num(e, "height"), num(e, "x"),
                        num(e, "y"), str(e, "title"));
        } else {
            const long long w = num(e, "windows");
            std::printf("%-12s %2lld  %-12s %-7s %lld window%s  (id %lld)\n",
                        *str(e, "output") ? str(e, "output") : "(no output)", num(e, "index"),
                        str(e, "name"),
                        flag(e, "active") ? "active" : flag(e, "urgent") ? "urgent" : "", w,
                        w == 1 ? "" : "s", num(e, "id"));
        }
    }
    if (n == 0 && std::strcmp(key, "workspaces") == 0)
        std::printf("no workspaces\n");
    return 0;
}

volatile std::sig_atomic_t g_stop = 0;

int on_event(sd_varlink*, sd_json_variant* params, const char* error_id, sd_varlink_reply_flags_t,
             void*) {
    if (error_id) {
        std::fprintf(stderr, "lumen-ctl: monitor: %s\n", error_id);
        g_stop = 1;
        return 0;
    }
    if (g_json) {
        (void)sd_json_variant_dump(params,
                                   static_cast<sd_json_format_flags_t>(SD_JSON_FORMAT_NEWLINE |
                                                                       SD_JSON_FORMAT_FLUSH),
                                   stdout, nullptr);
        return 0;
    }
    const char* event = str(params, "event");
    sd_json_variant* w = field(params, "window");
    if (std::strcmp(event, "health") == 0) {
        std::printf("health %s\n", str(params, "level"));
    } else if (std::strcmp(event, "config") == 0) {
        std::printf("config generation %lld\n", num(params, "generation"));
    } else if (sd_json_variant* ws = field(params, "workspace")) {
        std::printf("%s %lld %s %s%s%s %lld window(s)\n", event, num(ws, "id"), str(ws, "output"),
                    str(ws, "name"), flag(ws, "active") ? " active" : "",
                    flag(ws, "urgent") ? " urgent" : "", num(ws, "windows"));
    } else if (w) {
        std::printf("%s %lld %s \"%s\"\n", event, num(w, "id"), str(w, "app_id"), str(w, "title"));
    } else {
        std::printf("%s none\n", event);
    }
    std::fflush(stdout);
    return 0;
}

int cmd_monitor(connection& c) {
    (void)sd_varlink_set_relative_timeout(c.link, UINT64_MAX);
    if (sd_varlink_bind_reply(c.link, on_event) < 0 ||
        sd_varlink_observe(c.link, "io.lumen.Compositor.Subscribe", nullptr) < 0) {
        std::fprintf(stderr, "lumen-ctl: monitor: cannot subscribe\n");
        return 1;
    }
    struct sigaction sa {};
    sa.sa_handler = [](int) { g_stop = 1; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    while (!g_stop) {
        const int r = sd_varlink_process(c.link);
        if (r < 0) {
            if (r != -ENOTCONN && r != -ECONNRESET)
                std::fprintf(stderr, "lumen-ctl: monitor: %s\n", std::strerror(-r));
            return r == -ENOTCONN || r == -ECONNRESET ? 0 : 1;
        }
        if (r > 0)
            continue;
        if (sd_varlink_is_connected(c.link) <= 0)
            return 0;
        const int w = sd_varlink_wait(c.link, UINT64_MAX);
        if (w < 0 && w != -EINTR)
            return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const char* given = nullptr;
    int i = 1;
    for (; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            given = argv[++i];
        } else if (std::strncmp(argv[i], "--socket=", 9) == 0) {
            given = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--json") == 0 || std::strcmp(argv[i], "-j") == 0) {
            g_json = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    const std::string cmd = argv[i++];
    for (int k = i; k < argc; ++k)
        if (std::strcmp(argv[k], "--json") == 0 || std::strcmp(argv[k], "-j") == 0)
            g_json = true;

    // Offline: no session needed.
    if (cmd == "config")
        return lumen_ctl_config(argc - i, argv + i);
    if (cmd == "action" && i < argc &&
        (std::strcmp(argv[i], "--list") == 0 || std::strcmp(argv[i], "-l") == 0))
        return lumen_ctl_list_actions();

    const std::string path = socket_path(given);
    connection c;
    if (!connect(c, path, false))
        return cmd == "status" || cmd == "health" ? 3 : 1;

    if (cmd == "status")
        return cmd_status(c, false);
    if (cmd == "health")
        return cmd_status(c, true);
    if (cmd == "version") {
        sd_json_variant* v = fetch(c, "GetVersion");
        if (!v)
            return 1;
        if (g_json)
            dump(v);
        else
            std::printf("lumen-compositor %s, zlm_shell_v1 %lld, %s composite\n", str(v, "version"),
                        num(v, "zlm_version"), str(v, "backend"));
        return 0;
    }
    if (cmd == "outputs")
        return cmd_list(c, "ListOutputs", "outputs");
    if (cmd == "windows")
        return cmd_list(c, "ListWindows", "windows");
    if (cmd == "workspaces")
        return cmd_list(c, "ListWorkspaces", "workspaces");
    if (cmd == "reload") {
        sd_json_variant* r = fetch(c, "Reload");
        if (!r)
            return 1;
        if (g_json) {
            dump(r);
        } else {
            sd_json_variant* diags = field(r, "diagnostics");
            for (std::size_t k = 0; k < count(diags); ++k) {
                sd_json_variant* d = sd_json_variant_by_index(diags, k);
                const char* file = str(d, "file");
                const long long line = num(d, "line");
                const char* key = str(d, "key");
                std::string where = *file ? file : "lumen";
                if (line > 0)
                    where += ":" + std::to_string(line);
                std::printf("%s: %s: %s%s%s\n", str(d, "severity"), where.c_str(), key,
                            *key ? ": " : "", str(d, "message"));
            }
            std::printf("configuration generation %lld%s\n", num(r, "generation"),
                        flag(r, "applied") ? ""
                                           : " rejected: the configuration in use stays");
        }
        return flag(r, "applied") ? 0 : 1;
    }
    if (cmd == "action") {
        if (i >= argc || argv[i][0] == '-') {
            std::fprintf(stderr, "lumen-ctl: action NAME [ARG]\n");
            return 2;
        }
        sd_json_variant* params = nullptr;
        (void)sd_json_variant_set_field_string(&params, "action", argv[i]);
        if (i + 1 < argc && argv[i + 1][0] != '-')
            (void)sd_json_variant_set_field_string(&params, "argument", argv[i + 1]);
        const bool ok = call(c, "RunAction", params);
        sd_json_variant_unref(params);
        return ok ? 0 : 1;
    }
    if (cmd == "logout") {
        return call(c, "Logout", nullptr) ? 0 : 1;
    }
    if (cmd == "monitor")
        return cmd_monitor(c);
    std::fprintf(stderr, "lumen-ctl: unknown command '%s'\n", cmd.c_str());
    usage(stderr);
    return 2;
}
