// lumen-shell's tray (design-session-and-wm N.5, N.6): registration strings in both forms,
// the watcher's registry (dedup, an owner leaving), IconPixmap conversion (network byte
// order → premultiplied, the size chosen), dbusmenu entries and a GetLayout reply parsed
// (types, toggles, submenus, shortcuts, invisible items), menu labels and their mnemonics,
// the menu's layout; and over a socketpair the watcher's methods, properties and signals and
// the host reading an item's properties (GetAll answered by the other end).
#include "lumen_test.hpp"

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <sys/socket.h>
#endif

import std;
import lx.foundation;
import lx.shell;

namespace {

using namespace lx::shell;

} // namespace

LUMEN_TEST(service_strings) {
    std::string bus, path;
    LUMEN_CHECK(parse_sni_service("org.kde.StatusNotifierItem-12-1", ":1.5", bus, path));
    LUMEN_CHECK(bus == "org.kde.StatusNotifierItem-12-1" && path == "/StatusNotifierItem");
    // libappindicator registers its object path: the item is the caller's.
    LUMEN_CHECK(parse_sni_service("/org/ayatana/NotificationItem/x", ":1.5", bus, path));
    LUMEN_CHECK(bus == ":1.5" && path == "/org/ayatana/NotificationItem/x");
    LUMEN_CHECK(!parse_sni_service("/org/x", "", bus, path));
    // The watcher's own listing form ("bus/path").
    LUMEN_CHECK(parse_sni_service(":1.9/org/x/Item", "", bus, path) && bus == ":1.9" &&
                path == "/org/x/Item");
    LUMEN_CHECK(!parse_sni_service("", ":1.5", bus, path));
    LUMEN_CHECK(!parse_sni_service("nodots", ":1.5", bus, path));
    LUMEN_CHECK(!parse_sni_service("bad name!/x", ":1.5", bus, path));
}

LUMEN_TEST(registry) {
    sni_registry r;
    LUMEN_CHECK(r.add_item(":1.5/StatusNotifierItem"));
    LUMEN_CHECK(!r.add_item(":1.5/StatusNotifierItem"));
    LUMEN_CHECK(r.add_item(":1.5/other"));
    LUMEN_CHECK(r.add_item(":1.6/StatusNotifierItem"));
    const std::vector<std::string> gone = r.remove_bus(":1.5");
    LUMEN_CHECK(gone.size() == 2 && r.items().size() == 1 && r.items()[0] == ":1.6/StatusNotifierItem");
    LUMEN_CHECK(r.remove_item(":1.6/StatusNotifierItem") && r.items().empty());
    for (unsigned i = 0; i < k_max_tray_items; ++i)
        LUMEN_CHECK(r.add_item(":1." + std::to_string(100 + i) + "/i"));
    LUMEN_CHECK(!r.add_item(":1.999/i")); // bounded
    LUMEN_CHECK(!r.host_registered() && r.add_host("h") && !r.add_host("h") && r.host_registered());
    LUMEN_CHECK(r.remove_host("h") && !r.host_registered());
}

LUMEN_TEST(pixmaps) {
    // 2x1: opaque red, then half-transparent white (A, R, G, B bytes).
    const std::uint8_t data[] = {0xFF, 0xFF, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF};
    sni_pixmap p;
    LUMEN_CHECK(convert_sni_pixmap(2, 1, data, p));
    LUMEN_CHECK(p.argb.size() == 2 && p.argb[0] == 0xFFFF0000u && p.argb[1] == 0x80808080u);
    LUMEN_CHECK(!convert_sni_pixmap(2, 2, data, p)); // short
    LUMEN_CHECK(!convert_sni_pixmap(0, 1, data, p));
    std::vector<sni_pixmap> set(3);
    set[0].width = set[0].height = 16;
    set[1].width = set[1].height = 22;
    set[2].width = set[2].height = 48;
    LUMEN_CHECK(choose_pixmap(set, 20) == &set[1]);
    LUMEN_CHECK(choose_pixmap(set, 22) == &set[1]);
    LUMEN_CHECK(choose_pixmap(set, 16) == &set[0]);
    LUMEN_CHECK(choose_pixmap(set, 64) == &set[2]);
    LUMEN_CHECK(choose_pixmap({}, 16) == nullptr);
    // Fitted into the size asked for.
    sni_pixmap red;
    std::vector<std::uint8_t> px(22 * 22 * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = 0xFF;
        px[i + 1] = 0xFF;
    }
    LUMEN_CHECK(convert_sni_pixmap(22, 22, px, red));
    std::vector<std::uint32_t> icon;
    pixmap_to_icon(red, 16, icon);
    LUMEN_CHECK(icon.size() == 256 && icon[8 * 16 + 8] == 0xFFFF0000u);
}

LUMEN_TEST(menu_labels_and_layout) {
    int mn = -1;
    LUMEN_CHECK(menu_label("_Open", &mn) == "Open" && mn == 0);
    LUMEN_CHECK(menu_label("Save __As", &mn) == "Save _As" && mn == -1);
    LUMEN_CHECK(menu_label("E_xit", &mn) == "Exit" && mn == 1);
    dbusmenu_props p{};
    p.label = "_Toggle";
    p.toggle_type = "checkmark";
    p.toggle_state = 1;
    menu_entry e = dbusmenu_entry(7, p);
    LUMEN_CHECK(e.id == 7 && e.toggle_kind == menu_entry::toggle::check && e.toggled);
    p.toggle_type = "radio";
    p.toggle_state = 0;
    LUMEN_CHECK(dbusmenu_entry(1, p).toggle_kind == menu_entry::toggle::radio &&
                !dbusmenu_entry(1, p).toggled);
    std::vector<menu_entry> items(3);
    items[0].label = "_Open";
    items[1].separator = true;
    items[2].label = "Quit";
    menu_rows rows{};
    layout_menu(items, [](const char* s, void*) { return static_cast<int>(std::strlen(s)) * 7; },
                nullptr, rows);
    LUMEN_CHECK(rows.count == 3 && rows.width == k_menu_min_w);
    LUMEN_CHECK(rows.row[1].height == k_menu_separator_h && rows.row[2].y == rows.row[1].y + k_menu_separator_h);
    LUMEN_CHECK(rows.height == 2 * k_menu_pad + 2 * k_menu_row_h + k_menu_separator_h);
}

#if defined(LUMEN_HAS_LOGIND)

namespace {

struct peer {
    sd_bus* server = nullptr;
    sd_bus* client = nullptr;
    peer() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            throw lumen_test::assertion_error("socketpair");
        sd_id128_t id{};
        (void)sd_id128_randomize(&id);
        sd_bus_new(&server);
        sd_bus_set_fd(server, fds[0], fds[0]);
        sd_bus_set_server(server, 1, id);
        sd_bus_start(server);
        sd_bus_new(&client);
        sd_bus_set_fd(client, fds[1], fds[1]);
        sd_bus_start(client);
    }
    ~peer() {
        sd_bus_flush_close_unref(client);
        sd_bus_flush_close_unref(server);
    }
    void pump() {
        for (int i = 0; i < 400; ++i) {
            const int a = sd_bus_process(server, nullptr);
            const int b = sd_bus_process(client, nullptr);
            if (a <= 0 && b <= 0)
                return;
        }
    }
};

int append_layout_item(sd_bus_message* m, int id, const char* label, const char* type,
                       const char* toggle, bool visible, int nchildren) {
    int r = sd_bus_message_open_container(m, 'r', "ia{sv}av");
    if (r >= 0)
        r = sd_bus_message_append(m, "i", id);
    if (r >= 0)
        r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r >= 0 && label)
        r = sd_bus_message_append(m, "{sv}", "label", "s", label);
    if (r >= 0 && type)
        r = sd_bus_message_append(m, "{sv}", "type", "s", type);
    if (r >= 0 && toggle) {
        r = sd_bus_message_append(m, "{sv}", "toggle-type", "s", toggle);
        if (r >= 0)
            r = sd_bus_message_append(m, "{sv}", "toggle-state", "i", 1);
    }
    if (r >= 0 && !visible)
        r = sd_bus_message_append(m, "{sv}", "visible", "b", 0);
    if (r >= 0)
        r = sd_bus_message_append(m, "{sv}", "unknown-property", "d", 1.5);
    if (r >= 0)
        r = sd_bus_message_close_container(m);
    if (r >= 0)
        r = sd_bus_message_open_container(m, 'a', "v");
    (void)nchildren;
    return r;
}

int close_layout_item(sd_bus_message* m) {
    int r = sd_bus_message_close_container(m); // av
    if (r >= 0)
        r = sd_bus_message_close_container(m); // struct
    return r;
}

std::vector<std::string> g_signals;
int on_signal(sd_bus_message* m, void*, sd_bus_error*) {
    std::string s = sd_bus_message_get_member(m);
    const char* arg = nullptr;
    if (sd_bus_message_read(m, "s", &arg) > 0 && arg)
        s += std::string{" "} + arg;
    g_signals.push_back(s);
    return 0;
}

struct reply_box {
    bool done = false;
    std::string error;
    sd_bus_message* msg = nullptr;
    ~reply_box() { sd_bus_message_unref(msg); }
};
int on_reply(sd_bus_message* m, void* data, sd_bus_error*) {
    auto* box = static_cast<reply_box*>(data);
    box->done = true;
    if (sd_bus_message_is_method_error(m, nullptr))
        box->error = sd_bus_message_get_error(m)->name;
    else
        box->msg = sd_bus_message_ref(m);
    return 0;
}

// The item's side (on the client end): properties read by the host's GetAll.
int item_prop(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply, void*,
              sd_bus_error*) {
    const std::string p{property};
    if (p == "Id")
        return sd_bus_message_append(reply, "s", "probe-id");
    if (p == "Title")
        return sd_bus_message_append(reply, "s", "Probe title");
    if (p == "Status")
        return sd_bus_message_append(reply, "s", "NeedsAttention");
    if (p == "IconName")
        return sd_bus_message_append(reply, "s", "probe-icon");
    if (p == "ItemIsMenu")
        return sd_bus_message_append(reply, "b", 0);
    if (p == "Menu")
        return sd_bus_message_append(reply, "o", "/MenuBar");
    if (p == "ToolTip") {
        int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
        if (r >= 0)
            r = sd_bus_message_append(reply, "s", "");
        if (r >= 0)
            r = sd_bus_message_open_container(reply, 'a', "(iiay)");
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
        if (r >= 0)
            r = sd_bus_message_append(reply, "ss", "Tip", "<b>tip</b> text");
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
        return r;
    }
    if (p == "IconPixmap") {
        int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
        const std::uint8_t px[] = {0xFF, 0x00, 0xFF, 0x00};
        if (r >= 0)
            r = sd_bus_message_append(reply, "(iiay)", 1, 1, 4, px[0], px[1], px[2], px[3]);
        if (r >= 0)
            r = sd_bus_message_close_container(reply);
        return r;
    }
    return sd_bus_message_append(reply, "s", "");
}

const sd_bus_vtable k_item_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id", "s", item_prop, 0, 0),
    SD_BUS_PROPERTY("Title", "s", item_prop, 0, 0),
    SD_BUS_PROPERTY("Status", "s", item_prop, 0, 0),
    SD_BUS_PROPERTY("IconName", "s", item_prop, 0, 0),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", item_prop, 0, 0),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", item_prop, 0, 0),
    SD_BUS_PROPERTY("ItemIsMenu", "b", item_prop, 0, 0),
    SD_BUS_PROPERTY("Menu", "o", item_prop, 0, 0),
    SD_BUS_PROPERTY("IconThemePath", "s", item_prop, 0, 0),
    SD_BUS_VTABLE_END,
};

} // namespace

LUMEN_TEST(dbusmenu_layout) {
    peer p;
    p.pump(); // the authentication done (a connection still authenticating blocks its close)
    sd_bus_message* m = nullptr;
    LUMEN_CHECK(sd_bus_message_new_method_call(p.client, &m, "x.y", "/", "x.y", "M") >= 0);
    LUMEN_CHECK(sd_bus_message_append(m, "u", 7u) >= 0);
    // root(0) { Open(1), Check(2, checkmark), separator(3), hidden(9), More(4) { Sub(5) } }
    LUMEN_CHECK(append_layout_item(m, 0, nullptr, nullptr, nullptr, true, 5) >= 0);
    const auto child = [&](int id, const char* label, const char* type, const char* toggle,
                           bool visible) {
        LUMEN_CHECK(sd_bus_message_open_container(m, 'v', "(ia{sv}av)") >= 0);
        LUMEN_CHECK(append_layout_item(m, id, label, type, toggle, visible, 0) >= 0);
        LUMEN_CHECK(close_layout_item(m) >= 0);
        LUMEN_CHECK(sd_bus_message_close_container(m) >= 0);
    };
    child(1, "_Open", nullptr, nullptr, true);
    child(2, "_Check", nullptr, "checkmark", true);
    child(3, nullptr, "separator", nullptr, true);
    child(9, "Hidden", nullptr, nullptr, false);
    LUMEN_CHECK(sd_bus_message_open_container(m, 'v', "(ia{sv}av)") >= 0);
    LUMEN_CHECK(append_layout_item(m, 4, "_More", nullptr, nullptr, true, 1) >= 0);
    child(5, "_Sub", nullptr, nullptr, true);
    LUMEN_CHECK(close_layout_item(m) >= 0);
    LUMEN_CHECK(sd_bus_message_close_container(m) >= 0);
    LUMEN_CHECK(close_layout_item(m) >= 0);
    LUMEN_CHECK(sd_bus_message_seal(m, 1, 0) >= 0);
    LUMEN_CHECK(sd_bus_message_rewind(m, 1) >= 0);
    menu_entry root{};
    unsigned revision = 0;
    LUMEN_CHECK(parse_dbusmenu_layout(m, root, &revision));
    sd_bus_message_unref(m);
    LUMEN_CHECK(revision == 7 && root.children.size() == 4);
    LUMEN_CHECK(root.children[0].id == 1 && root.children[0].label == "_Open");
    LUMEN_CHECK(root.children[1].toggle_kind == menu_entry::toggle::check && root.children[1].toggled);
    LUMEN_CHECK(root.children[2].separator);
    LUMEN_CHECK(root.children[3].submenu && root.children[3].children.size() == 1 &&
                root.children[3].children[0].id == 5);
}

LUMEN_TEST(watcher_and_host) {
    peer p;
    tray_service tray;
    LUMEN_CHECK(tray.attach_raw(p.server));
    sd_bus_slot* item = nullptr;
    LUMEN_CHECK(sd_bus_add_object_vtable(p.client, &item, "/StatusNotifierItem",
                                         "org.kde.StatusNotifierItem", k_item_vtable, nullptr) >= 0);
    sd_bus_slot* match = nullptr;
    LUMEN_CHECK(sd_bus_add_match(p.client, &match,
                                 "type='signal',interface='org.kde.StatusNotifierWatcher'",
                                 on_signal, nullptr) >= 0);
    g_signals.clear();
    const char* w = "org.kde.StatusNotifierWatcher";
    const char* path = "/StatusNotifierWatcher";
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, w, path, w,
                                             "RegisterStatusNotifierItem", on_reply, &box, "s",
                                             "org.kde.StatusNotifierItem-7-1") >= 0);
        p.pump();
        LUMEN_CHECK(box.done && box.error.empty());
    }
    LUMEN_CHECK(tray.registry().items().size() == 1 &&
                tray.registry().items()[0] == "org.kde.StatusNotifierItem-7-1/StatusNotifierItem");
    LUMEN_CHECK(!g_signals.empty() &&
                g_signals[0] == "StatusNotifierItemRegistered org.kde.StatusNotifierItem-7-1/StatusNotifierItem");
    // The host read the item (GetAll on the client end).
    p.pump();
    LUMEN_CHECK(tray.count() == 1);
    const tray_item* it = tray.item(0);
    LUMEN_CHECK(it && it->fetched && it->id == "probe-id" && it->title == "Probe title" &&
                it->status == "NeedsAttention" && it->icon_name == "probe-icon" &&
                it->menu_path == "/MenuBar" && it->has_menu() && !it->item_is_menu);
    LUMEN_CHECK(it->tooltip_title == "Tip" && it->tooltip_text == "tip text");
    LUMEN_CHECK(it->icon.size() == 1 && it->icon[0].argb[0] == 0xFF00FF00u);
    // Registered twice: once.
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, w, path, w,
                                             "RegisterStatusNotifierItem", on_reply, &box, "s",
                                             "org.kde.StatusNotifierItem-7-1") >= 0);
        p.pump();
        LUMEN_CHECK(box.done && box.error.empty() && tray.count() == 1);
    }
    // A path with no sender (no bus here): refused.
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, w, path, w,
                                             "RegisterStatusNotifierItem", on_reply, &box, "s",
                                             "/org/ayatana/x") >= 0);
        p.pump();
        LUMEN_CHECK(box.done && !box.error.empty());
    }
    // Properties.
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, w, path,
                                             "org.freedesktop.DBus.Properties", "GetAll", on_reply,
                                             &box, "s", w) >= 0);
        p.pump();
        LUMEN_CHECK(box.msg);
        bool host = false;
        int version = -1;
        unsigned items = 0;
        LUMEN_CHECK(sd_bus_message_enter_container(box.msg, 'a', "{sv}") > 0);
        while (sd_bus_message_enter_container(box.msg, 'e', "sv") > 0) {
            const char* key = nullptr;
            (void)sd_bus_message_read(box.msg, "s", &key);
            const std::string k{key};
            if (k == "IsStatusNotifierHostRegistered") {
                int b = 0;
                (void)sd_bus_message_read(box.msg, "v", "b", &b);
                host = b != 0;
            } else if (k == "ProtocolVersion") {
                (void)sd_bus_message_read(box.msg, "v", "i", &version);
            } else if (k == "RegisteredStatusNotifierItems") {
                (void)sd_bus_message_enter_container(box.msg, 'v', "as");
                (void)sd_bus_message_enter_container(box.msg, 'a', "s");
                const char* s = nullptr;
                while (sd_bus_message_read(box.msg, "s", &s) > 0)
                    ++items;
                (void)sd_bus_message_exit_container(box.msg);
                (void)sd_bus_message_exit_container(box.msg);
            } else {
                (void)sd_bus_message_skip(box.msg, "v");
            }
            (void)sd_bus_message_exit_container(box.msg);
        }
        LUMEN_CHECK(host && version == 0 && items == 1);
    }
    // (An item's change signals are matched by its bus name, which a socketpair has none of:
    // app-smoke's N6 checks the refresh on the real bus.)
    sd_bus_slot_unref(match);
    sd_bus_slot_unref(item);
    tray.detach();
}

#endif

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
