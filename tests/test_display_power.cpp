// Display power (`lx.compositor:display_power`): the off and on edges and what each asks the
// compositor to do, the render outcomes (done, deferred while the VT is away, retry after a
// queued flip, rejected → soft blank, stale), waking on input, the VT resume as activity, the
// resume result, and the `mode` events clients are told — once per real change. Pure.
#include "lumen_test.hpp"

import lx.compositor;

using lx::compositor::display_power;
using lx::compositor::power_actions;
using lx::compositor::power_off_outcome;
using lx::compositor::power_on_outcome;
using lx::compositor::power_phase;

namespace {

bool has(const power_actions& a, unsigned bit) { return a.has(bit); }

/// A machine that went off cleanly.
display_power powered_off() {
    display_power p{};
    const power_actions off = p.request_off(false);
    LUMEN_CHECK(has(off, power_actions::gate) && has(off, power_actions::post_off));
    LUMEN_CHECK(!has(off, power_actions::send_mode)); // only once the CRTC is down
    const power_actions done = p.on_off_result(off.gen, power_off_outcome::done);
    LUMEN_CHECK(has(done, power_actions::send_mode) && !done.mode_on);
    LUMEN_CHECK(p.phase() == power_phase::off);
    return p;
}

} // namespace

LUMEN_TEST(off_edge_gates_and_posts_render_task) {
    display_power p{};
    LUMEN_CHECK(p.phase() == power_phase::on && p.reported_on() && !p.gated());
    const power_actions a = p.request_off(false);
    LUMEN_CHECK(a.bits == (power_actions::gate | power_actions::post_off));
    LUMEN_CHECK(a.gen == p.generation());
    LUMEN_CHECK(p.phase() == power_phase::off_pending && p.gated() && p.wants_wake());
    LUMEN_CHECK(p.reported_on()); // not yet off as far as clients know
    const power_actions done = p.on_off_result(a.gen, power_off_outcome::done);
    LUMEN_CHECK(has(done, power_actions::send_mode) && has(done, power_actions::log_off));
    LUMEN_CHECK(!p.reported_on());
    // Asking again changes nothing and tells nobody anything.
    LUMEN_CHECK(p.request_off(false).empty());
}

LUMEN_TEST(wake_resumes_with_modeset_and_reports_on_after_result) {
    display_power p = powered_off();
    const power_actions w = p.wake(false);
    LUMEN_CHECK(has(w, power_actions::ungate) && w.resume);
    LUMEN_CHECK(!has(w, power_actions::send_mode));
    LUMEN_CHECK(p.phase() == power_phase::on_pending && !p.gated());
    LUMEN_CHECK(p.wake(false).empty()); // input while coming on: nothing more
    const power_actions r = p.on_resume_result(power_on_outcome::ok);
    LUMEN_CHECK(has(r, power_actions::send_mode) && r.mode_on && has(r, power_actions::log_on));
    LUMEN_CHECK(p.phase() == power_phase::on && p.reported_on());
    LUMEN_CHECK(p.wake(false).empty()); // input while on: nothing
}

LUMEN_TEST(request_on_is_wake) {
    display_power p = powered_off();
    LUMEN_CHECK(has(p.request_on(false), power_actions::ungate));
    LUMEN_CHECK(p.request_on(false).empty());
}

LUMEN_TEST(retry_after_flip) {
    display_power p{};
    const power_actions a = p.request_off(false);
    const power_actions w = p.on_off_result(a.gen, power_off_outcome::retry_after_flip);
    LUMEN_CHECK(w.bits == power_actions::wait_flip);
    LUMEN_CHECK(p.waiting_for_flip() && p.phase() == power_phase::off_pending);
    const power_actions again = p.on_flip_drained();
    LUMEN_CHECK(again.bits == power_actions::post_off && again.gen == a.gen);
    LUMEN_CHECK(p.on_flip_drained().empty()); // only once
    LUMEN_CHECK(has(p.on_off_result(again.gen, power_off_outcome::done), power_actions::send_mode));
}

LUMEN_TEST(rejected_is_soft_blank) {
    display_power p{};
    const power_actions a = p.request_off(false);
    const power_actions r = p.on_off_result(a.gen, power_off_outcome::rejected);
    LUMEN_CHECK(has(r, power_actions::log_soft_blank) && has(r, power_actions::send_mode));
    LUMEN_CHECK(p.phase() == power_phase::soft_blank && p.gated() && p.wants_wake());
    LUMEN_CHECK(has(p.wake(false), power_actions::ungate));
}

LUMEN_TEST(wake_supersedes_off_in_flight) {
    display_power p{};
    const power_actions a = p.request_off(false);
    const power_actions w = p.wake(false);
    LUMEN_CHECK(has(w, power_actions::ungate) && w.resume);
    LUMEN_CHECK(p.generation() != a.gen); // the render task finds itself stale
    LUMEN_CHECK(p.on_off_result(a.gen, power_off_outcome::done).empty());
    LUMEN_CHECK(p.phase() == power_phase::on_pending && p.reported_on());
    // Clients were never told off, so they are not told on either.
    LUMEN_CHECK(!has(p.on_resume_result(power_on_outcome::ok), power_actions::send_mode));
    LUMEN_CHECK(p.phase() == power_phase::on);
}

LUMEN_TEST(vt_away_only_changes_target) {
    display_power p{};
    const power_actions a = p.request_off(true);
    LUMEN_CHECK(has(a, power_actions::gate) && !has(a, power_actions::post_off));
    LUMEN_CHECK(has(a, power_actions::send_mode) && !a.mode_on);
    LUMEN_CHECK(p.phase() == power_phase::off);
    // On while away: no resume of ours (the VT's resume does the modeset).
    const power_actions on = p.request_on(true);
    LUMEN_CHECK(has(on, power_actions::ungate) && !on.resume && has(on, power_actions::send_mode));
    LUMEN_CHECK(p.phase() == power_phase::on);
}

LUMEN_TEST(deferred_while_vt_paused_then_vt_resume_wakes) {
    display_power p{};
    const power_actions a = p.request_off(false);
    // The VT switched away before the render task ran.
    const power_actions d = p.on_off_result(a.gen, power_off_outcome::deferred);
    LUMEN_CHECK(has(d, power_actions::send_mode) && p.phase() == power_phase::off);
    // The VT comes back: activity; its own modeset powers the output.
    const power_actions v = p.on_vt_resumed();
    LUMEN_CHECK(has(v, power_actions::ungate) && !v.resume);
    LUMEN_CHECK(has(v, power_actions::send_mode) && v.mode_on);
    LUMEN_CHECK(p.phase() == power_phase::on);
    LUMEN_CHECK(p.on_vt_resumed().empty()); // already on
}

LUMEN_TEST(vt_resume_settles_a_pending_power_on) {
    display_power p = powered_off();
    (void)p.wake(false);
    LUMEN_CHECK(p.phase() == power_phase::on_pending);
    // A VT switch raced the wake; its resume settles it.
    const power_actions v = p.on_vt_resumed();
    LUMEN_CHECK(!has(v, power_actions::ungate)); // not gated: nothing to lower
    LUMEN_CHECK(has(v, power_actions::send_mode) && v.mode_on);
    LUMEN_CHECK(p.phase() == power_phase::on);
}

LUMEN_TEST(resume_result_outcomes) {
    {
        display_power p = powered_off();
        (void)p.wake(false);
        const power_actions r = p.on_resume_result(power_on_outcome::device_unavailable);
        LUMEN_CHECK(p.phase() == power_phase::on && has(r, power_actions::send_mode));
    }
    {
        display_power p = powered_off();
        (void)p.wake(false);
        const power_actions r = p.on_resume_result(power_on_outcome::rejected);
        LUMEN_CHECK(has(r, power_actions::gate) && has(r, power_actions::log_on_failed));
        LUMEN_CHECK(!has(r, power_actions::send_mode)); // clients were never told on
        LUMEN_CHECK(p.phase() == power_phase::off && p.wants_wake());
        LUMEN_CHECK(has(p.wake(false), power_actions::ungate)); // the next input retries
    }
    {
        display_power p{};
        LUMEN_CHECK(p.on_resume_result(power_on_outcome::ok).empty()); // not ours
    }
}

LUMEN_TEST(off_again_while_coming_on) {
    display_power p = powered_off();
    (void)p.wake(false);
    const power_actions a = p.request_off(false);
    LUMEN_CHECK(has(a, power_actions::gate) && has(a, power_actions::post_off));
    LUMEN_CHECK(p.on_resume_result(power_on_outcome::ok).empty()); // the old resume is moot
    // Still reported off: no second `mode off`.
    LUMEN_CHECK(!has(p.on_off_result(a.gen, power_off_outcome::done), power_actions::send_mode));
    LUMEN_CHECK(p.phase() == power_phase::off && !p.reported_on());
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
