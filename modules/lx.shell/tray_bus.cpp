module;

// The tray on sd-bus (`:tray`' tray_service, parse_dbusmenu_layout): the
// org.kde.StatusNotifierWatcher object, the host's view of every item, and com.canonical.
// dbusmenu calls. Every reply and signal is handled on the shell's thread as it arrives;
// an item's properties are read again only when it signals a change.

#include <errno.h>
#include <time.h>
#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

module lx.shell;

import std;
import lx.foundation;
import lx.trace;
import :tray;
import :menu_popup;
import :user_bus;

namespace {

constexpr const char* k_tray_category = "shell.tray";
constexpr const char* k_watcher_name = "org.kde.StatusNotifierWatcher";
constexpr const char* k_watcher_path = "/StatusNotifierWatcher";
constexpr const char* k_watcher_iface = "org.kde.StatusNotifierWatcher";
constexpr const char* k_item_iface = "org.kde.StatusNotifierItem";
constexpr const char* k_menu_iface = "com.canonical.dbusmenu";
constexpr unsigned k_menu_max_items = 256;
constexpr unsigned k_menu_max_depth = 8;

[[nodiscard]] bool tray_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

} // namespace

#if defined(LUMEN_HAS_LOGIND)

// ── com.canonical.dbusmenu layouts ───────────────────────────────────────────

namespace {

int read_string_variant(sd_bus_message* m, const char* contents, std::string& out) {
    if (std::strcmp(contents, "s") != 0)
        return sd_bus_message_skip(m, "v");
    const char* s = nullptr;
    const int r = sd_bus_message_read(m, "v", "s", &s);
    if (r >= 0)
        out = s ? s : "";
    return r;
}

int read_bool_variant(sd_bus_message* m, const char* contents, bool& out) {
    if (std::strcmp(contents, "b") != 0)
        return sd_bus_message_skip(m, "v");
    int b = 0;
    const int r = sd_bus_message_read(m, "v", "b", &b);
    if (r >= 0)
        out = b != 0;
    return r;
}

int read_shortcut(sd_bus_message* m, const char* contents, std::string& out) {
    if (std::strcmp(contents, "aas") != 0)
        return sd_bus_message_skip(m, "v");
    int r = sd_bus_message_enter_container(m, 'v', "aas");
    if (r < 0)
        return r;
    r = sd_bus_message_enter_container(m, 'a', "as");
    if (r < 0)
        return r;
    bool first = true;
    for (;;) {
        r = sd_bus_message_enter_container(m, 'a', "s");
        if (r <= 0)
            break;
        for (;;) {
            const char* key = nullptr;
            r = sd_bus_message_read(m, "s", &key);
            if (r <= 0)
                break;
            if (first) {
                if (!out.empty())
                    out += '+';
                out += key ? key : "";
            }
        }
        first = false;
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }
    if (r < 0)
        return r;
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;
    return sd_bus_message_exit_container(m);
}

/// One (ia{sv}av) at the message's position into `out`; `visible` reports its property.
int read_menu_item(sd_bus_message* m, lx::shell::menu_entry& out, bool& visible, unsigned depth,
                   unsigned& budget) {
    int r = sd_bus_message_enter_container(m, 'r', "ia{sv}av");
    if (r <= 0)
        return r < 0 ? r : -EBADMSG;
    std::int32_t id = 0;
    r = sd_bus_message_read(m, "i", &id);
    if (r < 0)
        return r;
    lx::shell::dbusmenu_props p{};
    r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0)
        return r;
    for (;;) {
        r = sd_bus_message_enter_container(m, 'e', "sv");
        if (r < 0)
            return r;
        if (r == 0)
            break;
        const char* key = nullptr;
        r = sd_bus_message_read(m, "s", &key);
        if (r < 0)
            return r;
        char type = 0;
        const char* contents = nullptr;
        r = sd_bus_message_peek_type(m, &type, &contents);
        if (r < 0)
            return r;
        const std::string_view k{key ? key : ""};
        const char* c = contents ? contents : "";
        if (k == "label") {
            r = read_string_variant(m, c, p.label);
        } else if (k == "enabled") {
            r = read_bool_variant(m, c, p.enabled);
        } else if (k == "visible") {
            r = read_bool_variant(m, c, p.visible);
        } else if (k == "type") {
            std::string t;
            r = read_string_variant(m, c, t);
            p.separator = t == "separator";
        } else if (k == "toggle-type") {
            r = read_string_variant(m, c, p.toggle_type);
        } else if (k == "toggle-state" && std::strcmp(c, "i") == 0) {
            std::int32_t v = -1;
            r = sd_bus_message_read(m, "v", "i", &v);
            p.toggle_state = v;
        } else if (k == "icon-name") {
            r = read_string_variant(m, c, p.icon_name);
        } else if (k == "children-display") {
            std::string t;
            r = read_string_variant(m, c, t);
            p.submenu = t == "submenu";
        } else if (k == "shortcut") {
            r = read_shortcut(m, c, p.shortcut);
        } else {
            r = sd_bus_message_skip(m, "v");
        }
        if (r < 0)
            return r;
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;
    out = lx::shell::dbusmenu_entry(id, p);
    visible = p.visible;
    r = sd_bus_message_enter_container(m, 'a', "v");
    if (r < 0)
        return r;
    for (;;) {
        r = sd_bus_message_enter_container(m, 'v', "(ia{sv}av)");
        if (r < 0)
            return r;
        if (r == 0)
            break;
        if (depth >= k_menu_max_depth || budget == 0) {
            r = sd_bus_message_skip(m, "(ia{sv}av)");
        } else {
            --budget;
            lx::shell::menu_entry child{};
            bool child_visible = true;
            r = read_menu_item(m, child, child_visible, depth + 1, budget);
            if (r >= 0 && child_visible)
                out.children.push_back(std::move(child));
        }
        if (r < 0)
            return r;
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;
    // A submenu is what has children, whatever children-display said.
    out.submenu = !out.children.empty();
    return sd_bus_message_exit_container(m);
}

int read_pixmaps(sd_bus_message* m, std::vector<lx::shell::sni_pixmap>& out) {
    out.clear();
    int r = sd_bus_message_enter_container(m, 'a', "(iiay)");
    if (r < 0)
        return r;
    for (;;) {
        r = sd_bus_message_enter_container(m, 'r', "iiay");
        if (r < 0)
            return r;
        if (r == 0)
            break;
        std::int32_t w = 0, h = 0;
        r = sd_bus_message_read(m, "ii", &w, &h);
        if (r < 0)
            return r;
        const void* data = nullptr;
        std::size_t size = 0;
        r = sd_bus_message_read_array(m, 'y', &data, &size);
        if (r < 0)
            return r;
        lx::shell::sni_pixmap p{};
        if (out.size() < 8 &&
            lx::shell::convert_sni_pixmap(w, h, {static_cast<const std::uint8_t*>(data), size}, p))
            out.push_back(std::move(p));
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }
    return sd_bus_message_exit_container(m);
}

} // namespace

bool lx::shell::parse_dbusmenu_layout(void* message, menu_entry& root, unsigned* revision) {
    auto* m = static_cast<sd_bus_message*>(message);
    std::uint32_t rev = 0;
    if (sd_bus_message_read(m, "u", &rev) < 0)
        return false;
    if (revision)
        *revision = rev;
    root = {};
    bool visible = true;
    unsigned budget = k_menu_max_items;
    return read_menu_item(m, root, visible, 0, budget) >= 0;
}

// ── The service ──────────────────────────────────────────────────────────────

struct lx::shell::tray_service::events {
    static tray_service& self(void* data) { return *static_cast<tray_service*>(data); }

    // ── org.kde.StatusNotifierWatcher ──
    static int register_item(sd_bus_message* m, void* data, sd_bus_error* err) {
        auto& t = self(data);
        const char* service = nullptr;
        int r = sd_bus_message_read(m, "s", &service);
        if (r < 0)
            return r;
        const char* sender = sd_bus_message_get_sender(m);
        std::string bus, path;
        if (!parse_sni_service(service ? service : "", sender ? sender : "", bus, path))
            return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                     "not a StatusNotifierItem service: %s", service ? service : "");
        const std::string id = bus + path;
        if (t.registry_.add_item(id)) {
            (void)sd_bus_emit_signal(static_cast<sd_bus*>(t.bus_), k_watcher_path, k_watcher_iface,
                                     "StatusNotifierItemRegistered", "s", id.c_str());
            lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                             "tray: item registered {} (by {})", id,
                                             sender ? sender : "?");
            t.add_item(bus, path, false);
        }
        return sd_bus_reply_method_return(m, "");
    }

    static int register_host(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        const char* service = nullptr;
        int r = sd_bus_message_read(m, "s", &service);
        if (r < 0)
            return r;
        if (service && t.registry_.add_host(service))
            (void)sd_bus_emit_signal(static_cast<sd_bus*>(t.bus_), k_watcher_path, k_watcher_iface,
                                     "StatusNotifierHostRegistered", "");
        return sd_bus_reply_method_return(m, "");
    }

    static int prop_items(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                          void* data, sd_bus_error*) {
        auto& t = self(data);
        int r = sd_bus_message_open_container(reply, 'a', "s");
        for (const std::string& s : t.registry_.items())
            if (r >= 0)
                r = sd_bus_message_append(reply, "s", s.c_str());
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
        return r;
    }

    static int prop_host(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                         void* data, sd_bus_error*) {
        return sd_bus_message_append(reply, "b", self(data).registry_.host_registered() ? 1 : 0);
    }

    static int prop_version(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                            void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "i", 0);
    }

    // ── Items ──
    static int item_signal(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* r = static_cast<item_rec*>(data);
        r->refresh = true;
        if (tray_debug())
            lx::trace::logger::global().logf(lx::trace::level::debug, k_tray_category,
                                             "tray: {} signals {}", r->service,
                                             sd_bus_message_get_member(m) ? sd_bus_message_get_member(m) : "?");
        return 0;
    }

    static int item_owner(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* r = static_cast<item_rec*>(data);
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
            return 0;
        if (new_owner && *new_owner)
            return 0;
        lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                         "tray: item {} left the bus", r->service);
        r->owner->remove_item(r, true);
        return 0;
    }

    static int got_all(sd_bus_message* m, void* data, sd_bus_error*) {
        auto* r = static_cast<item_rec*>(data);
        tray_service& t = *r->owner;
        r->fetch_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->fetch_slot));
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            if (!r->fetched) {
                lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                                 "tray: item {} not reachable ({}): dropped",
                                                 r->service, e && e->name ? e->name : "error");
                t.remove_item(r, true);
            }
            return 0;
        }
        const bool first = !r->fetched;
        if (parse_properties(m, *r) < 0) {
            lx::trace::logger::global().logf(lx::trace::level::warn, k_tray_category,
                                             "tray: item {}: malformed properties", r->service);
            if (first) {
                t.remove_item(r, true);
                return 0;
            }
        }
        r->fetched = true;
        ++r->rev;
        ++t.rev_;
        if (first)
            lx::trace::logger::global().logf(
                lx::trace::level::info, k_tray_category,
                "tray: item {} id=\"{}\" title=\"{}\" status={} icon={}{} menu={}", r->service, r->id,
                r->title, r->status.empty() ? "?" : r->status,
                r->icon_name.empty() ? "-" : r->icon_name,
                r->icon.empty() ? "" : " (pixmap)", r->has_menu() ? r->menu_path : "-");
        else if (tray_debug())
            lx::trace::logger::global().logf(lx::trace::level::debug, k_tray_category,
                                             "tray: item {} updated status={} icon={}", r->service,
                                             r->status, r->icon_name);
        if (r->refresh && !r->fetch_slot) {
            r->refresh = false;
            t.fetch(*r);
        }
        return 0;
    }

    static int parse_properties(sd_bus_message* m, item_rec& it) {
        int r = sd_bus_message_enter_container(m, 'a', "{sv}");
        if (r < 0)
            return r;
        for (;;) {
            r = sd_bus_message_enter_container(m, 'e', "sv");
            if (r < 0)
                return r;
            if (r == 0)
                break;
            const char* key = nullptr;
            r = sd_bus_message_read(m, "s", &key);
            if (r < 0)
                return r;
            char type = 0;
            const char* contents = nullptr;
            r = sd_bus_message_peek_type(m, &type, &contents);
            if (r < 0)
                return r;
            const std::string_view k{key ? key : ""};
            const char* c = contents ? contents : "";
            if (k == "Id")
                r = read_string_variant(m, c, it.id);
            else if (k == "Title")
                r = read_string_variant(m, c, it.title);
            else if (k == "Category")
                r = read_string_variant(m, c, it.category);
            else if (k == "Status")
                r = read_string_variant(m, c, it.status);
            else if (k == "IconName")
                r = read_string_variant(m, c, it.icon_name);
            else if (k == "AttentionIconName")
                r = read_string_variant(m, c, it.attention_icon_name);
            else if (k == "OverlayIconName")
                r = read_string_variant(m, c, it.overlay_icon_name);
            else if (k == "IconThemePath")
                r = read_string_variant(m, c, it.theme_path);
            else if (k == "ItemIsMenu")
                r = read_bool_variant(m, c, it.item_is_menu);
            else if (k == "Menu" && std::strcmp(c, "o") == 0) {
                const char* o = nullptr;
                r = sd_bus_message_read(m, "v", "o", &o);
                it.menu_path = o ? o : "";
            } else if ((k == "IconPixmap" || k == "AttentionIconPixmap" ||
                        k == "OverlayIconPixmap") &&
                       std::strcmp(c, "a(iiay)") == 0) {
                r = sd_bus_message_enter_container(m, 'v', "a(iiay)");
                if (r >= 0)
                    r = read_pixmaps(m, k == "IconPixmap"            ? it.icon
                                        : k == "AttentionIconPixmap" ? it.attention
                                                                     : it.overlay);
                if (r >= 0)
                    r = sd_bus_message_exit_container(m);
            } else if (k == "ToolTip" && std::strcmp(c, "(sa(iiay)ss)") == 0) {
                r = sd_bus_message_enter_container(m, 'v', "(sa(iiay)ss)");
                if (r >= 0)
                    r = sd_bus_message_enter_container(m, 'r', "sa(iiay)ss");
                const char *icon = nullptr, *title = nullptr, *text = nullptr;
                if (r >= 0)
                    r = sd_bus_message_read(m, "s", &icon);
                if (r >= 0)
                    r = sd_bus_message_skip(m, "a(iiay)");
                if (r >= 0)
                    r = sd_bus_message_read(m, "ss", &title, &text);
                if (r >= 0) {
                    it.tooltip_title = title ? title : "";
                    it.tooltip_text = text ? markup_to_plain(text) : "";
                    r = sd_bus_message_exit_container(m);
                }
                if (r >= 0)
                    r = sd_bus_message_exit_container(m);
            } else {
                r = sd_bus_message_skip(m, "v");
            }
            if (r < 0)
                return r;
            r = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
        }
        return sd_bus_message_exit_container(m);
    }

    // ── Another watcher (we are queued) ──
    static int remote_registered(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        const char* service = nullptr;
        if (sd_bus_message_read(m, "s", &service) < 0 || !service)
            return 0;
        std::string bus, path;
        if (parse_sni_service(service, "", bus, path))
            t.add_item(bus, path, true);
        return 0;
    }

    static int remote_unregistered(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        const char* service = nullptr;
        if (sd_bus_message_read(m, "s", &service) < 0 || !service)
            return 0;
        std::string bus, path;
        if (parse_sni_service(service, "", bus, path))
            if (item_rec* r = t.find_service(bus + path))
                t.remove_item(r, false);
        return 0;
    }

    static int remote_items(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        t.remote_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(t.remote_call_));
        if (sd_bus_message_is_method_error(m, nullptr))
            return 0;
        if (sd_bus_message_enter_container(m, 'v', "as") < 0 ||
            sd_bus_message_enter_container(m, 'a', "s") < 0)
            return 0;
        const char* s = nullptr;
        while (sd_bus_message_read(m, "s", &s) > 0 && s) {
            std::string bus, path;
            if (parse_sni_service(s, "", bus, path))
                t.add_item(bus, path, true);
        }
        return 0;
    }

    static int remote_owner(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || t.watcher_owned_)
            return 0;
        if (new_owner && *new_owner && t.remote_) {
            // Another watcher replaced the one we followed: register with it.
            t.go_remote();
        }
        return 0;
    }

    // ── Menus ──
    static int got_layout(sd_bus_message* m, void* data, sd_bus_error*) {
        auto& t = self(data);
        const unsigned key = t.layout_key_;
        t.layout_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(t.layout_call_));
        const layout_fn fn = t.layout_fn_;
        void* user = t.layout_user_;
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                             "tray: menu of item {}: {}", key,
                                             e && e->name ? e->name : "error");
            return 0;
        }
        menu_entry root{};
        unsigned revision = 0;
        if (!parse_dbusmenu_layout(m, root, &revision)) {
            lx::trace::logger::global().logf(lx::trace::level::warn, k_tray_category,
                                             "tray: menu of item {}: malformed layout", key);
            return 0;
        }
        if (tray_debug())
            lx::trace::logger::global().logf(lx::trace::level::debug, k_tray_category,
                                             "tray: menu of item {} revision {} items {}", key,
                                             revision, root.children.size());
        if (fn)
            fn(key, std::move(root.children), user);
        return 0;
    }

    static int menu_changed(sd_bus_message*, void* data, sd_bus_error*) {
        auto& t = self(data);
        // LayoutUpdated or ItemsPropertiesUpdated: the whole layout again (one call).
        if (t.menu_key_ && t.menu_fn_)
            (void)t.fetch_layout(t.menu_key_, t.menu_fn_, t.menu_user_);
        return 0;
    }

    static int ignore_reply(sd_bus_message* m, void*, sd_bus_error*) {
        if (tray_debug() && sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            lx::trace::logger::global().logf(lx::trace::level::debug, k_tray_category,
                                             "tray: call failed: {}", e && e->name ? e->name : "?");
        }
        return 0;
    }
};

namespace {

const sd_bus_vtable k_watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("RegisterStatusNotifierItem", SD_BUS_ARGS("s", service),
                            SD_BUS_NO_RESULT, lx::shell::tray_service::events::register_item,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("RegisterStatusNotifierHost", SD_BUS_ARGS("s", service),
                            SD_BUS_NO_RESULT, lx::shell::tray_service::events::register_host,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as",
                    lx::shell::tray_service::events::prop_items, 0, 0),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
                    lx::shell::tray_service::events::prop_host, 0, 0),
    SD_BUS_PROPERTY("ProtocolVersion", "i", lx::shell::tray_service::events::prop_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL_WITH_ARGS("StatusNotifierItemRegistered", SD_BUS_ARGS("s", service), 0),
    SD_BUS_SIGNAL_WITH_ARGS("StatusNotifierItemUnregistered", SD_BUS_ARGS("s", service), 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
    SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
    SD_BUS_VTABLE_END,
};

} // namespace

lx::shell::tray_service::~tray_service() { detach(); }

bool lx::shell::tray_service::attach_raw(void* b) {
    detach();
    sd_bus_slot* slot = nullptr;
    const int r = sd_bus_add_object_vtable(static_cast<sd_bus*>(b), &slot, k_watcher_path,
                                           k_watcher_iface, k_watcher_vtable, this);
    if (r < 0) {
        lx::trace::logger::global().logf(lx::trace::level::warn, k_tray_category,
                                         "tray: cannot export {}: {}", k_watcher_path,
                                         std::strerror(-r));
        return false;
    }
    bus_ = b;
    watcher_slot_ = slot;
    std::snprintf(host_name_, sizeof(host_name_), "org.kde.StatusNotifierHost-%d",
                  static_cast<int>(::getpid()));
    // This shell is a host (the panel) whichever watcher there is.
    (void)registry_.add_host(host_name_);
    return true;
}

bool lx::shell::tray_service::attach(user_bus& bus, bool request_names) {
    if (!bus.is_open() || !attach_raw(bus.bus()))
        return false;
    ubus_ = &bus;
    if (request_names) {
        (void)bus.request_name(
            k_watcher_name,
            [](const char*, user_bus::name_state st, void* user) {
                static_cast<tray_service*>(user)->watcher_name(st);
            },
            this);
        (void)bus.request_name(host_name_, nullptr, nullptr);
    }
    return true;
}

void lx::shell::tray_service::detach() {
    for (auto& r : items_) {
        r->signal_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->signal_slot));
        r->owner_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->owner_slot));
        r->fetch_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->fetch_slot));
    }
    items_.clear();
    for (auto& s : remote_slot_)
        s = sd_bus_slot_unref(static_cast<sd_bus_slot*>(s));
    remote_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(remote_call_));
    menu_slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(menu_slot_));
    layout_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(layout_call_));
    watcher_slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(watcher_slot_));
    if (ubus_ && ubus_->is_open()) {
        ubus_->release_name(k_watcher_name);
        ubus_->release_name(host_name_);
    }
    ubus_ = nullptr;
    bus_ = nullptr;
    registry_ = {};
    watcher_owned_ = remote_ = false;
    menu_key_ = 0;
    ++rev_;
}

lx::shell::tray_service::item_rec* lx::shell::tray_service::find_rec(unsigned key) {
    for (auto& r : items_)
        if (r->key == key)
            return r.get();
    return nullptr;
}

const lx::shell::tray_item* lx::shell::tray_service::find(unsigned key) const {
    for (const auto& r : items_)
        if (r->key == key)
            return r.get();
    return nullptr;
}

lx::shell::tray_service::item_rec* lx::shell::tray_service::find_service(std::string_view service) {
    for (auto& r : items_)
        if (r->service == service)
            return r.get();
    return nullptr;
}

void lx::shell::tray_service::add_item(const std::string& bus, const std::string& path, bool) {
    if (!bus_ || find_service(bus + path) || items_.size() >= k_max_tray_items)
        return;
    auto rec = std::make_unique<item_rec>();
    rec->owner = this;
    rec->key = next_key_++;
    rec->bus = bus;
    rec->path = path;
    rec->service = bus + path;
    auto* b = static_cast<sd_bus*>(bus_);
    char match[600];
    std::snprintf(match, sizeof(match),
                  "type='signal',sender='%s',path='%s',interface='org.kde.StatusNotifierItem'",
                  bus.c_str(), path.c_str());
    sd_bus_slot* slot = nullptr;
    if (sd_bus_add_match_async(b, &slot, match, events::item_signal, nullptr, rec.get()) >= 0)
        rec->signal_slot = slot;
    std::snprintf(match, sizeof(match),
                  "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
                  bus.c_str());
    slot = nullptr;
    if (sd_bus_add_match_async(b, &slot, match, events::item_owner, nullptr, rec.get()) >= 0)
        rec->owner_slot = slot;
    item_rec& r = *rec;
    items_.push_back(std::move(rec));
    fetch(r);
}

void lx::shell::tray_service::remove_item(item_rec* r, bool announce) {
    if (!r)
        return;
    if (announce && registry_.remove_item(r->service) && bus_ && !remote_)
        (void)sd_bus_emit_signal(static_cast<sd_bus*>(bus_), k_watcher_path, k_watcher_iface,
                                 "StatusNotifierItemUnregistered", "s", r->service.c_str());
    if (menu_key_ == r->key)
        set_menu_shown(0);
    r->signal_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->signal_slot));
    r->owner_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->owner_slot));
    r->fetch_slot = sd_bus_slot_unref(static_cast<sd_bus_slot*>(r->fetch_slot));
    std::erase_if(items_, [r](const std::unique_ptr<item_rec>& p) { return p.get() == r; });
    ++rev_;
}

void lx::shell::tray_service::fetch(item_rec& r) {
    if (r.fetch_slot) {
        r.refresh = true;
        return;
    }
    sd_bus_slot* slot = nullptr;
    if (sd_bus_call_method_async(static_cast<sd_bus*>(bus_), &slot, r.bus.c_str(), r.path.c_str(),
                                 "org.freedesktop.DBus.Properties", "GetAll", events::got_all, &r,
                                 "s", k_item_iface) >= 0)
        r.fetch_slot = slot;
}

void lx::shell::tray_service::after_dispatch() {
    for (auto& r : items_)
        if (r->refresh && !r->fetch_slot) {
            r->refresh = false;
            fetch(*r);
        }
}

void lx::shell::tray_service::watcher_name(user_bus::name_state st) {
    switch (st) {
    case user_bus::name_state::owned: go_local(); break;
    case user_bus::name_state::queued:
    case user_bus::name_state::lost: go_remote(); break;
    default: break;
    }
}

void lx::shell::tray_service::go_local() {
    for (auto& s : remote_slot_)
        s = sd_bus_slot_unref(static_cast<sd_bus_slot*>(s));
    remote_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(remote_call_));
    if (remote_) {
        // Items followed through the other watcher register with this one now.
        while (!items_.empty())
            remove_item(items_.back().get(), false);
    }
    remote_ = false;
    watcher_owned_ = true;
    (void)sd_bus_emit_signal(static_cast<sd_bus*>(bus_), k_watcher_path, k_watcher_iface,
                             "StatusNotifierHostRegistered", "");
    lx::trace::logger::global().log(lx::trace::level::info, k_tray_category,
                                    "tray: watcher and host (org.kde.StatusNotifierWatcher)");
}

void lx::shell::tray_service::go_remote() {
    auto* b = static_cast<sd_bus*>(bus_);
    if (!b)
        return;
    watcher_owned_ = false;
    for (auto& s : remote_slot_)
        s = sd_bus_slot_unref(static_cast<sd_bus_slot*>(s));
    remote_ = true;
    sd_bus_slot* slot = nullptr;
    if (sd_bus_match_signal_async(b, &slot, k_watcher_name, k_watcher_path, k_watcher_iface,
                                  "StatusNotifierItemRegistered", events::remote_registered,
                                  nullptr, this) >= 0)
        remote_slot_[0] = slot;
    slot = nullptr;
    if (sd_bus_match_signal_async(b, &slot, k_watcher_name, k_watcher_path, k_watcher_iface,
                                  "StatusNotifierItemUnregistered", events::remote_unregistered,
                                  nullptr, this) >= 0)
        remote_slot_[1] = slot;
    char match[256];
    std::snprintf(match, sizeof(match),
                  "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
                  k_watcher_name);
    slot = nullptr;
    if (sd_bus_add_match_async(b, &slot, match, events::remote_owner, nullptr, this) >= 0)
        remote_slot_[2] = slot;
    (void)sd_bus_call_method_async(b, nullptr, k_watcher_name, k_watcher_path, k_watcher_iface,
                                   "RegisterStatusNotifierHost", events::ignore_reply, nullptr, "s",
                                   host_name_);
    remote_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(remote_call_));
    slot = nullptr;
    if (sd_bus_call_method_async(b, &slot, k_watcher_name, k_watcher_path,
                                 "org.freedesktop.DBus.Properties", "Get", events::remote_items,
                                 this, "ss", k_watcher_iface, "RegisteredStatusNotifierItems") >= 0)
        remote_call_ = slot;
    lx::trace::logger::global().log(lx::trace::level::info, k_tray_category,
                                    "tray: another StatusNotifierWatcher runs: host only");
}

void lx::shell::tray_service::activate(unsigned key, int x, int y, const char* token,
                                       bool secondary) {
    item_rec* r = find_rec(key);
    if (!r || !bus_)
        return;
    auto* b = static_cast<sd_bus*>(bus_);
    if (token && *token)
        (void)sd_bus_call_method_async(b, nullptr, r->bus.c_str(), r->path.c_str(), k_item_iface,
                                       "ProvideXdgActivationToken", events::ignore_reply, nullptr,
                                       "s", token);
    (void)sd_bus_call_method_async(b, nullptr, r->bus.c_str(), r->path.c_str(), k_item_iface,
                                   secondary ? "SecondaryActivate" : "Activate",
                                   events::ignore_reply, nullptr, "ii", x, y);
    lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                     "tray: {} {} x={} y={} token={}", r->service,
                                     secondary ? "SecondaryActivate" : "Activate", x, y,
                                     token && *token ? "yes" : "no");
}

void lx::shell::tray_service::context_menu(unsigned key, int x, int y) {
    item_rec* r = find_rec(key);
    if (!r || !bus_)
        return;
    (void)sd_bus_call_method_async(static_cast<sd_bus*>(bus_), nullptr, r->bus.c_str(),
                                   r->path.c_str(), k_item_iface, "ContextMenu",
                                   events::ignore_reply, nullptr, "ii", x, y);
    lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                     "tray: {} ContextMenu x={} y={}", r->service, x, y);
}

void lx::shell::tray_service::scroll(unsigned key, int delta, bool horizontal) {
    item_rec* r = find_rec(key);
    if (!r || !bus_)
        return;
    (void)sd_bus_call_method_async(static_cast<sd_bus*>(bus_), nullptr, r->bus.c_str(),
                                   r->path.c_str(), k_item_iface, "Scroll", events::ignore_reply,
                                   nullptr, "is", delta, horizontal ? "horizontal" : "vertical");
}

bool lx::shell::tray_service::fetch_layout(unsigned key, layout_fn fn, void* user) {
    item_rec* r = find_rec(key);
    if (!r || !bus_ || !r->has_menu())
        return false;
    auto* b = static_cast<sd_bus*>(bus_);
    sd_bus_message* call = nullptr;
    if (sd_bus_message_new_method_call(b, &call, r->bus.c_str(), r->menu_path.c_str(), k_menu_iface,
                                       "GetLayout") < 0)
        return false;
    int rc = sd_bus_message_append(call, "ii", 0, -1);
    if (rc >= 0)
        rc = sd_bus_message_append(call, "as", 9, "label", "enabled", "visible", "type",
                                   "toggle-type", "toggle-state", "icon-name", "children-display",
                                   "shortcut");
    layout_call_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(layout_call_));
    sd_bus_slot* slot = nullptr;
    if (rc >= 0)
        rc = sd_bus_call_async(b, &slot, call, events::got_layout, this, 2'000'000);
    sd_bus_message_unref(call);
    if (rc < 0)
        return false;
    layout_call_ = slot;
    layout_key_ = key;
    layout_fn_ = fn;
    layout_user_ = user;
    return true;
}

bool lx::shell::tray_service::fetch_menu(unsigned key, layout_fn fn, void* user) {
    item_rec* r = find_rec(key);
    if (!r || !bus_ || !r->has_menu())
        return false;
    // "AboutToShow(0)": the application may update the menu before it is read.
    (void)sd_bus_call_method_async(static_cast<sd_bus*>(bus_), nullptr, r->bus.c_str(),
                                   r->menu_path.c_str(), k_menu_iface, "AboutToShow",
                                   events::ignore_reply, nullptr, "i", 0);
    return fetch_layout(key, fn, user);
}

void lx::shell::tray_service::set_menu_shown(unsigned key) {
    menu_slot_ = sd_bus_slot_unref(static_cast<sd_bus_slot*>(menu_slot_));
    menu_key_ = 0;
    if (!key) {
        menu_fn_ = nullptr;
        return;
    }
    item_rec* r = find_rec(key);
    if (!r || !bus_ || !r->has_menu())
        return;
    char match[600];
    std::snprintf(match, sizeof(match),
                  "type='signal',sender='%s',path='%s',interface='com.canonical.dbusmenu'",
                  r->bus.c_str(), r->menu_path.c_str());
    sd_bus_slot* slot = nullptr;
    if (sd_bus_add_match_async(static_cast<sd_bus*>(bus_), &slot, match, events::menu_changed,
                               nullptr, this) >= 0)
        menu_slot_ = slot;
    menu_key_ = key;
    menu_fn_ = layout_fn_;
    menu_user_ = layout_user_;
}

void lx::shell::tray_service::menu_event(unsigned key, int id, const char* type) {
    item_rec* r = find_rec(key);
    if (!r || !bus_ || !r->has_menu())
        return;
    auto* b = static_cast<sd_bus*>(bus_);
    sd_bus_message* call = nullptr;
    if (sd_bus_message_new_method_call(b, &call, r->bus.c_str(), r->menu_path.c_str(), k_menu_iface,
                                       "Event") < 0)
        return;
    const std::uint32_t stamp = static_cast<std::uint32_t>(::time(nullptr));
    if (sd_bus_message_append(call, "isvu", id, type, "i", 0, stamp) >= 0)
        (void)sd_bus_call_async(b, nullptr, call, events::ignore_reply, nullptr, 0);
    sd_bus_message_unref(call);
    if (std::strcmp(type, "clicked") == 0)
        lx::trace::logger::global().logf(lx::trace::level::info, k_tray_category,
                                         "tray: {} menu Event id={} clicked", r->service, id);
}

void lx::shell::tray_service::menu_about_to_show(unsigned key, int id) {
    item_rec* r = find_rec(key);
    if (!r || !bus_ || !r->has_menu())
        return;
    (void)sd_bus_call_method_async(static_cast<sd_bus*>(bus_), nullptr, r->bus.c_str(),
                                   r->menu_path.c_str(), k_menu_iface, "AboutToShow",
                                   events::ignore_reply, nullptr, "i", id);
}

#else // !LUMEN_HAS_LOGIND

struct lx::shell::tray_service::events {};
bool lx::shell::parse_dbusmenu_layout(void*, menu_entry&, unsigned*) { return false; }
lx::shell::tray_service::~tray_service() = default;
bool lx::shell::tray_service::attach(user_bus&, bool) { return false; }
bool lx::shell::tray_service::attach_raw(void*) { return false; }
void lx::shell::tray_service::detach() {}
const lx::shell::tray_item* lx::shell::tray_service::find(unsigned) const { return nullptr; }
lx::shell::tray_service::item_rec* lx::shell::tray_service::find_rec(unsigned) { return nullptr; }
lx::shell::tray_service::item_rec* lx::shell::tray_service::find_service(std::string_view) {
    return nullptr;
}
void lx::shell::tray_service::add_item(const std::string&, const std::string&, bool) {}
void lx::shell::tray_service::remove_item(item_rec*, bool) {}
void lx::shell::tray_service::fetch(item_rec&) {}
void lx::shell::tray_service::after_dispatch() {}
void lx::shell::tray_service::watcher_name(user_bus::name_state) {}
void lx::shell::tray_service::go_local() {}
void lx::shell::tray_service::go_remote() {}
void lx::shell::tray_service::activate(unsigned, int, int, const char*, bool) {}
void lx::shell::tray_service::context_menu(unsigned, int, int) {}
void lx::shell::tray_service::scroll(unsigned, int, bool) {}
bool lx::shell::tray_service::fetch_layout(unsigned, layout_fn, void*) { return false; }
bool lx::shell::tray_service::fetch_menu(unsigned, layout_fn, void*) { return false; }
void lx::shell::tray_service::set_menu_shown(unsigned) {}
void lx::shell::tray_service::menu_event(unsigned, int, const char*) {}
void lx::shell::tray_service::menu_about_to_show(unsigned, int) {}

#endif
