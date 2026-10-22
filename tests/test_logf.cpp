#include "lumen_test.hpp"

import std;
import lx.trace;

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

} // namespace

LUMEN_TEST(logf_formats_like_the_printf_lines_it_replaced) {
    capture c;
    sink_guard g{c, lx::trace::level::debug};
    const auto& log = lx::trace::logger::global();
    log.logf(lx::trace::level::debug, "t", "geometry id={} x={} w={} cause={} serial={}", 42ull, -3,
             640u, "map", 7u);
    log.logf(lx::trace::level::debug, "t", "flags=0x{:x} t={}.{:09} scale={:g} rgba={:.3f}", 0x2au,
             12, 5'000'000, 1.5, 0.25);
    log.logf(lx::trace::level::debug, "t", "token={:.8}… title=\"{:.4}\"", "abcdefghijk",
             "Terminal");
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
    lx::trace::logger::global().logf(lx::trace::level::info, "t", "on={} off={:d} src={} out={}",
                                     true, false, none, name);
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0] == "on=1 off=0 src=(null) out=HDMI-A-1");
}

LUMEN_TEST(logf_skips_filtered_levels) {
    capture c;
    sink_guard g{c, lx::trace::level::info};
    lx::trace::logger::global().logf(lx::trace::level::debug, "t", "hidden {}", 1);
    lx::trace::logger::global().logf(lx::trace::level::warn, "t", "shown {}", 2);
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0] == "shown 2");
}

LUMEN_TEST(logf_truncates_long_lines) {
    capture c;
    sink_guard g{c, lx::trace::level::debug};
    const std::string big(3 * lx::trace::logger::k_max_line, 'x');
    lx::trace::logger::global().logf(lx::trace::level::info, "t", "{}{}", big, "tail");
    LUMEN_CHECK(c.lines.size() == 1);
    LUMEN_CHECK(c.lines[0].size() == lx::trace::logger::k_max_line - 1);
    LUMEN_CHECK(c.lines[0].find_first_not_of('x') == std::string::npos);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
