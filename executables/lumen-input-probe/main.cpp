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

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

import lx.foundation;
import lx.input;
import lx.session;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::FILE* g_log = nullptr;
std::chrono::steady_clock::time_point g_start{};

/// Everything goes to both the terminal and the log, so what you read is what you can
/// paste. The log carries a millisecond offset per line — event timing is often the answer.
void logf(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    std::fputs(line, stdout);
    std::fflush(stdout);
    if (g_log) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - g_start)
                            .count();
        std::fprintf(g_log, "[%7lld] %s", static_cast<long long>(ms), line);
        std::fflush(g_log);
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
            logf("  open  %-24s via logind TakeDevice\n", path);
            return std::move(taken).value().release();
        }
        logf("  open  %-24s logind TakeDevice refused, trying direct\n", path);
    }
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        ++g_open_failed;
        logf("  open  %-24s FAILED: %s\n", path, std::strerror(errno));
        return -errno;
    }
    ++g_open_ok;
    logf("  open  %-24s direct\n", path);
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

void on_device(const char* name, bool keyboard, bool pointer, bool touch, void* user) {
    auto* st = static_cast<probe_state*>(user);
    if (keyboard) ++st->keyboards;
    if (pointer) ++st->pointers;
    logf("device  %-40s%s%s%s\n", name ? name : "(unnamed)", keyboard ? " keyboard" : "",
         pointer ? " pointer" : "", touch ? " touch" : "");
}

void on_key(const lx::input::key_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->keys;
    auto& km = st->mgr->default_seat().keymap();
    char utf8[8]{};
    (void)km.utf8(e.evdev_keycode, utf8, sizeof(utf8));
    const auto mods = km.named();
    logf("key     %-8s code=%-3u sym=0x%04x utf8='%s'%s%s%s%s\n",
         e.pressed ? "press" : "release", e.evdev_keycode, km.keysym(e.evdev_keycode),
         utf8[0] >= 0x20 ? utf8 : "", mods.ctrl ? " ctrl" : "", mods.alt ? " alt" : "",
         mods.shift ? " shift" : "", mods.super ? " super" : "");
}

void on_modifiers(const lx::input::modifier_state& m, void* user) {
    (void)user;
    logf("mods    depressed=0x%x latched=0x%x locked=0x%x group=%u\n", m.depressed, m.latched,
         m.locked, m.group);
}

void on_motion(const lx::input::pointer_motion_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->motions;
    // One line per motion would drown everything else; the running count still tells the
    // story, and the timestamps show whether they are arriving smoothly.
    if (st->motions % 20 == 1)
        logf("motion  x=%.1f y=%.1f (%u so far)\n", e.x, e.y, st->motions);
}

void on_button(const lx::input::pointer_button_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->buttons;
    logf("button  %-8s %s\n", e.pressed ? "press" : "release", button_name(e.which));
}

void on_axis(const lx::input::pointer_axis_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->axes;
    logf("scroll  v=%.2f h=%.2f discrete_v=%.0f\n", e.vertical, e.horizontal,
         e.vertical_discrete);
}

/// Group membership decides whether a direct open can work at all, and it is the most
/// common reason this fails — so it is recorded rather than left to be asked about.
void report_identity() {
    logf("uid=%u gid=%u", ::getuid(), ::getgid());
    if (const auto* pw = ::getpwuid(::getuid()))
        logf(" user=%s", pw->pw_name);
    logf("\n");

    gid_t groups[64];
    const int n = ::getgroups(64, groups);
    bool in_input = false;
    logf("groups:");
    for (int i = 0; i < n; ++i) {
        if (const auto* gr = ::getgrgid(groups[i])) {
            logf(" %s", gr->gr_name);
            if (std::strcmp(gr->gr_name, "input") == 0)
                in_input = true;
        }
    }
    logf("\n");
    logf("in 'input' group: %s\n", in_input ? "yes" : "NO");
}

/// The device nodes and their permissions, independent of libinput. If these are all
/// root:input 0660 and the user is not in that group, nothing else matters.
void report_device_nodes() {
    DIR* dir = ::opendir("/dev/input");
    if (!dir) {
        logf("/dev/input: cannot list: %s\n", std::strerror(errno));
        return;
    }
    logf("/dev/input nodes:\n");
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
        logf("  %-22s %s:%-8s %04o  %s\n", path, owner, group,
             static_cast<unsigned>(st.st_mode & 07777),
             can_read ? "readable" : "NOT readable");
    }
    ::closedir(dir);
    logf("  %u nodes, %u readable by this process\n", total, readable);
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
    if (!g_log)
        std::printf("WARN: cannot write %s: %s\n", log_path, std::strerror(errno));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::time_t now = std::time(nullptr);
    char stamp[64]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    logf("lumen-input-probe  %s  listening %ds  log=%s\n", stamp, seconds, log_path);

#if defined(LUMEN_HAS_INPUT)
    logf("built with LUMEN_HAS_INPUT: yes\n");
#else
    // Without this the module is stubs and nothing can ever arrive, which looks identical
    // to a permission problem from the outside.
    logf("built with LUMEN_HAS_INPUT: NO — libinput or xkbcommon was missing at configure\n"
         "  time, so lx.input is compiled out entirely. Install them and reconfigure.\n");
#endif

    logf("\n-- identity --------------------------------------------------\n");
    report_identity();
    logf("\n-- device nodes ----------------------------------------------\n");
    report_device_nodes();

    logf("\n-- session ---------------------------------------------------\n");
    if (auto session = lx::session::logind_session::open(); session) {
        g_session = std::move(session).value();
        g_have_session = true;
        logf("logind session: yes (devices requested via TakeDevice)\n");
    } else {
        logf("logind session: no — falling back to opening /dev/input/* directly,\n"
             "  which needs root or membership of the 'input' group.\n");
    }

    logf("\n-- opening devices -------------------------------------------\n");
    lx::input::device_provider provider{};
    provider.open_device = open_device;
    provider.close_device = close_device;

    auto opened = lx::input::input_manager::open(provider);
    if (!opened) {
        logf("\nFAILED to open a seat: %s\n", opened.get_error().message);
        logf("opens: %u succeeded, %u failed\n", g_open_ok, g_open_failed);
        logf("\nMost likely causes, in order:\n"
             "  1. Not in the 'input' group and logind declined. Fix with\n"
             "     'sudo usermod -aG input $USER', then log out and back in —\n"
             "     group changes apply only to new logins.\n"
             "  2. This session is not the active one on seat0. logind hands devices only\n"
             "     to the active session, so a detached or remote shell is refused even\n"
             "     when loginctl shows an active session elsewhere.\n"
             "  3. No input devices on the seat.\n");
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

    logf("\n-- keymap ----------------------------------------------------\n");
    auto& km = mgr.default_seat().keymap();
    if (km.valid()) {
        unsigned size = 0;
        if (auto fd = km.keymap_fd(size); fd)
            logf("keymap compiled: yes (%u bytes, sent to clients as a sealed memfd)\n", size);
        else
            logf("keymap compiled: yes, but export failed: %s\n", fd.get_error().message);
    } else {
        logf("keymap compiled: NO — keys would arrive with no meaning attached.\n");
    }

    logf("\n-- devices ---------------------------------------------------\n");
    // Devices arrive as events on the first dispatch, so the sink has to be installed
    // before it or the enumeration is silent.
    (void)mgr.dispatch();
    logf("total=%u keyboard=%u pointer=%u\n", mgr.device_count(), state.keyboards,
         state.pointers);
    if (mgr.device_count() == 0)
        logf("  none on this seat — nothing will be reported.\n");
    else if (state.keyboards == 0)
        logf("  no keyboard among them — typing cannot be reported.\n");

    const int fd = mgr.fd();
    if (fd < 0) {
        logf("FAILED: libinput has no fd\n");
        if (g_log)
            std::fclose(g_log);
        return 1;
    }

    logf("\n-- events (type, move the mouse, scroll; Ctrl-C to stop) ------\n");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 200) > 0)
            (void)mgr.dispatch();
    }

    logf("\n-- summary ---------------------------------------------------\n");
    logf("keys=%u buttons=%u motion=%u scroll=%u devices=%u opens_ok=%u opens_failed=%u\n",
         state.keys, state.buttons, state.motions, state.axes, mgr.device_count(), g_open_ok,
         g_open_failed);

    int rc = 0;
    if (mgr.device_count() == 0) {
        // Telling these apart is the entire reason this tool exists, so it must not guess:
        // no devices is a permission or seat problem, and pointing at routing would send
        // you to the wrong place.
        logf("VERDICT: no devices opened. This is a permission or seat problem, not a\n"
             "compositor one — see the causes under 'opening devices' above.\n");
        rc = 2;
    } else if (state.keys == 0 && state.buttons == 0 && state.motions == 0) {
        logf("VERDICT: devices opened but nothing arrived. Either the devices listed above\n"
             "are not the ones you are using, or nothing was touched.\n");
        rc = 2;
    } else {
        logf("VERDICT: the input stack works. Anything still broken is above lx.input —\n"
             "routing to clients, focus, or the client itself.\n");
    }

    logf("\nlog written to %s\n", log_path);
    if (g_log)
        std::fclose(g_log);
    return rc;
}
