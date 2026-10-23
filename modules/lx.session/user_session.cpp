module;

#include <stdlib.h>
#include <time.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#endif

module lx.session;

import std;
import lx.foundation;

namespace lx::session {

// ── sd_notify ────────────────────────────────────────────────────────────────

bool service_notifier::listening() {
    const char* s = std::getenv("NOTIFY_SOCKET");
    return s && *s;
}

namespace {
#if defined(LUMEN_HAS_LOGIND)
void notify_with_status(const char* head, const char* status) {
    char msg[320];
    if (status && *status)
        std::snprintf(msg, sizeof(msg), "%s\nSTATUS=%s", head, status);
    else
        std::snprintf(msg, sizeof(msg), "%s", head);
    (void)sd_notify(0, msg);
}
#endif
} // namespace

void service_notifier::ready(const char* status) {
#if defined(LUMEN_HAS_LOGIND)
    notify_with_status("READY=1", status);
#else
    (void)status;
#endif
}

void service_notifier::reloading(const char* status) {
#if defined(LUMEN_HAS_LOGIND)
    // Type=notify-reload: RELOADING=1 must carry the CLOCK_MONOTONIC time of the reload,
    // so the manager can tell this reload's READY from an earlier one.
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    char head[96];
    std::snprintf(head, sizeof(head), "RELOADING=1\nMONOTONIC_USEC=%llu",
                  static_cast<unsigned long long>(ts.tv_sec) * 1'000'000ull +
                      static_cast<unsigned long long>(ts.tv_nsec) / 1000ull);
    notify_with_status(head, status);
#else
    (void)status;
#endif
}

void service_notifier::stopping(const char* status) {
#if defined(LUMEN_HAS_LOGIND)
    notify_with_status("STOPPING=1", status);
#else
    (void)status;
#endif
}

void service_notifier::status(const char* status) {
#if defined(LUMEN_HAS_LOGIND)
    if (!status)
        return;
    char msg[288];
    std::snprintf(msg, sizeof(msg), "STATUS=%s", status);
    (void)sd_notify(0, msg);
#else
    (void)status;
#endif
}

unsigned long long service_notifier::watchdog_usec() {
#if defined(LUMEN_HAS_LOGIND)
    std::uint64_t usec = 0;
    if (sd_watchdog_enabled(0, &usec) > 0)
        return usec;
#endif
    return 0;
}

void service_notifier::watchdog_ping() {
#if defined(LUMEN_HAS_LOGIND)
    (void)sd_notify(0, "WATCHDOG=1");
#endif
}

// ── The session environment ──────────────────────────────────────────────────

user_environment::~user_environment() { close(); }

#if defined(LUMEN_HAS_LOGIND)
namespace {

int on_set_reply(sd_bus_message* m, void* user, sd_bus_error*) {
    static_cast<user_environment*>(user)->reply_arrived(true, !sd_bus_message_is_method_error(m, nullptr));
    return 1;
}

int on_activation_reply(sd_bus_message* m, void* user, sd_bus_error*) {
    static_cast<user_environment*>(user)->reply_arrived(false, !sd_bus_message_is_method_error(m, nullptr));
    return 1;
}

} // namespace
#endif

lx::result<void> user_environment::open(void* event) {
#if defined(LUMEN_HAS_LOGIND)
    if (bus_)
        return {};
    sd_bus* bus = nullptr;
    if (sd_bus_open_user_with_description(&bus, "lumen-session") < 0 || !bus)
        return lx::make_error(lx::io_err::not_found, "no user bus (DBUS_SESSION_BUS_ADDRESS)");
    if (event && sd_bus_attach_event(bus, static_cast<sd_event*>(event), 0) < 0) {
        sd_bus_unref(bus);
        return lx::make_error(lx::io_err::broken_pipe, "user bus: cannot attach to the loop");
    }
    bus_ = bus;
    return {};
#else
    (void)event;
    return lx::not_implemented("lx::session::user_environment::open");
#endif
}

lx::result<void> user_environment::export_async(std::span<const std::string_view> assignments,
                                                unsigned timeout_ms,
                                                void (*done)(void* user, bool ok), void* user) {
#if defined(LUMEN_HAS_LOGIND)
    auto* bus = static_cast<sd_bus*>(bus_);
    if (!bus)
        return lx::make_error(lx::io_err::not_found, "user bus not open");
    if (pending_ != 0)
        return lx::invalid_argument("an environment export is already in flight");
    if (assignments.empty() || assignments.size() > k_max_names)
        return lx::invalid_argument("bad environment export size");

    // Names accumulate across exports (a configuration reload re-exports XCURSOR_*): every
    // one is unset at the end.
    sd_bus_message* set = nullptr;
    sd_bus_message* act = nullptr;
    int r = sd_bus_message_new_method_call(bus, &set, "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager", "SetEnvironment");
    if (r >= 0)
        r = sd_bus_message_new_method_call(bus, &act, "org.freedesktop.DBus",
                                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                           "UpdateActivationEnvironment");
    if (r >= 0)
        r = sd_bus_message_open_container(set, 'a', "s");
    if (r >= 0)
        r = sd_bus_message_open_container(act, 'a', "{ss}");
    char kv[512];
    for (std::size_t i = 0; r >= 0 && i < assignments.size(); ++i) {
        const std::string_view a = assignments[i];
        const auto eq = a.find('=');
        if (eq == std::string_view::npos || eq == 0 || eq >= 63 || a.size() >= sizeof(kv)) {
            r = -EINVAL;
            break;
        }
        std::memcpy(kv, a.data(), a.size());
        kv[a.size()] = '\0';
        std::array<char, 64> name{};
        std::memcpy(name.data(), a.data(), eq);
        if (std::ranges::find(names_, name) == names_.end())
            (void)names_.try_push_back(name);
        r = sd_bus_message_append(set, "s", kv);
        if (r >= 0) {
            kv[eq] = '\0';
            r = sd_bus_message_append(act, "{ss}", kv, kv + eq + 1);
        }
    }
    if (r >= 0)
        r = sd_bus_message_close_container(set);
    if (r >= 0)
        r = sd_bus_message_close_container(act);
    const std::uint64_t usec = static_cast<std::uint64_t>(timeout_ms) * 1000ull;
    sd_bus_slot* s1 = nullptr;
    sd_bus_slot* s2 = nullptr;
    if (r >= 0)
        r = sd_bus_call_async(bus, &s1, set, &on_set_reply, this, usec);
    if (r >= 0 && sd_bus_call_async(bus, &s2, act, &on_activation_reply, this, usec) < 0)
        s2 = nullptr; // the manager's environment is the one that counts; go on without
    sd_bus_message_unref(set);
    sd_bus_message_unref(act);
    if (r < 0) {
        if (s1)
            sd_bus_slot_unref(s1);
        return lx::make_error(lx::io_err::broken_pipe, "SetEnvironment could not be sent");
    }
    set_slot_ = s1;
    activation_slot_ = s2;
    pending_ = s2 ? 2u : 1u;
    manager_ok_ = false;
    exported_ = false;
    done_ = done;
    done_user_ = user;
    return {};
#else
    (void)assignments;
    (void)timeout_ms;
    (void)done;
    (void)user;
    return lx::not_implemented("lx::session::user_environment::export_async");
#endif
}

void user_environment::reply_arrived(bool manager, bool ok) {
#if defined(LUMEN_HAS_LOGIND)
    // The slot fired; drop our reference (sd-bus already disconnected it from the call).
    void*& slot = manager ? set_slot_ : activation_slot_;
    if (slot) {
        sd_bus_slot_unref(static_cast<sd_bus_slot*>(slot));
        slot = nullptr;
    }
#endif
    if (manager)
        manager_ok_ = ok;
    if (pending_ > 0 && --pending_ == 0) {
        exported_ = manager_ok_;
        if (auto* done = std::exchange(done_, nullptr))
            done(done_user_, manager_ok_);
    }
}

void user_environment::unset_now(unsigned timeout_ms) {
#if defined(LUMEN_HAS_LOGIND)
    auto* bus = static_cast<sd_bus*>(bus_);
    if (!bus || names_.empty())
        return;
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.systemd1",
                                           "/org/freedesktop/systemd1",
                                           "org.freedesktop.systemd1.Manager", "UnsetEnvironment");
    if (r >= 0)
        r = sd_bus_message_open_container(m, 'a', "s");
    for (const auto& n : names_)
        if (r >= 0)
            r = sd_bus_message_append(m, "s", n.data());
    if (r >= 0)
        r = sd_bus_message_close_container(m);
    if (r >= 0) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        (void)sd_bus_call(bus, m, static_cast<std::uint64_t>(timeout_ms) * 1000ull, &err, nullptr);
        sd_bus_error_free(&err);
    }
    sd_bus_message_unref(m);
    names_.clear();
    exported_ = false;
#else
    (void)timeout_ms;
#endif
}

void user_environment::close() {
#if defined(LUMEN_HAS_LOGIND)
    for (void** s : {&set_slot_, &activation_slot_}) {
        if (*s)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(*s));
        *s = nullptr;
    }
    if (auto* bus = static_cast<sd_bus*>(bus_)) {
        (void)sd_bus_flush(bus);
        (void)sd_bus_detach_event(bus);
        sd_bus_flush_close_unref(bus);
    }
#endif
    bus_ = nullptr;
    pending_ = 0;
    done_ = nullptr;
}

} // namespace lx::session
