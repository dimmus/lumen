// xdg_wm_base.ping/pong liveness on a fake clock: the rate limit, one outstanding ping per
// client, timeout → not responding, a late pong → responsive, a wrong serial ignored, the
// periodic ping of the focused client, clients without an xdg_wm_base, a ping cancelled
// with the object it went out on, forget, disabled with 0, and that nothing is ever killed
// (the tracker has no such op).

#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.compositor;

using lx::compositor::liveness_ops;
using lx::compositor::liveness_tracker;
using lx::compositor::ping_reason;

namespace {

constexpr long long k_ms = 1'000'000;
constexpr long long k_s = 1'000'000'000;

struct fake {
    unsigned serial = 100;
    struct ping {
        unsigned long long client;
        unsigned serial;
    };
    std::vector<ping> pings;
    struct change {
        unsigned long long client;
        bool on;
        unsigned ms;
    };
    std::vector<change> changes;
    /// Clients with no xdg_wm_base: send_ping fails.
    unsigned long long no_wm_base = 0;
};
fake g;

liveness_ops ops() {
    liveness_ops o{};
    o.send_ping = [](void*, lx::client_id c, unsigned serial) {
        if (c.id() == g.no_wm_base)
            return false;
        g.pings.push_back({c.id(), serial});
        return true;
    };
    o.next_serial = [](void*) { return ++g.serial; };
    o.changed = [](void*, lx::client_id c, bool on, unsigned ms) {
        g.changes.push_back({c.id(), on, ms});
    };
    return o;
}

/// A tracker with a 5 s timeout and the fake ops; the fake's record cleared.
liveness_tracker& fresh() {
    static liveness_tracker t;
    t = liveness_tracker{};
    t.set_ops(ops());
    t.set_timeout_ns(5 * k_s);
    g = fake{};
    return t;
}

const lx::client_id A{1};
const lx::client_id B{2};

} // namespace

LUMEN_TEST(liveness_one_ping_at_a_time_and_rate_limited) {
    liveness_tracker& t = fresh();
    long long now = 100 * k_s;
    t.request(A, now, ping_reason::activate);
    LUMEN_CHECK(g.pings.size() == 1 && g.pings[0].client == 1);
    const unsigned first = g.pings[0].serial;
    LUMEN_CHECK(t.outstanding(A) == first);
    // Outstanding: no second ping, whatever the reason.
    t.request(A, now + 3 * k_s, ping_reason::press);
    LUMEN_CHECK(g.pings.size() == 1);
    t.on_pong(A, first, now + 10 * k_ms);
    LUMEN_CHECK(t.outstanding(A) == 0);
    // Answered, but the last ping is < 2 s old: still nothing.
    t.request(A, now + 1500 * k_ms, ping_reason::key);
    LUMEN_CHECK(g.pings.size() == 1);
    t.request(A, now + 2 * k_s, ping_reason::key);
    LUMEN_CHECK(g.pings.size() == 2);
    // Another client is independent.
    t.request(B, now + 2 * k_s, ping_reason::press);
    LUMEN_CHECK(g.pings.size() == 3 && g.pings[2].client == 2);
    LUMEN_CHECK(g.changes.empty());
}

LUMEN_TEST(liveness_timeout_then_late_pong) {
    liveness_tracker& t = fresh();
    const long long t0 = 50 * k_s;
    t.request(A, t0, ping_reason::press);
    const unsigned serial = g.pings.back().serial;
    t.tick(t0 + 4999 * k_ms, {});
    LUMEN_CHECK(g.changes.empty() && !t.not_responding(A));
    t.tick(t0 + 5 * k_s, {});
    LUMEN_CHECK(g.changes.size() == 1 && g.changes[0].on && g.changes[0].client == 1);
    LUMEN_CHECK(g.changes[0].ms == 5000);
    LUMEN_CHECK(t.not_responding(A));
    // Still outstanding: a hung client gets no more pings, and nothing changes again.
    t.request(A, t0 + 20 * k_s, ping_reason::press);
    t.tick(t0 + 30 * k_s, A);
    LUMEN_CHECK(g.pings.size() == 1 && g.changes.size() == 1);
    // A pong with another serial is ignored.
    t.on_pong(A, serial + 7, t0 + 31 * k_s);
    LUMEN_CHECK(t.not_responding(A) && g.changes.size() == 1);
    // The client caught up: responsive, and how long it was not.
    t.on_pong(A, serial, t0 + 35 * k_s);
    LUMEN_CHECK(!t.not_responding(A));
    LUMEN_CHECK(g.changes.size() == 2 && !g.changes[1].on && g.changes[1].ms == 35000);
    // A second pong of the same serial does nothing.
    t.on_pong(A, serial, t0 + 36 * k_s);
    LUMEN_CHECK(g.changes.size() == 2);
}

LUMEN_TEST(liveness_periodic_ping_of_the_focused_client) {
    liveness_tracker& t = fresh();
    const long long t0 = 10 * k_s;
    t.request(A, t0, ping_reason::activate);
    t.on_pong(A, g.pings.back().serial, t0 + 5 * k_ms);
    t.tick(t0 + 9 * k_s, A);
    LUMEN_CHECK(g.pings.size() == 1);
    t.tick(t0 + 10 * k_s + 5 * k_ms, A); // 10 s after the pong
    LUMEN_CHECK(g.pings.size() == 2);
    // Not focused: no periodic ping.
    t.on_pong(A, g.pings.back().serial, t0 + 11 * k_s);
    t.tick(t0 + 40 * k_s, B);
    LUMEN_CHECK(g.pings.size() == 2);
    // A client never pinged is not pinged by the periodic check (activation does that).
    t.tick(t0 + 50 * k_s, B);
    LUMEN_CHECK(g.pings.size() == 2);
}

LUMEN_TEST(liveness_client_without_wm_base) {
    liveness_tracker& t = fresh();
    g.no_wm_base = 1;
    t.request(A, 10 * k_s, ping_reason::press);
    LUMEN_CHECK(g.pings.empty() && t.outstanding(A) == 0);
    t.tick(30 * k_s, A);
    LUMEN_CHECK(g.changes.empty() && !t.not_responding(A));
}

LUMEN_TEST(liveness_forget_and_disable) {
    liveness_tracker& t = fresh();
    t.request(A, 10 * k_s, ping_reason::press);
    t.forget(A);
    t.tick(30 * k_s, {});
    LUMEN_CHECK(g.changes.empty() && t.outstanding(A) == 0);
    // Pinged again from scratch after forget (a new client may reuse the id space).
    t.request(A, 30 * k_s, ping_reason::press);
    LUMEN_CHECK(g.pings.size() == 2);

    liveness_tracker& off = fresh();
    off.set_timeout_ns(0);
    off.request(A, 10 * k_s, ping_reason::activate);
    off.tick(100 * k_s, A);
    LUMEN_CHECK(g.pings.empty() && g.changes.empty());
}

LUMEN_TEST(liveness_cancel_when_the_ping_object_goes) {
    // The xdg_wm_base a ping went out on was destroyed before the pong: nothing can answer
    // that serial any more. Without cancel the client stays awaited, then not responding,
    // for good (and its pointer motion stays muted).
    liveness_tracker& t = fresh();
    const long long t0 = 20 * k_s;
    t.request(A, t0, ping_reason::press);
    const unsigned serial = g.pings.back().serial;
    t.cancel(A, serial + 1, t0 + 100 * k_ms); // another serial: ignored
    LUMEN_CHECK(t.outstanding(A) == serial);
    t.cancel(A, serial, t0 + 100 * k_ms);
    LUMEN_CHECK(t.outstanding(A) == 0);
    t.tick(t0 + 10 * k_s, {});
    LUMEN_CHECK(g.changes.empty() && !t.not_responding(A));
    // Pinged again on the next suspicion (past the rate limit).
    t.request(A, t0 + 10 * k_s, ping_reason::press);
    LUMEN_CHECK(g.pings.size() == 2 && t.outstanding(A) == g.pings.back().serial);

    // Already marked not responding: the destroy request shows the client is dispatching,
    // so it is responsive again (the `changed` op unmutes it and clears the title).
    const unsigned second = g.pings.back().serial;
    t.tick(t0 + 15 * k_s, {});
    LUMEN_CHECK(t.not_responding(A) && g.changes.size() == 1 && g.changes[0].on);
    t.cancel(A, second, t0 + 16 * k_s);
    LUMEN_CHECK(!t.not_responding(A) && t.outstanding(A) == 0);
    LUMEN_CHECK(g.changes.size() == 2 && !g.changes[1].on && g.changes[1].ms == 6000);
    // Nothing left to time out.
    t.tick(t0 + 60 * k_s, {});
    LUMEN_CHECK(g.changes.size() == 2);
}

LUMEN_TEST(liveness_table_keeps_hung_clients) {
    liveness_tracker& t = fresh();
    // Fill the table with clients that answered; one more evicts the oldest of them, never
    // a client that is awaited or not responding.
    t.request(A, 1 * k_s, ping_reason::press); // stays outstanding
    for (unsigned i = 0; i < liveness_tracker::k_max - 1; ++i) {
        const lx::client_id c{100 + i};
        t.request(c, 2 * k_s + i, ping_reason::press);
        t.on_pong(c, g.pings.back().serial, 2 * k_s + i);
    }
    t.request(lx::client_id{999}, 3 * k_s, ping_reason::press);
    LUMEN_CHECK(t.outstanding(lx::client_id{999}) != 0);
    LUMEN_CHECK(t.outstanding(A) != 0);
    t.tick(10 * k_s, {});
    LUMEN_CHECK(t.not_responding(A));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
