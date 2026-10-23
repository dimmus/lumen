module;

// lumen-shell's user-bus connection (`:user_bus`). See the partition.

#include <errno.h>
#include <poll.h>
#include <time.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

module lx.shell;

import std;
import lx.trace;
import :user_bus;

namespace {

constexpr const char* k_bus_category = "shell.bus";

} // namespace

#if defined(LUMEN_HAS_LOGIND)

struct lx::shell::user_bus::events {
    static user_bus& self(void* data) { return *static_cast<user_bus*>(data); }

    static name_slot* slot_of(user_bus& b, const char* name) {
        for (auto& s : b.names_)
            if (s.used && name && std::strcmp(s.name, name) == 0)
                return &s;
        return nullptr;
    }

    static void set_state(name_slot& s, name_state st) {
        if (s.state == st)
            return;
        s.state = st;
        lx::trace::logger::global().logf(
            st == name_state::failed ? lx::trace::level::warn : lx::trace::level::info,
            k_bus_category, "bus name {}: {}", s.name,
            st == name_state::owned    ? "owned"
            : st == name_state::queued ? "queued (another owner has it; taken when it exits)"
            : st == name_state::lost   ? "lost"
                                       : "request failed");
        if (s.fn)
            s.fn(s.name, st, s.user);
    }

    static int requested(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* s = static_cast<name_slot*>(data);
        s->call = sd_bus_slot_unref(static_cast<sd_bus_slot*>(s->call));
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            lx::trace::logger::global().logf(lx::trace::level::warn, k_bus_category,
                                             "RequestName {}: {}", s->name,
                                             e && e->message ? e->message : "error");
            set_state(*s, name_state::failed);
            return 0;
        }
        std::uint32_t r = 0;
        if (sd_bus_message_read(m, "u", &r) < 0)
            r = 0;
        // 1 primary owner, 2 in queue, 3 exists (not queued), 4 already the owner.
        set_state(*s, r == 1 || r == 4 ? name_state::owned
                      : r == 2         ? name_state::queued
                                       : name_state::failed);
        return 0;
    }

    static int acquired(sd_bus_message* m, void* data, sd_bus_error*) {
        const char* name = nullptr;
        if (sd_bus_message_read(m, "s", &name) < 0)
            return 0;
        if (name_slot* s = slot_of(self(data), name))
            set_state(*s, name_state::owned);
        return 0;
    }

    static int name_lost(sd_bus_message* m, void* data, sd_bus_error*) {
        const char* name = nullptr;
        if (sd_bus_message_read(m, "s", &name) < 0)
            return 0;
        if (name_slot* s = slot_of(self(data), name))
            set_state(*s, name_state::lost);
        return 0;
    }
};

lx::shell::user_bus::~user_bus() { close(); }

bool lx::shell::user_bus::open() {
    if (bus_)
        return true;
    sd_bus* bus = nullptr;
    const int r = sd_bus_open_user_with_description(&bus, "lumen-shell");
    if (r < 0) {
        if (!told_unavailable_) {
            told_unavailable_ = true;
            lx::trace::logger::global().logf(lx::trace::level::warn, k_bus_category,
                                             "no user bus ({}): no notifications, no tray",
                                             std::strerror(-r));
        }
        return false;
    }
    adopt(bus);
    sd_bus_unref(bus);
    return true;
}

void lx::shell::user_bus::adopt(void* b) {
    close();
    auto* bus = sd_bus_ref(static_cast<sd_bus*>(b));
    bus_ = bus;
    sd_bus_slot* slot = nullptr;
    // Unicast from the bus driver: our names' NameAcquired / NameLost.
    if (sd_bus_match_signal_async(bus, &slot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "NameAcquired", events::acquired,
                                  nullptr, this) >= 0)
        acquired_ = slot;
    slot = nullptr;
    if (sd_bus_match_signal_async(bus, &slot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "NameLost", events::name_lost, nullptr,
                                  this) >= 0)
        name_lost_ = slot;
}

void lx::shell::user_bus::close() {
    if (!bus_)
        return;
    for (auto& s : names_) {
        if (s.call)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(s.call));
        s = {};
    }
    acquired_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(acquired_));
    name_lost_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(name_lost_));
    bus_ = sd_bus_flush_close_unref(static_cast<sd_bus*>(bus_));
}

const char* lx::shell::user_bus::unique_name() const {
    const char* u = nullptr;
    if (bus_ && sd_bus_get_unique_name(static_cast<sd_bus*>(bus_), &u) >= 0 && u)
        return u;
    return "";
}

int lx::shell::user_bus::fd() const {
    return bus_ ? sd_bus_get_fd(static_cast<sd_bus*>(bus_)) : -1;
}

short lx::shell::user_bus::events() const {
    if (!bus_)
        return 0;
    const int e = sd_bus_get_events(static_cast<sd_bus*>(bus_));
    return e > 0 ? static_cast<short>(e) : static_cast<short>(POLLIN);
}

int lx::shell::user_bus::timeout_ms() const {
    if (!bus_)
        return -1;
    std::uint64_t until = 0;
    if (sd_bus_get_timeout(static_cast<sd_bus*>(bus_), &until) < 0 || until == UINT64_MAX)
        return -1;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const std::uint64_t now_us = static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
                                 static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
    const std::uint64_t ms = until > now_us ? (until - now_us + 999u) / 1000u : 0u;
    return ms > 600000u ? 600000 : static_cast<int>(ms);
}

void lx::shell::user_bus::process() {
    if (!bus_)
        return;
    auto* bus = static_cast<sd_bus*>(bus_);
    for (int guard = 0; guard < 256; ++guard) {
        const int r = sd_bus_process(bus, nullptr);
        if (r == 0)
            return;
        if (r < 0) {
            lx::trace::logger::global().logf(lx::trace::level::warn, k_bus_category,
                                             "user bus connection lost: {}", std::strerror(-r));
            lost();
            return;
        }
    }
}

void lx::shell::user_bus::lost() {
    // Observers first (their slots still reference the bus), then the connection.
    for (const auto& l : lost_)
        if (l.fn)
            l.fn(l.user);
    close();
}

bool lx::shell::user_bus::request_name(const char* name, name_fn fn, void* user) {
    if (!bus_ || !name)
        return false;
    name_slot* s = nullptr;
    for (auto& c : names_)
        if (!c.used) {
            s = &c;
            break;
        }
    if (!s)
        return false;
    *s = {};
    std::snprintf(s->name, sizeof(s->name), "%s", name);
    s->fn = fn;
    s->user = user;
    s->used = true;
    sd_bus_slot* call = nullptr;
    const int r = sd_bus_request_name_async(static_cast<sd_bus*>(bus_), &call, name,
                                            SD_BUS_NAME_QUEUE, events::requested, s);
    if (r < 0) {
        lx::trace::logger::global().logf(lx::trace::level::warn, k_bus_category,
                                         "RequestName {}: {}", name, std::strerror(-r));
        *s = {};
        return false;
    }
    s->call = call;
    return true;
}

void lx::shell::user_bus::release_name(const char* name) {
    for (auto& s : names_) {
        if (!s.used || std::strcmp(s.name, name) != 0)
            continue;
        if (s.call)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(s.call));
        if (bus_)
            (void)sd_bus_release_name_async(static_cast<sd_bus*>(bus_), nullptr, s.name, nullptr,
                                            nullptr);
        s = {};
    }
}

bool lx::shell::user_bus::owns(const char* name) const {
    for (const auto& s : names_)
        if (s.used && std::strcmp(s.name, name) == 0)
            return s.state == name_state::owned;
    return false;
}

void lx::shell::user_bus::add_lost_handler(lost_fn fn, void* user) {
    for (auto& l : lost_)
        if (!l.fn) {
            l = {fn, user};
            return;
        }
}

#else // !LUMEN_HAS_LOGIND

struct lx::shell::user_bus::events {};
lx::shell::user_bus::~user_bus() = default;
bool lx::shell::user_bus::open() {
    if (!told_unavailable_) {
        told_unavailable_ = true;
        lx::trace::logger::global().log_error(lx::not_implemented("lx::shell::user_bus::open"),
                                              k_bus_category);
    }
    return false;
}
void lx::shell::user_bus::adopt(void*) {}
void lx::shell::user_bus::close() {}
const char* lx::shell::user_bus::unique_name() const { return ""; }
int lx::shell::user_bus::fd() const { return -1; }
short lx::shell::user_bus::events() const { return 0; }
int lx::shell::user_bus::timeout_ms() const { return -1; }
void lx::shell::user_bus::process() {}
void lx::shell::user_bus::lost() {}
bool lx::shell::user_bus::request_name(const char*, name_fn, void*) { return false; }
void lx::shell::user_bus::release_name(const char*) {}
bool lx::shell::user_bus::owns(const char*) const { return false; }
void lx::shell::user_bus::add_lost_handler(lost_fn, void*) {}

#endif
