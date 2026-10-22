// One frame clock per output over one shared timer (`lx.scheduler:repaint` frame_clocks,
// ADR 008 M6): two outputs at 60 and 75 Hz keep their own phases, outputs due within 1 ms
// share a tick, the shared timer is armed at the earliest start, an output's clock stops
// after its own quiet ticks while the other runs, dark outputs are never armed, and nothing
// runs (the housekeeping tick takes over) once every clock has stopped.
#include "lumen_test.hpp"

import lx.runtime;
import lx.scheduler;

using lx::scheduler::frame_clocks;

namespace {
constexpr long long k_ms = 1'000'000;
constexpr long long k_60 = 16'666'667;
constexpr long long k_75 = 13'333'333;
} // namespace

LUMEN_TEST(frame_clocks_independent_phases) {
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_75);
    LUMEN_CHECK(c.live_mask() == 0b11 && c.running_mask() == 0);
    LUMEN_CHECK(c.next_wakeup() == 0); // nothing runs: the housekeeping tick takes over
    const long long t0 = 1'000 * k_ms;
    // Each output flipped at its own time: their next starts follow their own vblanks.
    c.clock(0).on_present(t0 - 2 * k_ms);
    c.clock(1).on_present(t0 - 9 * k_ms);
    LUMEN_CHECK(c.request(0b11, t0) == 0b11);
    const long long s0 = c.start_of(0);
    const long long s1 = c.start_of(1);
    LUMEN_CHECK(s0 > t0 && s1 > t0 && s0 != s1);
    // Output 0's next vblank is at t0 - 2 ms + 16.7 ms; output 1's at t0 - 9 + 13.3.
    LUMEN_CHECK(s0 < t0 - 2 * k_ms + k_60);
    LUMEN_CHECK(s1 < t0 - 9 * k_ms + k_75);
    // The shared timer is armed at the earlier one, and only that output is due then.
    const long long first = c.next_wakeup();
    LUMEN_CHECK(first == (s0 < s1 ? s0 : s1));
    const unsigned due = c.take_due(first);
    LUMEN_CHECK(due == (s0 < s1 ? 0b01u : 0b10u));
    // Once served, an output's next start is its following vblank, not the same one.
    const unsigned served = s0 < s1 ? 0 : 1;
    LUMEN_CHECK(c.start_of(served) > first);
    // A request for a running clock keeps its schedule.
    const long long before = c.start_of(1 - served);
    LUMEN_CHECK(c.request(0b11, first) == 0);
    LUMEN_CHECK(c.start_of(1 - served) == before);
}

LUMEN_TEST(frame_clocks_coalesce_within_a_millisecond) {
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_60);
    const long long t0 = 2'000 * k_ms;
    // Flips half a millisecond apart: one tick serves both.
    c.clock(0).on_present(t0 - 4 * k_ms);
    c.clock(1).on_present(t0 - 4 * k_ms + k_ms / 2);
    (void)c.request(0b11, t0);
    LUMEN_CHECK(c.take_due(c.next_wakeup()) == 0b11);
    // Three milliseconds apart: two ticks.
    frame_clocks d{};
    d.set_live(0, true, k_60);
    d.set_live(1, true, k_60);
    d.clock(0).on_present(t0 - 4 * k_ms);
    d.clock(1).on_present(t0 - 1 * k_ms);
    (void)d.request(0b11, t0);
    LUMEN_CHECK(d.take_due(d.next_wakeup()) == 0b01);
    LUMEN_CHECK(d.take_due(d.next_wakeup()) == 0b10);
}

LUMEN_TEST(frame_clocks_flip_rephases_one_output) {
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_60);
    const long long t0 = 3'000 * k_ms;
    (void)c.request(0b11, t0);
    const long long s1 = c.start_of(1);
    // A flip of output 0 moves output 0's start only.
    c.on_present(0, t0 + 5 * k_ms, t0 + 5 * k_ms);
    LUMEN_CHECK(c.start_of(1) == s1);
    LUMEN_CHECK(c.start_of(0) > t0 + 5 * k_ms && c.start_of(0) < t0 + 5 * k_ms + k_60);
}

LUMEN_TEST(frame_clocks_quiet_stops_one_output) {
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_60);
    (void)c.request(0b11, 4'000 * k_ms);
    // Output 1 is quiet twice: it stops; output 0 keeps running.
    LUMEN_CHECK(!c.settle(1, true));
    LUMEN_CHECK(!c.settle(0, false));
    LUMEN_CHECK(c.settle(1, true));
    LUMEN_CHECK(c.running_mask() == 0b01);
    // A busy tick in between resets the count.
    LUMEN_CHECK(!c.settle(0, true));
    LUMEN_CHECK(!c.settle(0, false));
    LUMEN_CHECK(!c.settle(0, true));
    LUMEN_CHECK(c.settle(0, true));
    LUMEN_CHECK(c.running_mask() == 0 && c.next_wakeup() == 0);
}

LUMEN_TEST(frame_clocks_dark_outputs_never_run) {
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_60);
    c.set_dark(1, true);
    LUMEN_CHECK(c.request(0b11, 5'000 * k_ms) == 0b01);
    LUMEN_CHECK(c.running_mask() == 0b01);
    // Going dark stops a running clock.
    c.set_dark(0, true);
    LUMEN_CHECK(c.running_mask() == 0 && c.next_wakeup() == 0);
    c.set_dark(1, false);
    LUMEN_CHECK(c.request(0b11, 5'001 * k_ms) == 0b10);
    // An output that goes away leaves nothing behind.
    c.set_live(1, false, 0);
    LUMEN_CHECK(c.running_mask() == 0 && c.live_mask() == 0b01);
}

LUMEN_TEST(frame_clocks_flat_out_ticks_once_a_period) {
    // A repaint estimate over a period (one slow frame, a screenshot read back): the clock
    // runs flat out, which is once a refresh, not once a millisecond.
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.clock(0).observe_repaint(20'000, 2'000);
    const long long t0 = 6'000 * k_ms;
    c.clock(0).on_present(t0 - 3 * k_ms);
    (void)c.request(0b1, t0);
    long long t = c.next_wakeup();
    unsigned ticks = 0;
    for (; t < t0 + 100 * k_ms; t = c.next_wakeup())
        ticks += c.take_due(t) ? 1u : 0u;
    LUMEN_CHECK(ticks >= 5 && ticks <= 7);
}

LUMEN_TEST(frame_clocks_flips_without_vblank_never_tick_faster_than_the_refresh) {
    // A display whose flips complete when the frame lands rather than at a vblank (vmwgfx):
    // each flip re-phases the clock. With a large repaint estimate (one slow frame of 12 ms
    // in the window) the start aimed back from the next "vblank" used to come ~8 ms after
    // the previous one; ticks must stay a refresh apart.
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.clock(0).observe_repaint(11'000, 1'000);
    const long long t0 = 2'000 * k_ms;
    c.clock(0).on_present(t0 - 10 * k_ms);
    (void)c.request(0b1, t0);
    long long prev = 0, min_gap = 1'000 * k_ms;
    for (int i = 0; i < 30; ++i) {
        const long long t = c.next_wakeup();
        LUMEN_CHECK(c.take_due(t) == 0b1);
        if (prev)
            min_gap = t - prev < min_gap ? t - prev : min_gap;
        prev = t;
        // The flip lands 5 ms after the tick started (render + host), not on a vblank.
        c.on_present(0, t + 5 * k_ms, t + 5 * k_ms);
    }
    LUMEN_CHECK(min_gap >= k_60 - frame_clocks::k_coalesce_ns);
}

LUMEN_TEST(frame_clocks_adaptive_sync_starts_on_demand) {
    // With adaptive sync (ADR 008 §C.4) there is no vblank phase: a repaint starts as soon as
    // it can land one period (the mode's highest rate) after the last flip.
    frame_clocks c{};
    c.set_live(0, true, k_60);
    c.set_live(1, true, k_60);
    c.set_vrr(1, true);
    const long long t0 = 1'000 * k_ms;
    c.clock(0).on_present(t0 - 2 * k_ms);
    c.clock(1).on_present(t0 - 2 * k_ms);
    LUMEN_CHECK(c.request(0b11, t0) == 0b11);
    // The fixed-rate clock aims at its next vblank; the VRR one may start at once if it can
    // land after the minimum interval…
    const long long fixed = c.start_of(0);
    const long long vrr = c.start_of(1);
    LUMEN_CHECK(vrr <= fixed && vrr >= t0);
    // …and never sooner than one period after the last flip minus its repaint budget.
    LUMEN_CHECK(vrr >= t0 - 2 * k_ms + k_60 - c.clock(1).estimate_ns() - 2 * k_ms ||
                vrr == t0);
    // Long after the last flip (content arriving late): it starts now, not at a phase.
    frame_clocks d{};
    d.set_live(0, true, k_60);
    d.set_vrr(0, true);
    d.clock(0).on_present(t0 - 500 * k_ms);
    LUMEN_CHECK(d.request(0b1, t0) == 0b1 && d.start_of(0) == t0);
    // set_live resets the clock (a new mode): adaptive sync is set again by the owner.
    d.set_live(0, true, k_60);
    LUMEN_CHECK(!d.clock(0).vrr());
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
