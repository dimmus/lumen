module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
// wayland-scanner names an argument `namespace`, a C++ keyword (Wayfire renames it the same
// way).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

import lx.foundation;
import lx.text;
import lx.trace;

export module lx.shell:panel;

import std;

import :task_model;
import :bridge;
import :shm_canvas;

export namespace lx::shell {

// ── Panel layout (pure) ───────────────────────────────────────────────────────
//
// One row: the Apps button at the left end, the workspace indicator (one cell per workspace
// of the panel's output, while there is more than one), then the task entries left to right
// in task order (or, while a notice shows, the notice in their place), then the logout
// button.
// Kept separate from drawing so both repaint and damage are a comparison of two layouts:
// what a buffer holds (or what was last committed) against what should be shown.

inline constexpr int k_panel_height = 32;
inline constexpr int k_panel_pad = 4;
inline constexpr int k_panel_gap = 4;
inline constexpr int k_entry_height = 24;
inline constexpr int k_entry_min_width = 48;
inline constexpr int k_entry_max_width = 220;
/// Room for the "+k" label of entries that do not fit.
inline constexpr int k_panel_more_width = 44;
/// The logout button at the right end.
inline constexpr int k_logout_width = 84;
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

/// The logout button's input to a layout. Two clicks log out: the first arms it ("Confirm?"),
/// the second sends the request; moving off the button disarms it. No timer, so nothing
/// runs while the panel is idle, and a stray click cannot end the session.
struct panel_logout {
    bool shown = false; // the compositor supports zlm_shell_v1.request_logout
    bool armed = false;
    bool pressed = false;
};

enum panel_logout_flag : unsigned {
    logout_hover = 1u << 0,
    logout_pressed = 1u << 1,
    logout_armed = 1u << 2,
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
    /// 0 = nothing (an unpainted buffer, nothing committed yet).
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
void layout_panel(const task_model& model, int width, int height, double pointer_x,
                  double pointer_y, task_handle pressed, panel_layout& out,
                  panel_logout logout = {}, panel_apps apps = {}, panel_notice notice = {},
                  panel_filter filter = {}, bool capturing = false,
                  const panel_ws_input* ws = nullptr, const panel_tray_input* tray = nullptr,
                  unsigned tray_pressed = 0, panel_bell bell = {}, bool bell_pressed = false);
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

/// Globals the panel draws through (all `void*`: no Wayland type is exported).
struct panel_globals {
    void* compositor = nullptr;  // wl_compositor (v4+: damage_buffer)
    void* shm = nullptr;         // wl_shm
    void* layer_shell = nullptr; // zwlr_layer_shell_v1
    /// Optional: with both, the panel renders at the output's fractional scale.
    void* viewporter = nullptr;  // wp_viewporter
    void* fractional = nullptr;  // wp_fractional_scale_manager_v1
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
    void* user = nullptr;
};

/// Input on the tray or the notification indicator, for the shell to act on.
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
/// 32 px, exclusive zone 32, no keyboard) with the Apps button, one entry per window (the
/// focused one highlighted) and the logout button. A left click on an entry (press and
/// release on the same entry) asks the compositor to raise and activate that window.
/// Pointer events come from the shell's seat, which routes them by surface.
///
/// Drawing is event driven: nothing runs while nothing changes. Model changes, pointer
/// motion and configures only mark the panel dirty; `after_dispatch()` draws at most once
/// per frame callback, repainting in its buffer only the cells that differ from what that
/// buffer holds and damaging only the cells that differ from the last commit.
class panel {
public:
    panel() = default;
    ~panel();
    panel(const panel&) = delete;
    panel& operator=(const panel&) = delete;

    /// Creates the layer surface on `output` (wl_output*; null: the compositor picks) and
    /// sends its initial commit.
    [[nodiscard]] lx::result<void> create(panel_globals globals, policy_bridge& bridge,
                                          void* output = nullptr);
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
    [[nodiscard]] void* layer() const { return layer_; }
    /// Keyboard interactivity on_demand while one of its menus is open (the menu's grab
    /// then takes the keyboard), none otherwise. Committed at once.
    void set_menu_keyboard(bool on);
    /// The compositor closed the layer surface (its output went away) or a buffer could
    /// not be made: the shell should exit and be restarted.
    [[nodiscard]] bool closed() const { return closed_; }
    /// The panel has been configured and drawn once.
    [[nodiscard]] bool first_frame_committed() const { return committed_.width > 0; }
    [[nodiscard]] const panel_layout& committed() const { return committed_; }
    /// wl_surface* (for routing pointer events and for activation tokens).
    [[nodiscard]] void* surface() const { return surface_; }
    /// The configured width (the output's, logical).
    [[nodiscard]] int width() const { return width_; }
    /// The output's preferred scale in 120ths, as the panel's surface was told (120 without
    /// fractional scale).
    [[nodiscard]] unsigned scale120() const { return canvas_.scale120(); }

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

    // ── Pointer, routed here by the shell ──
    void pointer_enter(double x, double y);
    void pointer_leave();
    void pointer_motion(double x, double y);
    void pointer_button(unsigned button, bool pressed, unsigned serial = 0);
    /// Wheel steps (discrete; positive: down) — over the indicator, the next workspace; over
    /// a tray item, its Scroll.
    void pointer_scroll(int steps);

    struct events;

private:
    friend struct events;

    void draw(bool force);
    void paint(int bi, const panel_layout& l, const lx::rect2i* rects, unsigned n);
    void paint_cell(int bi, const panel_layout& l, unsigned i);
    void paint_more(int bi, const panel_layout& l);
    void paint_logout(int bi, const panel_layout& l);
    void paint_apps(int bi, const panel_layout& l);
    void paint_notice(int bi, const panel_layout& l);
    void paint_capture(int bi, const panel_layout& l);
    void paint_ws(int bi, const panel_layout& l, unsigned i);
    void paint_tray(int bi, const panel_layout& l, unsigned i);
    void paint_bell(int bi, const panel_layout& l);
    void log_layout(const panel_layout& l) const;

    panel_globals g_{};
    policy_bridge* bridge_ = nullptr;
    void* surface_ = nullptr; // wl_surface*
    void* layer_ = nullptr;   // zwlr_layer_surface_v1*
    shm_canvas canvas_{};
    panel_layout drawn_[2]{}; // what each buffer's pixels show
    int width_ = 0;
    int height_ = 0;
    bool configured_ = false;
    bool resized_ = false;
    /// A configure was acked: the next draw commits even if nothing changed.
    bool must_commit_ = false;
    bool dirty_ = false;
    bool closed_ = false;
    double pointer_x_ = -1.0;
    double pointer_y_ = -1.0;
    task_handle hovered_{};
    task_handle pressed_{};
    bool logout_hovered_ = false;
    bool logout_pressed_ = false;
    bool logout_armed_ = false;
    bool apps_shown_ = false;
    bool apps_open_ = false;
    bool apps_hovered_ = false;
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
    bool menu_keyboard_ = false;
    /// The indicator's input of the layout being drawn (its names).
    panel_ws_input ws_input_{};
    int ws_pressed_ = -1;
    /// The indicator as last logged.
    char ws_logged_[160]{};
    char output_name_[32]{};
    panel_layout committed_{};
    panel_layout next_{};
};

} // namespace lx::shell

// ── Layout ───────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool in_box(double px, double py, int x, int w, int y, int h) {
    return w > 0 && px >= x && px < x + w && py >= y && py < y + h;
}

} // namespace

void lx::shell::layout_panel(const task_model& model, int width, int height, double px,
                             double py, task_handle pressed, panel_layout& out,
                             panel_logout logout, panel_apps apps, panel_notice notice,
                             panel_filter filter, bool capturing, const panel_ws_input* ws,
                             const panel_tray_input* tray, unsigned tray_pressed, panel_bell bell,
                             bool bell_pressed) {
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
    // The logout button takes the right end, the Apps button the left; entries share
    // what is left.
    int reserve = 0;
    if (logout.shown && width >= 2 * k_panel_pad + k_logout_width + k_entry_min_width) {
        out.logout_w = k_logout_width;
        out.logout_x = width - k_panel_pad - k_logout_width;
        reserve = k_logout_width + k_panel_gap;
        if (logout.armed)
            out.logout_flags |= logout_armed;
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
    return n;
}

// ── Drawing ──────────────────────────────────────────────────────────────────

namespace {

constexpr lx::trace::category log_panel{"shell.panel"};
constexpr std::uint32_t k_bg = lx::shell::k_canvas_bg;
constexpr std::uint32_t k_hover = 0xFF34343Au;
constexpr std::uint32_t k_pressed = 0xFF2A2A30u;
constexpr std::uint32_t k_focused = 0xFF3D6FB4u;
/// Demands attention: an amber accent, as GNOME's and KDE's task bars use.
constexpr std::uint32_t k_attention = 0xFFB4782Du;
/// The armed logout button: a second click ends the session.
constexpr std::uint32_t k_danger = 0xFFB43C3Cu;
/// The capture indicator's dot (GNOME's and KDE's privacy indicators are red too).
constexpr std::uint32_t k_capture_red = 0xFFE0312Bu;
constexpr lx::text::raster_color k_text{0xE6, 0xE6, 0xE6, 0xFF};
constexpr lx::text::raster_color k_text_focused{0xFF, 0xFF, 0xFF, 0xFF};
constexpr lx::text::raster_color k_text_more{0xA8, 0xA8, 0xAC, 0xFF};
constexpr lx::text::raster_color k_text_warning{0xF0, 0xB4, 0x5A, 0xFF};
constexpr lx::text::font_desc k_panel_font{"Sans", 10.f, lx::text::weight::normal,
                                           lx::text::style::normal};
constexpr int k_text_pad = 8;

[[nodiscard]] bool intersects(lx::rect2i a, int x, int w) {
    return w > 0 && x < a.x + a.width && x + w > a.x;
}

} // namespace

void lx::shell::panel::paint_cell(int bi, const panel_layout& l, unsigned i) {
    const panel_cell& c = l.cells[i];
    const lx::rect2i box{c.x, l.cell_y, c.w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (c.flags & cell_pressed)
        bgc = k_pressed;
    else if (c.flags & cell_focused)
        bgc = k_focused;
    else if (c.flags & cell_attention)
        bgc = k_attention;
    else if (c.flags & cell_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    const task_entry* e = bridge_ ? bridge_->model().get(c.task) : nullptr;
    const char* label = "";
    if (e)
        label = e->title[0] ? e->title : (e->app_id[0] ? e->app_id : "untitled");
    // The icon, 16 logical px, resolved at the pixels it lands on; else a letter tile.
    int text_x = c.x + k_text_pad;
    if (e && c.w >= k_entry_min_width + k_panel_icon) {
        const lx::rect2i icon_box{c.x + k_text_pad - 2, l.cell_y + (l.cell_h - k_panel_icon) / 2,
                                  k_panel_icon, k_panel_icon};
        const int px = canvas_.pixel_rect(icon_box).width;
        const std::uint32_t* pixels =
            icons_.resolve ? icons_.resolve(c.task, *e, px, icons_.user) : nullptr;
        if (pixels) {
            canvas_.blit(bi, icon_box, pixels, px);
        } else {
            canvas_.fill_rounded(bi, icon_box, 0xFF5A5A66u, bgc, 3);
            char letter[8] = {};
            const char* src = e->app_id[0] ? e->app_id : label;
            // The last dot-separated part of a reverse-DNS app id names the app.
            if (const char* dot = std::strrchr(src, '.'); dot && dot[1])
                src = dot + 1;
            letter[0] = src[0] >= 'a' && src[0] <= 'z' ? static_cast<char>(src[0] - 32) : src[0];
            (void)canvas_.text(bi, {icon_box.x + 4, icon_box.y, icon_box.width - 4, icon_box.height},
                               letter[0] ? letter : "?", k_text_focused,
                               {"Sans", 8.f, lx::text::weight::bold, lx::text::style::normal});
        }
        text_x = icon_box.x + k_panel_icon + 6;
    }
    (void)canvas_.text(bi, {text_x, l.cell_y, c.x + c.w - k_text_pad - text_x, l.cell_h}, label,
                       (c.flags & (cell_not_responding | cell_minimized)) ? k_text_more
                       : (c.flags & cell_focused)                          ? k_text_focused
                                                                           : k_text,
                       k_panel_font);
}

void lx::shell::panel::paint_ws(int bi, const panel_layout& l, unsigned i) {
    if (i >= l.ws_count)
        return;
    const panel_ws_cell& c = l.ws[i];
    const lx::rect2i box{c.x, l.cell_y, c.w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (c.flags & ws_cell_pressed)
        bgc = k_pressed;
    else if (c.flags & ws_cell_active)
        bgc = k_focused;
    else if (c.flags & ws_cell_urgent)
        bgc = k_attention;
    else if (c.flags & ws_cell_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    const char* name = i < ws_input_.count ? ws_input_.items[i].name : "";
    const int tw = canvas_.text_width(name, k_panel_font);
    const int tx = tw > 0 && tw < c.w ? c.x + (c.w - tw) / 2 : c.x + 4;
    (void)canvas_.text(bi, {tx, l.cell_y - 2, c.x + c.w - tx, l.cell_h}, name,
                       (c.flags & ws_cell_active) ? k_text_focused : k_text, k_panel_font);
    // Occupied: a dot under the name (the active one shows its windows anyway).
    if (c.flags & ws_cell_occupied) {
        const lx::rect2i dot{c.x + c.w / 2 - 2, l.cell_y + l.cell_h - 6, 4, 4};
        canvas_.fill_rounded(bi, dot, (c.flags & ws_cell_active) ? 0xFFFFFFFFu : 0xFFA8A8ACu,
                             bgc, 2);
    }
}

void lx::shell::panel::paint_more(int bi, const panel_layout& l) {
    if (!l.hidden)
        return;
    char more[16];
    std::snprintf(more, sizeof(more), "+%u", l.hidden);
    const int right = l.logout_w > 0 ? l.logout_x - k_panel_gap : width_ - k_panel_pad;
    int w = right - l.more_x;
    w = w < k_panel_more_width ? w : k_panel_more_width;
    (void)canvas_.text(bi, {l.more_x + 4, l.cell_y, w - 4, l.cell_h}, more, k_text_more,
                       k_panel_font);
}

void lx::shell::panel::paint_logout(int bi, const panel_layout& l) {
    if (l.logout_w <= 0)
        return;
    const lx::rect2i box{l.logout_x, l.cell_y, l.logout_w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (l.logout_flags & logout_pressed)
        bgc = k_pressed;
    else if (l.logout_flags & logout_armed)
        bgc = k_danger;
    else if (l.logout_flags & logout_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    const bool armed = (l.logout_flags & logout_armed) != 0;
    (void)canvas_.text(bi, {l.logout_x + k_text_pad, l.cell_y, l.logout_w - 2 * k_text_pad, l.cell_h},
                       armed ? "Confirm?" : "Log out", armed ? k_text_focused : k_text_more,
                       k_panel_font);
}

void lx::shell::panel::paint_apps(int bi, const panel_layout& l) {
    if (l.apps_w <= 0)
        return;
    const lx::rect2i box{l.apps_x, l.cell_y, l.apps_w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (l.apps_flags & apps_pressed)
        bgc = k_pressed;
    else if (l.apps_flags & apps_open)
        bgc = k_focused;
    else if (l.apps_flags & apps_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    (void)canvas_.text(bi, {l.apps_x + k_text_pad, l.cell_y, l.apps_w - 2 * k_text_pad, l.cell_h},
                       "Apps", (l.apps_flags & apps_open) ? k_text_focused : k_text, k_panel_font);
}

void lx::shell::panel::paint_notice(int bi, const panel_layout& l) {
    if (l.notice_w <= 0)
        return;
    const lx::rect2i box{l.notice_x, l.cell_y, l.notice_w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    (void)canvas_.text(bi, {l.notice_x + k_text_pad, l.cell_y, l.notice_w - 2 * k_text_pad, l.cell_h},
                       notice_, k_text_warning, k_panel_font);
}

void lx::shell::panel::paint_capture(int bi, const panel_layout& l) {
    if (l.capture_w <= 0)
        return;
    const lx::rect2i box{l.capture_x, l.cell_y, l.capture_w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    const lx::rect2i dot{l.capture_x + (l.capture_w - k_capture_dot) / 2,
                         l.cell_y + (l.cell_h - k_capture_dot) / 2, k_capture_dot, k_capture_dot};
    canvas_.fill_rounded(bi, dot, k_capture_red, k_bg, k_capture_dot / 2);
}

void lx::shell::panel::paint_tray(int bi, const panel_layout& l, unsigned i) {
    if (i >= l.tray_count)
        return;
    const panel_tray_cell& c = l.tray[i];
    const lx::rect2i box{c.x, l.cell_y, c.w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (c.flags & cell_pressed)
        bgc = k_pressed;
    else if (c.flags & cell_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    const lx::rect2i icon_box{c.x + (c.w - k_tray_icon) / 2, l.cell_y + (l.cell_h - k_tray_icon) / 2,
                              k_tray_icon, k_tray_icon};
    const int px = canvas_.pixel_rect(icon_box).width;
    const std::uint32_t* pixels =
        widgets_.tray_icon ? widgets_.tray_icon(c.key, px, widgets_.user) : nullptr;
    if (pixels) {
        canvas_.blit(bi, icon_box, pixels, px);
        return;
    }
    // No icon: a letter tile, as for windows.
    canvas_.fill_rounded(bi, icon_box, 0xFF5A5A66u, bgc, 3);
    char letter[2] = {widgets_.tray_letter ? widgets_.tray_letter(c.key, widgets_.user) : '?', 0};
    if (letter[0] >= 'a' && letter[0] <= 'z')
        letter[0] = static_cast<char>(letter[0] - 32);
    (void)canvas_.text(bi, {icon_box.x + 4, icon_box.y, icon_box.width - 4, icon_box.height},
                       letter[0] ? letter : "?", k_text_focused,
                       {"Sans", 8.f, lx::text::weight::bold, lx::text::style::normal});
}

void lx::shell::panel::paint_bell(int bi, const panel_layout& l) {
    if (l.bell_w <= 0)
        return;
    const lx::rect2i box{l.bell_x, l.cell_y, l.bell_w, l.cell_h};
    canvas_.fill(bi, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (l.bell_flags & bell_flag_pressed)
        bgc = k_pressed;
    else if (l.bell_flags & bell_flag_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        canvas_.fill_rounded(bi, box, bgc, k_bg);
    const bool dnd = (l.bell_flags & bell_flag_dnd) != 0;
    const std::uint32_t fg = dnd ? 0xFF8C8C94u : 0xFFE6E6E6u;
    // A bell in a 16 px box: the dome, its rim, the clapper.
    const int bx = l.bell_x + 6, by = l.cell_y + (l.cell_h - 16) / 2;
    canvas_.fill_rounded(bi, {bx + 3, by + 1, 10, 10}, fg, bgc, 5);
    canvas_.fill(bi, {bx + 3, by + 6, 10, 5}, fg);
    canvas_.fill_rounded(bi, {bx + 1, by + 10, 14, 3}, fg, bgc, 1);
    canvas_.fill_rounded(bi, {bx + 6, by + 13, 4, 3}, fg, bgc, 1);
    if (dnd) {
        // Do not disturb: crossed out.
        for (int i = 0; i < 15; ++i)
            canvas_.fill(bi, {bx + i, by + i, 2, 2}, 0xFFE0312Bu);
    }
    if (l.bell_unread > 0) {
        char count[8];
        std::snprintf(count, sizeof(count), "%u", l.bell_unread > 999 ? 999u : l.bell_unread);
        const lx::rect2i pill{bx + 20, l.cell_y + (l.cell_h - 16) / 2, l.bell_w - 26, 16};
        canvas_.fill_rounded(bi, pill, k_focused, bgc, 8);
        const int tw = canvas_.text_width(count, {"Sans", 8.f, lx::text::weight::bold,
                                                  lx::text::style::normal});
        const int tx = tw > 0 && tw < pill.width ? pill.x + (pill.width - tw) / 2 : pill.x + 2;
        (void)canvas_.text(bi, {tx, pill.y, pill.x + pill.width - tx, pill.height}, count,
                           k_text_focused,
                           {"Sans", 8.f, lx::text::weight::bold, lx::text::style::normal});
    }
}

void lx::shell::panel::paint(int bi, const panel_layout& l, const lx::rect2i* rects, unsigned n) {
    for (unsigned r = 0; r < n; ++r) {
        const lx::rect2i& rect = rects[r];
        if (rect.width == l.width && rect.height == l.height) {
            // Everything: background, every cell, the "+k" label.
            canvas_.fill(bi, rect, k_bg);
            paint_apps(bi, l);
            for (unsigned i = 0; i < l.ws_count; ++i)
                paint_ws(bi, l, i);
            for (unsigned i = 0; i < l.count; ++i)
                paint_cell(bi, l, i);
            paint_more(bi, l);
            paint_notice(bi, l);
            for (unsigned i = 0; i < l.tray_count; ++i)
                paint_tray(bi, l, i);
            paint_bell(bi, l);
            paint_capture(bi, l);
            paint_logout(bi, l);
            return;
        }
        for (unsigned i = 0; i < l.count; ++i)
            if (intersects(rect, l.cells[i].x, l.cells[i].w))
                paint_cell(bi, l, i);
        if (intersects(rect, l.logout_x, l.logout_w))
            paint_logout(bi, l);
        if (intersects(rect, l.apps_x, l.apps_w))
            paint_apps(bi, l);
        if (intersects(rect, l.notice_x, l.notice_w))
            paint_notice(bi, l);
        for (unsigned i = 0; i < l.ws_count; ++i)
            if (intersects(rect, l.ws[i].x, l.ws[i].w))
                paint_ws(bi, l, i);
        for (unsigned i = 0; i < l.tray_count; ++i)
            if (intersects(rect, l.tray[i].x, l.tray[i].w))
                paint_tray(bi, l, i);
        if (intersects(rect, l.bell_x, l.bell_w))
            paint_bell(bi, l);
    }
}

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

// ── Pointer ──

void lx::shell::panel::pointer_enter(double x, double y) { pointer_motion(x, y); }

void lx::shell::panel::pointer_leave() {
    if (pressed_ || logout_pressed_ || logout_armed_ || apps_pressed_)
        dirty_ = true;
    pressed_ = {};
    logout_pressed_ = false;
    logout_armed_ = false;
    apps_pressed_ = false;
    notice_pressed_ = false;
    if (ws_pressed_ >= 0 || tray_pressed_ || bell_pressed_)
        dirty_ = true;
    ws_pressed_ = -1;
    tray_pressed_ = 0;
    bell_pressed_ = false;
    pointer_motion(-1.0, -1.0);
}

void lx::shell::panel::pointer_motion(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
    // Hover is judged against what is on screen; a redraw only when it moves to another
    // cell, never per motion event.
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
        // Moving off the armed button cancels: confirmation is one deliberate gesture.
        if (!on_logout)
            logout_armed_ = false;
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
    constexpr unsigned k_btn_left = 0x110;
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
    const bool on_logout = panel_hit_logout(committed_, px, py);
    if (down && on_logout) {
        logout_pressed_ = true;
        dirty_ = true;
        return;
    }
    if (!down && logout_pressed_) {
        logout_pressed_ = false;
        dirty_ = true;
        if (!on_logout || !bridge_)
            return;
        if (!logout_armed_) {
            logout_armed_ = true; // first click: "Confirm?"
            log_panel.debug("logout armed");
            return;
        }
        logout_armed_ = false;
        log_panel.info("logout requested");
        bridge_->request_logout();
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

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::panel::events {
    static panel& self(void* data) { return *static_cast<panel*>(data); }

    // ── zwlr_layer_surface_v1 ──
    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial,
                          uint32_t w, uint32_t h) {
        auto& p = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        const int nw = static_cast<int>(w);
        const int nh = h ? static_cast<int>(h) : k_panel_height;
        if (!p.configured_ || nw != p.width_ || nh != p.height_) {
            p.width_ = nw;
            p.height_ = nh;
            p.resized_ = true;
            log_panel.info("configure serial={} w={} h={}", serial, nw, nh);
        }
        p.configured_ = true;
        // The ack takes effect with the next commit: make one even if nothing changed.
        p.must_commit_ = true;
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        self(data).closed_ = true;
        log_panel.warn("layer surface closed by the compositor");
    }
};

namespace {

const zwlr_layer_surface_v1_listener k_layer_listener = {
    .configure = lx::shell::panel::events::configure,
    .closed = lx::shell::panel::events::closed,
};

} // namespace

lx::result<void> lx::shell::panel::create(panel_globals globals, policy_bridge& bridge,
                                          void* output) {
    if (!globals.compositor || !globals.shm || !globals.layer_shell)
        return lx::invalid_argument("panel needs wl_compositor, wl_shm and zwlr_layer_shell_v1");
    g_ = globals;
    bridge_ = &bridge;
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return lx::make_error(lx::wayland_err::bind_failed, "wl_compositor.create_surface failed");
    surface_ = surface;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(
        static_cast<zwlr_layer_shell_v1*>(g_.layer_shell), surface,
        static_cast<wl_output*>(output), ZWLR_LAYER_SHELL_V1_LAYER_TOP, "lumen-panel");
    layer_ = layer;
    zwlr_layer_surface_v1_add_listener(layer, &k_layer_listener, this);
    // Rendered at the output's pixel density (wp_fractional_scale_v1), shown at its logical
    // size: the first preferred scale arrives with the object, before the first configure.
    canvas_.attach_scale(g_.viewporter, g_.fractional, surface);
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer, 0, k_panel_height);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, k_panel_height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    // The panel is opaque everywhere: the compositor may skip blending and what is under it.
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_region_add(region, 0, 0, 1 << 30, 1 << 30);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface); // initial commit: no buffer, asks for a configure
    return {};
}

void lx::shell::panel::after_dispatch() {
    if (!bridge_ || !surface_ || closed_)
        return;
    task_model& model = bridge_->model();
    // A snapshot in progress is never shown half-done; its changes wait for snapshot_done.
    if (model.in_snapshot())
        return;
    after_dispatch(model.take_changes());
}

void lx::shell::panel::after_dispatch(unsigned changes) {
    if (!bridge_ || !surface_ || closed_)
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
    if (!configured_)
        return;
    // A new preferred scale: new buffers at the same logical size, everything repainted.
    if (canvas_.take_rescale())
        resized_ = true;
    if (resized_ || must_commit_) {
        draw(true);
        return;
    }
    if (dirty_ && !canvas_.frame_pending())
        draw(false);
}

void lx::shell::panel::draw(bool force) {
    if (resized_ || !canvas_.allocated()) {
        if (!canvas_.allocate(g_.shm, width_, height_, "lumen-panel")) {
            log_panel.error("cannot allocate panel buffers");
            closed_ = true;
            return;
        }
        drawn_[0].width = drawn_[1].width = 0;
        drawn_[0].count = drawn_[1].count = 0;
        committed_.width = 0; // nothing of this size is on screen yet
        committed_.count = 0;
        resized_ = false;
    }
    const int bi = canvas_.acquire();
    if (bi < 0) {
        // Both still held by the compositor. An ack can still be answered with a bare
        // commit while the panel on screen has the configured size; anything else waits
        // for a release (dirty_ / must_commit_ stay set, and a release re-runs this).
        if (force && committed_.width == width_ && committed_.height == height_) {
            wl_surface_commit(static_cast<wl_surface*>(surface_));
            must_commit_ = false;
        }
        return;
    }
    ws_input_.count = 0;
    if (ws_source_.fill)
        ws_source_.fill(ws_input_, ws_source_.user);
    ws_input_.pressed = ws_pressed_;
    tray_input_.count = 0;
    if (widgets_.fill_tray)
        widgets_.fill_tray(tray_input_, widgets_.user);
    const panel_bell bell = widgets_.bell ? widgets_.bell(widgets_.user) : panel_bell{};
    layout_panel(bridge_->model(), width_, height_, pointer_x_, pointer_y_, pressed_, next_,
                 panel_logout{bridge_->can_logout(), logout_armed_, logout_pressed_},
                 panel_apps{apps_shown_, apps_open_, apps_pressed_},
                 panel_notice{notice_[0] != '\0', notice_rev_}, filter_,
                 bridge_->capture_active(), &ws_input_, &tray_input_, tray_pressed_, bell,
                 bell_pressed_);
    lx::rect2i damage[k_max_panel_cells];
    const unsigned nd = panel_diff(committed_, next_, damage, k_max_panel_cells);
    if (nd == 0 && !force) {
        dirty_ = false; // what is on screen is already right
        return;
    }
    lx::rect2i repaint[k_max_panel_cells];
    const unsigned nr = panel_diff(drawn_[bi], next_, repaint, k_max_panel_cells);
    paint(bi, next_, repaint, nr);
    copy_layout(drawn_[bi], next_);

    const bool arrangement = nd == 1 && damage[0].width == next_.width &&
                             damage[0].height == next_.height;
    if (arrangement)
        log_layout(next_);
    {
        // What the indicator shows, logged when it changes (a workspace switch, a window
        // moving): `1* 2+ 3` — * active, + occupied, ! urgent.
        char line[sizeof(ws_logged_)];
        unsigned n = 0;
        line[0] = '\0';
        for (unsigned i = 0; i < next_.ws_count && i < ws_input_.count; ++i) {
            const unsigned f = next_.ws[i].flags;
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

    canvas_.commit(surface_, bi, damage, nd);
    copy_layout(committed_, next_);
    dirty_ = false;
    must_commit_ = false;

    if (log_panel.enabled(lx::trace::level::debug)) {
        int focused = -1;
        for (unsigned i = 0; i < next_.count; ++i)
            if (next_.cells[i].flags & cell_focused)
                focused = static_cast<int>(i);
        log_panel.debug("draw entries={} hidden={} focused={} damage={}{} repaint={} buffer={}",
                        next_.count, next_.hidden, focused, nd, arrangement ? "(full)" : "", nr,
                        bi);
    }
}

void lx::shell::panel::set_menu_keyboard(bool on) {
    if (on == menu_keyboard_ || !layer_ || !surface_)
        return;
    menu_keyboard_ = on;
    // on_demand is layer-shell v4's; below it the menu takes the pointer only.
    if (zwlr_layer_surface_v1_get_version(static_cast<zwlr_layer_surface_v1*>(layer_)) < 4)
        return;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        static_cast<zwlr_layer_surface_v1*>(layer_),
        on ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
           : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(static_cast<wl_surface*>(surface_));
}

void lx::shell::panel::destroy() {
    canvas_.free();
    canvas_.release_scale(); // before the wl_surface it extends
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    if (surface_)
        wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
    bridge_ = nullptr;
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::panel::events {};

lx::result<void> lx::shell::panel::create(panel_globals, policy_bridge&, void*) {
    return lx::not_implemented("lx::shell::panel::create");
}
void lx::shell::panel::destroy() {}
void lx::shell::panel::set_menu_keyboard(bool) {}
void lx::shell::panel::after_dispatch() {}
void lx::shell::panel::after_dispatch(unsigned) {}
void lx::shell::panel::draw(bool) {}

#endif

lx::shell::panel::~panel() { destroy(); }
