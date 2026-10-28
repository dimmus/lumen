#include "lumen_test.hpp"

import std;
import lx.runtime;

// Kernel memory pressure (PSI): the trigger strings, the averages, the cgroup path and the
// level between triggers, and how the coordinator combines it with its own pools. None of
// it needs real pressure; the last test only checks the monitor starts and stops cleanly
// wherever it runs.

using lx::runtime::memory_pressure;

LUMEN_TEST(psi_trigger_carries_its_nul) {
    std::array<char, 48> buf{};
    const std::size_t n = lx::runtime::format_psi_trigger(buf, false, 150'000, 2'000'000);
    // The kernel overwrites the last byte written with a NUL: without one of our own the
    // window would read 200000.
    LUMEN_CHECK(n == std::string_view{"some 150000 2000000"}.size() + 1);
    LUMEN_CHECK(std::string_view(buf.data()) == "some 150000 2000000");
    LUMEN_CHECK(buf[n - 1] == '\0');

    LUMEN_CHECK(lx::runtime::format_psi_trigger(buf, true, 100'000, 2'000'000) ==
                std::string_view{"full 100000 2000000"}.size() + 1);
    LUMEN_CHECK(std::string_view(buf.data()) == "full 100000 2000000");
}

LUMEN_TEST(psi_trigger_rejects_what_the_kernel_would) {
    std::array<char, 48> buf{};
    LUMEN_CHECK(lx::runtime::format_psi_trigger(buf, false, 0, 2'000'000) == 0);
    LUMEN_CHECK(lx::runtime::format_psi_trigger(buf, false, 3'000'000, 2'000'000) == 0);
    std::array<char, 19> tight{}; // one short of "some 150000 2000000" and its NUL
    LUMEN_CHECK(lx::runtime::format_psi_trigger(tight, false, 150'000, 2'000'000) == 0);
    std::array<char, 20> exact{};
    LUMEN_CHECK(lx::runtime::format_psi_trigger(exact, false, 150'000, 2'000'000) == 20);
}

LUMEN_TEST(psi_share_matches_avg10_units) {
    LUMEN_CHECK(lx::runtime::psi_share_hundredths(150'000, 2'000'000) == 750); // 7.50 %
    LUMEN_CHECK(lx::runtime::psi_share_hundredths(100'000, 2'000'000) == 500);
    LUMEN_CHECK(lx::runtime::psi_share_hundredths(1, 0) == 0);
}

LUMEN_TEST(psi_averages_parse_both_lines) {
    const auto avg = lx::runtime::parse_psi_averages(
        "some avg10=12.34 avg60=1.00 avg300=0.10 total=14421\n"
        "full avg10=0.05 avg60=0.00 avg300=0.00 total=104\n");
    LUMEN_CHECK(avg.valid);
    LUMEN_CHECK(avg.some_avg10 == 1234);
    LUMEN_CHECK(avg.full_avg10 == 5);

    const auto whole = lx::runtime::parse_psi_averages(
        "some avg10=100 avg60=0 avg300=0 total=1\nfull avg10=7.5 avg60=0 avg300=0 total=1");
    LUMEN_CHECK(whole.valid && whole.some_avg10 == 10'000 && whole.full_avg10 == 750);

    // cpu.pressure has no "full" on old kernels; a truncated read has neither.
    LUMEN_CHECK(!lx::runtime::parse_psi_averages("some avg10=1.00 avg60=0.00").valid);
    LUMEN_CHECK(!lx::runtime::parse_psi_averages("").valid);
}

LUMEN_TEST(cgroup_path_is_the_unified_line) {
    LUMEN_CHECK(lx::runtime::cgroup_v2_path("0::/user.slice/user-1000.slice/session-3.scope\n") ==
                "/user.slice/user-1000.slice/session-3.scope");
    // Hybrid hierarchy: v1 controllers first, the unified line last.
    LUMEN_CHECK(lx::runtime::cgroup_v2_path("12:memory:/user.slice\n1:name=systemd:/x\n0::/a/b") ==
                "/a/b");
    LUMEN_CHECK(lx::runtime::cgroup_v2_path("0::/\n") == "/");
    LUMEN_CHECK(lx::runtime::cgroup_v2_path("4:cpu:/x\n").empty()); // v1 only
}

LUMEN_TEST(psi_level_rises_on_triggers_and_waits_out_a_quiet_period) {
    lx::runtime::psi_config cfg{};
    cfg.quiet_ns = 10;
    lx::runtime::psi_pressure_state state{cfg};
    LUMEN_CHECK(state.level() == memory_pressure::normal && state.deadline() == -1);

    LUMEN_CHECK(state.on_trigger(memory_pressure::moderate, 100) == memory_pressure::moderate);
    LUMEN_CHECK(state.deadline() == 110);
    LUMEN_CHECK(state.on_trigger(memory_pressure::critical, 105) == memory_pressure::critical);
    // A "some" trigger during critical pressure does not lower it, but says it goes on.
    LUMEN_CHECK(state.on_trigger(memory_pressure::moderate, 108) == memory_pressure::critical);
    LUMEN_CHECK(state.deadline() == 118);
}

LUMEN_TEST(psi_level_decays_only_as_far_as_avg10_allows) {
    lx::runtime::psi_config cfg{};
    cfg.quiet_ns = 10;
    lx::runtime::psi_pressure_state state{cfg};
    (void)state.on_trigger(memory_pressure::critical, 0);

    // "full" still over 5 %: critical stays, and the next check is a quiet period away.
    lx::runtime::psi_averages avg{.some_avg10 = 3000, .full_avg10 = 600, .valid = true};
    LUMEN_CHECK(state.on_quiet(10, avg) == memory_pressure::critical);
    LUMEN_CHECK(state.deadline() == 20);

    // "full" gone, "some" still over 7.5 %: one step down.
    avg = {.some_avg10 = 900, .full_avg10 = 100, .valid = true};
    LUMEN_CHECK(state.on_quiet(20, avg) == memory_pressure::moderate);
    LUMEN_CHECK(state.deadline() == 30);

    // Averages never raise the level: triggers do.
    avg = {.some_avg10 = 9000, .full_avg10 = 9000, .valid = true};
    LUMEN_CHECK(state.on_quiet(30, avg) == memory_pressure::moderate);

    avg = {.some_avg10 = 749, .full_avg10 = 0, .valid = true};
    LUMEN_CHECK(state.on_quiet(40, avg) == memory_pressure::normal);
    LUMEN_CHECK(state.deadline() == -1);
}

LUMEN_TEST(psi_level_decays_when_averages_cannot_be_read) {
    lx::runtime::psi_pressure_state state{};
    (void)state.on_trigger(memory_pressure::critical, 0);
    LUMEN_CHECK(state.on_quiet(state.deadline(), {}) == memory_pressure::normal);
    LUMEN_CHECK(state.deadline() == -1);
}

namespace {
memory_pressure g_level = memory_pressure::normal;
int g_calls = 0;
void on_pressure(memory_pressure level, void*) {
    g_level = level;
    ++g_calls;
}
} // namespace

LUMEN_TEST(external_pressure_combines_with_the_pools) {
    lx::runtime::memory_budget_config cfg{};
    cfg.staging_budget_bytes = 100u * 1024u * 1024u;
    lx::runtime::memory_budget_coordinator coord{cfg};
    coord.set_pressure_handler(&on_pressure);
    g_calls = 0;

    // The kernel says moderate; the next frame's pool evaluation (all pools near empty)
    // must not overwrite it.
    coord.on_external_pressure(memory_pressure::moderate);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::moderate && g_calls == 1);
    coord.tick(1);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::moderate && g_calls == 1);

    // The pools go critical: the higher wins. When the kernel's report decays, the pools'
    // own level is what remains.
    coord.report_usage(lx::runtime::memory_pool::staging, 95u * 1024u * 1024u, 0);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::critical && g_calls == 2);
    coord.on_external_pressure(memory_pressure::normal);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::critical && g_calls == 2);
    coord.report_usage(lx::runtime::memory_pool::staging, 0, 0);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::normal);
    LUMEN_CHECK(g_level == memory_pressure::normal && g_calls == 3);

    // A critical kernel report over moderate pools; withdrawing it leaves moderate.
    coord.report_usage(lx::runtime::memory_pool::staging, 80u * 1024u * 1024u, 0);
    coord.on_external_pressure(memory_pressure::critical);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::critical);
    coord.on_external_pressure(memory_pressure::normal);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::moderate);
    LUMEN_CHECK(g_level == memory_pressure::moderate);
}

// Wherever PSI is (a user's /proc/pressure/memory on 6.5+) or is not (a container, an old
// kernel), start either arms both triggers or fails cleanly, and stop leaves nothing behind.
LUMEN_TEST(psi_monitor_starts_or_declines_cleanly) {
    lx::runtime::event_loop loop;
    lx::runtime::memory_budget_coordinator coord{};
    const unsigned sources = loop.source_count();
    {
        lx::runtime::psi_monitor monitor;
        const auto started = monitor.start(loop, coord);
        LUMEN_CHECK(started.ok() == monitor.active());
        if (started.ok()) {
            LUMEN_CHECK(!monitor.path().empty());
            LUMEN_CHECK(loop.source_count() == sources + 3);
        } else {
            LUMEN_CHECK(loop.source_count() == sources);
        }
        LUMEN_CHECK(monitor.level() == memory_pressure::normal);
    }
    LUMEN_CHECK(loop.source_count() == sources);
    LUMEN_CHECK(coord.current_pressure() == memory_pressure::normal);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
