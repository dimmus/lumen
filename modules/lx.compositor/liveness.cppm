module;

#include <cstdio>

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.compositor:liveness;

export namespace lx::compositor {

/// What made the compositor ping a client (the `ping … reason=` log line).
enum class ping_reason : unsigned char { activate, press, key, watchdog, periodic };

[[nodiscard]] const char* ping_reason_name(ping_reason r);

/// The wire side of liveness, installed by the protocol layer (no Wayland types here).
struct liveness_ops {
    /// Sends xdg_wm_base.ping(serial) on one of the client's xdg_wm_base objects. False when
    /// the client has none (a layer-shell-only client): no ping, nothing tracked.
    bool (*send_ping)(void* user, lx::client_id client, unsigned serial) = nullptr;
    /// Next serial of the seat's serial space.
    unsigned (*next_serial)(void* user) = nullptr;
    /// Responsiveness changed. `ms`: how long the ping has gone unanswered (`on`), or how
    /// long the client was not responding (`!on`).
    void (*changed)(void* user, lx::client_id client, bool not_responding, unsigned ms) = nullptr;
    void* user = nullptr;
};

/// xdg_wm_base.ping/pong liveness. UI affinity, Wayland-free, fixed storage.
///
/// Pings go out on suspicion, not on a schedule for everyone (Mutter): when a toplevel is
/// activated, when a button or key press is delivered, when a configure goes unanswered
/// past the watchdog, and every `k_periodic_ns` to the client that has the keyboard. At
/// most one ping per client is outstanding, and pings to a client are at least
/// `k_min_interval_ns` apart, so typing costs one 12-byte event every two seconds.
///
/// A ping unanswered for the timeout marks the client not responding; it stays outstanding
/// (a hung client gets no more pings) and its pong — whenever the client catches up — marks
/// it responsive again. Never posts xdg_wm_base.error.unresponsive and never disconnects:
/// what to do about a hung client is the user's decision, in the shell.
class liveness_tracker {
public:
    static constexpr unsigned k_max = 64;
    /// Between two pings to the same client.
    static constexpr lx::runtime::clock_time k_min_interval_ns = 2'000'000'000;
    /// Ping cadence for the client with keyboard focus.
    static constexpr lx::runtime::clock_time k_periodic_ns = 10'000'000'000;
    /// Mutter's check-alive-timeout default.
    static constexpr lx::runtime::clock_time k_default_timeout_ns = 5'000'000'000;

    void set_ops(liveness_ops ops) { ops_ = ops; }
    /// 0 disables pinging altogether.
    void set_timeout_ns(lx::runtime::clock_time t) { timeout_ns_ = t < 0 ? 0 : t; }
    [[nodiscard]] lx::runtime::clock_time timeout_ns() const { return timeout_ns_; }

    /// No-op when disabled, when a ping to `client` is outstanding, or when the last one
    /// went out less than `k_min_interval_ns` ago.
    void request(lx::client_id client, lx::runtime::clock_time now, ping_reason why);
    /// xdg_wm_base.pong. A serial other than the outstanding one is ignored (the protocol
    /// has no error for it).
    void on_pong(lx::client_id client, unsigned serial, lx::runtime::clock_time now);
    /// The object ping `serial` went out on is gone (the client destroyed that
    /// xdg_wm_base), so no pong can ever answer it: the ping is dropped, and a client marked
    /// not responding is responsive again — it is evidently dispatching requests. Mutter
    /// drops a window's ping with the window. A serial other than the outstanding one is
    /// ignored.
    void cancel(lx::client_id client, unsigned serial, lx::runtime::clock_time now);
    /// Once per frame tick. O(1) unless the earliest ping deadline has passed; `focused` is
    /// the client with keyboard focus (null: none), pinged every `k_periodic_ns`.
    void tick(lx::runtime::clock_time now, lx::client_id focused);
    /// The client is gone: its entry is dropped without a `changed` call.
    void forget(lx::client_id client);

    [[nodiscard]] bool not_responding(lx::client_id client) const;
    /// The outstanding ping's serial, 0 when none.
    [[nodiscard]] unsigned outstanding(lx::client_id client) const;

private:
    struct entry {
        lx::client_id client{};
        unsigned serial = 0; // outstanding ping, 0 when none
        lx::runtime::clock_time sent_ns = 0;
        lx::runtime::clock_time last_ping_ns = 0;
        lx::runtime::clock_time last_pong_ns = 0;
        bool pinged = false; // any ping ever sent (last_ping_ns is meaningful)
        bool not_responding = false;
        lx::runtime::clock_time since_ns = 0;
        bool used = false;
    };
    [[nodiscard]] entry* find(lx::client_id client);
    [[nodiscard]] const entry* find(lx::client_id client) const;
    [[nodiscard]] entry* find_or_add(lx::client_id client, lx::runtime::clock_time now);
    void recompute_deadline();

    liveness_ops ops_{};
    lx::runtime::clock_time timeout_ns_ = k_default_timeout_ns;
    entry entries_[k_max]{};
    /// Earliest `sent_ns + timeout` over outstanding pings of responsive clients.
    lx::runtime::clock_time next_deadline_ = k_never;
    static constexpr lx::runtime::clock_time k_never = 0x7FFF'FFFF'FFFF'FFFFLL;
};

} // namespace lx::compositor

namespace {

constexpr const char* k_liveness_category = "compositor.liveness";

[[nodiscard]] bool liveness_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] unsigned to_ms(lx::runtime::clock_time ns) {
    return ns <= 0 ? 0u : static_cast<unsigned>(ns / 1'000'000);
}

} // namespace

const char* lx::compositor::ping_reason_name(ping_reason r) {
    switch (r) {
    case ping_reason::activate: return "activate";
    case ping_reason::press: return "press";
    case ping_reason::key: return "key";
    case ping_reason::watchdog: return "watchdog";
    case ping_reason::periodic: return "periodic";
    }
    return "-";
}

lx::compositor::liveness_tracker::entry*
lx::compositor::liveness_tracker::find(lx::client_id client) {
    if (!client)
        return nullptr;
    for (auto& e : entries_)
        if (e.used && e.client == client)
            return &e;
    return nullptr;
}

const lx::compositor::liveness_tracker::entry*
lx::compositor::liveness_tracker::find(lx::client_id client) const {
    return const_cast<liveness_tracker*>(this)->find(client);
}

lx::compositor::liveness_tracker::entry*
lx::compositor::liveness_tracker::find_or_add(lx::client_id client, lx::runtime::clock_time) {
    if (entry* e = find(client))
        return e;
    // A free slot, else the responsive client with no ping outstanding pinged longest ago:
    // a not-responding or awaited client is never forgotten for a new one.
    entry* victim = nullptr;
    for (auto& e : entries_) {
        if (!e.used) {
            victim = &e;
            break;
        }
        if (e.serial != 0 || e.not_responding)
            continue;
        if (!victim || e.last_ping_ns < victim->last_ping_ns)
            victim = &e;
    }
    if (!victim)
        return nullptr;
    *victim = {};
    victim->used = true;
    victim->client = client;
    return victim;
}

void lx::compositor::liveness_tracker::recompute_deadline() {
    next_deadline_ = k_never;
    for (const auto& e : entries_)
        if (e.used && e.serial != 0 && !e.not_responding && e.sent_ns + timeout_ns_ < next_deadline_)
            next_deadline_ = e.sent_ns + timeout_ns_;
}

void lx::compositor::liveness_tracker::request(lx::client_id client, lx::runtime::clock_time now,
                                               ping_reason why) {
    if (timeout_ns_ <= 0 || !client || !ops_.send_ping)
        return;
    entry* e = find_or_add(client, now);
    if (!e || e->serial != 0)
        return;
    if (e->pinged && now - e->last_ping_ns < k_min_interval_ns)
        return;
    const unsigned serial = ops_.next_serial ? ops_.next_serial(ops_.user) : 0;
    // Remembered either way: a client with no xdg_wm_base is not asked again for a while.
    e->pinged = true;
    e->last_ping_ns = now;
    if (serial == 0 || !ops_.send_ping(ops_.user, client, serial))
        return;
    e->serial = serial;
    e->sent_ns = now;
    if (now + timeout_ns_ < next_deadline_)
        next_deadline_ = now + timeout_ns_;
    if (liveness_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "ping client=%llu serial=%u reason=%s", client.id(),
                      serial, ping_reason_name(why));
        lx::trace::logger::global().log(lx::trace::level::debug, k_liveness_category, line);
    }
}

void lx::compositor::liveness_tracker::on_pong(lx::client_id client, unsigned serial,
                                               lx::runtime::clock_time now) {
    entry* e = find(client);
    if (!e || e->serial == 0 || serial != e->serial)
        return;
    const lx::runtime::clock_time rtt = now - e->sent_ns;
    e->serial = 0;
    e->last_pong_ns = now;
    if (liveness_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "pong client=%llu serial=%u rtt_ms=%u", client.id(),
                      serial, to_ms(rtt));
        lx::trace::logger::global().log(lx::trace::level::debug, k_liveness_category, line);
    }
    if (e->not_responding) {
        e->not_responding = false;
        if (ops_.changed)
            ops_.changed(ops_.user, client, false, to_ms(now - e->since_ns));
    }
}

void lx::compositor::liveness_tracker::cancel(lx::client_id client, unsigned serial,
                                              lx::runtime::clock_time now) {
    entry* e = find(client);
    if (!e || e->serial == 0 || serial != e->serial)
        return;
    const bool awaited = !e->not_responding;
    e->serial = 0;
    if (liveness_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "ping-cancel client=%llu serial=%u", client.id(),
                      serial);
        lx::trace::logger::global().log(lx::trace::level::debug, k_liveness_category, line);
    }
    if (e->not_responding) {
        e->not_responding = false;
        if (ops_.changed)
            ops_.changed(ops_.user, client, false, to_ms(now - e->since_ns));
    }
    if (awaited)
        recompute_deadline();
}

void lx::compositor::liveness_tracker::tick(lx::runtime::clock_time now, lx::client_id focused) {
    if (timeout_ns_ <= 0)
        return;
    if (focused) {
        const entry* e = find(focused);
        if (e && e->serial == 0) {
            const lx::runtime::clock_time last =
                e->last_pong_ns > e->last_ping_ns ? e->last_pong_ns : e->last_ping_ns;
            if (now - last >= k_periodic_ns)
                request(focused, now, ping_reason::periodic);
        }
        // No entry: the client was never pinged — activation pings it when it gets focus.
    }
    if (now < next_deadline_)
        return;
    for (auto& e : entries_) {
        if (!e.used || e.serial == 0 || e.not_responding || now - e.sent_ns < timeout_ns_)
            continue;
        e.not_responding = true;
        e.since_ns = e.sent_ns;
        if (ops_.changed)
            ops_.changed(ops_.user, e.client, true, to_ms(now - e.sent_ns));
    }
    recompute_deadline();
}

void lx::compositor::liveness_tracker::forget(lx::client_id client) {
    if (entry* e = find(client)) {
        const bool awaited = e->serial != 0;
        *e = {};
        if (awaited)
            recompute_deadline();
    }
}

bool lx::compositor::liveness_tracker::not_responding(lx::client_id client) const {
    const entry* e = find(client);
    return e && e->not_responding;
}

unsigned lx::compositor::liveness_tracker::outstanding(lx::client_id client) const {
    const entry* e = find(client);
    return e ? e->serial : 0u;
}
