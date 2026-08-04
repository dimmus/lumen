#include "lumen_test.hpp"

import lx.foundation;
import lx.runtime;
import lx.scheduler;

namespace {

constexpr lx::runtime::clock_time k_60hz = 16'666'667;

[[nodiscard]] lx::scheduler::repaint_scheduler at_60hz() {
    lx::scheduler::repaint_scheduler s{};
    s.set_refresh_period(k_60hz);
    return s;
}

/// Feeds one cost repeatedly so the peak estimate settles on it.
void settle(lx::scheduler::repaint_scheduler& s, unsigned ui_us, unsigned render_us) {
    for (unsigned i = 0; i < lx::scheduler::repaint_scheduler::k_window; ++i)
        s.observe_repaint(ui_us, render_us);
}

} // namespace

// Before any frame is measured the estimate must be pessimistic, not zero: starting a
// repaint at the deadline because "it costs nothing" misses every frame until the first
// measurement lands.
LUMEN_TEST(repaint_estimate_starts_conservative) {
    auto s = at_60hz();
    LUMEN_CHECK(s.estimate_ns() > 0);
    LUMEN_CHECK(s.estimate_ns() == k_60hz / 2);
}

LUMEN_TEST(repaint_estimate_tracks_the_window_peak) {
    auto s = at_60hz();
    settle(s, 1000, 1000); // 2 ms
    LUMEN_CHECK(s.estimate_ns() == 2'000'000);

    // One expensive frame must move the estimate immediately — the whole point is to start
    // earlier *before* missing a deadline, not after.
    s.observe_repaint(5000, 3000); // 8 ms
    LUMEN_CHECK(s.estimate_ns() == 8'000'000);

    // ...and must be forgotten once it leaves the window, so one outlier does not
    // pessimize the schedule forever.
    settle(s, 1000, 1000);
    LUMEN_CHECK(s.estimate_ns() == 2'000'000);
}

// The core behavior: a cheap frame starts late (low latency), an expensive one starts
// early, and both aim at the same deadline.
LUMEN_TEST(repaint_start_moves_with_measured_cost) {
    auto cheap = at_60hz();
    settle(cheap, 500, 500); // 1 ms
    auto costly = at_60hz();
    settle(costly, 4000, 4000); // 8 ms

    const lx::runtime::clock_time now = 1'000'000'000;
    cheap.on_present(now);
    costly.on_present(now);

    const auto cheap_start = cheap.next_start(now);
    const auto costly_start = costly.next_start(now);

    LUMEN_CHECK(cheap.next_deadline(now) == costly.next_deadline(now));
    LUMEN_CHECK(costly_start < cheap_start); // expensive frame begins earlier
    // The cheap frame should idle most of the period rather than working through it.
    LUMEN_CHECK(cheap_start - now > k_60hz / 2);
}

// A repaint must finish before the vblank it targets, with the margin intact.
LUMEN_TEST(repaint_start_leaves_room_for_the_estimate) {
    auto s = at_60hz();
    settle(s, 2000, 2000); // 4 ms

    const lx::runtime::clock_time now = 500'000'000;
    s.on_present(now);
    const auto start = s.next_start(now);
    const auto deadline = s.next_deadline(now);

    const auto available = deadline - start;
    LUMEN_CHECK(available >= s.estimate_ns());
    LUMEN_CHECK(available >= s.estimate_ns() + lx::scheduler::repaint_scheduler::k_margin_ns - 1);
}

// The deadline is the next vblank strictly ahead, extrapolated from the last flip — not
// simply "a period from now", which would drift away from the display's cadence.
LUMEN_TEST(repaint_deadline_extrapolates_from_the_last_flip) {
    auto s = at_60hz();
    const lx::runtime::clock_time flip = 1'000'000'000;
    s.on_present(flip);

    LUMEN_CHECK(s.next_deadline(flip) == flip + k_60hz);
    // Two and a half periods later, the next vblank is the third one.
    LUMEN_CHECK(s.next_deadline(flip + k_60hz * 2 + k_60hz / 2) == flip + k_60hz * 3);
    // Exactly on a vblank, the deadline is the following one, never the current instant.
    LUMEN_CHECK(s.next_deadline(flip + k_60hz) == flip + k_60hz * 2);
}

// With no flip yet — the timer-driven path before the first present — the deadline is
// still a period out rather than zero or the past.
LUMEN_TEST(repaint_deadline_without_a_flip_is_one_period_out) {
    auto s = at_60hz();
    LUMEN_CHECK(!s.has_presented());
    const lx::runtime::clock_time now = 42'000'000;
    LUMEN_CHECK(s.next_deadline(now) == now + k_60hz);
    LUMEN_CHECK(s.next_start(now) >= now);
}

// A repaint that no longer fits before the next vblank should target the one after it,
// rather than starting late and guaranteeing a miss.
LUMEN_TEST(repaint_skips_to_the_next_deadline_when_it_cannot_fit) {
    auto s = at_60hz();
    settle(s, 6000, 6000); // 12 ms

    const lx::runtime::clock_time flip = 2'000'000'000;
    s.on_present(flip);

    // Only 3 ms left before the next vblank, but the repaint costs 12.
    const auto now = flip + k_60hz - 3'000'000;
    const auto start = s.next_start(now);
    LUMEN_CHECK(start >= now);
    // It must be aiming past the imminent vblank.
    LUMEN_CHECK(start + s.estimate_ns() > flip + k_60hz);
}

// Never schedule in the past, whatever the numbers say.
LUMEN_TEST(repaint_never_starts_in_the_past) {
    auto s = at_60hz();
    settle(s, 40'000, 40'000); // 80 ms — far beyond a 60 Hz period

    const lx::runtime::clock_time flip = 3'000'000'000;
    s.on_present(flip);
    for (int i = 0; i < 8; ++i) {
        const auto now = flip + k_60hz * i / 2;
        LUMEN_CHECK(s.next_start(now) >= now);
    }
    // And it should say plainly that the frame cannot keep up, rather than only running late.
    LUMEN_CHECK(s.over_budget_for_refresh());
}

LUMEN_TEST(repaint_reports_fitting_within_refresh) {
    auto s = at_60hz();
    settle(s, 1000, 1000);
    LUMEN_CHECK(!s.over_budget_for_refresh());
}

// A faster panel must shorten the deadline spacing — the schedule follows the display,
// not a configured target.
LUMEN_TEST(repaint_follows_the_display_refresh_period) {
    lx::scheduler::repaint_scheduler s{};
    constexpr lx::runtime::clock_time k_144hz = 6'944'444;
    s.set_refresh_period(k_144hz);
    settle(s, 500, 500);

    const lx::runtime::clock_time flip = 9'000'000'000;
    s.on_present(flip);
    LUMEN_CHECK(s.next_deadline(flip) == flip + k_144hz);
    LUMEN_CHECK(s.refresh_period() == k_144hz);

    // A zero or negative period is nonsense and must not wipe out the real one.
    s.set_refresh_period(0);
    LUMEN_CHECK(s.refresh_period() == k_144hz);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
