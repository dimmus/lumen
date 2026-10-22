// lumen-shell's app launching: the launch environment, unit names, terminal composition,
// pre-launch checks, and the double-fork fallback run for real (session, parent, signal
// mask, inherited fds of the launched process).
#include "lumen_test.hpp"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.shell;

namespace {

std::string g_root;

void write_file(const std::string& path, std::string_view text, mode_t mode) {
    FILE* f = std::fopen(path.c_str(), "w");
    LUMEN_CHECK(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    chmod(path.c_str(), mode);
}

std::string read_file(const std::string& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return s;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    std::fclose(f);
    return s;
}

struct outcome_log {
    lx::shell::launch_outcome last{};
    unsigned count = 0;
};

void on_outcome(const lx::shell::launch_outcome& o, void* user) {
    auto& l = *static_cast<outcome_log*>(user);
    l.last = o;
    ++l.count;
}

bool wait_outcome(lx::shell::app_launcher& l, outcome_log& log, unsigned want) {
    for (int i = 0; i < 300 && log.count < want; ++i) {
        pollfd fds[lx::shell::app_launcher::k_max_poll];
        int timeout = 20;
        const unsigned n = l.poll_fds(fds, lx::shell::app_launcher::k_max_poll, timeout);
        (void)poll(fds, n, timeout);
        l.dispatch({fds, n});
    }
    return log.count >= want;
}

bool wait_for_file(const std::string& p) {
    for (int i = 0; i < 300; ++i) {
        struct stat st{};
        if (stat(p.c_str(), &st) == 0 && st.st_size > 0) {
            usleep(20000); // the writer may still be writing
            return true;
        }
        usleep(10000);
    }
    return false;
}

} // namespace

LUMEN_TEST(launch_env_drops_sets_and_keeps) {
    const char* env[] = {"PATH=/usr/bin",         "DISPLAY=:0",
                         "LUMEN_LOG=debug",       "XDG_ACTIVATION_TOKEN=old",
                         "DESKTOP_STARTUP_ID=old", "INVOCATION_ID=abc",
                         "NOTIFY_SOCKET=/run/x",  "TRIGGER_UNIT=x.path",
                         "WAYLAND_DISPLAY=elsewhere", "GDK_BACKEND=wayland",
                         "LANG=de_DE.UTF-8",      "XDG_CURRENT_DESKTOP=GNOME",
                         "NOEQUALS",              nullptr};
    lx::shell::launch_env out;
    lx::shell::build_launch_env(env, {"lumen-7", "tok123"}, out);
    LUMEN_CHECK(!out.get("DISPLAY") && !out.get("LUMEN_LOG") && !out.get("INVOCATION_ID") &&
                !out.get("NOTIFY_SOCKET") && !out.get("TRIGGER_UNIT") && !out.get("NOEQUALS"));
    LUMEN_CHECK(std::strcmp(out.get("WAYLAND_DISPLAY"), "lumen-7") == 0);
    LUMEN_CHECK(std::strcmp(out.get("XDG_SESSION_TYPE"), "wayland") == 0);
    LUMEN_CHECK(std::strcmp(out.get("XDG_ACTIVATION_TOKEN"), "tok123") == 0);
    LUMEN_CHECK(std::strcmp(out.get("DESKTOP_STARTUP_ID"), "tok123") == 0);
    LUMEN_CHECK(std::strcmp(out.get("XDG_CURRENT_DESKTOP"), "GNOME") == 0); // kept
    LUMEN_CHECK(std::strcmp(out.get("GDK_BACKEND"), "wayland") == 0);
    LUMEN_CHECK(std::strcmp(out.get("PATH"), "/usr/bin") == 0);
    LUMEN_CHECK(out.envp.back() == nullptr && out.envp.size() == out.vars.size() + 1);
    // No token: none at all (not the inherited one); no desktop: Lumen.
    const char* bare[] = {"XDG_ACTIVATION_TOKEN=old", "XDG_CURRENT_DESKTOP=", nullptr};
    lx::shell::build_launch_env(bare, {{}, {}}, out);
    LUMEN_CHECK(!out.get("XDG_ACTIVATION_TOKEN") && !out.get("DESKTOP_STARTUP_ID"));
    LUMEN_CHECK(std::strcmp(out.get("XDG_CURRENT_DESKTOP"), "Lumen") == 0);
    LUMEN_CHECK(std::strcmp(out.get("WAYLAND_DISPLAY"), "wayland-0") == 0);
}

LUMEN_TEST(unit_names_escape_and_cap_length) {
    const unsigned char rnd[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                   0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xff};
    char name[256];
    const std::size_t n = lx::shell::make_unit_name("org.gnome.Foo-bar", rnd, name);
    LUMEN_CHECK(std::string_view(name, n) ==
                "app-lumen-org.gnome.Foo\\x2dbar@0123456789abcdef00112233445566ff.service");
    char esc[32];
    LUMEN_CHECK(lx::shell::escape_unit_id("a b/é", esc, sizeof(esc)) == 2 + 4 + 4 + 8);
    LUMEN_CHECK(std::string_view(esc) == "a\\x20b\\x2f\\xc3\\xa9");
    // A long ID is cut at a whole escape so the name stays ≤ 255 bytes.
    const std::string long_id(400, '-');
    const std::size_t ln = lx::shell::make_unit_name(long_id, rnd, name);
    LUMEN_CHECK(ln <= 255 && std::strlen(name) == ln);
    LUMEN_CHECK(std::string_view(name).ends_with("ff.service"));
    const std::string_view mid(name + 10, ln - 10 - 33 - 8);
    LUMEN_CHECK(mid.size() % 4 == 0); // only whole "\x2d" escapes
}

LUMEN_TEST(scope_names_version_modes_and_start_errors) {
    const unsigned char rnd[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                   0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xff};
    char name[256];
    const std::size_t n = lx::shell::make_scope_name("org.gnome.Foo-bar", rnd, name);
    LUMEN_CHECK(std::string_view(name, n) ==
                "app-lumen-org.gnome.Foo\\x2dbar-0123456789abcdef00112233445566ff.scope");
    const std::size_t ln = lx::shell::make_scope_name(std::string(400, 'a'), rnd, name);
    LUMEN_CHECK(ln == 255 && std::string_view(name).ends_with("ff.scope"));

    // A service needs ExitType=cgroup (250); older, or unreadable: a scope.
    using lx::shell::systemd_launch_mode;
    LUMEN_CHECK(lx::shell::launch_mode_for_version("262-1-arch") == systemd_launch_mode::service);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("250") == systemd_launch_mode::service);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("255.4-1ubuntu8") ==
                systemd_launch_mode::service);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("249.11-0ubuntu3") ==
                systemd_launch_mode::scope);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("239 (239-78.el8)") ==
                systemd_launch_mode::scope);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("") == systemd_launch_mode::scope);
    LUMEN_CHECK(lx::shell::launch_mode_for_version("v262") == systemd_launch_mode::scope);

    // NoReply is not "no systemd" (the unit may still start: never launched twice);
    // PropertyReadOnly is an older manager (retried as a scope).
    using lx::shell::start_error_class;
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.DBus.Error.ServiceUnknown") ==
                start_error_class::no_systemd);
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.DBus.Error.NameHasNoOwner") ==
                start_error_class::no_systemd);
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.DBus.Error.NoReply") ==
                start_error_class::unknown_outcome);
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.DBus.Error.PropertyReadOnly") ==
                start_error_class::too_old);
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.DBus.Error.InvalidArgs") ==
                start_error_class::failed);
    LUMEN_CHECK(lx::shell::classify_start_error("org.freedesktop.systemd1.UnitExists") ==
                start_error_class::failed);
}

LUMEN_TEST(terminal_argv_composition_and_prechecks) {
    char tmpl[] = "/tmp/lumen-launch-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    g_root = tmpl;
    (void)mkdir((g_root + "/applications").c_str(), 0755);
    (void)mkdir((g_root + "/bin").c_str(), 0755);
    write_file(g_root + "/bin/vim", "#!/bin/sh\n", 0755);
    write_file(g_root + "/bin/foot", "#!/bin/sh\n", 0755);
    write_file(g_root + "/applications/vim.desktop",
               "[Desktop Entry]\nType=Application\nName=Vim\nExec=vim %F\nTerminal=true\n"
               "Path=/nonexistent-dir\n",
               0644);
    write_file(g_root + "/applications/gone.desktop",
               "[Desktop Entry]\nType=Application\nName=Gone\nExec=/nonexistent/x\n", 0644);
    write_file(g_root + "/applications/foot.desktop",
               "[Desktop Entry]\nType=Application\nName=Foot\nExec=foot\n"
               "Categories=TerminalEmulator;\nPath=/tmp\n",
               0644);
    lx::shell::catalog_config cfg{};
    cfg.data_home = g_root;
    cfg.path_env = g_root + "/bin";
    lx::shell::app_catalog cat;
    cat.configure(cfg);
    cat.scan();
    static lx::shell::prepared_launch p;
    const auto* vim = cat.find("vim.desktop");
    LUMEN_CHECK(vim && vim->terminal);
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*vim, cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(p.argv.argc == 3 && std::strcmp(p.argv.argv[0], "foot") == 0 &&
                std::strcmp(p.argv.argv[1], "-e") == 0 && std::strcmp(p.argv.argv[2], "vim") == 0);
    LUMEN_CHECK(std::string_view(p.program) == g_root + "/bin/foot");
    LUMEN_CHECK(std::strcmp(p.id, "vim") == 0 && std::strcmp(p.name, "Vim") == 0);
    LUMEN_CHECK(std::strcmp(p.workdir, "/home/u") == 0); // Path missing: $HOME
    const auto* foot = cat.find("foot.desktop");
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*foot, cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(std::strcmp(p.workdir, "/tmp") == 0);
    LUMEN_CHECK(std::string_view(p.source_path).ends_with("/applications/foot.desktop"));
    LUMEN_CHECK(lx::shell::prepare_terminal_launch(cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(std::strcmp(p.id, "foot") == 0 && p.argv.argc == 1);
    // The copy re-points argv into its own buffer.
    LUMEN_CHECK(p.argv.argv[0] >= p.argv.buf && p.argv.argv[0] < p.argv.buf + sizeof(p.argv.buf));
    const auto* gone = cat.find("gone.desktop");
    LUMEN_CHECK(gone && gone->listed()); // listed: whether it runs is found at launch
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*gone, cat, "/home/u", p) ==
                lx::shell::launch_failure::not_installed);
    // Terminal=true without any terminal.
    unlink((g_root + "/applications/foot.desktop").c_str());
    cat.scan();
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*cat.find("vim.desktop"), cat, "/h", p) ==
                lx::shell::launch_failure::no_terminal);
    LUMEN_CHECK(lx::shell::prepare_terminal_launch(cat, "/h", p) ==
                lx::shell::launch_failure::no_terminal);
    // The composition alone.
    lx::shell::exec_argv t, a, out;
    LUMEN_CHECK(t.push("xdg-terminal-exec") && a.push("htop") && a.push("-d") && a.push("5"));
    LUMEN_CHECK(lx::shell::compose_terminal_argv(t, "", a, out) && out.argc == 4);
    LUMEN_CHECK(std::strcmp(out.argv[1], "htop") == 0 && out.argv[4] == nullptr);
    (void)std::system(("rm -rf '" + g_root + "'").c_str());
}

LUMEN_TEST(double_fork_fallback_detaches_for_real) {
    char tmpl[] = "/tmp/lumen-fork-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    g_root = tmpl;
    const std::string report = g_root + "/report";
    // The helper records what a launched app would see, then becomes `sleep` so its
    // inherited state (signal mask, fds) can be read from /proc while it lingers.
    const std::string helper = g_root + "/helper";
    write_file(helper,
               "#!/bin/sh\n"
               "{ echo \"pid=$$\"; echo \"ppid=$PPID\";"
               "  echo \"sid=$(ps -o sid= -p $$ | tr -d ' ')\"; echo \"pgid=$(ps -o pgid= -p $$ | tr -d ' ')\";"
               "  echo \"wd=$(pwd)\"; echo \"wl=$WAYLAND_DISPLAY\"; echo \"tok=$XDG_ACTIVATION_TOKEN\";"
               "  echo \"display=${DISPLAY-unset}\"; } > \"$1.tmp\"; mv \"$1.tmp\" \"$1\"\n"
               "exec sleep 2\n",
               0755);
    // The test blocks a signal and holds an extra fd, as lumen-shell does (signalfd).
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, nullptr);
    const int opened = open("/dev/null", O_RDONLY);
    const int extra = fcntl(opened, F_DUPFD, 77); // not CLOEXEC on purpose
    close(opened);
    setenv("DISPLAY", ":9", 1);

    lx::shell::app_launcher l;
    l.set_method(lx::shell::launch_method::fork);
    outcome_log log;
    l.set_outcome_handler(on_outcome, &log);
    static lx::shell::prepared_launch p;
    p.argv.clear();
    LUMEN_CHECK(p.argv.push("helper") && p.argv.push(report));
    std::snprintf(p.program, sizeof(p.program), "%s", helper.c_str());
    std::snprintf(p.id, sizeof(p.id), "helper");
    std::snprintf(p.name, sizeof(p.name), "Helper");
    std::snprintf(p.workdir, sizeof(p.workdir), "%s", g_root.c_str());
    l.start(p, "tok-1", "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 1));
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::none && log.last.forked);
    LUMEN_CHECK(log.last.pid > 0);
    LUMEN_CHECK(wait_for_file(report));
    const std::string r = read_file(report);
    std::printf("    report: %s", r.c_str());
    char want[64];
    std::snprintf(want, sizeof(want), "pid=%d\n", log.last.pid);
    LUMEN_CHECK(r.find(want) != std::string::npos);
    std::snprintf(want, sizeof(want), "ppid=%d\n", static_cast<int>(getpid()));
    LUMEN_CHECK(r.find(want) == std::string::npos); // not our child
    // Its own session and process group (the middle child's: a double fork's grandchild is
    // deliberately not a session leader, so it can never acquire a controlling terminal).
    const auto field = [&](const char* key) {
        const std::size_t at = r.find(key);
        return at == std::string::npos ? -1 : std::atoi(r.c_str() + at + std::strlen(key));
    };
    LUMEN_CHECK(field("sid=") > 0 && field("sid=") != static_cast<int>(getsid(0)));
    LUMEN_CHECK(field("pgid=") == field("sid="));
    // Now `sleep`: what it inherited. No blocked signal (the test blocks SIGTERM), and only
    // the standard fds (the test holds fd 77 without CLOEXEC).
    usleep(100000);
    const std::string status = read_file("/proc/" + std::to_string(log.last.pid) + "/status");
    LUMEN_CHECK(status.find("SigBlk:\t0000000000000000") != std::string::npos);
    std::string fds;
    if (DIR* d = opendir(("/proc/" + std::to_string(log.last.pid) + "/fd").c_str())) {
        while (dirent* de = readdir(d))
            if (de->d_name[0] != '.')
                fds += std::string(de->d_name) + " ";
        closedir(d);
    }
    std::printf("    fds of the launched process: %s\n", fds.c_str());
    LUMEN_CHECK(fds == "0 1 2 ");
    (void)extra;
    LUMEN_CHECK(r.find("wd=" + g_root + "\n") != std::string::npos);
    LUMEN_CHECK(r.find("wl=lumen-test\n") != std::string::npos);
    LUMEN_CHECK(r.find("tok=tok-1\n") != std::string::npos);
    LUMEN_CHECK(r.find("display=unset\n") != std::string::npos);

    // execve failing: the errno comes back.
    std::snprintf(p.program, sizeof(p.program), "%s/not-there", g_root.c_str());
    l.start(p, {}, "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 2));
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::exec_error &&
                log.last.error_number == ENOENT);
    LUMEN_CHECK(l.pending() == 0);

    close(extra);
    sigprocmask(SIG_UNBLOCK, &block, nullptr);
    unsetenv("DISPLAY");
    usleep(1100000); // the helper's sleep
    (void)std::system(("rm -rf '" + g_root + "'").c_str());
}

// Starts real user units, so it runs only when asked (LUMEN_TEST_SYSTEMD_LAUNCH=1) and
// when systemd --user answers.
LUMEN_TEST(systemd_transient_service_reports_done_and_failed) {
    if (!std::getenv("LUMEN_TEST_SYSTEMD_LAUNCH")) {
        std::printf("    skipped (set LUMEN_TEST_SYSTEMD_LAUNCH=1)\n");
        return;
    }
    lx::shell::app_launcher l;
    outcome_log log;
    l.set_outcome_handler(on_outcome, &log);
    static lx::shell::prepared_launch p;
    p.argv.clear();
    LUMEN_CHECK(p.argv.push("true"));
    std::snprintf(p.program, sizeof(p.program), "/bin/true");
    std::snprintf(p.id, sizeof(p.id), "lumen-test-true");
    std::snprintf(p.name, sizeof(p.name), "True");
    std::snprintf(p.workdir, sizeof(p.workdir), "/");
    l.start(p, {}, "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 1));
    std::printf("    unit %s: %s (%s)\n", log.last.unit, lx::shell::launch_failure_text(log.last.failure),
                log.last.detail);
    LUMEN_CHECK(l.systemd_available() && !log.last.forked);
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::none);
    LUMEN_CHECK(std::string_view(log.last.unit).starts_with("app-lumen-lumen\\x2dtest\\x2dtrue@"));
    std::snprintf(p.program, sizeof(p.program), "/nonexistent/lumen-x");
    l.start(p, {}, "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 2));
    std::printf("    unit %s: %s (%s)\n", log.last.unit, lx::shell::launch_failure_text(log.last.failure),
                log.last.detail);
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::start_failed);
    LUMEN_CHECK(std::strcmp(log.last.detail, "failed") == 0);
}

// The scope path (systemd < 250, or LUMEN_SHELL_LAUNCH=scope) for real: the process is
// held before execve until it is in its scope, so the app starts inside it. Opt-in, like
// the service case.
LUMEN_TEST(systemd_scope_holds_the_process_until_it_is_in_its_scope) {
    if (!std::getenv("LUMEN_TEST_SYSTEMD_LAUNCH")) {
        std::printf("    skipped (set LUMEN_TEST_SYSTEMD_LAUNCH=1)\n");
        return;
    }
    char tmpl[] = "/tmp/lumen-scope-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    g_root = tmpl;
    const std::string report = g_root + "/cgroup";
    const std::string helper = g_root + "/helper";
    // Its cgroup at its very first instruction: the scope already, or the hold failed.
    write_file(helper,
               "#!/bin/sh\ncat /proc/self/cgroup > \"$1.tmp\"; mv \"$1.tmp\" \"$1\"\n", 0755);
    lx::shell::app_launcher l;
    l.set_method(lx::shell::launch_method::scope);
    outcome_log log;
    l.set_outcome_handler(on_outcome, &log);
    static lx::shell::prepared_launch p;
    p.argv.clear();
    LUMEN_CHECK(p.argv.push("helper") && p.argv.push(report));
    std::snprintf(p.program, sizeof(p.program), "%s", helper.c_str());
    std::snprintf(p.id, sizeof(p.id), "lumen-test-scope");
    std::snprintf(p.name, sizeof(p.name), "Scope test");
    std::snprintf(p.workdir, sizeof(p.workdir), "/");
    l.start(p, {}, "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 1));
    LUMEN_CHECK(l.systemd_available());
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::none && log.last.forked);
    std::printf("    unit %s\n", log.last.unit);
    LUMEN_CHECK(
        std::string_view(log.last.unit).starts_with("app-lumen-lumen\\x2dtest\\x2dscope-"));
    LUMEN_CHECK(wait_for_file(report));
    const std::string cg = read_file(report);
    std::printf("    cgroup: %s", cg.c_str());
    // In the cgroup the unit name's backslash is kept verbatim.
    LUMEN_CHECK(cg.find(std::string("/app.slice/") + log.last.unit) != std::string::npos);
    // The scope ends with its process; the launcher keeps nothing.
    for (int i = 0; i < 50 && l.pending() > 0; ++i) {
        pollfd fds[lx::shell::app_launcher::k_max_poll];
        int timeout = 20;
        const unsigned n = l.poll_fds(fds, lx::shell::app_launcher::k_max_poll, timeout);
        (void)poll(fds, n, timeout);
        l.dispatch({fds, n});
    }
    LUMEN_CHECK(l.pending() == 0);
    (void)std::system(("rm -rf '" + g_root + "'").c_str());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
