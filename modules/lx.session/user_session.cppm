module;

// The systemd user session around the compositor (design-session-and-wm S.3, S.4, S.9):
// the sd_notify protocol to the service manager (READY, RELOADING, STOPPING, STATUS and the
// watchdog), and the session environment exported to the user manager and to D-Bus
// activation, and unset again at the end. Declarations only; the bodies are in
// user_session.cpp. No sd-bus or sd-event type crosses this interface (`void*`).

import lx.foundation;

export module lx.session:user_session;

import std;

export namespace lx::session {

/// The sd_notify(3) protocol. Every call is a no-op when the process was not started by a
/// service manager that listens ($NOTIFY_SOCKET unset). One datagram each, non-blocking.
struct service_notifier {
    /// $NOTIFY_SOCKET is set: a manager waits for READY.
    [[nodiscard]] static bool listening();
    /// READY=1 with STATUS=`status` (null: none).
    static void ready(const char* status);
    /// RELOADING=1 with MONOTONIC_USEC (what Type=notify-reload requires), then READY when
    /// the reload is done.
    static void reloading(const char* status);
    /// STOPPING=1: a logout started; the manager shows "deactivating" from here on.
    static void stopping(const char* status);
    static void status(const char* status);
    /// The watchdog interval systemd expects pings within (WatchdogSec=), in µs; 0 off.
    [[nodiscard]] static unsigned long long watchdog_usec();
    /// WATCHDOG=1.
    static void watchdog_ping();
};

/// The session environment on the user bus: org.freedesktop.systemd1.Manager.SetEnvironment
/// and UnsetEnvironment, and org.freedesktop.DBus.UpdateActivationEnvironment. UI affinity.
/// The bus is attached to an sd-event loop the caller owns (the control socket's), so its
/// replies are dispatched with that loop's fd and nothing here blocks except `unset_now`.
class user_environment {
public:
    user_environment() = default;
    ~user_environment();
    user_environment(const user_environment&) = delete;
    user_environment& operator=(const user_environment&) = delete;

    /// Connects to the user bus (a local socket; authentication completes asynchronously)
    /// and attaches it to `sd_event` (an `sd_event*`).
    [[nodiscard]] lx::result<void> open(void* sd_event);
    [[nodiscard]] bool is_open() const { return bus_ != nullptr; }

    /// Exports `assignments` ("NAME=value") to the user manager and to D-Bus activation,
    /// asynchronously. `done(user, ok)` runs once both calls answered (ok: the manager
    /// accepted it) or after `timeout_ms` — from inside the event loop's dispatch.
    /// Names are remembered for `unset_now`. One export at a time.
    [[nodiscard]] lx::result<void> export_async(std::span<const std::string_view> assignments,
                                                unsigned timeout_ms,
                                                void (*done)(void* user, bool ok), void* user);
    /// The last export was accepted by the user manager.
    [[nodiscard]] bool exported() const { return exported_; }
    /// An export was sent and has not been answered yet (the manager may still apply it).
    [[nodiscard]] bool export_pending() const { return pending_ > 0; }
    /// UnsetEnvironment of every name exported, synchronously within `timeout_ms` (at
    /// teardown, when nothing else runs). The D-Bus activation environment cannot be unset
    /// (dbus-broker has no such call); the user manager's is what systemd-activated D-Bus
    /// services start with.
    void unset_now(unsigned timeout_ms);

    /// Detaches from the event loop and closes the bus (before the loop is destroyed).
    void close();

    /// Internal: one of the two export calls answered (the sd-bus reply handlers).
    void reply_arrived(bool manager, bool ok);

private:

    void* bus_ = nullptr;
    void* set_slot_ = nullptr;
    void* activation_slot_ = nullptr;
    unsigned pending_ = 0;
    bool manager_ok_ = false;
    bool exported_ = false;
    void (*done_)(void*, bool) = nullptr;
    void* done_user_ = nullptr;
    static constexpr unsigned k_max_names = 16;
    std::inplace_vector<std::array<char, 64>, k_max_names> names_{};
};

} // namespace lx::session
