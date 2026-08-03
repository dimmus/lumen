// Fuzz the Wayland request-dispatch path.
//
// The compositor's socket is its one interface to untrusted input: every byte a client
// sends is attacker-controlled, and it reaches object lookup, opcode dispatch, argument
// unmarshalling and then Lumen's own request handlers. That boundary had no fuzzing.
//
// The harness feeds raw bytes down a real client connection rather than calling handlers
// directly, so the wire decoding — object ids, opcodes, lengths, fd counts, strings and
// arrays — is exercised exactly as libwayland will see it in production, along with
// whatever Lumen does in response.
//
// Build:  cmake -B build/fuzz -DLUMEN_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
// Run:    ./build/fuzz/fuzz_wayland_dispatch -max_total_time=60
//         ./build/fuzz/fuzz_wayland_dispatch corpus/    # replay a corpus
//
// A crash here is a compositor a client can kill, so treat any finding as release-blocking.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#if defined(LUMEN_HAS_WAYLAND)
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#endif

import lx.foundation;
import lx.wayland.server;
import lx.compositor;

#if !defined(LUMEN_HAS_WAYLAND)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }

#else

namespace {

/// One display plus its globals, torn down per input.
///
/// Rebuilding per input costs throughput, but sharing a display across inputs would let
/// state from one input decide whether the next one crashes — which makes any finding
/// unreproducible from the single file the fuzzer hands you.
struct fuzz_server {
    wl_display* display = nullptr;
    lx::compositor::p0_protocol_context p0{};
    lx::wayland::server server{};
    bool ready = false;

    fuzz_server() {
        // No socket on the filesystem: a fuzzer runs thousands of iterations a second and
        // would otherwise leave a trail of sockets and race on the name.
        if (auto bound = server.bind_socketless(); !bound)
            return;
        display = static_cast<wl_display*>(server.native_display());
        if (!display)
            return;

        p0.server = &server;
        if (auto installed = lx::compositor::install_p0_protocols(p0); !installed)
            return;
        ready = true;
    }

    // No destructor: `wayland::server` owns the display and destroys it. Destroying it
    // here as well is a double free, which presents as a SEGV deep inside libwayland and
    // looks exactly like a protocol-parsing bug until you check who owns what.

    fuzz_server(const fuzz_server&) = delete;
    fuzz_server& operator=(const fuzz_server&) = delete;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // A Wayland message is at least a header: object id plus size/opcode word.
    if (size < 8 || size > 64 * 1024)
        return 0;

    fuzz_server srv{};
    if (!srv.ready)
        return 0;

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return 0;

    // wl_client_create takes ownership of fds[0] and closes it with the client.
    wl_client* client = wl_client_create(srv.display, fds[0]);
    if (!client) {
        ::close(fds[0]);
        ::close(fds[1]);
        return 0;
    }

    // Partial writes are fine and interesting in their own right — a truncated message is
    // exactly the sort of input that finds length-handling bugs.
    (void)::write(fds[1], data, size);

    // Dispatch until the queue drains or the client is destroyed by a protocol error.
    // Bounded so a fuzz input cannot spin here forever.
    wl_event_loop* loop = wl_display_get_event_loop(srv.display);
    for (int i = 0; i < 8; ++i) {
        if (wl_event_loop_dispatch(loop, 0) < 0)
            break;
    }
    wl_display_flush_clients(srv.display);

    ::close(fds[1]);
    return 0;
}

#endif // LUMEN_HAS_WAYLAND
