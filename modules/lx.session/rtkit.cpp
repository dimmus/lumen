module;

#include <errno.h>
#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

module lx.session;

import std;
import lx.runtime;

namespace lx::session {

namespace {

[[maybe_unused]] constexpr const char* k_service = "org.freedesktop.RealtimeKit1";
[[maybe_unused]] constexpr const char* k_path = "/org/freedesktop/RealtimeKit1";

#if defined(LUMEN_HAS_LOGIND)
/// An errno for a failed call: ENOENT when rtkit is not there to answer (not installed, not
/// activatable), else what sd-bus maps the D-Bus error to (AccessDenied → EACCES, …).
int call_errno(int r, const sd_bus_error& err) {
    if (sd_bus_error_has_names(&err, SD_BUS_ERROR_SERVICE_UNKNOWN, SD_BUS_ERROR_NAME_HAS_NO_OWNER,
                               SD_BUS_ERROR_UNKNOWN_OBJECT, SD_BUS_ERROR_UNKNOWN_METHOD))
        return ENOENT;
    const int e = sd_bus_error_is_set(&err) ? sd_bus_error_get_errno(&err) : -r;
    return e > 0 ? e : EIO;
}
#endif

} // namespace

rtkit_client::~rtkit_client() {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_)
        sd_bus_flush_close_unref(static_cast<sd_bus*>(bus_));
#endif
    bus_ = nullptr;
}

int rtkit_client::connect() {
#if defined(LUMEN_HAS_LOGIND)
    if (connect_error_ >= 0)
        return connect_error_;
    sd_bus* bus = nullptr;
    const int r = sd_bus_open_system_with_description(&bus, "lumen-rtkit");
    if (r < 0 || !bus) {
        connect_error_ = ENOENT;
        return connect_error_;
    }
    // rtkit is bus-activated: the first call may start it. Never the 25 s default.
    (void)sd_bus_set_method_call_timeout(bus, 1'000'000);
    bus_ = bus;
    connect_error_ = 0;
    return 0;
#else
    return ENOENT;
#endif
}

int rtkit_client::limits(runtime::sched_broker_limits& out) {
#if defined(LUMEN_HAS_LOGIND)
    if (limits_error_ < 0) {
        limits_error_ = connect();
        auto* bus = static_cast<sd_bus*>(bus_);
        auto get = [&](const char* name, char type, void* value) {
            if (limits_error_ != 0)
                return;
            sd_bus_error err = SD_BUS_ERROR_NULL;
            const int r = sd_bus_get_property_trivial(bus, k_service, k_path, k_service, name,
                                                      &err, type, value);
            if (r < 0)
                limits_error_ = call_errno(r, err);
            sd_bus_error_free(&err);
        };
        std::int32_t prio = 0;
        std::int32_t nice = 0;
        std::int64_t rttime = 0;
        get("MaxRealtimePriority", 'i', &prio);
        get("MinNiceLevel", 'i', &nice);
        get("RTTimeUSecMax", 'x', &rttime);
        if (limits_error_ == 0) {
            limits_.max_rt_priority = prio;
            limits_.min_nice = nice;
            limits_.rttime_max_us = rttime > 0 ? static_cast<unsigned long long>(rttime) : 0;
        }
    }
    out = limits_;
    return limits_error_;
#else
    (void)out;
    return ENOENT;
#endif
}

int rtkit_client::make_realtime(int tid, int priority) {
#if defined(LUMEN_HAS_LOGIND)
    if (const int e = connect(); e != 0)
        return e;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const int r = sd_bus_call_method(static_cast<sd_bus*>(bus_), k_service, k_path, k_service,
                                     "MakeThreadRealtimeWithPID", &err, nullptr, "ttu",
                                     static_cast<std::uint64_t>(::getpid()),
                                     static_cast<std::uint64_t>(tid),
                                     static_cast<std::uint32_t>(priority));
    const int e = r < 0 ? call_errno(r, err) : 0;
    sd_bus_error_free(&err);
    return e;
#else
    (void)tid;
    (void)priority;
    return ENOENT;
#endif
}

int rtkit_client::make_high_priority(int tid, int nice) {
#if defined(LUMEN_HAS_LOGIND)
    if (const int e = connect(); e != 0)
        return e;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const int r = sd_bus_call_method(static_cast<sd_bus*>(bus_), k_service, k_path, k_service,
                                     "MakeThreadHighPriorityWithPID", &err, nullptr, "tti",
                                     static_cast<std::uint64_t>(::getpid()),
                                     static_cast<std::uint64_t>(tid),
                                     static_cast<std::int32_t>(nice));
    const int e = r < 0 ? call_errno(r, err) : 0;
    sd_bus_error_free(&err);
    return e;
#else
    (void)tid;
    (void)nice;
    return ENOENT;
#endif
}

runtime::sched_broker rtkit_client::broker() {
    runtime::sched_broker b{};
#if defined(LUMEN_HAS_LOGIND)
    b.ctx = this;
    b.limits = [](void* c, runtime::sched_broker_limits& out) {
        return static_cast<rtkit_client*>(c)->limits(out);
    };
    b.make_realtime = [](void* c, int tid, int priority) {
        return static_cast<rtkit_client*>(c)->make_realtime(tid, priority);
    };
    b.make_high_priority = [](void* c, int tid, int nice) {
        return static_cast<rtkit_client*>(c)->make_high_priority(tid, nice);
    };
#endif
    return b;
}

} // namespace lx::session
