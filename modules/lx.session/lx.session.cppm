module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

import lx.foundation;
import lx.runtime;

export module lx.session;

export import :privilege;

export namespace lx::session {

class logind_session {
public:
    logind_session() = default;
    ~logind_session();

    logind_session(const logind_session&) = delete;
    logind_session& operator=(const logind_session&) = delete;
    logind_session(logind_session&& other) noexcept;
    logind_session& operator=(logind_session&& other) noexcept;

    [[nodiscard]] static lx::result<logind_session> open();

    [[nodiscard]] credentials peer_credentials(int socket_fd) const;
    [[nodiscard]] bool is_active() const;
    [[nodiscard]] const char* session_id() const;

    /// Acquire a device via logind TakeDevice (major/minor). Falls back to path open
    /// when LUMEN_HAS_LOGIND is unavailable.
    [[nodiscard]] lx::result<lx::unique_fd> take_device(unsigned major, unsigned minor);
    [[nodiscard]] lx::result<lx::unique_fd> take_device_path(const char* path);

    void lock();
    void unlock();
    [[nodiscard]] lx::result<void> pause_devices();
    [[nodiscard]] lx::result<void> resume_devices();

    /// Process pending sd-bus messages (PauseDevice / ResumeDevice / Active).
    void dispatch();
    [[nodiscard]] int bus_fd() const;

private:
    void release();

    char session_id_buf_[64]{};
    const char* session_id_ = "";
    bool active_ = false;
    bool has_control_ = false;
#if defined(LUMEN_HAS_LOGIND)
    sd_bus* bus_ = nullptr;
    char session_path_[256]{};
#endif
};

} // namespace lx::session

module :private;

lx::session::logind_session::~logind_session() { release(); }

lx::session::logind_session::logind_session(logind_session&& other) noexcept {
    *this = static_cast<logind_session&&>(other);
}

lx::session::logind_session& lx::session::logind_session::operator=(logind_session&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    std::memcpy(session_id_buf_, other.session_id_buf_, sizeof(session_id_buf_));
    session_id_ = session_id_buf_[0] ? session_id_buf_ : "";
    active_ = other.active_;
    has_control_ = other.has_control_;
#if defined(LUMEN_HAS_LOGIND)
    bus_ = other.bus_;
    other.bus_ = nullptr;
    std::memcpy(session_path_, other.session_path_, sizeof(session_path_));
#endif
    other.session_id_buf_[0] = '\0';
    other.session_id_ = "";
    other.active_ = false;
    other.has_control_ = false;
    return *this;
}

void lx::session::logind_session::release() {
#if defined(LUMEN_HAS_LOGIND)
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
#endif
    has_control_ = false;
    active_ = false;
    session_id_buf_[0] = '\0';
    session_id_ = "";
}

lx::result<lx::session::logind_session> lx::session::logind_session::open() {
#if defined(LUMEN_HAS_LOGIND)
    logind_session s{};
    char* sid = nullptr;
    if (sd_pid_get_session(0, &sid) < 0 || !sid) {
        // Not in a logind session (e.g. some CI) — return a soft inactive session.
        s.active_ = false;
        s.session_id_ = "";
        return s;
    }
    std::snprintf(s.session_id_buf_, sizeof(s.session_id_buf_), "%s", sid);
    s.session_id_ = s.session_id_buf_;
    free(sid);

    if (sd_bus_default_system(&s.bus_) < 0 || !s.bus_) {
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::not_found),
                              "sd_bus_default_system failed");
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_call_method(s.bus_, "org.freedesktop.login1", "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager", "GetSession", &err, &reply,
                                     "s", s.session_id_);
    if (r < 0) {
        sd_bus_error_free(&err);
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::not_found),
                              "logind GetSession failed");
    }
    const char* path = nullptr;
    if (sd_bus_message_read(reply, "o", &path) < 0 || !path) {
        sd_bus_message_unref(reply);
        return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::not_found),
                              "logind GetSession bad reply");
    }
    std::snprintf(s.session_path_, sizeof(s.session_path_), "%s", path);
    sd_bus_message_unref(reply);

    sd_bus_error take_err = SD_BUS_ERROR_NULL;
    if (sd_bus_call_method(s.bus_, "org.freedesktop.login1", s.session_path_,
                           "org.freedesktop.login1.Session", "TakeControl", &take_err, nullptr, "b",
                           0) < 0) {
        sd_bus_error_free(&take_err);
        // Continue without control — direct open fallback still works for render nodes.
        s.has_control_ = false;
        s.active_ = true;
        return s;
    }
    s.has_control_ = true;
    s.active_ = true;
    return s;
#else
    return lx::not_implemented("lx::session::logind_session::open");
#endif
}

lx::session::credentials lx::session::logind_session::peer_credentials(int socket_fd) const {
    credentials out{};
#if !defined(_WIN32)
    if (socket_fd < 0)
        return out;
    ucred peer{};
    socklen_t len = sizeof(peer);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0) {
        out.pid = static_cast<unsigned>(peer.pid);
        out.uid = static_cast<unsigned>(peer.uid);
        out.gid = static_cast<unsigned>(peer.gid);
    }
#else
    (void)socket_fd;
#endif
    return out;
}

bool lx::session::logind_session::is_active() const { return active_; }
const char* lx::session::logind_session::session_id() const { return session_id_; }

lx::result<lx::unique_fd> lx::session::logind_session::take_device(unsigned major, unsigned minor) {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_ && has_control_ && session_path_[0] != '\0') {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call_method(bus_, "org.freedesktop.login1", session_path_,
                                         "org.freedesktop.login1.Session", "TakeDevice", &err,
                                         &reply, "uu", major, minor);
        if (r >= 0 && reply) {
            int fd = -1;
            int inactive = 0;
            if (sd_bus_message_read(reply, "hb", &fd, &inactive) >= 0 && fd >= 0) {
                // sd-bus duplicates the fd into the message; dup again for ownership.
                const int owned = ::dup(fd);
                sd_bus_message_unref(reply);
                if (owned >= 0)
                    return lx::unique_fd{owned};
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
    return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::permission_denied),
                          "TakeDevice failed");
}

lx::result<lx::unique_fd> lx::session::logind_session::take_device_path(const char* path) {
#if !defined(_WIN32)
    if (!path || path[0] == '\0') {
        return lx::make_error(lx::error_domain::invalid_argument, 0, "null device path");
    }
    struct stat st{};
    if (::stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
        auto taken = take_device(major(st.st_rdev), minor(st.st_rdev));
        if (taken)
            return taken;
    }
    // Direct-open fallback (video group / root / render node world-writable).
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0)
        return lx::unique_fd{fd};
    return lx::make_error(lx::error_domain::io, static_cast<int>(lx::io_err::permission_denied),
                          "failed to open device path");
#else
    (void)path;
    return lx::not_implemented("lx::session::logind_session::take_device_path");
#endif
}

void lx::session::logind_session::lock() { active_ = false; }
void lx::session::logind_session::unlock() { active_ = true; }

lx::result<void> lx::session::logind_session::pause_devices() {
#if defined(LUMEN_HAS_LOGIND)
    active_ = false;
    return {};
#else
    return lx::not_implemented("lx::session::logind_session::pause_devices");
#endif
}

lx::result<void> lx::session::logind_session::resume_devices() {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_ && has_control_ && session_path_[0] != '\0') {
        sd_bus_call_method(bus_, "org.freedesktop.login1", session_path_,
                           "org.freedesktop.login1.Session", "PauseDeviceComplete", nullptr, nullptr,
                           "uu", 0u, 0u);
    }
    active_ = true;
    return {};
#else
    return lx::not_implemented("lx::session::logind_session::resume_devices");
#endif
}

void lx::session::logind_session::dispatch() {
#if defined(LUMEN_HAS_LOGIND)
    if (!bus_)
        return;
    while (sd_bus_process(bus_, nullptr) > 0) {
    }
#endif
}

int lx::session::logind_session::bus_fd() const {
#if defined(LUMEN_HAS_LOGIND)
    return bus_ ? sd_bus_get_fd(bus_) : -1;
#else
    return -1;
#endif
}
