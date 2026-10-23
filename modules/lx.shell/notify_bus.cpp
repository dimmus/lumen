module;

// org.freedesktop.Notifications on sd-bus (`:notifications`' notification_server): the
// object, its methods and signals, the hints read into a `notify_request`, and the senders
// watched while they have notifications with actions. Every call is answered inline on the
// shell's thread; nothing here runs unless a message arrives.

#include <errno.h>
#include <time.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

module lx.shell;

import std;
import lx.foundation;
import lx.trace;
import :notifications;

namespace {

constexpr const char* k_notify_bus_category = "shell.notify";
constexpr const char* k_path = "/org/freedesktop/Notifications";
constexpr const char* k_iface = "org.freedesktop.Notifications";
constexpr unsigned k_max_actions = 16;
constexpr unsigned k_max_watched = 32;
/// The largest icon kept from image-data (the cards draw 48 logical px, 96 at scale 2).
constexpr int k_image_max = 96;

long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

[[nodiscard]] bool notify_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

std::string bounded(const char* s, std::size_t cap) {
    std::string out{s ? s : ""};
    if (out.size() > cap) {
        out.resize(cap);
        // Not inside a UTF-8 sequence.
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0u)
            out.pop_back();
    }
    return out;
}

} // namespace

#if defined(LUMEN_HAS_LOGIND)

struct lx::shell::notification_server::events {
    static notification_server& self(void* data) { return *static_cast<notification_server*>(data); }

    static long long now(const notification_server& s) { return s.now_ ? s.now_() : monotonic_ns(); }

    static int get_capabilities(sd_bus_message* m, void*, sd_bus_error*) {
        sd_bus_message* reply = nullptr;
        int r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
            return r;
        r = sd_bus_message_append(reply, "as", 6, "actions", "body", "body-markup",
                                  "body-hyperlinks", "icon-static", "persistence");
        if (r >= 0)
            r = sd_bus_send(nullptr, reply, nullptr);
        sd_bus_message_unref(reply);
        return r;
    }

    static int get_server_information(sd_bus_message* m, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(m, "ssss", "Lumen", "Lumen", "0.3.0", "1.2");
    }

    /// One hint's value (the message is at the variant).
    static int read_hint(sd_bus_message* m, const char* key, notify_request& req) {
        const std::string_view k{key};
        char type = 0;
        const char* contents = nullptr;
        int r = sd_bus_message_peek_type(m, &type, &contents);
        if (r < 0)
            return r;
        const std::string_view sig{contents ? contents : ""};
        if (k == "urgency") {
            std::uint8_t b = 1;
            std::int32_t i = 1;
            std::uint32_t u = 1;
            if (sig == "y")
                r = sd_bus_message_read(m, "v", "y", &b);
            else if (sig == "i" && (r = sd_bus_message_read(m, "v", "i", &i)) >= 0)
                b = static_cast<std::uint8_t>(i < 0 ? 1 : i);
            else if (sig == "u" && (r = sd_bus_message_read(m, "v", "u", &u)) >= 0)
                b = static_cast<std::uint8_t>(u);
            else
                return sd_bus_message_skip(m, "v");
            if (r >= 0)
                req.urgency = b == 0 ? notify_urgency::low
                              : b >= 2 ? notify_urgency::critical
                                       : notify_urgency::normal;
            return r;
        }
        if ((k == "category" || k == "desktop-entry" || k == "image-path" || k == "image_path") &&
            sig == "s") {
            const char* s = nullptr;
            r = sd_bus_message_read(m, "v", "s", &s);
            if (r < 0)
                return r;
            if (k == "category")
                req.category = bounded(s, 128);
            else if (k == "desktop-entry")
                req.desktop_entry = bounded(s, 256);
            else
                req.image_path = bounded(s, 4096);
            return r;
        }
        if ((k == "resident" || k == "transient") && sig == "b") {
            int b = 0;
            r = sd_bus_message_read(m, "v", "b", &b);
            if (r >= 0)
                (k == "resident" ? req.resident : req.transient) = b != 0;
            return r;
        }
        if ((k == "image-data" || k == "image_data" || k == "icon_data") && sig == "(iiibiiay)") {
            // image-data wins over the deprecated spellings and icon_data.
            const bool weaker = k == "icon_data" || k == "image_data";
            r = sd_bus_message_enter_container(m, 'v', "(iiibiiay)");
            if (r < 0)
                return r;
            r = sd_bus_message_enter_container(m, 'r', "iiibiiay");
            if (r < 0)
                return r;
            std::int32_t w = 0, h = 0, stride = 0, bps = 0, ch = 0;
            int alpha = 0;
            r = sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bps, &ch);
            if (r < 0)
                return r;
            const void* data = nullptr;
            std::size_t size = 0;
            r = sd_bus_message_read_array(m, 'y', &data, &size);
            if (r < 0)
                return r;
            if (!(weaker && req.image.size > 0)) {
                notify_image img{};
                const image_data_error e = decode_image_data(
                    w, h, stride, alpha != 0, bps, ch,
                    {static_cast<const std::uint8_t*>(data), size}, k_image_max, img);
                if (e == image_data_error::none)
                    req.image = std::move(img);
                else
                    lx::trace::logger::global().logf(
                        lx::trace::level::warn, k_notify_bus_category,
                        "notify: {}'s {} ({}x{}, stride {}, {} channels): {}; ignored",
                        req.app_name, key, w, h, stride, ch, image_data_error_text(e));
            }
            r = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
            return sd_bus_message_exit_container(m);
        }
        // x, y (no meaning on Wayland), sound-* and suppress-sound (no sound capability),
        // action-icons (not drawn), unknown ones.
        return sd_bus_message_skip(m, "v");
    }

    static int notify(sd_bus_message* m, void* data, sd_bus_error* err) {
        auto& s = self(data);
        if (!s.center_)
            return sd_bus_error_set_const(err, "org.freedesktop.DBus.Error.Failed", "not ready");
        notify_request req{};
        const char *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
        std::uint32_t replaces = 0;
        int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
        if (r < 0)
            return r;
        req.app_name = bounded(app, 256);
        req.app_icon = bounded(icon, 4096);
        req.summary = bounded(summary, 1024);
        req.body = bounded(body, 8192);
        req.replaces_id = replaces;
        if (const char* sender = sd_bus_message_get_sender(m))
            req.sender = sender;
        // Actions: key, label pairs.
        r = sd_bus_message_enter_container(m, 'a', "s");
        if (r < 0)
            return r;
        const char* key = nullptr;
        for (;;) {
            const char* item = nullptr;
            r = sd_bus_message_read(m, "s", &item);
            if (r < 0)
                return r;
            if (r == 0)
                break;
            if (!key) {
                key = item;
                continue;
            }
            if (req.actions.size() < k_max_actions)
                req.actions.push_back({bounded(key, 128), bounded(item, 256)});
            key = nullptr;
        }
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
        // Hints.
        r = sd_bus_message_enter_container(m, 'a', "{sv}");
        if (r < 0)
            return r;
        for (;;) {
            r = sd_bus_message_enter_container(m, 'e', "sv");
            if (r < 0)
                return r;
            if (r == 0)
                break;
            const char* hint = nullptr;
            r = sd_bus_message_read(m, "s", &hint);
            if (r < 0)
                return r;
            r = read_hint(m, hint ? hint : "", req);
            if (r < 0)
                return r;
            r = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
        }
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
        std::int32_t timeout = -1;
        r = sd_bus_message_read(m, "i", &timeout);
        if (r < 0)
            return r;
        req.expire_timeout = timeout;
        const bool watch = !req.sender.empty() && !req.actions.empty();
        const std::string sender = req.sender;
        const notify_urgency urgency = req.urgency;
        const std::uint32_t asked = req.replaces_id;
        bool merged = false;
        const unsigned id = s.center_->notify(std::move(req), now(s), &merged);
        s.changed_ = true;
        if (watch)
            s.watch_sender(sender);
        if (const notification* n = s.center_->find(id)) {
            lx::trace::logger::global().logf(
                lx::trace::level::info, k_notify_bus_category,
                "notify id={}{} app=\"{}\" urgency={} timeout={} actions={}{}{} summary=\"{}\" "
                "{}",
                id, asked ? (asked == id ? " (replaced)" : " (replacement of an unknown id)") : "",
                n->req.app_name,
                urgency == notify_urgency::low ? "low"
                : urgency == notify_urgency::critical ? "critical"
                                                      : "normal",
                n->req.expire_timeout, n->req.actions.size(), n->req.image.size ? " image" : "",
                merged ? " merged" : "", n->req.summary,
                n->in_popup ? (s.center_->locked() ? "queued (locked)" : "popup")
                : n->in_history ? "history (do not disturb)"
                                : "dropped");
        }
        return sd_bus_reply_method_return(m, "u", id);
    }

    static int close_notification(sd_bus_message* m, void* data, sd_bus_error* err) {
        auto& s = self(data);
        std::uint32_t id = 0;
        int r = sd_bus_message_read(m, "u", &id);
        if (r < 0)
            return r;
        if (!s.center_ || !s.center_->close(id, close_reason::closed, now(s))) {
            // "If the notification no longer exists, an empty D-BUS Error message is sent
            // back."
            if (notify_debug())
                lx::trace::logger::global().logf(lx::trace::level::debug, k_notify_bus_category,
                                                 "CloseNotification {}: no such notification", id);
            return sd_bus_error_set_const(err, "org.freedesktop.Notifications.Error.NotFound",
                                          nullptr);
        }
        s.changed_ = true;
        s.unwatch_idle_senders();
        return sd_bus_reply_method_return(m, "");
    }

    static int sender_changed(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* w = static_cast<sender_watch*>(data);
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
            return 0;
        if (new_owner && *new_owner)
            return 0;
        notification_server& s = *w->owner;
        if (s.center_)
            s.center_->sender_gone(w->name);
        if (notify_debug())
            lx::trace::logger::global().logf(lx::trace::level::debug, k_notify_bus_category,
                                             "notify: {} left the bus: its actions are gone",
                                             w->name);
        s.changed_ = true;
        s.unwatch_idle_senders();
        return 0;
    }
};

namespace {

const sd_bus_vtable k_notify_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("GetCapabilities", SD_BUS_NO_ARGS, SD_BUS_RESULT("as", capabilities),
                            lx::shell::notification_server::events::get_capabilities,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Notify",
                            SD_BUS_ARGS("s", app_name, "u", replaces_id, "s", app_icon, "s",
                                        summary, "s", body, "as", actions, "a{sv}", hints, "i",
                                        expire_timeout),
                            SD_BUS_RESULT("u", id), lx::shell::notification_server::events::notify,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("CloseNotification", SD_BUS_ARGS("u", id), SD_BUS_NO_RESULT,
                            lx::shell::notification_server::events::close_notification,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("GetServerInformation", SD_BUS_NO_ARGS,
                            SD_BUS_RESULT("s", name, "s", vendor, "s", version, "s", spec_version),
                            lx::shell::notification_server::events::get_server_information,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL_WITH_ARGS("NotificationClosed", SD_BUS_ARGS("u", id, "u", reason), 0),
    SD_BUS_SIGNAL_WITH_ARGS("ActionInvoked", SD_BUS_ARGS("u", id, "s", action_key), 0),
    SD_BUS_SIGNAL_WITH_ARGS("ActivationToken", SD_BUS_ARGS("u", id, "s", activation_token), 0),
    SD_BUS_VTABLE_END,
};

} // namespace

lx::shell::notification_server::~notification_server() { detach(); }

bool lx::shell::notification_server::attach(void* bus, notification_center& center) {
    detach();
    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_add_object_vtable(static_cast<sd_bus*>(bus), &slot, k_path, k_iface,
                                           k_notify_vtable, this);
    if (r < 0) {
        lx::trace::logger::global().logf(lx::trace::level::warn, k_notify_bus_category,
                                         "notifications: cannot export {}: {}", k_path,
                                         std::strerror(-r));
        return false;
    }
    bus_ = bus;
    slot_ = slot;
    center_ = &center;
    return true;
}

void lx::shell::notification_server::detach() {
    for (auto& w : senders_)
        if (w->slot)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(w->slot));
    senders_.clear();
    slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(slot_));
    bus_ = nullptr;
    center_ = nullptr;
}

void lx::shell::notification_server::emit_closed(unsigned id, close_reason reason) {
    if (!bus_)
        return;
    (void)sd_bus_emit_signal(static_cast<sd_bus*>(bus_), k_path, k_iface, "NotificationClosed",
                             "uu", id, static_cast<std::uint32_t>(reason));
    lx::trace::logger::global().logf(lx::trace::level::info, k_notify_bus_category,
                                     "notify id={} closed reason={} ({})", id,
                                     static_cast<unsigned>(reason),
                                     reason == close_reason::expired     ? "expired"
                                     : reason == close_reason::dismissed ? "dismissed"
                                     : reason == close_reason::closed    ? "CloseNotification"
                                                                         : "undefined");
}

void lx::shell::notification_server::action_invoked(unsigned id, const char* key, const char* token,
                                                    long long now) {
    if (!center_)
        return;
    const notification* n = center_->find(id);
    if (n && n->open && bus_) {
        // "ActivationToken … may be emitted before ActionInvoked": the app raises its window
        // with it.
        if (token && *token)
            (void)sd_bus_emit_signal(static_cast<sd_bus*>(bus_), k_path, k_iface,
                                     "ActivationToken", "us", id, token);
        (void)sd_bus_emit_signal(static_cast<sd_bus*>(bus_), k_path, k_iface, "ActionInvoked",
                                 "us", id, key ? key : "");
        lx::trace::logger::global().logf(lx::trace::level::info, k_notify_bus_category,
                                         "notify id={} action \"{}\" invoked token={}", id,
                                         key ? key : "", token && *token ? "yes" : "no");
    }
    center_->finish_invoke(id, now);
    changed_ = true;
    unwatch_idle_senders();
}

void lx::shell::notification_server::watch_sender(const std::string& sender) {
    if (!bus_ || sender.empty() || sender[0] != ':')
        return;
    for (const auto& w : senders_)
        if (w->name == sender)
            return;
    if (senders_.size() >= k_max_watched)
        return; // unwatched: its actions stay invokable after it leaves (harmless)
    auto w = std::make_unique<sender_watch>();
    w->owner = this;
    w->name = sender;
    char match[256];
    std::snprintf(match, sizeof(match),
                  "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
                  sender.c_str());
    sd_bus_slot* slot = nullptr;
    if (sd_bus_add_match_async(static_cast<sd_bus*>(bus_), &slot, match, events::sender_changed,
                               nullptr, w.get()) < 0)
        return;
    w->slot = slot;
    senders_.push_back(std::move(w));
}

void lx::shell::notification_server::unwatch_idle_senders() {
    if (!center_)
        return;
    std::erase_if(senders_, [&](const std::unique_ptr<sender_watch>& w) {
        if (center_->sender_has_actions(w->name))
            return false;
        if (w->slot)
            sd_bus_slot_unref(static_cast<sd_bus_slot*>(w->slot));
        return true;
    });
}

unsigned lx::shell::notification_server::watched_senders() const {
    return static_cast<unsigned>(senders_.size());
}

#else // !LUMEN_HAS_LOGIND

struct lx::shell::notification_server::events {};
lx::shell::notification_server::~notification_server() = default;
bool lx::shell::notification_server::attach(void*, notification_center&) { return false; }
void lx::shell::notification_server::detach() {}
void lx::shell::notification_server::emit_closed(unsigned, close_reason) {}
void lx::shell::notification_server::action_invoked(unsigned id, const char*, const char*,
                                                    long long now) {
    if (center_)
        center_->finish_invoke(id, now);
}
void lx::shell::notification_server::watch_sender(const std::string&) {}
void lx::shell::notification_server::unwatch_idle_senders() {}
unsigned lx::shell::notification_server::watched_senders() const { return 0; }

#endif
