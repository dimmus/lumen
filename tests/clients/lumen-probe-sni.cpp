// lumen-probe sni: a StatusNotifierItem with a com.canonical.dbusmenu menu, on sd-bus, for
// the tray checks (design-session-and-wm T.3 N6, N7; the user decided against Python test
// helpers: this mode keeps the test in the repository). No Wayland connection. main()
// dispatches here.
//
//   lumen-probe sni [--id ID] [--title T] [--icon-name NAME | --pixmap] [--status S]
//                   [--tooltip TEXT] [--no-menu] [--item-is-menu] [--register-path]
//                   [--change-after MS] [--exit-after S]
//
// It owns org.kde.StatusNotifierItem-<pid>-1 (or, with --register-path, registers its object
// path the way libappindicator does), exports /StatusNotifierItem and /MenuBar, registers
// with org.kde.StatusNotifierWatcher and registers again whenever that name gets a new owner
// (a restarted shell). Every event is one stdout line:
//
//   sni name=<bus name> unique=<:1.n>          sni no watcher (waiting)
//   sni registered watcher=<:1.n>              sni watcher gone
//   sni token <token>                          (ProvideXdgActivationToken)
//   sni activate x=X y=Y token=<token|->       sni secondary-activate x=X y=Y token=<…>
//   sni context-menu x=X y=Y                   sni scroll delta=D orientation=O
//   sni menu about-to-show id=N                sni menu get-layout parent=P depth=D
//   sni menu event id=N type=T                 sni menu clicked id=N label=L
//   sni changed status=S                       (--change-after)
//
// The menu: _Open (1), _Toggle (2, a checkmark, on), _More ▸ { _Sub item (5) }, a separator
// (3), _Quit (6) — Quit only logs (the probe exits on its timer or SIGTERM).

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#endif

int sni_main(int argc, char** argv);

#if defined(LUMEN_HAS_LOGIND)

namespace {

volatile std::sig_atomic_t g_stop = 0;

struct item_state {
    sd_bus* bus = nullptr;
    std::string name;
    std::string id = "lumen-probe";
    std::string title = "Lumen probe";
    std::string icon_name = "dialog-information";
    std::string status = "Active";
    std::string tooltip = "A tray item from lumen-probe";
    std::string last_token;
    bool pixmap = false;
    bool menu = true;
    bool item_is_menu = false;
    bool register_path = false;
    bool toggled = true;
    unsigned revision = 1;
};

item_state g;

void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::putchar('\n');
    std::fflush(stdout);
}

// ── org.kde.StatusNotifierItem ──

int prop_string(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
                void*, sd_bus_error*) {
    const std::string p{property};
    const char* v = "";
    if (p == "Category")
        v = "ApplicationStatus";
    else if (p == "Id")
        v = g.id.c_str();
    else if (p == "Title")
        v = g.title.c_str();
    else if (p == "Status")
        v = g.status.c_str();
    else if (p == "IconName")
        v = g.pixmap ? "" : g.icon_name.c_str();
    else if (p == "AttentionIconName")
        v = g.pixmap ? "" : "dialog-warning";
    return sd_bus_message_append(reply, "s", v);
}

int prop_empty_string(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                      sd_bus_error*) {
    return sd_bus_message_append(reply, "s", "");
}

/// A filled disc, orange (attention: red), ARGB32 in network byte order.
int append_pixmaps(sd_bus_message* reply, bool attention) {
    int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    for (const int size : {22, 32}) {
        std::vector<unsigned char> px(static_cast<std::size_t>(size * size * 4), 0);
        const double c = (size - 1) / 2.0, rad = size / 2.0 - 1.0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const double dx = x - c, dy = y - c;
                if (dx * dx + dy * dy > rad * rad)
                    continue;
                unsigned char* p = px.data() + (y * size + x) * 4;
                p[0] = 0xFF;
                p[1] = attention ? 0xE0 : 0xF0;
                p[2] = attention ? 0x31 : 0x90;
                p[3] = attention ? 0x2B : 0x20;
            }
        if (r >= 0)
            r = sd_bus_message_open_container(reply, 'r', "iiay");
        if (r >= 0)
            r = sd_bus_message_append(reply, "ii", size, size);
        if (r >= 0)
            r = sd_bus_message_append_array(reply, 'y', px.data(), px.size());
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
    }
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    return r;
}

int prop_pixmap(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
                void*, sd_bus_error*) {
    if (!g.pixmap || std::strcmp(property, "OverlayIconPixmap") == 0) {
        int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
        return r >= 0 ? sd_bus_message_close_container(reply) : r;
    }
    return append_pixmaps(reply, std::strcmp(property, "AttentionIconPixmap") == 0);
}

int prop_tooltip(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                 sd_bus_error*) {
    int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
    if (r >= 0)
        r = sd_bus_message_append(reply, "s", "");
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_message_append(reply, "ss", g.title.c_str(), g.tooltip.c_str());
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    return r;
}

int prop_bool(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
              sd_bus_error*) {
    return sd_bus_message_append(reply, "b", g.item_is_menu ? 1 : 0);
}

int prop_menu(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
              sd_bus_error*) {
    return sd_bus_message_append(reply, "o", g.menu ? "/MenuBar" : "/");
}

int prop_window(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                sd_bus_error*) {
    return sd_bus_message_append(reply, "i", 0);
}

int m_activate(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t x = 0, y = 0;
    const int r = sd_bus_message_read(m, "ii", &x, &y);
    if (r < 0)
        return r;
    const bool secondary = std::strcmp(sd_bus_message_get_member(m), "SecondaryActivate") == 0;
    line("sni %s x=%d y=%d token=%s", secondary ? "secondary-activate" : "activate", x, y,
         g.last_token.empty() ? "-" : g.last_token.c_str());
    g.last_token.clear();
    return sd_bus_reply_method_return(m, "");
}

int m_context_menu(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t x = 0, y = 0;
    const int r = sd_bus_message_read(m, "ii", &x, &y);
    if (r < 0)
        return r;
    line("sni context-menu x=%d y=%d", x, y);
    return sd_bus_reply_method_return(m, "");
}

int m_scroll(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t delta = 0;
    const char* orientation = nullptr;
    const int r = sd_bus_message_read(m, "is", &delta, &orientation);
    if (r < 0)
        return r;
    line("sni scroll delta=%d orientation=%s", delta, orientation ? orientation : "?");
    return sd_bus_reply_method_return(m, "");
}

int m_token(sd_bus_message* m, void*, sd_bus_error*) {
    const char* token = nullptr;
    const int r = sd_bus_message_read(m, "s", &token);
    if (r < 0)
        return r;
    g.last_token = token ? token : "";
    line("sni token %s", g.last_token.c_str());
    return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable k_item_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", prop_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", prop_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", prop_string, 0, 0),
    SD_BUS_PROPERTY("Status", "s", prop_string, 0, 0),
    SD_BUS_PROPERTY("WindowId", "i", prop_window, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconName", "s", prop_string, 0, 0),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("OverlayIconName", "s", prop_empty_string, 0, 0),
    SD_BUS_PROPERTY("OverlayIconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("AttentionIconName", "s", prop_string, 0, 0),
    SD_BUS_PROPERTY("AttentionIconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("AttentionMovieName", "s", prop_empty_string, 0, 0),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", prop_tooltip, 0, 0),
    SD_BUS_PROPERTY("ItemIsMenu", "b", prop_bool, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", prop_menu, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "s", prop_empty_string, 0, 0),
    SD_BUS_METHOD("ContextMenu", "ii", "", m_context_menu, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Activate", "ii", "", m_activate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", m_activate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Scroll", "is", "", m_scroll, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ProvideXdgActivationToken", "s", "", m_token, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NewTitle", "", 0),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewAttentionIcon", "", 0),
    SD_BUS_SIGNAL("NewOverlayIcon", "", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_VTABLE_END,
};

// ── com.canonical.dbusmenu ──

struct menu_item {
    int id;
    const char* label;
    const char* type;        // "standard" | "separator"
    const char* toggle_type; // "" | "checkmark"
    int parent;
};

const menu_item k_menu[] = {
    {1, "_Open", "standard", "", 0},
    {2, "_Toggle", "standard", "checkmark", 0},
    {4, "_More", "standard", "", 0},
    {5, "_Sub item", "standard", "", 4},
    {3, "", "separator", "", 0},
    {6, "_Quit", "standard", "", 0},
};

const char* label_of(int id) {
    for (const auto& it : k_menu)
        if (it.id == id)
            return it.label;
    return "?";
}

bool has_children(int id) {
    for (const auto& it : k_menu)
        if (it.parent == id)
            return true;
    return false;
}

int append_item(sd_bus_message* reply, int id, int depth) {
    int r = sd_bus_message_open_container(reply, 'r', "ia{sv}av");
    if (r >= 0)
        r = sd_bus_message_append(reply, "i", id);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "{sv}");
    const menu_item* item = nullptr;
    for (const auto& it : k_menu)
        if (it.id == id)
            item = &it;
    if (item && r >= 0) {
        if (std::strcmp(item->type, "separator") == 0)
            r = sd_bus_message_append(reply, "{sv}", "type", "s", "separator");
        else
            r = sd_bus_message_append(reply, "{sv}", "label", "s", item->label);
        if (r >= 0 && item->toggle_type[0]) {
            r = sd_bus_message_append(reply, "{sv}", "toggle-type", "s", item->toggle_type);
            if (r >= 0)
                r = sd_bus_message_append(reply, "{sv}", "toggle-state", "i", g.toggled ? 1 : 0);
        }
        if (r >= 0 && id == 6)
            r = sd_bus_message_append(reply, "{sv}", "icon-name", "s", "application-exit");
        if (r >= 0 && id == 6) {
            // shortcut aas: [["Control", "q"]]
            r = sd_bus_message_open_container(reply, 'e', "sv");
            if (r >= 0)
                r = sd_bus_message_append(reply, "s", "shortcut");
            if (r >= 0)
                r = sd_bus_message_open_container(reply, 'v', "aas");
            if (r >= 0)
                r = sd_bus_message_open_container(reply, 'a', "as");
            if (r >= 0)
                r = sd_bus_message_append(reply, "as", 2, "Control", "q");
            if (r >= 0)
                r = sd_bus_message_close_container(reply);
            if (r >= 0)
                r = sd_bus_message_close_container(reply);
            if (r >= 0)
                r = sd_bus_message_close_container(reply);
        }
    }
    if ((id == 0 || has_children(id)) && r >= 0)
        r = sd_bus_message_append(reply, "{sv}", "children-display", "s", "submenu");
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "v");
    if (depth != 0)
        for (const auto& it : k_menu) {
            if (it.parent != id || r < 0)
                continue;
            r = sd_bus_message_open_container(reply, 'v', "(ia{sv}av)");
            if (r >= 0)
                r = append_item(reply, it.id, depth < 0 ? -1 : depth - 1);
            if (r >= 0)
                r = sd_bus_message_close_container(reply);
        }
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    return r;
}

int m_get_layout(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t parent = 0, depth = -1;
    int r = sd_bus_message_read(m, "ii", &parent, &depth);
    if (r < 0)
        return r;
    r = sd_bus_message_skip(m, "as");
    if (r < 0)
        return r;
    line("sni menu get-layout parent=%d depth=%d", parent, depth);
    sd_bus_message* reply = nullptr;
    r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0)
        r = sd_bus_message_append(reply, "u", g.revision);
    if (r >= 0)
        r = append_item(reply, parent, depth);
    if (r >= 0)
        r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int m_event(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t id = 0;
    const char* type = nullptr;
    int r = sd_bus_message_read(m, "is", &id, &type);
    if (r < 0)
        return r;
    r = sd_bus_message_skip(m, "vu");
    if (r < 0)
        return r;
    line("sni menu event id=%d type=%s", id, type ? type : "?");
    if (type && std::strcmp(type, "clicked") == 0) {
        line("sni menu clicked id=%d label=%s", id, label_of(id));
        if (id == 2) {
            g.toggled = !g.toggled;
            ++g.revision;
            (void)sd_bus_emit_signal(g.bus, "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated",
                                     "ui", g.revision, 0);
        }
    }
    return sd_bus_reply_method_return(m, "");
}

int m_event_group(sd_bus_message* m, void*, sd_bus_error*) {
    int r = sd_bus_message_enter_container(m, 'a', "(isvu)");
    if (r < 0)
        return r;
    for (;;) {
        r = sd_bus_message_enter_container(m, 'r', "isvu");
        if (r <= 0)
            break;
        std::int32_t id = 0;
        const char* type = nullptr;
        if (sd_bus_message_read(m, "is", &id, &type) >= 0)
            line("sni menu event id=%d type=%s", id, type ? type : "?");
        (void)sd_bus_message_skip(m, "vu");
        (void)sd_bus_message_exit_container(m);
    }
    (void)sd_bus_message_exit_container(m);
    return sd_bus_reply_method_return(m, "ai", 0);
}

int m_about_to_show(sd_bus_message* m, void*, sd_bus_error*) {
    std::int32_t id = 0;
    const int r = sd_bus_message_read(m, "i", &id);
    if (r < 0)
        return r;
    line("sni menu about-to-show id=%d", id);
    return sd_bus_reply_method_return(m, "b", 0);
}

int m_group_properties(sd_bus_message* m, void*, sd_bus_error*) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int prop_menu_version(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                      sd_bus_error*) {
    return sd_bus_message_append(reply, "u", 3);
}

int prop_menu_string(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
                     void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "s", std::strcmp(property, "TextDirection") == 0 ? "ltr"
                                                                                       : "normal");
}

int prop_menu_paths(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                    sd_bus_error*) {
    return sd_bus_message_append(reply, "as", 0);
}

const sd_bus_vtable k_menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", prop_menu_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", prop_menu_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", prop_menu_string, 0, 0),
    SD_BUS_PROPERTY("IconThemePath", "as", prop_menu_paths, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", m_get_layout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", m_group_properties,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", m_event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "ai", m_event_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShow", "i", "b", m_about_to_show, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemActivationRequested", "iu", 0),
    SD_BUS_VTABLE_END,
};

// ── The watcher ──

int registered(sd_bus_message* m, void*, sd_bus_error*) {
    if (sd_bus_message_is_method_error(m, nullptr)) {
        const sd_bus_error* e = sd_bus_message_get_error(m);
        if (e && e->name && std::strstr(e->name, "ServiceUnknown"))
            line("sni no watcher (waiting)");
        else
            line("sni register failed %s", e && e->name ? e->name : "?");
        return 0;
    }
    const char* owner = sd_bus_message_get_sender(m);
    line("sni registered watcher=%s", owner ? owner : "?");
    return 0;
}

void register_item() {
    const char* service = g.register_path ? "/StatusNotifierItem" : g.name.c_str();
    (void)sd_bus_call_method_async(g.bus, nullptr, "org.kde.StatusNotifierWatcher",
                                   "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                                   "RegisterStatusNotifierItem", registered, nullptr, "s", service);
}

int watcher_owner(sd_bus_message* m, void*, sd_bus_error*) {
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
        return 0;
    if (new_owner && *new_owner)
        register_item(); // a new watcher (the shell restarted): register again
    else
        line("sni watcher gone");
    return 0;
}

void on_signal(int) { g_stop = 1; }

long long now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

int sni_main(int argc, char** argv) {
    long long change_after = -1;
    double exit_after = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--id")
            g.id = next();
        else if (a == "--title")
            g.title = next();
        else if (a == "--icon-name")
            g.icon_name = next();
        else if (a == "--pixmap")
            g.pixmap = true;
        else if (a == "--status")
            g.status = next();
        else if (a == "--tooltip")
            g.tooltip = next();
        else if (a == "--no-menu")
            g.menu = false;
        else if (a == "--item-is-menu")
            g.item_is_menu = true;
        else if (a == "--register-path")
            g.register_path = true;
        else if (a == "--change-after")
            change_after = std::atoll(next());
        else if (a == "--exit-after")
            exit_after = std::atof(next());
        else {
            std::fprintf(stderr, "lumen-probe sni: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    sd_bus* bus = nullptr;
    int r = sd_bus_open_user_with_description(&bus, "lumen-probe-sni");
    if (r < 0) {
        std::fprintf(stderr, "lumen-probe sni: no user bus: %s\n", std::strerror(-r));
        return 1;
    }
    g.bus = bus;
    sd_bus_slot* item_slot = nullptr;
    sd_bus_slot* menu_slot = nullptr;
    r = sd_bus_add_object_vtable(bus, &item_slot, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                                 k_item_vtable, nullptr);
    if (r >= 0 && g.menu)
        r = sd_bus_add_object_vtable(bus, &menu_slot, "/MenuBar", "com.canonical.dbusmenu",
                                     k_menu_vtable, nullptr);
    if (r < 0) {
        std::fprintf(stderr, "lumen-probe sni: cannot export: %s\n", std::strerror(-r));
        return 1;
    }
    const char* unique = nullptr;
    (void)sd_bus_get_unique_name(bus, &unique);
    if (!g.register_path) {
        char name[96];
        std::snprintf(name, sizeof(name), "org.kde.StatusNotifierItem-%d-1", static_cast<int>(getpid()));
        g.name = name;
        r = sd_bus_request_name(bus, name, 0);
        if (r < 0) {
            std::fprintf(stderr, "lumen-probe sni: cannot own %s: %s\n", name, std::strerror(-r));
            return 1;
        }
    } else {
        g.name = unique ? unique : "";
    }
    line("sni name=%s unique=%s", g.name.c_str(), unique ? unique : "?");
    sd_bus_slot* owner_slot = nullptr;
    (void)sd_bus_add_match(bus, &owner_slot,
                           "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                           "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                           "arg0='org.kde.StatusNotifierWatcher'",
                           watcher_owner, nullptr);
    register_item();
    const long long start = now_ms();
    bool changed = false;
    while (!g_stop) {
        for (;;) {
            r = sd_bus_process(bus, nullptr);
            if (r <= 0)
                break;
        }
        if (r < 0) {
            std::fprintf(stderr, "lumen-probe sni: bus: %s\n", std::strerror(-r));
            break;
        }
        const long long now = now_ms();
        if (exit_after > 0 && now - start >= static_cast<long long>(exit_after * 1000.0))
            break;
        if (change_after >= 0 && !changed && now - start >= change_after) {
            changed = true;
            g.status = g.status == "NeedsAttention" ? "Active" : "NeedsAttention";
            (void)sd_bus_emit_signal(bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                                     "NewStatus", "s", g.status.c_str());
            (void)sd_bus_emit_signal(bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                                     "NewIcon", "");
            line("sni changed status=%s", g.status.c_str());
        }
        long long wait_ms = 1000;
        if (exit_after > 0)
            wait_ms = std::min(wait_ms, static_cast<long long>(exit_after * 1000.0) - (now - start));
        if (change_after >= 0 && !changed)
            wait_ms = std::min(wait_ms, change_after - (now - start));
        if (wait_ms < 0)
            wait_ms = 0;
        r = sd_bus_wait(bus, static_cast<std::uint64_t>(wait_ms) * 1000u);
        if (r < 0 && r != -EINTR) {
            std::fprintf(stderr, "lumen-probe sni: wait: %s\n", std::strerror(-r));
            break;
        }
    }
    sd_bus_slot_unref(owner_slot);
    sd_bus_slot_unref(menu_slot);
    sd_bus_slot_unref(item_slot);
    sd_bus_flush_close_unref(bus);
    line("sni exit");
    return 0;
}

#else

int sni_main(int, char**) {
    std::fprintf(stderr, "lumen-probe sni: built without libsystemd\n");
    return 2;
}

#endif
