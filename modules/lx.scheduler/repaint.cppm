module;

import lx.foundation;
import lx.runtime;

export module lx.scheduler:repaint;

export namespace lx::scheduler {

/// Decides *when* a repaint should start, so it finishes just before the display needs it.
///
/// The obvious loop — wake on an interval, build a frame, present whenever it is ready —
/// samples input at the start of the interval and shows it a whole frame later. Latency is
/// then the repaint duration plus however much of the interval was spent idle first, and
/// the idle part is pure waste: nothing observed between the wake-up and the deadline can
/// reach that frame.
///
/// Aiming instead for `deadline − estimate − margin` spends that idle time *before* the
/// work rather than after it, so the frame carries input from as late as possible. It also
/// makes the schedule adaptive: a frame that gets more expensive starts earlier on its own,
/// instead of quietly missing deadlines.
///
/// The estimate is the peak over a short sliding window, not the mean. A mean is wrong in
/// the direction that hurts — half the frames exceed it and miss their deadline, and a
/// missed deadline costs a whole refresh period, whereas over-estimating costs only the
/// difference in latency. A window rather than an all-time peak so one pathological frame
/// does not pessimize the schedule forever.
class repaint_scheduler {
public:
    /// Frames of repaint history kept for the peak estimate. Roughly a quarter second at
    /// 60 Hz: long enough to cover a burst, short enough to forget an outlier quickly.
    static constexpr unsigned k_window = 16;

    /// Safety margin between the predicted finish and the deadline. Absorbs scheduler
    /// jitter and the part of present that is not measured.
    static constexpr lx::runtime::clock_time k_margin_ns = 1'000'000;

    /// Never start closer than this to the deadline — below it the frame is certain to
    /// miss, and starting anyway just burns the CPU to produce a late frame.
    static constexpr lx::runtime::clock_time k_min_lead_ns = 250'000;

    void set_refresh_period(lx::runtime::clock_time period_ns) {
        if (period_ns > 0)
            period_ns_ = period_ns;
    }
    [[nodiscard]] lx::runtime::clock_time refresh_period() const { return period_ns_; }

    /// Feed the measured cost of a completed repaint. Both phases count: the frame is not
    /// ready to present until the render thread is done, so the deadline applies to their
    /// sum, not to whichever finished last.
    void observe_repaint(unsigned tick_ui_us, unsigned tick_render_us) {
        const auto total_ns =
            static_cast<lx::runtime::clock_time>(tick_ui_us + tick_render_us) * 1000;
        window_[cursor_] = total_ns;
        cursor_ = (cursor_ + 1) % k_window;
        if (filled_ < k_window)
            ++filled_;
    }

    /// A flip landed. `flip_ns` is on the same clock as `next_start`'s `now`.
    void on_present(lx::runtime::clock_time flip_ns) {
        last_present_ns_ = flip_ns;
        ++presents_;
    }

    /// Peak repaint cost over the window, or a conservative default before any frame has
    /// been measured — half a refresh period, which starts pessimistic and converges down.
    [[nodiscard]] lx::runtime::clock_time estimate_ns() const {
        if (filled_ == 0)
            return period_ns_ / 2;
        lx::runtime::clock_time peak = 0;
        for (unsigned i = 0; i < filled_; ++i) {
            if (window_[i] > peak)
                peak = window_[i];
        }
        return peak;
    }

    /// The presentation deadline the next repaint is aiming at: the first vblank strictly
    /// after `now`, extrapolated from the last flip. Falls back to one period out when no
    /// flip has been seen, which is the timer-driven case.
    [[nodiscard]] lx::runtime::clock_time next_deadline(lx::runtime::clock_time now) const {
        if (last_present_ns_ <= 0 || period_ns_ <= 0)
            return now + period_ns_;
        const auto elapsed = now - last_present_ns_;
        if (elapsed < 0)
            return last_present_ns_ + period_ns_;
        // Whole periods since the flip, then one more — the next vblank strictly ahead.
        const auto periods = elapsed / period_ns_ + 1;
        return last_present_ns_ + periods * period_ns_;
    }

    /// When the next repaint should begin. Never in the past, and never so close to the
    /// deadline that the frame cannot land.
    [[nodiscard]] lx::runtime::clock_time next_start(lx::runtime::clock_time now) const {
        const auto deadline = next_deadline(now);
        const auto lead = estimate_ns() + k_margin_ns;

        auto start = deadline - lead;
        if (start < now) {
            // The repaint no longer fits before this deadline. Aim at the following one
            // rather than starting late — a frame that misses is shown a period late
            // anyway, and starting on time for the next keeps the cadence.
            const auto next = deadline + period_ns_;
            start = next - lead;
            if (start < now)
                start = now; // cost exceeds a full period; run flat out
        }
        if (deadline - start < k_min_lead_ns)
            start = deadline - k_min_lead_ns;
        return start > now ? start : now;
    }

    [[nodiscard]] unsigned present_count() const { return presents_; }
    [[nodiscard]] bool has_presented() const { return last_present_ns_ > 0; }

    /// True when the measured cost no longer fits in a refresh period, so the compositor
    /// cannot hit the display's rate however it is scheduled. Worth reporting: it is the
    /// difference between "scheduled badly" and "too slow", which look identical from
    /// outside.
    [[nodiscard]] bool over_budget_for_refresh() const {
        return estimate_ns() + k_margin_ns >= period_ns_;
    }

private:
    lx::runtime::clock_time period_ns_ = 16'666'667;
    lx::runtime::clock_time last_present_ns_ = 0;
    lx::runtime::clock_time window_[k_window]{};
    unsigned cursor_ = 0;
    unsigned filled_ = 0;
    unsigned presents_ = 0;
};

} // namespace lx::scheduler
