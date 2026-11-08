module;
#include <time.h>
import lx.config;
export module lx.shell:clock;
import std;
export namespace lx::shell {
struct calendar_day {
    std::chrono::year_month_day date{};
    bool in_month = false;
    bool today = false;
    bool selected = false;
};
/// Gregorian civil-date model. ISO week labels always use Monday/Thursday ISO rules.
class calendar_model {
public:
    std::chrono::year_month_day today{};
    std::chrono::year_month_day selected{};
    std::chrono::year_month displayed{};
    std::array<calendar_day, 42> cells{};
    lx::config::clock_first_day first_day = lx::config::clock_first_day::monday;
    void reset(std::chrono::year_month_day d) {
        if (!valid_date(d))
            return;
        today = selected = d;
        displayed = d.year() / d.month();
        rebuild();
    }
    [[nodiscard]] static bool valid_date(std::chrono::year_month_day d) {
        return d.ok() && int(d.year()) >= 1 && int(d.year()) <= 9999;
    }
    void rebuild() {
        using namespace std::chrono;
        if (!displayed.ok() || int(displayed.year()) < 1 || int(displayed.year()) > 9999)
            return;
        const sys_days first{displayed / 1};
        const unsigned weekday_index = weekday{first}.c_encoding();
        const unsigned offset = first_day == lx::config::clock_first_day::monday
                                    ? (weekday_index + 6) % 7
                                    : weekday_index;
        for (unsigned i = 0; i < cells.size(); ++i) {
            const year_month_day d{first + days{static_cast<int>(i) - static_cast<int>(offset)}};
            cells[i] = {d, d.month() == displayed.month() && d.year() == displayed.year(),
                        d == today, d == selected};
        }
    }
    void select(std::chrono::year_month_day d) {
        if (!valid_date(d))
            return;
        selected = d;
        displayed = d.year() / d.month();
        rebuild();
    }
    void move_days(int n) {
        using namespace std::chrono;
        const year_month_day next{sys_days{selected} + days{n}};
        if (!next.ok() || int(next.year()) < 1 || int(next.year()) > 9999)
            return;
        select(next);
    }
    void move_months(int n) {
        using namespace std::chrono;
        const year_month ym = displayed + months{n};
        if (int(ym.year()) < 1 || int(ym.year()) > 9999)
            return;
        auto d = ym / selected.day();
        if (!d.ok())
            d = year_month_day{ym / last};
        select(d);
    }
    void go_today() { select(today); }
};
[[nodiscard]] unsigned calendar_iso_week(std::chrono::year_month_day d) {
    if (!calendar_model::valid_date(d))
        return 0;
    using namespace std::chrono;
    const sys_days day{d};
    const sys_days thursday = day + days{4 - static_cast<int>(weekday{day}.iso_encoding())};
    const year y = year_month_day{thursday}.year();
    const sys_days jan4{y / January / 4};
    const sys_days start = jan4 - days{static_cast<int>(weekday{jan4}.iso_encoding()) - 1};
    return static_cast<unsigned>((thursday - start).count() / 7 + 1);
}
[[nodiscard]] std::chrono::year_month_day clock_local_date(std::time_t now) {
    std::tm t{};
    if (!localtime_r(&now, &t))
        return {};
    const auto date = std::chrono::year{t.tm_year + 1900} / std::chrono::month{static_cast<unsigned>(t.tm_mon + 1)} /
                      std::chrono::day{static_cast<unsigned>(t.tm_mday)};
    return calendar_model::valid_date(date) ? date : std::chrono::year_month_day{};
}
/// Fixed-size text, refreshed at wall boundaries rather than on frame construction.
struct clock_text {
    char panel[96]{};
    char heading[96]{};
};
} // namespace lx::shell

namespace {
using lx::shell::clock_text;
[[nodiscard]] clock_text format_clock_ascii(std::tm t, const lx::config::clock_settings& s) {
    static constexpr const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* full_weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static constexpr const char* full_months[] = {"January", "February", "March", "April", "May", "June",
                                                  "July", "August", "September", "October", "November", "December"};
    clock_text out{};
    if (t.tm_wday < 0 || t.tm_wday >= 7 || t.tm_mon < 0 || t.tm_mon >= 12)
        return out;
    char time[32]{};
    const unsigned hour = static_cast<unsigned>(t.tm_hour);
    if (s.format == lx::config::clock_format::h12) {
        const unsigned h = hour % 12 ? hour % 12 : 12;
        if (s.show_seconds) {
            std::snprintf(time, sizeof(time), "%02u:%02u:%02u %s", h,
                          static_cast<unsigned>(t.tm_min), static_cast<unsigned>(t.tm_sec),
                          hour < 12 ? "AM" : "PM");
        } else {
            std::snprintf(time, sizeof(time), "%02u:%02u %s", h,
                          static_cast<unsigned>(t.tm_min), hour < 12 ? "AM" : "PM");
        }
    } else if (s.show_seconds) {
        std::snprintf(time, sizeof(time), "%02u:%02u:%02u", hour,
                      static_cast<unsigned>(t.tm_min), static_cast<unsigned>(t.tm_sec));
    } else {
        std::snprintf(time, sizeof(time), "%02u:%02u", hour, static_cast<unsigned>(t.tm_min));
    }
    char date[48]{};
    if (s.show_date && s.show_weekday)
        std::snprintf(date, sizeof(date), "%s, %02u %s", weekdays[t.tm_wday],
                      static_cast<unsigned>(t.tm_mday), months[t.tm_mon]);
    else if (s.show_date)
        std::snprintf(date, sizeof(date), "%02u %s", static_cast<unsigned>(t.tm_mday), months[t.tm_mon]);
    else if (s.show_weekday)
        std::snprintf(date, sizeof(date), "%s", weekdays[t.tm_wday]);
    std::snprintf(out.panel, sizeof(out.panel), "%s%s%s", date, date[0] ? "  " : "", time);
    std::snprintf(out.heading, sizeof(out.heading), "%s, %02u %s %04d  %s", full_weekdays[t.tm_wday],
                  static_cast<unsigned>(t.tm_mday), full_months[t.tm_mon], t.tm_year + 1900, time);
    return out;
}
} // namespace

export namespace lx::shell {
[[nodiscard]] clock_text format_clock(std::time_t now, const lx::config::clock_settings& s) {
    std::tm t{};
    if (!localtime_r(&now, &t))
        return {};
    clock_text out{};
    char time[32]{};
    char date[48]{};
    const bool h12 = s.format == lx::config::clock_format::h12;
    const char* time_format = h12 ? (s.show_seconds ? "%I:%M:%S %p" : "%I:%M %p")
                                  : (s.show_seconds ? "%H:%M:%S" : "%H:%M");
    const auto time_size = std::strftime(time, sizeof(time), time_format, &t);
    std::size_t date_size = 1;
    if (s.show_date)
        date_size = std::strftime(date, sizeof(date), s.show_weekday ? "%a, %d %b" : "%d %b", &t);
    else if (s.show_weekday)
        date_size = std::strftime(date, sizeof(date), "%a", &t);
    char full[64]{};
    const auto full_size = std::strftime(full, sizeof(full), "%A, %d %B %Y", &t);
    if (time_size == 0 || date_size == 0 || full_size == 0)
        return format_clock_ascii(t, s);
    std::snprintf(out.panel, sizeof(out.panel), "%s%s%s", date, date[0] ? "  " : "", time);
    std::snprintf(out.heading, sizeof(out.heading), "%s  %s", full, time);
    return out;
}

[[nodiscard]] long long clock_next_boundary(long long realtime_ns, bool seconds) {
    const long long step = seconds ? 1'000'000'000ll : 60'000'000'000ll;
    const long long remainder = ((realtime_ns % step) + step) % step;
    return realtime_ns + step - remainder;
}
}
