export module lx.runtime:psi;

import std;

import :memory;

// Kernel pressure stall information (Documentation/accounting/psi.rst), the part that needs
// no kernel: the trigger strings, the averages, the cgroup path and the level a monitor
// holds between triggers. `psi_monitor` (lx.runtime) does the I/O.

export namespace lx::runtime {

/// When kernel memory pressure counts as Lumen's `moderate` and `critical`.
struct psi_config {
    /// The trigger window. Without CAP_SYS_RESOURCE the kernel (6.5+) takes only multiples
    /// of 2 s, up to 10 s: 1 s and 3 s are EINVAL as a user, 2 s works for both.
    unsigned window_us = 2'000'000;
    /// "some": at least one task stalled on memory 150 ms of the window (7.5 %) → moderate.
    unsigned moderate_some_us = 150'000;
    /// "full": every non-idle task stalled at once 100 ms of the window (5 %) → critical.
    unsigned critical_full_us = 100'000;
    /// No trigger for this long → step down to what avg10 still shows.
    long long quiet_ns = 10'000'000'000;
};

/// Writes the trigger `"<some|full> <stall_us> <window_us>"` with its NUL into `out` and
/// returns the bytes to write, NUL included — the kernel overwrites the last byte it is
/// given with a NUL, so a string written without one loses its last digit. 0 when the
/// trigger is invalid (no stall, a stall longer than the window) or does not fit.
[[nodiscard]] std::size_t format_psi_trigger(std::span<char> out, bool full, unsigned stall_us,
                                             unsigned window_us);

/// A stall share as avg10 reports it, in hundredths of a percent: 150 ms of 2 s → 750.
[[nodiscard]] constexpr unsigned psi_share_hundredths(unsigned stall_us, unsigned window_us) {
    return window_us == 0
               ? 0
               : static_cast<unsigned>(static_cast<unsigned long long>(stall_us) * 10'000ull /
                                       window_us);
}

/// avg10 of a pressure file's "some" and "full" lines, in hundredths of a percent.
struct psi_averages {
    unsigned some_avg10 = 0;
    unsigned full_avg10 = 0;
    bool valid = false; ///< both lines were found
};

/// Parses `some avg10=1.23 avg60=… total=…\nfull avg10=…` (the file a trigger fd also reads).
[[nodiscard]] psi_averages parse_psi_averages(std::string_view text);

/// The unified-hierarchy path of `/proc/self/cgroup` text (its `0::/path` line), empty on
/// a cgroup v1-only system or when the line is missing.
[[nodiscard]] std::string_view cgroup_v2_path(std::string_view proc_self_cgroup);

/// The level kernel pressure holds between triggers. A trigger only says a window went over
/// its threshold — nothing reports the pressure ending — so the level stays until the
/// triggers have been quiet for `quiet_ns`, and then steps down only as far as avg10 allows:
/// a burst that has passed decays, a stall that goes on just under the trigger does not.
class psi_pressure_state {
public:
    explicit psi_pressure_state(psi_config config = {}) : config_{config} {}

    /// A trigger for `level` fired: raises the level (never lowers it — a "some" trigger
    /// during critical pressure only says it goes on) and restarts the quiet period.
    memory_pressure on_trigger(memory_pressure level, long long now_ns);
    /// The quiet period ended: the level drops to what `avg` still supports, never rises
    /// (triggers raise). Averages that could not be read decay to normal.
    memory_pressure on_quiet(long long now_ns, const psi_averages& avg);

    /// When `on_quiet` is due, -1 at normal.
    [[nodiscard]] long long deadline() const { return deadline_ns_; }
    [[nodiscard]] memory_pressure level() const { return level_; }

private:
    psi_config config_{};
    memory_pressure level_ = memory_pressure::normal;
    long long deadline_ns_ = -1;
};

} // namespace lx::runtime


std::size_t lx::runtime::format_psi_trigger(std::span<char> out, bool full, unsigned stall_us,
                                            unsigned window_us) {
    if (stall_us == 0 || stall_us > window_us || out.empty())
        return 0;
    const auto r = std::format_to_n(out.data(), static_cast<std::ptrdiff_t>(out.size() - 1),
                                    "{} {} {}", full ? "full" : "some", stall_us, window_us);
    if (r.size < 0 || static_cast<std::size_t>(r.size) >= out.size())
        return 0;
    *r.out = '\0';
    return static_cast<std::size_t>(r.size) + 1;
}

namespace {

/// "12.34" → 1234; stops at the first character that is not part of the number.
unsigned parse_hundredths(std::string_view s) {
    unsigned whole = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
        whole = whole * 10 + static_cast<unsigned>(s[i] - '0');
    unsigned frac = 0;
    unsigned digits = 0;
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            if (digits < 2) {
                frac = frac * 10 + static_cast<unsigned>(s[i] - '0');
                ++digits;
            }
        }
    }
    for (; digits < 2; ++digits)
        frac *= 10;
    return whole * 100 + frac;
}

} // namespace

lx::runtime::psi_averages lx::runtime::parse_psi_averages(std::string_view text) {
    psi_averages avg{};
    bool some = false;
    bool full = false;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        const std::size_t at = line.find(" avg10=");
        if (at == std::string_view::npos)
            continue;
        const unsigned value = parse_hundredths(line.substr(at + 7));
        if (line.starts_with("some ")) {
            avg.some_avg10 = value;
            some = true;
        } else if (line.starts_with("full ")) {
            avg.full_avg10 = value;
            full = true;
        }
    }
    avg.valid = some && full;
    return avg;
}

std::string_view lx::runtime::cgroup_v2_path(std::string_view text) {
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.starts_with("0::/"))
            return line.substr(3);
    }
    return {};
}

lx::runtime::memory_pressure lx::runtime::psi_pressure_state::on_trigger(memory_pressure level,
                                                                         long long now_ns) {
    level_ = max_pressure(level_, level);
    if (level_ != memory_pressure::normal)
        deadline_ns_ = now_ns + config_.quiet_ns;
    return level_;
}

lx::runtime::memory_pressure lx::runtime::psi_pressure_state::on_quiet(long long now_ns,
                                                                       const psi_averages& avg) {
    // avg10 decays over about 10 s, so a burst that has stopped falls under the trigger's
    // own share within a quiet period or two; pressure that goes on keeps the level.
    memory_pressure supported = memory_pressure::normal;
    if (avg.valid) {
        if (avg.full_avg10 >= psi_share_hundredths(config_.critical_full_us, config_.window_us))
            supported = memory_pressure::critical;
        else if (avg.some_avg10 >=
                 psi_share_hundredths(config_.moderate_some_us, config_.window_us))
            supported = memory_pressure::moderate;
    }
    if (static_cast<unsigned>(supported) < static_cast<unsigned>(level_))
        level_ = supported;
    deadline_ns_ = level_ == memory_pressure::normal ? -1 : now_ns + config_.quiet_ns;
    return level_;
}
