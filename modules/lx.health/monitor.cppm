module;

import lx.foundation;

export module lx.health:monitor;

import std;

import :types;

export namespace lx::health {

/// The last `k_frames` presented frames of one output, one bit each: missed or not. One bit
/// write per presented frame is the only per-frame cost of the health report.
class miss_ring {
public:
    static constexpr unsigned k_frames = 128;

    void record(bool missed) {
        const unsigned word = next_ >> 6;
        const std::uint64_t bit = std::uint64_t{1} << (next_ & 63u);
        bits_[word] = missed ? (bits_[word] | bit) : (bits_[word] & ~bit);
        next_ = (next_ + 1) % k_frames;
        if (filled_ < k_frames)
            ++filled_;
    }
    /// Misses among the frames recorded (at most the last `k_frames`).
    [[nodiscard]] unsigned missed() const {
        return static_cast<unsigned>(std::popcount(bits_[0]) + std::popcount(bits_[1]));
    }
    [[nodiscard]] unsigned frames() const { return filled_; }
    void reset() { *this = {}; }

private:
    std::uint64_t bits_[2]{};
    unsigned next_ = 0;
    unsigned filled_ = 0;
};

/// Where the levels change. Defaults: a miss in 16 recent frames is noticeable stutter
/// (degraded); one in four is a display that cannot keep up (critical).
struct health_thresholds {
    unsigned missed_recent_degraded = 8;
    unsigned missed_recent_critical = 32;
    /// Recent frames needed before misses are judged (a few frames say nothing).
    unsigned min_recent_frames = 16;
    /// The render thread not answering within this many ms is critical.
    unsigned render_timeout_ms = 500;
};

/// How the compositor reaches the display.
enum class display_path : unsigned char {
    /// KMS with DRM master: page flips.
    kms,
    /// KMS opened without master (nested, or another session holds it).
    kms_without_master,
    /// `--virtual-outputs`: composite only, timer-paced.
    virtual_outputs,
    /// No display at all (a fallback headless output).
    none,
};

struct output_input {
    text<32> name{};
    unsigned refresh_mhz = 0;
    /// Paced by page-flip events (else by a timer: no vblank to aim at).
    bool flip_paced = false;
    unsigned long long presented = 0;
    unsigned long long missed = 0;
    miss_ring recent{};
};

/// Facts the compositor knows; `evaluate` judges them. Plain data, filled on the UI thread.
struct health_inputs {
    unsigned long long uptime_ms = 0;
    const char* backend = "";
    int compositor_pid = 0;

    bool wayland_serving = false;
    const char* socket_name = "";
    display_path display = display_path::none;
    /// The session is in the background (VT switched away): the display is not ours now.
    bool display_paused = false;

    /// The render thread answered the health request (else it is stuck).
    bool render_answered = false;
    unsigned render_answer_ms = 0;

    /// libinput devices attached to the seat.
    unsigned input_devices = 0;
    /// Input devices are read at all (libinput on a seat).
    bool input_open = false;

    bool logind_control = false;
    const char* session_id = "";

    bool shell_connected = false;
    int shell_pid = 0;
    unsigned shell_binds = 0;

    /// Started as a systemd session (`--session`): the environment export and READY count.
    bool session_mode = false;
    bool environment_exported = false;
    bool ready_sent = false;
    bool watchdog = false;

    /// 0 normal, 1 moderate, 2 critical (`runtime::memory_pressure`).
    unsigned memory_pressure = 0;
    unsigned long long rss_kib = 0;
    unsigned import_cache_used = 0;
    unsigned import_cache_capacity = 0;
    unsigned long long staging_bytes = 0;
    unsigned long long staging_budget = 0;
    /// GPU memory from DRM fdinfo (`lx::drm::read_gpu_memory`), copied into the report.
    gpu_memory_status gpu{};
    /// The render thread's figures (import cache, staging) are known (it answered).
    bool render_figures = false;

    unsigned last_tick_ui_us = 0;
    unsigned last_tick_render_us = 0;
    unsigned long long ui_over_budget = 0;
    unsigned long long render_over_budget = 0;

    std::inplace_vector<output_input, health_report::k_max_outputs> outputs{};

    unsigned config_generation = 0;
    unsigned config_errors = 0;
    unsigned config_warnings = 0;
};

/// Judges `in` (health.md §3): every part, process, the memory and each output's pacing get
/// a level; the report's is the worst of them. Pure, allocation-free.
[[nodiscard]] health_report evaluate(const health_inputs& in, const health_thresholds& t = {});

/// /proc/self/statm's resident set in KiB (0 when unreadable). One small read: for a
/// request, never per frame.
[[nodiscard]] unsigned long long read_rss_kib();

} // namespace lx::health
