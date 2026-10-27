// Prints what the input stack sees, and writes the same thing to a log file.
//
// The point is isolation. "Typing does nothing" can mean no permission to open devices, no
// devices on the seat, a build with input compiled out, a keymap that failed to compile,
// events that arrive but are not routed, or a client that never bound a keyboard. Running
// the whole compositor exercises all of those at once and reports which failed only by
// omission.
//
// This exercises lx.input alone: device open, libinput dispatch, xkb translation. If it
// records keystrokes, everything below the compositor works and anything still broken is
// routing. If it does not, the log says which cause applies.
//
//   ./lumen-input-probe                  # 10 seconds -> ./lumen-input-probe.log
//   ./lumen-input-probe 60               # 60 seconds
//   ./lumen-input-probe 60 -o /tmp/x.log # log elsewhere
//
// Ctrl-C stops early and still writes the summary. The log is self-contained — environment,
// permissions, per-device open results, keymap, every event, and a verdict — so it can be
// read without asking follow-up questions.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.input;
import lx.session;
import lx.trace;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::FILE* g_log = nullptr;
std::chrono::steady_clock::time_point g_start{};

constexpr lx::trace::category log_probe{"probe"};

const char* level_name(lx::trace::level lvl) {
    switch (lvl) {
    case lx::trace::level::trace:
        return "TRACE";
    case lx::trace::level::debug:
        return "DEBUG";
    case lx::trace::level::info:
        return "INFO";
    case lx::trace::level::warn:
        return "WARN";
    case lx::trace::level::error:
        return "ERROR";
    case lx::trace::level::critical:
        return "CRITICAL";
    }
    return "?";
}

/// The probe's own lines as they are; lx.input's and lx.session's with level and category.
void write_line(std::FILE* f, lx::trace::level lvl, const char* category, const char* message) {
    if (std::strcmp(category, log_probe.name()) != 0)
        std::fprintf(f, "[%s] %s: ", level_name(lvl), category);
    std::fprintf(f, "%s\n", message);
    std::fflush(f);
}

/// Everything goes to both the terminal and the log, so what you read is what you can
/// paste. The log carries a millisecond offset per line — event timing is often the answer.
void probe_sink(lx::trace::level lvl, const char* category, const char* message, void*) {
    write_line(stdout, lvl, category, message);
    if (g_log) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_start)
                            .count();
        std::fprintf(g_log, "[%7lld] ", static_cast<long long>(ms));
        write_line(g_log, lvl, category, message);
    }
}

lx::session::logind_session g_session{};
bool g_have_session = false;
unsigned g_open_ok = 0;
unsigned g_open_failed = 0;

int open_device(const char* path, int flags, void*) {
    if (g_have_session) {
        if (auto taken = g_session.take_device_path(path); taken) {
            ++g_open_ok;
            log_probe.info("  open  {:<24} via logind TakeDevice", path);
            return std::move(taken).value().release();
        }
        log_probe.info("  open  {:<24} logind TakeDevice refused, trying direct", path);
    }
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        ++g_open_failed;
        log_probe.info("  open  {:<24} FAILED: {}", path, std::strerror(errno));
        return -errno;
    }
    ++g_open_ok;
    log_probe.info("  open  {:<24} direct", path);
    return fd;
}

void close_device(int fd, void*) { ::close(fd); }

const char* button_name(lx::input::button b) {
    switch (b) {
    case lx::input::button::left: return "left";
    case lx::input::button::middle: return "middle";
    case lx::input::button::right: return "right";
    case lx::input::button::side: return "side";
    case lx::input::button::extra: return "extra";
    default: return "other";
    }
}

struct probe_state {
    lx::input::input_manager* mgr = nullptr;
    unsigned keys = 0;
    unsigned buttons = 0;
    unsigned motions = 0;
    unsigned axes = 0;
    unsigned keyboards = 0;
    unsigned pointers = 0;
};

void on_device(const char* name, bool keyboard, bool pointer, bool touch, bool gesture,
               void* user) {
    auto* st = static_cast<probe_state*>(user);
    if (keyboard) ++st->keyboards;
    if (pointer) ++st->pointers;
    log_probe.info("device  {:<40}{}{}{}{}", name ? name : "(unnamed)", keyboard ? " keyboard" : "",
                   pointer ? " pointer" : "", touch ? " touch" : "", gesture ? " gesture" : "");
}

void on_key(const lx::input::key_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->keys;
    auto& km = st->mgr->default_seat().keymap();
    char utf8[8]{};
    (void)km.utf8(e.evdev_keycode, utf8, sizeof(utf8));
    const auto mods = km.named();
    log_probe.info("key     {:<8} code={:<3} sym=0x{:04x} utf8='{}'{}{}{}{}",
                   e.pressed ? "press" : "release", e.evdev_keycode, km.keysym(e.evdev_keycode),
                   utf8[0] >= 0x20 ? utf8 : "", mods.ctrl ? " ctrl" : "", mods.alt ? " alt" : "",
                   mods.shift ? " shift" : "", mods.super ? " super" : "");
}

void on_modifiers(const lx::input::modifier_state& m, void* user) {
    (void)user;
    log_probe.info("mods    depressed=0x{:x} latched=0x{:x} locked=0x{:x} group={}", m.depressed,
                   m.latched, m.locked, m.group);
}

void on_motion(const lx::input::pointer_motion_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->motions;
    // One line per motion would drown everything else; the running count still tells the
    // story, and the timestamps show whether they are arriving smoothly.
    if (st->motions % 20 == 1)
        log_probe.info("motion  x={:.1f} y={:.1f} ({} so far)", e.x, e.y, st->motions);
}

void on_button(const lx::input::pointer_button_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->buttons;
    log_probe.info("button  {:<8} {}", e.pressed ? "press" : "release", button_name(e.which));
}

void on_axis(const lx::input::pointer_axis_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->axes;
    log_probe.info("scroll  v={:.2f} h={:.2f} discrete_v={:.0f}", e.vertical, e.horizontal,
                   e.vertical_discrete);
}

/// Group membership decides whether a direct open can work at all, and it is the most
/// common reason this fails — so it is recorded rather than left to be asked about.
void report_identity() {
    const auto* pw = ::getpwuid(::getuid());
    log_probe.info("uid={} gid={}{}{}", ::getuid(), ::getgid(), pw ? " user=" : "",
                   pw ? pw->pw_name : "");

    gid_t groups[64];
    const int n = ::getgroups(64, groups);
    bool in_input = false;
    char names[1024] = "";
    char* at = names;
    for (int i = 0; i < n; ++i) {
        if (const auto* gr = ::getgrgid(groups[i])) {
            at = std::format_to_n(at, names + sizeof(names) - 1 - at, " {}", gr->gr_name).out;
            if (std::strcmp(gr->gr_name, "input") == 0)
                in_input = true;
        }
    }
    *at = '\0';
    log_probe.info("groups:{}", names);
    log_probe.info("in 'input' group: {}", in_input ? "yes" : "NO");
}

/// The device nodes and their permissions, independent of libinput. If these are all
/// root:input 0660 and the user is not in that group, nothing else matters.
void report_device_nodes() {
    DIR* dir = ::opendir("/dev/input");
    if (!dir) {
        log_probe.info("/dev/input: cannot list: {}", std::strerror(errno));
        return;
    }
    log_probe.info("/dev/input nodes:");
    unsigned readable = 0;
    unsigned total = 0;
    while (dirent* e = ::readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0)
            continue;
        ++total;
        char path[256];
        std::snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        struct stat st{};
        if (::stat(path, &st) != 0)
            continue;
        const char* owner = "?";
        const char* group = "?";
        if (const auto* pw = ::getpwuid(st.st_uid))
            owner = pw->pw_name;
        if (const auto* gr = ::getgrgid(st.st_gid))
            group = gr->gr_name;
        const bool can_read = ::access(path, R_OK) == 0;
        if (can_read)
            ++readable;
        log_probe.info("  {:<22} {}:{:<8} {:04o}  {}", path, owner, group,
                       static_cast<unsigned>(st.st_mode & 07777),
                       can_read ? "readable" : "NOT readable");
    }
    ::closedir(dir);
    log_probe.info("  {} nodes, {} readable by this process", total, readable);
}

} // namespace

int main(int argc, char* argv[]) {
    int seconds = 10;
    const char* log_path = "lumen-input-probe.log";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else if (argv[i][0] != '-')
            seconds = std::atoi(argv[i]);
    }
    if (seconds <= 0)
        seconds = 10;

    g_start = std::chrono::steady_clock::now();
    g_log = std::fopen(log_path, "w");
    lx::trace::logger::global().set_sink(probe_sink);
    // The report is the point: LUMEN_LOG=warn must not hide it.
    if (!log_probe.enabled(lx::trace::level::info))
        lx::trace::logger::global().set_min_level(lx::trace::level::info);
    if (!g_log)
        log_probe.warn("WARN: cannot write {}: {}", log_path, std::strerror(errno));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::time_t now = std::time(nullptr);
    char stamp[64]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log_probe.info("lumen-input-probe  {}  listening {}s  log={}", stamp, seconds, log_path);

#if defined(LUMEN_HAS_INPUT)
    log_probe.info("built with LUMEN_HAS_INPUT: yes");
#else
    // Without this the module is stubs and nothing can ever arrive, which looks identical
    // to a permission problem from the outside.
    log_probe.info(
        "built with LUMEN_HAS_INPUT: NO — libinput or xkbcommon was missing at configure\n"
        "  time, so lx.input is compiled out entirely. Install them and reconfigure.");
#endif

    log_probe.info("\n-- identity --------------------------------------------------");
    report_identity();
    log_probe.info("\n-- device nodes ----------------------------------------------");
    report_device_nodes();

    log_probe.info("\n-- session ---------------------------------------------------");
    if (auto session = lx::session::logind_session::open(); session) {
        g_session = std::move(session).value();
        g_have_session = true;
        log_probe.info("logind session: yes (devices requested via TakeDevice)");
    } else {
        log_probe.info("logind session: no — falling back to opening /dev/input/* directly,\n"
                       "  which needs root or membership of the 'input' group.");
    }

    log_probe.info("\n-- opening devices -------------------------------------------");
    lx::input::device_provider provider{};
    provider.open_device = open_device;
    provider.close_device = close_device;

    auto opened = lx::input::input_manager::open(provider);
    if (!opened) {
        log_probe.info("\nFAILED to open a seat: {}", opened.get_error().message);
        log_probe.info("opens: {} succeeded, {} failed", g_open_ok, g_open_failed);
        log_probe.info(
            "\nMost likely causes, in order:\n"
            "  1. Not in the 'input' group and logind declined. Fix with\n"
            "     'sudo usermod -aG input $USER', then log out and back in —\n"
            "     group changes apply only to new logins.\n"
            "  2. This session is not the active one on seat0. logind hands devices only\n"
            "     to the active session, so a detached or remote shell is refused even\n"
            "     when loginctl shows an active session elsewhere.\n"
            "  3. No input devices on the seat.");
        if (g_log)
            std::fclose(g_log);
        return 1;
    }

    auto mgr = std::move(opened).value();
    probe_state state{};
    state.mgr = &mgr;

    lx::input::event_sink sink{};
    sink.user = &state;
    sink.key = on_key;
    sink.modifiers = on_modifiers;
    sink.pointer_motion = on_motion;
    sink.pointer_button = on_button;
    sink.pointer_axis = on_axis;
    sink.device_added = on_device;
    mgr.set_sink(sink);

    log_probe.info("\n-- keymap ----------------------------------------------------");
    auto& km = mgr.default_seat().keymap();
    if (km.valid()) {
        unsigned size = 0;
        if (auto fd = km.keymap_fd(size); fd)
            log_probe.info("keymap compiled: yes ({} bytes, sent to clients as a sealed memfd)",
                           size);
        else
            log_probe.info("keymap compiled: yes, but export failed: {}", fd.get_error().message);
    } else {
        log_probe.info("keymap compiled: NO — keys would arrive with no meaning attached.");
    }

    log_probe.info("\n-- devices ---------------------------------------------------");
    // Devices arrive as events on the first dispatch, so the sink has to be installed
    // before it or the enumeration is silent.
    (void)mgr.dispatch();
    log_probe.info("total={} keyboard={} pointer={}", mgr.device_count(), state.keyboards,
                   state.pointers);
    if (mgr.device_count() == 0)
        log_probe.info("  none on this seat — nothing will be reported.");
    else if (state.keyboards == 0)
        log_probe.info("  no keyboard among them — typing cannot be reported.");

    const int fd = mgr.fd();
    if (fd < 0) {
        log_probe.info("FAILED: libinput has no fd");
        if (g_log)
            std::fclose(g_log);
        return 1;
    }

    log_probe.info("\n-- events (type, move the mouse, scroll; Ctrl-C to stop) ------");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 200) > 0)
            (void)mgr.dispatch();
    }

    log_probe.info("\n-- summary ---------------------------------------------------");
    log_probe.info("keys={} buttons={} motion={} scroll={} devices={} opens_ok={} opens_failed={}",
                   state.keys, state.buttons, state.motions, state.axes, mgr.device_count(),
                   g_open_ok, g_open_failed);

    int rc = 0;
    if (mgr.device_count() == 0) {
        // Telling these apart is the entire reason this tool exists, so it must not guess:
        // no devices is a permission or seat problem, and pointing at routing would send
        // you to the wrong place.
        log_probe.info("VERDICT: no devices opened. This is a permission or seat problem, not a\n"
                       "compositor one — see the causes under 'opening devices' above.");
        rc = 2;
    } else if (state.keys == 0 && state.buttons == 0 && state.motions == 0) {
        log_probe.info(
            "VERDICT: devices opened but nothing arrived. Either the devices listed above\n"
            "are not the ones you are using, or nothing was touched.");
        rc = 2;
    } else {
        log_probe.info("VERDICT: the input stack works. Anything still broken is above lx.input —\n"
                       "routing to clients, focus, or the client itself.");
    }

    log_probe.info("\nlog written to {}", log_path);
    if (g_log)
        std::fclose(g_log);
    return rc;
}
