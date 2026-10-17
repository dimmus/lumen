module;

export module lx.compositor:session_state;

// The session's pause/resume state machine (VT switching, logind PauseDevice/ResumeDevice
// and Session.Active). Pure, UI affinity, free of Wayland, libdrm and D-Bus: the compositor
// feeds it events and executes the actions it returns, in the order documented on
// `session_actions`. See docs/architecture.md, "Session pause/resume".
//
//   active ──pause edge──▶ paused ──resume edge──▶ resuming ──render ok──▶ active
//                            ▲                        │ (device unavailable / rejected)
//                            └────────────────────────┘
//   any ── DRM "gone" ──▶ display_lost (terminal; input keeps working)

export namespace lx::compositor {

enum class session_phase : unsigned char { disabled, active, paused, resuming, display_lost };

/// logind's PauseDevice type: `pause` must be acknowledged, `force` and `gone` must not.
enum class session_pause_kind : unsigned char { pause, force, gone };

/// How the render thread's resume modeset went.
enum class session_resume_outcome : unsigned char {
    ok,
    /// EACCES/EPERM/EBUSY: master is not ours (again). Retried at the next trigger.
    device_unavailable,
    /// The kernel refused the state (EINVAL — typically a mode that no longer exists).
    /// Retried only on the next ResumeDevice.
    rejected,
};

/// What to do, as bits. When several are set the compositor runs them in this order:
/// stop_display, suspend_input, begin_resume, resume_input, complete_pause; the log bits
/// accompany them.
struct session_actions {
    enum : unsigned {
        /// Gate the render thread (no composite, no commit), ask it to quiesce
        /// (`quiesce_gen`), stop the frame clock in favour of the 1 Hz paused tick, end
        /// interactive grabs and drags, mark windows suspended.
        stop_display = 1u << 0,
        /// libinput suspend (releasing every device), the held-key and button sweep,
        /// keyboard leave.
        suspend_input = 1u << 1,
        /// Ungate render with `resume_gen` (it runs the resume procedure and the modeset),
        /// resume input, keyboard enter, unsuspend windows, full damage, frame clock.
        begin_resume = 1u << 2,
        /// libinput resume and keyboard enter only (the display stays down).
        resume_input = 1u << 3,
        /// PauseDeviceComplete(ack_major, ack_minor).
        complete_pause = 1u << 4,
        log_paused = 1u << 5,
        log_resumed = 1u << 6,
        log_resume_rejected = 1u << 7,
        log_display_lost = 1u << 8,
    };
    unsigned bits = 0;
    unsigned ack_major = 0;
    unsigned ack_minor = 0;
    unsigned quiesce_gen = 0;
    unsigned resume_gen = 0;
    /// For `log_paused`: what started it.
    session_pause_kind kind = session_pause_kind::force;
    bool by_active = false;

    [[nodiscard]] bool has(unsigned b) const { return (bits & b) != 0; }
    [[nodiscard]] bool empty() const { return bits == 0; }
};

class session_state {
public:
    /// `drm_present`: this compositor scans out on a card logind manages (`drm_major`:
    /// `drm_minor`). Without it the machine is disabled and every event is ignored.
    /// `start_inactive`: TakeDevice said the session is not in the foreground; the
    /// returned actions pause at once.
    [[nodiscard]] session_actions init(bool drm_present, unsigned drm_major, unsigned drm_minor,
                                       bool start_inactive);

    [[nodiscard]] session_actions on_device_paused(unsigned major, unsigned minor,
                                                   session_pause_kind kind);
    [[nodiscard]] session_actions on_device_resumed(unsigned major, unsigned minor);
    /// Session.Active changed. `drm_master`: drmIsMaster on the card right now.
    [[nodiscard]] session_actions on_active_changed(bool active, bool drm_master);
    /// The 1 Hz tick while paused: covers a lost ResumeDevice and a card opened without
    /// logind (master comes back with the VT, no signal).
    [[nodiscard]] session_actions on_paused_tick(bool active, bool drm_master);
    /// The render thread ran the quiesce task posted with `gen`.
    [[nodiscard]] session_actions on_render_quiesced(unsigned gen);
    /// The render thread's resume modeset for `gen`.
    [[nodiscard]] session_actions on_resume_result(unsigned gen, session_resume_outcome outcome);

    [[nodiscard]] session_phase phase() const { return phase_; }
    [[nodiscard]] bool enabled() const { return phase_ != session_phase::disabled; }
    /// Nothing is presented: paused, or the display is gone.
    [[nodiscard]] bool display_stopped() const {
        return phase_ == session_phase::paused || phase_ == session_phase::display_lost;
    }
    [[nodiscard]] bool input_suspended() const { return input_suspended_; }
    [[nodiscard]] bool is_drm(unsigned major, unsigned minor) const {
        return major == drm_major_ && minor == drm_minor_;
    }
    /// A `pause` acknowledgement waits for the render thread.
    [[nodiscard]] bool ack_pending() const { return pending_ack_; }
    [[nodiscard]] unsigned pause_generation() const { return pause_gen_; }
    [[nodiscard]] unsigned resume_generation() const { return resume_gen_; }

private:
    /// The pause edge from active or resuming.
    void pause_edge(session_actions& a, bool suspend_input);
    void resume_edge(session_actions& a);

    session_phase phase_ = session_phase::disabled;
    unsigned drm_major_ = ~0u;
    unsigned drm_minor_ = ~0u;
    unsigned pause_gen_ = 0;
    unsigned quiesced_gen_ = 0;
    unsigned resume_gen_ = 0;
    bool pending_ack_ = false;
    bool input_suspended_ = false;
    /// The last resume was refused by the kernel: only a new ResumeDevice retries.
    bool hold_until_resume_device_ = false;
    /// `log_resume_rejected` once per hold.
    bool rejected_logged_ = false;
};

} // namespace lx::compositor

lx::compositor::session_actions lx::compositor::session_state::init(bool drm_present,
                                                                    unsigned drm_major,
                                                                    unsigned drm_minor,
                                                                    bool start_inactive) {
    *this = {};
    session_actions a{};
    if (!drm_present)
        return a;
    drm_major_ = drm_major;
    drm_minor_ = drm_minor;
    phase_ = session_phase::active;
    if (start_inactive) {
        a.by_active = true;
        pause_edge(a, true);
    }
    return a;
}

void lx::compositor::session_state::pause_edge(session_actions& a, bool suspend_input) {
    a.bits |= session_actions::stop_display | session_actions::log_paused;
    a.quiesce_gen = ++pause_gen_;
    if (suspend_input && !input_suspended_) {
        a.bits |= session_actions::suspend_input;
        input_suspended_ = true;
    }
    phase_ = session_phase::paused;
}

void lx::compositor::session_state::resume_edge(session_actions& a) {
    a.bits |= session_actions::begin_resume;
    a.resume_gen = ++resume_gen_;
    input_suspended_ = false;
    phase_ = session_phase::resuming;
}

lx::compositor::session_actions
lx::compositor::session_state::on_device_paused(unsigned major, unsigned minor,
                                                session_pause_kind kind) {
    session_actions a{};
    a.kind = kind;
    // evdev devices: libinput notices the revoked fd (or has been suspended already); a
    // `pause` for one still held is acknowledged by the compositor after the batch.
    if (!enabled() || !is_drm(major, minor) || phase_ == session_phase::display_lost)
        return a;
    if (kind == session_pause_kind::gone) {
        // Unplugged: never acknowledged, never coming back. Clients and input go on (and
        // the keyboard way out with them).
        pending_ack_ = false;
        if (phase_ == session_phase::active || phase_ == session_phase::resuming) {
            a.bits |= session_actions::stop_display;
            a.quiesce_gen = ++pause_gen_;
        }
        a.bits |= session_actions::log_display_lost;
        phase_ = session_phase::display_lost;
        return a;
    }
    if (phase_ == session_phase::active || phase_ == session_phase::resuming)
        pause_edge(a, true);
    if (kind == session_pause_kind::pause) {
        pending_ack_ = true;
        // Already quiesced for this pause (a duplicate signal): acknowledge now.
        if (quiesced_gen_ == pause_gen_ && !a.has(session_actions::stop_display)) {
            a.bits |= session_actions::complete_pause;
            a.ack_major = major;
            a.ack_minor = minor;
            pending_ack_ = false;
        }
    }
    return a;
}

lx::compositor::session_actions lx::compositor::session_state::on_device_resumed(unsigned major,
                                                                                 unsigned minor) {
    session_actions a{};
    if (!enabled() || !is_drm(major, minor) || phase_ != session_phase::paused)
        return a;
    hold_until_resume_device_ = false;
    rejected_logged_ = false;
    pending_ack_ = false; // the switch it belonged to is over
    resume_edge(a);
    return a;
}

lx::compositor::session_actions lx::compositor::session_state::on_active_changed(bool active,
                                                                                 bool drm_master) {
    session_actions a{};
    if (!enabled())
        return a;
    if (!active) {
        if (phase_ == session_phase::active || phase_ == session_phase::resuming) {
            a.by_active = true;
            pause_edge(a, true);
        } else if (phase_ == session_phase::display_lost && !input_suspended_) {
            // No display, but the input devices go with the session all the same.
            a.bits |= session_actions::suspend_input;
            input_suspended_ = true;
        }
        return a;
    }
    if (phase_ == session_phase::paused && !hold_until_resume_device_ && drm_master) {
        resume_edge(a);
    } else if (phase_ == session_phase::display_lost && input_suspended_) {
        a.bits |= session_actions::resume_input;
        input_suspended_ = false;
    }
    return a;
}

lx::compositor::session_actions lx::compositor::session_state::on_paused_tick(bool active,
                                                                              bool drm_master) {
    session_actions a{};
    if (phase_ == session_phase::paused && !hold_until_resume_device_ && active && drm_master)
        resume_edge(a);
    return a;
}

lx::compositor::session_actions lx::compositor::session_state::on_render_quiesced(unsigned gen) {
    session_actions a{};
    if (gen != pause_gen_)
        return a; // an older pause's round trip
    quiesced_gen_ = gen;
    if (pending_ack_ && (phase_ == session_phase::paused)) {
        a.bits |= session_actions::complete_pause;
        a.ack_major = drm_major_;
        a.ack_minor = drm_minor_;
        pending_ack_ = false;
    }
    return a;
}

lx::compositor::session_actions
lx::compositor::session_state::on_resume_result(unsigned gen, session_resume_outcome outcome) {
    session_actions a{};
    if (phase_ != session_phase::resuming || gen != resume_gen_)
        return a; // a resume that a later pause already superseded
    switch (outcome) {
    case session_resume_outcome::ok:
        phase_ = session_phase::active;
        a.bits |= session_actions::log_resumed;
        break;
    case session_resume_outcome::device_unavailable:
        a.by_active = true;
        pause_edge(a, true);
        a.bits &= ~static_cast<unsigned>(session_actions::log_paused);
        break;
    case session_resume_outcome::rejected:
        pause_edge(a, true);
        a.bits &= ~static_cast<unsigned>(session_actions::log_paused);
        hold_until_resume_device_ = true;
        if (!rejected_logged_) {
            rejected_logged_ = true;
            a.bits |= session_actions::log_resume_rejected;
        }
        break;
    }
    return a;
}
