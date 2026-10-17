module;

// Display power (design I4/I5): the output's power state machine. Pure, UI affinity, free of
// Wayland and libdrm — the compositor feeds it requests (zwlr_output_power_v1.set_mode),
// real input and the render thread's results, and executes the actions it returns. See
// docs/architecture.md, "Idle, inhibition and display power".
//
//   on ──request_off──▶ off_pending ──render: done / deferred──▶ off
//    ▲                       │ render: rejected ──▶ soft_blank (black frame, CRTC kept on)
//    │                       │ render: retry_after_flip ──▶ (posted again once the flip lands)
//    └─ on_pending ◀──wake / request_on── off, soft_blank, off_pending
//         │ render: ok ──▶ on          │ rejected ──▶ off (the next wake retries)
//
// Off is the kernel's atomic "DPMS off": CRTC ACTIVE=0 with MODE_ID and the connector routing
// kept, cursor plane off, one blocking ALLOW_MODESET commit by the render thread (the only
// committer). The resources stay reserved, so power-on cannot fail for lack of them; blocking
// means no flip event is needed (drivers differ on events for a disabled CRTC). On is the
// session-resume procedure: full repaint, then a full ALLOW_MODESET state with ACTIVE=1.
// While the power gate is up the display counts as stopped exactly as when the VT is away:
// render composites and commits nothing, the frame clock gives way to the 1 Hz paused tick
// (frame callbacks go on, so no client wedges), toplevels are `suspended` and in-flight
// presentation feedback is discarded. Input stays live — it wakes the output (I5).
//
// With the VT away nothing is committed: a request only changes the target (and what clients
// are told), and the VT resume — activity — brings the output back on with its own modeset.

export module lx.compositor:display_power;

export namespace lx::compositor {

enum class power_phase : unsigned char { on, off_pending, off, soft_blank, on_pending };

/// The render thread's power-off task.
enum class power_off_outcome : unsigned char {
    /// ACTIVE=0 committed.
    done,
    /// The VT is away (or master went with it): nothing to turn off — the output is not ours
    /// until the VT resume, which powers it on again.
    deferred,
    /// A flip is still queued; the CRTC takes one commit at a time. Posted again once it lands.
    retry_after_flip,
    /// The kernel refused every variant: a black frame was committed instead, CRTC on.
    rejected,
    /// The task belonged to a request a later one superseded; it did nothing.
    stale,
};

/// The resume modeset a power-on asked for.
enum class power_on_outcome : unsigned char { ok, device_unavailable, rejected };

/// What to do, as bits, in this order: gate, post_off, ungate, send_mode; the log bits
/// accompany them.
struct power_actions {
    enum : unsigned {
        /// Raise the power gate (see the file comment): stop the frame clock for the 1 Hz
        /// paused tick, suspend toplevels, discard in-flight presentation feedback.
        gate = 1u << 0,
        /// Post the render thread's power-off task for generation `gen`.
        post_off = 1u << 1,
        /// Lower the power gate; with `resume`, also start the resume procedure (new render
        /// generation, full damage, the frame clock): the output was powered down by us.
        ungate = 1u << 2,
        /// zwlr_output_power_v1.mode(`mode_on`) to every object of the output.
        send_mode = 1u << 3,
        /// The off task asked to wait for the queued flip: post it again once none is
        /// pending (at once if none is any more, else from the flip handler).
        wait_flip = 1u << 4,
        log_off = 1u << 5,
        log_soft_blank = 1u << 6,
        log_on = 1u << 7,
        log_on_failed = 1u << 8,
    };
    unsigned bits = 0;
    unsigned gen = 0;
    bool mode_on = true;
    bool resume = false;

    [[nodiscard]] bool has(unsigned b) const { return (bits & b) != 0; }
    [[nodiscard]] bool empty() const { return bits == 0; }
};

class display_power {
public:
    /// zwlr_output_power_v1.set_mode(off). `vt_away`: the session's display is paused.
    [[nodiscard]] power_actions request_off(bool vt_away);
    /// zwlr_output_power_v1.set_mode(on).
    [[nodiscard]] power_actions request_on(bool vt_away);
    /// Real input (I5): powers the output on when it is off or going off; nothing when on.
    [[nodiscard]] power_actions wake(bool vt_away);
    /// The render thread's power-off task for `gen`.
    [[nodiscard]] power_actions on_off_result(unsigned gen, power_off_outcome outcome);
    /// No flip is pending any more (the flip handler, or at once): a waiting off task is
    /// posted again.
    [[nodiscard]] power_actions on_flip_drained();
    /// The VT came back: activity. An output that was off is on again — the VT resume's own
    /// modeset turns it on, so no resume of ours is asked for.
    [[nodiscard]] power_actions on_vt_resumed();
    /// The resume modeset that `ungate` with `resume` asked for.
    [[nodiscard]] power_actions on_resume_result(power_on_outcome outcome);

    [[nodiscard]] power_phase phase() const { return phase_; }
    /// The power gate is to be up: render composites and commits nothing.
    [[nodiscard]] bool gated() const {
        return phase_ == power_phase::off_pending || phase_ == power_phase::off ||
               phase_ == power_phase::soft_blank;
    }
    /// Input is to wake the output.
    [[nodiscard]] bool wants_wake() const { return gated(); }
    /// What zwlr_output_power_v1 objects have been told (and a new one is told at once).
    [[nodiscard]] bool reported_on() const { return reported_on_; }
    /// The newest off task's generation: an older task finds itself stale.
    [[nodiscard]] unsigned generation() const { return gen_; }
    /// An off task waits for a flip to land.
    [[nodiscard]] bool waiting_for_flip() const { return waiting_flip_; }

private:
    void report(power_actions& a, bool on);
    power_actions power_on(bool vt_away);

    power_phase phase_ = power_phase::on;
    unsigned gen_ = 0;
    bool reported_on_ = true;
    bool waiting_flip_ = false;
};

} // namespace lx::compositor

void lx::compositor::display_power::report(power_actions& a, bool on) {
    if (reported_on_ == on)
        return;
    reported_on_ = on;
    a.bits |= power_actions::send_mode;
    a.mode_on = on;
}

lx::compositor::power_actions lx::compositor::display_power::request_off(bool vt_away) {
    power_actions a{};
    if (phase_ != power_phase::on && phase_ != power_phase::on_pending)
        return a; // already off or going off: no change, no event
    a.bits |= power_actions::gate;
    a.gen = ++gen_;
    waiting_flip_ = false;
    if (vt_away) {
        // Not ours to commit: the target changes, and the VT resume (activity) restores it.
        phase_ = power_phase::off;
        a.bits |= power_actions::log_off;
        report(a, false);
        return a;
    }
    phase_ = power_phase::off_pending;
    a.bits |= power_actions::post_off;
    return a;
}

lx::compositor::power_actions lx::compositor::display_power::power_on(bool vt_away) {
    power_actions a{};
    // Any off task still in flight is stale from here on.
    ++gen_;
    waiting_flip_ = false;
    a.bits |= power_actions::ungate;
    if (vt_away) {
        phase_ = power_phase::on;
        a.bits |= power_actions::log_on;
        report(a, true);
        return a;
    }
    a.resume = true;
    phase_ = power_phase::on_pending;
    return a;
}

lx::compositor::power_actions lx::compositor::display_power::request_on(bool vt_away) {
    if (!gated())
        return {};
    return power_on(vt_away);
}

lx::compositor::power_actions lx::compositor::display_power::wake(bool vt_away) {
    if (!wants_wake())
        return {};
    return power_on(vt_away);
}

lx::compositor::power_actions lx::compositor::display_power::on_off_result(unsigned gen,
                                                                           power_off_outcome outcome) {
    power_actions a{};
    if (gen != gen_ || phase_ != power_phase::off_pending)
        return a; // superseded by a later request
    switch (outcome) {
    case power_off_outcome::done:
    case power_off_outcome::deferred:
        phase_ = power_phase::off;
        a.bits |= power_actions::log_off;
        report(a, false);
        break;
    case power_off_outcome::rejected:
        phase_ = power_phase::soft_blank;
        a.bits |= power_actions::log_soft_blank;
        report(a, false);
        break;
    case power_off_outcome::retry_after_flip:
        waiting_flip_ = true;
        a.bits |= power_actions::wait_flip;
        break;
    case power_off_outcome::stale:
        break;
    }
    return a;
}

lx::compositor::power_actions lx::compositor::display_power::on_flip_drained() {
    power_actions a{};
    if (!waiting_flip_ || phase_ != power_phase::off_pending)
        return a;
    waiting_flip_ = false;
    a.bits |= power_actions::post_off;
    a.gen = gen_;
    return a;
}

lx::compositor::power_actions lx::compositor::display_power::on_vt_resumed() {
    if (!gated() && phase_ != power_phase::on_pending)
        return {};
    power_actions a{};
    ++gen_;
    waiting_flip_ = false;
    if (gated())
        a.bits |= power_actions::ungate | power_actions::log_on;
    phase_ = power_phase::on;
    report(a, true);
    return a;
}

lx::compositor::power_actions
lx::compositor::display_power::on_resume_result(power_on_outcome outcome) {
    power_actions a{};
    if (phase_ != power_phase::on_pending)
        return a; // a later request already decided
    switch (outcome) {
    case power_on_outcome::ok:
        phase_ = power_phase::on;
        a.bits |= power_actions::log_on;
        report(a, true);
        break;
    case power_on_outcome::device_unavailable:
        // Master went (a VT switch raced the wake): the session pauses, and its resume
        // brings the output back. As far as power goes, it is on.
        phase_ = power_phase::on;
        report(a, true);
        break;
    case power_on_outcome::rejected:
        // The mode is no longer offered (a VM window resized while dark): stay dark, and
        // let the next wake try again.
        phase_ = power_phase::off;
        a.bits |= power_actions::gate | power_actions::log_on_failed;
        break;
    }
    return a;
}
