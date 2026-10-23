module;

// The system tray (design-session-and-wm N.5, N.6): a StatusNotifierWatcher and a host in
// lumen-shell, waybar's arrangement — KDE's de-facto StatusNotifierItem specification
// (org.kde.StatusNotifierWatcher at /StatusNotifierWatcher; items at /StatusNotifierItem or
// the path they register), menus over com.canonical.dbusmenu.
//
// Watcher: owns org.kde.StatusNotifierWatcher (queued behind another one, e.g. a KDE
// component: then only the host role runs, against that watcher). Items register by bus
// name ("org.kde.StatusNotifierItem-PID-N", path /StatusNotifierItem) or by object path
// (libappindicator: the sender's unique name with that path); both are kept as
// "bus/path", deduplicated, at most k_max_items, and dropped when their bus name loses its
// owner (one NameOwnerChanged match per item). When the shell restarts, items see the
// watcher's name change owner and register again (libappindicator, KStatusNotifierItem and
// Qt all do).
//
// Host: org.kde.StatusNotifierHost-<pid>; each item's properties are read once (GetAll),
// then again only when the item signals a change (NewIcon, NewStatus, …; coalesced per
// dispatch). Idle: no timers, no polling — the bus fd sleeps.
//
// The pure parts (service strings, pixmaps, the dbusmenu layout) are tested in
// tests/test_tray.cpp; the bus parts in tray_bus.cpp. Shell thread only.

import lx.foundation;

export module lx.shell:tray;

import std;

import :menu_popup;
import :user_bus;

export namespace lx::shell {

inline constexpr unsigned k_max_tray_items = 32;

/// A registration string → (bus name, object path). `/path` alone: the sender's unique
/// name with that path; `name/path`: as given; `name`: the default path
/// /StatusNotifierItem. False for an empty or malformed one.
[[nodiscard]] bool parse_sni_service(std::string_view registered, std::string_view sender,
                                     std::string& bus, std::string& path);

/// One IconPixmap entry, converted: premultiplied ARGB8888 in native order.
struct sni_pixmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> argb;
};
/// ARGB32 in network byte order (the spec's) → premultiplied native. False when the data
/// is shorter than width × height × 4 or a dimension is ≤ 0 or > 1024.
[[nodiscard]] bool convert_sni_pixmap(int width, int height, std::span<const std::uint8_t> data,
                                      sni_pixmap& out);
/// The pixmap to draw at `px`: the smallest at least that large, else the largest.
[[nodiscard]] const sni_pixmap* choose_pixmap(const std::vector<sni_pixmap>& set, int px);
/// `p` fitted into a `px`² square (premultiplied ARGB).
void pixmap_to_icon(const sni_pixmap& p, int px, std::vector<std::uint32_t>& out);

struct tray_item {
    /// Stable for the item's life (the panel's cells refer to it).
    unsigned key = 0;
    std::string bus;
    std::string path;
    /// "bus/path", as the watcher lists it.
    std::string service;
    std::string id;
    std::string title;
    std::string category;
    /// Active, Passive (hidden), NeedsAttention.
    std::string status;
    std::string icon_name;
    std::string attention_icon_name;
    std::string overlay_icon_name;
    /// IconThemePath (KDE): a directory searched before the icon theme.
    std::string theme_path;
    std::vector<sni_pixmap> icon;
    std::vector<sni_pixmap> attention;
    std::vector<sni_pixmap> overlay;
    std::string tooltip_title;
    std::string tooltip_text;
    bool item_is_menu = false;
    /// The dbusmenu object ("" or "/": none).
    std::string menu_path;
    /// The properties were read at least once (drawn only then).
    bool fetched = false;
    /// Changes with whatever the panel draws (icon, status, tooltip).
    unsigned rev = 0;

    [[nodiscard]] bool hidden() const { return !fetched || status == "Passive"; }
    [[nodiscard]] bool has_menu() const { return !menu_path.empty() && menu_path != "/"; }
};

/// The watcher's bookkeeping (pure): registered items ("bus/path") and hosts.
class sni_registry {
public:
    /// False when known already or full.
    bool add_item(const std::string& service);
    bool remove_item(const std::string& service);
    /// Every item whose bus is `bus` (its owner left); returns them.
    std::vector<std::string> remove_bus(std::string_view bus);
    [[nodiscard]] const std::vector<std::string>& items() const { return items_; }
    bool add_host(const std::string& name);
    bool remove_host(std::string_view name);
    [[nodiscard]] bool host_registered() const { return !hosts_.empty(); }

private:
    std::vector<std::string> items_;
    std::vector<std::string> hosts_;
};

/// The properties of one com.canonical.dbusmenu item, read into a menu entry (the bus layer
/// calls this per item of a GetLayout reply). Invisible items are left out by the caller.
struct dbusmenu_props {
    std::string label;
    bool enabled = true;
    bool visible = true;
    bool separator = false;
    /// "checkmark", "radio" or "".
    std::string toggle_type;
    int toggle_state = -1;
    std::string icon_name;
    bool submenu = false;
    /// "Control+q" from the shortcut property (aas), the first combination.
    std::string shortcut;
};
[[nodiscard]] menu_entry dbusmenu_entry(int id, const dbusmenu_props& p);
/// A GetLayout reply ("u(ia{sv}av)", sd_bus_message* positioned at its start) read into
/// `root` (its children are the menu). False when malformed. Recursion and items bounded
/// (depth 8, 256 items).
[[nodiscard]] bool parse_dbusmenu_layout(void* message, menu_entry& root, unsigned* revision);

/// The watcher, the host and the items on a bus connection (tray_bus.cpp).
class tray_service {
public:
    tray_service() = default;
    ~tray_service();
    tray_service(const tray_service&) = delete;
    tray_service& operator=(const tray_service&) = delete;

    /// Exports /StatusNotifierWatcher and requests the watcher and host names on `bus`.
    /// `request_names` false: the object only (tests drive a socketpair without names).
    [[nodiscard]] bool attach(user_bus& bus, bool request_names = true);
    /// The same on a bare connection (tests); names are not requested.
    [[nodiscard]] bool attach_raw(void* sd_bus);
    void detach();

    /// The items, in registration order (hidden ones included; see `tray_item::hidden`).
    [[nodiscard]] unsigned count() const { return static_cast<unsigned>(items_.size()); }
    [[nodiscard]] const tray_item* item(unsigned i) const {
        return i < items_.size() ? items_[i].get() : nullptr;
    }
    [[nodiscard]] const tray_item* find(unsigned key) const;
    /// Changes whenever an item appears, goes or changes.
    [[nodiscard]] unsigned rev() const { return rev_; }
    [[nodiscard]] const sni_registry& registry() const { return registry_; }
    /// This shell owns org.kde.StatusNotifierWatcher (else another one does).
    [[nodiscard]] bool watcher_owned() const { return watcher_owned_; }

    // ── Asking an item ──
    /// ProvideXdgActivationToken(token) first when given (KDE's extension; its absence is
    /// not an error), then Activate / SecondaryActivate (x, y).
    void activate(unsigned key, int x, int y, const char* token, bool secondary);
    void context_menu(unsigned key, int x, int y);
    void scroll(unsigned key, int delta, bool horizontal);

    // ── Its menu (com.canonical.dbusmenu) ──
    using layout_fn = void (*)(unsigned key, std::vector<menu_entry> items, void* user);
    /// AboutToShow(0), then GetLayout(0, -1); `fn` gets the items (once).
    bool fetch_menu(unsigned key, layout_fn fn, void* user);
    /// While the menu is shown its layout changes are followed (`fn` again with the new
    /// items); `key` 0: no menu shown.
    void set_menu_shown(unsigned key);
    /// Event(id, "clicked" | "opened" | "closed", 0, timestamp).
    void menu_event(unsigned key, int id, const char* type);
    void menu_about_to_show(unsigned key, int id);

    /// Reads again what items signalled since the last call (one GetAll each). Call once
    /// per loop iteration after the bus was processed.
    void after_dispatch();

    struct events;

private:
    friend struct events;
    struct item_rec : tray_item {
        tray_service* owner = nullptr;
        void* signal_slot = nullptr; // sd_bus_slot*: the item's signals
        void* owner_slot = nullptr;  // sd_bus_slot*: NameOwnerChanged of its bus
        void* fetch_slot = nullptr;  // sd_bus_slot*: GetAll in flight
        bool refresh = false;
    };
    item_rec* find_rec(unsigned key);
    item_rec* find_service(std::string_view service);
    void add_item(const std::string& bus, const std::string& path, bool from_watcher_signal);
    void remove_item(item_rec* r, bool announce);
    void fetch(item_rec& r);
    void watcher_name(user_bus::name_state state);
    void go_remote();
    void go_local();
    /// GetLayout(0, -1); `fn` gets the items.
    bool fetch_layout(unsigned key, layout_fn fn, void* user);

    user_bus* ubus_ = nullptr;
    void* bus_ = nullptr;           // sd_bus*
    void* watcher_slot_ = nullptr;  // sd_bus_slot*: the exported object
    void* remote_slot_[3]{};        // matches on another watcher's signals; its owner
    void* remote_call_ = nullptr;   // sd_bus_slot*: Get RegisteredStatusNotifierItems
    sni_registry registry_{};
    std::vector<std::unique_ptr<item_rec>> items_;
    char host_name_[64]{};
    bool watcher_owned_ = false;
    bool remote_ = false;
    unsigned next_key_ = 1;
    unsigned rev_ = 1;
    /// The item whose menu is shown, its layout handler and the match on its signals.
    unsigned menu_key_ = 0;
    layout_fn menu_fn_ = nullptr;
    void* menu_user_ = nullptr;
    void* menu_slot_ = nullptr;
    /// The GetLayout in flight (one at a time) and whom it is for.
    void* layout_call_ = nullptr;
    unsigned layout_key_ = 0;
    layout_fn layout_fn_ = nullptr;
    void* layout_user_ = nullptr;
};

} // namespace lx::shell
