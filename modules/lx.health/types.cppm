module;

import lx.foundation;

export module lx.health:types;

import std;

export namespace lx::health {

/// How well something works. Ordered: a report's level is the worst of its parts.
enum class level : unsigned char {
    ok,
    /// Works, but not as it should: a fallback path, a missing shell, moderate memory
    /// pressure, frames missing their deadlines now and then.
    degraded,
    /// Something the session cannot do without: the display, the render thread, memory.
    critical,
    /// Not known (nothing to judge by yet).
    unknown,
};

[[nodiscard]] constexpr const char* level_name(level l) {
    switch (l) {
    case level::ok: return "ok";
    case level::degraded: return "degraded";
    case level::critical: return "critical";
    case level::unknown: return "unknown";
    }
    return "unknown";
}

/// The worse of two levels (`unknown` loses to anything known).
[[nodiscard]] constexpr level worst(level a, level b) {
    if (a == level::unknown)
        return b;
    if (b == level::unknown)
        return a;
    return static_cast<unsigned char>(a) > static_cast<unsigned char>(b) ? a : b;
}

/// A fixed text buffer, so a report is a plain value (no heap) the UI thread can build on
/// request and hand over.
template <std::size_t N>
struct text {
    char buf[N]{};
    void set(std::string_view s) {
        const std::size_t n = s.size() < N - 1 ? s.size() : N - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    [[nodiscard]] const char* c_str() const { return buf; }
    [[nodiscard]] bool empty() const { return buf[0] == '\0'; }
};

/// One subsystem: "wayland", "display", "graphics", "render", "input", "session", "shell",
/// "presentation", "systemd".
struct part_status {
    const char* name = "";
    level lvl = level::unknown;
    /// False for a part that is still a stub in this build (health.md's "implemented").
    bool implemented = true;
    text<96> detail{};
};

/// One process of the session ("compositor", "shell").
struct process_status {
    const char* role = "";
    /// 0: unknown or not running.
    int pid = 0;
    bool connected = false;
    /// How often it has bound (the shell: 1 at first, +1 for each restart).
    unsigned binds = 0;
    level lvl = level::unknown;
};

struct memory_status {
    /// "normal", "moderate", "critical" (`runtime::memory_pressure`).
    const char* pressure = "normal";
    unsigned long long rss_kib = 0;
    unsigned import_cache_used = 0;
    unsigned import_cache_capacity = 0;
    unsigned long long staging_bytes = 0;
    /// 0: no budget configured.
    unsigned long long staging_budget = 0;
    level lvl = level::unknown;
};

/// One output's frame pacing.
struct speed_status {
    text<32> output{};
    unsigned refresh_mhz = 0;
    /// Content frames that reached the display since the output appeared.
    unsigned long long presented = 0;
    /// Of those, frames that landed a refresh period or more after the vblank they aimed at.
    unsigned long long missed = 0;
    /// Misses among the last `miss_ring::k_frames` presented frames.
    unsigned missed_recent = 0;
    /// How many of those frames were observed (≤ `miss_ring::k_frames`).
    unsigned recent_frames = 0;
    unsigned last_tick_ui_us = 0;
    unsigned last_tick_render_us = 0;
    /// UI / render ticks over the hot-path budget since start (shared by every output).
    unsigned long long ui_over_budget = 0;
    unsigned long long render_over_budget = 0;
    level lvl = level::unknown;
};

struct health_report {
    static constexpr unsigned k_max_parts = 12;
    static constexpr unsigned k_max_processes = 4;
    static constexpr unsigned k_max_outputs = 16;

    level overall = level::unknown;
    unsigned long long uptime_ms = 0;
    /// The composite backend: "vulkan", "gl", "cpu", "headless".
    const char* backend = "";
    std::inplace_vector<part_status, k_max_parts> parts{};
    std::inplace_vector<process_status, k_max_processes> processes{};
    memory_status memory{};
    std::inplace_vector<speed_status, k_max_outputs> speed{};
    unsigned config_generation = 0;
    unsigned config_errors = 0;
    unsigned config_warnings = 0;
};

} // namespace lx::health
