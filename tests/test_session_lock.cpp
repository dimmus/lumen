// The session lock (`lx.compositor:session_lock`): every transition — the request, the wait
// for lock surfaces (content on every output, or 1 s), the locked frame, unlock, a locker that
// gives up or dies (abandoned, then replaced and locked at once), a second locker (finished),
// logind's Unlock — and the lock surfaces' bookkeeping: configure serials and acks
// (invalid_serial for unknown, re-acked and older ones), the commit rules
// (commit_before_first_ack, null_buffer, dimensions_mismatch), keyboard focus among lock
// surfaces. Pure.
#include "lumen_test.hpp"

import lx.foundation;
import lx.input;
import lx.compositor;

using lx::compositor::lock_actions;
using lx::compositor::lock_state;
using lx::compositor::session_lock;

namespace {

constexpr unsigned long long k_s = 1'000'000'000ull;

void* res(unsigned long long n) { return reinterpret_cast<void*>(n * 16); }

bool has(const lock_actions& a, unsigned bit) { return a.has(bit); }

/// A machine whose lock `res(1)` covers output 0 with content (locking).
session_lock locking_one_output(unsigned& surface_id) {
    session_lock m{};
    (void)m.set_outputs(0x1);
    const lock_actions req = m.request(res(1), 10 * k_s);
    LUMEN_CHECK(m.state() == lock_state::awaiting_surfaces);
    LUMEN_CHECK(has(req, lock_actions::input_lock) && has(req, lock_actions::notify_shell));
    LUMEN_CHECK(!has(req, lock_actions::scene_lock) && !has(req, lock_actions::send_locked));
    surface_id = m.add_surface(res(2), res(3), lx::surface_id{7}, 0);
    LUMEN_CHECK(surface_id != 0);
    m.note_configure(surface_id, 100, {1366, 768});
    LUMEN_CHECK(m.ack(surface_id, 100) == session_lock::ack_result::ok);
    const lock_actions c = m.content(surface_id);
    LUMEN_CHECK(m.state() == lock_state::locking);
    LUMEN_CHECK(has(c, lock_actions::scene_lock) && has(c, lock_actions::refocus));
    LUMEN_CHECK(m.keyboard_surface() == lx::surface_id{7} && m.keyboard_resource() == res(3));
    return m;
}

} // namespace

LUMEN_TEST(lock_waits_for_surfaces_then_locks_after_the_locked_frame) {
    unsigned id = 0;
    session_lock m = locking_one_output(id);
    LUMEN_CHECK(m.input_locked() && m.scene_locked() && m.current() == res(1));
    // `locked` only once a locked frame is shown — never at the request (L1).
    const lock_actions shown = m.locked_frame_shown();
    LUMEN_CHECK(m.state() == lock_state::locked);
    LUMEN_CHECK(has(shown, lock_actions::send_locked) && shown.lock == res(1));
    LUMEN_CHECK(has(shown, lock_actions::locked_hint) && shown.hint);
    LUMEN_CHECK(has(shown, lock_actions::notify_shell));
    // Once only.
    LUMEN_CHECK(m.locked_frame_shown().empty());
    LUMEN_CHECK(lx::compositor::shell_lock_state(lock_state::awaiting_surfaces) == 1);
    LUMEN_CHECK(lx::compositor::shell_lock_state(lock_state::locking) == 1);
    LUMEN_CHECK(lx::compositor::shell_lock_state(lock_state::locked) == 2);
    LUMEN_CHECK(lx::compositor::shell_lock_state(lock_state::abandoned) == 3);
}

LUMEN_TEST(deadline_locks_without_surfaces) {
    session_lock m{};
    (void)m.set_outputs(0x3);
    (void)m.request(res(1), 5 * k_s);
    LUMEN_CHECK(m.deadline() == 6 * k_s);
    // One output covered of two: still waiting.
    const unsigned a = m.add_surface(res(2), res(3), lx::surface_id{7}, 0);
    m.note_configure(a, 1, {800, 600});
    (void)m.ack(a, 1);
    (void)m.content(a);
    LUMEN_CHECK(m.state() == lock_state::awaiting_surfaces);
    LUMEN_CHECK(m.on_timer(5 * k_s + k_s / 2).empty());
    const lock_actions t = m.on_timer(6 * k_s);
    LUMEN_CHECK(m.state() == lock_state::locking && has(t, lock_actions::scene_lock));
    LUMEN_CHECK(m.deadline() == 0);
}

LUMEN_TEST(every_output_or_one_going_completes_the_wait) {
    session_lock m{};
    (void)m.set_outputs(0x3);
    (void)m.request(res(1), 0);
    const unsigned a = m.add_surface(res(2), res(3), lx::surface_id{7}, 0);
    m.note_configure(a, 1, {800, 600});
    (void)m.ack(a, 1);
    (void)m.content(a);
    LUMEN_CHECK(m.state() == lock_state::awaiting_surfaces);
    // The output without a lock surface goes: every output is covered now.
    const lock_actions o = m.set_outputs(0x1);
    LUMEN_CHECK(m.state() == lock_state::locking && has(o, lock_actions::scene_lock));
    // No output at all: nothing to wait for.
    session_lock none{};
    (void)none.request(res(5), 0);
    LUMEN_CHECK(none.state() == lock_state::locking);
}

LUMEN_TEST(a_second_locker_is_finished) {
    unsigned id = 0;
    session_lock m = locking_one_output(id);
    const lock_actions second = m.request(res(9), 11 * k_s);
    LUMEN_CHECK(has(second, lock_actions::send_finished) && second.finished == res(9));
    LUMEN_CHECK(m.current() == res(1) && m.state() == lock_state::locking);
    // Its objects going changes nothing.
    LUMEN_CHECK(m.released(res(9), false).empty());
    (void)m.locked_frame_shown();
    LUMEN_CHECK(has(m.request(res(10), 12 * k_s), lock_actions::send_finished));
}

LUMEN_TEST(unlock_and_destroy_unlocks) {
    unsigned id = 0;
    session_lock m = locking_one_output(id);
    (void)m.locked_frame_shown();
    // Only the lock in charge unlocks.
    LUMEN_CHECK(m.unlock(res(9)).empty());
    const lock_actions u = m.unlock(res(1));
    LUMEN_CHECK(m.state() == lock_state::unlocked && m.current() == nullptr);
    LUMEN_CHECK(has(u, lock_actions::scene_unlock) && has(u, lock_actions::input_unlock));
    LUMEN_CHECK(has(u, lock_actions::drop_surfaces) && has(u, lock_actions::activity));
    LUMEN_CHECK(has(u, lock_actions::locked_hint) && !u.hint);
    LUMEN_CHECK(has(u, lock_actions::notify_shell));
    // The resource's destructor afterwards: nothing more.
    LUMEN_CHECK(m.released(res(1), false).empty());
    // Unlock before `locked` is the wire's invalid_unlock; the machine ignores it.
    session_lock early{};
    (void)early.set_outputs(0x1);
    (void)early.request(res(1), 0);
    LUMEN_CHECK(early.unlock(res(1)).empty() && early.state() == lock_state::awaiting_surfaces);
}

LUMEN_TEST(a_locker_that_gives_up_before_locking_unlocks) {
    session_lock m{};
    (void)m.set_outputs(0x1);
    (void)m.request(res(1), 0);
    const lock_actions gave_up = m.released(res(1), false);
    LUMEN_CHECK(m.state() == lock_state::unlocked);
    LUMEN_CHECK(has(gave_up, lock_actions::input_unlock) && !has(gave_up, lock_actions::scene_unlock));
    unsigned id = 0;
    session_lock l = locking_one_output(id);
    const lock_actions g2 = l.released(res(1), false);
    LUMEN_CHECK(l.state() == lock_state::unlocked && has(g2, lock_actions::scene_unlock));
}

LUMEN_TEST(a_locker_that_dies_leaves_the_session_locked) {
    unsigned id = 0;
    session_lock m = locking_one_output(id);
    (void)m.locked_frame_shown();
    const lock_actions died = m.released(res(1), true);
    LUMEN_CHECK(m.state() == lock_state::abandoned && m.input_locked() && m.scene_locked());
    LUMEN_CHECK(has(died, lock_actions::notify_shell) && has(died, lock_actions::drop_surfaces));
    LUMEN_CHECK(!has(died, lock_actions::scene_unlock) && !has(died, lock_actions::input_unlock));
    LUMEN_CHECK(m.current() == nullptr);
    m.clear_surfaces();
    // A new locker takes over: locked at once (nothing of the session is on screen).
    const lock_actions again = m.request(res(20), 30 * k_s);
    LUMEN_CHECK(m.state() == lock_state::locked && m.current() == res(20));
    LUMEN_CHECK(has(again, lock_actions::send_locked) && again.lock == res(20));
    // Dying before `locked` fails closed too: the scene locks, abandoned.
    session_lock w{};
    (void)w.set_outputs(0x1);
    (void)w.request(res(1), 0);
    const lock_actions d2 = w.released(res(1), true);
    LUMEN_CHECK(w.state() == lock_state::abandoned && has(d2, lock_actions::scene_lock));
    LUMEN_CHECK(has(d2, lock_actions::locked_hint) && d2.hint);
}

LUMEN_TEST(logind_unlock_from_any_state) {
    unsigned id = 0;
    session_lock m = locking_one_output(id);
    (void)m.locked_frame_shown();
    const lock_actions u = m.force_unlock();
    LUMEN_CHECK(m.state() == lock_state::unlocked);
    LUMEN_CHECK(has(u, lock_actions::send_finished) && u.finished == res(1));
    LUMEN_CHECK(has(u, lock_actions::scene_unlock) && has(u, lock_actions::input_unlock));
    // Its unlock_and_destroy afterwards (the locker saw `locked`): nothing.
    LUMEN_CHECK(m.unlock(res(1)).empty());
    LUMEN_CHECK(m.force_unlock().empty());
    // Abandoned: nobody to finish.
    session_lock a = locking_one_output(id);
    (void)a.locked_frame_shown();
    (void)a.released(res(1), true);
    const lock_actions ua = a.force_unlock();
    LUMEN_CHECK(a.state() == lock_state::unlocked && !has(ua, lock_actions::send_finished));
}

LUMEN_TEST(configure_serials_and_acks) {
    session_lock m{};
    (void)m.set_outputs(0x1);
    (void)m.request(res(1), 0);
    const unsigned s = m.add_surface(res(2), res(3), lx::surface_id{7}, 0);
    m.note_configure(s, 10, {800, 600});
    LUMEN_CHECK(m.check_commit(s, true, {800, 600}) ==
                session_lock::commit_error::before_first_ack);
    LUMEN_CHECK(m.ack(s, 9) == session_lock::ack_result::invalid_serial); // never sent
    m.note_configure(s, 11, {1024, 768});
    m.note_configure(s, 12, {1366, 768});
    LUMEN_CHECK(m.ack(s, 11) == session_lock::ack_result::ok);
    LUMEN_CHECK(m.ack(s, 11) == session_lock::ack_result::invalid_serial); // again
    LUMEN_CHECK(m.ack(s, 10) == session_lock::ack_result::invalid_serial); // older
    LUMEN_CHECK(m.check_commit(s, false, {}) == session_lock::commit_error::null_buffer);
    LUMEN_CHECK(m.check_commit(s, true, {800, 600}) ==
                session_lock::commit_error::dimensions_mismatch);
    LUMEN_CHECK(m.check_commit(s, true, {1024, 768}) == session_lock::commit_error::none);
    LUMEN_CHECK(m.ack(s, 12) == session_lock::ack_result::ok);
    LUMEN_CHECK(m.check_commit(s, true, {1024, 768}) ==
                session_lock::commit_error::dimensions_mismatch);
    LUMEN_CHECK(m.check_commit(s, true, {1366, 768}) == session_lock::commit_error::none);
    // An inert record (gone) checks nothing.
    LUMEN_CHECK(m.check_commit(999, false, {}) == session_lock::commit_error::none);
    // Sixteen outstanding at most: the oldest is forgotten.
    for (unsigned i = 0; i < 20; ++i)
        m.note_configure(s, 100 + i, {1, 1});
    LUMEN_CHECK(m.ack(s, 100) == session_lock::ack_result::invalid_serial);
    LUMEN_CHECK(m.ack(s, 119) == session_lock::ack_result::ok);
}

LUMEN_TEST(keyboard_focus_among_lock_surfaces) {
    session_lock m{};
    (void)m.set_outputs(0x3);
    (void)m.request(res(1), 0);
    const unsigned a = m.add_surface(res(2), res(3), lx::surface_id{7}, 0);
    const unsigned b = m.add_surface(res(4), res(5), lx::surface_id{8}, 1);
    for (unsigned id : {a, b}) {
        m.note_configure(id, id, {10, 10});
        (void)m.ack(id, id);
    }
    LUMEN_CHECK(!m.keyboard_surface());
    // The second created gets content first: it has the keyboard.
    (void)m.content(b);
    LUMEN_CHECK(m.keyboard_surface() == lx::surface_id{8});
    (void)m.content(a);
    LUMEN_CHECK(m.keyboard_surface() == lx::surface_id{8});
    // A click moves it.
    LUMEN_CHECK(has(m.clicked(lx::surface_id{7}), lock_actions::refocus));
    LUMEN_CHECK(m.keyboard_surface() == lx::surface_id{7});
    LUMEN_CHECK(m.clicked(lx::surface_id{7}).empty());
    LUMEN_CHECK(m.clicked(lx::surface_id{99}).empty());
    // The focused one goes: the keyboard moves to the one that got content first.
    LUMEN_CHECK(has(m.remove_surface(a), lock_actions::refocus));
    LUMEN_CHECK(m.keyboard_surface() == lx::surface_id{8} && m.surface_count() == 1);
    (void)m.remove_surface(b);
    LUMEN_CHECK(!m.keyboard_surface() && m.keyboard_resource() == nullptr);
}

// A lock that starts while the VT is away decides "no keyboard focus" while the router's
// focus is already null (suspended). The router must still record that decision — what it
// compares a new one with is `chosen_keyboard_focus` — or the resume hands the keys to the
// window that had them before the switch, behind the lock.
LUMEN_TEST(lock_decided_while_suspended_replaces_the_resume_focus) {
    lx::compositor::input_router router{};
    lx::input::seat seat{};
    void* terminal = res(9);
    router.suspend_keyboard_focus(seat);
    router.set_keyboard_focus(terminal, seat); // the window manager's choice before the lock
    LUMEN_CHECK(router.keyboard_suspended());
    LUMEN_CHECK(router.keyboard_focus() == nullptr);
    LUMEN_CHECK(router.chosen_keyboard_focus() == terminal);
    // The lock: nobody. A caller comparing with keyboard_focus() would skip this call.
    router.set_keyboard_focus(nullptr, seat);
    LUMEN_CHECK(router.chosen_keyboard_focus() == nullptr);
    router.resume_keyboard_focus(seat);
    LUMEN_CHECK(!router.keyboard_suspended());
    LUMEN_CHECK(router.keyboard_focus() == nullptr && router.chosen_keyboard_focus() == nullptr);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
