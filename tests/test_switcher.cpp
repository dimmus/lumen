// lumen-shell's window switcher (design-session-and-wm K.2), pure parts: the task model's
// most-recently-used order (seeded from stacking after a snapshot, focus moves a window to
// the front, the focus-follows-mouse debounce), the switcher model (scopes, minimized, the
// same-application filter, next/previous wrap, the keys and the user's bindings, Escape,
// Return, a window going during the hold, the open delay) and the view's layout.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.shell;

namespace {

using namespace lx::shell;

void* proxy(unsigned n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + n * 16)); }

/// A synced model with windows w0…w(n-1), w0 at the bottom of the stacking order.
void snapshot(task_model& m, unsigned n, const char* app = "app") {
    m.clear();
    m.begin_snapshot();
    for (unsigned i = 0; i < n; ++i) {
        char title[16];
        std::snprintf(title, sizeof(title), "w%u", i);
        (void)m.snapshot_add(proxy(i), app, title, 0, {0, 0, 100, 100}, i);
    }
    m.end_snapshot();
    (void)m.take_changes();
}

task_handle by_title(const task_model& m, const char* title) {
    task_handle all[k_max_tasks];
    const unsigned n = m.ordered(all, k_max_tasks);
    for (unsigned i = 0; i < n; ++i)
        if (const task_entry* e = m.get(all[i]); e && std::strcmp(e->title, title) == 0)
            return all[i];
    return {};
}

/// The MRU order as "w2 w0 w1".
std::string mru_of(const task_model& m) {
    task_handle all[k_max_tasks];
    const unsigned n = m.mru(all, k_max_tasks);
    std::string s;
    for (unsigned i = 0; i < n; ++i) {
        if (i)
            s += ' ';
        s += m.get(all[i])->title;
    }
    return s;
}

std::string list_of(const task_model& m, const switcher_model& s) {
    std::string out;
    for (unsigned i = 0; i < s.count(); ++i) {
        if (i)
            out += ' ';
        out += m.get(s.entry(i))->title;
    }
    return out;
}

const char* selected_title(const task_model& m, const switcher_model& s) {
    const task_entry* e = m.get(s.selected());
    return e ? e->title : "";
}

constexpr long long k_ms = 1'000'000;

} // namespace

LUMEN_TEST(mru_seeded_from_stacking_then_focus_moves_to_front) {
    static task_model m;
    snapshot(m, 4);
    LUMEN_CHECK(mru_of(m) == "w3 w2 w1 w0");
    m.set_focus(by_title(m, "w1"));
    LUMEN_CHECK(mru_of(m) == "w1 w3 w2 w0");
    m.set_focus(by_title(m, "w0"));
    LUMEN_CHECK(mru_of(m) == "w0 w1 w3 w2");
    // No focus changes nothing in the order.
    m.set_focus({});
    LUMEN_CHECK(mru_of(m) == "w0 w1 w3 w2");
    // A new window enters at the back until it is focused; a window that goes leaves.
    const task_handle w4 = m.add(proxy(9));
    (void)m.set_state(w4, "app", "w4", 0);
    LUMEN_CHECK(mru_of(m) == "w0 w1 w3 w2 w4");
    m.set_focus(w4);
    LUMEN_CHECK(mru_of(m) == "w4 w0 w1 w3 w2");
    (void)m.remove(by_title(m, "w1"));
    LUMEN_CHECK(mru_of(m) == "w4 w0 w3 w2");
    // A new snapshot (the shell restarted) starts again from the stacking order.
    snapshot(m, 3);
    LUMEN_CHECK(mru_of(m) == "w2 w1 w0");
}

LUMEN_TEST(mru_debounce_forgets_windows_focus_only_passed) {
    static task_model m;
    snapshot(m, 4);
    m.set_focus(by_title(m, "w0"), 0);
    LUMEN_CHECK(mru_of(m) == "w0 w3 w2 w1");
    m.set_focus_debounce(750 * k_ms);
    // The pointer passes over w2 and w1 (100 ms each) and settles on w3.
    m.set_focus(by_title(m, "w2"), 1000 * k_ms);
    m.set_focus(by_title(m, "w1"), 1100 * k_ms);
    m.set_focus(by_title(m, "w3"), 1200 * k_ms);
    LUMEN_CHECK(mru_of(m) == "w3 w0 w2 w1");
    // w3 was kept 2 s: it counts when focus moves on.
    m.set_focus(by_title(m, "w0"), 3200 * k_ms);
    LUMEN_CHECK(mru_of(m) == "w0 w3 w2 w1");
    // An explicit focus (the switcher's choice) counts however briefly it lasts.
    m.note_explicit_focus(by_title(m, "w1"));
    m.set_focus(by_title(m, "w1"), 4000 * k_ms);
    m.set_focus(by_title(m, "w2"), 4050 * k_ms);
    LUMEN_CHECK(mru_of(m) == "w2 w1 w0 w3");
    m.set_focus_debounce(0);
}

LUMEN_TEST(switcher_next_selects_the_previous_window_and_wraps) {
    static task_model m;
    snapshot(m, 3);
    m.set_focus(by_title(m, "w2"));
    switcher_model s;
    const switcher_options o{};
    LUMEN_CHECK(s.begin(m, true, false, o, nullptr, nullptr, 0) == 3);
    LUMEN_CHECK(list_of(m, s) == "w2 w1 w0" && std::strcmp(selected_title(m, s), "w1") == 0);
    // Tab, Tab: wraps to the focused window.
    LUMEN_CHECK(s.key(switcher_keysym::tab, 0x4, "switcher.next") == switcher_key_result::changed);
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w0") == 0);
    (void)s.key(switcher_keysym::tab, 0x4, "switcher.next");
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w2") == 0);
    // Shift+Tab (bound to switcher.previous) and the arrows go back.
    (void)s.key(switcher_keysym::tab, 0x5, "switcher.previous");
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w0") == 0);
    (void)s.key(switcher_keysym::left, 0x4, "");
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w1") == 0);
    // Shift+Tab with no binding for it still steps back (ISO_Left_Tab, or Tab with shift).
    (void)s.key(switcher_keysym::iso_left_tab, 0x5, "");
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w2") == 0);
    (void)s.key(switcher_keysym::tab, 0x5, "");
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w0") == 0);
    // Keys that are not the switcher's do nothing.
    LUMEN_CHECK(s.key('q', 0x4, "") == switcher_key_result::none);
    // Released: the selection is the result; the session is over.
    const task_handle got = s.finish(true);
    LUMEN_CHECK(got == by_title(m, "w0") && !s.active() && s.count() == 0);
}

LUMEN_TEST(switcher_previous_starts_at_the_end_escape_cancels_return_commits) {
    static task_model m;
    snapshot(m, 3);
    m.set_focus(by_title(m, "w2"));
    switcher_model s;
    LUMEN_CHECK(s.begin(m, false, false, {}, nullptr, nullptr, 0) == 3);
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w0") == 0);
    LUMEN_CHECK(s.key(switcher_keysym::escape, 0x4, "") == switcher_key_result::cancel);
    LUMEN_CHECK(!s.finish(false));
    LUMEN_CHECK(s.begin(m, true, false, {}, nullptr, nullptr, 0) == 3);
    LUMEN_CHECK(s.key(switcher_keysym::return_key, 0x4, "") == switcher_key_result::commit);
    LUMEN_CHECK(s.finish(true) == by_title(m, "w1"));
    // With nothing focused the most recent window is selected first.
    m.set_focus({});
    LUMEN_CHECK(s.begin(m, true, false, {}, nullptr, nullptr, 0) == 3);
    LUMEN_CHECK(std::strcmp(selected_title(m, s), "w2") == 0);
    (void)s.finish(false);
    // One window: it stays selected (Alt+Tab activates it again, a no-op).
    snapshot(m, 1);
    m.set_focus(by_title(m, "w0"));
    LUMEN_CHECK(s.begin(m, true, false, {}, nullptr, nullptr, 0) == 1 && s.selection() == 0);
    (void)s.finish(false);
    // None: nothing to switch to.
    m.clear();
    LUMEN_CHECK(s.begin(m, true, false, {}, nullptr, nullptr, 0) == 0);
    (void)s.finish(false);
}

LUMEN_TEST(switcher_scopes_minimized_modal_and_same_app) {
    static task_model m;
    m.clear();
    m.begin_snapshot();
    (void)m.snapshot_add(proxy(0), "foot", "term-a", 0, {}, 0);
    (void)m.snapshot_add(proxy(1), "org.gtk.Demo4", "demo", 3 /* minimized */, {}, 1);
    (void)m.snapshot_add(proxy(2), "foot", "term-b", 0, {}, 2);
    (void)m.snapshot_add(proxy(3), "org.gtk.Demo4", "dialog", 5 /* modal */, {}, 3);
    (void)m.snapshot_add(proxy(4), "mpv", "video", 0, {}, 4);
    m.end_snapshot();
    (void)m.set_parent(by_title(m, "dialog"), by_title(m, "demo"));
    m.set_focus(by_title(m, "term-b"));
    switcher_model s;
    // A modal dialog is never listed (its parent stands for it); minimized ones are.
    LUMEN_CHECK(s.begin(m, true, false, {}, nullptr, nullptr, 0) == 4);
    LUMEN_CHECK(list_of(m, s) == "term-b video demo term-a");
    (void)s.finish(false);
    switcher_options no_min{};
    no_min.include_minimized = false;
    LUMEN_CHECK(s.begin(m, true, false, no_min, nullptr, nullptr, 0) == 3);
    LUMEN_CHECK(list_of(m, s) == "term-b video term-a");
    (void)s.finish(false);
    // Scope "workspace": the shell's filter decides (here: not the video).
    const auto not_video = [](const task_entry& e, void*) { return std::strcmp(e.app_id, "mpv") != 0; };
    LUMEN_CHECK(s.begin(m, true, false, {}, not_video, nullptr, 0) == 3);
    LUMEN_CHECK(list_of(m, s) == "term-b demo term-a");
    (void)s.finish(false);
    switcher_options all{};
    all.all = true;
    LUMEN_CHECK(s.begin(m, true, false, all, not_video, nullptr, 0) == 4);
    (void)s.finish(false);
    // Alt+grave: the focused window's application only.
    LUMEN_CHECK(s.begin(m, true, true, {}, nullptr, nullptr, 0) == 2);
    LUMEN_CHECK(list_of(m, s) == "term-b term-a" && std::strcmp(selected_title(m, s), "term-a") == 0);
    (void)s.finish(false);
}

LUMEN_TEST(switcher_open_delay_and_quick_release) {
    static task_model m;
    snapshot(m, 2);
    m.set_focus(by_title(m, "w1"));
    switcher_model s;
    switcher_options o{};
    o.open_delay_ns = 150 * k_ms;
    (void)s.begin(m, true, false, o, nullptr, nullptr, 1000 * k_ms);
    LUMEN_CHECK(s.open_at() == 1150 * k_ms && !s.opened());
    // A quick Alt+Tab: released before the delay — committed, never opened.
    LUMEN_CHECK(s.finish(true) == by_title(m, "w0") && s.open_at() == 0);
    (void)s.begin(m, true, false, o, nullptr, nullptr, 2000 * k_ms);
    s.mark_opened();
    LUMEN_CHECK(s.opened() && s.open_at() == 0);
    (void)s.finish(false);
}

LUMEN_TEST(switcher_a_window_that_goes_leaves_the_list) {
    static task_model m;
    snapshot(m, 4);
    m.set_focus(by_title(m, "w3"));
    switcher_model s;
    (void)s.begin(m, true, false, {}, nullptr, nullptr, 0);
    LUMEN_CHECK(list_of(m, s) == "w3 w2 w1 w0" && std::strcmp(selected_title(m, s), "w2") == 0);
    // Not the selected one: the selection stays on its window.
    (void)m.remove(by_title(m, "w3"));
    LUMEN_CHECK(s.prune(m) && list_of(m, s) == "w2 w1 w0" &&
                std::strcmp(selected_title(m, s), "w2") == 0);
    // The selected one: the one in its place.
    (void)m.remove(by_title(m, "w2"));
    LUMEN_CHECK(s.prune(m) && std::strcmp(selected_title(m, s), "w1") == 0);
    LUMEN_CHECK(!s.prune(m));
    (void)s.finish(false);
}

LUMEN_TEST(switcher_layout_wraps_and_pages) {
    // Three windows on 1920x1080: one row of three 96 px cells.
    const switcher_layout a = layout_switcher(3, 0, 1920, 1080);
    LUMEN_CHECK(a.columns == 3 && a.rows == 1 && a.first == 0 && a.shown == 3);
    // At least 400 wide (the title's room), the cells centred in it.
    LUMEN_CHECK(a.width == k_switcher_min_width &&
                a.height == k_switcher_cell + 2 * k_switcher_pad + k_switcher_title_h);
    LUMEN_CHECK(a.grid_x == (k_switcher_min_width - 3 * k_switcher_cell) / 2);
    LUMEN_CHECK(switcher_hit(a, a.grid_x + 1, k_switcher_pad + 1) == 0);
    LUMEN_CHECK(switcher_hit(a, a.grid_x + k_switcher_cell * 2 + 5, k_switcher_pad + 50) == 2);
    LUMEN_CHECK(switcher_hit(a, a.grid_x - 2, k_switcher_pad + 1) == -1);
    LUMEN_CHECK(switcher_hit(a, 2, 2) == -1);
    // Ten windows: wider than the minimum, the grid fills it.
    const switcher_layout w = layout_switcher(10, 0, 1920, 1080);
    LUMEN_CHECK(w.width == 10 * k_switcher_cell + 2 * k_switcher_pad && w.grid_x == k_switcher_pad);
    // 40 windows on a small output: rows wrap; when even those do not fit, a page holds
    // the selection.
    const switcher_layout b = layout_switcher(40, 0, 800, 600);
    LUMEN_CHECK(b.columns == 7 && b.rows == 4 && b.first == 0 && b.shown == 28);
    const switcher_layout c = layout_switcher(40, 30, 800, 600);
    LUMEN_CHECK(c.first == 28 && c.shown == 12);
    LUMEN_CHECK(switcher_hit(c, c.grid_x + 1, k_switcher_pad + 1) == 28);
    const lx::rect2i t = switcher_title_rect(b);
    LUMEN_CHECK(t.y == k_switcher_pad + 4 * k_switcher_cell && t.height == k_switcher_title_h);
    // Up and Down move by a row once the view says how many columns it has.
    static task_model m;
    snapshot(m, 10);
    switcher_model s;
    (void)s.begin(m, true, false, {}, nullptr, nullptr, 0);
    s.set_columns(4);
    const unsigned at = s.selection();
    (void)s.key(switcher_keysym::down, 0x4, "");
    LUMEN_CHECK(s.selection() == at + 4);
    (void)s.key(switcher_keysym::up, 0x4, "");
    LUMEN_CHECK(s.selection() == at);
    (void)s.key(switcher_keysym::end, 0x4, "");
    LUMEN_CHECK(s.selection() == 9);
    (void)s.key(switcher_keysym::home, 0x4, "");
    LUMEN_CHECK(s.selection() == 0);
    (void)s.finish(false);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
