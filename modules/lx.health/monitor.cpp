module;

#include <fcntl.h>
#include <unistd.h>

module lx.health;

import std;
import lx.foundation;

namespace lx::health {

namespace {

part_status& add_part(health_report& r, const char* name, level lvl, std::string_view detail) {
    part_status p{};
    p.name = name;
    p.lvl = lvl;
    p.detail.set(detail);
    r.parts.push_back(p);
    return r.parts.back();
}

const char* pressure_name(unsigned p) {
    switch (p) {
    case 0: return "normal";
    case 1: return "moderate";
    default: return "critical";
    }
}

} // namespace

health_report evaluate(const health_inputs& in, const health_thresholds& t) {
    health_report r{};
    r.uptime_ms = in.uptime_ms;
    r.backend = in.backend ? in.backend : "";
    r.config_generation = in.config_generation;
    r.config_errors = in.config_errors;
    r.config_warnings = in.config_warnings;
    char line[96];

    // ── Parts ──
    if (in.wayland_serving) {
        std::snprintf(line, sizeof(line), "serving %s", in.socket_name ? in.socket_name : "");
        add_part(r, "wayland", level::ok, line);
    } else {
        add_part(r, "wayland", level::critical, "no Wayland socket");
    }

    switch (in.display) {
    case display_path::kms:
        add_part(r, "display", level::ok,
                 in.display_paused ? "KMS, session in the background" : "KMS page flips");
        break;
    case display_path::kms_without_master:
        add_part(r, "display", level::degraded, "card open without DRM master: not shown");
        break;
    case display_path::virtual_outputs:
        add_part(r, "display", level::ok, "virtual outputs (no KMS)");
        break;
    case display_path::none:
        add_part(r, "display", level::degraded, "no display: headless fallback output");
        break;
    }

    {
        const bool headless = std::string_view{r.backend} == "headless" || r.backend[0] == '\0';
        std::snprintf(line, sizeof(line), "%s composite", headless ? "no" : r.backend);
        add_part(r, "graphics", headless && in.display == display_path::kms ? level::degraded
                                                                             : level::ok,
                 line);
    }

    if (in.render_answered) {
        std::snprintf(line, sizeof(line), "answered in %u ms", in.render_answer_ms);
        add_part(r, "render", level::ok, line);
    } else {
        std::snprintf(line, sizeof(line), "render thread silent for %u ms", t.render_timeout_ms);
        add_part(r, "render", level::critical, line);
    }

    if (!in.input_open) {
        add_part(r, "input", level::degraded, "no input devices opened");
    } else {
        std::snprintf(line, sizeof(line), "%u device%s on the seat", in.input_devices,
                      in.input_devices == 1 ? "" : "s");
        add_part(r, "input", in.input_devices == 0 ? level::degraded : level::ok, line);
    }

    if (in.logind_control) {
        std::snprintf(line, sizeof(line), "logind session %s, in control",
                      in.session_id ? in.session_id : "");
        add_part(r, "session", level::ok, line);
    } else if (in.session_id && in.session_id[0]) {
        std::snprintf(line, sizeof(line), "logind session %s, not in control", in.session_id);
        add_part(r, "session", level::degraded, line);
    } else {
        add_part(r, "session", in.display == display_path::virtual_outputs ? level::ok
                                                                           : level::degraded,
                 "no logind session");
    }

    {
        unsigned timer = 0;
        for (const auto& o : in.outputs)
            if (!o.flip_paced)
                ++timer;
        if (in.outputs.empty()) {
            add_part(r, "presentation", level::degraded, "no outputs");
        } else if (timer == 0) {
            add_part(r, "presentation", level::ok, "every output paced by page flips");
        } else {
            std::snprintf(line, sizeof(line), "%u of %u output%s timer-paced", timer,
                          static_cast<unsigned>(in.outputs.size()),
                          in.outputs.size() == 1 ? "" : "s");
            add_part(r, "presentation",
                     in.display == display_path::kms ? level::degraded : level::ok, line);
        }
    }

    if (in.session_mode) {
        if (in.ready_sent && in.environment_exported)
            add_part(r, "systemd", level::ok,
                     in.watchdog ? "ready, environment exported, watchdog on"
                                 : "ready, environment exported");
        else if (in.ready_sent)
            add_part(r, "systemd", level::degraded, "ready, environment export failed");
        else
            add_part(r, "systemd", level::degraded, "starting (READY not sent yet)");
    }

    // ── Processes ──
    {
        process_status c{};
        c.role = "compositor";
        c.pid = in.compositor_pid;
        c.connected = true;
        c.binds = 1;
        c.lvl = level::ok;
        r.processes.push_back(c);
        process_status s{};
        s.role = "shell";
        s.pid = in.shell_connected ? in.shell_pid : 0;
        s.connected = in.shell_connected;
        s.binds = in.shell_binds;
        s.lvl = in.shell_connected ? level::ok : level::degraded;
        r.processes.push_back(s);
    }

    // ── Memory ──
    {
        memory_status& m = r.memory;
        m.pressure = pressure_name(in.memory_pressure);
        m.rss_kib = in.rss_kib;
        m.import_cache_used = in.import_cache_used;
        m.import_cache_capacity = in.import_cache_capacity;
        m.staging_bytes = in.staging_bytes;
        m.staging_budget = in.staging_budget;
        m.gpu = in.gpu;
        m.lvl = in.memory_pressure == 0   ? level::ok
                : in.memory_pressure == 1 ? level::degraded
                                          : level::critical;
        if (in.import_cache_capacity > 0 && in.import_cache_used >= in.import_cache_capacity)
            m.lvl = worst(m.lvl, level::degraded);
    }

    // ── Speed ──
    for (const auto& o : in.outputs) {
        speed_status s{};
        s.output = o.name;
        s.refresh_mhz = o.refresh_mhz;
        s.presented = o.presented;
        s.missed = o.missed;
        s.missed_recent = o.recent.missed();
        s.recent_frames = o.recent.frames();
        s.last_tick_ui_us = in.last_tick_ui_us;
        s.last_tick_render_us = in.last_tick_render_us;
        s.ui_over_budget = in.ui_over_budget;
        s.render_over_budget = in.render_over_budget;
        if (s.recent_frames < t.min_recent_frames)
            s.lvl = level::ok;
        else if (s.missed_recent >= t.missed_recent_critical)
            s.lvl = level::critical;
        else if (s.missed_recent >= t.missed_recent_degraded)
            s.lvl = level::degraded;
        else
            s.lvl = level::ok;
        r.speed.push_back(s);
    }

    level overall = level::ok;
    for (const auto& p : r.parts)
        overall = worst(overall, p.lvl);
    for (const auto& p : r.processes)
        overall = worst(overall, p.lvl);
    overall = worst(overall, r.memory.lvl);
    for (const auto& s : r.speed)
        overall = worst(overall, s.lvl);
    r.overall = overall;
    return r;
}

unsigned long long read_rss_kib() {
    const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char buf[128];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    unsigned long long size = 0;
    unsigned long long resident = 0;
    if (std::sscanf(buf, "%llu %llu", &size, &resident) != 2)
        return 0;
    const long page = ::sysconf(_SC_PAGESIZE);
    return resident * static_cast<unsigned long long>(page > 0 ? page : 4096) / 1024ull;
}

} // namespace lx::health
