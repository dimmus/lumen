module;

import lx.foundation;
import lx.runtime;

export module lx.scheduler:repaint;

import std;

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

    /// Adaptive sync (VRR, ADR 008 §C.4): the display refreshes when a frame arrives, no
    /// sooner than one period of the mode's refresh after the last flip (its highest rate),
    /// so there is no vblank phase to aim at — a repaint starts on demand, as soon as it can
    /// land after that minimum interval (niri's VRR frame clock).
    void set_vrr(bool on) { vrr_ = on; }
    [[nodiscard]] bool vrr() const { return vrr_; }

    /// When the next repaint should begin. Never in the past, and never so close to the
    /// deadline that the frame cannot land.
    [[nodiscard]] lx::runtime::clock_time next_start(lx::runtime::clock_time now) const {
        if (vrr_) {
            if (last_present_ns_ <= 0)
                return now;
            const auto earliest = last_present_ns_ + period_ns_ - (estimate_ns() + k_margin_ns);
            return earliest > now ? earliest : now;
        }
        // No flip yet, so no display phase to aim at: start now. Aiming at "one period out"
        // only delays the first frame (by the default estimate's half period); a clock
        // that never flips (virtual outputs) is still paced by `frame_clocks::take_due`.
        if (last_present_ns_ <= 0)
            return now;
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
    bool vrr_ = false;
    lx::runtime::clock_time window_[k_window]{};
    unsigned cursor_ = 0;
    unsigned filled_ = 0;
    unsigned presents_ = 0;
};

/// One frame clock per output over one shared timer (ADR 008 M6; Mutter's per-view frame
/// clocks, with one timerfd). Each output repaints on its own display's phase — its
/// `repaint_scheduler` predicts its next start from its own flips — and an output with
/// nothing to do stops on its own. The owner arms one timer at `next_wakeup()`; when it
/// fires, `take_due` hands back every output whose start falls within `k_coalesce_ns`, so
/// two outputs in phase cost one tick. Pure, no allocation; UI affinity.
class frame_clocks {
public:
    static constexpr unsigned k_max = 16;
    /// Outputs due within this of the earliest are served by the same tick.
    static constexpr lx::runtime::clock_time k_coalesce_ns = 1'000'000;
    /// Consecutive quiet ticks before an output's clock stops.
    static constexpr unsigned k_idle_ticks = 2;

    /// An output joins (`live`) or leaves; joining resets its clock to `period_ns`.
    void set_live(unsigned slot, bool live, lx::runtime::clock_time period_ns);
    [[nodiscard]] bool live(unsigned slot) const { return slot < k_max && (live_ >> slot) & 1u; }
    /// A dark output (powered off, or the session paused) is never armed; going dark disarms.
    void set_dark(unsigned slot, bool dark);
    /// Output `slot` runs with adaptive sync: its clock starts repaints on demand
    /// (`repaint_scheduler::set_vrr`).
    void set_vrr(unsigned slot, bool on) {
        if (slot < k_max)
            clocks_[slot].set_vrr(on);
    }
    [[nodiscard]] repaint_scheduler& clock(unsigned slot) { return clocks_[slot < k_max ? slot : 0]; }
    [[nodiscard]] const repaint_scheduler& clock(unsigned slot) const {
        return clocks_[slot < k_max ? slot : 0];
    }

    /// Starts the clocks of `mask` that are stopped, each at its own next start (its display's
    /// phase continues while stopped, so the last flip still predicts it). Running ones keep
    /// their schedule. Returns the outputs it started.
    std::uint32_t request(std::uint32_t mask, lx::runtime::clock_time now);
    /// commit-timing-v1: starts `slot`'s clock (when stopped) for the tick aiming at the
    /// first vblank at or after `target − slack` — no earlier tick runs for it, so a desktop
    /// waiting on a target wakes once. A running clock keeps its schedule (each tick checks
    /// the target). True when it started.
    bool request_at(unsigned slot, lx::runtime::clock_time target, lx::runtime::clock_time slack,
                    lx::runtime::clock_time now);
    /// A flip of `slot` landed at `flip_ns`: its phase, and its next start from `now` — never
    /// sooner than one period after the start of the tick that produced it. A display whose
    /// flips complete when the frame arrives rather than at a vblank (vmwgfx, virtual
    /// outputs) re-phases the clock to each flip; with a large repaint estimate the start
    /// aimed back from the following "vblank" would then come less than a period after the
    /// last one, and the cadence would run above the refresh rate (FIFO clients presented
    /// 8 ms apart after one slow frame). On a vblank-locked display the floor never binds.
    void on_present(unsigned slot, lx::runtime::clock_time flip_ns, lx::runtime::clock_time now);
    /// The earliest start among running clocks; 0 when none runs.
    [[nodiscard]] lx::runtime::clock_time next_wakeup() const;
    /// Every running output whose start is at or before `now + k_coalesce_ns`; each is moved
    /// to its following start (a flip re-phases it when one comes).
    [[nodiscard]] std::uint32_t take_due(lx::runtime::clock_time now);
    /// After a tick served `slot`: a quiet tick counts toward stopping it (`k_idle_ticks` in
    /// a row), a busy one resets the count. True when its clock stopped.
    bool settle(unsigned slot, bool quiet);
    /// Stops `slot` now (its quiet count is spent: the housekeeping tick needs one more).
    void stop(unsigned slot);
    [[nodiscard]] std::uint32_t running_mask() const { return running_; }
    [[nodiscard]] std::uint32_t live_mask() const { return live_; }
    [[nodiscard]] std::uint32_t dark_mask() const { return dark_; }
    [[nodiscard]] lx::runtime::clock_time start_of(unsigned slot) const {
        return slot < k_max ? next_start_[slot] : 0;
    }

private:
    repaint_scheduler clocks_[k_max]{};
    lx::runtime::clock_time next_start_[k_max]{};
    /// The start of the last tick served per output (0: none since it went live).
    lx::runtime::clock_time last_tick_[k_max]{};
    unsigned quiet_[k_max]{};
    std::uint32_t live_ = 0;
    std::uint32_t dark_ = 0;
    std::uint32_t running_ = 0;
};

} // namespace lx::scheduler

void lx::scheduler::frame_clocks::set_live(unsigned slot, bool live,
                                           lx::runtime::clock_time period_ns) {
    if (slot >= k_max)
        return;
    const std::uint32_t bit = 1u << slot;
    running_ &= ~bit;
    quiet_[slot] = 0;
    next_start_[slot] = 0;
    last_tick_[slot] = 0;
    if (live) {
        live_ |= bit;
        clocks_[slot] = repaint_scheduler{};
        clocks_[slot].set_refresh_period(period_ns);
    } else {
        live_ &= ~bit;
        dark_ &= ~bit;
    }
}

void lx::scheduler::frame_clocks::set_dark(unsigned slot, bool dark) {
    if (slot >= k_max)
        return;
    const std::uint32_t bit = 1u << slot;
    if (dark) {
        dark_ |= bit;
        running_ &= ~bit;
    } else {
        dark_ &= ~bit;
    }
}

std::uint32_t lx::scheduler::frame_clocks::request(std::uint32_t mask, lx::runtime::clock_time now) {
    const std::uint32_t start = mask & live_ & ~dark_ & ~running_;
    for (unsigned s = 0; s < k_max; ++s) {
        if (!(start & (1u << s)))
            continue;
        next_start_[s] = clocks_[s].next_start(now);
        quiet_[s] = 0;
    }
    running_ |= start;
    return start;
}

bool lx::scheduler::frame_clocks::request_at(unsigned slot, lx::runtime::clock_time target,
                                             lx::runtime::clock_time slack,
                                             lx::runtime::clock_time now) {
    if (slot >= k_max)
        return false;
    const std::uint32_t bit = 1u << slot;
    if (!(live_ & bit) || (dark_ & bit) || (running_ & bit))
        return false;
    const repaint_scheduler& c = clocks_[slot];
    // `next_deadline` is the first vblank strictly after its argument.
    const lx::runtime::clock_time from = target - slack - 1 > now ? target - slack - 1 : now;
    const lx::runtime::clock_time deadline = c.next_deadline(from);
    lx::runtime::clock_time start = deadline - (c.estimate_ns() + repaint_scheduler::k_margin_ns);
    if (start < now)
        start = now;
    next_start_[slot] = start;
    quiet_[slot] = 0;
    running_ |= bit;
    return true;
}

void lx::scheduler::frame_clocks::on_present(unsigned slot, lx::runtime::clock_time flip_ns,
                                             lx::runtime::clock_time now) {
    if (slot >= k_max || !live(slot))
        return;
    clocks_[slot].on_present(flip_ns);
    if (!(running_ & (1u << slot)))
        return;
    lx::runtime::clock_time next = clocks_[slot].next_start(now);
    if (last_tick_[slot] > 0) {
        const lx::runtime::clock_time floor =
            last_tick_[slot] + clocks_[slot].refresh_period() - k_coalesce_ns;
        if (next < floor)
            next = floor;
    }
    next_start_[slot] = next;
}

lx::runtime::clock_time lx::scheduler::frame_clocks::next_wakeup() const {
    lx::runtime::clock_time best = 0;
    for (unsigned s = 0; s < k_max; ++s)
        if ((running_ & (1u << s)) && (best == 0 || next_start_[s] < best))
            best = next_start_[s];
    return best;
}

std::uint32_t lx::scheduler::frame_clocks::take_due(lx::runtime::clock_time now) {
    std::uint32_t due = 0;
    for (unsigned s = 0; s < k_max; ++s) {
        if (!(running_ & (1u << s)) || next_start_[s] > now + k_coalesce_ns)
            continue;
        due |= 1u << s;
        last_tick_[s] = next_start_[s];
        // The following start: past the deadline this tick aims at, and never sooner than
        // one period after this one. When the repaint estimate exceeds a period (a slow
        // frame, a screenshot read back) `next_start` runs flat out (`now`): without the
        // floor every tick would be due again a millisecond later.
        const lx::runtime::clock_time floor =
            next_start_[s] + clocks_[s].refresh_period() - k_coalesce_ns;
        auto next = clocks_[s].next_start(now + k_coalesce_ns);
        if (next < floor)
            next = floor;
        if (next <= now)
            next = now + clocks_[s].refresh_period();
        next_start_[s] = next;
    }
    return due;
}

bool lx::scheduler::frame_clocks::settle(unsigned slot, bool quiet) {
    if (slot >= k_max || !(running_ & (1u << slot)))
        return false;
    if (!quiet) {
        quiet_[slot] = 0;
        return false;
    }
    if (++quiet_[slot] < k_idle_ticks)
        return false;
    running_ &= ~(1u << slot);
    quiet_[slot] = 0;
    return true;
}

void lx::scheduler::frame_clocks::stop(unsigned slot) {
    if (slot >= k_max)
        return;
    running_ &= ~(1u << slot);
    quiet_[slot] = 0;
}
