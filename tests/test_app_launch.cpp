// lumen-shell's app launching: the launch environment, unit names, terminal composition,
// pre-launch checks, the double-fork fallback run for real (session, parent, signal
// mask, inherited fds of the launched process), and the GPU an app is started on
// (PrefersNonDefaultGPU: switcheroo-control's variables, answered over a socketpair, or
// DRI_PRIME=1).
#include "lumen_test.hpp"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

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

LUMEN_TEST(launch_env_takes_the_gpu_variables_last) {
    const char* env[] = {"PATH=/usr/bin", "DRI_PRIME=0", "LANG=C", nullptr};
    const std::vector<std::string> extra = {"DRI_PRIME=pci-0000_01_00_0", "DRI_PRIME=1",
                                            "WAYLAND_DISPLAY=elsewhere", "NOEQUALS",
                                            "__NV_PRIME_RENDER_OFFLOAD=1"};
    lx::shell::launch_env out;
    lx::shell::build_launch_env(env, {"lumen-7", {}, extra}, out);
    // The shell's DRI_PRIME is replaced, a repeated name keeps its first value, and what
    // the launch environment sets itself cannot be overridden.
    unsigned prime = 0;
    for (const std::string& v : out.vars)
        prime += v.starts_with("DRI_PRIME=") ? 1u : 0u;
    LUMEN_CHECK(prime == 1 && std::strcmp(out.get("DRI_PRIME"), "pci-0000_01_00_0") == 0);
    LUMEN_CHECK(std::strcmp(out.get("WAYLAND_DISPLAY"), "lumen-7") == 0);
    LUMEN_CHECK(std::strcmp(out.get("__NV_PRIME_RENDER_OFFLOAD"), "1") == 0);
    LUMEN_CHECK(!out.get("NOEQUALS") && std::strcmp(out.get("LANG"), "C") == 0);
    // Without extra variables the shell's own value stays.
    lx::shell::build_launch_env(env, {"lumen-7", {}}, out);
    LUMEN_CHECK(std::strcmp(out.get("DRI_PRIME"), "0") == 0);
}

LUMEN_TEST(offload_env_with_and_without_switcheroo) {
    using lx::shell::switcheroo_gpu;
    std::vector<std::string> env;
    // No switcheroo-control: the render nodes decide, DRI_PRIME=1 with two or more.
    LUMEN_CHECK(!lx::shell::choose_offload_env(nullptr, 0, env) && env.empty());
    LUMEN_CHECK(!lx::shell::choose_offload_env(nullptr, 1, env) && env.empty());
    LUMEN_CHECK(lx::shell::choose_offload_env(nullptr, 2, env) && env.size() == 1 &&
                env[0] == "DRI_PRIME=1");
    // switcheroo-control decides when it answers: one GPU, nothing (whatever the nodes).
    const switcheroo_gpu intel{
        "Intel HD Graphics 5500", {"DRI_PRIME", "pci-0000_00_02_0"}, true, false};
    const switcheroo_gpu nouveau{
        "NVIDIA GM107M [GeForce GTX 950M]", {"DRI_PRIME", "pci-0000_01_00_0"}, false, true};
    std::vector<switcheroo_gpu> gpus{intel};
    LUMEN_CHECK(!lx::shell::choose_offload_env(&gpus, 3, env) && env.empty());
    // The notebook: the Intel GPU is the default; the GeForce's variables.
    gpus = {intel, nouveau};
    LUMEN_CHECK(lx::shell::choose_offload_env(&gpus, 2, env) && env.size() == 1 &&
                env[0] == "DRI_PRIME=pci-0000_01_00_0");
    // The NVIDIA driver's PRIME render offload: every pair, in order.
    const switcheroo_gpu nvidia{"NVIDIA",
                                {"__GLX_VENDOR_LIBRARY_NAME", "nvidia", "__NV_PRIME_RENDER_OFFLOAD",
                                 "1", "__VK_LAYER_NV_optimus", "NVIDIA_only"},
                                false,
                                true};
    gpus = {intel, nvidia};
    LUMEN_CHECK(lx::shell::choose_offload_env(&gpus, 2, env) && env.size() == 3 &&
                env[0] == "__GLX_VENDOR_LIBRARY_NAME=nvidia" &&
                env[1] == "__NV_PRIME_RENDER_OFFLOAD=1" &&
                env[2] == "__VK_LAYER_NV_optimus=NVIDIA_only");
    // Several non-default GPUs: a discrete one before the first listed; without Discrete
    // (older switcheroo-control) the first non-default one.
    const switcheroo_gpu igpu2{"Other", {"DRI_PRIME", "pci-0000_05_00_0"}, false, false};
    gpus = {intel, igpu2, nouveau};
    LUMEN_CHECK(lx::shell::choose_offload_env(&gpus, 3, env) &&
                env[0] == "DRI_PRIME=pci-0000_01_00_0");
    switcheroo_gpu old_nouveau = nouveau;
    old_nouveau.discrete = false;
    gpus = {intel, igpu2, old_nouveau};
    LUMEN_CHECK(lx::shell::choose_offload_env(&gpus, 3, env) &&
                env[0] == "DRI_PRIME=pci-0000_05_00_0");
    // No usable Environment on any non-default GPU (missing, a lone name): DRI_PRIME=1.
    gpus = {intel, {"x", {}, false, true}, {"y", {"DRI_PRIME"}, false, false}};
    LUMEN_CHECK(lx::shell::choose_offload_env(&gpus, 2, env) && env.size() == 1 &&
                env[0] == "DRI_PRIME=1");
}

LUMEN_TEST(render_nodes_are_counted) {
    char tmpl[] = "/tmp/lumen-dri-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    const std::string dir = tmpl;
    LUMEN_CHECK(lx::shell::count_render_nodes(dir.c_str()) == 0);
    for (const char* n : {"card0", "card1", "renderD128", "renderD129", "by-path"})
        write_file(dir + "/" + n, "", 0644);
    LUMEN_CHECK(lx::shell::count_render_nodes(dir.c_str()) == 2);
    LUMEN_CHECK(lx::shell::count_render_nodes((dir + "/missing").c_str()) == 0);
    (void)std::system(("rm -rf '" + dir + "'").c_str());
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
    write_file(g_root + "/bin/game", "#!/bin/sh\n", 0755);
    write_file(g_root + "/applications/game.desktop",
               "[Desktop Entry]\nType=Application\nName=Game\nExec=game\n"
               "PrefersNonDefaultGPU=true\n",
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
    LUMEN_CHECK(!p.non_default_gpu);
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*cat.find("game.desktop"), cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(p.non_default_gpu && std::strcmp(p.id, "game") == 0);
    const auto* foot = cat.find("foot.desktop");
    LUMEN_CHECK(lx::shell::prepare_entry_launch(*foot, cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(std::strcmp(p.workdir, "/tmp") == 0);
    LUMEN_CHECK(std::string_view(p.source_path).ends_with("/applications/foot.desktop"));
    LUMEN_CHECK(lx::shell::prepare_terminal_launch(cat, "/home/u", p) ==
                lx::shell::launch_failure::none);
    LUMEN_CHECK(std::strcmp(p.id, "foot") == 0 && p.argv.argc == 1 && !p.non_default_gpu);
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
    write_file(
        helper,
        "#!/bin/sh\n"
        "{ echo \"pid=$$\"; echo \"ppid=$PPID\";"
        "  echo \"sid=$(ps -o sid= -p $$ | tr -d ' ')\"; echo \"pgid=$(ps -o pgid= -p $$ | tr -d ' "
        "')\";"
        "  echo \"wd=$(pwd)\"; echo \"wl=$WAYLAND_DISPLAY\"; echo \"tok=$XDG_ACTIVATION_TOKEN\";"
        "  echo \"display=${DISPLAY-unset}\"; echo \"prime=${DRI_PRIME-unset}\";"
        "} > \"$1.tmp\"; mv \"$1.tmp\" \"$1\"\n"
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
    // Two GPUs without switcheroo-control: a non-default-GPU launch gets DRI_PRIME=1.
    l.probe_gpus(2, nullptr);
    LUMEN_CHECK(l.offload_available() && !l.offload_from_switcheroo());
    p.non_default_gpu = true;
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
    LUMEN_CHECK(r.find("prime=1\n") != std::string::npos);
    p.non_default_gpu = false;

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

    // A non-default-GPU launch: the variables are in the unit's environment.
    char tmpl[] = "/tmp/lumen-unit-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    const std::string out = std::string{tmpl} + "/prime";
    l.probe_gpus(2, nullptr);
    p.argv.clear();
    LUMEN_CHECK(p.argv.push("sh") && p.argv.push("-c") &&
                p.argv.push("echo \"$DRI_PRIME\" > \"$0\"") && p.argv.push(out));
    std::snprintf(p.program, sizeof(p.program), "/bin/sh");
    p.non_default_gpu = true;
    l.start(p, {}, "lumen-test");
    LUMEN_CHECK(wait_outcome(l, log, 3));
    LUMEN_CHECK(log.last.failure == lx::shell::launch_failure::none);
    LUMEN_CHECK(wait_for_file(out) && read_file(out) == "1\n");
    p.non_default_gpu = false;
    (void)std::system(("rm -rf '" + std::string{tmpl} + "'").c_str());
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

#if defined(LUMEN_HAS_LOGIND)

namespace {

// switcheroo-control's side: net.hadess.SwitcherooControl.GPUs, as the service builds it
// (Name, Environment, Default, Discrete; plus a key the reader must skip).
struct fake_switcheroo {
    std::vector<lx::shell::switcheroo_gpu> gpus;
    unsigned asked = 0;
};

int get_gpus(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* user,
             sd_bus_error*) {
    auto& f = *static_cast<fake_switcheroo*>(user);
    ++f.asked;
    int r = sd_bus_message_open_container(reply, 'a', "a{sv}");
    for (const auto& g : f.gpus) {
        r = r < 0 ? r : sd_bus_message_open_container(reply, 'a', "{sv}");
        r = r < 0 ? r : sd_bus_message_append(reply, "{sv}", "Name", "s", g.name.c_str());
        r = r < 0 ? r : sd_bus_message_open_container(reply, 'e', "sv");
        r = r < 0 ? r : sd_bus_message_append(reply, "s", "Environment");
        r = r < 0 ? r : sd_bus_message_open_container(reply, 'v', "as");
        r = r < 0 ? r : sd_bus_message_open_container(reply, 'a', "s");
        for (const std::string& v : g.environment)
            r = r < 0 ? r : sd_bus_message_append(reply, "s", v.c_str());
        r = r < 0 ? r : sd_bus_message_close_container(reply);
        r = r < 0 ? r : sd_bus_message_close_container(reply);
        r = r < 0 ? r : sd_bus_message_close_container(reply);
        r = r < 0 ? r : sd_bus_message_append(reply, "{sv}", "Default", "b", g.is_default);
        r = r < 0 ? r : sd_bus_message_append(reply, "{sv}", "Discrete", "b", g.discrete);
        r = r < 0 ? r : sd_bus_message_append(reply, "{sv}", "X-Future", "u", 7u);
        r = r < 0 ? r : sd_bus_message_close_container(reply);
    }
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

const sd_bus_vtable k_switcheroo_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("GPUs", "aa{sv}", get_gpus, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END,
};

/// A system bus on a socketpair: the launcher holds the client end.
struct fake_system_bus {
    sd_bus* server = nullptr;
    sd_bus* client = nullptr;
    sd_bus_slot* object = nullptr;
    fake_switcheroo service;
    explicit fake_system_bus(bool with_switcheroo) {
        int fds[2];
        LUMEN_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        sd_id128_t id{};
        (void)sd_id128_randomize(&id);
        sd_bus_new(&server);
        sd_bus_set_fd(server, fds[0], fds[0]);
        sd_bus_set_server(server, 1, id);
        sd_bus_start(server);
        sd_bus_new(&client);
        sd_bus_set_fd(client, fds[1], fds[1]);
        sd_bus_start(client);
        if (with_switcheroo)
            LUMEN_CHECK(sd_bus_add_object_vtable(server, &object, "/net/hadess/SwitcherooControl",
                                                 "net.hadess.SwitcherooControl",
                                                 k_switcheroo_vtable, &service) >= 0);
    }
    ~fake_system_bus() {
        sd_bus_slot_unref(object);
        sd_bus_flush_close_unref(client);
        sd_bus_flush_close_unref(server);
    }
    /// Runs both ends until `done` (or a while: the answer is in by then).
    template <class F>
    bool pump(lx::shell::app_launcher& l, F done) {
        for (int i = 0; i < 500; ++i) {
            while (sd_bus_process(server, nullptr) > 0) {
            }
            l.dispatch({});
            if (done())
                return true;
            usleep(1000);
        }
        return false;
    }
};

} // namespace

LUMEN_TEST(switcheroo_answer_picks_the_gpu_variables) {
    using lx::shell::switcheroo_gpu;
    // The notebook: Intel HD 5500 (default, drives the display), GeForce GTX 950M on NVK.
    fake_system_bus bus(true);
    bus.service.gpus = {
        {"Intel Corporation HD Graphics 5500", {"DRI_PRIME", "pci-0000_00_02_0"}, true, false},
        {"NVIDIA Corporation GM107M [GeForce GTX 950M]",
         {"DRI_PRIME", "pci-0000_0a_00_0"},
         false,
         true},
    };
    lx::shell::app_launcher l;
    l.probe_gpus(2, bus.client);
    // Until the answer: DRI_PRIME=1.
    LUMEN_CHECK(l.offload_available() && !l.offload_from_switcheroo() &&
                l.offload_env().front() == "DRI_PRIME=1");
    LUMEN_CHECK(bus.pump(l, [&] { return l.offload_from_switcheroo(); }));
    LUMEN_CHECK(bus.service.asked == 1);
    LUMEN_CHECK(l.offload_env().size() == 1 &&
                l.offload_env().front() == "DRI_PRIME=pci-0000_0a_00_0");
    // The poll set has the system bus while it is open.
    pollfd fds[lx::shell::app_launcher::k_max_poll];
    int timeout = -1;
    LUMEN_CHECK(l.poll_fds(fds, lx::shell::app_launcher::k_max_poll, timeout) == 1 &&
                fds[0].fd == sd_bus_get_fd(bus.client));
}

LUMEN_TEST(switcheroo_reader_and_its_one_gpu_answer) {
    // switcheroo-control knows better than the render nodes (one may be vgem, a display-only
    // device): one GPU in its answer, nothing to offload to.
    fake_system_bus bus(true);
    bus.service.gpus = {{"Intel", {"DRI_PRIME", "pci-0000_00_02_0"}, true, false}};
    lx::shell::app_launcher l;
    l.probe_gpus(2, bus.client);
    LUMEN_CHECK(bus.pump(l, [&] { return l.offload_from_switcheroo(); }));
    LUMEN_CHECK(!l.offload_available() && l.offload_env().empty());
}

LUMEN_TEST(no_switcheroo_falls_back_to_dri_prime) {
    // Nothing at the path (on a real bus: ServiceUnknown): the error is the answer.
    fake_system_bus bus(false);
    lx::shell::app_launcher l;
    l.probe_gpus(2, bus.client);
    (void)bus.pump(l, [] { return false; });
    LUMEN_CHECK(l.offload_available() && !l.offload_from_switcheroo());
    LUMEN_CHECK(l.offload_env().size() == 1 && l.offload_env().front() == "DRI_PRIME=1");
    // One GPU: no system bus at all, nothing to offload to.
    l.probe_gpus(1, bus.client);
    pollfd fds[lx::shell::app_launcher::k_max_poll];
    int timeout = -1;
    LUMEN_CHECK(!l.offload_available() &&
                l.poll_fds(fds, lx::shell::app_launcher::k_max_poll, timeout) == 0);
}

#endif // LUMEN_HAS_LOGIND

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
