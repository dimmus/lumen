module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#include <wayland-client.h>
#ifdef LUMEN_PROTOCOL_EXT_BACKGROUND_EFFECT_V1
#include "ext-background-effect-v1-client-protocol.h"
#define LUMEN_SHELL_BACKGROUND_EFFECT 1
#endif

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.trace;
import lx.wayland.client;
import lx.ui;
import lx.ui.host;

export module lx.shell:panel;

import std;

import :task_model;
import :bridge;

export namespace lx::shell {

// ── Panel layout (pure) ───────────────────────────────────────────────────────
//
// One row: the Apps button at the left end, the workspace indicator (one cell per workspace
// of the panel's output, while there is more than one), then the task entries left to right
// in task order (or, while a notice shows, the notice in their place), then the logout
// button.
// Kept separate from drawing so the UI tree below is described from one snapshot of it, and
// so both the input state machine and the tests read the same geometry the panel draws.

inline constexpr int k_panel_height = 32;
inline constexpr int k_panel_pad = 4;
inline constexpr int k_panel_gap = 4;
inline constexpr int k_entry_height = 24;
inline constexpr int k_entry_min_width = 48;
inline constexpr int k_entry_max_width = 220;
/// Room for the "+k" label of entries that do not fit.
inline constexpr int k_panel_more_width = 44;
/// The logout icon at the right end.
inline constexpr int k_logout_width = 28;
/// The Apps button (opens the launcher) at the left end.
inline constexpr int k_apps_width = 64;
/// The privacy indicator (design-app-compat-capture CI1): a red dot left of the logout
/// button while some client captures the screen.
inline constexpr int k_capture_width = 16;
inline constexpr int k_capture_dot = 10;
inline constexpr unsigned k_max_panel_cells = 128;
/// The workspace indicator (design-session-and-wm W.7): a cell per workspace.
inline constexpr unsigned k_max_ws_cells = 16;
inline constexpr int k_ws_cell_min = 26;
inline constexpr int k_ws_cell_max = 96;
inline constexpr int k_ws_gap = 2;
/// The tray (design-session-and-wm N.5): a cell per StatusNotifierItem, left of the
/// notification indicator, its icon 16 logical px.
inline constexpr unsigned k_max_tray_cells = 32;
inline constexpr int k_tray_cell = 24;
inline constexpr int k_tray_icon = 16;
inline constexpr int k_tray_gap = 2;
/// The notification indicator (N.4): a bell, the unread count beside it.
inline constexpr int k_bell_width = 28;

enum panel_cell_flag : unsigned {
    cell_focused = 1u << 0,
    cell_hover = 1u << 1,
    cell_pressed = 1u << 2,
    /// The window asked for attention (an activation that was not granted focus).
    cell_attention = 1u << 3,
    /// Its client does not answer pings: the label is drawn dimmed.
    cell_not_responding = 1u << 4,
    /// Minimized (zlm v7): dimmed too; a click brings it back.
    cell_minimized = 1u << 5,
};

/// One workspace cell's look.
enum panel_ws_flag : unsigned {
    ws_cell_active = 1u << 0,
    /// A window is on it (a dot under the name).
    ws_cell_occupied = 1u << 1,
    /// A window on it asks for attention.
    ws_cell_urgent = 1u << 2,
    ws_cell_hover = 1u << 3,
    ws_cell_pressed = 1u << 4,
};

/// The indicator's input: the panel's output's workspaces in order.
struct panel_ws_item {
    char name[32]{};
    /// ws_cell_active | ws_cell_occupied | ws_cell_urgent.
    unsigned flags = 0;
    /// Changes with the name or state (`shell_workspace::rev`).
    unsigned rev = 0;
    /// The workspace (the shell's view index), handed back on a click.
    int id = -1;
};
struct panel_ws_input {
    unsigned count = 0;
    panel_ws_item items[k_max_ws_cells]{};
    /// The cell a button went down on (-1: none).
    int pressed = -1;
};
struct panel_ws_cell {
    int x = 0;
    int w = 0;
    unsigned flags = 0;
    unsigned rev = 0;
};

/// One tray item as the panel draws it.
struct panel_tray_item {
    /// The tray's stable key for the item.
    unsigned key = 0;
    /// Changes with its icon or status.
    unsigned rev = 0;
};
struct panel_tray_input {
    unsigned count = 0;
    panel_tray_item items[k_max_tray_cells]{};
};
struct panel_tray_cell {
    int x = 0;
    int w = 0;
    unsigned key = 0;
    unsigned rev = 0;
    /// cell_hover | cell_pressed.
    unsigned flags = 0;
};
/// The notification indicator's input: shown once the notification server runs.
struct panel_bell {
    bool shown = false;
    /// Do-not-disturb (manual or while the screen is shared): drawn crossed out.
    bool dnd = false;
    unsigned unread = 0;
};
/// The clock's input: the shell's "HH:MM" (24 h), as laid out at `width` logical px in the
/// panel's font, and a revision that changes with it.
struct panel_clock {
    char text[96]{};
    int width = 0;
    unsigned rev = 0;
    unsigned flags = 0;
};
enum panel_bell_flag : unsigned {
    bell_flag_hover = 1u << 0,
    bell_flag_pressed = 1u << 1,
    bell_flag_dnd = 1u << 2,
};

struct panel_cell {
    task_handle task{};
    unsigned label_rev = 0;
    /// The task's icon revision (zlm v6): a new icon repaints the cell.
    unsigned icon_rev = 0;
    int x = 0;
    int w = 0;
    unsigned flags = 0;
};

/// The logout icon's input to a layout.
struct panel_logout {
    bool shown = false; // the compositor supports zlm_shell_v1.request_logout
    bool pressed = false;
};

enum panel_logout_flag : unsigned {
    logout_hover = 1u << 0,
    logout_pressed = 1u << 1,
};

/// The Apps button's input to a layout: `open` latches it while the launcher is up.
struct panel_apps {
    bool shown = false;
    bool open = false;
    bool pressed = false;
};

enum panel_apps_flag : unsigned {
    apps_hover = 1u << 0,
    apps_pressed = 1u << 1,
    apps_open = 1u << 2,
};

/// A notice (a launch that failed while the launcher was closed) shown in place of the
/// task entries; `rev` changes with its text.
struct panel_notice {
    bool shown = false;
    unsigned rev = 0;
};

struct panel_layout {
    /// 0 = nothing (nothing drawn yet).
    int width = 0;
    int height = 0;
    int cell_y = 0;
    int cell_h = 0;
    unsigned count = 0;
    panel_cell cells[k_max_panel_cells]{};
    /// Entries that did not fit, shown as "+hidden" at more_x.
    unsigned hidden = 0;
    int more_x = 0;
    /// The logout button; logout_w 0 = not shown.
    int logout_x = 0;
    int logout_w = 0;
    unsigned logout_flags = 0;
    /// The Apps button; apps_w 0 = not shown.
    int apps_x = 0;
    int apps_w = 0;
    unsigned apps_flags = 0;
    /// The notice cell; notice_w 0 = not shown.
    int notice_x = 0;
    int notice_w = 0;
    unsigned notice_rev = 0;
    /// The screen-capture indicator; capture_w 0 = not shown.
    int capture_x = 0;
    int capture_w = 0;
    /// The workspace indicator; ws_count 0 = not shown (one workspace, or none known).
    unsigned ws_count = 0;
    panel_ws_cell ws[k_max_ws_cells]{};
    /// The tray's cells, right to left from the notification indicator.
    unsigned tray_count = 0;
    panel_tray_cell tray[k_max_tray_cells]{};
    /// The notification indicator; bell_w 0 = not shown.
    int bell_x = 0;
    int bell_w = 0;
    unsigned bell_flags = 0;
    unsigned bell_unread = 0;
    /// The clock, left of the tray; clock_w 0 = not shown.
    int clock_x = 0;
    int clock_w = 0;
    unsigned clock_rev = 0;
    unsigned clock_flags = 0;
};

/// The logical edge of a task entry's icon, left of its title.
inline constexpr int k_panel_icon = 16;
/// Resolves a task's icon at `px`² pixels (premultiplied ARGB8888; null: none — the panel
/// draws a letter tile). The shell's: the icon-theme name, the window's own pixel buffers,
/// the app's desktop entry icon.
struct panel_icons {
    const std::uint32_t* (*resolve)(task_handle h, const task_entry& e, int px, void* user) = nullptr;
    void* user = nullptr;
};

/// Which entries a panel lists (the tasks of its output, M13); null: all of them.
struct panel_filter {
    bool (*keep)(const task_entry& e, void* user) = nullptr;
    void* user = nullptr;
};

/// Lays out `model`'s entries (those `filter` keeps) on a `width`×`height` panel. The
/// pointer (surface coordinates; negative = not on the panel) marks the entry under it as
/// hovered; `pressed` marks the entry a button went down on.
/// `capturing`: some client captures the screen (zlm v6 capture_state): the indicator shows.
/// `ws`: the workspace indicator's cells (null or fewer than two: not shown).
/// `tray`: the tray's items (null: none); `tray_pressed` the key a button went down on.
/// `bell`: the notification indicator (`bell_pressed`: a button is down on it).
/// `clock`: the clock (empty text or width 0: not shown).
void layout_panel(const task_model& model, int width, int height, double pointer_x,
                  double pointer_y, task_handle pressed, panel_layout& out,
                  panel_logout logout = {}, panel_apps apps = {}, panel_notice notice = {},
                  panel_filter filter = {}, bool capturing = false,
                  const panel_ws_input* ws = nullptr, const panel_tray_input* tray = nullptr,
                  unsigned tray_pressed = 0, panel_bell bell = {}, bool bell_pressed = false,
                  panel_clock clock = {});
/// The width of `text` in the panel's font at `scale` (the clock is laid out with it),
/// rounded up to whole logical px.
[[nodiscard]] int panel_text_width(const char* text, float scale);
/// Copies the used part of `from` (the cells in use, not all k_max_panel_cells).
///
/// Also a workaround: Clang 23.1 finds no viable copy assignment for this class here and
/// crashes in codegen when one is explicitly defaulted, so layouts are copied field-wise.
void copy_layout(panel_layout& to, const panel_layout& from);
/// Index of the cell under (x, y), or -1.
[[nodiscard]] int panel_hit(const panel_layout& layout, double x, double y);
/// Whether (x, y) is on the logout button.
[[nodiscard]] bool panel_hit_logout(const panel_layout& layout, double x, double y);
/// Whether (x, y) is on the Apps button.
[[nodiscard]] bool panel_hit_apps(const panel_layout& layout, double x, double y);
/// Whether (x, y) is on the notice.
[[nodiscard]] bool panel_hit_notice(const panel_layout& layout, double x, double y);
/// The workspace cell under (x, y), or -1.
[[nodiscard]] int panel_hit_workspace(const panel_layout& layout, double x, double y);
/// The tray cell under (x, y), or -1.
[[nodiscard]] int panel_hit_tray(const panel_layout& layout, double x, double y);
/// Whether (x, y) is on the notification indicator.
[[nodiscard]] bool panel_hit_bell(const panel_layout& layout, double x, double y);
/// Rects of `to` that differ from `from`: the whole panel when the arrangement changed
/// (size, entry count, any position or width, the "+k" label, a button or the notice
/// appearing), else one rect per entry, button or notice whose content or state changed.
/// Returns how many were written (≤ cap; a full-panel rect when they would not fit).
unsigned panel_diff(const panel_layout& from, const panel_layout& to, lx::rect2i* out,
                    unsigned cap);

// ── The panel ─────────────────────────────────────────────────────────────────

/// What the panel needs from the shell: the UI host context its surface joins (one per
/// UI thread; the panel is an lx.ui tree on a layer surface, ADR-010 Phase 5 P5-6), and the
/// background-effect manager when it is to be translucent over a blur.
/// The lx.ui describer (defined with the drawing): builds the panel's tree; the surface's
/// describer and the re-arms from `request_repaint` both name it.
[[nodiscard]] lx::ui::child panel_describe(lx::ui::build_context& ctx);

/// The panel's background over a blur (`[panel] blur`): the opaque one at 72 %, premultiplied.
inline constexpr std::uint32_t k_panel_translucent_bg = 0xB8161619u;
inline constexpr std::uint32_t k_panel_opaque_bg = 0xFF1E1E22u;

struct panel_globals {
    /// The shell's lx.ui.host context (null: the panel cannot be created).
    lx::ui::host::context* ui = nullptr;
    /// ext_background_effect_manager_v1 when the panel is to be translucent over a blur of
    /// what is behind it (config.toml `[panel] blur`, and the compositor offers blur); null:
    /// opaque. Read at `create`: the surface's buffers are picked there.
    void* background_effect = nullptr;
};

/// What a click on the panel asks the shell for (the panel itself only raises and
/// activates windows and logs out).
enum class panel_action : unsigned char {
    /// The Apps button was clicked: toggle the launcher.
    apps,
    /// A task entry was clicked (the window is being activated).
    task,
    /// A workspace cell was clicked: show workspace `arg` (`panel_ws_item::id`).
    workspace,
    /// The wheel over the indicator: the next (`arg` 1) or previous (-1) workspace.
    workspace_scroll,
};

/// Where the indicator's cells come from (the shell: its ext-workspace view).
struct panel_ws_source {
    void (*fill)(panel_ws_input& out, void* user) = nullptr;
    void* user = nullptr;
};

/// The tray's items, their icons, and the notification indicator (the shell's).
struct panel_widgets_source {
    void (*fill_tray)(panel_tray_input& out, void* user) = nullptr;
    /// The item's icon at `px`² pixels (premultiplied ARGB), or null (a letter tile).
    const std::uint32_t* (*tray_icon)(unsigned key, int px, void* user) = nullptr;
    /// A letter for an item without an icon.
    char (*tray_letter)(unsigned key, void* user) = nullptr;
    panel_bell (*bell)(void* user) = nullptr;
    /// The clock, refreshed by the shell at each minute.
    panel_clock (*clock)(void* user) = nullptr;
    void* user = nullptr;
};

/// Input on panel indicators and the logout icon, for the shell to act on.
struct panel_widget_event {
    enum class kind : unsigned char {
        /// A button released on the tray cell it went down on.
        tray_click,
        /// The wheel over a tray cell (`delta`: discrete steps, positive down).
        tray_scroll,
        /// The pointer moved onto another tray cell (`key` 0: off the tray).
        tray_hover,
        /// A button released on the notification indicator.
        bell_click,
        clock_click,
        logout_click,
    } what = kind::tray_click;
    unsigned key = 0;
    /// BTN_LEFT, BTN_RIGHT, BTN_MIDDLE.
    unsigned button = 0;
    /// The press's serial (activation tokens, menu grabs).
    unsigned serial = 0;
    int delta = 0;
    /// The cell, in panel-surface coordinates (a menu's or tooltip's anchor).
    lx::rect2i cell{};
};

/// The shell's top panel: a wlr-layer-shell surface (layer top, anchored top/left/right,
/// 32 px, exclusive zone 32, no keyboard) drawn as an lx.ui tree on an lx.ui.host surface
/// (ADR-010 Phase 5 P5-6): the Apps button, one entry per window (the focused one
/// highlighted) and the logout button, laid out by `layout_panel` and described into the
/// engine's retained tree. A left click on an entry (press and release on the same entry)
/// asks the compositor to raise and activate that window.
///
/// Input comes through the context's seat router (lx.ui.host :input): the cells are the
/// tree's interactive nodes; their press, release, wheel and hover events are forwarded to
/// the panel's own state machine below, which keeps the press-release and arm-confirm
/// semantics and asks the shell through the same handlers as before.
///
/// Drawing is event driven: nothing runs while nothing changes. Model changes, pointer
/// motion and configures mark the panel dirty; `after_dispatch()` draws at most once per
/// frame — the surface's frame loop (its frame callbacks, its damage ledger) does the rest.
class panel final {
public:
    panel() = default;
    ~panel();
    panel(const panel&) = delete;
    panel& operator=(const panel&) = delete;

    /// Creates the UI surface with the layer role on output slot `output_slot`
    /// (`display::output_at`; ~0u: the compositor picks) and sends its initial commit.
    [[nodiscard]] lx::result<void> create(panel_globals globals, policy_bridge& bridge,
                                          unsigned output_slot = ~0u);
    void destroy();
    /// Call after every dispatch: consumes model changes and draws if needed. With several
    /// panels the shell takes the model's changes once and hands them to each
    /// (`after_dispatch(changes)`).
    void after_dispatch();
    void after_dispatch(unsigned changes);
    /// Lists only the tasks `filter` keeps (the windows on this panel's output).
    void set_filter(panel_filter filter) {
        filter_ = filter;
        dirty_ = true;
    }
    /// The output's windows may have changed (a window moved, an output moved): redraw if
    /// the list differs.
    void filter_changed() { dirty_ = true; }
    /// The clock's minute passed: its text changed, redraw.
    void clock_changed() { dirty_ = true; }
    /// The workspace indicator's source (none: no indicator).
    void set_workspaces(panel_ws_source source) {
        ws_source_ = source;
        dirty_ = true;
    }
    /// The workspaces changed (an ext-workspace `done`): redraw what differs.
    void workspaces_changed() { dirty_ = true; }
    /// The output's name, for the log.
    void set_output_name(const char* name) {
        std::snprintf(output_name_, sizeof(output_name_), "%s", name ? name : "");
    }
    /// Where task icons come from (none: letter tiles).
    void set_icons(panel_icons icons) {
        icons_ = icons;
        dirty_ = true;
    }
    /// The tray and the notification indicator (none: neither is shown).
    using widget_fn = void (*)(const panel_widget_event& e, void* user);
    void set_widgets(panel_widgets_source source, widget_fn fn, void* user) {
        widgets_ = source;
        widget_fn_ = fn;
        widget_user_ = user;
        dirty_ = true;
    }
    /// The tray or the notification indicator changed: redraw what differs.
    void widgets_changed() { dirty_ = true; }
    /// zwlr_layer_surface_v1* (a menu's or tooltip's parent).
    [[nodiscard]] void* layer() const {
        return role_.valid() ? role_.layer().native() : nullptr;
    }
    /// The layer role (a menu's or tooltip's xdg_popup parent).
    [[nodiscard]] lx::ui::host::layer_role& popup_parent() { return role_; }
    /// Keyboard interactivity on_demand while one of its menus is open (the menu's grab
    /// then takes the keyboard), none otherwise. Committed at once.
    void set_menu_keyboard(bool on);
    /// The manager (`ext_background_effect_manager_v1`) the panel is to be translucent
    /// over, or null for opaque. Read at `create` (the surface's buffers are picked there):
    /// a later change takes effect when the panel is created again. Logs once.
    void set_blur(void* manager);
    /// The compositor closed the layer surface (its output went away) or the surface
    /// failed: the shell should exit and be restarted.
    [[nodiscard]] bool closed() const { return closed_; }
    /// The panel has been configured and drawn once.
    [[nodiscard]] bool first_frame_committed() const {
        return surface_.valid() && surface_.configured() && surface_.stats().frames > 0;
    }
    [[nodiscard]] const panel_layout& committed() const { return committed_; }
    /// wl_surface* (for routing pointer events and for activation tokens).
    [[nodiscard]] void* surface() const {
        return surface_.valid() ? surface_.wl().native() : nullptr;
    }
    /// The configured width (the output's, logical).
    [[nodiscard]] int width() const {
        return surface_.valid() ? surface_.logical_size().width : 0;
    }
    /// The output's preferred scale in 120ths, as the panel draws at (120 without
    /// fractional scale).
    [[nodiscard]] unsigned scale120() const {
        return surface_.valid()
                   ? static_cast<unsigned>(surface_.scale() * 120.f + 0.5f)
                   : 120u;
    }

    using action_fn = void (*)(panel_action action, int arg, void* user);
    void set_action_handler(action_fn fn, void* user) {
        action_fn_ = fn;
        action_user_ = user;
    }
    /// Shows the Apps button (and whether the launcher is open).
    void set_apps(bool shown, bool open);
    /// Shows `text` in place of the task entries; null or "" clears it. A click on it
    /// clears it too.
    void set_notice(const char* text);
    [[nodiscard]] bool notice_shown() const { return notice_[0] != '\0'; }

    // ── Pointer, forwarded from the UI tree's cells (the context's seat router) ──
    void pointer_enter(double x, double y);
    void pointer_leave();
    void pointer_motion(double x, double y);
    void pointer_button(unsigned button, bool pressed, unsigned serial = 0);
    /// Wheel steps (discrete; positive: down) — over the indicator, the next workspace; over
    /// a tray item, its Scroll.
    void pointer_scroll(int steps);

    // ── the lx.ui side (implementation below): the describer and the forwarding of the
    // cells' pointer events into the state machine. Public so the node types below the
    // class can carry them as callbacks.
    [[nodiscard]] lx::ui::child build_ui();
    static void cell_notify(void* user);
    static void cell_event(void* user, const lx::ui::pointer_event& e);
    static bool root_mouse(void* user, const lx::ui::mouse_event& e);

private:
    /// Fills the workspace / tray inputs and lays the panel out into `next_` at the
    /// surface's current logical size; `committed_` follows for the input state machine.
    void relayout();
    /// The atlas tile of a task's or tray item's icon (its pixels converted into the
    /// context's atlas; cached per task slot and icon revision).
    [[nodiscard]] lx::gfx::atlas_tile_id icon_tile(unsigned slot, task_handle task,
                                                   unsigned rev, const std::uint32_t* pixels,
                                                   int px);
    void request_repaint();
    void log_layout(const panel_layout& l) const;

    panel_globals g_{};
    /// ext_background_effect_surface_v1* while blurred.
    void* effect_ = nullptr;
    /// The panel's background: opaque, or translucent (premultiplied) over the blur.
    std::uint32_t bg_ = k_panel_opaque_bg;
    policy_bridge* bridge_ = nullptr;
    lx::ui::host::surface surface_{};
    lx::ui::host::layer_role role_{};
    /// One entry per task slot: the icon pixels' tile in the context's atlas.
    struct icon_slot {
        task_handle task{};
        unsigned rev = 0;
        int px = 0;
        lx::gfx::atlas_tile_id tile{};
    };
    static constexpr unsigned k_icon_slots = 64;
    icon_slot icon_tiles_[k_icon_slots]{};
    bool closed_ = false;
    bool dirty_ = false;
    double pointer_x_ = -1.0;
    double pointer_y_ = -1.0;
    task_handle hovered_{};
    task_handle pressed_{};
    bool logout_hovered_ = false;
    bool logout_pressed_ = false;
    unsigned logout_pressed_serial_ = 0;
    bool apps_hovered_ = false;
    bool apps_shown_ = false;
    bool apps_open_ = false;
    bool apps_pressed_ = false;
    bool notice_pressed_ = false;
    char notice_[192]{};
    unsigned notice_rev_ = 0;
    action_fn action_fn_ = nullptr;
    void* action_user_ = nullptr;
    panel_filter filter_{};
    panel_icons icons_{};
    panel_ws_source ws_source_{};
    panel_widgets_source widgets_{};
    widget_fn widget_fn_ = nullptr;
    void* widget_user_ = nullptr;
    panel_tray_input tray_input_{};
    unsigned tray_hovered_ = 0;
    unsigned tray_pressed_ = 0;
    unsigned tray_pressed_button_ = 0;
    unsigned tray_pressed_serial_ = 0;
    bool bell_hovered_ = false;
    bool bell_pressed_ = false;
    unsigned bell_pressed_button_ = 0;
    unsigned bell_pressed_serial_ = 0;
    bool clock_hovered_ = false;
    bool clock_pressed_ = false;
    unsigned clock_pressed_button_ = 0;
    unsigned clock_pressed_serial_ = 0;
    /// The clock's text of the layout being drawn.
    char clock_text_[sizeof(panel_clock::text)]{};
    bool menu_keyboard_ = false;
    /// The indicator's input of the layout being drawn (its names).
    panel_ws_input ws_input_{};
    int ws_pressed_ = -1;
    /// The indicator as last logged.
    char ws_logged_[160]{};
    char output_name_[32]{};
    panel_layout committed_{};
    panel_layout next_{};
    /// The layout as last logged (`layout idx=…` lines, on an arrangement change).
    panel_layout logged_{};
    /// The layout of the last frame: what an arrangement change is measured against.
    panel_layout frame_{};
    /// The last frame answered an arrangement change (a whole-strip repaint).
    bool arranged_ = false;
    /// The configure size as last logged (`configure w=… h=…`, −1: none yet).
    int configure_logged_w_ = -1;
    int configure_logged_h_ = -1;

    friend lx::ui::child panel_describe(lx::ui::build_context&);
};

} // namespace lx::shell

// ── Layout ───────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool in_box(double px, double py, int x, int w, int y, int h) {
    return w > 0 && px >= x && px < x + w && py >= y && py < y + h;
}

constexpr lx::trace::category log_panel{"shell.panel"};

/// The panel whose tree is being described (the UI thread builds one frame at a time).
lx::shell::panel* g_describing = nullptr;

/// Names the panel whose tree is described for one frame.
struct describing_guard {
    explicit describing_guard(lx::shell::panel* p) : previous_{g_describing} { g_describing = p; }
    ~describing_guard() { g_describing = previous_; }
    describing_guard(const describing_guard&) = delete;
    describing_guard& operator=(const describing_guard&) = delete;

private:
    lx::shell::panel* previous_;
};

/// `abgr` (0xAARRGGBB, the panels' old constants) as a straight-alpha colour.
[[nodiscard]] constexpr lx::color panel_color(std::uint32_t argb) {
    return lx::color::rgb(static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                          static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                          static_cast<float>(argb & 0xFF) / 255.f,
                          static_cast<float>((argb >> 24) & 0xFF) / 255.f);
}

constexpr lx::color k_text = panel_color(0xFFE6E6E6u);
constexpr lx::color k_text_focused = panel_color(0xFFFFFFFFu);
constexpr lx::color k_text_more = panel_color(0xFFA8A8ACu);
constexpr lx::color k_text_warning = panel_color(0xFFF0B45Au);
constexpr lx::text::font_desc k_panel_font{"Sans", 10.f, lx::text::weight::normal,
                                           lx::text::style::normal};
constexpr lx::text::font_desc k_panel_font_bold{"Sans", 8.f, lx::text::weight::bold,
                                                lx::text::style::normal};

/// wl_pointer button codes, as the panel's state machine has always spoken them.
constexpr unsigned k_btn_left = 0x110;
constexpr unsigned k_btn_right = 0x111;
constexpr unsigned k_btn_middle = 0x112;

[[nodiscard]] unsigned button_code(lx::ui::mouse_button b) {
    switch (b) {
    case lx::ui::mouse_button::right: return k_btn_right;
    case lx::ui::mouse_button::middle: return k_btn_middle;
    default: return k_btn_left;
    }
}

} // namespace

// ── the UI nodes the panel is built from ─────────────────────────────────────

namespace {

/// A cell of the panel: a control (it takes hover and press, so the engine paints the
/// states) that forwards every pointer event and interaction change to the panel's own
/// state machine — which keeps the press-release, wheel and arm-confirm semantics.
struct panel_cell_props {
    lx::ui::control_props control{};
    /// Hover or press changed on the cell.
    void (*notify)(void* user) = nullptr;
    /// A pointer event on the cell (press, release, wheel, secondary, middle).
    void (*event)(void* user, const lx::ui::pointer_event& e) = nullptr;
    void* user = nullptr;
};

class panel_cell_node final : public lx::ui::control_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "panel_cell"; }

    bool apply(const panel_cell_props& p) {
        props_ = p;
        return lx::ui::control_node::apply(p.control);
    }

    void on_interaction_changed() override {
        control_node::on_interaction_changed();
        if (props_.notify)
            props_.notify(props_.user);
    }

    bool on_pointer(const lx::ui::pointer_event& e) override {
        if (props_.event)
            props_.event(props_.user, e);
        return true;
    }

private:
    panel_cell_props props_{};
};

/// The panel's root: a flex container that receives what no cell took (a left press or
/// release on the panel's background, a secondary or middle button's release) and forwards
/// it to the state machine as well.
struct panel_root_props {
    lx::ui::flex_props flex{};
    /// A mouse event nothing else took; false lets it go.
    bool (*mouse)(void* user, const lx::ui::mouse_event& e) = nullptr;
    void* user = nullptr;
};

class panel_root_node final : public lx::ui::flex_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "panel_root"; }

    bool apply(const panel_root_props& p) {
        props_ = p;
        return lx::ui::flex_node::apply(p.flex);
    }

    bool on_mouse(lx::ui::mouse_event e) override {
        return props_.mouse ? props_.mouse(props_.user, e) : false;
    }

private:
    panel_root_props props_{};
};

} // namespace

// ── input forwarding (the context's seat router → the state machine) ─────────

void lx::shell::panel::pointer_enter(double x, double y) { pointer_motion(x, y); }

void lx::shell::panel::pointer_leave() {
    if (pressed_ || logout_pressed_ || apps_pressed_)
        dirty_ = true;
    pressed_ = {};
    logout_pressed_ = false;
    logout_pressed_serial_ = 0;
    apps_pressed_ = false;
    notice_pressed_ = false;
    if (ws_pressed_ >= 0 || tray_pressed_ || bell_pressed_ || clock_pressed_ || clock_hovered_)
        dirty_ = true;
    ws_pressed_ = -1;
    tray_pressed_ = 0;
    bell_pressed_ = false;
    clock_hovered_ = false;
    clock_pressed_ = false;
    clock_pressed_button_ = 0;
    clock_pressed_serial_ = 0;
    pointer_motion(-1.0, -1.0);
}

void lx::shell::panel::pointer_motion(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
    // Hover is judged against what is on screen; a redraw only when it moves to another
    // cell, never per motion event. (The hover painting itself is the engine's: the cells
    // are interactive nodes and the engine paints their states.)
    const int hit = panel_hit(committed_, x, y);
    const task_handle under = hit >= 0 ? committed_.cells[hit].task : task_handle{};
    if (!(under == hovered_)) {
        hovered_ = under;
        dirty_ = true;
    }
    const bool on_logout = panel_hit_logout(committed_, x, y);
    if (on_logout != logout_hovered_) {
        logout_hovered_ = on_logout;
        dirty_ = true;
    }
    const bool on_apps = panel_hit_apps(committed_, x, y);
    if (on_apps != apps_hovered_) {
        apps_hovered_ = on_apps;
        dirty_ = true;
    }
    // The tray: hover redraws, and the shell hears which item (its tooltip).
    {
        const int on_tray = panel_hit_tray(committed_, x, y);
        const unsigned key = on_tray >= 0 ? committed_.tray[on_tray].key : 0u;
        if (key != tray_hovered_) {
            tray_hovered_ = key;
            dirty_ = true;
            if (widget_fn_) {
                panel_widget_event e{};
                e.what = panel_widget_event::kind::tray_hover;
                e.key = key;
                if (on_tray >= 0)
                    e.cell = {committed_.tray[on_tray].x, committed_.cell_y,
                              committed_.tray[on_tray].w, committed_.cell_h};
                widget_fn_(e, widget_user_);
            }
        }
        const bool on_bell = panel_hit_bell(committed_, x, y);
        if (on_bell != bell_hovered_) {
            bell_hovered_ = on_bell;
            dirty_ = true;
        }
        const bool on_clock = committed_.clock_w > 0 && x >= committed_.clock_x &&
                              x < committed_.clock_x + committed_.clock_w &&
                              y >= committed_.cell_y && y < committed_.cell_y + committed_.cell_h;
        if (on_clock != clock_hovered_) {
            clock_hovered_ = on_clock;
            dirty_ = true;
        }
    }
    // Workspace cells hover (a redraw only when the cell under the pointer changes).
    if (committed_.ws_count > 0) {
        const int on_ws = panel_hit_workspace(committed_, x, y);
        for (unsigned i = 0; i < committed_.ws_count; ++i)
            if (((committed_.ws[i].flags & ws_cell_hover) != 0) != (static_cast<int>(i) == on_ws))
                dirty_ = true;
    }
}

void lx::shell::panel::pointer_scroll(int steps) {
    if (steps == 0)
        return;
    if (const int t = panel_hit_tray(committed_, pointer_x_, pointer_y_); t >= 0 && widget_fn_) {
        panel_widget_event e{};
        e.what = panel_widget_event::kind::tray_scroll;
        e.key = committed_.tray[t].key;
        e.delta = steps;
        widget_fn_(e, widget_user_);
        return;
    }
    if (panel_hit_workspace(committed_, pointer_x_, pointer_y_) < 0 || !action_fn_)
        return;
    action_fn_(panel_action::workspace_scroll, steps > 0 ? 1 : -1, action_user_);
}

void lx::shell::panel::pointer_button(unsigned button, bool down, unsigned serial) {
    const double px = pointer_x_, py = pointer_y_;
    // The tray and the notification indicator take every button: press and release on the
    // same one is a click, handed to the shell with the press's serial.
    {
        const int on_tray = panel_hit_tray(committed_, px, py);
        if (down && on_tray >= 0 && widget_fn_) {
            tray_pressed_ = committed_.tray[on_tray].key;
            tray_pressed_button_ = button;
            tray_pressed_serial_ = serial;
            dirty_ = true;
            return;
        }
        if (!down && tray_pressed_) {
            const unsigned key = tray_pressed_;
            tray_pressed_ = 0;
            dirty_ = true;
            if (button == tray_pressed_button_ && on_tray >= 0 &&
                committed_.tray[on_tray].key == key && widget_fn_) {
                panel_widget_event e{};
                e.what = panel_widget_event::kind::tray_click;
                e.key = key;
                e.button = button;
                e.serial = tray_pressed_serial_;
                e.cell = {committed_.tray[on_tray].x, committed_.cell_y, committed_.tray[on_tray].w,
                          committed_.cell_h};
                widget_fn_(e, widget_user_);
            }
            return;
        }
        const bool on_bell = panel_hit_bell(committed_, px, py);
        const bool on_clock = committed_.clock_w > 0 && px >= committed_.clock_x &&
                              px < committed_.clock_x + committed_.clock_w &&
                              py >= committed_.cell_y && py < committed_.cell_y + committed_.cell_h;
        if (down && on_clock && widget_fn_) {
            clock_pressed_ = true;
            clock_pressed_button_ = button;
            clock_pressed_serial_ = serial;
            dirty_ = true;
            return;
        }
        if (!down && clock_pressed_) {
            clock_pressed_ = false;
            dirty_ = true;
            if (on_clock && button == clock_pressed_button_ && widget_fn_) {
                panel_widget_event e{};
                e.what = panel_widget_event::kind::clock_click;
                e.button = button;
                e.serial = clock_pressed_serial_;
                e.cell = {committed_.clock_x, committed_.cell_y, committed_.clock_w, committed_.cell_h};
                widget_fn_(e, widget_user_);
            }
            return;
        }
        if (down && on_bell && widget_fn_) {
            bell_pressed_ = true;
            bell_pressed_button_ = button;
            bell_pressed_serial_ = serial;
            dirty_ = true;
            return;
        }
        if (!down && bell_pressed_) {
            bell_pressed_ = false;
            dirty_ = true;
            if (on_bell && button == bell_pressed_button_ && widget_fn_) {
                panel_widget_event e{};
                e.what = panel_widget_event::kind::bell_click;
                e.button = button;
                e.serial = bell_pressed_serial_;
                e.cell = {committed_.bell_x, committed_.cell_y, committed_.bell_w,
                          committed_.cell_h};
                widget_fn_(e, widget_user_);
            }
            return;
        }
    }
    if (button != k_btn_left)
        return;
    const bool on_logout = panel_hit_logout(committed_, px, py);
    if (down && on_logout) {
        logout_pressed_ = true;
        logout_pressed_serial_ = serial;
        dirty_ = true;
        return;
    }
    if (!down && logout_pressed_) {
        logout_pressed_ = false;
        dirty_ = true;
        if (on_logout && widget_fn_) {
            panel_widget_event e{};
            e.what = panel_widget_event::kind::logout_click;
            e.button = button;
            e.serial = logout_pressed_serial_;
            e.cell = {committed_.logout_x, committed_.cell_y, committed_.logout_w,
                      committed_.cell_h};
            widget_fn_(e, widget_user_);
        }
        logout_pressed_serial_ = 0;
        return;
    }
    // Apps: press and release on the button toggles the launcher.
    const bool on_apps = panel_hit_apps(committed_, px, py);
    if (down && on_apps) {
        apps_pressed_ = true;
        dirty_ = true;
        return;
    }
    if (!down && apps_pressed_) {
        apps_pressed_ = false;
        dirty_ = true;
        if (on_apps && action_fn_)
            action_fn_(panel_action::apps, 0, action_user_);
        return;
    }
    // A workspace cell: press and release on the same cell shows that workspace.
    const int on_ws = panel_hit_workspace(committed_, px, py);
    if (down && on_ws >= 0) {
        ws_pressed_ = on_ws;
        dirty_ = true;
        return;
    }
    if (!down && ws_pressed_ >= 0) {
        const int was = ws_pressed_;
        ws_pressed_ = -1;
        dirty_ = true;
        if (on_ws == was && action_fn_ && static_cast<unsigned>(was) < ws_input_.count)
            action_fn_(panel_action::workspace, ws_input_.items[was].id, action_user_);
        return;
    }
    // The notice: a click dismisses it.
    const bool on_notice = panel_hit_notice(committed_, px, py);
    if (down && on_notice) {
        notice_pressed_ = true;
        return;
    }
    if (!down && notice_pressed_) {
        notice_pressed_ = false;
        if (on_notice)
            set_notice(nullptr);
        return;
    }
    const int hit = panel_hit(committed_, px, py);
    const task_handle under = hit >= 0 ? committed_.cells[hit].task : task_handle{};
    if (down) {
        pressed_ = under;
        dirty_ = true;
        return;
    }
    const task_handle pressed = pressed_;
    pressed_ = {};
    dirty_ = true;
    if (!pressed || !(pressed == under) || !bridge_)
        return;
    if (log_panel.enabled(lx::trace::level::debug)) {
        const task_entry* e = bridge_->model().get(pressed);
        log_panel.debug("click idx={} handle={} title=\"{}\"", hit, pressed.index,
                        e ? e->title : "");
    }
    if (action_fn_)
        action_fn_(panel_action::task, 0, action_user_);
    // Raise, then activate. The compositor's activate raises too; the panel still says
    // both so its intent stays explicit if focus and stacking policy ever separate.
    bridge_->request_raise(pressed);
    bridge_->request_activate(pressed);
}

namespace {

} // namespace

// ── input forwarding (the context's seat router → the state machine) ─────────

/// A cell's hover or press moved: the panel follows the pointer (the engine has already
/// put it where the event says).
void lx::shell::panel::cell_notify(void* user) {
    auto& p = *static_cast<panel*>(user);
    if (!p.bridge_ || !p.surface_.valid())
        return;
    if (p.surface_.input().pointer_inside()) {
        const lx::point2i at = p.surface_.root().pointer().position();
        p.pointer_motion(static_cast<double>(at.x), static_cast<double>(at.y));
    } else {
        p.pointer_leave();
    }
}

/// A pointer event on a cell: press, release, wheel, secondary and middle go to the panel's
/// state machine (its press-release and arm-confirm semantics are its own).
void lx::shell::panel::cell_event(void* user, const lx::ui::pointer_event& e) {
    auto& p = *static_cast<panel*>(user);
    if (!p.bridge_)
        return;
    const unsigned serial = p.g_.ui ? p.g_.ui->last_press_serial() : 0;
    switch (e.kind) {
    case lx::ui::pointer_kind::press:
        p.pointer_button(button_code(e.button), true, serial);
        break;
    case lx::ui::pointer_kind::release:
        p.pointer_button(button_code(e.button), false, 0);
        break;
    case lx::ui::pointer_kind::secondary:
        p.pointer_button(k_btn_right, true, serial);
        break;
    case lx::ui::pointer_kind::middle:
        p.pointer_button(k_btn_middle, true, serial);
        break;
    case lx::ui::pointer_kind::wheel:
        // Discrete steps, positive down, as the state machine has always taken them.
        p.pointer_scroll(e.dy > 0.f ? 1 : -1);
        break;
    default:
        break;
    }
}

/// What no cell took (a press or release on the panel's background, a secondary or middle
/// button's release): to the state machine the same way.
bool lx::shell::panel::root_mouse(void* user, const lx::ui::mouse_event& e) {
    auto& p = *static_cast<panel*>(user);
    if (!p.bridge_)
        return false;
    const unsigned serial = p.g_.ui ? p.g_.ui->last_press_serial() : 0;
    p.pointer_button(button_code(e.button), e.pressed, serial);
    return true;
}

void lx::shell::layout_panel(const task_model& model, int width, int height, double px,
                             double py, task_handle pressed, panel_layout& out,
                             panel_logout logout, panel_apps apps, panel_notice notice,
                             panel_filter filter, bool capturing, const panel_ws_input* ws,
                             const panel_tray_input* tray, unsigned tray_pressed, panel_bell bell,
                             bool bell_pressed, panel_clock clock) {
    out.width = width;
    out.height = height;
    out.cell_h = height < k_entry_height ? height : k_entry_height;
    out.cell_y = (height - out.cell_h) / 2;
    out.count = 0;
    out.hidden = 0;
    out.more_x = 0;
    out.logout_x = 0;
    out.logout_w = 0;
    out.logout_flags = 0;
    out.apps_x = 0;
    out.apps_w = 0;
    out.apps_flags = 0;
    out.notice_x = 0;
    out.notice_w = 0;
    out.notice_rev = 0;
    out.capture_x = 0;
    out.capture_w = 0;
    out.ws_count = 0;
    out.tray_count = 0;
    out.bell_x = 0;
    out.bell_w = 0;
    out.bell_flags = 0;
    out.bell_unread = 0;
    out.clock_x = 0;
    out.clock_w = 0;
    out.clock_rev = 0;
    out.clock_flags = 0;
    // The logout button takes the right end, the Apps button the left; entries share
    // what is left.
    int reserve = 0;
    if (logout.shown && width >= 2 * k_panel_pad + k_logout_width + k_entry_min_width) {
        out.logout_w = k_logout_width;
        out.logout_x = width - k_panel_pad - k_logout_width;
        reserve = k_logout_width + k_panel_gap;
        if (logout.pressed)
            out.logout_flags |= logout_pressed;
        if (in_box(px, py, out.logout_x, out.logout_w, out.cell_y, out.cell_h))
            out.logout_flags |= logout_hover;
    }
    // The capture indicator, left of the logout button (or at the right end).
    if (capturing && width >= 2 * k_panel_pad + reserve + k_capture_width + k_entry_min_width) {
        out.capture_w = k_capture_width;
        out.capture_x = (out.logout_w > 0 ? out.logout_x - k_panel_gap : width - k_panel_pad) -
                        k_capture_width;
        reserve += k_capture_width + k_panel_gap;
    }
    // The notification indicator, then the tray, right to left from there.
    int right = width - k_panel_pad - reserve;
    if (bell.shown) {
        int w = k_bell_width;
        if (bell.unread > 0)
            w += 6 + 7 * (bell.unread > 99 ? 3 : bell.unread > 9 ? 2 : 1);
        if (width >= 2 * k_panel_pad + reserve + w + k_entry_min_width) {
            out.bell_w = w;
            out.bell_x = right - w;
            out.bell_unread = bell.unread;
            out.bell_flags = (bell.dnd ? bell_flag_dnd : 0u) | (bell_pressed ? bell_flag_pressed : 0u);
            if (in_box(px, py, out.bell_x, out.bell_w, out.cell_y, out.cell_h))
                out.bell_flags |= bell_flag_hover;
            reserve += w + k_panel_gap;
            right -= w + k_panel_gap;
        }
    }
    if (tray) {
        for (unsigned i = 0; i < tray->count && i < k_max_tray_cells; ++i) {
            if (width < 2 * k_panel_pad + reserve + k_tray_cell + k_entry_min_width)
                break;
            panel_tray_cell& c = out.tray[out.tray_count++];
            c.w = k_tray_cell;
            c.x = right - k_tray_cell;
            c.key = tray->items[i].key;
            c.rev = tray->items[i].rev;
            c.flags = 0;
            if (tray_pressed && c.key == tray_pressed)
                c.flags |= cell_pressed;
            if (in_box(px, py, c.x, c.w, out.cell_y, out.cell_h))
                c.flags |= cell_hover;
            reserve += k_tray_cell + k_tray_gap;
            right -= k_tray_cell + k_tray_gap;
        }
        if (out.tray_count > 0)
            reserve += k_panel_gap - k_tray_gap;
    }
    // The clock, left of whatever the right end holds (the tray, the bell, the logout
    // button): status icons keep the corner, as GNOME's and KDE's panels place it.
    if (clock.width > 0 && clock.text[0] &&
        width >= 2 * k_panel_pad + reserve + clock.width + k_panel_gap + k_entry_min_width) {
        out.clock_w = clock.width;
        out.clock_x = right - clock.width;
        out.clock_rev = clock.rev;
        out.clock_flags = clock.flags;
        reserve += clock.width + k_panel_gap;
    }
    int start = k_panel_pad;
    if (apps.shown && width >= 2 * k_panel_pad + reserve + k_apps_width + k_entry_min_width) {
        out.apps_x = k_panel_pad;
        out.apps_w = k_apps_width;
        start += k_apps_width + k_panel_gap;
        reserve += k_apps_width + k_panel_gap;
        if (apps.open)
            out.apps_flags |= apps_open;
        if (apps.pressed)
            out.apps_flags |= apps_pressed;
        if (in_box(px, py, out.apps_x, out.apps_w, out.cell_y, out.cell_h))
            out.apps_flags |= apps_hover;
    }
    // The workspace indicator, right of the Apps button: one cell per workspace, hidden while
    // there is only one (dynamic mode with no window anywhere yet).
    if (ws && ws->count >= 2) {
        const unsigned n = ws->count < k_max_ws_cells ? ws->count : k_max_ws_cells;
        int total = 0;
        int widths[k_max_ws_cells];
        for (unsigned i = 0; i < n; ++i) {
            const int len = static_cast<int>(std::strlen(ws->items[i].name));
            int w = 12 + 7 * len;
            w = w < k_ws_cell_min ? k_ws_cell_min : (w > k_ws_cell_max ? k_ws_cell_max : w);
            widths[i] = w;
            total += w + (i ? k_ws_gap : 0);
        }
        if (width >= 2 * k_panel_pad + reserve + total + k_panel_gap + k_entry_min_width) {
            int x = start;
            for (unsigned i = 0; i < n; ++i) {
                panel_ws_cell& c = out.ws[i];
                c.x = x;
                c.w = widths[i];
                c.rev = ws->items[i].rev;
                c.flags = ws->items[i].flags & (ws_cell_active | ws_cell_occupied | ws_cell_urgent);
                if (static_cast<int>(i) == ws->pressed)
                    c.flags |= ws_cell_pressed;
                if (in_box(px, py, c.x, c.w, out.cell_y, out.cell_h))
                    c.flags |= ws_cell_hover;
                x += widths[i] + k_ws_gap;
            }
            out.ws_count = n;
            start += total + k_panel_gap;
            reserve += total + k_panel_gap;
        }
    }
    const int avail = width - 2 * k_panel_pad - reserve;
    if (notice.shown && avail > 0) {
        out.notice_x = start;
        out.notice_w = avail;
        out.notice_rev = notice.rev;
        return;
    }
    task_handle order[k_max_tasks];
    unsigned n = model.ordered(order, k_max_tasks);
    if (filter.keep) {
        unsigned kept = 0;
        for (unsigned i = 0; i < n; ++i)
            if (const task_entry* e = model.get(order[i]); e && filter.keep(*e, filter.user))
                order[kept++] = order[i];
        n = kept;
    }
    // A modal dialog is not listed beside the window it blocks: clicking that window's entry
    // activates the dialog (the compositor redirects focus), as GNOME's and KDE's task bars do.
    {
        unsigned kept = 0;
        for (unsigned i = 0; i < n; ++i)
            if (const task_entry* e = model.get(order[i]); e && !task_is_modal_child(model, *e))
                order[kept++] = order[i];
        n = kept;
    }
    if (n == 0 || avail <= 0)
        return;
    const int ni = static_cast<int>(n);
    unsigned visible = n;
    int w = k_entry_min_width;
    if (ni * k_entry_min_width + (ni - 1) * k_panel_gap <= avail) {
        w = (avail - (ni - 1) * k_panel_gap) / ni;
        w = w < k_entry_min_width ? k_entry_min_width
                                  : (w > k_entry_max_width ? k_entry_max_width : w);
    } else {
        const int m = (avail - k_panel_more_width) / (k_entry_min_width + k_panel_gap);
        visible = m > 0 ? static_cast<unsigned>(m) : 0u;
    }
    if (visible > k_max_panel_cells)
        visible = k_max_panel_cells;
    const task_handle focused = model.focused();
    for (unsigned i = 0; i < visible; ++i) {
        panel_cell& c = out.cells[i];
        c.task = order[i];
        const task_entry* e = model.get(order[i]);
        c.label_rev = e ? e->label_rev : 0;
        c.icon_rev = e ? e->icon_rev : 0;
        c.x = start + static_cast<int>(i) * (w + k_panel_gap);
        c.w = w;
        c.flags = 0;
        if (c.task == focused)
            c.flags |= cell_focused;
        if (e && (e->flags & task_flag_demands_attention))
            c.flags |= cell_attention;
        if (e && (e->flags & task_flag_not_responding))
            c.flags |= cell_not_responding;
        if (e && e->state == k_task_state_minimized)
            c.flags |= cell_minimized;
        if (pressed && c.task == pressed)
            c.flags |= cell_pressed;
        if (in_box(px, py, c.x, c.w, out.cell_y, out.cell_h))
            c.flags |= cell_hover;
    }
    out.count = visible;
    out.hidden = n - visible;
    if (out.hidden)
        out.more_x = start + static_cast<int>(visible) * (w + k_panel_gap);
}

int lx::shell::panel_text_width(const char* text, float scale) {
    if (!text || !text[0])
        return 0;
    lx::text::font_desc f = k_panel_font;
    f.size_pt = static_cast<float>(f.size_pt * scale);
    lx::text::shaped_line line{};
    if (!lx::text::shape_line(text, f, line))
        return 0;
    return static_cast<int>(std::ceil(line.width_px / scale));
}

void lx::shell::copy_layout(panel_layout& to, const panel_layout& from) {
    to.width = from.width;
    to.height = from.height;
    to.cell_y = from.cell_y;
    to.cell_h = from.cell_h;
    to.count = from.count;
    to.hidden = from.hidden;
    to.more_x = from.more_x;
    to.logout_x = from.logout_x;
    to.logout_w = from.logout_w;
    to.logout_flags = from.logout_flags;
    to.apps_x = from.apps_x;
    to.apps_w = from.apps_w;
    to.apps_flags = from.apps_flags;
    to.notice_x = from.notice_x;
    to.notice_w = from.notice_w;
    to.notice_rev = from.notice_rev;
    to.capture_x = from.capture_x;
    to.capture_w = from.capture_w;
    to.tray_count = from.tray_count;
    for (unsigned i = 0; i < from.tray_count && i < k_max_tray_cells; ++i)
        to.tray[i] = from.tray[i];
    to.bell_x = from.bell_x;
    to.bell_w = from.bell_w;
    to.bell_flags = from.bell_flags;
    to.bell_unread = from.bell_unread;
    to.clock_x = from.clock_x;
    to.clock_w = from.clock_w;
    to.clock_rev = from.clock_rev;
    to.clock_flags = from.clock_flags;
    to.ws_count = from.ws_count;
    for (unsigned i = 0; i < from.ws_count && i < k_max_ws_cells; ++i) {
        to.ws[i].x = from.ws[i].x;
        to.ws[i].w = from.ws[i].w;
        to.ws[i].flags = from.ws[i].flags;
        to.ws[i].rev = from.ws[i].rev;
    }
    for (unsigned i = 0; i < from.count && i < k_max_panel_cells; ++i) {
        panel_cell& d = to.cells[i];
        const panel_cell& c = from.cells[i];
        d.task.index = c.task.index;
        d.task.gen = c.task.gen;
        d.label_rev = c.label_rev;
        d.icon_rev = c.icon_rev;
        d.x = c.x;
        d.w = c.w;
        d.flags = c.flags;
    }
}

int lx::shell::panel_hit(const panel_layout& l, double x, double y) {
    if (y < l.cell_y || y >= l.cell_y + l.cell_h)
        return -1;
    for (unsigned i = 0; i < l.count; ++i)
        if (x >= l.cells[i].x && x < l.cells[i].x + l.cells[i].w)
            return static_cast<int>(i);
    return -1;
}

bool lx::shell::panel_hit_logout(const panel_layout& l, double x, double y) {
    return in_box(x, y, l.logout_x, l.logout_w, l.cell_y, l.cell_h);
}

bool lx::shell::panel_hit_apps(const panel_layout& l, double x, double y) {
    return in_box(x, y, l.apps_x, l.apps_w, l.cell_y, l.cell_h);
}

bool lx::shell::panel_hit_notice(const panel_layout& l, double x, double y) {
    return in_box(x, y, l.notice_x, l.notice_w, l.cell_y, l.cell_h);
}

int lx::shell::panel_hit_workspace(const panel_layout& l, double x, double y) {
    for (unsigned i = 0; i < l.ws_count; ++i)
        if (in_box(x, y, l.ws[i].x, l.ws[i].w, l.cell_y, l.cell_h))
            return static_cast<int>(i);
    return -1;
}

int lx::shell::panel_hit_tray(const panel_layout& l, double x, double y) {
    for (unsigned i = 0; i < l.tray_count; ++i)
        if (in_box(x, y, l.tray[i].x, l.tray[i].w, l.cell_y, l.cell_h))
            return static_cast<int>(i);
    return -1;
}

bool lx::shell::panel_hit_bell(const panel_layout& l, double x, double y) {
    return in_box(x, y, l.bell_x, l.bell_w, l.cell_y, l.cell_h);
}

unsigned lx::shell::panel_diff(const panel_layout& a, const panel_layout& b, lx::rect2i* out,
                               unsigned cap) {
    if (cap == 0 || b.width <= 0 || b.height <= 0)
        return 0;
    const lx::rect2i whole{0, 0, b.width, b.height};
    bool arrangement = a.width != b.width || a.height != b.height || a.count != b.count ||
                       a.hidden != b.hidden || a.more_x != b.more_x || a.cell_y != b.cell_y ||
                       a.cell_h != b.cell_h || a.logout_x != b.logout_x ||
                       a.logout_w != b.logout_w || a.apps_x != b.apps_x ||
                       a.apps_w != b.apps_w || a.notice_x != b.notice_x ||
                       a.notice_w != b.notice_w || a.capture_x != b.capture_x ||
                       a.capture_w != b.capture_w || a.ws_count != b.ws_count ||
                       a.tray_count != b.tray_count || a.bell_x != b.bell_x ||
                       a.bell_w != b.bell_w;
    for (unsigned i = 0; !arrangement && i < b.count; ++i)
        arrangement = a.cells[i].x != b.cells[i].x || a.cells[i].w != b.cells[i].w;
    for (unsigned i = 0; !arrangement && i < b.ws_count; ++i)
        arrangement = a.ws[i].x != b.ws[i].x || a.ws[i].w != b.ws[i].w;
    for (unsigned i = 0; !arrangement && i < b.tray_count; ++i)
        arrangement = a.tray[i].x != b.tray[i].x || a.tray[i].w != b.tray[i].w;
    if (arrangement) {
        out[0] = whole;
        return 1;
    }
    unsigned n = 0;
    auto add = [&](lx::rect2i r) {
        if (n == cap) {
            out[0] = whole;
            n = 1;
            return false;
        }
        out[n++] = r;
        return true;
    };
    for (unsigned i = 0; i < b.count; ++i) {
        const panel_cell& x = a.cells[i];
        const panel_cell& y = b.cells[i];
        if (x.task == y.task && x.label_rev == y.label_rev && x.icon_rev == y.icon_rev &&
            x.flags == y.flags)
            continue;
        if (!add({y.x, b.cell_y, y.w, b.cell_h}))
            return 1;
    }
    if (b.logout_w > 0 && a.logout_flags != b.logout_flags &&
        !add({b.logout_x, b.cell_y, b.logout_w, b.cell_h}))
        return 1;
    if (b.apps_w > 0 && a.apps_flags != b.apps_flags &&
        !add({b.apps_x, b.cell_y, b.apps_w, b.cell_h}))
        return 1;
    if (b.notice_w > 0 && a.notice_rev != b.notice_rev &&
        !add({b.notice_x, b.cell_y, b.notice_w, b.cell_h}))
        return 1;
    for (unsigned i = 0; i < b.ws_count; ++i)
        if ((a.ws[i].flags != b.ws[i].flags || a.ws[i].rev != b.ws[i].rev) &&
            !add({b.ws[i].x, b.cell_y, b.ws[i].w, b.cell_h}))
            return 1;
    for (unsigned i = 0; i < b.tray_count; ++i)
        if ((a.tray[i].key != b.tray[i].key || a.tray[i].rev != b.tray[i].rev ||
             a.tray[i].flags != b.tray[i].flags) &&
            !add({b.tray[i].x, b.cell_y, b.tray[i].w, b.cell_h}))
            return 1;
    if (b.bell_w > 0 && (a.bell_flags != b.bell_flags || a.bell_unread != b.bell_unread) &&
        !add({b.bell_x, b.cell_y, b.bell_w, b.cell_h}))
        return 1;
    if (a.clock_w != b.clock_w || a.clock_x != b.clock_x || a.clock_rev != b.clock_rev ||
        a.clock_flags != b.clock_flags) {
        if (a.clock_w > 0 && !add({a.clock_x, a.cell_y, a.clock_w, a.cell_h}))
            return 1;
        if (b.clock_w > 0 && !add({b.clock_x, b.cell_y, b.clock_w, b.cell_h}))
            return 1;
    }
    return n;
}

// ── Drawing: the panel as an lx.ui tree ──────────────────────────────────────

void lx::shell::panel::relayout() {
    ws_input_.count = 0;
    if (ws_source_.fill)
        ws_source_.fill(ws_input_, ws_source_.user);
    ws_input_.pressed = ws_pressed_;
    tray_input_.count = 0;
    if (widgets_.fill_tray)
        widgets_.fill_tray(tray_input_, widgets_.user);
    const panel_bell bell = widgets_.bell ? widgets_.bell(widgets_.user) : panel_bell{};
    panel_clock clock = widgets_.clock ? widgets_.clock(widgets_.user) : panel_clock{};
    clock.flags |= (clock_hovered_ ? bell_flag_hover : 0u) |
                   (clock_pressed_ ? bell_flag_pressed : 0u);
    std::snprintf(clock_text_, sizeof(clock_text_), "%s", clock.text);
    layout_panel(bridge_->model(), width(), k_panel_height, pointer_x_, pointer_y_, pressed_,
                 next_, panel_logout{bridge_->can_logout(), logout_pressed_},
                 panel_apps{apps_shown_, apps_open_, apps_pressed_},
                 panel_notice{notice_[0] != '\0', notice_rev_}, filter_,
                 bridge_->capture_active(), &ws_input_, &tray_input_, tray_pressed_, bell,
                 bell_pressed_, clock);
    copy_layout(committed_, next_);
}

lx::gfx::atlas_tile_id lx::shell::panel::icon_tile(unsigned slot, task_handle task, unsigned rev,
                                                   const std::uint32_t* pixels, int px) {
    if (!pixels || px <= 0 || !surface_.valid())
        return {};
    icon_slot& s = icon_tiles_[slot % k_icon_slots];
    lx::gfx::atlas_tile place{};
    if (s.task == task && s.rev == rev && s.px == px && s.tile.valid() &&
        surface_.atlas().lookup(s.tile, place))
        return s.tile;
    // The pixels are premultiplied ARGB (0xAARRGGBB); the atlas wants premultiplied RGBA.
    std::uint8_t rgba[64 * 64 * 4];
    if (static_cast<unsigned>(px) > 64)
        return {};
    for (int i = 0; i < px * px; ++i) {
        const std::uint32_t v = pixels[i];
        rgba[i * 4 + 0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(v & 0xFF);
        rgba[i * 4 + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    }
    auto made = surface_.atlas().insert(lx::gfx::atlas_format::rgba8, static_cast<unsigned>(px),
                                        static_cast<unsigned>(px),
                                        std::span<const std::uint8_t>{rgba, sizeof(rgba)},
                                        static_cast<unsigned>(px * 4));
    if (!made) {
        log_panel.warn("icon tile insert failed: {}", made.error().message);
        return {};
    }
    s.task = task;
    s.rev = rev;
    s.px = px;
    s.tile = made.value();
    return s.tile;
}

void lx::shell::panel::request_repaint() {
    if (!surface_.valid())
        return;
    // The whole panel: the layout is a function of the shell's state, so a state change
    // re-describes. `set_describer` re-arms the describe (the ledger's paint request alone
    // repaints the old tree), and the repaint makes the frame worth drawing.
    surface_.root().set_describer(&panel_describe);
    surface_.root().add_repaint({0, 0, width(), k_panel_height});
}

namespace {

/// One cell of the panel: an interactive rounded box at `layout_panel`'s place, its
/// background following the pointer through the engine (hover, press), its `rest` colour
/// the state the layout gives it (focused, attention, armed …).
[[nodiscard]] lx::ui::child panel_button(const lx::ui::style& box, lx::ui::tok rest,
                                         std::span<const lx::ui::child> children,
                                         void* user, lx::ui::element_key key) {
    lx::ui::control_props c{};
    c.layout = box;
    c.paint.rest = {lx::ui::paint(rest), lx::ui::k_transparent};
    c.paint.hover = {rest == lx::ui::tok::none ? lx::ui::paint(lx::ui::tok::element_hover)
                                               : lx::ui::paint(rest),
                     lx::ui::k_transparent};
    c.paint.active = {lx::ui::paint(lx::ui::tok::element_active), lx::ui::k_transparent};
    for (lx::ui::radius& r : c.paint.corners)
        r = lx::ui::radius::sm;
    c.interactive = true;
    const panel_cell_props p{c, &lx::shell::panel::cell_notify, &lx::shell::panel::cell_event,
                             static_cast<lx::shell::panel*>(user)};
    return lx::ui::with_children(lx::ui::describe<panel_cell_node, panel_cell_props>(p, key),
                                 children);
}

/// A fixed box in a theme colour (a dot, a tile, a pill): not interactive, painted by a
/// control so its corners come from the paint.
[[nodiscard]] lx::ui::child panel_box(const lx::ui::style& box, lx::ui::tok bg, lx::ui::radius r,
                                      std::span<const lx::ui::child> children) {
    lx::ui::control_props c{};
    c.layout = box;
    c.paint = lx::ui::control_paint::fixed({lx::ui::paint(bg), lx::ui::k_transparent});
    for (lx::ui::radius& corner : c.paint.corners)
        corner = r;
    return lx::ui::control(c, children);
}

/// One line of text in the panel's font, ellipsized at its box.
[[nodiscard]] lx::ui::child panel_text(const char* text, lx::color color, bool bold,
                                       lx::ui::element_key key) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = bold ? k_panel_font_bold : k_panel_font;
    t.color = color;
    t.wrap = lx::text::text_wrap::none;
    t.ellipsize = true;
    t.context = lx::ui::active_ui_resources().text;
    return lx::ui::text_block(t, key);
}

} // namespace

lx::ui::child lx::shell::panel::build_ui() {
    // `after_dispatch` laid the panel out at the size it knew; the first frame (and a
    // configure's) runs after the surface's geometry settled: refresh when it differs.
    if (next_.width != width() || next_.height != k_panel_height)
        relayout();
    const panel_layout& l = next_;

    // What the indicator shows, logged when it changes (a workspace switch, a window
    // moving): `1* 2+ 3` — * active, + occupied, ! urgent.
    {
        char line[sizeof(ws_logged_)];
        unsigned n = 0;
        line[0] = '\0';
        for (unsigned i = 0; i < l.ws_count && i < ws_input_.count; ++i) {
            const unsigned f = l.ws[i].flags;
            const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s%s%s%s", i ? " " : "",
                                        ws_input_.items[i].name,
                                        (f & ws_cell_active) ? "*" : "",
                                        (f & ws_cell_occupied) ? "+" : "",
                                        (f & ws_cell_urgent) ? "!" : "");
            if (w <= 0 || n + static_cast<unsigned>(w) >= sizeof(line))
                break;
            n += static_cast<unsigned>(w);
        }
        if (std::strcmp(line, ws_logged_) != 0) {
            std::memcpy(ws_logged_, line, sizeof(line));
            log_panel.info("workspaces [{}] output={}", line[0] ? line : "-",
                           output_name_[0] ? output_name_ : "?");
        }
    }

    // The `layout idx=… x=… w=…` lines the smokes parse, on an arrangement change (a size,
    // an entry count, any position or width) — as the shm panel logged them.
    if (log_panel.enabled(lx::trace::level::debug)) {
        lx::rect2i diff[1];
        const unsigned nd = panel_diff(logged_, next_, diff, 1);
        if (nd == 1 && diff[0].width == next_.width && diff[0].height == next_.height) {
            log_layout(next_);
            copy_layout(logged_, next_);
        }
    }

    constexpr unsigned k_max_ui = k_max_panel_cells + k_max_ws_cells + k_max_tray_cells + 10;
    lx::ui::child kids[k_max_ui];
    unsigned n = 0;
    const lx::layout::length cell_h = lx::layout::px(static_cast<float>(l.cell_h));
    const lx::layout::length cell_y = lx::layout::px(static_cast<float>(l.cell_y));
    // Icons are resolved at the pixels they land on, drawn 1:1 on whole device pixels.
    const float scale = surface_.valid() ? surface_.scale() : 1.f;
    const int icon_px = static_cast<int>(std::lround(k_panel_icon * scale));

    // The Apps button (opens the launcher).
    if (l.apps_w > 0) {
        const lx::ui::child text[1] = {
            panel_text("Apps", (l.apps_flags & apps_open) ? k_text_focused : k_text, false,
                       0x7011u)};
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(l.apps_x)))
                                     .w(lx::layout::px(static_cast<float>(l.apps_w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .pl(lx::layout::px(8.f))
                                     .overflow_hidden(),
                                 (l.apps_flags & apps_open) ? lx::ui::tok::element_selected
                                                            : lx::ui::tok::none,
                                 text, this, 0x7001u);
    }
    // The workspace indicator: one cell per workspace, its name, a dot on the occupied ones.
    for (unsigned i = 0; i < l.ws_count; ++i) {
        const panel_ws_cell& c = l.ws[i];
        const char* name = i < ws_input_.count ? ws_input_.items[i].name : "";
        const lx::ui::tok rest = (c.flags & ws_cell_active)  ? lx::ui::tok::element_selected
                                 : (c.flags & ws_cell_urgent) ? lx::ui::tok::warning_background
                                                              : lx::ui::tok::none;
        lx::ui::child parts[2];
        unsigned k = 0;
        parts[k++] = panel_text(name, (c.flags & ws_cell_active) ? k_text_focused : k_text, false,
                                0x7100u + i);
        if (c.flags & ws_cell_occupied) {
            parts[k++] = panel_box(
                lx::ui::style{}.flex_none().w(lx::layout::px(4.f)).h(lx::layout::px(4.f)),
                (c.flags & ws_cell_active) ? lx::ui::tok::text : lx::ui::tok::text_muted,
                lx::ui::radius::full, {});
        }
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(c.x)))
                                     .w(lx::layout::px(static_cast<float>(c.w)))
                                     .h(cell_h)
                                     .flex_col()
                                     .items_center()
                                     .justify_center()
                                     .overflow_hidden(),
                                 rest, {parts, k}, this, 0x7100'0000u + i);
    }
    // The task entries, or the notice in their place.
    if (l.notice_w > 0) {
        const lx::ui::child text[1] = {panel_text(notice_, k_text_warning, false, 0x7200u)};
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(l.notice_x)))
                                     .w(lx::layout::px(static_cast<float>(l.notice_w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .pl(lx::layout::px(8.f))
                                     .overflow_hidden(),
                                 lx::ui::tok::none, text, this, 0x7201u);
    } else {
        for (unsigned i = 0; i < l.count; ++i) {
            const panel_cell& c = l.cells[i];
            const task_entry* e = bridge_ ? bridge_->model().get(c.task) : nullptr;
            const char* label = "";
            if (e)
                label = e->title[0] ? e->title : (e->app_id[0] ? e->app_id : "untitled");
            lx::ui::child parts[2];
            unsigned k = 0;
            const bool with_icon = e && c.w >= k_entry_min_width + k_panel_icon;
            if (with_icon) {
                const std::uint32_t* pixels =
                    icons_.resolve ? icons_.resolve(c.task, *e, icon_px, icons_.user) : nullptr;
                lx::gfx::atlas_tile_id tile =
                    icon_tile(c.task.index, c.task, c.icon_rev, pixels, icon_px);
                if (tile.valid()) {
                    parts[k++] = lx::ui::image(surface_.atlas(), tile, {icon_px, icon_px},
                                               0x7300'0000u + i);
                } else {
                    // No icon: a letter tile, the first letter of the app.
                    const char* src = e->app_id[0] ? e->app_id : label;
                    if (const char* dot = std::strrchr(src, '.'); dot && dot[1])
                        src = dot + 1;
                    char letter[2] = {src[0] >= 'a' && src[0] <= 'z'
                                          ? static_cast<char>(src[0] - 32)
                                          : src[0],
                                      0};
                    const lx::ui::child glyph = panel_text(letter[0] ? letter : "?",
                                                           k_text_focused, true,
                                                           0x7301'0000u + i);
                    parts[k++] = panel_box(
                        lx::ui::style{}.flex_none().w(lx::layout::px(16.f))
                                           .h(lx::layout::px(16.f))
                                           .flex_row()
                                           .items_center()
                                           .justify_center(),
                        lx::ui::tok::element_disabled, lx::ui::radius::sm, {&glyph, 1u});
                }
            }
            const lx::color label_color =
                (c.flags & (cell_not_responding | cell_minimized)) ? k_text_more
                : (c.flags & cell_focused)                         ? k_text_focused
                                                                   : k_text;
            parts[k++] = panel_text(label, label_color, false, 0x7302'0000u + i);
            lx::ui::style box = lx::ui::style{}
                                    .absolute()
                                    .top(cell_y)
                                    .left(lx::layout::px(static_cast<float>(c.x)))
                                    .w(lx::layout::px(static_cast<float>(c.w)))
                                    .h(cell_h)
                                    .flex_row()
                                    .items_center()
                                    .gap(lx::layout::px(6.f))
                                    .overflow_hidden();
            if (with_icon)
                box.pl(lx::layout::px(6.f));
            else
                box.pl(lx::layout::px(8.f));
            kids[n++] = panel_button(
                box, (c.flags & cell_focused)      ? lx::ui::tok::element_selected
                     : (c.flags & cell_attention)  ? lx::ui::tok::warning_background
                                                   : lx::ui::tok::none,
                {parts, k}, this, 0x7300'0000u + i);
        }
        if (l.hidden) {
            char more[16];
            std::snprintf(more, sizeof(more), "+%u", l.hidden);
            const lx::ui::child text = panel_text(more, k_text_more, false, 0x7400u);
            kids[n++] = lx::ui::styled(
                lx::ui::style{}
                    .absolute()
                    .top(cell_y)
                    .left(lx::layout::px(static_cast<float>(l.more_x)))
                    .w(lx::layout::px(static_cast<float>(k_panel_more_width)))
                    .h(cell_h)
                    .flex_row()
                    .items_center()
                    .pl(lx::layout::px(4.f))
                    .overflow_hidden(),
                text);
        }
    }
    // The tray: one cell per StatusNotifierItem, its icon or a letter tile.
    for (unsigned i = 0; i < l.tray_count; ++i) {
        const panel_tray_cell& c = l.tray[i];
        lx::ui::child parts[1];
        unsigned k = 0;
        const std::uint32_t* pixels =
            widgets_.tray_icon ? widgets_.tray_icon(c.key, icon_px, widgets_.user) : nullptr;
        lx::gfx::atlas_tile_id tile = icon_tile(0x4000u + c.key, task_handle{c.key, c.rev},
                                                c.rev, pixels, icon_px);
        if (tile.valid()) {
            parts[k++] = lx::ui::image(surface_.atlas(), tile, {k_tray_icon, k_tray_icon},
                                       0x7500'0000u + i);
        } else {
            char letter[2] = {widgets_.tray_letter ? widgets_.tray_letter(c.key, widgets_.user)
                                                   : '?',
                              0};
            if (letter[0] >= 'a' && letter[0] <= 'z')
                letter[0] = static_cast<char>(letter[0] - 32);
            const lx::ui::child glyph = panel_text(letter[0] ? letter : "?", k_text_focused,
                                                   true, 0x7501'0000u + i);
            parts[k++] = panel_box(
                lx::ui::style{}.flex_none().w(lx::layout::px(16.f))
                                   .h(lx::layout::px(16.f))
                                   .flex_row()
                                   .items_center()
                                   .justify_center(),
                lx::ui::tok::element_disabled, lx::ui::radius::sm, {&glyph, 1u});
        }
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(c.x)))
                                     .w(lx::layout::px(static_cast<float>(c.w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .justify_center(),
                                 lx::ui::tok::none, {parts, k}, this, 0x7500'0000u + i);
    }
    // The clock is a real press target, with the same hover/pressed treatment as other panel controls.
    if (l.clock_w > 0) {
        lx::ui::child text[1] = {panel_text(clock_text_,
            l.clock_flags & bell_flag_pressed ? k_text_focused :
            l.clock_flags & bell_flag_hover ? k_text_focused : k_text, false, 0x7700u)};
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(l.clock_x)))
                                     .w(lx::layout::px(static_cast<float>(l.clock_w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .justify_center(),
                                 lx::ui::tok::none, {text, 1u}, this, 0x7701u);
    }
    // The notification indicator: a bell, the unread count beside it.
    if (l.bell_w > 0) {
        const bool dnd = (l.bell_flags & bell_flag_dnd) != 0;
        lx::ui::child parts[2];
        unsigned k = 0;
        const lx::ui::child bell[1] = {
            lx::ui::icon_view(dnd ? lx::ui::icon::bell_off : lx::ui::icon::bell, 16.f,
                              dnd ? lx::ui::tok::icon_muted : lx::ui::tok::icon)};
        parts[k++] = lx::ui::styled(
            lx::ui::style{}.flex_none().flex_row().items_center(), bell[0]);
        if (l.bell_unread > 0) {
            char count[8];
            std::snprintf(count, sizeof(count), "%u", l.bell_unread > 999 ? 999u : l.bell_unread);
            const lx::ui::child label = panel_text(count, k_text_focused, true, 0x7600u);
            parts[k++] = panel_box(
                lx::ui::style{}.flex_none().h(lx::layout::px(16.f))
                                   .flex_row()
                                   .items_center()
                                   .px(lx::layout::px(6.f))
                                   .overflow_hidden(),
                lx::ui::tok::element_selected, lx::ui::radius::full, {&label, 1u});
        }
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(l.bell_x)))
                                     .w(lx::layout::px(static_cast<float>(l.bell_w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .pl(lx::layout::px(6.f))
                                     .gap(lx::layout::px(4.f))
                                     .overflow_hidden(),
                                 lx::ui::tok::none, {parts, k}, this, 0x7601u);
    }
    // The privacy indicator: a red dot while some client captures the screen.
    if (l.capture_w > 0) {
        const lx::ui::child dot = panel_box(
            lx::ui::style{}.flex_none().w(lx::layout::px(10.f)).h(lx::layout::px(10.f)),
            lx::ui::tok::error, lx::ui::radius::full, {});
        kids[n++] = lx::ui::styled(
            lx::ui::style{}
                .absolute()
                .top(cell_y)
                .left(lx::layout::px(static_cast<float>(l.capture_x)))
                .w(lx::layout::px(static_cast<float>(l.capture_w)))
                .h(cell_h)
                .flex_row()
                .items_center()
                .justify_center(),
            dot);
    }
    // The logout icon opens the session menu.
    if (l.logout_w > 0) {
        const lx::ui::child icon[1] = {
            lx::ui::icon_view(lx::ui::icon::log_out, 16.f, lx::ui::tok::icon)};
        kids[n++] = panel_button(lx::ui::style{}
                                     .absolute()
                                     .top(cell_y)
                                     .left(lx::layout::px(static_cast<float>(l.logout_x)))
                                     .w(lx::layout::px(static_cast<float>(l.logout_w)))
                                     .h(cell_h)
                                     .flex_row()
                                     .items_center()
                                     .justify_center(),
                                 lx::ui::tok::none, icon, this, 0x7701u);
    }

    // The root: the panel's background over everything, the cells placed on it.
    panel_root_props root{};
    root.flex.style.width = lx::layout::percent(100.f);
    root.flex.style.height = lx::layout::percent(100.f);
    root.flex.fill = panel_color(bg_);
    root.mouse = &root_mouse;
    root.user = this;
    return lx::ui::with_children(lx::ui::describe<panel_root_node, panel_root_props>(root, 0x7000u),
                                 {kids, n});
}

lx::ui::child lx::shell::panel_describe(lx::ui::build_context&) {
    panel* p = g_describing;
    return p ? p->build_ui() : nullptr;
}

// ── Shell plumbing ───────────────────────────────────────────────────────────

void lx::shell::panel::log_layout(const panel_layout& l) const {
    if (!log_panel.enabled(lx::trace::level::debug) || !bridge_)
        return;
    for (unsigned i = 0; i < l.count; ++i) {
        const task_entry* e = bridge_->model().get(l.cells[i].task);
        log_panel.debug("layout idx={} x={} w={} handle={} title=\"{}\"", i, l.cells[i].x,
                        l.cells[i].w, l.cells[i].task.index, e ? e->title : "");
    }
    if (l.hidden) {
        log_panel.debug("layout more={} x={}", l.hidden, l.more_x);
    }
    if (l.apps_w > 0) {
        log_panel.debug("layout apps x={} w={}", l.apps_x, l.apps_w);
    }
    for (unsigned i = 0; i < l.tray_count; ++i)
        log_panel.debug("layout tray key={} x={} w={} output={}", l.tray[i].key, l.tray[i].x,
                        l.tray[i].w, output_name_);
    if (l.bell_w > 0)
        log_panel.debug("layout bell x={} w={} output={}", l.bell_x, l.bell_w, output_name_);
}

void lx::shell::panel::set_apps(bool shown, bool open) {
    if (shown == apps_shown_ && open == apps_open_)
        return;
    apps_shown_ = shown;
    apps_open_ = open;
    dirty_ = true;
}

void lx::shell::panel::set_notice(const char* text) {
    const char* t = text ? text : "";
    if (std::strcmp(t, notice_) == 0)
        return;
    std::snprintf(notice_, sizeof(notice_), "%s", t);
    ++notice_rev_;
    dirty_ = true;
}

void lx::shell::panel::set_menu_keyboard(bool on) {
    if (on == menu_keyboard_ || !role_.valid())
        return;
    menu_keyboard_ = on;
    // on_demand is layer-shell v4's; below it the menu takes the pointer only.
    if (!role_.set_keyboard_interactivity(on ? lx::wayland::keyboard_interactivity::on_demand
                                             : lx::wayland::keyboard_interactivity::none))
        return;
    surface_.commit_state();
}

void lx::shell::panel::set_blur(void* manager) {
    if (manager == g_.background_effect)
        return;
    g_.background_effect = manager;
    // The buffers were picked at create; a change takes effect when the panel is created
    // again. Logged once so a config reload is not silent about it.
    if (surface_.valid())
        log_panel.warn("panel blur changed after create: it applies to the next panel");
}


lx::result<void> lx::shell::panel::create(panel_globals globals, policy_bridge& bridge,
                                          unsigned output_slot) {
    if (!globals.ui)
        return lx::invalid_argument("panel needs the shell's lx.ui.host context");
    g_ = globals;
    bridge_ = &bridge;
    if (!g_.ui->started())
        return lx::invalid_argument("panel: the shell's lx.ui.host context is not started");
    lx::ui::host::surface_options so{};
    so.describe = &panel_describe;
    so.size = {0, k_panel_height};
    // Translucent over a blur of what is behind it, else opaque: the buffers are picked
    // here and stay for the surface's life.
    const bool blur = g_.background_effect != nullptr;
    so.opaque = !blur;
    if (blur)
        bg_ = k_panel_translucent_bg;
    so.frame_records = false;
    if (auto made = surface_.create(*g_.ui, so); !made) {
        log_panel.error(made.get_error());
        return made.get_error();
    }
    lx::ui::host::layer_role_options lo{};
    lo.layer.output_slot = output_slot;
    lo.layer.level = lx::wayland::layer::top;
    lo.layer.name_space = "lumen-panel";
    lo.size = {0, k_panel_height};
    lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_left |
                lx::wayland::anchor_right;
    lo.exclusive_zone = k_panel_height;
    if (auto made = role_.create(surface_, lo); !made) {
        log_panel.error(made.get_error());
        surface_.destroy();
        return made.get_error();
    }
    role_.set_closed_handler(
        [](void* user) {
            auto& p = *static_cast<panel*>(user);
            p.closed_ = true;
            log_panel.warn("layer surface closed by the compositor");
        },
        this);
#ifdef LUMEN_SHELL_BACKGROUND_EFFECT
    if (blur) {
        // Translucent over a blur of everything behind it: no opaque region, the whole
        // surface the blur region (the compositor clips it).
        auto* effect = ext_background_effect_manager_v1_get_background_effect(
            static_cast<ext_background_effect_manager_v1*>(g_.background_effect),
            static_cast<wl_surface*>(surface_.wl().native()));
        if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(
                g_.ui->display().native_global(lx::wayland::global_kind::compositor)))) {
            wl_region_add(region, 0, 0, 1 << 30, 1 << 30);
            ext_background_effect_surface_v1_set_blur_region(effect, region);
            wl_region_destroy(region);
        }
        effect_ = effect;
    }
#endif
    if (auto shown = surface_.show(); !shown) {
        log_panel.error(shown.get_error());
        role_.destroy();
        surface_.destroy();
        return shown.get_error();
    }
    dirty_ = true;
    return {};
}

void lx::shell::panel::after_dispatch() {
    if (!bridge_ || !surface_.valid() || closed_)
        return;
    task_model& model = bridge_->model();
    // A snapshot in progress is never shown half-done; its changes wait for snapshot_done.
    if (model.in_snapshot())
        return;
    after_dispatch(model.take_changes());
}

void lx::shell::panel::after_dispatch(unsigned changes) {
    if (!bridge_ || !surface_.valid() || closed_)
        return;
    task_model& model = bridge_->model();
    if (model.in_snapshot())
        return;
    // With a filter, a window moving between outputs changes which panel lists it.
    const unsigned relevant = task_change_list | task_change_label | task_change_focus |
                              task_change_flags | task_change_icon | task_change_parent |
                              task_change_state | task_change_workspace |
                              (filter_.keep ? unsigned{task_change_geometry} : 0u);
    if (changes & relevant) {
        dirty_ = true;
        // The entry under a still pointer may be a different one now.
        if (changes & task_change_list)
            pointer_motion(pointer_x_, pointer_y_);
        // A press on an entry that went away is no press.
        if (pressed_ && !model.get(pressed_))
            pressed_ = {};
    }
    // The privacy indicator follows the compositor's capture state.
    if (bridge_->capture_active() != (committed_.capture_w > 0))
        dirty_ = true;
    relayout();
    // An arrangement change (a size, an entry count, any position or width) repaints the
    // whole strip, as the shm panel damaged it — the freed cell, the moved ones and the
    // dirty cells in one pass.
    lx::rect2i arranged[1];
    const unsigned na = panel_diff(frame_, next_, arranged, 1);
    // Sticks until the frame that answers it is logged: a change seen while a frame
    // callback is outstanding belongs to that frame.
    arranged_ = arranged_ || (next_.width > 0 &&
                              (frame_.width == 0 ||
                               (na == 1 && arranged[0].width == next_.width &&
                                arranged[0].height == next_.height)));
    copy_layout(frame_, next_);
    if (dirty_ || arranged_)
        request_repaint();
    dirty_ = false;
    // The surface's frame loop: a frame when its frame callback allows one and the ledger
    // has work (our repaint, the engine's hover damage, a configure).
    if (surface_.frame_due()) {
        describing_guard guard{this};
        surface_.run_frame();
        // The `configure w=… h=…` line the session scripts wait for before starting the
        // first client (run-desktop.sh: the panel's strip reserved): logged when a
        // configure's size settled — the first, and any later one that changes it.
        if (surface_.configured()) {
            const auto settled = surface_.logical_size();
            if (settled.width != configure_logged_w_ || settled.height != configure_logged_h_) {
                configure_logged_w_ = settled.width;
                configure_logged_h_ = settled.height;
                log_panel.info("configure w={} h={}", settled.width, settled.height);
            }
        }
        if (log_panel.enabled(lx::trace::level::debug) && surface_.stats().frames > 0) {
            // The `draw entries=…` line the smokes parse: what the last frame showed and
            // what it damaged (the engine's damage rects; `(full)` the whole panel).
            int focused = -1;
            for (unsigned i = 0; i < next_.count; ++i)
                if (next_.cells[i].flags & cell_focused)
                    focused = static_cast<int>(i);
            log_panel.debug("draw entries={} hidden={} focused={} damage={}{} repaint={}",
                            next_.count, next_.hidden, focused,
                            surface_.stats().last_damage_rects,
                            (arranged_ || surface_.last_damage().full) ? "(full)" : "",
                            surface_.stats().last_repaint_rects);
        }
        arranged_ = false;
        if (surface_.stats().frames == 1)
            log_panel.info("panel drawn {}×{} output={}", width(), k_panel_height,
                           output_name_[0] ? output_name_ : "?");
    }
}

void lx::shell::panel::destroy() {
#ifdef LUMEN_SHELL_BACKGROUND_EFFECT
    if (effect_)
        ext_background_effect_surface_v1_destroy(
            static_cast<ext_background_effect_surface_v1*>(effect_));
#endif
    effect_ = nullptr;
    role_.destroy();
    surface_.destroy();
    bridge_ = nullptr;
}


lx::shell::panel::~panel() { destroy(); }
