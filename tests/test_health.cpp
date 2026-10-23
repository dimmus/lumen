// lx.health: the levels `evaluate` gives each part, process, the memory and an output's
// pacing, the worst-of rule, and the miss ring (design-session-and-wm S.8, health.md §3).
#include "lumen_test.hpp"

import std;
import lx.health;

namespace {

using lx::health::level;

/// A compositor that is fine: KMS, a shell, input, an output presenting on time.
lx::health::health_inputs healthy() {
    lx::health::health_inputs in{};
    in.uptime_ms = 61'000;
    in.backend = "gl";
    in.compositor_pid = 42;
    in.wayland_serving = true;
    in.socket_name = "lumen-0";
    in.display = lx::health::display_path::kms;
    in.render_answered = true;
    in.render_answer_ms = 1;
    in.input_open = true;
    in.input_devices = 3;
    in.logind_control = true;
    in.session_id = "1";
    in.shell_connected = true;
    in.shell_pid = 77;
    in.shell_binds = 1;
    lx::health::output_input o{};
    o.name.set("Virtual-1");
    o.refresh_mhz = 60000;
    o.flip_paced = true;
    for (int i = 0; i < 100; ++i) {
        o.recent.record(false);
        ++o.presented;
    }
    in.outputs.push_back(o);
    return in;
}

const lx::health::part_status* part(const lx::health::health_report& r, std::string_view name) {
    for (const auto& p : r.parts)
        if (name == p.name)
            return &p;
    return nullptr;
}

} // namespace

LUMEN_TEST(a_healthy_session_is_ok) {
    const auto r = lx::health::evaluate(healthy());
    LUMEN_CHECK(r.overall == level::ok);
    LUMEN_CHECK(r.uptime_ms == 61'000);
    LUMEN_CHECK(std::string_view{r.backend} == "gl");
    LUMEN_CHECK(part(r, "wayland") && part(r, "wayland")->lvl == level::ok);
    LUMEN_CHECK(part(r, "render") && part(r, "render")->lvl == level::ok);
    LUMEN_CHECK(part(r, "systemd") == nullptr); // not a --session run
    LUMEN_CHECK(r.processes.size() == 2);
    LUMEN_CHECK(r.processes[1].pid == 77 && r.processes[1].connected);
    LUMEN_CHECK(r.speed.size() == 1 && r.speed[0].lvl == level::ok);
    LUMEN_CHECK(r.speed[0].presented == 100 && r.speed[0].recent_frames == 100);
}

LUMEN_TEST(a_missing_shell_degrades_and_a_silent_render_thread_is_critical) {
    auto in = healthy();
    in.shell_connected = false;
    auto r = lx::health::evaluate(in);
    LUMEN_CHECK(r.overall == level::degraded);
    LUMEN_CHECK(r.processes[1].pid == 0 && r.processes[1].lvl == level::degraded);

    in.render_answered = false;
    r = lx::health::evaluate(in);
    LUMEN_CHECK(r.overall == level::critical);
    LUMEN_CHECK(part(r, "render")->lvl == level::critical);
}

LUMEN_TEST(memory_pressure_maps_to_levels) {
    auto in = healthy();
    in.memory_pressure = 1;
    LUMEN_CHECK(lx::health::evaluate(in).memory.lvl == level::degraded);
    in.memory_pressure = 2;
    const auto r = lx::health::evaluate(in);
    LUMEN_CHECK(r.memory.lvl == level::critical && r.overall == level::critical);
    LUMEN_CHECK(std::string_view{r.memory.pressure} == "critical");
    in.memory_pressure = 0;
    in.import_cache_capacity = 4;
    in.import_cache_used = 4;
    LUMEN_CHECK(lx::health::evaluate(in).memory.lvl == level::degraded);
}

LUMEN_TEST(missed_frames_degrade_then_go_critical_by_threshold) {
    auto in = healthy();
    auto& ring = in.outputs[0].recent;
    ring.reset();
    for (int i = 0; i < 128; ++i)
        ring.record(i % 16 == 0); // 8 misses in 128
    LUMEN_CHECK(ring.missed() == 8 && ring.frames() == 128);
    LUMEN_CHECK(lx::health::evaluate(in).speed[0].lvl == level::degraded);
    for (int i = 0; i < 128; ++i)
        ring.record(i % 4 == 0); // 32 in the newest 128
    LUMEN_CHECK(ring.missed() == 32);
    LUMEN_CHECK(lx::health::evaluate(in).speed[0].lvl == level::critical);
    // Too few frames to judge: ok whatever they say.
    ring.reset();
    for (int i = 0; i < 4; ++i)
        ring.record(true);
    LUMEN_CHECK(lx::health::evaluate(in).speed[0].lvl == level::ok);
}

LUMEN_TEST(the_ring_forgets_frames_older_than_its_window) {
    lx::health::miss_ring ring;
    for (int i = 0; i < 128; ++i)
        ring.record(true);
    LUMEN_CHECK(ring.missed() == 128);
    for (int i = 0; i < 100; ++i)
        ring.record(false);
    LUMEN_CHECK(ring.missed() == 28);
    LUMEN_CHECK(ring.frames() == 128);
}

LUMEN_TEST(timer_paced_outputs_degrade_presentation_only_on_kms) {
    auto in = healthy();
    in.outputs[0].flip_paced = false;
    LUMEN_CHECK(part(lx::health::evaluate(in), "presentation")->lvl == level::degraded);
    in.display = lx::health::display_path::virtual_outputs;
    in.logind_control = false;
    in.session_id = "";
    const auto r = lx::health::evaluate(in);
    LUMEN_CHECK(part(r, "presentation")->lvl == level::ok);
    LUMEN_CHECK(part(r, "session")->lvl == level::ok);
    LUMEN_CHECK(part(r, "display")->lvl == level::ok);
}

LUMEN_TEST(a_session_run_reports_systemd_readiness) {
    auto in = healthy();
    in.session_mode = true;
    LUMEN_CHECK(part(lx::health::evaluate(in), "systemd")->lvl == level::degraded);
    in.ready_sent = true;
    in.environment_exported = true;
    in.watchdog = true;
    const auto r = lx::health::evaluate(in);
    LUMEN_CHECK(part(r, "systemd")->lvl == level::ok);
    LUMEN_CHECK(std::string_view{part(r, "systemd")->detail.c_str()}.find("watchdog") !=
                std::string_view::npos);
}

LUMEN_TEST(worst_ignores_unknown) {
    LUMEN_CHECK(lx::health::worst(level::unknown, level::ok) == level::ok);
    LUMEN_CHECK(lx::health::worst(level::degraded, level::unknown) == level::degraded);
    LUMEN_CHECK(lx::health::worst(level::critical, level::degraded) == level::critical);
    LUMEN_CHECK(std::string_view{lx::health::level_name(level::degraded)} == "degraded");
}

LUMEN_TEST(rss_is_read_from_proc) {
    LUMEN_CHECK(lx::health::read_rss_kib() > 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
