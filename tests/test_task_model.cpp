// lumen-shell's pure parts: the task model fed by zlm_policy_bridge_v1, and the panel's
// layout and layout diff (what is repainted and what is damaged).
#include "lumen_test.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

import lx.foundation;
import lx.shell;

namespace {

using lx::shell::task_handle;
using lx::shell::task_model;

// Opaque proxies: the model never dereferences them.
void* proxy(unsigned n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + n * 16)); }

unsigned g_dropped = 0;
void count_drop(void*, void*) { ++g_dropped; }

/// A model with `n` entries from a completed snapshot, titled "w0", "w1", ….
task_model& snapshot_of(unsigned n) {
    static task_model m;
    m.clear();
    m.begin_snapshot();
    for (unsigned i = 0; i < n; ++i) {
        char title[16];
        std::snprintf(title, sizeof(title), "w%u", i);
        (void)m.snapshot_add(proxy(i), "app", title, 0, {0, 0, 100, 100}, i);
    }
    m.end_snapshot();
    (void)m.take_changes();
    return m;
}

const char* title_at(const task_model& m, unsigned i) {
    task_handle order[lx::shell::k_max_tasks];
    const unsigned n = m.ordered(order, lx::shell::k_max_tasks);
    if (i >= n)
        return "";
    const auto* e = m.get(order[i]);
    return e ? e->title : "";
}

} // namespace

LUMEN_TEST(snapshot_lands_whole_and_replaces_the_previous_one) {
    static task_model m;
    m.begin_snapshot();
    LUMEN_CHECK(m.in_snapshot() && !m.synced());
    (void)m.snapshot_add(proxy(1), "foot", "one", 0, {1, 2, 3, 4}, 0);
    (void)m.snapshot_add(proxy(2), "foot", "two", 1, {5, 6, 7, 8}, 1);
    LUMEN_CHECK(m.in_snapshot()); // a renderer holds off until snapshot_done
    m.end_snapshot();
    LUMEN_CHECK(!m.in_snapshot() && m.synced());
    LUMEN_CHECK(m.count() == 2);
    LUMEN_CHECK(std::strcmp(title_at(m, 0), "one") == 0);
    LUMEN_CHECK(std::strcmp(title_at(m, 1), "two") == 0);
    LUMEN_CHECK(m.take_changes() & lx::shell::task_change_list);

    // A second snapshot (a re-bound bridge) drops every proxy the first one held.
    g_dropped = 0;
    m.begin_snapshot(count_drop, nullptr);
    LUMEN_CHECK(g_dropped == 2 && m.count() == 0);
    (void)m.snapshot_add(proxy(3), "gtk", "three", 0, {}, 0);
    m.end_snapshot();
    LUMEN_CHECK(m.count() == 1 && std::strcmp(title_at(m, 0), "three") == 0);
}

LUMEN_TEST(deltas_during_and_after_the_snapshot_apply_in_order) {
    static task_model m;
    m.clear();
    m.begin_snapshot();
    const task_handle a = m.snapshot_add(proxy(1), "a", "A", 0, {}, 0);
    // A delta inside the batch names an entry the batch introduced.
    LUMEN_CHECK(m.set_state(a, "a", "A2", 0) & lx::shell::task_change_label);
    m.end_snapshot();
    // focus_changed right after snapshot_done (the compositor sends it there).
    m.set_focus(a);
    LUMEN_CHECK(m.focused() == a);
    const task_handle b = m.add(proxy(2));
    LUMEN_CHECK(b && m.count() == 2);
    LUMEN_CHECK(m.set_state(b, "b", "B", 0) != 0);
    LUMEN_CHECK(m.set_geometry(b, {10, 20, 30, 40}));
    LUMEN_CHECK(m.get(b)->geometry.width == 30);
    LUMEN_CHECK(std::strcmp(title_at(m, 0), "A2") == 0);
    LUMEN_CHECK(std::strcmp(title_at(m, 1), "B") == 0);
    // Same state again: no change bits, label revision unchanged.
    const unsigned rev = m.get(b)->label_rev;
    (void)m.take_changes();
    LUMEN_CHECK(m.set_state(b, "b", "B", 0) == 0);
    LUMEN_CHECK(m.get(b)->label_rev == rev && m.take_changes() == 0);
}

LUMEN_TEST(remove_of_an_unknown_or_stale_handle_is_refused) {
    task_model& m = snapshot_of(2);
    LUMEN_CHECK(!m.remove(task_handle{}));
    LUMEN_CHECK(!m.remove(task_handle{999, 1}));
    task_handle order[4];
    (void)m.ordered(order, 4);
    LUMEN_CHECK(m.remove(order[0]));
    LUMEN_CHECK(!m.remove(order[0])); // twice: stale now
    LUMEN_CHECK(m.count() == 1);
}

LUMEN_TEST(order_is_stable_across_remove_and_add) {
    task_model& m = snapshot_of(4); // w0 w1 w2 w3
    task_handle order[8];
    (void)m.ordered(order, 8);
    LUMEN_CHECK(m.remove(order[1])); // w0 w2 w3
    const task_handle n = m.add(proxy(9));
    (void)m.set_state(n, "x", "new", 0); // reuses w1's slot, but goes last
    LUMEN_CHECK(n.index == order[1].index);
    LUMEN_CHECK(std::strcmp(title_at(m, 0), "w0") == 0);
    LUMEN_CHECK(std::strcmp(title_at(m, 1), "w2") == 0);
    LUMEN_CHECK(std::strcmp(title_at(m, 2), "w3") == 0);
    LUMEN_CHECK(std::strcmp(title_at(m, 3), "new") == 0);
}

LUMEN_TEST(generation_rejects_a_handle_whose_slot_was_reused) {
    task_model& m = snapshot_of(1);
    task_handle order[2];
    (void)m.ordered(order, 2);
    const task_handle old = order[0];
    LUMEN_CHECK(m.remove(old));
    const task_handle fresh = m.add(proxy(5));
    LUMEN_CHECK(fresh.index == old.index && fresh.gen != old.gen);
    LUMEN_CHECK(m.get(old) == nullptr);
    LUMEN_CHECK(m.set_state(old, "x", "y", 0) == 0);
    LUMEN_CHECK(!m.set_geometry(old, {}));
    m.set_focus(old);
    LUMEN_CHECK(!m.focused());
    // Proxy lookup finds the new entry, with or without the right hint.
    LUMEN_CHECK(m.find_proxy(fresh.index, proxy(5)) == fresh);
    LUMEN_CHECK(m.find_proxy(200, proxy(5)) == fresh);
    LUMEN_CHECK(!m.find_proxy(fresh.index, proxy(6)));
}

LUMEN_TEST(focus_to_a_removed_handle_is_none) {
    task_model& m = snapshot_of(2);
    task_handle order[2];
    (void)m.ordered(order, 2);
    m.set_focus(order[1]);
    LUMEN_CHECK(m.focused() == order[1]);
    (void)m.take_changes();
    LUMEN_CHECK(m.remove(order[1]));
    LUMEN_CHECK(!m.focused());
    LUMEN_CHECK(m.take_changes() & lx::shell::task_change_focus);
    m.set_focus(order[1]);
    LUMEN_CHECK(!m.focused());
}

LUMEN_TEST(titles_are_cut_at_255_bytes_on_a_code_point_boundary) {
    char longest[600];
    // 254 ASCII bytes, then "é" (2 bytes) would end at byte 256: it must go entirely.
    std::memset(longest, 'a', 254);
    std::memcpy(longest + 254, "\xC3\xA9tail", 6);
    longest[260] = '\0';
    task_model& m = snapshot_of(0);
    const task_handle h = m.add(proxy(1));
    (void)m.set_state(h, "app", longest, 0);
    const char* t = m.get(h)->title;
    LUMEN_CHECK(std::strlen(t) == 254);
    // 253 ASCII + "é" = 255 bytes fits exactly.
    std::memset(longest, 'a', 253);
    std::memcpy(longest + 253, "\xC3\xA9", 3);
    (void)m.set_state(h, "app", longest, 0);
    LUMEN_CHECK(std::strlen(m.get(h)->title) == 255);
    // A 4-byte sequence straddling the limit.
    std::memset(longest, 'b', 253);
    std::memcpy(longest + 253, "\xF0\x9F\x98\x80!", 6);
    (void)m.set_state(h, "app", longest, 0);
    LUMEN_CHECK(std::strlen(m.get(h)->title) == 253);
    char small[4];
    LUMEN_CHECK(lx::shell::copy_utf8(small, sizeof(small), "\xE2\x80\xA6x")); // "…x"
    LUMEN_CHECK(std::strcmp(small, "\xE2\x80\xA6") == 0);
    LUMEN_CHECK(lx::shell::copy_utf8(small, sizeof(small), "\xF0\x9F\x98\x80"));
    LUMEN_CHECK(small[0] == '\0');
    LUMEN_CHECK(!lx::shell::copy_utf8(small, sizeof(small), nullptr) && small[0] == '\0');
}

LUMEN_TEST(full_table_refuses_without_corrupting) {
    static task_model m;
    m.clear();
    for (unsigned i = 0; i < lx::shell::k_max_tasks; ++i)
        LUMEN_CHECK(static_cast<bool>(m.add(proxy(i))));
    LUMEN_CHECK(!m.add(proxy(999)));
    LUMEN_CHECK(m.count() == lx::shell::k_max_tasks);
}

// ── Panel layout ──

LUMEN_TEST(layout_widths_clamp_and_overflow_shows_plus_k) {
    static lx::shell::panel_layout l;
    task_model& m = snapshot_of(2);
    lx::shell::layout_panel(m, 1914, 32, -1, -1, {}, l);
    LUMEN_CHECK(l.count == 2 && l.hidden == 0);
    LUMEN_CHECK(l.cells[0].x == 4 && l.cells[0].w == 220); // capped at 220
    LUMEN_CHECK(l.cells[1].x == 4 + 220 + 4);
    LUMEN_CHECK(l.cell_y == 4 && l.cell_h == 24);

    task_model& many = snapshot_of(10);
    lx::shell::layout_panel(many, 400, 32, -1, -1, {}, l);
    // (400 - 8 - 9*4) / 10 = 35 < 48: does not fit; (392 - 44) / 52 = 6 shown, +4.
    LUMEN_CHECK(l.count == 6 && l.hidden == 4);
    LUMEN_CHECK(l.cells[0].w == 48 && l.more_x == 4 + 6 * 52);

    task_model& some = snapshot_of(5);
    lx::shell::layout_panel(some, 600, 32, -1, -1, {}, l);
    LUMEN_CHECK(l.count == 5 && l.cells[0].w == (592 - 16) / 5);
}

LUMEN_TEST(layout_flags_focus_hover_press_and_hit) {
    static lx::shell::panel_layout l;
    task_model& m = snapshot_of(3);
    task_handle order[3];
    (void)m.ordered(order, 3);
    m.set_focus(order[1]);
    const int x2 = 4 + 2 * 224 + 10;
    lx::shell::layout_panel(m, 1000, 32, x2, 16, order[0], l);
    LUMEN_CHECK(l.cells[0].flags == lx::shell::cell_pressed);
    LUMEN_CHECK(l.cells[1].flags == lx::shell::cell_focused);
    LUMEN_CHECK(l.cells[2].flags == lx::shell::cell_hover);
    LUMEN_CHECK(lx::shell::panel_hit(l, x2, 16) == 2);
    LUMEN_CHECK(lx::shell::panel_hit(l, 2, 16) == -1);           // left padding
    LUMEN_CHECK(lx::shell::panel_hit(l, 4 + 220 + 1, 16) == -1); // gap
    LUMEN_CHECK(lx::shell::panel_hit(l, x2, 1) == -1);           // above the entries
}

LUMEN_TEST(diff_damages_only_changed_entries_or_everything) {
    static lx::shell::panel_layout a, b;
    lx::rect2i r[lx::shell::k_max_panel_cells];
    task_model& m = snapshot_of(3);
    task_handle order[3];
    (void)m.ordered(order, 3);
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, a);
    // Nothing committed yet: everything.
    static lx::shell::panel_layout none;
    LUMEN_CHECK(lx::shell::panel_diff(none, a, r, 8) == 1 && r[0].width == 1000);
    // Unchanged: nothing.
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, b);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 0);
    // Focus moves from none to entry 1: that entry only.
    m.set_focus(order[1]);
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, b);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1);
    LUMEN_CHECK(r[0].x == b.cells[1].x && r[0].width == b.cells[1].w && r[0].y == 4 &&
                r[0].height == 24);
    // A title change on entry 2 plus focus: two rects.
    (void)m.set_state(order[2], "app", "renamed", 0);
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, b);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 2);
    // With room for one rect only: everything instead.
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 1) == 1 && r[0].width == 1000);
    // An entry added: positions move, everything.
    (void)m.add(proxy(7));
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, b);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1 && r[0].width == 1000);
    // A resize: everything.
    lx::shell::layout_panel(m, 1200, 32, -1, -1, {}, a);
    lx::shell::layout_panel(m, 1300, 32, -1, -1, {}, b);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1 && r[0].width == 1300);
}

// ── Logout button ──

LUMEN_TEST(logout_button_takes_the_right_end_and_leaves_entries_the_rest) {
    static lx::shell::panel_layout l;
    task_model& many = snapshot_of(10);
    lx::shell::layout_panel(many, 400, 32, -1, -1, {}, l, lx::shell::panel_logout{true});
    LUMEN_CHECK(l.logout_w == lx::shell::k_logout_width);
    LUMEN_CHECK(l.logout_x == 400 - 4 - lx::shell::k_logout_width);
    // Entries share 400 - 8 - (84 + 4) = 304: (304 - 44) / 52 = 5 shown, +5, all left of it.
    LUMEN_CHECK(l.count == 5 && l.hidden == 5);
    LUMEN_CHECK(l.cells[4].x + l.cells[4].w < l.logout_x);
    LUMEN_CHECK(l.more_x + lx::shell::k_panel_more_width <= l.logout_x);

    // No windows at all: the button is still there.
    task_model& none = snapshot_of(0);
    lx::shell::layout_panel(none, 1000, 32, -1, -1, {}, l, lx::shell::panel_logout{true});
    LUMEN_CHECK(l.count == 0 && l.logout_w == lx::shell::k_logout_width);

    // Not supported by the compositor: no button, entries as before. (snapshot_of reuses
    // one model, so the ten windows are rebuilt.)
    task_model& again = snapshot_of(10);
    lx::shell::layout_panel(again, 400, 32, -1, -1, {}, l);
    LUMEN_CHECK(l.logout_w == 0 && l.count == 6);
}

LUMEN_TEST(logout_button_hit_hover_and_armed_flags) {
    static lx::shell::panel_layout l;
    task_model& m = snapshot_of(2);
    const double bx = 1000 - 4 - lx::shell::k_logout_width + 10;
    lx::shell::layout_panel(m, 1000, 32, bx, 16, {}, l,
                            lx::shell::panel_logout{true, true, false});
    LUMEN_CHECK(lx::shell::panel_hit_logout(l, bx, 16));
    LUMEN_CHECK(!lx::shell::panel_hit_logout(l, bx, 1));   // above the row
    LUMEN_CHECK(!lx::shell::panel_hit_logout(l, 10, 16));  // an entry
    LUMEN_CHECK(lx::shell::panel_hit(l, bx, 16) == -1);    // never counted as an entry
    LUMEN_CHECK(l.logout_flags == (lx::shell::logout_hover | lx::shell::logout_armed));
}

LUMEN_TEST(arming_the_logout_button_damages_only_the_button) {
    static lx::shell::panel_layout a, b;
    task_model& m = snapshot_of(3);
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, a, lx::shell::panel_logout{true});
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, b,
                            lx::shell::panel_logout{true, true, false});
    lx::rect2i r[8];
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1);
    LUMEN_CHECK(r[0].x == b.logout_x && r[0].width == b.logout_w);
    // The button appearing (a compositor that supports logout) re-lays out everything.
    static lx::shell::panel_layout c;
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, c);
    LUMEN_CHECK(lx::shell::panel_diff(c, a, r, 8) == 1 && r[0].width == 1000);
}

LUMEN_TEST(task_output_is_the_output_with_most_of_the_window) {
    const lx::rect2i outs[2] = {{0, 0, 1000, 800}, {1000, 0, 800, 600}};
    LUMEN_CHECK(lx::shell::task_output({100, 100, 300, 200}, outs, 2) == 0);
    LUMEN_CHECK(lx::shell::task_output({1100, 100, 300, 200}, outs, 2) == 1);
    // Straddling: the larger share wins (700 px on the right, 100 on the left).
    LUMEN_CHECK(lx::shell::task_output({900, 100, 800, 200}, outs, 2) == 1);
    // On no output (moved off, or minimized to nowhere): none.
    LUMEN_CHECK(lx::shell::task_output({5000, 5000, 10, 10}, outs, 2) == -1);
    LUMEN_CHECK(lx::shell::task_output({0, 0, 10, 10}, nullptr, 0) == -1);
}

namespace {
struct on_output {
    lx::rect2i outs[2];
    int mine;
};
bool keep_on_output(const lx::shell::task_entry& e, void* user) {
    const auto* o = static_cast<const on_output*>(user);
    return lx::shell::task_output(e.geometry, o->outs, 2) == o->mine;
}
} // namespace

LUMEN_TEST(panel_filter_lists_only_its_outputs_windows) {
    static task_model m;
    m.clear();
    m.begin_snapshot();
    (void)m.snapshot_add(proxy(0), "app", "left", 0, {10, 10, 100, 100}, 0);
    (void)m.snapshot_add(proxy(1), "app", "right", 0, {1200, 10, 100, 100}, 1);
    (void)m.snapshot_add(proxy(2), "app", "left2", 0, {300, 300, 100, 100}, 2);
    m.end_snapshot();
    (void)m.take_changes();
    on_output left{{{0, 0, 1000, 800}, {1000, 0, 800, 600}}, 0};
    on_output right{{{0, 0, 1000, 800}, {1000, 0, 800, 600}}, 1};
    static lx::shell::panel_layout a, b;
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, a, {}, {}, {}, {&keep_on_output, &left});
    lx::shell::layout_panel(m, 800, 32, -1, -1, {}, b, {}, {}, {}, {&keep_on_output, &right});
    LUMEN_CHECK(a.count == 2 && b.count == 1);
    LUMEN_CHECK(std::strcmp(m.get(b.cells[0].task)->title, "right") == 0);
    // No filter: every window.
    static lx::shell::panel_layout c;
    lx::shell::layout_panel(m, 1000, 32, -1, -1, {}, c);
    LUMEN_CHECK(c.count == 3);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
