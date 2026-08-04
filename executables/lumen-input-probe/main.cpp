// Prints what the input stack sees, and nothing else.
//
// The point is isolation. "Typing does nothing" can mean no permission to open devices, no
// devices on the seat, a keymap that failed to compile, events that arrive but are not
// routed, or a client that never bound a keyboard. Running the whole compositor tests all
// of those at once and tells you which failed only by omission.
//
// This exercises lx.input alone: device open, libinput dispatch, xkb translation. If it
// prints keystrokes, everything below the compositor works and any remaining problem is
// routing. If it does not, the message says why.
//
//   ./lumen-input-probe          # 10 seconds, then exits
//   ./lumen-input-probe 60       # 60 seconds
//
// Ctrl-C also exits. Needs permission to open /dev/input/* — see the diagnostics it prints
// when it cannot.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

import lx.foundation;
import lx.input;
import lx.session;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

lx::session::logind_session g_session{};
bool g_have_session = false;

int open_device(const char* path, int flags, void*) {
    if (g_have_session) {
        if (auto taken = g_session.take_device_path(path); taken)
            return std::move(taken).value().release();
    }
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        std::printf("  cannot open %s: %s\n", path, std::strerror(errno));
        return -errno;
    }
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
};

void on_key(const lx::input::key_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->keys;
    auto& km = st->mgr->default_seat().keymap();
    char utf8[8]{};
    (void)km.utf8(e.evdev_keycode, utf8, sizeof(utf8));
    const auto mods = km.named();

    std::printf("key   %-8s code=%-3u sym=0x%04x utf8='%s'%s%s%s%s\n",
                e.pressed ? "press" : "release", e.evdev_keycode, km.keysym(e.evdev_keycode),
                utf8[0] >= 0x20 ? utf8 : "", mods.ctrl ? " ctrl" : "",
                mods.alt ? " alt" : "", mods.shift ? " shift" : "",
                mods.super ? " super" : "");
    std::fflush(stdout);
}

void on_modifiers(const lx::input::modifier_state& m, void* user) {
    auto* st = static_cast<probe_state*>(user);
    (void)st;
    std::printf("mods  depressed=0x%x latched=0x%x locked=0x%x group=%u\n", m.depressed,
                m.latched, m.locked, m.group);
    std::fflush(stdout);
}

void on_motion(const lx::input::pointer_motion_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->motions;
    // One line per motion event would drown everything else.
    if (st->motions % 20 == 1) {
        std::printf("motion x=%.1f y=%.1f (%u events)\n", e.x, e.y, st->motions);
        std::fflush(stdout);
    }
}

void on_button(const lx::input::pointer_button_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->buttons;
    std::printf("button %-8s %s\n", e.pressed ? "press" : "release", button_name(e.which));
    std::fflush(stdout);
}

void on_axis(const lx::input::pointer_axis_event& e, void* user) {
    auto* st = static_cast<probe_state*>(user);
    ++st->axes;
    std::printf("scroll v=%.2f h=%.2f discrete_v=%.0f\n", e.vertical, e.horizontal,
                e.vertical_discrete);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char* argv[]) {
    int seconds = 10;
    if (argc > 1)
        seconds = std::atoi(argv[1]);
    if (seconds <= 0)
        seconds = 10;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("lumen-input-probe: listening for %d seconds\n", seconds);

    if (auto session = lx::session::logind_session::open(); session) {
        g_session = std::move(session).value();
        g_have_session = true;
        std::printf("logind session: yes (devices opened via TakeDevice)\n");
    } else {
        std::printf("logind session: no — falling back to opening /dev/input/* directly.\n"
                    "  That needs root or membership of the 'input' group.\n");
    }

    lx::input::device_provider provider{};
    provider.open_device = open_device;
    provider.close_device = close_device;

    auto opened = lx::input::input_manager::open(provider);
    if (!opened) {
        std::printf("\nFAILED to open a seat: %s\n", opened.get_error().message);
        std::printf("\nMost likely causes, in order:\n"
                    "  1. This session is not the active one on seat0. logind only hands\n"
                    "     devices to the active session — switch to this TTY and retry.\n"
                    "  2. No logind session at all (ssh, container). Add yourself to the\n"
                    "     'input' group and re-login, or run as root.\n"
                    "  3. No input devices on the seat.\n");
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
    mgr.set_sink(sink);

    // Devices arrive as events on the first dispatch, so the count is only meaningful after.
    (void)mgr.dispatch();
    std::printf("devices: %u\n", mgr.device_count());
    if (mgr.device_count() == 0) {
        std::printf("  none on this seat — nothing will be reported.\n");
    }

    const int fd = mgr.fd();
    if (fd < 0) {
        std::printf("FAILED: libinput has no fd\n");
        return 1;
    }
    std::printf("\nType, move the mouse, scroll. Ctrl-C to stop.\n\n");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 200);
        if (ready > 0)
            (void)mgr.dispatch();
    }

    std::printf("\nsummary: %u keys, %u buttons, %u motion, %u scroll, %u devices\n",
                state.keys, state.buttons, state.motions, state.axes, mgr.device_count());
    if (mgr.device_count() == 0) {
        // Distinguishing these two is the entire reason this tool exists, so it must not
        // guess: no devices is a permission or seat problem, and saying anything about
        // routing here would send you to the wrong place.
        std::printf("\nNo devices were opened, so nothing could arrive. This is a\n"
                    "permission or seat problem, not a compositor one.\n\n"
                    "  Quickest fix — add yourself to the 'input' group, then log out and\n"
                    "  back in (the group is only applied to new logins):\n"
                    "      sudo usermod -aG input \"$USER\"\n\n"
                    "  Or run this from the TTY you are actually logged into. logind hands\n"
                    "  devices only to the session that is active on the seat, so a probe\n"
                    "  started from a detached or remote shell is refused even when\n"
                    "  'loginctl' shows an active session elsewhere.\n\n"
                    "  Or, to confirm the devices themselves are fine:  sudo %s\n",
                    argv[0]);
        return 2;
    }
    if (state.keys == 0 && state.buttons == 0 && state.motions == 0) {
        std::printf("Devices opened but nothing arrived — either you did not touch\n"
                    "anything, or the devices on this seat are not the ones you are using.\n");
        return 2;
    }
    std::printf("Input stack works. Anything still broken is above lx.input.\n");
    return 0;
}
