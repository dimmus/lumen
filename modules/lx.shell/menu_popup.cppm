module;

// Popup menus and tooltips of lumen-shell (design-session-and-wm N.4, N.6): the tray's
// com.canonical.dbusmenu menus, the notification history and a notification's extra
// actions. Each is an xdg_popup of a layer surface (zwlr_layer_surface_v1.get_popup), a
// submenu an xdg_popup of its parent menu, every level with an explicit grab on the click's
// serial — so a click anywhere else closes it (popup_done) and, while the parent layer
// surface is keyboard-interactive, the keyboard goes to the topmost level: arrows, Return,
// Space, Escape, Left/Right for submenus and the mnemonic letters ("_Open": O, underlined).
// The parent surface's owner makes it `on_demand` while a menu is open (the panel takes no
// keyboard otherwise).
//
// The tooltip is an xdg_popup without a grab and with an empty input region.
//
// No Wayland or D-Bus type crosses this interface. Shell thread only; the surfaces exist
// only while open; nothing runs while closed.

import lx.foundation;
import lx.text;

export module lx.shell:menu_popup;

import std;

import :shm_canvas;

export namespace lx::shell {

struct menu_entry {
    /// '_' marks the mnemonic ("__" is a literal underscore).
    std::string label;
    /// The owner's identifier (a dbusmenu item id, a history entry).
    int id = 0;
    bool enabled = true;
    bool separator = false;
    /// A title row: dim, not selectable.
    bool header = false;
    enum class toggle : unsigned char { none, check, radio };
    toggle toggle_kind = toggle::none;
    bool toggled = false;
    std::string icon_name;
    /// Shown right-aligned ("Ctrl+Q").
    std::string shortcut;
    bool submenu = false;
    std::vector<menu_entry> children;
};

/// A label without its mnemonic marks, and the mnemonic's byte offset in it (-1: none).
[[nodiscard]] std::string menu_label(std::string_view label, int* mnemonic = nullptr);

inline constexpr int k_menu_row_h = 26;
inline constexpr int k_menu_separator_h = 9;
inline constexpr int k_menu_pad = 4;
inline constexpr int k_menu_min_w = 160;
inline constexpr int k_menu_max_w = 480;
inline constexpr unsigned k_menu_max_rows = 64;
inline constexpr unsigned k_menu_max_levels = 6;

/// One level's rows (logical px, in the level's surface).
struct menu_rows {
    int width = 0;
    int height = 0;
    unsigned count = 0;
    lx::rect2i row[k_menu_max_rows]{};
};
/// Lays out `items` (visible ones) with `text_width` measuring a label.
void layout_menu(const std::vector<menu_entry>& items, int (*text_width)(const char*, void*),
                 void* user, menu_rows& out);

struct menu_globals {
    void* compositor = nullptr; // wl_compositor
    void* shm = nullptr;        // wl_shm
    void* wm_base = nullptr;    // xdg_wm_base
    void* seat = nullptr;       // wl_seat
    void* viewporter = nullptr; // wp_viewporter
    void* fractional = nullptr; // wp_fractional_scale_manager_v1
};

using menu_icon_fn = const std::uint32_t* (*)(const char* name, int px, void* user);

class menu_popup {
public:
    /// An enabled item was chosen (the menu closes after).
    using activate_fn = void (*)(int id, void* user);
    /// The menu closed (chosen, dismissed or closed by the owner): dbusmenu's "closed".
    using closed_fn = void (*)(void* user);
    /// A submenu is about to open (dbusmenu AboutToShow).
    using submenu_fn = void (*)(int id, void* user);

    menu_popup() = default;
    ~menu_popup();
    menu_popup(const menu_popup&) = delete;
    menu_popup& operator=(const menu_popup&) = delete;

    void init(menu_globals g, menu_icon_fn icons, void* user);
    /// Opens `items` under `anchor` (the parent surface's coordinates) of the layer surface
    /// `layer` (zwlr_layer_surface_v1*), grabbing with `serial` (a button press on that
    /// surface). A menu already open is closed first.
    bool open(void* layer, lx::rect2i anchor, std::vector<menu_entry> items, unsigned serial,
              activate_fn on_activate, closed_fn on_closed, submenu_fn on_submenu, void* user,
              const char* name);
    /// Closes every level (`on_closed` runs).
    void close();
    [[nodiscard]] bool is_open() const { return levels_ > 0; }
    /// Whether `surface` (wl_surface*) is one of its levels.
    [[nodiscard]] bool owns(void* surface) const;
    /// The items changed while open (dbusmenu LayoutUpdated): submenus close, the root is
    /// redrawn with the new items at its size (rows that do not fit are dropped).
    void replace_items(std::vector<menu_entry> items);

    // ── Input, routed here by the shell ──
    void pointer_enter(void* surface, double x, double y);
    void pointer_leave(void* surface);
    void pointer_motion(void* surface, double x, double y);
    void pointer_button(void* surface, unsigned button, bool pressed, unsigned serial);
    /// A key press while open (keysym; `utf8` for mnemonics), `serial` for submenu grabs.
    void key(unsigned keysym, const char* utf8, unsigned serial);
    void after_dispatch();

    struct events;

private:
    friend struct events;
    struct level {
        menu_popup* owner = nullptr;
        void* surface = nullptr;  // wl_surface*
        void* xdg = nullptr;      // xdg_surface*
        void* popup = nullptr;    // xdg_popup*
        std::vector<menu_entry>* items = nullptr;
        menu_rows rows{};
        shm_canvas canvas{};
        int selected = -1;
        bool configured = false;
        bool dirty = false;
        /// The row of the parent level this submenu hangs from.
        int parent_row = -1;
    };
    bool open_level(unsigned index, void* parent_xdg, void* parent_layer, lx::rect2i anchor,
                    std::vector<menu_entry>* items, unsigned serial, bool submenu);
    void close_from(unsigned index);
    void draw(level& l);
    void activate(unsigned index, int row, unsigned serial);
    /// The level a surface belongs to, or -1.
    [[nodiscard]] int level_of(void* surface) const;
    void select(unsigned index, int row);
    int step(const level& l, int from, int dir) const;

    menu_globals g_{};
    menu_icon_fn icons_ = nullptr;
    void* icons_user_ = nullptr;
    std::vector<menu_entry> root_{};
    level level_[k_menu_max_levels]{};
    unsigned levels_ = 0;
    activate_fn on_activate_ = nullptr;
    closed_fn on_closed_ = nullptr;
    submenu_fn on_submenu_ = nullptr;
    void* user_ = nullptr;
    unsigned last_serial_ = 0;
    /// Bumped by every `open`: an activation handler that opened another menu keeps it.
    unsigned generation_ = 0;
    int pointer_level_ = -1;
    char name_[32]{};
};

/// A tooltip: an xdg_popup of a layer surface, no grab, no input. Up to four lines.
class tooltip_popup {
public:
    tooltip_popup() = default;
    ~tooltip_popup();
    tooltip_popup(const tooltip_popup&) = delete;
    tooltip_popup& operator=(const tooltip_popup&) = delete;

    void init(menu_globals g) { g_ = g; }
    bool open(void* layer, lx::rect2i anchor, std::string_view title, std::string_view text);
    void close();
    [[nodiscard]] bool is_open() const { return surface_ != nullptr; }
    void after_dispatch();

    struct events;

private:
    friend struct events;
    void draw();
    menu_globals g_{};
    shm_canvas canvas_{};
    std::string title_{};
    lx::text::wrapped_text text_{};
    std::string body_{};
    int width_ = 0;
    int height_ = 0;
    void* surface_ = nullptr;
    void* xdg_ = nullptr;
    void* popup_ = nullptr;
    bool configured_ = false;
    bool drawn_ = false;
};

} // namespace lx::shell
