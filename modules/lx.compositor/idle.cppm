module;

// Idle tracking (ext-idle-notify-v1, staging, v2) and idle inhibitors (idle-inhibit-unstable-v1):
// which notifications are idle, when the next one becomes due, and whether a visible inhibitor
// holds the inhibitor-obeying ones back. Pure and Wayland-free: resources and clients are
// opaque pointers, events leave through an `idle_sink` the wire unit implements
// (wire_idle.cpp). UI affinity throughout. See docs/architecture.md, "Idle, inhibition and
// display power".
//
// Activity is real input only (design I2): key presses and releases, pointer motion, buttons,
// scrolling and touch from libinput, counted before any binding or routing — never key
// repeat, compositor-synthesised motion or a virtual keyboard. A VT resume counts too.
//
// The timer is lazy. `note_activity` runs for every pointer motion, so it only stores the time
// (and resumes what is idle, which an idle counter makes free when nothing is). The
// compositor's one idle timer is armed at `next_deadline()`; when it fires, `on_timer` idles
// what is due and the timer is re-armed for the rest, whose deadlines activity may have pushed
// out meanwhile. One wakeup per timeout period at most, never one per input event.
//
// A notification's deadline is `start + timeout`, where `start` is the latest of its creation,
// the last activity, and — for one that obeys inhibitors — the end of the last inhibition
// (wlroots and KWin restart obeying timers there too). While inhibited, an obeying
// notification has no deadline. Becoming inhibited never resumes one that is already idle:
// zwp_idle_inhibitor_v1 "isn't honored if the system was already idled at the time" (wlroots
// resumes it instead; Lumen follows the protocol text, design I3).
//
// A zero timeout is idle at once, and again right after every resume (the protocol's "as soon
// as possible when the seat is inactive"; wlroots does the same).

export module lx.compositor:idle;

import lx.foundation;

export namespace lx::compositor {

/// Where idle events go. `idled` and `resumed` alternate per notification by construction
/// (sending either twice in a row is a compositor protocol error).
struct idle_sink {
    void* user = nullptr;
    void (*idled)(void* user, void* resource) = nullptr;
    void (*resumed)(void* user, void* resource) = nullptr;
};

class idle_tracker {
public:
    /// CLOCK_MONOTONIC nanoseconds.
    using clock_ns = unsigned long long;
    /// Notifications in the session, and per client: a client cannot take every slot from
    /// the others (the shell's own policy notification among them).
    static constexpr unsigned k_max = 64;
    static constexpr unsigned k_max_per_client = 16;

    /// A notification created at `now` for `client`. `obey_inhibitors` is false for
    /// ext_idle_notifier_v1.get_input_idle_notification (v2). A zero timeout is idle at once
    /// unless an inhibitor holds it. False when there is no room (the caller reports
    /// no_memory); the sink is not called then.
    [[nodiscard]] bool add(void* resource, void* client, unsigned timeout_ms, bool obey_inhibitors,
                           clock_ns now, const idle_sink& sink);
    /// Forgets a notification (destroy request, or its resource destructor). No event.
    void remove(void* resource);
    /// Real user activity at `now`: every idle notification is resumed (and a zero-timeout one
    /// idled again at once). O(1) while none is idle. True when the next deadline moved
    /// earlier than the armed one could be — the caller re-arms its timer.
    bool note_activity(clock_ns now, const idle_sink& sink);
    /// An inhibitor became honoured or stopped being so. No event either way (I3); obeying
    /// timers restart when it ends. True when the next deadline changed.
    bool set_inhibited(bool inhibited, clock_ns now);
    /// The timer fired at `now`: idles every notification that is due. Re-arm afterwards at
    /// `next_deadline()`.
    void on_timer(clock_ns now, const idle_sink& sink);

    /// The earliest deadline of a notification that is not idle; 0 when there is none.
    [[nodiscard]] clock_ns next_deadline() const;
    [[nodiscard]] unsigned count() const { return count_; }
    [[nodiscard]] unsigned idle_count() const { return idle_count_; }
    [[nodiscard]] bool inhibited() const { return inhibited_; }
    [[nodiscard]] clock_ns last_activity() const { return last_activity_; }
    /// Whether `resource` is idle right now (tests).
    [[nodiscard]] bool is_idle(const void* resource) const;

private:
    struct entry {
        void* resource = nullptr;
        void* client = nullptr;
        clock_ns timeout_ns = 0;
        clock_ns created = 0;
        bool obey = true;
        bool idle = false;
    };
    /// 0 when `e` has no deadline (idle already, or held by an inhibitor).
    [[nodiscard]] clock_ns deadline_of(const entry& e) const;
    void set_idle(entry& e, bool idle, const idle_sink& sink);

    entry entries_[k_max]{};
    unsigned count_ = 0;
    unsigned idle_count_ = 0;
    clock_ns last_activity_ = 0;
    clock_ns uninhibited_since_ = 0;
    bool inhibited_ = false;
};

/// zwp_idle_inhibitor_v1 objects and the surface each names. Surface ids are never reused,
/// so an inhibitor whose wl_surface is gone simply names a surface that is never visible
/// again (it is inert, as the protocol wants, without a hook on the surface).
class inhibitor_set {
public:
    static constexpr unsigned k_max = 64;
    static constexpr unsigned k_max_per_client = 16;

    /// False when there is no room (the caller reports no_memory).
    [[nodiscard]] bool add(void* resource, void* client, lx::surface_id surface);
    void remove(void* resource);
    [[nodiscard]] unsigned count() const { return count_; }
    /// True when any inhibitor's surface passes `visible` — the visibility pass of the last
    /// published frame (on screen, not covered, on a powered output). O(count).
    [[nodiscard]] bool any(bool (*visible)(void* user, lx::surface_id surface), void* user) const;

private:
    struct entry {
        void* resource = nullptr;
        void* client = nullptr;
        lx::surface_id surface{};
    };
    entry entries_[k_max]{};
    unsigned count_ = 0;
};

} // namespace lx::compositor

// ── idle_tracker ────────────────────────────────────────────────────────────

lx::compositor::idle_tracker::clock_ns
lx::compositor::idle_tracker::deadline_of(const entry& e) const {
    if (e.idle || (e.obey && inhibited_))
        return 0;
    clock_ns start = e.created;
    if (last_activity_ > start)
        start = last_activity_;
    if (e.obey && uninhibited_since_ > start)
        start = uninhibited_since_;
    const clock_ns d = start + e.timeout_ns;
    return d == 0 ? 1 : d; // 0 means "none"
}

void lx::compositor::idle_tracker::set_idle(entry& e, bool idle, const idle_sink& sink) {
    if (e.idle == idle)
        return;
    e.idle = idle;
    if (idle) {
        ++idle_count_;
        if (sink.idled)
            sink.idled(sink.user, e.resource);
    } else {
        --idle_count_;
        if (sink.resumed)
            sink.resumed(sink.user, e.resource);
    }
}

bool lx::compositor::idle_tracker::add(void* resource, void* client, unsigned timeout_ms,
                                       bool obey_inhibitors, clock_ns now,
                                       const idle_sink& sink) {
    if (!resource || count_ >= k_max)
        return false;
    unsigned mine = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].client == client)
            ++mine;
    if (mine >= k_max_per_client)
        return false;
    entry& e = entries_[count_++];
    e = {};
    e.resource = resource;
    e.client = client;
    e.timeout_ns = static_cast<clock_ns>(timeout_ms) * 1'000'000ull;
    e.created = now;
    e.obey = obey_inhibitors;
    // Due at once: idled in the same dispatch (no timer round trip for a zero timeout).
    if (e.timeout_ns == 0 && !(e.obey && inhibited_))
        set_idle(e, true, sink);
    return true;
}

void lx::compositor::idle_tracker::remove(void* resource) {
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].resource != resource)
            continue;
        if (entries_[i].idle)
            --idle_count_;
        entries_[i] = entries_[--count_];
        entries_[count_] = {};
        return;
    }
}

bool lx::compositor::idle_tracker::note_activity(clock_ns now, const idle_sink& sink) {
    last_activity_ = now;
    if (idle_count_ == 0)
        return false; // every deadline only moved later: the armed timer fires early, once
    bool rearm = false;
    for (unsigned i = 0; i < count_ && idle_count_ > 0; ++i) {
        entry& e = entries_[i];
        if (!e.idle)
            continue;
        set_idle(e, false, sink);
        if (e.timeout_ns == 0 && !(e.obey && inhibited_))
            set_idle(e, true, sink); // still inactive the moment after: idle again
        else
            rearm = true;
    }
    return rearm;
}

bool lx::compositor::idle_tracker::set_inhibited(bool inhibited, clock_ns now) {
    if (inhibited == inhibited_)
        return false;
    inhibited_ = inhibited;
    if (!inhibited)
        uninhibited_since_ = now;
    return true;
}

void lx::compositor::idle_tracker::on_timer(clock_ns now, const idle_sink& sink) {
    for (unsigned i = 0; i < count_; ++i) {
        entry& e = entries_[i];
        const clock_ns d = deadline_of(e);
        if (d != 0 && d <= now)
            set_idle(e, true, sink);
    }
}

lx::compositor::idle_tracker::clock_ns lx::compositor::idle_tracker::next_deadline() const {
    clock_ns best = 0;
    for (unsigned i = 0; i < count_; ++i) {
        const clock_ns d = deadline_of(entries_[i]);
        if (d != 0 && (best == 0 || d < best))
            best = d;
    }
    return best;
}

bool lx::compositor::idle_tracker::is_idle(const void* resource) const {
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].resource == resource)
            return entries_[i].idle;
    return false;
}

// ── inhibitor_set ───────────────────────────────────────────────────────────

bool lx::compositor::inhibitor_set::add(void* resource, void* client, lx::surface_id surface) {
    if (!resource || count_ >= k_max)
        return false;
    unsigned mine = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].client == client)
            ++mine;
    if (mine >= k_max_per_client)
        return false;
    entries_[count_++] = {resource, client, surface};
    return true;
}

void lx::compositor::inhibitor_set::remove(void* resource) {
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].resource != resource)
            continue;
        entries_[i] = entries_[--count_];
        entries_[count_] = {};
        return;
    }
}

bool lx::compositor::inhibitor_set::any(bool (*visible)(void* user, lx::surface_id surface),
                                        void* user) const {
    if (!visible)
        return false;
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].surface && visible(user, entries_[i].surface))
            return true;
    return false;
}
