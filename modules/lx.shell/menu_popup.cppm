module;

// Popup menus and tooltips of lumen-shell (design-session-and-wm N.4, N.6): the tray's
// com.canonical.dbusmenu menus, the notification history and a notification's extra
// actions. Each is an xdg_popup of a layer surface, a submenu an xdg_popup of its parent
// menu, every level with an explicit grab on the click's serial — so a click anywhere else
// closes it (popup_done) and, while the parent layer surface is keyboard-interactive, the
// keyboard goes to the topmost level: arrows, Return, Space, Escape, Left/Right for
// submenus and the mnemonic letters ("_Open": O, underlined). The parent surface's owner
// makes it `on_demand` while a menu is open (the panel takes no keyboard otherwise).
//
// The levels are lx.ui trees on lx.ui.host surfaces with a popup role (ADR-010 Phase 5
// P5-8): the same layout_menu geometry, the canvas's exact palette (view_ui's literal
// boxes), the rows the tree's interactive nodes — hovering selects (and opens a submenu's
// row), a release activates, the keyboard is the focused root's and goes through the same
// key handling as before. The grab focuses the popup surface, and the context's seat router
// routes what it sends into that level's tree. The tooltip is a popup without a grab and
// with an empty input region.
//
// No Wayland or D-Bus type crosses this interface. Shell thread only; the surfaces exist
// only while open; nothing runs while closed.

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;

export module lx.shell:menu_popup;

import std;

import :view_ui;

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

/// The generic popup's UI font at the requested theme size and weight.
[[nodiscard]] lx::text::font_desc menu_font(
    lx::text::weight weight = lx::text::weight::normal,
    lx::ui::text_size size = lx::ui::text_size::ui);

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
    /// The shell's lx.ui.host context (null: no menu can open).
    lx::ui::host::context* ui = nullptr;
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
    /// Opens `items` under `anchor` (the parent surface's coordinates) of the parent layer
    /// surface's role, grabbing with `serial` (a button press on that surface). A menu
    /// already open is closed first.
    bool open(lx::ui::host::layer_role& layer, lx::rect2i anchor, std::vector<menu_entry> items,
              unsigned serial, activate_fn on_activate, closed_fn on_closed, submenu_fn on_submenu,
              void* user, const char* name);
    /// Closes every level (`on_closed` runs).
    void close();
    [[nodiscard]] bool is_open() const { return levels_ > 0; }
    /// The items changed while open (dbusmenu LayoutUpdated): submenus close, the root is
    /// redrawn with the new items at its size (rows that do not fit are dropped).
    void replace_items(std::vector<menu_entry> items);
    /// Redraws what changed (the shell's loop, once per dispatch).
    void after_dispatch();

private:
    friend lx::ui::child menu_describe(lx::ui::build_context& ctx);
    friend class menu_root_node;
    /// One open level: its surface, popup role and tree.
    struct level {
        lx::ui::host::surface surface{};
        lx::ui::host::popup_role role{};
        std::vector<menu_entry>* items = nullptr;
        menu_rows rows{};
        int selected = -1;
        bool dirty = false;
    };
    /// A row's stable identity for the tree's callbacks.
    struct row_ref {
        menu_popup* self = nullptr;
        unsigned index = 0;
        int row = -1;
    };
    /// The root's: the tree's focused node forwards the level's keys.
    struct root_ref {
        menu_popup* self = nullptr;
        unsigned index = 0;
    };
    /// An icon's atlas tile, resolved once at a size.
    struct icon_slot {
        char name[64]{};
        int px = 0;
        lx::gfx::atlas_tile_id tile{};
    };

    bool open_level(unsigned index, lx::ui::host::popup_role* parent_popup,
                    lx::ui::host::layer_role* parent_layer, lx::rect2i anchor,
                    std::vector<menu_entry>* items, unsigned serial, bool submenu);
    void close_from(unsigned index);
    void describe_level(unsigned index);
    [[nodiscard]] lx::ui::child build_ui(unsigned index);
    void activate(unsigned index, int row, unsigned serial);
    void select(unsigned index, int row);
    int step(const level& l, int from, int dir) const;
    void key(unsigned index, unsigned keysym, const char* utf8, unsigned serial);
    [[nodiscard]] lx::gfx::atlas_tile_id icon_tile(unsigned level_index, const char* name, int px);
    void on_row_hover(const row_ref& ref, bool hovered);
    void on_row_event(const row_ref& ref, const lx::ui::pointer_event& e);
    void on_root_key(unsigned index, const lx::ui::key_event& e);
    void on_level_key_focus(unsigned index, bool entered);

    menu_globals g_{};
    menu_icon_fn icons_ = nullptr;
    void* icons_user_ = nullptr;
    /// The describe frame's attrs scratch (a span into it is copied by the text's apply,
    /// which runs within the same describe).
    std::vector<lx::text::text_attr> mnemonic_scratch_{};
    std::vector<menu_entry> root_{};
    level level_[k_menu_max_levels]{};
    row_ref row_refs_[k_menu_max_levels][k_menu_max_rows]{};
    root_ref root_refs_[k_menu_max_levels]{};
    static constexpr unsigned k_icon_slots = 32;
    icon_slot icon_slots_[k_menu_max_levels][k_icon_slots]{};
    unsigned levels_ = 0;
    activate_fn on_activate_ = nullptr;
    closed_fn on_closed_ = nullptr;
    submenu_fn on_submenu_ = nullptr;
    void* user_ = nullptr;
    unsigned last_serial_ = 0;
    /// Bumped by every `open`: an activation handler that opened another menu keeps it.
    unsigned generation_ = 0;
    char name_[32]{};
};

/// The levels' describing function (the surface's frame loop calls it inside `run_frame`).
[[nodiscard]] lx::ui::child menu_describe(lx::ui::build_context& ctx);

/// A tooltip: an xdg_popup of a layer surface, no grab, no input. Up to four lines.
class tooltip_popup {
public:
    tooltip_popup() = default;
    ~tooltip_popup();
    tooltip_popup(const tooltip_popup&) = delete;
    tooltip_popup& operator=(const tooltip_popup&) = delete;

    void init(menu_globals g) { g_ = g; }
    bool open(lx::ui::host::layer_role& layer, lx::rect2i anchor, std::string_view title,
              std::string_view text);
    void close();
    [[nodiscard]] bool is_open() const { return surface_.valid(); }
    void after_dispatch();

private:
    friend lx::ui::child tooltip_describe(lx::ui::build_context& ctx);
    [[nodiscard]] lx::ui::child build_ui();
    void request_draw();

    menu_globals g_{};
    lx::ui::host::surface surface_{};
    lx::ui::host::popup_role role_{};
    /// The describe frame's attrs scratch (see `menu_popup`).
    std::vector<lx::text::text_attr> mnemonic_scratch_{};
    std::string title_{};
    lx::text::wrapped_text text_{};
    std::string body_{};
    int width_ = 0;
    int height_ = 0;
    bool drawn_ = false;
};

/// The tooltip's describing function.
[[nodiscard]] lx::ui::child tooltip_describe(lx::ui::build_context& ctx);

} // namespace lx::shell
