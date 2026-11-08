// Calendar contracts use published civil-date examples and observable panel geometry.
#include <locale.h>
#include <stdlib.h>
#include <time.h>
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.config;
import lx.shell;

namespace {
using namespace std::chrono;
using lx::shell::calendar_model;

// The runner is serial. Restore process-global formatting state even after an assertion.
struct clock_environment {
    std::string locale = setlocale(LC_TIME, nullptr);
    std::string timezone = getenv("TZ") ? getenv("TZ") : "";
    bool had_timezone = getenv("TZ") != nullptr;
    clock_environment(const char* zone = "UTC0") {
        LUMEN_CHECK(setlocale(LC_TIME, "C") != nullptr);
        LUMEN_CHECK(setenv("TZ", zone, 1) == 0);
        tzset();
    }
    ~clock_environment() {
        (void)setlocale(LC_TIME, locale.c_str());
        if (had_timezone) (void)setenv("TZ", timezone.c_str(), 1);
        else (void)unsetenv("TZ");
        tzset();
    }
};

std::time_t instant(year_month_day date, seconds time = seconds{0}) {
    return system_clock::to_time_t(sys_days{date} + time);
}

lx::shell::panel_clock panel_clock(int width, unsigned revision = 1) {
    lx::shell::panel_clock clock{};
    std::snprintf(clock.text, sizeof(clock.text), "Mon, 01 Jan  12:34");
    clock.width = width;
    clock.rev = revision;
    return clock;
}

bool damaged(const lx::rect2i* rects, unsigned count, int x, int y) {
    for (unsigned i = 0; i < count; ++i)
        if (x >= rects[i].x && x < rects[i].x + rects[i].width &&
            y >= rects[i].y && y < rects[i].y + rects[i].height)
            return true;
    return false;
}

void clock_layout(const lx::shell::task_model& tasks, int width,
                  lx::shell::panel_layout& layout, lx::shell::panel_clock clock) {
    lx::shell::layout_panel(tasks, width, 32, -1, -1, {}, layout, {}, {}, {}, {}, false,
                            nullptr, nullptr, 0, {}, false, clock);
}
} // namespace

LUMEN_TEST(calendar_leap_centuries_and_month_end_clamping) {
    calendar_model m;
    m.reset(2000y/January/31);
    m.move_months(1);
    LUMEN_CHECK(m.selected == 2000y/February/29);
    m.move_days(1);
    LUMEN_CHECK(m.selected == 2000y/March/1);
    m.reset(2100y/January/31);
    m.move_months(1);
    LUMEN_CHECK(m.selected == 2100y/February/28);
    m.move_days(1);
    LUMEN_CHECK(m.selected == 2100y/March/1);
    m.reset(2024y/March/31);
    m.move_months(-1);
    LUMEN_CHECK(m.selected == 2024y/February/29);
    m.reset(2024y/February/29);
    m.move_months(12);
    LUMEN_CHECK(m.selected == 2025y/February/28);
    m.reset(2024y/February/29);
    m.move_months(-12);
    LUMEN_CHECK(m.selected == 2023y/February/28);
    m.reset(2024y/December/31);
    m.move_months(2);
    LUMEN_CHECK(m.selected == 2025y/February/28);
}

LUMEN_TEST(calendar_grid_has_42_consecutive_dates_and_exact_month_membership) {
    // Exercise every weekday alignment, leap centuries, and both visual week starts.
    for (int y : {2000, 2023, 2024, 2100}) {
        for (unsigned month_number = 1; month_number <= 12; ++month_number) {
            for (auto first : {lx::config::clock_first_day::monday,
                               lx::config::clock_first_day::sunday}) {
                calendar_model m;
                m.first_day = first;
                const auto date = year{y}/month{month_number}/15;
                m.reset(date);
                LUMEN_CHECK(m.cells.size() == 42);
                const auto start = sys_days{m.cells.front().date};
                const auto end = sys_days{m.cells.back().date};
                LUMEN_CHECK(end - start == days{41});
                LUMEN_CHECK(weekday{start} == (first == lx::config::clock_first_day::monday
                                                   ? Monday : Sunday));
                LUMEN_CHECK(start <= sys_days{year{y}/month{month_number}/1});
                LUMEN_CHECK(start + days{7} > sys_days{year{y}/month{month_number}/1});
                unsigned in_month = 0, today = 0, selected = 0;
                for (unsigned i = 0; i < 42; ++i) {
                    const auto& cell = m.cells[i];
                    LUMEN_CHECK(cell.date.ok());
                    LUMEN_CHECK(sys_days{cell.date} == start + days{i});
                    LUMEN_CHECK(cell.in_month == (cell.date.year() == year{y} &&
                                                   cell.date.month() == month{month_number}));
                    LUMEN_CHECK(cell.today == (cell.date == date));
                    LUMEN_CHECK(cell.selected == (cell.date == date));
                    in_month += cell.in_month;
                    today += cell.today;
                    selected += cell.selected;
                }
                LUMEN_CHECK(in_month == unsigned(year_month_day_last{year{y}/month{month_number}/last}.day()));
                LUMEN_CHECK(today == 1 && selected == 1);
            }
        }
    }
}

LUMEN_TEST(calendar_known_week_starts_selection_and_today_are_independent) {
    calendar_model m;
    m.reset(2024y/September/15); // September begins on Sunday.
    LUMEN_CHECK(m.cells.front().date == 2024y/August/26);
    LUMEN_CHECK(m.cells.back().date == 2024y/October/6);
    m.first_day = lx::config::clock_first_day::sunday;
    m.rebuild();
    LUMEN_CHECK(m.cells.front().date == 2024y/September/1);
    LUMEN_CHECK(m.cells.back().date == 2024y/October/12);
    m.select(2024y/October/1);
    LUMEN_CHECK(m.today == 2024y/September/15);
    LUMEN_CHECK(m.displayed == 2024y/October);
    m.go_today();
    LUMEN_CHECK(m.selected == 2024y/September/15);
    LUMEN_CHECK(m.displayed == 2024y/September);
}

LUMEN_TEST(calendar_rejects_invalid_dates_and_navigation_outside_supported_years) {
    calendar_model m;
    m.reset(1y/January/1);
    m.move_days(-1);
    m.move_months(-1);
    m.move_months(-12);
    LUMEN_CHECK(m.selected == 1y/January/1);
    m.select(0y/December/31);
    LUMEN_CHECK(m.selected == 1y/January/1);
    m.reset(9999y/December/31);
    m.move_days(1);
    m.move_months(1);
    m.move_months(12);
    LUMEN_CHECK(m.selected == 9999y/December/31);
    m.select(year{10000}/January/1);
    m.select(2100y/February/29);
    LUMEN_CHECK(m.selected == 9999y/December/31);
    m.reset(year{0}/January/1);
    LUMEN_CHECK(m.today == 9999y/December/31);
    LUMEN_CHECK(m.selected == 9999y/December/31);
}

LUMEN_TEST(calendar_iso_week_uses_iso_year_at_new_year) {
    struct example { year_month_day date; unsigned week; };
    const example cases[] = {
        {2015y/December/31, 53}, {2016y/January/1, 53}, {2016y/January/3, 53},
        {2016y/January/4, 1}, {2018y/December/31, 1}, {2020y/December/31, 53},
        {2021y/January/1, 53}, {2021y/January/3, 53}, {2021y/January/4, 1},
        {2022y/January/1, 52}, {2023y/January/1, 52}, {2024y/December/30, 1}
    };
    for (auto test : cases)
        LUMEN_CHECK(lx::shell::calendar_iso_week(test.date) == test.week);
    LUMEN_CHECK(lx::shell::calendar_iso_week(2100y/February/29) == 0);
    LUMEN_CHECK(lx::shell::calendar_iso_week(0y/January/1) == 0);
    LUMEN_CHECK(lx::shell::calendar_iso_week(year{10000}/January/1) == 0);
}

LUMEN_TEST(clock_formats_12_and_24_hour_midnight_noon_seconds_and_date_flags) {
    clock_environment environment;
    lx::config::clock_settings settings{};
    settings.show_date = false;
    settings.show_weekday = false;
    settings.show_seconds = false;
    settings.format = lx::config::clock_format::h24;
    const auto midnight = instant(2024y/February/29);
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(midnight, settings).panel, "00:00") == 0);
    settings.format = lx::config::clock_format::h12;
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(midnight, settings).panel, "12:00 AM") == 0);
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(midnight + 12*3600, settings).panel, "12:00 PM") == 0);
    const auto afternoon = midnight + 13*3600 + 5*60 + 9;
    settings.show_seconds = true;
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(afternoon, settings).panel, "01:05:09 PM") == 0);
    settings.format = lx::config::clock_format::h24;
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(afternoon, settings).panel, "13:05:09") == 0);
    settings.show_date = true;
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(afternoon, settings).panel, "29 Feb  13:05:09") == 0);
    settings.show_weekday = true;
    const auto full = lx::shell::format_clock(afternoon, settings);
    LUMEN_CHECK(std::strcmp(full.panel, "Thu, 29 Feb  13:05:09") == 0);
    LUMEN_CHECK(std::strcmp(full.heading, "Thursday, 29 February 2024  13:05:09") == 0);
    settings.show_date = false;
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(afternoon, settings).panel, "Thu  13:05:09") == 0);
}

LUMEN_TEST(clock_12_hour_midnight_and_noon_outputs_are_bounded_and_terminated) {
    clock_environment environment;
    lx::config::clock_settings settings{};
    settings.format = lx::config::clock_format::h12;
    settings.show_date = false;
    settings.show_weekday = false;
    const auto midnight = instant(2024y/February/29);
    struct example { std::time_t offset; bool seconds; const char* expected; };
    const example cases[] = {
        {0, false, "12:00 AM"}, {12*3600, false, "12:00 PM"},
        {9, true, "12:00:09 AM"}, {12*3600 + 9, true, "12:00:09 PM"}
    };
    for (auto test : cases) {
        settings.show_seconds = test.seconds;
        const auto text = lx::shell::format_clock(midnight + test.offset, settings);
        const auto* panel_end = std::find(std::begin(text.panel), std::end(text.panel), '\0');
        const auto* heading_end = std::find(std::begin(text.heading), std::end(text.heading), '\0');
        LUMEN_CHECK(panel_end != std::end(text.panel));
        LUMEN_CHECK(heading_end != std::end(text.heading));
        LUMEN_CHECK(std::strcmp(text.panel, test.expected) == 0);
        LUMEN_CHECK(std::string_view{text.heading}.ends_with(test.expected));
    }
}

LUMEN_TEST(clock_uses_explicit_local_timezone_for_text_and_date_rollover) {
    clock_environment environment{"EST5"}; // POSIX fixed UTC-5; no zone database needed.
    lx::config::clock_settings settings{};
    settings.format = lx::config::clock_format::h24;
    settings.show_date = false;
    settings.show_weekday = false;
    settings.show_seconds = false;
    const auto now = instant(2024y/January/1, hours{2});
    LUMEN_CHECK(lx::shell::clock_local_date(now) == 2023y/December/31);
    LUMEN_CHECK(std::strcmp(lx::shell::format_clock(now, settings).panel, "21:00") == 0);
}

LUMEN_TEST(clock_deadlines_are_strictly_next_realtime_second_or_minute) {
    struct example { long long now, second, minute; };
    const example cases[] = {
        {0, 1'000'000'000, 60'000'000'000},
        {1, 1'000'000'000, 60'000'000'000},
        {999'999'999, 1'000'000'000, 60'000'000'000},
        {1'000'000'000, 2'000'000'000, 60'000'000'000},
        {59'999'999'999, 60'000'000'000, 60'000'000'000},
        {60'000'000'000, 61'000'000'000, 120'000'000'000},
        {-1, 0, 0}, {-1'000'000'000, 0, 0},
        {-60'000'000'001, -60'000'000'000, -60'000'000'000}
    };
    for (auto test : cases) {
        LUMEN_CHECK(lx::shell::clock_next_boundary(test.now, true) == test.second);
        LUMEN_CHECK(lx::shell::clock_next_boundary(test.now, false) == test.minute);
    }
}

LUMEN_TEST(panel_clock_damage_covers_old_and_new_width_and_unchanged_is_idle) {
    static lx::shell::task_model tasks;
    tasks.clear();
    lx::shell::panel_layout old_layout{}, new_layout{};
    clock_layout(tasks, 800, old_layout, panel_clock(64));
    clock_layout(tasks, 800, new_layout, panel_clock(240, 2));
    LUMEN_CHECK(old_layout.clock_w == 64 && new_layout.clock_w == 240);
    lx::rect2i rects[16];
    LUMEN_CHECK(lx::shell::panel_diff(old_layout, old_layout, rects, 16) == 0);
    for (bool grow : {true, false}) {
        const auto& from = grow ? old_layout : new_layout;
        const auto& to = grow ? new_layout : old_layout;
        const unsigned count = lx::shell::panel_diff(from, to, rects, 16);
        LUMEN_CHECK(count > 0);
        for (const auto* layout : {&from, &to})
            for (int x = layout->clock_x; x < layout->clock_x + layout->clock_w; ++x)
                LUMEN_CHECK(damaged(rects, count, x, layout->cell_y + layout->cell_h/2));
    }
    clock_layout(tasks, 800, new_layout, panel_clock(64, 2));
    const unsigned count = lx::shell::panel_diff(old_layout, new_layout, rects, 16);
    LUMEN_CHECK(count > 0);
    LUMEN_CHECK(damaged(rects, count, old_layout.clock_x, old_layout.cell_y));
    LUMEN_CHECK(!damaged(rects, count, 0, 0));
    LUMEN_CHECK(lx::shell::panel_hit(new_layout, new_layout.clock_x + 1, 16) == -1);
}

LUMEN_TEST(panel_clock_committed_copy_preserves_hit_geometry_and_damage_state) {
    static lx::shell::task_model tasks;
    tasks.clear();
    lx::shell::panel_layout next{}, committed{}, hidden{};
    auto clock = panel_clock(160, 42);
    clock.flags = lx::shell::bell_flag_hover | lx::shell::bell_flag_pressed;
    clock_layout(tasks, 800, next, clock);
    lx::shell::copy_layout(committed, next);
    LUMEN_CHECK(committed.clock_x == next.clock_x && committed.clock_x > 0);
    LUMEN_CHECK(committed.clock_w == 160);
    LUMEN_CHECK(committed.clock_rev == 42);
    LUMEN_CHECK(committed.clock_flags == clock.flags);
    // Input routes using committed geometry after the frame is published.
    const int clicked_x = next.clock_x + next.clock_w/2;
    LUMEN_CHECK(clicked_x >= committed.clock_x &&
                clicked_x < committed.clock_x + committed.clock_w);
    LUMEN_CHECK(lx::shell::panel_hit(committed, clicked_x, 16) == -1);
    lx::rect2i rects[16];
    LUMEN_CHECK(lx::shell::panel_diff(committed, next, rects, 16) == 0);

    ++clock.rev;
    clock_layout(tasks, 800, next, clock);
    const unsigned changed = lx::shell::panel_diff(committed, next, rects, 16);
    LUMEN_CHECK(changed > 0);
    LUMEN_CHECK(damaged(rects, changed, clicked_x, 16));
    LUMEN_CHECK(!damaged(rects, changed, 0, 0));
    lx::shell::copy_layout(committed, next);
    LUMEN_CHECK(committed.clock_rev == 43);
    LUMEN_CHECK(lx::shell::panel_diff(committed, next, rects, 16) == 0);

    clock_layout(tasks, 800, hidden, {});
    const unsigned removed = lx::shell::panel_diff(committed, hidden, rects, 16);
    LUMEN_CHECK(removed > 0);
    LUMEN_CHECK(damaged(rects, removed, clicked_x, 16));
    lx::shell::copy_layout(committed, hidden);
    LUMEN_CHECK(committed.clock_x == 0 && committed.clock_w == 0);
    LUMEN_CHECK(committed.clock_rev == 0 && committed.clock_flags == 0);
    LUMEN_CHECK(lx::shell::panel_diff(committed, hidden, rects, 16) == 0);
}

LUMEN_TEST(panel_clock_coexists_with_controls_and_clears_when_hidden_on_narrow_layout) {
    static lx::shell::task_model tasks;
    tasks.clear();
    for (unsigned i = 0; i < 12; ++i)
        (void)tasks.add(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + i*16)));
    lx::shell::panel_tray_input tray{};
    tray.count = 2;
    tray.items[0].key = 1;
    tray.items[1].key = 2;
    lx::shell::panel_ws_input ws{};
    ws.count = 2;
    std::snprintf(ws.items[0].name, sizeof(ws.items[0].name), "1");
    std::snprintf(ws.items[1].name, sizeof(ws.items[1].name), "2");
    lx::shell::panel_layout layout{};
    for (int width : {1200, 640, 400, 240}) {
        lx::shell::layout_panel(tasks, width, 32, -1, -1, {}, layout, {true}, {true}, {}, {},
                                true, &ws, &tray, 0, {true}, false, panel_clock(160));
        std::vector<std::pair<int, int>> intervals;
        auto add = [&](int x, int w) { if (w > 0) intervals.emplace_back(x, x+w); };
        add(layout.apps_x, layout.apps_w);
        add(layout.logout_x, layout.logout_w);
        add(layout.capture_x, layout.capture_w);
        add(layout.bell_x, layout.bell_w);
        add(layout.clock_x, layout.clock_w);
        for (unsigned i = 0; i < layout.ws_count; ++i) add(layout.ws[i].x, layout.ws[i].w);
        for (unsigned i = 0; i < layout.tray_count; ++i) add(layout.tray[i].x, layout.tray[i].w);
        for (unsigned i = 0; i < layout.count; ++i) add(layout.cells[i].x, layout.cells[i].w);
        if (layout.hidden) add(layout.more_x, lx::shell::k_panel_more_width);
        std::sort(intervals.begin(), intervals.end());
        for (unsigned i = 0; i < intervals.size(); ++i) {
            LUMEN_CHECK(intervals[i].first >= 0 && intervals[i].second <= width);
            if (i) LUMEN_CHECK(intervals[i-1].second <= intervals[i].first);
        }
        if (width == 1200) LUMEN_CHECK(layout.clock_w == 160);
        if (width == 240) LUMEN_CHECK(layout.clock_w == 0);
    }
    // Reuse the output object: hiding the clock must clear its previous geometry/revision.
    clock_layout(tasks, 800, layout, panel_clock(160, 99));
    clock_layout(tasks, 800, layout, {});
    LUMEN_CHECK(layout.clock_w == 0 && layout.clock_x == 0 && layout.clock_rev == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
