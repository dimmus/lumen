// The session pause/resume state machine (VT switching): logind's PauseDevice types, the
// order of Active against ResumeDevice, the `pause` acknowledgement waiting for the render
// thread, DRM removal, duplicate signals, and the resume outcomes. Pure: no logind, no DRM.
#include "lumen_test.hpp"

import lx.compositor;

using lx::compositor::session_actions;
using lx::compositor::session_pause_kind;
using lx::compositor::session_phase;
using lx::compositor::session_resume_outcome;
using lx::compositor::session_state;

namespace {

constexpr unsigned k_drm_major = 226;
constexpr unsigned k_drm_minor = 0;
constexpr unsigned k_evdev_major = 13;
constexpr unsigned k_evdev_minor = 64;

session_state started() {
    session_state s{};
    const session_actions a = s.init(true, k_drm_major, k_drm_minor, false);
    LUMEN_CHECK(a.empty());
    LUMEN_CHECK(s.phase() == session_phase::active);
    return s;
}

/// A full pause through a DRM force pause, render quiesced.
session_state paused_by_force() {
    session_state s = started();
    const session_actions a =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(a.has(session_actions::stop_display) && a.has(session_actions::suspend_input));
    (void)s.on_render_quiesced(a.quiesce_gen);
    return s;
}

} // namespace

LUMEN_TEST(disabled_without_drm) {
    session_state s{};
    LUMEN_CHECK(s.init(false, 0, 0, false).empty());
    LUMEN_CHECK(s.phase() == session_phase::disabled);
    LUMEN_CHECK(s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force).empty());
    LUMEN_CHECK(s.on_active_changed(false, false).empty());
    LUMEN_CHECK(s.on_paused_tick(true, true).empty());
}

LUMEN_TEST(force_pause_then_resume_device) {
    session_state s = started();
    const session_actions p =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(p.has(session_actions::stop_display));
    LUMEN_CHECK(p.has(session_actions::suspend_input));
    LUMEN_CHECK(p.has(session_actions::log_paused) && !p.by_active);
    LUMEN_CHECK(p.kind == session_pause_kind::force);
    LUMEN_CHECK(!p.has(session_actions::complete_pause)); // force is never acknowledged
    LUMEN_CHECK(s.phase() == session_phase::paused && s.display_stopped());
    LUMEN_CHECK(s.input_suspended());
    // The quiesce round trip does not acknowledge a force pause either.
    LUMEN_CHECK(!s.on_render_quiesced(p.quiesce_gen).has(session_actions::complete_pause));

    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r.has(session_actions::begin_resume) && r.resume_gen != 0);
    LUMEN_CHECK(s.phase() == session_phase::resuming && !s.display_stopped());
    LUMEN_CHECK(!s.input_suspended());
    const session_actions ok = s.on_resume_result(r.resume_gen, session_resume_outcome::ok);
    LUMEN_CHECK(ok.has(session_actions::log_resumed));
    LUMEN_CHECK(s.phase() == session_phase::active);
}

LUMEN_TEST(active_false_before_pause_device) {
    session_state s = started();
    const session_actions a = s.on_active_changed(false, false);
    LUMEN_CHECK(a.has(session_actions::stop_display) && a.has(session_actions::suspend_input));
    LUMEN_CHECK(a.by_active);
    LUMEN_CHECK(s.phase() == session_phase::paused);
    // The PauseDevice that follows changes nothing.
    const session_actions b =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(b.empty());
}

LUMEN_TEST(active_true_without_master_waits_for_resume_device) {
    session_state s = paused_by_force();
    // logind may report Active before it has restored master: no modeset on that alone.
    LUMEN_CHECK(s.on_active_changed(true, false).empty());
    LUMEN_CHECK(s.phase() == session_phase::paused);
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r.has(session_actions::begin_resume));
    LUMEN_CHECK(s.phase() == session_phase::resuming);
}

LUMEN_TEST(active_true_with_master_resumes) {
    session_state s = paused_by_force();
    const session_actions r = s.on_active_changed(true, true);
    LUMEN_CHECK(r.has(session_actions::begin_resume));
    // ResumeDevice arriving afterwards does not resume twice.
    LUMEN_CHECK(s.on_device_resumed(k_drm_major, k_drm_minor).empty());
}

LUMEN_TEST(resume_device_before_active) {
    session_state s = paused_by_force();
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r.has(session_actions::begin_resume));
    LUMEN_CHECK(s.on_active_changed(true, true).empty());
    LUMEN_CHECK(s.phase() == session_phase::resuming);
}

LUMEN_TEST(pause_type_acknowledged_after_render_quiesced) {
    session_state s = started();
    const session_actions p =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::pause);
    LUMEN_CHECK(p.has(session_actions::stop_display));
    LUMEN_CHECK(!p.has(session_actions::complete_pause)); // not before render stopped
    LUMEN_CHECK(s.ack_pending());
    // A stale round trip (an earlier pause's) acknowledges nothing.
    LUMEN_CHECK(s.on_render_quiesced(p.quiesce_gen - 1).empty());
    const session_actions q = s.on_render_quiesced(p.quiesce_gen);
    LUMEN_CHECK(q.has(session_actions::complete_pause));
    LUMEN_CHECK(q.ack_major == k_drm_major && q.ack_minor == k_drm_minor);
    LUMEN_CHECK(!s.ack_pending());
    // Exactly once.
    LUMEN_CHECK(s.on_render_quiesced(p.quiesce_gen).empty());
}

LUMEN_TEST(pause_type_after_quiesce_acknowledged_at_once) {
    session_state s = started();
    // Active=false first (no ack owed), render quiesces, then the `pause` signal.
    const session_actions a = s.on_active_changed(false, false);
    (void)s.on_render_quiesced(a.quiesce_gen);
    const session_actions p =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::pause);
    LUMEN_CHECK(p.has(session_actions::complete_pause) && !p.has(session_actions::stop_display));
    LUMEN_CHECK(p.ack_major == k_drm_major && p.ack_minor == k_drm_minor);
}

LUMEN_TEST(force_and_gone_never_acknowledged) {
    session_state s = started();
    const session_actions f =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(!f.has(session_actions::complete_pause));
    LUMEN_CHECK(!s.on_render_quiesced(f.quiesce_gen).has(session_actions::complete_pause));
    const session_actions g = s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::gone);
    LUMEN_CHECK(!g.has(session_actions::complete_pause));
    LUMEN_CHECK(g.has(session_actions::log_display_lost));
}

LUMEN_TEST(evdev_pauses_do_not_change_state) {
    session_state s = started();
    for (auto k : {session_pause_kind::pause, session_pause_kind::force, session_pause_kind::gone})
        LUMEN_CHECK(s.on_device_paused(k_evdev_major, k_evdev_minor, k).empty());
    LUMEN_CHECK(s.phase() == session_phase::active);
    LUMEN_CHECK(s.on_device_resumed(k_evdev_major, k_evdev_minor).empty());
}

LUMEN_TEST(drm_gone_is_display_lost) {
    session_state s = started();
    const session_actions g = s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::gone);
    LUMEN_CHECK(g.has(session_actions::stop_display));
    LUMEN_CHECK(!g.has(session_actions::suspend_input)); // input keeps working
    LUMEN_CHECK(g.has(session_actions::log_display_lost));
    LUMEN_CHECK(s.phase() == session_phase::display_lost && s.display_stopped());
    // Terminal: nothing resumes the display.
    LUMEN_CHECK(s.on_device_resumed(k_drm_major, k_drm_minor).empty());
    LUMEN_CHECK(s.on_paused_tick(true, true).empty());
    // The session leaving and coming back still takes the input devices with it.
    const session_actions away = s.on_active_changed(false, false);
    LUMEN_CHECK(away.bits == session_actions::suspend_input);
    const session_actions back = s.on_active_changed(true, false);
    LUMEN_CHECK(back.bits == session_actions::resume_input);
    LUMEN_CHECK(s.phase() == session_phase::display_lost);
}

LUMEN_TEST(duplicate_signals_are_idempotent) {
    session_state s = started();
    const session_actions p1 =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(p1.has(session_actions::stop_display));
    LUMEN_CHECK(s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force).empty());
    LUMEN_CHECK(s.on_active_changed(false, false).empty());
    const session_actions r1 = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r1.has(session_actions::begin_resume));
    LUMEN_CHECK(s.on_device_resumed(k_drm_major, k_drm_minor).empty());
    LUMEN_CHECK(s.on_resume_result(r1.resume_gen, session_resume_outcome::ok)
                    .has(session_actions::log_resumed));
    LUMEN_CHECK(s.on_resume_result(r1.resume_gen, session_resume_outcome::ok).empty());
    LUMEN_CHECK(s.on_device_resumed(k_drm_major, k_drm_minor).empty());
}

LUMEN_TEST(device_unavailable_retries_at_paused_tick) {
    session_state s = paused_by_force();
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    const session_actions fail =
        s.on_resume_result(r.resume_gen, session_resume_outcome::device_unavailable);
    LUMEN_CHECK(fail.has(session_actions::stop_display));
    LUMEN_CHECK(!fail.has(session_actions::log_resume_rejected));
    LUMEN_CHECK(s.phase() == session_phase::paused);
    // Not while master is still elsewhere, nor while the session is inactive.
    LUMEN_CHECK(s.on_paused_tick(true, false).empty());
    LUMEN_CHECK(s.on_paused_tick(false, true).empty());
    const session_actions again = s.on_paused_tick(true, true);
    LUMEN_CHECK(again.has(session_actions::begin_resume) && again.resume_gen > r.resume_gen);
}

LUMEN_TEST(rejected_waits_for_the_next_resume_device) {
    session_state s = paused_by_force();
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    const session_actions rej = s.on_resume_result(r.resume_gen, session_resume_outcome::rejected);
    LUMEN_CHECK(rej.has(session_actions::log_resume_rejected));
    LUMEN_CHECK(rej.has(session_actions::stop_display));
    LUMEN_CHECK(s.phase() == session_phase::paused);
    LUMEN_CHECK(s.on_paused_tick(true, true).empty());
    LUMEN_CHECK(s.on_active_changed(true, true).empty());
    const session_actions r2 = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r2.has(session_actions::begin_resume));
    // Rejected again: logged again only after a new hold began (one line per switch).
    const session_actions rej2 = s.on_resume_result(r2.resume_gen, session_resume_outcome::rejected);
    LUMEN_CHECK(rej2.has(session_actions::log_resume_rejected));
}

LUMEN_TEST(pause_during_resume_supersedes_it) {
    session_state s = paused_by_force();
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    const session_actions p =
        s.on_device_paused(k_drm_major, k_drm_minor, session_pause_kind::force);
    LUMEN_CHECK(p.has(session_actions::stop_display) && p.has(session_actions::suspend_input));
    // The old resume's result is stale.
    LUMEN_CHECK(s.on_resume_result(r.resume_gen, session_resume_outcome::ok).empty());
    LUMEN_CHECK(s.phase() == session_phase::paused);
}

LUMEN_TEST(initial_inactive_starts_paused) {
    session_state s{};
    const session_actions a = s.init(true, k_drm_major, k_drm_minor, true);
    LUMEN_CHECK(a.has(session_actions::stop_display) && a.has(session_actions::suspend_input));
    LUMEN_CHECK(s.phase() == session_phase::paused);
    const session_actions r = s.on_device_resumed(k_drm_major, k_drm_minor);
    LUMEN_CHECK(r.has(session_actions::begin_resume));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
