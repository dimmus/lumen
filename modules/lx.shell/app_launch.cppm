module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <systemd/sd-journal.h>
#endif

import lx.foundation;
import lx.trace;

export module lx.shell:app_launch;

import :desktop_entry;
import :app_catalog;

export namespace lx::shell {

// ── Launching applications, detached from the shell ──────────────────────────
//
// Primary path: each launch is its own systemd user service,
// app-lumen-<escaped id>@<random>.service in app.slice, started with
// org.freedesktop.systemd1.Manager.StartTransientUnit (systemd.io/DESKTOP_ENVIRONMENTS,
// as KDE's KIO does). systemd spawns the process, so it inherits nothing from the shell —
// no fds, no signal mask, no cgroup — and it is resource-tracked and outlives the shell.
// Type=exec turns a failed execve into a failed start job, reported through JobRemoved.
//
// A service needs ExitType=cgroup (systemd 250): without it the unit ends — and its
// cgroup is killed — when the first process exits, which an app that forks and exits does.
// Against an older manager (Manager.Version, read once when the bus is opened) each launch
// is instead the double fork below, held before execve until the process has been moved
// into its own transient scope, app-lumen-<escaped id>-<random>.scope (as KIO does below
// 250, and GNOME Shell always — but without their window in which the app runs, and can
// fork, outside its scope; the hold ends after at most `k_scope_hold_us` regardless).
//
// Fallback when there is no systemd --user (or LUMEN_SHELL_LAUNCH=fork): double fork +
// setsid, an empty signal mask, default dispositions, stdin from /dev/null, stdout and
// stderr to the journal, every other fd closed on exec. The app then stays in the shell's
// cgroup, which is why it is only the fallback.

enum class launch_failure : unsigned char {
    none,
    /// argv[0] (or the terminal) is not an executable in PATH.
    not_installed,
    /// Terminal=true and no terminal was found.
    no_terminal,
    /// The Exec line cannot be expanded.
    invalid_exec,
    /// The systemd start job did not end in "done" (see the journal).
    start_failed,
    /// The fallback's execve failed (`error_number`).
    exec_error,
    /// StartTransientUnit itself failed (`detail` = the D-Bus error name).
    systemd_error,
    /// fork or pipe failed.
    spawn_error,
};
/// The reason in a notice: "not installed", "no terminal found", ….
[[nodiscard]] const char* launch_failure_text(launch_failure f);

/// The environment a launched app gets (§B.5): the shell's, minus DISPLAY, LUMEN_*,
/// inherited activation tokens and per-service systemd variables; plus WAYLAND_DISPLAY,
/// XDG_SESSION_TYPE=wayland, XDG_CURRENT_DESKTOP (kept, or Lumen) and the token as
/// XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID.
struct launch_env {
    std::vector<std::string> vars;
    std::vector<const char*> envp;

    /// The value of `name`, or null.
    [[nodiscard]] const char* get(std::string_view name) const;
};
struct launch_env_context {
    std::string_view wayland_display;
    std::string_view token;
};
void build_launch_env(const char* const* environ, const launch_env_context& ctx,
                      launch_env& out);

/// `id` escaped for a unit name: every byte outside [A-Za-z0-9:_.] becomes \xNN ('-'
/// included: it separates the name's parts). Returns the length written (NUL-terminated,
/// cut at a whole escape so it fits `cap`).
[[nodiscard]] std::size_t escape_unit_id(std::string_view id, char* out, std::size_t cap);
/// app-lumen-<escaped id>@<32 lowercase hex>.service, at most 255 bytes.
[[nodiscard]] std::size_t make_unit_name(std::string_view id, const unsigned char (&rnd)[16],
                                         char (&out)[256]);
/// app-lumen-<escaped id>-<32 lowercase hex>.scope, at most 255 bytes.
[[nodiscard]] std::size_t make_scope_name(std::string_view id, const unsigned char (&rnd)[16],
                                          char (&out)[256]);

/// How a launch is handed to systemd --user.
enum class systemd_launch_mode : unsigned char {
    /// A transient service: Type=exec, ExitType=cgroup (systemd ≥ 250).
    service,
    /// A double fork moved into a transient scope (any systemd).
    scope,
};
/// The mode for a Manager.Version string ("262-1-arch", "249.11-0ubuntu3"): a service
/// from 250 on, a scope below it or when the version cannot be read.
[[nodiscard]] systemd_launch_mode launch_mode_for_version(std::string_view version);

/// What a StartTransientUnit error reply means for the launch.
enum class start_error_class : unsigned char {
    /// There is no systemd --user: this launch and every later one by double fork.
    no_systemd,
    /// NoReply: the call timed out, but systemd may still start the unit. Not retried
    /// (the app would run twice); JobRemoved may yet say how it ended.
    unknown_outcome,
    /// PropertyReadOnly ("Cannot set property …, or unknown property"): a manager older
    /// than its Version said. Nothing was created; retried as a scope.
    too_old,
    /// A real failure, reported.
    failed,
};
[[nodiscard]] start_error_class classify_start_error(std::string_view dbus_error_name);

/// Everything one launch needs, resolved before any D-Bus or fork work.
struct prepared_launch {
    exec_argv argv{};
    /// Absolute path of argv[0] (ExecStart's path; execve's).
    char program[PATH_MAX]{};
    /// Unit and log id: the desktop ID without ".desktop", or basename(argv[0]).
    char id[128]{};
    /// Shown in notices and the unit's Description.
    char name[128]{};
    /// The desktop file ("" for a raw command): the unit's SourcePath.
    char source_path[PATH_MAX]{};
    char workdir[PATH_MAX]{};
};

/// Pre-launch checks and composition for an entry: Exec expanded, argv[0] resolved in
/// `path_env`, Terminal=true wrapped in the catalog's terminal, Path checked (else
/// `home`). `none` when it can be started.
[[nodiscard]] launch_failure prepare_entry_launch(const desktop_entry& e, const app_catalog& cat,
                                                  std::string_view home, prepared_launch& out);
/// The catalog's terminal on its own (Super+Return).
[[nodiscard]] launch_failure prepare_terminal_launch(const app_catalog& cat, std::string_view home,
                                                     prepared_launch& out);
/// terminal argv + [flag] + app argv. False when it does not fit.
[[nodiscard]] bool compose_terminal_argv(const exec_argv& terminal, std::string_view flag,
                                         const exec_argv& app, exec_argv& out);

struct launch_outcome {
    launch_failure failure = launch_failure::none;
    int error_number = 0;
    char id[128]{};
    char name[128]{};
    /// The unit, for the systemd path.
    char unit[256]{};
    /// The job result or D-Bus error name.
    char detail[128]{};
    /// The process, for the fallback path (0 unknown).
    int pid = 0;
    bool forked = false;
};

/// `automatic`: by systemd --user's version (service or scope), double fork without it.
/// `scope` / `fork` force one path (LUMEN_SHELL_LAUNCH=scope|fork).
enum class launch_method : unsigned char { automatic, scope, fork };

/// Starts prepared launches; outcomes arrive through the handler, from `dispatch` (or
/// from `start` itself for a synchronous failure). Single-threaded (the shell's thread).
class app_launcher {
public:
    using outcome_fn = void (*)(const launch_outcome& outcome, void* user);
    static constexpr unsigned k_max_pending = 16;
    static constexpr unsigned k_max_poll = 1 + k_max_pending;
    /// Scope path: the longest a launched process waits before execve for systemd to move
    /// it into its scope. Past it the app starts anyway (moved whenever systemd gets to it).
    static constexpr std::uint64_t k_scope_hold_us = 1'000'000;
    /// After a NoReply, how much longer JobRemoved is waited for before the launch is
    /// reported as failed.
    static constexpr std::uint64_t k_no_reply_grace_us = 30'000'000;

    app_launcher() = default;
    ~app_launcher();
    app_launcher(const app_launcher&) = delete;
    app_launcher& operator=(const app_launcher&) = delete;

    void set_outcome_handler(outcome_fn fn, void* user) {
        fn_ = fn;
        user_ = user;
    }
    void set_method(launch_method m) { method_ = m; }
    /// From LUMEN_SHELL_LAUNCH ("fork" forces the fallback, "scope" the scope path).
    void configure_from_env();

    void start(const prepared_launch& p, std::string_view token, std::string_view wayland_display);

    /// The fds to poll: the user bus (once opened) and pending fallback pipes. `timeout_ms`
    /// is lowered to the next deadline — the bus's, a scope hold's, a NoReply grace's (it
    /// is left alone otherwise).
    [[nodiscard]] unsigned poll_fds(pollfd* out, unsigned cap, int& timeout_ms) const;
    /// After poll: processes the bus and the pipes with events.
    void dispatch(const pollfd* fds, unsigned n);
    [[nodiscard]] bool systemd_available() const { return !no_systemd_; }
    /// The path systemd launches take (known once the bus has been opened).
    [[nodiscard]] systemd_launch_mode systemd_mode() const { return mode_; }
    /// Launches whose outcome is not known yet.
    [[nodiscard]] unsigned pending() const;

    struct events;

private:
    friend struct events;
    struct pending_pipe;
    struct pending_unit {
        app_launcher* owner = nullptr;
        bool used = false;
        bool replied = false;
        char unit[256]{};
        /// A service launch: what to fork if systemd turns out to be absent or too old.
        std::unique_ptr<prepared_launch> launch;
        std::string token;
        std::string display;
        void* slot = nullptr; // sd_bus_slot* of the call
        /// A scope registration: the held process's pipe (its outcome is reported there).
        pending_pipe* scope_for = nullptr;
        /// CLOCK_MONOTONIC µs after which a NoReply'd start is given up (0: none).
        std::uint64_t deadline_us = 0;
    };
    struct pending_pipe {
        int fd = -1;
        unsigned got = 0;
        /// Two 8-byte records {tag, value}, each one atomic write: the middle child's
        /// {1, pid} (or {3, errno} when its fork failed), and the grandchild's {2, errno}
        /// if execve failed. Either may come first.
        unsigned char buf[16]{};
        launch_outcome outcome{};
        /// Scope path: the write end the grandchild waits on before execve; closing it
        /// releases the process. -1 when not held.
        int hold_fd = -1;
        bool scope_sent = false;
        /// CLOCK_MONOTONIC µs at which the hold ends even without systemd's answer.
        std::uint64_t release_at_us = 0;
    };

    [[nodiscard]] bool open_bus();
    [[nodiscard]] bool start_unit(const prepared_launch& p, std::string_view token,
                                  std::string_view display);
    /// `hold`: the scope path (the process waits before execve; see `register_scope`).
    void start_fork(const prepared_launch& p, std::string_view token, std::string_view display,
                    bool hold);
    /// Scope path, once the pipe has the pid: StartTransientUnit of its scope.
    void register_scope(pending_pipe& pp, int pid);
    static void release_hold(pending_pipe& pp);
    /// Ends scope holds and NoReply graces that are due.
    void run_deadlines();
    void systemd_gone(const char* why);
    void report(const launch_outcome& o);
    void finish_pipe(pending_pipe& pp);

    outcome_fn fn_ = nullptr;
    void* user_ = nullptr;
    launch_method method_ = launch_method::automatic;
    void* bus_ = nullptr;       // sd_bus*
    void* match_slot_ = nullptr; // sd_bus_slot*
    void* sub_slot_ = nullptr;
    bool no_systemd_ = false;
    bool told_fallback_ = false;
    systemd_launch_mode mode_ = systemd_launch_mode::service;
    pending_unit units_[k_max_pending]{};
    pending_pipe pipes_[k_max_pending]{};
};

} // namespace lx::shell

// ── Implementation ───────────────────────────────────────────────────────────

extern "C" char** environ;

namespace {

constexpr const char* k_launch_category = "shell.apps";

void launch_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_launch_category, line);
}

[[nodiscard]] bool launch_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] std::string_view name_of(std::string_view kv) {
    const std::size_t eq = kv.find('=');
    return eq == std::string_view::npos ? kv : kv.substr(0, eq);
}

/// Variables a launched app must not inherit from the shell.
[[nodiscard]] bool dropped(std::string_view name) {
    static constexpr std::string_view k_exact[] = {
        "DISPLAY",
        "XDG_ACTIVATION_TOKEN",
        "DESKTOP_STARTUP_ID",
        // Per-service: they describe the shell's own unit, not the app's.
        "INVOCATION_ID",
        "JOURNAL_STREAM",
        "NOTIFY_SOCKET",
        "LISTEN_PID",
        "LISTEN_FDS",
        "LISTEN_FDNAMES",
        "WATCHDOG_PID",
        "WATCHDOG_USEC",
        "SYSTEMD_EXEC_PID",
        "MANAGERPID",
        "MEMORY_PRESSURE_WATCH",
        "MEMORY_PRESSURE_WRITE",
        // The shell process's own shell state.
        "PWD",
        "OLDPWD",
        "SHLVL",
        "_",
        // Set below.
        "WAYLAND_DISPLAY",
        "XDG_SESSION_TYPE",
    };
    for (std::string_view e : k_exact)
        if (name == e)
            return true;
    return name.starts_with("LUMEN_") || name.starts_with("TRIGGER_");
}

void copy_str(char* out, std::size_t cap, std::string_view s) {
    const std::size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
}

[[nodiscard]] bool is_dir(const char* p) {
    struct stat st{};
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void set_workdir(std::string_view path, std::string_view home, char (&out)[PATH_MAX]) {
    if (!path.empty() && path.size() < PATH_MAX) {
        copy_str(out, PATH_MAX, path);
        if (is_dir(out))
            return;
        if (launch_debug()) {
            char line[PATH_MAX + 64];
            std::snprintf(line, sizeof(line), "apps: Path %s is not a directory: using $HOME", out);
            launch_log(lx::trace::level::debug, line);
        }
    }
    copy_str(out, PATH_MAX, home.empty() ? std::string_view{"/"} : home);
}

void id_of(std::string_view desktop_id, char (&out)[128]) {
    if (desktop_id.ends_with(".desktop"))
        desktop_id.remove_suffix(8);
    copy_str(out, sizeof(out), desktop_id);
}

} // namespace

const char* lx::shell::launch_failure_text(launch_failure f) {
    switch (f) {
    case launch_failure::none: return "started";
    case launch_failure::not_installed: return "not installed";
    case launch_failure::no_terminal: return "no terminal found";
    case launch_failure::invalid_exec: return "invalid Exec line";
    case launch_failure::start_failed: return "failed to start (see journal)";
    case launch_failure::exec_error: return "cannot execute";
    case launch_failure::systemd_error: return "systemd error";
    case launch_failure::spawn_error: return "cannot start a process";
    }
    return "?";
}

// ── Environment ──

const char* lx::shell::launch_env::get(std::string_view name) const {
    for (const std::string& v : vars)
        if (name_of(v) == name)
            return v.c_str() + name.size() + 1;
    return nullptr;
}

void lx::shell::build_launch_env(const char* const* env, const launch_env_context& ctx,
                                 launch_env& out) {
    out.vars.clear();
    out.envp.clear();
    bool have_desktop = false;
    for (const char* const* e = env; e && *e; ++e) {
        const std::string_view kv{*e};
        const std::string_view name = name_of(kv);
        if (name.size() == kv.size() || name.empty() || dropped(name))
            continue;
        if (name == "XDG_CURRENT_DESKTOP")
            have_desktop = kv.size() > name.size() + 1;
        if (name == "XDG_CURRENT_DESKTOP" && !have_desktop)
            continue;
        out.vars.emplace_back(kv);
    }
    const std::string_view display =
        ctx.wayland_display.empty() ? std::string_view{"wayland-0"} : ctx.wayland_display;
    out.vars.emplace_back(std::string("WAYLAND_DISPLAY=").append(display));
    out.vars.emplace_back("XDG_SESSION_TYPE=wayland");
    if (!have_desktop)
        out.vars.emplace_back("XDG_CURRENT_DESKTOP=Lumen");
    if (!ctx.token.empty()) {
        out.vars.emplace_back(std::string("XDG_ACTIVATION_TOKEN=").append(ctx.token));
        out.vars.emplace_back(std::string("DESKTOP_STARTUP_ID=").append(ctx.token));
    }
    out.envp.reserve(out.vars.size() + 1);
    for (const std::string& v : out.vars)
        out.envp.push_back(v.c_str());
    out.envp.push_back(nullptr);
}

// ── Unit names ──

std::size_t lx::shell::escape_unit_id(std::string_view id, char* out, std::size_t cap) {
    static constexpr char k_hex[] = "0123456789abcdef";
    std::size_t n = 0;
    for (char ch : id) {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '.';
        const std::size_t need = plain ? 1 : 4;
        if (n + need + 1 > cap)
            break;
        if (plain) {
            out[n++] = static_cast<char>(c);
        } else {
            out[n++] = '\\';
            out[n++] = 'x';
            out[n++] = k_hex[c >> 4];
            out[n++] = k_hex[c & 0xF];
        }
    }
    if (cap)
        out[n] = '\0';
    return n;
}

namespace {
std::size_t compose_unit_name(std::string_view id, const unsigned char (&rnd)[16], char sep,
                              std::string_view suffix, char (&out)[256]) {
    static constexpr std::string_view k_prefix = "app-lumen-";
    static constexpr char k_hex[] = "0123456789abcdef";
    // prefix + id + sep + 32 hex + suffix ≤ 255.
    const std::size_t room = 255 - k_prefix.size() - 1 - 32 - suffix.size();
    std::size_t n = 0;
    std::memcpy(out, k_prefix.data(), k_prefix.size());
    n += k_prefix.size();
    n += lx::shell::escape_unit_id(id.empty() ? std::string_view{"app"} : id, out + n, room + 1);
    out[n++] = sep;
    for (unsigned char b : rnd) {
        out[n++] = k_hex[b >> 4];
        out[n++] = k_hex[b & 0xF];
    }
    std::memcpy(out + n, suffix.data(), suffix.size());
    n += suffix.size();
    out[n] = '\0';
    return n;
}
} // namespace

std::size_t lx::shell::make_unit_name(std::string_view id, const unsigned char (&rnd)[16],
                                      char (&out)[256]) {
    return compose_unit_name(id, rnd, '@', ".service", out);
}

std::size_t lx::shell::make_scope_name(std::string_view id, const unsigned char (&rnd)[16],
                                       char (&out)[256]) {
    // systemd.io/DESKTOP_ENVIRONMENTS: app-<launcher>-<ApplicationID>-<RANDOM>.scope (the
    // escaped ID holds no '-').
    return compose_unit_name(id, rnd, '-', ".scope", out);
}

lx::shell::systemd_launch_mode lx::shell::launch_mode_for_version(std::string_view version) {
    // The leading decimal number: "262-1-arch", "249.11-0ubuntu3", "255.4-1ubuntu8".
    unsigned major = 0;
    std::size_t i = 0;
    while (i < version.size() && i < 6 && version[i] >= '0' && version[i] <= '9')
        major = major * 10 + static_cast<unsigned>(version[i++] - '0');
    if (i == 0)
        return systemd_launch_mode::scope;
    return major >= 250 ? systemd_launch_mode::service : systemd_launch_mode::scope;
}

lx::shell::start_error_class lx::shell::classify_start_error(std::string_view name) {
    static constexpr std::string_view k_absent[] = {
        "org.freedesktop.DBus.Error.ServiceUnknown",
        "org.freedesktop.DBus.Error.NameHasNoOwner",
        "org.freedesktop.DBus.Error.Spawn.ChildExited",
    };
    for (std::string_view a : k_absent)
        if (name == a)
            return start_error_class::no_systemd;
    if (name == "org.freedesktop.DBus.Error.NoReply" ||
        name == "org.freedesktop.DBus.Error.Timeout")
        return start_error_class::unknown_outcome;
    if (name == "org.freedesktop.DBus.Error.PropertyReadOnly")
        return start_error_class::too_old;
    return start_error_class::failed;
}

// ── Preparation ──

bool lx::shell::compose_terminal_argv(const exec_argv& terminal, std::string_view flag,
                                      const exec_argv& app, exec_argv& out) {
    out.clear();
    for (unsigned i = 0; i < terminal.argc; ++i)
        if (!out.push(terminal.argv[i]))
            return false;
    if (!flag.empty() && !out.push(flag))
        return false;
    for (unsigned i = 0; i < app.argc; ++i)
        if (!out.push(app.argv[i]))
            return false;
    return true;
}

lx::shell::launch_failure lx::shell::prepare_terminal_launch(const app_catalog& cat,
                                                             std::string_view home,
                                                             prepared_launch& out) {
    const terminal_choice& t = cat.terminal();
    if (!t.found)
        return launch_failure::no_terminal;
    if (t.entry >= 0) {
        const desktop_entry* e = cat.entry(static_cast<unsigned>(t.entry));
        if (!e)
            return launch_failure::no_terminal;
        return prepare_entry_launch(*e, cat, home, out);
    }
    out.argv.clear();
    if (!out.argv.push(t.argv0))
        return launch_failure::invalid_exec;
    copy_str(out.program, sizeof(out.program), t.program);
    const std::string_view a0{t.argv0};
    const std::size_t slash = a0.rfind('/');
    copy_str(out.id, sizeof(out.id), slash == std::string_view::npos ? a0 : a0.substr(slash + 1));
    copy_str(out.name, sizeof(out.name), out.id);
    out.source_path[0] = '\0';
    set_workdir({}, home, out.workdir);
    return launch_failure::none;
}

lx::shell::launch_failure lx::shell::prepare_entry_launch(const desktop_entry& e,
                                                          const app_catalog& cat,
                                                          std::string_view home,
                                                          prepared_launch& out) {
    static exec_argv app; // one thread; kept off the stack
    if (auto ok = expand_exec(e, app); !ok)
        return launch_failure::invalid_exec;
    id_of(e.id, out.id);
    copy_str(out.name, sizeof(out.name), e.name);
    copy_str(out.source_path, sizeof(out.source_path), e.file);
    set_workdir(e.path, home, out.workdir);
    const std::string_view path_env = cat.config().path_env;
    if (!e.terminal) {
        if (!resolve_program(app.argv[0], path_env, out.program))
            return launch_failure::not_installed;
        out.argv = app;
        // `argv` points into `app.buf`: re-point it into the copy.
        for (unsigned i = 0; i < out.argv.argc; ++i)
            out.argv.argv[i] = out.argv.buf + (app.argv[i] - app.buf);
        return launch_failure::none;
    }
    // Terminal=true: the program must exist too, or the terminal opens and closes at once.
    char program[PATH_MAX];
    if (!resolve_program(app.argv[0], path_env, program))
        return launch_failure::not_installed;
    const terminal_choice& t = cat.terminal();
    if (!t.found)
        return launch_failure::no_terminal;
    static exec_argv term;
    term.clear();
    if (t.entry >= 0) {
        const desktop_entry* te = cat.entry(static_cast<unsigned>(t.entry));
        if (!te || !expand_exec(*te, term))
            return launch_failure::no_terminal;
    } else if (!term.push(t.argv0)) {
        return launch_failure::no_terminal;
    }
    if (!resolve_program(term.argv[0], path_env, out.program))
        return launch_failure::no_terminal;
    if (!compose_terminal_argv(term, t.exec_flag, app, out.argv))
        return launch_failure::invalid_exec;
    return launch_failure::none;
}

// ── Launcher ──

void lx::shell::app_launcher::configure_from_env() {
    const char* m = std::getenv("LUMEN_SHELL_LAUNCH");
    if (m && std::strcmp(m, "fork") == 0) {
        method_ = launch_method::fork;
        launch_log(lx::trace::level::info,
                   "apps: LUMEN_SHELL_LAUNCH=fork: launching with double fork (not "
                   "resource-tracked)");
    } else if (m && std::strcmp(m, "scope") == 0) {
        method_ = launch_method::scope;
        launch_log(lx::trace::level::info,
                   "apps: LUMEN_SHELL_LAUNCH=scope: launching into transient scopes");
    }
}

namespace {
std::uint64_t monotonic_us() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
}

/// The pid of the middle child's {1, pid} record, or 0.
int pipe_pid(const unsigned char* buf, unsigned got) {
    for (unsigned off = 0; off + 8 <= got; off += 8) {
        int rec[2];
        std::memcpy(rec, buf + off, 8);
        if (rec[0] == 1)
            return rec[1];
    }
    return 0;
}

/// The middle child's {3, errno}: its fork failed, nothing waits on the hold.
bool pipe_spawn_failed(const unsigned char* buf, unsigned got) {
    for (unsigned off = 0; off + 8 <= got; off += 8) {
        int rec[2];
        std::memcpy(rec, buf + off, 8);
        if (rec[0] == 3)
            return true;
    }
    return false;
}
} // namespace

void lx::shell::app_launcher::release_hold(pending_pipe& pp) {
    // EOF on the grandchild's read end: it goes on to execve.
    if (pp.hold_fd >= 0)
        close(pp.hold_fd);
    pp.hold_fd = -1;
    pp.release_at_us = 0;
}

void lx::shell::app_launcher::report(const launch_outcome& o) {
    if (fn_)
        fn_(o, user_);
}

unsigned lx::shell::app_launcher::pending() const {
    unsigned n = 0;
    for (const auto& u : units_)
        n += u.used ? 1u : 0u;
    for (const auto& p : pipes_)
        n += p.fd >= 0 ? 1u : 0u;
    return n;
}

void lx::shell::app_launcher::start(const prepared_launch& p, std::string_view token,
                                    std::string_view display) {
    if (method_ != launch_method::fork && !no_systemd_ && open_bus()) {
        const systemd_launch_mode mode =
            method_ == launch_method::scope ? systemd_launch_mode::scope : mode_;
        if (mode == systemd_launch_mode::scope) {
            start_fork(p, token, display, true);
            return;
        }
        if (start_unit(p, token, display))
            return;
    }
    start_fork(p, token, display, false);
}

void lx::shell::app_launcher::systemd_gone(const char* why) {
    no_systemd_ = true;
    if (told_fallback_)
        return;
    told_fallback_ = true;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "apps: systemd --user unavailable (%s) — launching with double fork (not "
                  "resource-tracked)",
                  why);
    launch_log(lx::trace::level::info, line);
}

void lx::shell::app_launcher::finish_pipe(pending_pipe& pp) {
    close(pp.fd);
    pp.fd = -1;
    release_hold(pp);
    // A scope registration still in flight no longer has a process to release.
    for (auto& u : units_)
        if (u.used && u.scope_for == &pp)
            u.scope_for = nullptr;
    launch_outcome o = pp.outcome;
    for (unsigned off = 0; off + 8 <= pp.got; off += 8) {
        int rec[2];
        std::memcpy(rec, pp.buf + off, 8);
        if (rec[0] == 1) {
            o.pid = rec[1];
        } else if (rec[0] == 2) {
            o.error_number = rec[1];
            o.failure = launch_failure::exec_error;
        } else if (rec[0] == 3) {
            o.error_number = rec[1];
            o.failure = launch_failure::spawn_error;
        }
    }
    report(o);
}

void lx::shell::app_launcher::start_fork(const prepared_launch& p, std::string_view token,
                                         std::string_view display, bool hold) {
    launch_outcome o{};
    copy_str(o.id, sizeof(o.id), p.id);
    copy_str(o.name, sizeof(o.name), p.name);
    o.forked = true;
    pending_pipe* slot = nullptr;
    for (auto& pp : pipes_)
        if (pp.fd < 0) {
            slot = &pp;
            break;
        }
    // Everything the children use is prepared here: after fork they only make syscalls.
    launch_env env;
    build_launch_env(environ, {display, token}, env);
    int fds[2];
    if (!slot || pipe2(fds, O_CLOEXEC) != 0) {
        o.failure = launch_failure::spawn_error;
        o.error_number = slot ? errno : EAGAIN;
        report(o);
        return;
    }
    // Scope path: the grandchild reads `go[0]` before execve and goes on at EOF — when the
    // shell closes `go[1]` (scope ready, or the hold timed out), or exits.
    int go[2] = {-1, -1};
    if (hold && pipe2(go, O_CLOEXEC) != 0)
        go[0] = go[1] = -1; // not held: launched like the plain fallback
    const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    int out_fd = -1;
#if defined(LUMEN_HAS_LOGIND)
    out_fd = sd_journal_stream_fd(p.id, LOG_INFO, 0);
    if (out_fd >= 0)
        (void)fcntl(out_fd, F_SETFD, FD_CLOEXEC);
#endif
    if (out_fd < 0)
        out_fd = devnull;
    const pid_t middle = fork();
    if (middle == 0) {
        // Middle child: a new session (no controlling terminal to SIGHUP it), clean signal
        // state, the standard fds, everything else closed on exec. Only the shell keeps the
        // hold's write end, so its EOF is the shell's to give.
        if (go[1] >= 0)
            close(go[1]);
        (void)setsid();
        sigset_t none;
        sigemptyset(&none);
        (void)sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int s = 1; s < NSIG; ++s)
            (void)signal(s, SIG_DFL);
        if (devnull >= 0)
            (void)dup2(devnull, 0);
        if (out_fd >= 0) {
            (void)dup2(out_fd, 1);
            (void)dup2(out_fd, 2);
        }
        (void)syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */);
        if (chdir(p.workdir) != 0)
            (void)chdir("/");
        const pid_t child = fork();
        if (child == 0) {
            if (go[0] >= 0) {
                char b;
                while (read(go[0], &b, 1) < 0 && errno == EINTR) {
                }
            }
            execve(p.program, const_cast<char* const*>(p.argv.argv),
                   const_cast<char* const*>(env.envp.data()));
            const int rec[2] = {2, errno};
            (void)!write(fds[1], rec, sizeof(rec));
            _exit(127);
        }
        const int rec[2] = {child > 0 ? 1 : 3, child > 0 ? static_cast<int>(child) : errno};
        (void)!write(fds[1], rec, sizeof(rec));
        _exit(0);
    }
    const int fork_errno = errno;
    close(fds[1]);
    if (go[0] >= 0)
        close(go[0]);
    if (devnull >= 0)
        close(devnull);
    if (out_fd >= 0 && out_fd != devnull)
        close(out_fd);
    if (middle < 0) {
        close(fds[0]);
        if (go[1] >= 0)
            close(go[1]);
        o.failure = launch_failure::spawn_error;
        o.error_number = fork_errno;
        report(o);
        return;
    }
    // The middle child exits at once: reaped here, no zombie.
    int status = 0;
    while (waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    slot->fd = fds[0];
    slot->got = 0;
    slot->outcome = o;
    slot->hold_fd = go[1];
    slot->scope_sent = false;
    // Bounded from the start: a pid record that never comes does not hold the app forever.
    slot->release_at_us = go[1] >= 0 ? monotonic_us() + k_scope_hold_us : 0;
}

void lx::shell::app_launcher::run_deadlines() {
    std::uint64_t now = 0;
    for (auto& pp : pipes_) {
        if (pp.fd < 0 || pp.hold_fd < 0)
            continue;
        if (!now)
            now = monotonic_us();
        if (now >= pp.release_at_us) {
            if (launch_debug()) {
                char line[192];
                std::snprintf(line, sizeof(line),
                              "apps: %s: scope not ready in time — started outside it for now",
                              pp.outcome.id);
                launch_log(lx::trace::level::debug, line);
            }
            release_hold(pp);
        }
    }
    for (auto& u : units_) {
        if (!u.used || !u.deadline_us)
            continue;
        if (!now)
            now = monotonic_us();
        if (now < u.deadline_us)
            continue;
        // The call was answered (NoReply), so there is no slot left to cancel.
        if (u.scope_for) {
            release_hold(*u.scope_for);
            u = {};
            continue;
        }
        launch_outcome o{};
        copy_str(o.id, sizeof(o.id), u.launch ? u.launch->id : "");
        copy_str(o.name, sizeof(o.name), u.launch ? u.launch->name : "");
        copy_str(o.unit, sizeof(o.unit), u.unit);
        copy_str(o.detail, sizeof(o.detail), "no reply from systemd");
        o.failure = launch_failure::systemd_error;
        u = {};
        report(o);
    }
}

unsigned lx::shell::app_launcher::poll_fds(pollfd* out, unsigned cap, int& timeout_ms) const {
    unsigned n = 0;
#if defined(LUMEN_HAS_LOGIND)
    if (bus_ && n < cap) {
        auto* bus = static_cast<sd_bus*>(bus_);
        const int fd = sd_bus_get_fd(bus);
        const int ev = sd_bus_get_events(bus);
        if (fd >= 0 && ev >= 0) {
            out[n++] = {fd, static_cast<short>(ev), 0};
            std::uint64_t until = 0;
            if (sd_bus_get_timeout(bus, &until) >= 0 && until != UINT64_MAX) {
                timespec now{};
                clock_gettime(CLOCK_MONOTONIC, &now);
                const std::uint64_t now_us = static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
                                             static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
                const std::uint64_t ms = until > now_us ? (until - now_us + 999u) / 1000u : 0u;
                const int t = ms > 600000u ? 600000 : static_cast<int>(ms);
                if (timeout_ms < 0 || t < timeout_ms)
                    timeout_ms = t;
            }
        }
    }
#endif
    std::uint64_t next = UINT64_MAX;
    for (const auto& pp : pipes_)
        if (pp.fd >= 0 && pp.hold_fd >= 0 && pp.release_at_us < next)
            next = pp.release_at_us;
    for (const auto& u : units_)
        if (u.used && u.deadline_us && u.deadline_us < next)
            next = u.deadline_us;
    if (next != UINT64_MAX) {
        const std::uint64_t now = monotonic_us();
        const std::uint64_t ms = next > now ? (next - now + 999u) / 1000u : 0u;
        const int t = ms > 600000u ? 600000 : static_cast<int>(ms);
        if (timeout_ms < 0 || t < timeout_ms)
            timeout_ms = t;
    }
    for (const auto& pp : pipes_)
        if (pp.fd >= 0 && n < cap)
            out[n++] = {pp.fd, POLLIN, 0};
    return n;
}

void lx::shell::app_launcher::dispatch(const pollfd* fds, unsigned n) {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_) {
        // The bus is processed every time: its timeouts (a call's reply deadline) are due
        // even without fd activity.
        auto* bus = static_cast<sd_bus*>(bus_);
        for (int guard = 0; guard < 64; ++guard) {
            const int r = sd_bus_process(bus, nullptr);
            if (r <= 0) {
                if (r < 0) {
                    char line[128];
                    std::snprintf(line, sizeof(line), "apps: user bus failed: %s", std::strerror(-r));
                    launch_log(lx::trace::level::warn, line);
                    for (auto& u : units_)
                        if (u.used && u.slot)
                            u.slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(u.slot));
                    match_slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(match_slot_));
                    sub_slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(sub_slot_));
                    bus_ = sd_bus_flush_close_unref(bus);
                    // Launches still waiting for systemd: their outcome is unknown. A held
                    // process goes on without its scope (its pipe reports it).
                    for (auto& u : units_)
                        if (u.used && u.scope_for) {
                            release_hold(*u.scope_for);
                            u = {};
                        }
                    for (auto& u : units_)
                        if (u.used) {
                            launch_outcome o{};
                            copy_str(o.id, sizeof(o.id), u.launch ? u.launch->id : "");
                            copy_str(o.name, sizeof(o.name), u.launch ? u.launch->name : "");
                            copy_str(o.unit, sizeof(o.unit), u.unit);
                            copy_str(o.detail, sizeof(o.detail), "bus connection lost");
                            o.failure = launch_failure::systemd_error;
                            u = {};
                            report(o);
                        }
                }
                break;
            }
        }
    }
#endif
    for (unsigned i = 0; i < n; ++i) {
        if (!fds[i].revents)
            continue;
        for (auto& pp : pipes_) {
            if (pp.fd != fds[i].fd || pp.fd < 0)
                continue;
            for (;;) {
                const ssize_t r = read(pp.fd, pp.buf + pp.got, sizeof(pp.buf) - pp.got);
                if (r > 0) {
                    pp.got += static_cast<unsigned>(r);
                    if (pp.got < sizeof(pp.buf))
                        continue;
                    finish_pipe(pp);
                    break;
                }
                if (r < 0 && errno == EINTR)
                    continue;
                if (r < 0 && errno == EAGAIN)
                    break;
                finish_pipe(pp); // EOF: execve closed the last write end
                break;
            }
            // Scope path: the pid is known, the grandchild waits — move it into its scope.
            if (pp.fd >= 0 && pp.hold_fd >= 0 && !pp.scope_sent) {
                if (pipe_spawn_failed(pp.buf, pp.got))
                    release_hold(pp);
                else if (const int pid = pipe_pid(pp.buf, pp.got); pid > 0)
                    register_scope(pp, pid);
            }
        }
    }
    run_deadlines();
}

#if defined(LUMEN_HAS_LOGIND)

struct lx::shell::app_launcher::events {
    static int job_removed(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& l = *static_cast<app_launcher*>(data);
        std::uint32_t job_id = 0;
        const char* job = nullptr;
        const char* unit = nullptr;
        const char* result = nullptr;
        if (sd_bus_message_read(m, "uoss", &job_id, &job, &unit, &result) < 0 || !unit)
            return 0;
        for (auto& u : l.units_) {
            if (!u.used || std::strcmp(u.unit, unit) != 0)
                continue;
            if (u.scope_for || !u.launch) {
                // A scope: its process may go on (it is in the scope now, or never will
                // be); the pipe reports the launch.
                if (u.scope_for) {
                    release_hold(*u.scope_for);
                    if (!result || std::strcmp(result, "done") != 0)
                        u.scope_for->outcome.unit[0] = '\0'; // not in it
                }
                if (launch_debug() && result && std::strcmp(result, "done") != 0) {
                    char line[384];
                    std::snprintf(line, sizeof(line), "apps: scope %s: %s", unit, result);
                    launch_log(lx::trace::level::debug, line);
                }
                if (u.slot)
                    sd_bus_slot_unref(static_cast<sd_bus_slot*>(u.slot));
                u = {};
                return 0;
            }
            launch_outcome o{};
            copy_str(o.id, sizeof(o.id), u.launch ? u.launch->id : "");
            copy_str(o.name, sizeof(o.name), u.launch ? u.launch->name : "");
            copy_str(o.unit, sizeof(o.unit), u.unit);
            copy_str(o.detail, sizeof(o.detail), result ? result : "");
            o.failure = result && std::strcmp(result, "done") == 0 ? launch_failure::none
                                                                   : launch_failure::start_failed;
            if (u.slot)
                sd_bus_slot_unref(static_cast<sd_bus_slot*>(u.slot));
            u = {};
            l.report(o);
            return 0;
        }
        return 0;
    }

    static int started(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* u = static_cast<pending_unit*>(data);
        app_launcher* l = u->owner;
        if (!l || !u->used)
            return 0;
        if (u->slot) {
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(u->slot));
            u->slot = nullptr;
        }
        if (!sd_bus_message_is_method_error(m, nullptr)) {
            u->replied = true; // JobRemoved says how it ended
            return 0;
        }
        const sd_bus_error* err = sd_bus_message_get_error(m);
        const char* name = err && err->name ? err->name : "unknown";
        const start_error_class kind = classify_start_error(name);
        if (err && err->message && launch_debug()) {
            char line[384];
            std::snprintf(line, sizeof(line), "apps: StartTransientUnit %s: %s: %s", u->unit, name,
                          err->message);
            launch_log(lx::trace::level::debug, line);
        }
        if (kind == start_error_class::unknown_outcome) {
            // The call timed out, but systemd may still run it: never launched a second
            // time. JobRemoved can still report it; past the grace it counts as failed (a
            // scope's process is released at its hold deadline anyway).
            u->replied = true;
            u->deadline_us = monotonic_us() + k_no_reply_grace_us;
            return 0;
        }
        if (u->scope_for || !u->launch) {
            // A scope: the process goes on without it; its pipe reports the launch.
            if (u->scope_for) {
                release_hold(*u->scope_for);
                u->scope_for->outcome.unit[0] = '\0';
            }
            if (kind == start_error_class::no_systemd)
                l->systemd_gone(name);
            *u = {};
            u->owner = l;
            return 0;
        }
        if (kind == start_error_class::no_systemd || kind == start_error_class::too_old) {
            // Nothing was created. No systemd --user: this launch, and every later one, by
            // double fork. A manager without ExitType/Type=exec (older than its Version
            // claimed): as a scope from now on.
            std::unique_ptr<prepared_launch> launch = std::move(u->launch);
            const std::string token = u->token;
            const std::string display = u->display;
            *u = {};
            u->owner = l;
            if (kind == start_error_class::no_systemd) {
                l->systemd_gone(name);
            } else if (l->mode_ != systemd_launch_mode::scope) {
                l->mode_ = systemd_launch_mode::scope;
                launch_log(lx::trace::level::info,
                           "apps: systemd --user refused the service properties — launching "
                           "into transient scopes");
            }
            if (launch)
                l->start_fork(*launch, token, display, kind == start_error_class::too_old);
            return 0;
        }
        launch_outcome o{};
        copy_str(o.id, sizeof(o.id), u->launch ? u->launch->id : "");
        copy_str(o.name, sizeof(o.name), u->launch ? u->launch->name : "");
        copy_str(o.unit, sizeof(o.unit), u->unit);
        copy_str(o.detail, sizeof(o.detail), name);
        o.failure = launch_failure::systemd_error;
        *u = {};
        u->owner = l;
        l->report(o);
        return 0;
    }

    static int subscribed(sd_bus_message* m, void*, sd_bus_error*) {
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* err = sd_bus_message_get_error(m);
            char line[192];
            std::snprintf(line, sizeof(line), "apps: systemd Subscribe failed: %s",
                          err && err->name ? err->name : "?");
            launch_log(lx::trace::level::debug, line);
        }
        return 0;
    }
};

bool lx::shell::app_launcher::open_bus() {
    if (bus_)
        return true;
    sd_bus* bus = nullptr;
    int r = sd_bus_open_user_with_description(&bus, "lumen-shell-launch");
    if (r < 0) {
        systemd_gone(std::strerror(-r));
        return false;
    }
    // Manager.Version decides service or scope (ExitType=cgroup is 250's). Once, blocking
    // for at most a second, at the first launch — as KIO does; a local round trip.
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* call = nullptr;
    sd_bus_message* reply = nullptr;
    r = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.DBus.Properties", "Get");
    if (r >= 0)
        r = sd_bus_message_append(call, "ss", "org.freedesktop.systemd1.Manager", "Version");
    if (r >= 0)
        r = sd_bus_call(bus, call, 1'000'000, &err, &reply);
    sd_bus_message_unref(call);
    const char* version = nullptr;
    if (r >= 0 && sd_bus_message_read(reply, "v", "s", &version) < 0)
        version = nullptr;
    if (r < 0 && err.name &&
        classify_start_error(err.name) == start_error_class::no_systemd) {
        systemd_gone(err.name);
        sd_bus_error_free(&err);
        sd_bus_flush_close_unref(bus);
        return false;
    }
    mode_ = version ? launch_mode_for_version(version) : systemd_launch_mode::scope;
    {
        char line[256];
        std::snprintf(line, sizeof(line), "apps: systemd --user %s: %s",
                      version ? version : (err.name ? err.name : "version unknown"),
                      mode_ == systemd_launch_mode::service
                          ? "one transient service per launch"
                          : "one transient scope per launch (no ExitType=cgroup before 250)");
        launch_log(mode_ == systemd_launch_mode::service ? lx::trace::level::debug
                                                         : lx::trace::level::info,
                   line);
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    bus_ = bus;
    sd_bus_slot* match = nullptr;
    if (sd_bus_match_signal_async(bus, &match, "org.freedesktop.systemd1",
                                  "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
                                  "JobRemoved", events::job_removed, nullptr, this) >= 0)
        match_slot_ = match;
    sd_bus_slot* sub = nullptr;
    // JobRemoved is only emitted while someone is subscribed.
    if (sd_bus_call_method_async(bus, &sub, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                 "org.freedesktop.systemd1.Manager", "Subscribe",
                                 events::subscribed, this, "") >= 0)
        sub_slot_ = sub;
    return true;
}

bool lx::shell::app_launcher::start_unit(const prepared_launch& p, std::string_view token,
                                         std::string_view display) {
    if (!bus_)
        return false;
    pending_unit* u = nullptr;
    for (auto& cand : units_)
        if (!cand.used) {
            u = &cand;
            break;
        }
    if (!u)
        return false; // sixteen starts in flight: this one by fork
    unsigned char rnd[16];
    if (getrandom(rnd, sizeof(rnd), 0) != static_cast<ssize_t>(sizeof(rnd)))
        return false;
    char unit[256];
    (void)make_unit_name(p.id, rnd, unit);

    launch_env env;
    build_launch_env(environ, {display, token}, env);
    auto* bus = static_cast<sd_bus*>(bus_);
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager", "StartTransientUnit");
    // A failure in the middle of building leaves r < 0; checked once at the end.
    auto chk = [&](int v) {
        if (r >= 0 && v < 0)
            r = v;
    };
    if (r >= 0) {
        chk(sd_bus_message_append(m, "ss", unit, "fail"));
        chk(sd_bus_message_open_container(m, 'a', "(sv)"));
        chk(sd_bus_message_append(m, "(sv)", "Description", "s", p.name));
        if (p.source_path[0])
            chk(sd_bus_message_append(m, "(sv)", "SourcePath", "s", p.source_path));
        chk(sd_bus_message_append(m, "(sv)", "Type", "s", "exec"));
        chk(sd_bus_message_append(m, "(sv)", "ExitType", "s", "cgroup"));
        chk(sd_bus_message_append(m, "(sv)", "Slice", "s", "app.slice"));
        chk(sd_bus_message_append(m, "(sv)", "CollectMode", "s", "inactive-or-failed"));
        chk(sd_bus_message_append(m, "(sv)", "WorkingDirectory", "s", p.workdir));
        // Environment: the full launch environment (KDE does the same).
        chk(sd_bus_message_open_container(m, 'r', "sv"));
        chk(sd_bus_message_append(m, "s", "Environment"));
        chk(sd_bus_message_open_container(m, 'v', "as"));
        chk(sd_bus_message_open_container(m, 'a', "s"));
        for (const std::string& v : env.vars)
            chk(sd_bus_message_append(m, "s", v.c_str()));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        // The manager's own environment is merged under ours: strip what must not reach
        // the app from there too (DISPLAY: Lumen has no XWayland).
        chk(sd_bus_message_open_container(m, 'r', "sv"));
        chk(sd_bus_message_append(m, "s", "UnsetEnvironment"));
        chk(sd_bus_message_open_container(m, 'v', "as"));
        chk(sd_bus_message_open_container(m, 'a', "s"));
        chk(sd_bus_message_append(m, "s", "DISPLAY"));
        if (token.empty()) {
            chk(sd_bus_message_append(m, "s", "XDG_ACTIVATION_TOKEN"));
            chk(sd_bus_message_append(m, "s", "DESKTOP_STARTUP_ID"));
        }
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        // ExecStart: [(path, argv, ignore-failure=false)].
        chk(sd_bus_message_open_container(m, 'r', "sv"));
        chk(sd_bus_message_append(m, "s", "ExecStart"));
        chk(sd_bus_message_open_container(m, 'v', "a(sasb)"));
        chk(sd_bus_message_open_container(m, 'a', "(sasb)"));
        chk(sd_bus_message_open_container(m, 'r', "sasb"));
        chk(sd_bus_message_append(m, "s", p.program));
        chk(sd_bus_message_append_strv(m, const_cast<char**>(p.argv.argv)));
        chk(sd_bus_message_append(m, "b", 0));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_close_container(m)); // properties
        chk(sd_bus_message_append(m, "a(sa(sv))", 0));
    }
    sd_bus_slot* slot = nullptr;
    if (r >= 0)
        r = sd_bus_call_async(bus, &slot, m, events::started, u, 0);
    sd_bus_message_unref(m);
    if (r < 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "apps: StartTransientUnit not sent: %s", std::strerror(-r));
        launch_log(lx::trace::level::warn, line);
        return false;
    }
    u->owner = this;
    u->used = true;
    u->replied = false;
    copy_str(u->unit, sizeof(u->unit), unit);
    u->launch = std::make_unique<prepared_launch>(p);
    // argv points into the original's buffer: re-point it into the copy.
    for (unsigned i = 0; i < u->launch->argv.argc; ++i)
        u->launch->argv.argv[i] = u->launch->argv.buf + (p.argv.argv[i] - p.argv.buf);
    u->token.assign(token);
    u->display.assign(display);
    u->slot = slot;
    return true;
}

void lx::shell::app_launcher::register_scope(pending_pipe& pp, int pid) {
    pp.scope_sent = true;
    pending_unit* u = nullptr;
    for (auto& cand : units_)
        if (!cand.used) {
            u = &cand;
            break;
        }
    unsigned char rnd[16];
    if (!bus_ || !u || getrandom(rnd, sizeof(rnd), 0) != static_cast<ssize_t>(sizeof(rnd))) {
        release_hold(pp);
        return;
    }
    char unit[256];
    (void)make_scope_name(pp.outcome.id, rnd, unit);
    auto* bus = static_cast<sd_bus*>(bus_);
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager", "StartTransientUnit");
    auto chk = [&](int v) {
        if (r >= 0 && v < 0)
            r = v;
    };
    if (r >= 0) {
        // Only properties every systemd with transient scopes knows.
        chk(sd_bus_message_append(m, "ss", unit, "fail"));
        chk(sd_bus_message_open_container(m, 'a', "(sv)"));
        chk(sd_bus_message_append(m, "(sv)", "Description", "s", pp.outcome.name));
        chk(sd_bus_message_append(m, "(sv)", "Slice", "s", "app.slice"));
        chk(sd_bus_message_append(m, "(sv)", "PIDs", "au", 1, static_cast<std::uint32_t>(pid)));
        chk(sd_bus_message_close_container(m));
        chk(sd_bus_message_append(m, "a(sa(sv))", 0));
    }
    sd_bus_slot* slot = nullptr;
    if (r >= 0)
        r = sd_bus_call_async(bus, &slot, m, events::started, u, 0);
    sd_bus_message_unref(m);
    if (r < 0) {
        release_hold(pp);
        return;
    }
    u->owner = this;
    u->used = true;
    u->replied = false;
    copy_str(u->unit, sizeof(u->unit), unit);
    u->scope_for = &pp;
    u->slot = slot;
    copy_str(pp.outcome.unit, sizeof(pp.outcome.unit), unit);
}

lx::shell::app_launcher::~app_launcher() {
    for (auto& pp : pipes_)
        release_hold(pp);
    for (auto& u : units_)
        if (u.slot)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(u.slot));
    if (match_slot_)
        sd_bus_slot_unref(static_cast<sd_bus_slot*>(match_slot_));
    if (sub_slot_)
        sd_bus_slot_unref(static_cast<sd_bus_slot*>(sub_slot_));
    if (bus_)
        sd_bus_flush_close_unref(static_cast<sd_bus*>(bus_));
    for (auto& pp : pipes_)
        if (pp.fd >= 0)
            close(pp.fd);
}

#else // !LUMEN_HAS_LOGIND

struct lx::shell::app_launcher::events {};

bool lx::shell::app_launcher::open_bus() {
    systemd_gone("built without libsystemd");
    return false;
}
bool lx::shell::app_launcher::start_unit(const prepared_launch&, std::string_view,
                                         std::string_view) {
    return open_bus();
}
void lx::shell::app_launcher::register_scope(pending_pipe& pp, int) { release_hold(pp); }
lx::shell::app_launcher::~app_launcher() {
    for (auto& pp : pipes_) {
        release_hold(pp);
        if (pp.fd >= 0)
            close(pp.fd);
    }
}

#endif
