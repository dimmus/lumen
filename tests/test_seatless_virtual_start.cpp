// A virtual-outputs compositor start is seat-less (the .zcode/TODO.md row "compositor::
// start() has no seat-less mode", found 2026-11-24): `logind_session::open()` and the
// real-seat `begin_input()` used to run unconditionally, so every in-process test
// compositor built with `virtual_outputs` — ~20 ctest files — took the user's logind
// session (TakeControl; a text login's `loginctl show-session 1 -p Type` read "wayland"
// for as long as the test lived, "tty" again after) and opened the seat's input devices
// through it. The fix derives the seat-less start from the configuration itself: virtual
// outputs set and not the session's own display server (`session_mode`) ⇒ no logind
// session is opened and no libinput seat is assigned.
//
// This test is the hermetic proof, and the fixture is hermetic by construction — that is
// the row's other half: once the fix holds, an in-process virtual-outputs compositor
// may run anywhere, no seat, no broker ticket (the memory-pressure cascade test and the
// other compositor fixtures inherit the same). It asserts on the started compositor's
// process only — nothing on the host is consulted, so it cannot pass or fail because of
// the machine it runs on:
//
//   no fd into /dev/input        the real seat's devices were never opened
//   no D-Bus system-bus fd       logind_session::open() never ran (it is the only
//                                system-bus client on this start path)
//   the seat's keymap is valid   a seat-less start still answers wl_keyboard (the
//                                keymap is compiled regardless of devices opening)
//
// On the unfixed tree, on a host where the user has a logind session, the first two
// fail: open() connects to the system bus before anything else, and with control taken,
// TakeDevice hands libinput the real event nodes. On a host without a session for the
// user the defect code already finds nothing — the test passes there either way; it
// pins the contract, it does not simulate the takeover.
#include "lumen_test.hpp"

#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.compositor;

namespace {

namespace rt = lx::runtime;
namespace cmp = lx::compositor;

/// Every `/proc/self/fd` entry's readlink target. Entries that vanish between the
/// readdir and the readlink (fds this process closes as it runs) are skipped.
std::vector<std::string> fd_targets() {
    std::vector<std::string> targets;
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir)
        return targets;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.')
            continue;
        char link[64];
        const char prefix[] = "/proc/self/fd/";
        if (std::size(link) <= std::size(prefix) + std::strlen(entry->d_name))
            continue;
        *std::format_to_n(link, sizeof(link) - 1, "{}{}", prefix, entry->d_name).out = '\0';
        char target[96];
        const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
        if (n <= 0)
            continue;
        target[n] = '\0';
        targets.emplace_back(target, static_cast<std::size_t>(n));
    }
    ::closedir(dir);
    return targets;
}

/// An fd of this process whose readlink says `/dev/input/...`, or "".
std::string open_input_fd() {
    for (const std::string& target : fd_targets())
        if (target.starts_with("/dev/input/"))
            return target;
    return "";
}

/// True when this process holds an established connection whose peer is the D-Bus system
/// bus. /proc/net/unix cannot show that: only a *bound* socket's row carries its path —
/// a connected client's row has none — but the kernel names the peer to the client
/// itself: getpeername() on the client returns the address the server bound
/// (/run/dbus/system_bus_socket). That connection exists only where something talked to
/// logind.
bool holds_system_bus() {
    // The sockets this process holds (`socket:[N]` in /proc/self/fd); only they can be
    // connected. Other fds — the DIR this loop reads among them, its own readlink says
    // "/proc/self/fd" — are skipped by the prefix. fds that vanish between the readdir
    // and the readlink are skipped, as in fd_targets().
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir)
        return false;
    bool holds = false;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.')
            continue;
        char link[64];
        const char prefix[] = "/proc/self/fd/";
        if (std::size(link) <= std::size(prefix) + std::strlen(entry->d_name))
            continue;
        *std::format_to_n(link, sizeof(link) - 1, "{}{}", prefix, entry->d_name).out = '\0';
        char target[96];
        const ssize_t linked = ::readlink(link, target, sizeof(target) - 1);
        if (linked <= 0)
            continue;
        if (!std::string_view{target, static_cast<std::size_t>(linked)}.starts_with("socket:["))
            continue;
        int fd = 0;
        const char* digits = entry->d_name;
        const char* end = digits + std::strlen(digits);
        if (auto [ptr, ec] = std::from_chars(digits, end, fd);
            ec != std::errc{} || ptr != end || fd < 0)
            continue;
        sockaddr_un peer{};
        socklen_t len = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0)
            continue; // not a socket we are connected through
        if (peer.sun_family != AF_UNIX)
            continue; // an inet socket's peer is not the bus
        // The path may fill sun_path with no room for its NUL; make it a string.
        char path[sizeof(peer.sun_path) + 1]{};
        const std::size_t path_len =
            len > sizeof(peer.sun_family)
                ? std::min<std::size_t>(len - sizeof(peer.sun_family), sizeof(peer.sun_path))
                : 0;
        std::memcpy(path, peer.sun_path, path_len);
        if (std::string_view{path}.find("system_bus_socket") != std::string_view::npos) {
            holds = true;
            break;
        }
    }
    ::closedir(dir);
    return holds;
}

/// The compositor every in-process fixture here is shaped like: one virtual output, the GL
/// present path asked for (as the cascade test asks, for the same reason — without a card
/// it has nothing to run on and the compositor stays on its default device).
struct started_compositor {
    rt::affinity saved = rt::current_affinity();
    std::unique_ptr<cmp::compositor> comp;

    started_compositor() {
        cmp::config cfg{};
        cfg.socket_name = "lumen-test-seatless";
        cfg.target_fps = 30.0;
        cfg.present = cmp::present_backend::gl;
        cfg.virtual_outputs = "640x480";
        comp = std::make_unique<cmp::compositor>(cfg);
        const auto started = comp->start();
        LUMEN_CHECK(started.has_value());
        const cmp::output_manager& outputs = comp->outputs();
        LUMEN_CHECK(outputs.count() == 1);
        LUMEN_CHECK(outputs.state(outputs.nth(0)).virtual_output);
    }
    ~started_compositor() {
        comp.reset();
        rt::set_current_affinity(saved);
    }
    started_compositor(const started_compositor&) = delete;
    started_compositor& operator=(const started_compositor&) = delete;
};

LUMEN_TEST(virtual_outputs_start_takes_no_session_and_no_input_seat) {
    started_compositor fixture{};

    // No real seat's devices: nothing this process holds goes into /dev/input.
    LUMEN_CHECK(open_input_fd().empty());

    // No logind session taken: logind_session::open() is the only thing on this start
    // path that connects to the D-Bus system bus, and it was not run.
    LUMEN_CHECK(!holds_system_bus());

    // The seat-less start still answers keyboards: the keymap is compiled even when no
    // device will ever report through it ("a compositor that never found a keyboard still
    // has to answer it" — begin_input's contract, compositor_impl.cpp).
    LUMEN_CHECK(fixture.comp->seats().seat().keymap().valid());
}

} // namespace

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
