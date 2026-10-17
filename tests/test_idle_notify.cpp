// Idle tracking (`lx.compositor:idle`): ext-idle-notify's idled/resumed alternation, the lazy
// timer (activity never re-arms it while nothing is idle; one firing idles everything due),
// zero timeouts, idle inhibitors (no resume when inhibited while idle; obeying timers restart
// when inhibition ends; v2 input-only notifications ignore it), per-client and session caps —
// and lumen-shell's idle policy (`lx.shell:idle_policy`). Pure: no Wayland.
#include "lumen_test.hpp"

#include <cstdint>
#include <string>
#include <vector>

import lx.foundation;
import lx.compositor;
import lx.shell;

using lx::compositor::idle_sink;
using lx::compositor::idle_tracker;
using lx::compositor::inhibitor_set;

namespace {

constexpr unsigned long long ms = 1'000'000ull;
constexpr unsigned long long t0 = 1'000'000'000ull; // an arbitrary monotonic start

struct event {
    std::string kind;
    int id = 0;
};

/// Records every event and checks the protocol's alternation per notification: never two
/// `idled` (or two `resumed`) in a row, and no `resumed` before an `idled`.
struct recorder {
    std::vector<event> events;
    std::vector<int> state = std::vector<int>(256, 0); // 0 never, 1 idle, 2 resumed
    bool violated = false;

    idle_sink sink() {
        idle_sink k{};
        k.user = this;
        k.idled = [](void* u, void* r) {
            auto* self = static_cast<recorder*>(u);
            const int id = static_cast<int>(reinterpret_cast<std::uintptr_t>(r));
            if (self->state[id] == 1)
                self->violated = true;
            self->state[id] = 1;
            self->events.push_back({"idled", id});
        };
        k.resumed = [](void* u, void* r) {
            auto* self = static_cast<recorder*>(u);
            const int id = static_cast<int>(reinterpret_cast<std::uintptr_t>(r));
            if (self->state[id] != 1)
                self->violated = true;
            self->state[id] = 2;
            self->events.push_back({"resumed", id});
        };
        return k;
    }
    [[nodiscard]] unsigned count(const char* kind, int id) const {
        unsigned n = 0;
        for (const auto& e : events)
            if (e.kind == kind && e.id == id)
                ++n;
        return n;
    }
    void clear() { events.clear(); }
};

void* res(int id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)); }
void* client(int id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + id)); }

} // namespace

LUMEN_TEST(idle_after_timeout_and_resume_on_activity) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 3000, true, t0, r.sink()));
    LUMEN_CHECK(t.next_deadline() == t0 + 3000 * ms);
    t.on_timer(t0 + 2999 * ms, r.sink());
    LUMEN_CHECK(r.events.empty());
    t.on_timer(t0 + 3000 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1);
    LUMEN_CHECK(t.next_deadline() == 0); // idle: nothing more is due
    // Activity resumes it and asks for the timer again.
    LUMEN_CHECK(t.note_activity(t0 + 5000 * ms, r.sink()));
    LUMEN_CHECK(r.count("resumed", 1) == 1);
    LUMEN_CHECK(t.next_deadline() == t0 + 8000 * ms);
    LUMEN_CHECK(!r.violated);
}

LUMEN_TEST(lazy_timer_activity_does_not_rearm) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 1000, true, t0, r.sink()));
    // A burst of motion: nothing is idle, so nothing to re-arm (the armed timer fires early).
    for (int i = 1; i <= 100; ++i)
        LUMEN_CHECK(!t.note_activity(t0 + static_cast<unsigned long long>(i) * 5 * ms, r.sink()));
    // The early firing idles nothing and names the moved deadline.
    t.on_timer(t0 + 1000 * ms, r.sink());
    LUMEN_CHECK(r.events.empty());
    LUMEN_CHECK(t.next_deadline() == t0 + 500 * ms + 1000 * ms);
    t.on_timer(t0 + 1500 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1);
}

LUMEN_TEST(many_notifications_one_timer) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 300, true, t0, r.sink()));
    LUMEN_CHECK(t.add(res(2), client(1), 100, true, t0, r.sink()));
    LUMEN_CHECK(t.add(res(3), client(2), 200, false, t0, r.sink()));
    LUMEN_CHECK(t.next_deadline() == t0 + 100 * ms);
    t.on_timer(t0 + 100 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 2) == 1 && r.events.size() == 1);
    LUMEN_CHECK(t.next_deadline() == t0 + 200 * ms);
    // A late firing idles everything due at once.
    t.on_timer(t0 + 400 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1 && r.count("idled", 3) == 1);
    LUMEN_CHECK(t.idle_count() == 3);
    LUMEN_CHECK(t.note_activity(t0 + 500 * ms, r.sink()));
    LUMEN_CHECK(r.count("resumed", 1) == 1 && r.count("resumed", 2) == 1 &&
                r.count("resumed", 3) == 1);
    LUMEN_CHECK(t.idle_count() == 0);
    t.remove(res(2));
    LUMEN_CHECK(t.count() == 2);
    LUMEN_CHECK(t.next_deadline() == t0 + 700 * ms);
    LUMEN_CHECK(!r.violated);
}

LUMEN_TEST(zero_timeout_idles_at_once_and_after_every_resume) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 0, true, t0, r.sink()));
    LUMEN_CHECK(r.count("idled", 1) == 1); // in the same dispatch, no timer round trip
    LUMEN_CHECK(t.next_deadline() == 0);
    (void)t.note_activity(t0 + 10 * ms, r.sink());
    LUMEN_CHECK(r.count("resumed", 1) == 1 && r.count("idled", 1) == 2);
    LUMEN_CHECK(t.is_idle(res(1)));
    LUMEN_CHECK(!r.violated);
}

LUMEN_TEST(inhibited_while_idle_does_not_resume) {
    // zwp_idle_inhibitor_v1: "isn't honored if the system was already idled at the time".
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 1000, true, t0, r.sink()));
    t.on_timer(t0 + 1000 * ms, r.sink());
    LUMEN_CHECK(t.is_idle(res(1)));
    LUMEN_CHECK(t.set_inhibited(true, t0 + 1500 * ms));
    LUMEN_CHECK(r.count("resumed", 1) == 0);
    LUMEN_CHECK(t.is_idle(res(1)));
    // The next activity resumes it; while inhibited it has no deadline.
    (void)t.note_activity(t0 + 2000 * ms, r.sink());
    LUMEN_CHECK(r.count("resumed", 1) == 1);
    LUMEN_CHECK(t.next_deadline() == 0);
    t.on_timer(t0 + 9000 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1);
}

LUMEN_TEST(uninhibit_restarts_obeying_timers) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 1000, true, t0, r.sink()));
    LUMEN_CHECK(t.add(res(2), client(1), 1000, false, t0, r.sink())); // input-only (v2)
    LUMEN_CHECK(t.set_inhibited(true, t0 + 100 * ms));
    LUMEN_CHECK(!t.set_inhibited(true, t0 + 200 * ms)); // no edge
    // The input-only one idles regardless of the inhibitor.
    LUMEN_CHECK(t.next_deadline() == t0 + 1000 * ms);
    t.on_timer(t0 + 1000 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 2) == 1 && r.count("idled", 1) == 0);
    // Inhibition ends at 5 s: the obeying one counts from there, not from its creation.
    LUMEN_CHECK(t.set_inhibited(false, t0 + 5000 * ms));
    LUMEN_CHECK(t.next_deadline() == t0 + 6000 * ms);
    t.on_timer(t0 + 6000 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1);
    LUMEN_CHECK(!r.violated);
}

LUMEN_TEST(zero_timeout_held_by_inhibitor) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.set_inhibited(true, t0));
    LUMEN_CHECK(t.add(res(1), client(1), 0, true, t0, r.sink()));
    LUMEN_CHECK(r.events.empty());
    LUMEN_CHECK(t.set_inhibited(false, t0 + 50 * ms));
    LUMEN_CHECK(t.next_deadline() == t0 + 50 * ms);
    t.on_timer(t0 + 50 * ms, r.sink());
    LUMEN_CHECK(r.count("idled", 1) == 1);
}

LUMEN_TEST(caps_per_client_and_session) {
    idle_tracker t{};
    recorder r{};
    for (int i = 0; i < 16; ++i)
        LUMEN_CHECK(t.add(res(i + 1), client(1), 1000, true, t0, r.sink()));
    LUMEN_CHECK(!t.add(res(100), client(1), 1000, true, t0, r.sink())); // the client's share
    LUMEN_CHECK(t.add(res(100), client(2), 1000, true, t0, r.sink())); // another client fits
    int id = 120;
    for (int c = 3; t.count() < idle_tracker::k_max; ++c)
        for (int i = 0; i < 16 && t.count() < idle_tracker::k_max; ++i)
            LUMEN_CHECK(t.add(res(id++), client(c), 1000, true, t0, r.sink()));
    LUMEN_CHECK(!t.add(res(250), client(99), 1000, true, t0, r.sink())); // the session's table
    t.remove(res(1));
    LUMEN_CHECK(t.add(res(250), client(99), 1000, true, t0, r.sink()));
}

LUMEN_TEST(remove_keeps_idle_count) {
    idle_tracker t{};
    recorder r{};
    LUMEN_CHECK(t.add(res(1), client(1), 0, true, t0, r.sink()));
    LUMEN_CHECK(t.add(res(2), client(1), 0, true, t0, r.sink()));
    LUMEN_CHECK(t.idle_count() == 2);
    t.remove(res(1));
    LUMEN_CHECK(t.idle_count() == 1);
    t.remove(res(1)); // twice: nothing
    LUMEN_CHECK(t.count() == 1);
    (void)t.note_activity(t0 + ms, r.sink());
    LUMEN_CHECK(r.count("resumed", 1) == 0 && r.count("resumed", 2) == 1);
}

LUMEN_TEST(inhibitor_set_visibility) {
    inhibitor_set s{};
    LUMEN_CHECK(s.add(res(1), client(1), lx::surface_id{7}));
    LUMEN_CHECK(s.add(res(2), client(1), lx::surface_id{9}));
    struct vis {
        unsigned long long shown = 0;
    } v{};
    const auto visible = [](void* u, lx::surface_id id) {
        return id.id() == static_cast<vis*>(u)->shown;
    };
    LUMEN_CHECK(!s.any(visible, &v));
    v.shown = 9;
    LUMEN_CHECK(s.any(visible, &v));
    s.remove(res(2));
    LUMEN_CHECK(!s.any(visible, &v));
    LUMEN_CHECK(s.count() == 1);
    for (int i = 0; i < 15; ++i)
        LUMEN_CHECK(s.add(res(10 + i), client(1), lx::surface_id{7}));
    LUMEN_CHECK(!s.add(res(40), client(1), lx::surface_id{7}));
}

LUMEN_TEST(shell_idle_policy) {
    using lx::shell::idle_policy;
    using lx::shell::idle_policy_config;
    idle_policy p{};
    LUMEN_CHECK(p.blank_timeout_ms(true, true) == 600'000);
    LUMEN_CHECK(p.blank_timeout_ms(false, true) == 0);
    LUMEN_CHECK(p.blank_timeout_ms(true, false) == 0);
    using lx::shell::idle_action;
    LUMEN_CHECK(p.on_idled() == idle_action::blank);
    LUMEN_CHECK(p.on_idled() == idle_action::none); // twice without resumed: nothing
    p.on_resumed();
    LUMEN_CHECK(p.on_idled() == idle_action::blank);
    idle_policy_config c{};
    c.enabled = false;
    idle_policy none{c};
    LUMEN_CHECK(none.blank_timeout_ms(true, true) == 0);
    LUMEN_CHECK(none.on_idled() == idle_action::none);
    c.enabled = true;
    c.blank_after_s = 0;
    idle_policy never{c};
    LUMEN_CHECK(never.blank_timeout_ms(true, true) == 0);
    bool enabled = false;
    LUMEN_CHECK(lx::shell::parse_idle_policy("shell", enabled) && enabled);
    LUMEN_CHECK(lx::shell::parse_idle_policy("none", enabled) && !enabled);
    LUMEN_CHECK(!lx::shell::parse_idle_policy("sometimes", enabled));
    unsigned s = 0;
    LUMEN_CHECK(lx::shell::parse_idle_seconds("90", s) && s == 90);
    LUMEN_CHECK(lx::shell::parse_idle_seconds("0", s) && s == 0);
    LUMEN_CHECK(!lx::shell::parse_idle_seconds("-1", s));
    LUMEN_CHECK(!lx::shell::parse_idle_seconds("10s", s));
    LUMEN_CHECK(!lx::shell::parse_idle_seconds("", s));
}

LUMEN_TEST(shell_lock_policy) {
    using lx::shell::idle_action;
    using lx::shell::idle_policy;
    using lx::shell::idle_policy_config;
    using lx::shell::shell_lock;
    // No lock command: blanking only; lock requests are ignored.
    idle_policy plain{};
    LUMEN_CHECK(!plain.can_lock());
    LUMEN_CHECK(!plain.on_lock_requested());
    LUMEN_CHECK(plain.on_idled() == idle_action::blank);
    LUMEN_CHECK(plain.relaunch_delay(0) == -1);

    idle_policy_config c{};
    c.lock_command = "swaylock -f";
    idle_policy p{c};
    LUMEN_CHECK(p.can_lock());
    // Lock on blank: the blank timeout locks first, and blanks once locked — not at locking.
    LUMEN_CHECK(p.on_idled() == idle_action::lock_then_blank);
    LUMEN_CHECK(!p.on_lock_state(shell_lock::locking));
    LUMEN_CHECK(p.on_lock_state(shell_lock::locked));
    LUMEN_CHECK(!p.on_lock_state(shell_lock::locked)); // only once
    // Locked: the shorter timeout applies; another lock request is not needed.
    LUMEN_CHECK(p.locked_timeout_ms(true, true) == 60'000);
    LUMEN_CHECK(p.locked_timeout_ms(false, true) == 0);
    LUMEN_CHECK(p.on_locked_idled());
    LUMEN_CHECK(!p.on_lock_requested());
    p.on_resumed();
    // Idle again while locked: just blank.
    LUMEN_CHECK(p.on_idled() == idle_action::blank);
    LUMEN_CHECK(!p.on_lock_state(shell_lock::unlocked));
    LUMEN_CHECK(p.locked_timeout_ms(true, true) == 0);
    LUMEN_CHECK(!p.on_locked_idled());
    p.on_resumed();
    // The user came back before the locker locked: no blank when it does.
    LUMEN_CHECK(p.on_idled() == idle_action::lock_then_blank);
    p.on_resumed();
    LUMEN_CHECK(!p.on_lock_state(shell_lock::locked));
    LUMEN_CHECK(!p.on_lock_state(shell_lock::unlocked));

    // Without lock-on-blank the timeout only blanks.
    c.lock_on_blank = false;
    idle_policy dark{c};
    LUMEN_CHECK(dark.on_idled() == idle_action::blank);

    // Relaunch: at once the first time, then at most once per 5 s, four starts in all.
    idle_policy r{idle_policy_config{.lock_command = "locker"}};
    LUMEN_CHECK(r.on_lock_requested());
    LUMEN_CHECK(r.relaunch_delay(0) == 0);
    constexpr long long s5 = idle_policy::k_relaunch_spacing_ns;
    r.note_locker_started(1'000'000'000LL);
    (void)r.on_lock_state(shell_lock::locked);
    (void)r.on_lock_state(shell_lock::abandoned);
    LUMEN_CHECK(r.relaunch_delay(2'000'000'000LL) == s5 - 1'000'000'000LL);
    LUMEN_CHECK(r.relaunch_delay(1'000'000'000LL + s5) == 0);
    r.note_locker_started(10'000'000'000LL);
    r.note_locker_started(20'000'000'000LL);
    r.note_locker_started(30'000'000'000LL);
    LUMEN_CHECK(r.locker_starts() == 4);
    LUMEN_CHECK(r.relaunch_delay(40'000'000'000LL) == -1);
    // An unlock starts the count again.
    (void)r.on_lock_state(shell_lock::unlocked);
    LUMEN_CHECK(r.locker_starts() == 0);
    LUMEN_CHECK(r.relaunch_delay(41'000'000'000LL) == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
