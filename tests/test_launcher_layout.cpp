// lumen-shell's launcher (pure parts): its size on an output, row layout, scrolling, hit
// testing, the diff that limits repaint and damage, the query/selection model driven by
// keysyms — and the panel's Apps button and notice cell.
#include "lumen_test.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.ui;
import lx.shell;

namespace {

using lx::shell::launcher_key_result;
using lx::shell::launcher_layout;
using lx::shell::launcher_view;

constexpr unsigned k_escape = 0xff1b, k_down = 0xff54, k_up = 0xff52, k_page_down = 0xff56,
                   k_end = 0xff57, k_home = 0xff50, k_menu = 0xff67, k_f10 = 0xffc7;

std::string g_root;

/// A catalog of `n` apps named "App 00" … plus "Foot" and "Firefox".
lx::shell::app_catalog& catalog_of(unsigned n) {
    static lx::shell::app_catalog c;
    char tmpl[] = "/tmp/lumen-launcher-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    g_root = tmpl;
    (void)mkdir((g_root + "/applications").c_str(), 0755);
    auto put = [](const std::string& id, const std::string& name) {
        FILE* f = std::fopen((g_root + "/applications/" + id).c_str(), "w");
        std::fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=/bin/true\n",
                     name.c_str());
        std::fclose(f);
    };
    for (unsigned i = 0; i < n; ++i) {
        char id[32], name[32];
        std::snprintf(id, sizeof(id), "app%02u.desktop", i);
        std::snprintf(name, sizeof(name), "App %02u", i);
        put(id, name);
    }
    put("foot.desktop", "Foot");
    put("firefox.desktop", "Firefox");
    lx::shell::catalog_config cfg{};
    cfg.data_home = g_root;
    cfg.path_env = "/usr/bin:/bin";
    c.configure(cfg);
    c.scan();
    (void)std::system(("rm -rf '" + g_root + "'").c_str());
    return c;
}

std::string name_of(const lx::shell::app_catalog& c, int entry) {
    return entry >= 0 ? std::string(c.entry(static_cast<unsigned>(entry))->name) : "";
}

} // namespace

LUMEN_TEST(launcher_size_fits_the_output) {
    auto g = lx::shell::launcher_size(1914, 924, 32);
    // (924 - 32 - 72) / 32 = 25 → capped at 14.
    LUMEN_CHECK(g.width == 420 && g.rows == 14 && g.height == 40 + 14 * 32 + 24);
    g = lx::shell::launcher_size(400, 400, 32);
    // (368 - 72) / 32 = 9 rows; the width is the output's less the margins.
    LUMEN_CHECK(g.width == 392 && g.rows == 9);
    g = lx::shell::launcher_size(800, 200, 32);
    LUMEN_CHECK(g.rows == 4); // never fewer than 4
}

LUMEN_TEST(layout_rows_flags_hit_and_rects) {
    const unsigned matches[] = {7, 3, 9, 1, 4};
    launcher_view v{};
    v.matches = matches;
    v.count = 5;
    v.selection = 1;
    v.first = 0;
    v.hover = 2;
    v.pressed = 1;
    static launcher_layout l;
    lx::shell::layout_launcher(v, 420, 40 + 4 * 32 + 24, 4, l);
    LUMEN_CHECK(l.rows == 4 && l.total == 5);
    LUMEN_CHECK(l.row[0].entry == 7 && l.row[0].flags == 0);
    LUMEN_CHECK(l.row[1].entry == 3 && l.row[1].flags == (lx::shell::row_selected | lx::shell::row_pressed));
    LUMEN_CHECK(l.row[2].flags == lx::shell::row_hover);
    LUMEN_CHECK(lx::shell::launcher_hit(l, 10, 10) == -1);           // search row
    LUMEN_CHECK(lx::shell::launcher_hit(l, 10, 40 + 32 + 5) == 1);  // second row
    LUMEN_CHECK(lx::shell::launcher_hit(l, 10, 40 + 4 * 32 + 5) == -1); // status row
    LUMEN_CHECK(lx::shell::launcher_hit(l, 500, 50) == -1);
    const auto r = lx::shell::launcher_row_rect(l, 3);
    LUMEN_CHECK(r.y == 40 + 96 && r.height == 32 && r.width == 420);
    LUMEN_CHECK(lx::shell::launcher_status_rect(l).y == 40 + 128);
    // Scrolled: hit returns match indices, not row indices; past the end rows are empty.
    v.first = 3;
    lx::shell::layout_launcher(v, 420, 40 + 4 * 32 + 24, 4, l);
    LUMEN_CHECK(l.row[0].entry == 1 && l.row[1].entry == 4 && l.row[2].entry == ~0u);
    LUMEN_CHECK(lx::shell::launcher_hit(l, 10, 40 + 5) == 3);
    LUMEN_CHECK(lx::shell::launcher_hit(l, 10, 40 + 64 + 5) == -1);
}

LUMEN_TEST(scroll_keeps_the_selection_visible) {
    LUMEN_CHECK(lx::shell::launcher_scroll(0, 0, 4, 10) == 0);
    LUMEN_CHECK(lx::shell::launcher_scroll(4, 0, 4, 10) == 1);
    LUMEN_CHECK(lx::shell::launcher_scroll(9, 0, 4, 10) == 6);
    LUMEN_CHECK(lx::shell::launcher_scroll(2, 5, 4, 10) == 2);
    LUMEN_CHECK(lx::shell::launcher_scroll(3, 0, 4, 3) == 0); // everything fits
    LUMEN_CHECK(lx::shell::launcher_scroll(5, 8, 4, 10) == 5);
}

LUMEN_TEST(diff_repaints_only_what_changed) {
    const unsigned matches[] = {7, 3, 9, 1, 4};
    launcher_view v{};
    v.matches = matches;
    v.count = 5;
    static launcher_layout a, b, none;
    lx::rect2i r[20];
    lx::shell::layout_launcher(v, 420, 200, 4, a);
    LUMEN_CHECK(lx::shell::launcher_diff(none, a, r, 20) == 1 && r[0].height == 200);
    lx::shell::layout_launcher(v, 420, 200, 4, b);
    LUMEN_CHECK(lx::shell::launcher_diff(a, b, r, 20) == 0);
    // Selection moves from row 0 to row 1: those two rows.
    v.selection = 1;
    lx::shell::layout_launcher(v, 420, 200, 4, b);
    LUMEN_CHECK(lx::shell::launcher_diff(a, b, r, 20) == 2);
    LUMEN_CHECK(r[0].y == 40 && r[1].y == 72);
    // A typed character: the search row, every row whose entry changed, the status row.
    const unsigned fewer[] = {7};
    v.matches = fewer;
    v.count = 1;
    v.selection = 0;
    v.query_rev = 1;
    lx::shell::layout_launcher(v, 420, 200, 4, b);
    const unsigned n = lx::shell::launcher_diff(a, b, r, 20);
    LUMEN_CHECK(n == 1 + 3 + 1); // search, rows 1–3 emptied, status
    LUMEN_CHECK(r[0].y == 0 && r[0].height == 40 && r[n - 1].y == 40 + 128);
    // A rescanned catalog: everything (entry indices are a new generation's).
    v.catalog_gen = 9;
    lx::shell::layout_launcher(v, 420, 200, 4, b);
    LUMEN_CHECK(lx::shell::launcher_diff(a, b, r, 20) == 1 && r[0].height == 200);
    // No room for every rect: everything.
    v.catalog_gen = 0;
    lx::shell::layout_launcher(v, 420, 200, 4, b);
    LUMEN_CHECK(lx::shell::launcher_diff(a, b, r, 2) == 1 && r[0].height == 200);
}

LUMEN_TEST(model_typing_selection_and_keys) {
    auto& cat = catalog_of(20);
    // The query is a text_buffer (the view's search field edits it); typing is its job.
    char query_storage[lx::shell::launcher_model::k_max_query + 1];
    lx::ui::text_buffer query{.storage = query_storage, .size = 0, .revision = 0};
    query_storage[0] = '\0';
    const auto type = [&query](const char* text) {
        LUMEN_CHECK(query.assign(text));
    };
    lx::shell::launcher_model m;
    m.bind(&cat, &query);
    m.set_rows(4);
    m.reset();
    LUMEN_CHECK(m.count() == 22 && m.selection() == 0);
    LUMEN_CHECK(name_of(cat, m.selected_entry()) == "App 00"); // alphabetical
    // Down past the page scrolls; End and Home; Page_Down by a page.
    for (int i = 0; i < 5; ++i)
        LUMEN_CHECK(m.key(k_down, false) == launcher_key_result::changed);
    LUMEN_CHECK(m.selection() == 5 && m.first() == 2);
    (void)m.key(k_end, false);
    LUMEN_CHECK(m.selection() == 21 && m.first() == 18);
    (void)m.key(k_home, false);
    LUMEN_CHECK(m.selection() == 0 && m.first() == 0);
    (void)m.key(k_page_down, false);
    LUMEN_CHECK(m.selection() == 4);
    (void)m.key(k_up, false);
    LUMEN_CHECK(m.selection() == 3);
    // A query filters and puts the selection on the best match (sync_query re-ranks).
    const unsigned rev = query.revision;
    type("f");
    m.sync_query();
    LUMEN_CHECK(m.query_rev() == rev + 1 && std::strcmp(m.query(), "f") == 0);
    LUMEN_CHECK(m.count() == 2 && m.selection() == 0);
    type("fo");
    m.sync_query();
    // Foot by prefix first, then Firefox (which contains "fo").
    LUMEN_CHECK(m.count() == 2 && name_of(cat, m.selected_entry()) == "Foot");
    // The selected application's menu: Menu, Shift+F10 (F10 alone is nothing).
    LUMEN_CHECK(m.key(k_menu, false) == launcher_key_result::menu);
    LUMEN_CHECK(m.key(k_f10, true) == launcher_key_result::menu);
    LUMEN_CHECK(m.key(k_f10, false) == launcher_key_result::ignored);
    // An empty query lists everything again.
    type("");
    m.sync_query();
    LUMEN_CHECK(m.count() == 22 && m.query()[0] == '\0');
    // Escape: clears a query (the view empties the buffer), closes when it is empty.
    type("a");
    m.sync_query();
    LUMEN_CHECK(m.key(k_escape, false) == launcher_key_result::cleared);
    LUMEN_CHECK(query.assign(""));
    m.sync_query();
    LUMEN_CHECK(m.key(k_escape, false) == launcher_key_result::close);
    // The query is capped (the buffer's capacity is the model's k_max_query).
    char too_long[lx::shell::launcher_model::k_max_query + 2];
    for (std::size_t i = 0; i < sizeof(too_long) - 1; ++i)
        too_long[i] = 'a';
    too_long[sizeof(too_long) - 1] = '\0';
    LUMEN_CHECK(!query.assign(too_long));
    // Wheel scrolling moves the page, not the selection.
    m.reset();
    m.scroll(3);
    LUMEN_CHECK(m.first() == 3 && m.selection() == 0);
    m.scroll(100);
    LUMEN_CHECK(m.first() == 22 - 4);
    m.scroll(-100);
    LUMEN_CHECK(m.first() == 0);
}

// ── The panel's Apps button and notice ──

LUMEN_TEST(panel_apps_button_takes_the_left_end) {
    static lx::shell::task_model model;
    model.begin_snapshot();
    (void)model.snapshot_add(reinterpret_cast<void*>(0x1000), "a", "one", 0, {0, 0, 1, 1}, 0);
    (void)model.snapshot_add(reinterpret_cast<void*>(0x2000), "a", "two", 0, {0, 0, 1, 1}, 1);
    model.end_snapshot();
    static lx::shell::panel_layout l, plain;
    lx::shell::layout_panel(model, 1000, 32, -1, -1, {}, plain);
    lx::shell::layout_panel(model, 1000, 32, 10, 16, {}, l, {}, lx::shell::panel_apps{true, true, false});
    LUMEN_CHECK(l.apps_x == 4 && l.apps_w == lx::shell::k_apps_width);
    LUMEN_CHECK(l.apps_flags == (lx::shell::apps_hover | lx::shell::apps_open));
    LUMEN_CHECK(l.cells[0].x == 4 + lx::shell::k_apps_width + 4); // tasks after it
    LUMEN_CHECK(plain.cells[0].x == 4 && plain.apps_w == 0);      // unchanged without it
    LUMEN_CHECK(lx::shell::panel_hit_apps(l, 10, 16) && !lx::shell::panel_hit_apps(l, 10, 1));
    LUMEN_CHECK(lx::shell::panel_hit(l, 10, 16) == -1); // never counted as an entry
    // Hover leaving the button damages it alone; its appearance re-lays out everything.
    static lx::shell::panel_layout m;
    lx::shell::layout_panel(model, 1000, 32, -1, -1, {}, m, {}, lx::shell::panel_apps{true, true, false});
    lx::rect2i r[8];
    LUMEN_CHECK(lx::shell::panel_diff(l, m, r, 8) == 1 && r[0].x == l.apps_x &&
                r[0].width == l.apps_w);
    LUMEN_CHECK(lx::shell::panel_diff(plain, m, r, 8) == 1 && r[0].width == 1000);
}

LUMEN_TEST(panel_notice_replaces_the_task_entries) {
    static lx::shell::task_model model;
    model.begin_snapshot();
    (void)model.snapshot_add(reinterpret_cast<void*>(0x1000), "a", "one", 0, {0, 0, 1, 1}, 0);
    model.end_snapshot();
    static lx::shell::panel_layout a, b;
    lx::shell::layout_panel(model, 1000, 32, -1, -1, {}, a, lx::shell::panel_logout{true},
                            lx::shell::panel_apps{true}, lx::shell::panel_notice{true, 1});
    LUMEN_CHECK(a.count == 0 && a.notice_x == 4 + 64 + 4);
    LUMEN_CHECK(a.notice_x + a.notice_w + 4 == a.logout_x); // up to the logout button
    LUMEN_CHECK(lx::shell::panel_hit_notice(a, a.notice_x + 5, 16));
    // A new text: the notice cell alone.
    lx::shell::layout_panel(model, 1000, 32, -1, -1, {}, b, lx::shell::panel_logout{true},
                            lx::shell::panel_apps{true}, lx::shell::panel_notice{true, 2});
    lx::rect2i r[8];
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1 && r[0].x == a.notice_x &&
                r[0].width == a.notice_w);
    // Cleared: the entries come back (a re-layout).
    lx::shell::layout_panel(model, 1000, 32, -1, -1, {}, b, lx::shell::panel_logout{true},
                            lx::shell::panel_apps{true});
    LUMEN_CHECK(b.count == 1 && b.notice_w == 0);
    LUMEN_CHECK(lx::shell::panel_diff(a, b, r, 8) == 1 && r[0].width == 1000);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
