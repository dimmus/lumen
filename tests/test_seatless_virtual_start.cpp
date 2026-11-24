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

/// True when this process holds a unix-socket fd whose address is the D-Bus system bus
/// (`/proc/net/unix` names each server socket's inode; an fd's readlink carries the same
/// inode as `socket:[N]`). That connection exists only where something talked to logind.
bool holds_system_bus() {
    // inode -> socket path, for the sockets that have one.
    std::unordered_map<unsigned long long, std::string> named;
    if (std::ifstream unix_table{"/proc/net/unix"}; unix_table) {
        std::string line;
        std::getline(unix_table, line); // header
        while (std::getline(unix_table, line)) {
            std::istringstream fields{line};
            std::string skipped, inode, path;
            for (int i = 0; i < 5; ++i)
                fields >> skipped;
            if (!(fields >> inode) || !(fields >> path))
                continue;
            unsigned long long n = 0;
            std::from_chars(inode.data(), inode.data() + inode.size(), n);
            if (n)
                named[n] = path;
        }
    }
    for (const std::string& target : fd_targets()) {
        if (!target.starts_with("socket:[") || !target.ends_with("]"))
            continue;
        unsigned long long inode = 0;
        const char* digits = target.data() + 8;
        const char* end = target.data() + target.size() - 1;
        if (auto [ptr, ec] = std::from_chars(digits, end, inode);
            ec != std::errc{} || ptr != end || !inode)
            continue;
        if (const auto it = named.find(inode);
            it != named.end() && it->second.find("system_bus_socket") != std::string::npos)
            return true;
    }
    return false;
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
