module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

import lx.foundation;
import lx.runtime;

export module lx.session;

import std;

export import :privilege;
export import :user_session;
export import :rtkit;

export namespace lx::session {

/// How logind took a device away (org.freedesktop.login1.Session.PauseDevice, its type
/// string). Only `pause` asks for PauseDeviceComplete; logind sends it on seats without VTs,
/// where it waits for the acknowledgement before switching. `force` comes with a VT switch
/// (the device is already revoked) and `gone` with an unplug.
enum class device_pause : unsigned char { pause, force, gone };

/// Session signals, fired from inside `dispatch()` on the thread that owns the session (the
/// compositor's UI thread). A callback must only record what happened: sd-bus is in the
/// middle of processing, so no D-Bus call and no libinput work belongs in it.
struct session_listener {
    void (*device_paused)(unsigned major, unsigned minor, device_pause kind,
                          void* user) = nullptr;
    /// `fd` is borrowed and valid only during the call (sd-bus owns it). For DRM it is the
    /// device already held, with master restored; for evdev a *new* fd.
    void (*device_resumed)(unsigned major, unsigned minor, int fd, void* user) = nullptr;
    /// Session.Active changed (from PropertiesChanged, or the Get issued when the property
    /// was only invalidated).
    void (*active_changed)(bool active, void* user) = nullptr;
    /// Seat.SwitchTo failed. `error_name` is the D-Bus error name (static for the call).
    void (*vt_switch_failed)(const char* error_name, void* user) = nullptr;
    /// Session.Lock / Session.Unlock (`loginctl lock-session` / `unlock-session`, a lid
    /// policy; Unlock is polkit-gated by logind, org.freedesktop.login1.lock-sessions).
    void (*lock_requested)(void* user) = nullptr;
    void (*unlock_requested)(void* user) = nullptr;
    /// Manager.PrepareForSleep: true before suspend or hibernate, false after resume.
    void (*prepare_for_sleep)(bool start, void* user) = nullptr;
    void* user = nullptr;
};

// ── Finding the session (design-session-and-wm S.5) ─────────────────────────
//
// Run from a tty login (or run-desktop.sh) the compositor is inside its session's scope, and
// sd_pid_get_session finds it. Run as a systemd user unit it is not: user@.service belongs
// to no seat session. GNOME Shell, KWin and libseat find it the same way, in this order:
//   1. $XDG_SESSION_ID (lumen-session imports the login's into the user manager),
//   2. the session of this process (today's path, inside the scope),
//   3. logind's "display" session of the user (sd_uid_get_display),
//   4. the user's one active session on a seat.
// A candidate counts only when it belongs to this UID, sits on a seat and has class
// "user" (not "greeter", nor the "manager" session of user@.service). Steps 3 and 4 are
// for a process in no session at all (a user unit): one that belongs to a session without
// a seat — an SSH login, a test run from one — gets no session, exactly as before, and
// never takes over the console session it does not belong to.

/// What logind knows about one session, as `session_discovery` needs it.
struct session_facts {
    bool exists = false;
    unsigned uid = 0;
    bool has_seat = false;
    /// Class "user" (or "user-early"/"user-incomplete": a login still being set up).
    bool user_class = false;
    /// Class "manager": the session user@.service runs in (it counts as no session).
    bool manager_class = false;
    bool active = false;
};

/// Where `discover_session` found the session.
enum class session_found_by : unsigned char { none, environment, process, display, active_seat };

[[nodiscard]] constexpr const char* session_found_by_name(session_found_by b) {
    switch (b) {
    case session_found_by::none: return "none";
    case session_found_by::environment: return "XDG_SESSION_ID";
    case session_found_by::process: return "process";
    case session_found_by::display: return "display";
    case session_found_by::active_seat: return "active-seat";
    }
    return "?";
}

/// The lookups `discover_session` makes, injected so the order and the checks are testable
/// without logind. Each getter writes a NUL-terminated id into `out` and returns false when
/// there is none.
struct session_discovery {
    unsigned uid = 0;
    /// $XDG_SESSION_ID (null or empty: unset).
    const char* env_session_id = nullptr;
    bool (*pid_session)(void* user, char* out, std::size_t size) = nullptr;
    bool (*uid_display)(void* user, unsigned uid, char* out, std::size_t size) = nullptr;
    /// The user's sessions, `out[i]` each `id_size` bytes; returns how many were written.
    unsigned (*uid_sessions)(void* user, unsigned uid, char (*out)[64], unsigned cap) = nullptr;
    session_facts (*facts)(void* user, const char* id) = nullptr;
    void* user = nullptr;
};

struct session_choice {
    char id[64]{};
    session_found_by found_by = session_found_by::none;
    [[nodiscard]] bool found() const { return found_by != session_found_by::none; }
};

/// The session the compositor should take control of, by the order above. Pure.
[[nodiscard]] session_choice discover_session(const session_discovery& d);

/// The real lookups (sd-login), for `discover_session`. Without libsystemd every getter
/// reports nothing.
[[nodiscard]] session_discovery system_session_discovery();

/// The logind session this process runs in: session control (TakeControl), devices
/// (TakeDevice / ReleaseDevice / PauseDeviceComplete), the pause and resume signals, and
/// VT switching (Seat.SwitchTo). Owned by one thread.
class logind_session {
public:
    logind_session() = default;
    ~logind_session();

    logind_session(const logind_session&) = delete;
    logind_session& operator=(const logind_session&) = delete;
    /// Moves re-point the signal matches at the new object; a pending asynchronous call
    /// (SwitchTo, an Active Get) still names the old one, so move only before using them.
    logind_session(logind_session&& other) noexcept;
    logind_session& operator=(logind_session&& other) noexcept;

    /// Connects to logind, takes control of the session and reads Session.Seat, VTNr and
    /// Active and Seat.CanTTY (startup-only synchronous calls).
    [[nodiscard]] static lx::result<logind_session> open();

    [[nodiscard]] credentials peer_credentials(int socket_fd) const;
    /// Session.Active as last seen: read at `open`, then tracked from PropertiesChanged.
    [[nodiscard]] bool is_active() const;
    [[nodiscard]] const char* session_id() const;
    /// How `open` found the session (`session_found_by::none`: no session).
    [[nodiscard]] session_found_by found_by() const { return found_by_; }
    /// Session.Type as it was at `open` ("tty", "wayland", …; empty without a session).
    [[nodiscard]] const char* session_type() const { return type_; }

    /// Session.SetType(type), asynchronously, without waiting for the reply: what a display
    /// server started on a text login does so that `loginctl` and other components see a
    /// graphical session (only the controller may; logind restores the old type when control
    /// is released). A failure (an older logind without the method) is not reported.
    void set_type(const char* type);
    /// TakeControl succeeded: devices come from TakeDevice.
    [[nodiscard]] bool has_control() const { return has_control_; }
    /// Seat.CanTTY: the seat has virtual terminals (seat0 on a machine with a console).
    [[nodiscard]] bool can_switch_vt() const { return can_tty_; }
    /// Session.VTNr; 0 when the session has no VT.
    [[nodiscard]] unsigned vt_number() const { return vt_nr_; }

    /// Acquire a device via logind TakeDevice (major/minor). `inactive`, when given,
    /// receives logind's flag: the session is not in the foreground and the device is
    /// paused (DRM without master, evdev revoked) until ResumeDevice.
    [[nodiscard]] lx::result<lx::unique_fd> take_device(unsigned major, unsigned minor,
                                                        bool* inactive = nullptr);
    /// TakeDevice for a device node, with a direct-open fallback (which logind does not
    /// track, and `release_device` therefore leaves alone).
    [[nodiscard]] lx::result<lx::unique_fd> take_device_path(const char* path,
                                                             bool* inactive = nullptr);

    /// Installs the signal matches (PauseDevice, ResumeDevice, PropertiesChanged on the
    /// session path). Call on the object's final address. Synchronous, at startup.
    [[nodiscard]] lx::result<void> set_listener(session_listener listener);

    /// ReleaseDevice, sent without waiting for a reply — ordered before any later
    /// TakeDevice on the same connection, so a device can be taken again at once. Only for
    /// devices this object took (a fixed table of 64); anything else is ignored.
    void release_device(unsigned major, unsigned minor);
    /// `fstat` → character device → `release_device`.
    void release_device_fd(int fd);
    /// True while `major:minor` was taken through this object and not released.
    [[nodiscard]] bool holds_device(unsigned major, unsigned minor) const;
    /// PauseDeviceComplete for one device. Only for a pause of type `pause`; no reply.
    void complete_pause(unsigned major, unsigned minor);
    /// org.freedesktop.login1.Seat.SwitchTo(vt) on this session's seat, asynchronously
    /// (logind starts sending signals at once; a blocking call would stall on them). A
    /// failure reaches `session_listener::vt_switch_failed`. Refused now: vt 0 or > 63, no
    /// seat, or a seat without VTs.
    [[nodiscard]] lx::result<void> switch_vt(unsigned vt);

    /// Session.SetLockedHint(locked), asynchronously (what `loginctl` and desktop components
    /// read as "this session is locked"). No-op without a session.
    void set_locked_hint(bool locked);
    /// Manager.Inhibit("sleep", …, "delay"), asynchronously: sleep waits (logind's
    /// InhibitDelayMaxSec, 5 s by default) until `release_sleep_inhibitor` or this process
    /// goes — the time to lock the screen before the system sleeps. Idempotent.
    void take_sleep_inhibitor();
    void release_sleep_inhibitor();
    [[nodiscard]] bool holds_sleep_inhibitor() const { return sleep_fd_ >= 0; }

    /// Process pending sd-bus messages (PauseDevice / ResumeDevice / Active).
    void dispatch();
    [[nodiscard]] int bus_fd() const;
    /// When `dispatch` is next due, in ns from now: 0 when sd-bus already holds a message
    /// (read during a synchronous call, so the socket will not report it), -1 when only
    /// the socket becoming readable can bring work.
    [[nodiscard]] long long dispatch_timeout_ns() const;

private:
    void release();
    void track_taken(unsigned major, unsigned minor);
    /// The ReleaseDevice message itself, tracked or not.
    void send_release(unsigned major, unsigned minor);
    void fire_active(bool active);

    static constexpr unsigned k_max_taken = 64;
    struct taken_device {
        unsigned major = 0;
        unsigned minor = 0;
        bool used = false;
    };

    char session_id_buf_[64]{};
    const char* session_id_ = "";
    bool active_ = false;
    bool has_control_ = false;
    bool can_tty_ = false;
    unsigned vt_nr_ = 0;
    session_found_by found_by_ = session_found_by::none;
    char type_[16]{};
    taken_device taken_[k_max_taken]{};
    session_listener listener_{};
    /// The sleep delay inhibitor's fd (-1: none held).
    int sleep_fd_ = -1;
#if defined(LUMEN_HAS_LOGIND)
    sd_bus* bus_ = nullptr;
    char session_path_[256]{};
    char seat_path_[256]{};
    sd_bus_slot* pause_slot_ = nullptr;
    sd_bus_slot* resume_slot_ = nullptr;
    sd_bus_slot* props_slot_ = nullptr;
    sd_bus_slot* lock_slot_ = nullptr;
    sd_bus_slot* unlock_slot_ = nullptr;
    sd_bus_slot* sleep_slot_ = nullptr;
    /// The Inhibit call in flight (its reply carries the fd).
    sd_bus_slot* inhibit_call_ = nullptr;

    static int on_lock(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_unlock(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_prepare_for_sleep(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_inhibit_reply(sd_bus_message* m, void* user, sd_bus_error* err);

    static int on_pause_device(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_resume_device(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_properties_changed(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_active_reply(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_switch_reply(sd_bus_message* m, void* user, sd_bus_error* err);
#endif
};

} // namespace lx::session

module :private;

lx::session::logind_session::~logind_session() { release(); }

lx::session::logind_session::logind_session(logind_session&& other) noexcept {
    *this = std::move(other);
}

lx::session::logind_session& lx::session::logind_session::operator=(logind_session&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    std::memcpy(session_id_buf_, other.session_id_buf_, sizeof(session_id_buf_));
    session_id_ = session_id_buf_[0] ? session_id_buf_ : "";
    active_ = other.active_;
    has_control_ = other.has_control_;
    can_tty_ = other.can_tty_;
    vt_nr_ = other.vt_nr_;
    found_by_ = other.found_by_;
    std::memcpy(type_, other.type_, sizeof(type_));
    std::memcpy(taken_, other.taken_, sizeof(taken_));
    listener_ = other.listener_;
    sleep_fd_ = other.sleep_fd_;
    other.sleep_fd_ = -1;
#if defined(LUMEN_HAS_LOGIND)
    bus_ = other.bus_;
    other.bus_ = nullptr;
    std::memcpy(session_path_, other.session_path_, sizeof(session_path_));
    std::memcpy(seat_path_, other.seat_path_, sizeof(seat_path_));
    pause_slot_ = other.pause_slot_;
    resume_slot_ = other.resume_slot_;
    props_slot_ = other.props_slot_;
    lock_slot_ = other.lock_slot_;
    unlock_slot_ = other.unlock_slot_;
    sleep_slot_ = other.sleep_slot_;
    inhibit_call_ = other.inhibit_call_;
    other.pause_slot_ = other.resume_slot_ = other.props_slot_ = nullptr;
    other.lock_slot_ = other.unlock_slot_ = other.sleep_slot_ = other.inhibit_call_ = nullptr;
    // The matches carry the object's address as their userdata.
    for (sd_bus_slot* s :
         {pause_slot_, resume_slot_, props_slot_, lock_slot_, unlock_slot_, sleep_slot_,
          inhibit_call_})
        if (s)
            (void)sd_bus_slot_set_userdata(s, this);
    other.session_path_[0] = '\0';
    other.seat_path_[0] = '\0';
#endif
    other.session_id_buf_[0] = '\0';
    other.session_id_ = "";
    other.active_ = false;
    other.has_control_ = false;
    other.can_tty_ = false;
    other.vt_nr_ = 0;
    other.found_by_ = session_found_by::none;
    other.type_[0] = '\0';
    std::memset(other.taken_, 0, sizeof(other.taken_));
    other.listener_ = {};
    return *this;
}

void lx::session::logind_session::release() {
#if defined(LUMEN_HAS_LOGIND)
    // The matches go first: nothing may call back into an object being torn down.
    for (sd_bus_slot** s : {&pause_slot_, &resume_slot_, &props_slot_, &lock_slot_,
                            &unlock_slot_, &sleep_slot_, &inhibit_call_}) {
        if (*s)
            sd_bus_slot_unref(*s);
        *s = nullptr;
    }
    if (bus_ && has_control_ && session_path_[0] != '\0') {
        sd_bus_call_method(bus_, "org.freedesktop.login1", session_path_,
                           "org.freedesktop.login1.Session", "ReleaseControl", nullptr, nullptr,
                           nullptr);
    }
    if (bus_) {
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
    session_path_[0] = '\0';
    seat_path_[0] = '\0';
#endif
    has_control_ = false;
    active_ = false;
    can_tty_ = false;
    vt_nr_ = 0;
    found_by_ = session_found_by::none;
    type_[0] = '\0';
    std::memset(taken_, 0, sizeof(taken_));
    listener_ = {};
    session_id_buf_[0] = '\0';
    session_id_ = "";
    if (sleep_fd_ >= 0)
        ::close(sleep_fd_);
    sleep_fd_ = -1;
}

lx::session::session_choice lx::session::discover_session(const session_discovery& d) {
    session_choice c{};
    const auto acceptable = [&d](const char* id) {
        if (!id || !*id || !d.facts)
            return false;
        const session_facts f = d.facts(d.user, id);
        return f.exists && f.uid == d.uid && f.has_seat && f.user_class;
    };
    const auto take = [&c](const char* id, session_found_by by) {
        std::snprintf(c.id, sizeof(c.id), "%s", id);
        c.found_by = by;
        return c;
    };
    if (d.env_session_id && *d.env_session_id && acceptable(d.env_session_id))
        return take(d.env_session_id, session_found_by::environment);
    char id[64]{};
    if (d.pid_session && d.pid_session(d.user, id, sizeof(id))) {
        if (acceptable(id))
            return take(id, session_found_by::process);
        // In a session that has no seat (SSH): that is where this process runs; another
        // session's display is not its to take.
        if (!d.facts || !d.facts(d.user, id).manager_class)
            return c;
    }
    id[0] = '\0';
    if (d.uid_display && d.uid_display(d.user, d.uid, id, sizeof(id)) && acceptable(id))
        return take(id, session_found_by::display);
    if (d.uid_sessions) {
        // The one active session on a seat; two (a second seat) is ambiguous: none.
        char ids[8][64]{};
        const unsigned n = d.uid_sessions(d.user, d.uid, ids, 8);
        const char* only = nullptr;
        unsigned matches = 0;
        for (unsigned i = 0; i < n && i < 8; ++i) {
            if (!acceptable(ids[i]) || !d.facts(d.user, ids[i]).active)
                continue;
            only = ids[i];
            ++matches;
        }
        if (matches == 1)
            return take(only, session_found_by::active_seat);
    }
    return c;
}

lx::session::session_discovery lx::session::system_session_discovery() {
    session_discovery d{};
#if defined(LUMEN_HAS_LOGIND)
    d.uid = static_cast<unsigned>(::getuid());
    d.env_session_id = std::getenv("XDG_SESSION_ID");
    d.pid_session = [](void*, char* out, std::size_t size) {
        char* sid = nullptr;
        if (sd_pid_get_session(0, &sid) < 0 || !sid)
            return false;
        std::snprintf(out, size, "%s", sid);
        free(sid);
        return true;
    };
    d.uid_display = [](void*, unsigned uid, char* out, std::size_t size) {
        char* sid = nullptr;
        if (sd_uid_get_display(static_cast<uid_t>(uid), &sid) < 0 || !sid)
            return false;
        std::snprintf(out, size, "%s", sid);
        free(sid);
        return true;
    };
    d.uid_sessions = [](void*, unsigned uid, char (*out)[64], unsigned cap) {
        char** list = nullptr;
        const int n = sd_uid_get_sessions(static_cast<uid_t>(uid), 0, &list);
        unsigned written = 0;
        for (int i = 0; i < n && list && list[i]; ++i) {
            if (written < cap)
                std::snprintf(out[written++], 64, "%s", list[i]);
            free(list[i]);
        }
        free(list);
        return written;
    };
    d.facts = [](void*, const char* id) {
        session_facts f{};
        uid_t uid = 0;
        if (sd_session_get_uid(id, &uid) < 0)
            return f;
        f.exists = true;
        f.uid = static_cast<unsigned>(uid);
        char* seat = nullptr;
        f.has_seat = sd_session_get_seat(id, &seat) >= 0 && seat && *seat;
        free(seat);
        char* cls = nullptr;
        if (sd_session_get_class(id, &cls) >= 0 && cls) {
            f.user_class = std::strncmp(cls, "user", 4) == 0;
            f.manager_class = std::strncmp(cls, "manager", 7) == 0;
        }
        free(cls);
        f.active = sd_session_is_active(id) > 0;
        return f;
    };
#endif
    return d;
}

void lx::session::logind_session::set_type(const char* type) {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || !has_control_ || session_path_[0] == '\0' || !type)
        return;
    (void)sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.login1", session_path_,
                                   "org.freedesktop.login1.Session", "SetType", nullptr, nullptr,
                                   "s", type);
    (void)sd_bus_flush(bus_);
#else
    (void)type;
#endif
}

lx::result<lx::session::logind_session> lx::session::logind_session::open() {
#if defined(LUMEN_HAS_LOGIND)
    logind_session s{};
    const session_choice choice = discover_session(system_session_discovery());
    if (!choice.found()) {
        // Not in a logind session (e.g. some CI) — return a soft inactive session.
        s.active_ = false;
        s.session_id_ = "";
        return s;
    }
    std::snprintf(s.session_id_buf_, sizeof(s.session_id_buf_), "%s", choice.id);
    s.session_id_ = s.session_id_buf_;
    s.found_by_ = choice.found_by;
    {
        char* type = nullptr;
        if (sd_session_get_type(s.session_id_, &type) >= 0 && type)
            std::snprintf(s.type_, sizeof(s.type_), "%s", type);
        free(type);
    }

    if (sd_bus_default_system(&s.bus_) < 0 || !s.bus_) {
        return lx::make_error(lx::io_err::not_found, "sd_bus_default_system failed");
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_call_method(s.bus_, "org.freedesktop.login1", "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager", "GetSession", &err, &reply,
                                     "s", s.session_id_);
    if (r < 0) {
        sd_bus_error_free(&err);
        return lx::make_error(lx::io_err::not_found, "logind GetSession failed");
    }
    const char* path = nullptr;
    if (sd_bus_message_read(reply, "o", &path) < 0 || !path) {
        sd_bus_message_unref(reply);
        return lx::make_error(lx::io_err::not_found, "logind GetSession bad reply");
    }
    std::snprintf(s.session_path_, sizeof(s.session_path_), "%s", path);
    sd_bus_message_unref(reply);

    // Session.Seat (so), VTNr (u), Active (b); Seat.CanTTY (b). One call each, at startup.
    {
        sd_bus_error perr = SD_BUS_ERROR_NULL;
        sd_bus_message* seat_reply = nullptr;
        if (sd_bus_get_property(s.bus_, "org.freedesktop.login1", s.session_path_,
                                "org.freedesktop.login1.Session", "Seat", &perr, &seat_reply,
                                "(so)") >= 0 &&
            seat_reply) {
            const char* seat_id = nullptr;
            const char* seat_path = nullptr;
            if (sd_bus_message_read(seat_reply, "(so)", &seat_id, &seat_path) >= 0 && seat_path)
                std::snprintf(s.seat_path_, sizeof(s.seat_path_), "%s", seat_path);
            sd_bus_message_unref(seat_reply);
        }
        sd_bus_error_free(&perr);

        std::uint32_t vt = 0;
        perr = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_trivial(s.bus_, "org.freedesktop.login1", s.session_path_,
                                        "org.freedesktop.login1.Session", "VTNr", &perr, 'u',
                                        &vt) >= 0)
            s.vt_nr_ = vt;
        sd_bus_error_free(&perr);

        int active = 0;
        perr = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_trivial(s.bus_, "org.freedesktop.login1", s.session_path_,
                                        "org.freedesktop.login1.Session", "Active", &perr, 'b',
                                        &active) >= 0)
            s.active_ = active != 0;
        sd_bus_error_free(&perr);

        if (s.seat_path_[0] != '\0') {
            int can_tty = 0;
            perr = SD_BUS_ERROR_NULL;
            if (sd_bus_get_property_trivial(s.bus_, "org.freedesktop.login1", s.seat_path_,
                                            "org.freedesktop.login1.Seat", "CanTTY", &perr, 'b',
                                            &can_tty) >= 0)
                s.can_tty_ = can_tty != 0;
            sd_bus_error_free(&perr);
        }
    }

    sd_bus_error take_err = SD_BUS_ERROR_NULL;
    if (sd_bus_call_method(s.bus_, "org.freedesktop.login1", s.session_path_,
                           "org.freedesktop.login1.Session", "TakeControl", &take_err, nullptr, "b",
                           0) < 0) {
        sd_bus_error_free(&take_err);
        // Continue without control — direct open fallback still works for render nodes.
        s.has_control_ = false;
        return s;
    }
    s.has_control_ = true;
    return s;
#else
    return lx::not_implemented("lx::session::logind_session::open");
#endif
}

lx::session::credentials lx::session::logind_session::peer_credentials(int socket_fd) const {
    credentials out{};
    if (socket_fd < 0)
        return out;
    ucred peer{};
    socklen_t len = sizeof(peer);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0) {
        out.pid = static_cast<unsigned>(peer.pid);
        out.uid = static_cast<unsigned>(peer.uid);
        out.gid = static_cast<unsigned>(peer.gid);
    }
    return out;
}

bool lx::session::logind_session::is_active() const { return active_; }
const char* lx::session::logind_session::session_id() const { return session_id_; }

void lx::session::logind_session::track_taken(unsigned major, unsigned minor) {
    taken_device* free_slot = nullptr;
    for (auto& t : taken_) {
        if (t.used && t.major == major && t.minor == minor)
            return;
        if (!t.used && !free_slot)
            free_slot = &t;
    }
    // Past 64 devices the extra one is not tracked: it is never released explicitly, and
    // logind releases it with the session. No seat has that many input devices.
    if (free_slot)
        *free_slot = {major, minor, true};
}

bool lx::session::logind_session::holds_device(unsigned major, unsigned minor) const {
    for (const auto& t : taken_)
        if (t.used && t.major == major && t.minor == minor)
            return true;
    return false;
}

lx::result<lx::unique_fd> lx::session::logind_session::take_device(unsigned major, unsigned minor,
                                                                    bool* inactive) {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_ && has_control_ && session_path_[0] != '\0') {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call_method(bus_, "org.freedesktop.login1", session_path_,
                                         "org.freedesktop.login1.Session", "TakeDevice", &err,
                                         &reply, "uu", major, minor);
        if (r >= 0 && reply) {
            int fd = -1;
            int paused = 0;
            if (sd_bus_message_read(reply, "hb", &fd, &paused) >= 0 && fd >= 0) {
                // sd-bus owns the fd in the message; dup for ownership.
                const int owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
                sd_bus_message_unref(reply);
                if (owned >= 0) {
                    track_taken(major, minor);
                    if (inactive)
                        *inactive = paused != 0;
                    return lx::unique_fd{owned};
                }
                // logind holds it now and we do not (EMFILE): give it back. It was never
                // tracked, so not through `release_device`, which would skip it — and every
                // later TakeDevice of it would fail with "Device already taken".
                send_release(major, minor);
            } else {
                sd_bus_message_unref(reply);
            }
        } else {
            sd_bus_error_free(&err);
        }
    }
#else
    (void)major;
    (void)minor;
#endif
    (void)inactive;
    return lx::make_error(lx::io_err::permission_denied, "TakeDevice failed");
}

lx::result<lx::unique_fd> lx::session::logind_session::take_device_path(const char* path,
                                                                         bool* inactive) {
    if (!path || path[0] == '\0') {
        return lx::invalid_argument("null device path");
    }
    struct stat st{};
    if (::stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
        auto taken = take_device(major(st.st_rdev), minor(st.st_rdev), inactive);
        if (taken)
            return taken;
    }
    // Direct-open fallback (video group / root / render node world-writable).
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        if (inactive)
            *inactive = false;
        return lx::unique_fd{fd};
    }
    return lx::make_error(lx::io_err::permission_denied, "failed to open device path");
}

void lx::session::logind_session::release_device(unsigned major, unsigned minor) {
    bool tracked = false;
    for (auto& t : taken_) {
        if (t.used && t.major == major && t.minor == minor) {
            t = {};
            tracked = true;
        }
    }
    if (tracked)
        send_release(major, minor);
}

void lx::session::logind_session::send_release(unsigned major, unsigned minor) {
#if !defined(LUMEN_HAS_LOGIND)
    (void)major;
    (void)minor;
#else
    if (!bus_ || session_path_[0] == '\0')
        return;
    // No callback: sent with NO_REPLY_EXPECTED. The flush writes it now (a local socket, a
    // few hundred bytes), so it is ahead of the TakeDevice a resume issues.
    (void)sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.login1", session_path_,
                                   "org.freedesktop.login1.Session", "ReleaseDevice", nullptr,
                                   nullptr, "uu", major, minor);
    (void)sd_bus_flush(bus_);
#endif
}

void lx::session::logind_session::release_device_fd(int fd) {
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
        return;
    release_device(major(st.st_rdev), minor(st.st_rdev));
}

void lx::session::logind_session::complete_pause(unsigned major, unsigned minor) {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || !has_control_ || session_path_[0] == '\0')
        return;
    (void)sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.login1", session_path_,
                                   "org.freedesktop.login1.Session", "PauseDeviceComplete",
                                   nullptr, nullptr, "uu", major, minor);
    (void)sd_bus_flush(bus_);
#else
    (void)major;
    (void)minor;
#endif
}

lx::result<void> lx::session::logind_session::switch_vt(unsigned vt) {
    if (vt == 0 || vt > 63)
        return lx::invalid_argument("VT number out of range");
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || seat_path_[0] == '\0')
        return lx::make_error(lx::io_err::not_found, "no logind seat to switch VTs on");
    if (!can_tty_)
        return lx::make_error(lx::io_err::permission_denied, "seat has no virtual terminals");
    const int r = sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.login1", seat_path_,
                                           "org.freedesktop.login1.Seat", "SwitchTo",
                                           &logind_session::on_switch_reply, this, "u", vt);
    if (r < 0)
        return lx::make_error(lx::io_err::broken_pipe, "Seat.SwitchTo could not be sent");
    (void)sd_bus_flush(bus_);
    return {};
#else
    return lx::not_implemented("lx::session::logind_session::switch_vt");
#endif
}

void lx::session::logind_session::fire_active(bool active) {
    if (active == active_)
        return;
    active_ = active;
    if (listener_.active_changed)
        listener_.active_changed(active, listener_.user);
}

#if defined(LUMEN_HAS_LOGIND)

int lx::session::logind_session::on_pause_device(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    std::uint32_t maj = 0;
    std::uint32_t min = 0;
    const char* type = nullptr;
    if (!self || sd_bus_message_read(m, "uus", &maj, &min, &type) < 0)
        return 0;
    // logind_session_device.c: TRY_PAUSE → "pause", PAUSE → "force", RELEASE → "gone".
    // Anything newer is treated as the safe case: already taken, nothing to acknowledge.
    device_pause kind = device_pause::force;
    if (type && std::strcmp(type, "pause") == 0)
        kind = device_pause::pause;
    else if (type && std::strcmp(type, "gone") == 0)
        kind = device_pause::gone;
    if (self->listener_.device_paused)
        self->listener_.device_paused(maj, min, kind, self->listener_.user);
    return 0;
}

int lx::session::logind_session::on_resume_device(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    std::uint32_t maj = 0;
    std::uint32_t min = 0;
    int fd = -1;
    if (!self || sd_bus_message_read(m, "uuh", &maj, &min, &fd) < 0)
        return 0;
    if (self->listener_.device_resumed)
        self->listener_.device_resumed(maj, min, fd, self->listener_.user);
    return 0;
}

int lx::session::logind_session::on_active_reply(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    if (!self || sd_bus_message_is_method_error(m, nullptr))
        return 0;
    int active = 0;
    if (sd_bus_message_read(m, "v", "b", &active) >= 0)
        self->fire_active(active != 0);
    return 0;
}

int lx::session::logind_session::on_properties_changed(sd_bus_message* m, void* user,
                                                       sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    const char* iface = nullptr;
    if (!self || sd_bus_message_read(m, "s", &iface) < 0 || !iface ||
        std::strcmp(iface, "org.freedesktop.login1.Session") != 0)
        return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
        return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* name = nullptr;
        if (sd_bus_message_read(m, "s", &name) < 0)
            return 0;
        if (name && std::strcmp(name, "Active") == 0) {
            int active = 0;
            if (sd_bus_message_read(m, "v", "b", &active) < 0)
                return 0;
            self->fire_active(active != 0);
        } else if (sd_bus_message_skip(m, "v") < 0) {
            return 0;
        }
        if (sd_bus_message_exit_container(m) < 0)
            return 0;
    }
    if (sd_bus_message_exit_container(m) < 0)
        return 0;
    // Invalidated without a value: ask for it.
    if (sd_bus_message_enter_container(m, 'a', "s") < 0)
        return 0;
    const char* name = nullptr;
    while (sd_bus_message_read(m, "s", &name) > 0) {
        if (name && std::strcmp(name, "Active") == 0) {
            (void)sd_bus_call_method_async(self->bus_, nullptr, "org.freedesktop.login1",
                                           self->session_path_,
                                           "org.freedesktop.DBus.Properties", "Get",
                                           &logind_session::on_active_reply, self, "ss",
                                           "org.freedesktop.login1.Session", "Active");
        }
    }
    (void)sd_bus_message_exit_container(m);
    return 0;
}

int lx::session::logind_session::on_lock(sd_bus_message*, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    if (self && self->listener_.lock_requested)
        self->listener_.lock_requested(self->listener_.user);
    return 0;
}

int lx::session::logind_session::on_unlock(sd_bus_message*, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    if (self && self->listener_.unlock_requested)
        self->listener_.unlock_requested(self->listener_.user);
    return 0;
}

int lx::session::logind_session::on_prepare_for_sleep(sd_bus_message* m, void* user,
                                                      sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    int start = 0;
    if (!self || sd_bus_message_read(m, "b", &start) < 0)
        return 0;
    if (self->listener_.prepare_for_sleep)
        self->listener_.prepare_for_sleep(start != 0, self->listener_.user);
    return 0;
}

int lx::session::logind_session::on_inhibit_reply(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    if (!self)
        return 0;
    if (self->inhibit_call_) {
        sd_bus_slot_unref(self->inhibit_call_);
        self->inhibit_call_ = nullptr;
    }
    if (sd_bus_message_is_method_error(m, nullptr))
        return 0; // no inhibitor: sleep simply does not wait (logged by nobody; not an error)
    int fd = -1;
    if (sd_bus_message_read(m, "h", &fd) < 0 || fd < 0)
        return 0;
    // sd-bus owns the fd in the message.
    const int owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0)
        return 0;
    if (self->sleep_fd_ >= 0)
        ::close(self->sleep_fd_);
    self->sleep_fd_ = owned;
    return 0;
}

int lx::session::logind_session::on_switch_reply(sd_bus_message* m, void* user, sd_bus_error*) {
    auto* self = static_cast<logind_session*>(user);
    if (!self || !sd_bus_message_is_method_error(m, nullptr))
        return 0;
    const sd_bus_error* e = sd_bus_message_get_error(m);
    if (self->listener_.vt_switch_failed)
        self->listener_.vt_switch_failed(e && e->name ? e->name : "unknown",
                                         self->listener_.user);
    return 0;
}

#endif // LUMEN_HAS_LOGIND

lx::result<void> lx::session::logind_session::set_listener(session_listener listener) {
    listener_ = listener;
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || session_path_[0] == '\0')
        return lx::make_error(lx::io_err::not_found, "no logind session to listen on");
    if (pause_slot_)
        return {}; // already installed; the listener above replaced the callbacks
    int r = sd_bus_match_signal(bus_, &pause_slot_, "org.freedesktop.login1", session_path_,
                                "org.freedesktop.login1.Session", "PauseDevice",
                                &logind_session::on_pause_device, this);
    if (r >= 0)
        r = sd_bus_match_signal(bus_, &resume_slot_, "org.freedesktop.login1", session_path_,
                                "org.freedesktop.login1.Session", "ResumeDevice",
                                &logind_session::on_resume_device, this);
    if (r >= 0)
        r = sd_bus_match_signal(bus_, &props_slot_, "org.freedesktop.login1", session_path_,
                                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                &logind_session::on_properties_changed, this);
    if (r >= 0)
        r = sd_bus_match_signal(bus_, &lock_slot_, "org.freedesktop.login1", session_path_,
                                "org.freedesktop.login1.Session", "Lock",
                                &logind_session::on_lock, this);
    if (r >= 0)
        r = sd_bus_match_signal(bus_, &unlock_slot_, "org.freedesktop.login1", session_path_,
                                "org.freedesktop.login1.Session", "Unlock",
                                &logind_session::on_unlock, this);
    if (r >= 0)
        r = sd_bus_match_signal(bus_, &sleep_slot_, "org.freedesktop.login1",
                                "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                "PrepareForSleep", &logind_session::on_prepare_for_sleep, this);
    if (r < 0)
        return lx::make_error(lx::io_err::broken_pipe, "logind signal match failed");
    return {};
#else
    return lx::not_implemented("lx::session::logind_session::set_listener");
#endif
}

void lx::session::logind_session::set_locked_hint(bool locked) {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || session_path_[0] == '\0')
        return;
    (void)sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.login1", session_path_,
                                   "org.freedesktop.login1.Session", "SetLockedHint", nullptr,
                                   nullptr, "b", locked ? 1 : 0);
    (void)sd_bus_flush(bus_);
#else
    (void)locked;
#endif
}

void lx::session::logind_session::take_sleep_inhibitor() {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_ || sleep_fd_ >= 0 || inhibit_call_)
        return;
    (void)sd_bus_call_method_async(bus_, &inhibit_call_, "org.freedesktop.login1",
                                   "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                   "Inhibit", &logind_session::on_inhibit_reply, this, "ssss",
                                   "sleep", "lumen-compositor", "lock the screen before sleeping",
                                   "delay");
    (void)sd_bus_flush(bus_);
#endif
}

void lx::session::logind_session::release_sleep_inhibitor() {
    if (sleep_fd_ >= 0)
        ::close(sleep_fd_);
    sleep_fd_ = -1;
}

void lx::session::logind_session::dispatch() {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_)
        return;
    while (sd_bus_process(bus_, nullptr) > 0) {
    }
#endif
}

long long lx::session::logind_session::dispatch_timeout_ns() const {
#if defined(LUMEN_HAS_LOGIND)
    std::uint64_t at_us = 0;
    if (!bus_ || sd_bus_get_timeout(bus_, &at_us) <= 0 || at_us == UINT64_MAX)
        return -1;
    if (at_us == 0)
        return 0;
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long now_us = static_cast<long long>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
    const long long left_us = static_cast<long long>(at_us) - now_us;
    return left_us > 0 ? left_us * 1000 : 0;
#else
    return -1;
#endif
}

int lx::session::logind_session::bus_fd() const {
#if defined(LUMEN_HAS_LOGIND)
    return bus_ ? sd_bus_get_fd(bus_) : -1;
#else
    return -1;
#endif
}
