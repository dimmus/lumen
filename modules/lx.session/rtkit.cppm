module;

// RealtimeKit (org.freedesktop.RealtimeKit1 on the system bus): raises threads of a process
// that may not raise them itself — real-time (SCHED_RR | SCHED_RESET_ON_FORK) up to its
// MaxRealtimePriority for a process whose RLIMIT_RTTIME is at most RTTimeUSecMax, or a
// negative nice down to MinNiceLevel; polkit lets an active local session in. Declarations
// only; the bodies are in rtkit.cpp. No sd-bus type crosses this interface.

import lx.runtime;

export module lx.session:rtkit;

import std;

export namespace lx::session {

/// A `runtime::sched_broker` over rtkit, for `runtime::apply_thread_sched`. Its own system
/// bus connection, opened on first use and closed with the object; calls are synchronous
/// with a 1 s timeout (start-up only, never a frame path). One thread at a time.
class rtkit_client {
public:
    rtkit_client() = default;
    ~rtkit_client();
    rtkit_client(const rtkit_client&) = delete;
    rtkit_client& operator=(const rtkit_client&) = delete;

    /// The broker's calls, bound to this object. Without sd-bus: no broker (all null).
    [[nodiscard]] runtime::sched_broker broker();

    /// Each returns 0 or an errno; ENOENT: no system bus or no rtkit on it.
    [[nodiscard]] int limits(runtime::sched_broker_limits& out);
    [[nodiscard]] int make_realtime(int tid, int priority);
    [[nodiscard]] int make_high_priority(int tid, int nice);

private:
    [[nodiscard]] int connect();

    void* bus_ = nullptr;
    int connect_error_ = -1; ///< -1 not tried, 0 connected, else the errno
    int limits_error_ = -1;
    runtime::sched_broker_limits limits_{};
};

} // namespace lx::session
