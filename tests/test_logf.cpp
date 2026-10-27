#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.trace;

#include "lx.trace/scope.hpp"

namespace {

struct capture {
    std::vector<std::string> lines;
};

void capture_sink(lx::trace::level, const char*, const char* message, void* user) {
    static_cast<capture*>(user)->lines.emplace_back(message);
}

/// Routes the global logger into `c` at `min` for the lifetime of the guard.
struct sink_guard {
    sink_guard(capture& c, lx::trace::level min)
        : previous_{lx::trace::logger::global().min_level()} {
        lx::trace::logger::global().set_sink(capture_sink, &c);
        lx::trace::logger::global().set_min_level(min);
    }
    ~sink_guard() {
        lx::trace::logger::global().set_sink(nullptr);
        lx::trace::logger::global().set_min_level(previous_);
    }
    lx::trace::level previous_;
};

constexpr lx::trace::category log_t{"t"};

} // namespace

LUMEN_TEST(logf_formats_like_the_printf_lines_it_replaced) {
    capture c;
    sink_guard g{c, lx::trace::level::debug};
    log_t.debug("geometry id={} x={} w={} cause={} serial={}", 42ull, -3, 640u, "map", 7u);
    log_t.debug("flags=0x{:x} t={}.{:09} scale={:g} rgba={:.3f}", 0x2au, 12, 5'000'000, 1.5, 0.25);
    log_t.debug("token={:.8}… title=\"{:.4}\"", "abcdefghijk", "Terminal");
    LUMEN_CHECK(c.lines.size() == 3);
    LUMEN_CHECK(c.lines[0] == "geometry id=42 x=-3 w=640 cause=map serial=7");
    LUMEN_CHECK(c.lines[1] == "flags=0x2a t=12.005000000 scale=1.5 rgba=0.250");
    LUMEN_CHECK(c.lines[2] == "token=abcdefgh… title=\"Term\"");
}

LUMEN_TEST(logf_prints_bool_as_digit_and_null_string_as_null) {
    capture c;
    sink_guard g{c, lx::trace::level::debug};
    const char* none = nullptr;
    char name[16] = "HDMI-A-1";
    log_t.info("on={} off={:d} src={} out={}", true, false, none, name);
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0] == "on=1 off=0 src=(null) out=HDMI-A-1");
}

LUMEN_TEST(logf_skips_filtered_levels) {
    capture c;
    sink_guard g{c, lx::trace::level::info};
    log_t.debug("hidden {}", 1);
    log_t.warn("shown {}", 2);
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0] == "shown 2");
}

LUMEN_TEST(logf_truncates_long_lines) {
    capture c;
    sink_guard g{c, lx::trace::level::debug};
    const std::string big(3 * lx::trace::logger::k_max_line, 'x');
    log_t.info("{}{}", big, "tail");
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0].size() == lx::trace::logger::k_max_line - 1);
    LUMEN_CHECK(c.lines[0].find_first_not_of('x') == std::string::npos);
}

namespace {

struct leveled {
    std::vector<std::pair<lx::trace::level, std::string>> lines;
};

void leveled_sink(lx::trace::level lvl, const char* category, const char* message, void* user) {
    static_cast<leveled*>(user)->lines.emplace_back(lvl, std::string{category} + ": " + message);
}

constexpr lx::trace::category log_t_cat{"t.cat"};

} // namespace

LUMEN_TEST(category_logs_at_the_method_level_under_its_name) {
    leveled c;
    const auto previous = lx::trace::logger::global().min_level();
    lx::trace::logger::global().set_sink(leveled_sink, &c);
    lx::trace::logger::global().set_min_level(lx::trace::level::info);
    LUMEN_CHECK(!log_t_cat.enabled(lx::trace::level::debug));
    LUMEN_CHECK(log_t_cat.enabled(lx::trace::level::warn));
    log_t_cat.debug("hidden {}", 1);
    log_t_cat.info("plain {{braces}}");
    log_t_cat.warn("id={} on={}", 7u, true);
    log_t_cat.log(lx::trace::level::error, "{}", "runtime text");
    log_t_cat.error(lx::invalid_argument("bad input"));
    lx::trace::logger::global().set_sink(nullptr);
    lx::trace::logger::global().set_min_level(previous);
    LUMEN_CHECK(c.lines.size() == 4);
    LUMEN_CHECK(c.lines[0].first == lx::trace::level::info);
    LUMEN_CHECK(c.lines[0].second == "t.cat: plain {braces}");
    LUMEN_CHECK(c.lines[1].first == lx::trace::level::warn);
    LUMEN_CHECK(c.lines[1].second == "t.cat: id=7 on=1");
    LUMEN_CHECK(c.lines[2].second == "t.cat: runtime text");
    LUMEN_CHECK(c.lines[3].first == lx::trace::level::error);
    LUMEN_CHECK(c.lines[3].second.starts_with("t.cat: "));
    LUMEN_CHECK(c.lines[3].second.find("bad input") != std::string::npos);
}

// `##__LINE__` used to paste the token `__LINE__` itself, so a second scope in one block
// redeclared the first's variable.
LUMEN_TEST(trace_scope_twice_in_one_block) {
    capture c;
    sink_guard g{c, lx::trace::level::trace};
    {
        LX_TRACE_SCOPE("t", "outer");
        LX_TRACE_SCOPE("t", "inner");
        LUMEN_CHECK(c.lines.size() == 2);
    }
    LUMEN_CHECK(c.lines.size() == 4);
    LUMEN_CHECK(c.lines[0] == "outer");
    LUMEN_CHECK(c.lines[1] == "inner");
    LUMEN_CHECK(c.lines[2] == "~inner");
    LUMEN_CHECK(c.lines[3] == "~outer");
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
