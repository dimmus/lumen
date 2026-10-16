// The per-client input serial log behind set_selection (and later xdg-activation): which
// serials a client was really sent, as compressed ranges; wraparound at 2^32; the
// newest-press queries; LRU eviction beyond the client table; forgetting a client.
#include "lumen_test.hpp"

import lx.compositor;

using lx::compositor::input_serial_kind;
using lx::compositor::input_serial_log;
using lx::compositor::serial_newer;

namespace {
// The log is ~17 KiB of fixed storage: static, not on a test's stack.
input_serial_log g_log;
int g_a = 0, g_b = 0;
void* const A = &g_a;
void* const B = &g_b;

void reset() {
    for (unsigned i = 0; i < input_serial_log::k_clients + 1; ++i)
        g_log.forget(reinterpret_cast<void*>(static_cast<unsigned long>(0x1000 + i)));
    g_log.forget(A);
    g_log.forget(B);
}
} // namespace

LUMEN_TEST(serial_newer_wraps) {
    LUMEN_CHECK(serial_newer(2, 1));
    LUMEN_CHECK(!serial_newer(1, 2));
    LUMEN_CHECK(!serial_newer(5, 5));
    LUMEN_CHECK(serial_newer(3, 0xFFFFFFF0u)); // 3 was issued after the wrap
    LUMEN_CHECK(!serial_newer(0xFFFFFFF0u, 3));
}

LUMEN_TEST(serials_only_what_was_delivered) {
    reset();
    g_log.note(A, 10, input_serial_kind::focus);
    g_log.note(A, 11, input_serial_kind::modifiers);
    g_log.note(B, 12, input_serial_kind::focus); // another client's
    g_log.note(A, 20, input_serial_kind::key_press);
    g_log.note(A, 21, input_serial_kind::key_release);
    LUMEN_CHECK(g_log.delivered(A, 10) && g_log.delivered(A, 11));
    LUMEN_CHECK(g_log.delivered(A, 20) && g_log.delivered(A, 21));
    LUMEN_CHECK(!g_log.delivered(A, 12)); // B's
    LUMEN_CHECK(g_log.delivered(B, 12));
    LUMEN_CHECK(!g_log.delivered(A, 15)); // between ranges: a configure, say
    LUMEN_CHECK(!g_log.delivered(A, 9));  // older than anything noted
    LUMEN_CHECK(!g_log.delivered(A, 22)); // not issued yet
    LUMEN_CHECK(!g_log.delivered(nullptr, 10));
    int never = 0;
    LUMEN_CHECK(!g_log.delivered(&never, 10));
}

LUMEN_TEST(serials_press_queries) {
    reset();
    LUMEN_CHECK(g_log.last_user_serial(A) == 0);
    g_log.note(A, 5, input_serial_kind::focus);
    LUMEN_CHECK(g_log.last_user_serial(A) == 0);
    g_log.note(A, 6, input_serial_kind::button_press);
    g_log.note(A, 7, input_serial_kind::button_release);
    LUMEN_CHECK(g_log.last_button_press(A) == 6 && g_log.last_user_serial(A) == 6);
    g_log.note(A, 9, input_serial_kind::key_press);
    LUMEN_CHECK(g_log.last_key_press(A) == 9 && g_log.last_button_press(A) == 6);
    LUMEN_CHECK(g_log.last_user_serial(A) == 9);
    g_log.note(A, 10, input_serial_kind::modifiers);
    LUMEN_CHECK(g_log.last_user_serial(A) == 9);
}

LUMEN_TEST(serials_ring_keeps_the_newest_ranges) {
    reset();
    // Every serial its own range (gaps of 2): only the newest k_ranges survive.
    const unsigned n = input_serial_log::k_ranges + 8;
    for (unsigned i = 0; i < n; ++i)
        g_log.note(A, 100 + 2 * i, input_serial_kind::key_press);
    for (unsigned i = 0; i < 8; ++i)
        LUMEN_CHECK(!g_log.delivered(A, 100 + 2 * i)); // evicted: rejected, not guessed
    for (unsigned i = 8; i < n; ++i)
        LUMEN_CHECK(g_log.delivered(A, 100 + 2 * i));
    // Consecutive serials compress: a long run costs one range and keeps everything.
    reset();
    g_log.note(A, 1, input_serial_kind::focus);
    for (unsigned s = 1000; s < 1000 + 500; ++s)
        g_log.note(A, s, input_serial_kind::key_press);
    LUMEN_CHECK(g_log.delivered(A, 1) && g_log.delivered(A, 1000) && g_log.delivered(A, 1499));
    LUMEN_CHECK(!g_log.delivered(A, 999) && !g_log.delivered(A, 1500));
    // The same serial noted twice (two resources of one client) is one entry.
    g_log.note(A, 1499, input_serial_kind::key_press);
    LUMEN_CHECK(g_log.delivered(A, 1499));
}

LUMEN_TEST(serials_across_wraparound) {
    reset();
    g_log.note(A, 0xFFFFFFFEu, input_serial_kind::focus);
    g_log.note(A, 0xFFFFFFFFu, input_serial_kind::key_press);
    g_log.note(A, 0u, input_serial_kind::key_release); // consecutive across the wrap
    g_log.note(A, 1u, input_serial_kind::focus);
    g_log.note(A, 5u, input_serial_kind::button_press);
    LUMEN_CHECK(g_log.delivered(A, 0xFFFFFFFEu) && g_log.delivered(A, 0xFFFFFFFFu));
    LUMEN_CHECK(g_log.delivered(A, 0u) && g_log.delivered(A, 1u) && g_log.delivered(A, 5u));
    LUMEN_CHECK(!g_log.delivered(A, 3u) && !g_log.delivered(A, 0xFFFFFFFDu) &&
                !g_log.delivered(A, 6u));
    LUMEN_CHECK(g_log.last_button_press(A) == 5u && g_log.last_key_press(A) == 0xFFFFFFFFu);
}

LUMEN_TEST(serials_lru_eviction_and_forget) {
    reset();
    g_log.note(A, 1, input_serial_kind::focus);
    // Fill the rest of the table, then one more: A is the least recently noted and goes.
    for (unsigned i = 0; i < input_serial_log::k_clients; ++i)
        g_log.note(reinterpret_cast<void*>(static_cast<unsigned long>(0x1000 + i)), 100 + i,
                   input_serial_kind::focus);
    LUMEN_CHECK(!g_log.delivered(A, 1));
    LUMEN_CHECK(g_log.delivered(reinterpret_cast<void*>(0x1000ul), 100));
    // Touching a client keeps it: B noted, then a newcomer evicts the oldest other one.
    reset();
    g_log.note(A, 1, input_serial_kind::focus);
    g_log.note(B, 2, input_serial_kind::focus);
    for (unsigned i = 0; i < input_serial_log::k_clients - 2; ++i)
        g_log.note(reinterpret_cast<void*>(static_cast<unsigned long>(0x1000 + i)), 100 + i,
                   input_serial_kind::focus);
    g_log.note(A, 3, input_serial_kind::key_press); // A is now recent
    g_log.note(reinterpret_cast<void*>(0x2000ul), 500, input_serial_kind::focus);
    LUMEN_CHECK(g_log.delivered(A, 3) && !g_log.delivered(B, 2));
    // forget: nothing of the client remains, and the pointer can be reused cleanly.
    g_log.forget(A);
    LUMEN_CHECK(!g_log.delivered(A, 3) && g_log.last_user_serial(A) == 0);
    g_log.note(A, 700, input_serial_kind::focus);
    LUMEN_CHECK(g_log.delivered(A, 700) && !g_log.delivered(A, 3));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
