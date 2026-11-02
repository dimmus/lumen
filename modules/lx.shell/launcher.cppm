module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#endif

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;

export module lx.shell:launcher;

import std;

import :desktop_entry;
import :app_catalog;
import :view_ui;

export namespace lx::shell {

// ── Launcher layout (pure) ────────────────────────────────────────────────────
//
// A search row, a page of matching applications, a status row. As with the panel, drawing
// and damage are comparisons of two layouts.

inline constexpr int k_launcher_max_width = 420;
inline constexpr int k_launcher_search_h = 40;
inline constexpr int k_launcher_row_h = 32;
inline constexpr int k_launcher_status_h = 24;
inline constexpr int k_launcher_margin = 4;
inline constexpr unsigned k_launcher_min_rows = 4;
inline constexpr unsigned k_launcher_max_rows = 14;

struct launcher_geometry {
    int width = 0;
    int height = 0;
    unsigned rows = 0;
};
/// The launcher's size on an output of `output_w`×`output_h` with a `panel_h` panel:
/// width min(420, output − 8); rows = clamp((usable − 72) / 32, 4, 14).
[[nodiscard]] launcher_geometry launcher_size(int output_w, int output_h, int panel_h);

enum launcher_row_flag : unsigned {
    row_selected = 1u << 0,
    row_hover = 1u << 1,
    row_pressed = 1u << 2,
};

struct launcher_row {
    /// Catalog entry index; ~0u = an empty row.
    unsigned entry = ~0u;
    unsigned flags = 0;
};

/// What a layout is made from: the model's state plus pointer state.
struct launcher_view {
    const unsigned* matches = nullptr;
    unsigned count = 0;
    unsigned selection = 0;
    unsigned first = 0;
    /// Match index under the pointer / pressed, or -1.
    int hover = -1;
    int pressed = -1;
    unsigned query_rev = 0;
    unsigned notice_rev = 0;
    bool notice = false;
    unsigned catalog_gen = 0;
};

struct launcher_layout {
    int width = 0; // 0: nothing drawn
    int height = 0;
    unsigned rows = 0;
    unsigned first = 0;
    unsigned total = 0;
    launcher_row row[k_launcher_max_rows]{};
    unsigned query_rev = 0;
    unsigned notice_rev = 0;
    bool notice = false;
    unsigned catalog_gen = 0;
};

void layout_launcher(const launcher_view& v, int width, int height, unsigned rows,
                     launcher_layout& out);
/// Rect of visible row `i`.
[[nodiscard]] lx::rect2i launcher_row_rect(const launcher_layout& l, unsigned i);
[[nodiscard]] lx::rect2i launcher_search_rect(const launcher_layout& l);
[[nodiscard]] lx::rect2i launcher_status_rect(const launcher_layout& l);
/// Match index under (x, y), or -1.
[[nodiscard]] int launcher_hit(const launcher_layout& l, double x, double y);
/// Rects of `to` that differ from `from`: everything when the size, the page or the catalog
/// generation changed, else the search row, each changed row, and the status row.
unsigned launcher_diff(const launcher_layout& from, const launcher_layout& to, lx::rect2i* out,
                       unsigned cap);
/// The first visible match that keeps `selection` on a page of `rows`.
[[nodiscard]] unsigned launcher_scroll(unsigned selection, unsigned first, unsigned rows,
                                       unsigned total);

// ── Launcher model (pure) ─────────────────────────────────────────────────────

/// `menu`: the selected application's menu (the Menu key, Shift+F10); `cleared`: the query
/// is to be emptied (Escape with text — the text lives in the view's `text_buffer`).
enum class launcher_key_result : unsigned char { ignored, changed, cleared, close, menu };

/// Matches, selection and scroll. The query is the view's `text_buffer` (the search field
/// edits it; `sync_query` re-ranks when its revision moved). Keys are xkb keysyms: the
/// navigation and menu keys — the field takes the editing keys (typing, backspace, the
/// word shortcuts, Enter's submit) and the launcher's root takes what it declines.
class launcher_model {
public:
    static constexpr std::size_t k_max_query = 128;

    void bind(const app_catalog* catalog, const lx::ui::text_buffer* query);
    /// Selection on the first match, scrolled to the top; the query stays as it is.
    void reset();
    /// Re-ranks against the catalog; keeps the selection on the first match.
    void refresh();
    /// Re-ranks when the buffer's revision moved since the last rank.
    void sync_query();
    void set_rows(unsigned rows);
    [[nodiscard]] launcher_key_result key(unsigned keysym, bool shift);
    /// Scrolls by `rows` (the selection stays where it is, possibly off the page).
    void scroll(int rows);
    void select(unsigned match);

    [[nodiscard]] const char* query() const { return query_ ? query_->storage.data() : ""; }
    [[nodiscard]] unsigned query_rev() const { return query_ ? query_->revision : 0u; }
    [[nodiscard]] const unsigned* matches() const { return matches_.data(); }
    [[nodiscard]] unsigned count() const { return count_; }
    [[nodiscard]] unsigned selection() const { return selection_; }
    [[nodiscard]] unsigned first() const { return first_; }
    [[nodiscard]] unsigned rows() const { return rows_; }
    /// The catalog index of the selected match, or -1.
    [[nodiscard]] int selected_entry() const;
    [[nodiscard]] int entry_at(unsigned match) const;

private:
    void set_selection(unsigned s);

    const app_catalog* catalog_ = nullptr;
    const lx::ui::text_buffer* query_ = nullptr;
    unsigned seen_rev_ = 0;
    std::vector<unsigned> matches_{};
    unsigned count_ = 0;
    unsigned selection_ = 0;
    unsigned first_ = 0;
    unsigned rows_ = k_launcher_min_rows;
};

// ── Keyboard ──────────────────────────────────────────────────────────────────

/// One translated key press: the keysym and text of a key under the seat's keymap and
/// modifiers (lx::wayland::seat compiles the keymap; without xkbcommon no key translates
/// and the launcher is pointer-only).
struct key_press {
    unsigned keysym = 0;
    char utf8[16]{};
    bool repeats = false;
};

// ── The launcher ──────────────────────────────────────────────────────────────

/// An entry's theme icon (`Icon=`) as ARGB pixels at `px`², or null.
using launcher_icon_fn = const std::uint32_t* (*)(const char* name, int px, void* user);

struct launcher_globals {
    /// The shell's lx.ui.host context (null: the launcher cannot be created).
    lx::ui::host::context* ui = nullptr;
    /// The icon theme (null: rows draw without icons).
    launcher_icon_fn icons = nullptr;
    void* icons_user = nullptr;
};

/// The application launcher: an overlay-layer surface under the panel's left end
/// (keyboard on_demand — the compositor focuses it on map), with a search field, the
/// matching applications and a status row. It exists only while open: open creates the
/// surface, close frees it, so an idle shell holds no launcher memory. Closes on Escape
/// (with an empty query), on its toggle, on launch, and when it loses the keyboard (a
/// click elsewhere, a window taking focus).
///
/// The view is an lx.ui tree on an lx.ui.host surface (ADR-010 Phase 5 P5-7), drawn with
/// the theme's tokens like every §5.6 component (the one view that left view_ui's
/// canvas-palette stance): the search row is a real `text_input` on the model's
/// `text_buffer` — the engine's caret, selection, word editing and clipboard — the rows
/// are themed cells with the entry's icon, its name and a right-aligned GenericName, and
/// the status row is a muted label. The field holds the tree's focus: it edits the query,
/// Enter submits (launches), and every key it declines (the arrows, Page keys, Escape, the
/// Menu key) bubbles to the root, which runs the model's navigation. The rows are the
/// tree's interactive nodes (hover and press ride the engine's states, a release on the
/// pressed row launches, a right press opens its menu), and the wheel scrolls the page.
class launcher {
public:
    launcher() = default;
    ~launcher();
    launcher(const launcher&) = delete;
    launcher& operator=(const launcher&) = delete;

    /// The user picked catalog entry `entry`; `serial` is the input event that did it.
    using launch_fn = void (*)(unsigned entry, unsigned serial, void* user);
    /// The launcher closed by itself (Escape, keyboard leave, the compositor).
    using closed_fn = void (*)(void* user);
    /// Entry `entry`'s menu is asked for (a right click on its row, the Menu key), to open
    /// under `anchor` (the row, in the launcher's surface); `serial` is the input event.
    using menu_fn = void (*)(unsigned entry, lx::rect2i anchor, unsigned serial, void* user);

    void init(launcher_globals g, app_catalog& catalog, launch_fn on_launch, closed_fn on_closed,
              void* user);
    /// Opens on output slot `output_slot` (`display::output_at`; ~0u: the compositor picks),
    /// `output_w`×`output_h` logical; false when it cannot.
    bool open(int output_w, int output_h, int panel_h, unsigned output_slot = ~0u);
    void close();
    /// While closed: nothing to warm — the glyphs and their atlas pages are the context's,
    /// shared with the panel that has already shaped them (P5-7); a no-op kept for the
    /// shell's start-up path.
    void prepare(int output_w, int output_h, int panel_h, unsigned scale120);
    [[nodiscard]] bool is_open() const { return surface_.valid(); }
    [[nodiscard]] void* surface() const {
        return surface_.valid() ? surface_.wl().native() : nullptr;
    }
    /// zwlr_layer_surface_v1* while open (a menu's parent).
    [[nodiscard]] void* layer() const { return role_.valid() ? role_.layer().native() : nullptr; }
    /// The layer role (a menu's xdg_popup parent).
    [[nodiscard]] lx::ui::host::layer_role& popup_parent() { return role_; }
    /// Rows get a menu (null: none; the handler gets `init`'s `user`).
    void set_menu_handler(menu_fn fn) { on_menu_ = fn; }
    /// True when the keyboard's leaving must not close the launcher (its own menu took it);
    /// asked at the leave, `init`'s `user`.
    void set_keyboard_stay(bool (*fn)(void* user)) { keyboard_stay_ = fn; }
    /// The keyboard is on the launcher's surface right now (its menu may have taken it: the
    /// shell's refocus check asks after the menu goes).
    [[nodiscard]] bool keyboard_present() const { return had_keyboard_; }
    [[nodiscard]] launcher_model& model() { return model_; }
    /// Shows `text` in the status row until the query changes; null clears it.
    void set_notice(const char* text);
    /// The catalog was rescanned: re-rank.
    void catalog_changed();
    /// Re-describes what changed and runs the surface's frame loop.
    void after_dispatch();

private:
    /// The root's stable identity for the tree's callbacks.
    struct root_ref {
        launcher* self = nullptr;
    };
    /// A row's: `match` is the model's match index (the entry follows from it).
    struct row_ref {
        launcher* self = nullptr;
        unsigned match = 0;
    };
    /// One rasterised entry icon, keyed by theme-icon name and size (the atlas holds the
    /// pixels; a slot only remembers where).
    struct icon_slot {
        char name[128]{};
        int px = 0;
        lx::gfx::atlas_tile_id tile{};
    };

    friend lx::ui::child launcher_describe(lx::ui::build_context& ctx);
    friend class launcher_root_node;
    [[nodiscard]] lx::ui::child build_ui();
    void make_view(launcher_view& v) const;
    void request_repaint();
    void closed_by_itself(const char* why);
    // The tree's forwarded input (the root and the rows).
    void on_root_key(const lx::ui::key_event& e);
    void on_root_pointer(const lx::ui::pointer_event& e);
    void on_row_event(const row_ref& ref, const lx::ui::pointer_event& e);
    void on_row_hover(const row_ref& ref, bool hovered);
    void on_keyboard_focus(bool entered);
    void launch_selected(unsigned serial);
    void menu_for(unsigned match, unsigned serial);
    /// The query changed under the field's editing: repaint (the describe re-ranks).
    void on_query_changed();
    /// Enter in the field: launch the selection.
    void on_submit();
    /// The theme icon `name` at `px`² as an atlas tile (invalid: none).
    [[nodiscard]] lx::gfx::atlas_tile_id icon_tile(const char* name, int px);
    /// The tree's search field (null: not described yet).
    [[nodiscard]] lx::ui::ui_node* field_node();
    /// Points the tree's focus at the field (the launcher's keys are its keys).
    void focus_field();

    launcher_globals g_{};
    app_catalog* catalog_ = nullptr;
    launch_fn on_launch_ = nullptr;
    closed_fn on_closed_ = nullptr;
    menu_fn on_menu_ = nullptr;
    bool (*keyboard_stay_)(void* user) = nullptr;
    void* user_ = nullptr;
    launcher_model model_{};
    /// The query the field edits (`k_max_query` bytes of text).
    char query_storage_[launcher_model::k_max_query + 1]{};
    lx::ui::text_buffer query_buffer_{};
    lx::ui::host::surface surface_{};
    lx::ui::host::layer_role role_{};
    launcher_view layout_view_{};
    launcher_layout layout_{};
    int width_ = 0;
    int height_ = 0;
    unsigned rows_ = k_launcher_min_rows;
    bool dirty_ = false;
    bool had_keyboard_ = false;
    /// The row the left press began on (a release on it launches); its serial.
    int pressed_ = -1;
    unsigned press_serial_ = 0;
    root_ref root_ref_{};
    row_ref row_refs_[k_launcher_max_rows]{};
    icon_slot icon_slots_[32]{};
    char notice_[192]{};
    unsigned notice_rev_ = 0;
};

/// The launcher's describing function (the surface's frame loop calls it inside
/// `run_frame`; `launcher::after_dispatch` holds the guard that names the view).
[[nodiscard]] lx::ui::child launcher_describe(lx::ui::build_context& ctx);

} // namespace lx::shell

// ── Layout ───────────────────────────────────────────────────────────────────

lx::shell::launcher_geometry lx::shell::launcher_size(int output_w, int output_h, int panel_h) {
    launcher_geometry g{};
    const int avail_w = output_w - 2 * k_launcher_margin;
    g.width = avail_w < k_launcher_max_width ? avail_w : k_launcher_max_width;
    if (g.width < 160)
        g.width = 160;
    const int usable_h = output_h - panel_h;
    int rows = (usable_h - (k_launcher_search_h + k_launcher_status_h + 2 * k_launcher_margin)) /
               k_launcher_row_h;
    rows = rows < static_cast<int>(k_launcher_min_rows)   ? static_cast<int>(k_launcher_min_rows)
           : rows > static_cast<int>(k_launcher_max_rows) ? static_cast<int>(k_launcher_max_rows)
                                                          : rows;
    g.rows = static_cast<unsigned>(rows);
    g.height = k_launcher_search_h + rows * k_launcher_row_h + k_launcher_status_h;
    return g;
}

void lx::shell::layout_launcher(const launcher_view& v, int width, int height, unsigned rows,
                                launcher_layout& out) {
    out.width = width;
    out.height = height;
    out.rows = rows < k_launcher_max_rows ? rows : k_launcher_max_rows;
    out.first = v.first;
    out.total = v.count;
    out.query_rev = v.query_rev;
    out.notice_rev = v.notice_rev;
    out.notice = v.notice;
    out.catalog_gen = v.catalog_gen;
    for (unsigned i = 0; i < out.rows; ++i) {
        launcher_row& r = out.row[i];
        const unsigned m = v.first + i;
        if (m >= v.count || !v.matches) {
            r = {};
            continue;
        }
        r.entry = v.matches[m];
        r.flags = 0;
        if (m == v.selection)
            r.flags |= row_selected;
        if (static_cast<int>(m) == v.hover)
            r.flags |= row_hover;
        if (static_cast<int>(m) == v.pressed)
            r.flags |= row_pressed;
    }
}

lx::rect2i lx::shell::launcher_row_rect(const launcher_layout& l, unsigned i) {
    return {0, k_launcher_search_h + static_cast<int>(i) * k_launcher_row_h, l.width,
            k_launcher_row_h};
}

lx::rect2i lx::shell::launcher_search_rect(const launcher_layout& l) {
    return {0, 0, l.width, k_launcher_search_h};
}

lx::rect2i lx::shell::launcher_status_rect(const launcher_layout& l) {
    return {0, k_launcher_search_h + static_cast<int>(l.rows) * k_launcher_row_h, l.width,
            k_launcher_status_h};
}

int lx::shell::launcher_hit(const launcher_layout& l, double x, double y) {
    if (x < 0 || x >= l.width || y < k_launcher_search_h)
        return -1;
    const int i = static_cast<int>((y - k_launcher_search_h) / k_launcher_row_h);
    if (i < 0 || i >= static_cast<int>(l.rows) || l.row[i].entry == ~0u)
        return -1;
    return static_cast<int>(l.first) + i;
}

unsigned lx::shell::launcher_diff(const launcher_layout& a, const launcher_layout& b,
                                  lx::rect2i* out, unsigned cap) {
    if (cap == 0 || b.width <= 0 || b.height <= 0)
        return 0;
    const lx::rect2i whole{0, 0, b.width, b.height};
    if (a.width != b.width || a.height != b.height || a.rows != b.rows ||
        a.catalog_gen != b.catalog_gen) {
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
    if (a.query_rev != b.query_rev && !add(launcher_search_rect(b)))
        return 1;
    for (unsigned i = 0; i < b.rows; ++i) {
        if (a.row[i].entry == b.row[i].entry && a.row[i].flags == b.row[i].flags)
            continue;
        if (!add(launcher_row_rect(b, i)))
            return 1;
    }
    if ((a.total != b.total || a.notice != b.notice || a.notice_rev != b.notice_rev) &&
        !add(launcher_status_rect(b)))
        return 1;
    return n;
}

unsigned lx::shell::launcher_scroll(unsigned selection, unsigned first, unsigned rows,
                                    unsigned total) {
    if (rows == 0 || total <= rows)
        return 0;
    if (selection < first)
        first = selection;
    else if (selection >= first + rows)
        first = selection - rows + 1;
    if (first + rows > total)
        first = total - rows;
    return first;
}

// ── Model ────────────────────────────────────────────────────────────────────

namespace {

// xkb keysyms (xkbcommon-keysyms.h), spelled out so the model needs no xkb headers.
constexpr unsigned k_xk_escape = 0xff1b, k_xk_home = 0xff50, k_xk_up = 0xff52,
                   k_xk_down = 0xff54, k_xk_page_up = 0xff55, k_xk_page_down = 0xff56,
                   k_xk_end = 0xff57, k_xk_kp_home = 0xff95,
                   k_xk_kp_up = 0xff97, k_xk_kp_down = 0xff99, k_xk_kp_page_up = 0xff9a,
                   k_xk_kp_page_down = 0xff9b, k_xk_kp_end = 0xff9c;
constexpr unsigned k_xk_menu = 0xff67, k_xk_f10 = 0xffc7;

} // namespace

void lx::shell::launcher_model::bind(const app_catalog* catalog,
                                     const lx::ui::text_buffer* query) {
    catalog_ = catalog;
    query_ = query;
    seen_rev_ = query_ ? query_->revision : 0u;
}

void lx::shell::launcher_model::reset() {
    seen_rev_ = query_ ? query_->revision : seen_rev_;
    refresh();
}

void lx::shell::launcher_model::refresh() {
    count_ = 0;
    if (catalog_) {
        const std::size_t cap = catalog_->listed().size();
        if (matches_.size() < cap)
            matches_.resize(cap);
        const std::string_view q = query_ ? query_->view() : std::string_view{};
        count_ = catalog_->rank(q, matches_.data(), static_cast<unsigned>(matches_.size()));
    }
    selection_ = 0;
    first_ = 0;
}

void lx::shell::launcher_model::sync_query() {
    if (!query_ || query_->revision == seen_rev_)
        return;
    seen_rev_ = query_->revision;
    refresh();
}

void lx::shell::launcher_model::set_rows(unsigned rows) {
    rows_ = rows ? rows : 1;
    first_ = launcher_scroll(selection_, first_, rows_, count_);
}

void lx::shell::launcher_model::set_selection(unsigned s) {
    if (count_ == 0) {
        selection_ = first_ = 0;
        return;
    }
    selection_ = s >= count_ ? count_ - 1 : s;
    first_ = launcher_scroll(selection_, first_, rows_, count_);
}

void lx::shell::launcher_model::select(unsigned match) { set_selection(match); }

void lx::shell::launcher_model::scroll(int rows) {
    if (count_ <= rows_)
        return;
    long f = static_cast<long>(first_) + rows;
    const long max = static_cast<long>(count_ - rows_);
    f = f < 0 ? 0 : (f > max ? max : f);
    first_ = static_cast<unsigned>(f);
}

int lx::shell::launcher_model::entry_at(unsigned match) const {
    return match < count_ ? static_cast<int>(matches_[match]) : -1;
}

int lx::shell::launcher_model::selected_entry() const { return entry_at(selection_); }

lx::shell::launcher_key_result lx::shell::launcher_model::key(unsigned sym, bool shift) {
    switch (sym) {
    case k_xk_menu:
        return count_ ? launcher_key_result::menu : launcher_key_result::ignored;
    case k_xk_f10:
        return count_ && shift ? launcher_key_result::menu : launcher_key_result::ignored;
    case k_xk_escape:
        // Text in the field: the view empties the buffer (`cleared`); empty: close.
        return query_ && query_->size != 0 ? launcher_key_result::cleared
                                           : launcher_key_result::close;
    case k_xk_up:
    case k_xk_kp_up:
        set_selection(selection_ ? selection_ - 1 : 0);
        return launcher_key_result::changed;
    case k_xk_down:
    case k_xk_kp_down: set_selection(selection_ + 1); return launcher_key_result::changed;
    case k_xk_page_up:
    case k_xk_kp_page_up:
        set_selection(selection_ > rows_ ? selection_ - rows_ : 0);
        return launcher_key_result::changed;
    case k_xk_page_down:
    case k_xk_kp_page_down: set_selection(selection_ + rows_); return launcher_key_result::changed;
    case k_xk_home:
    case k_xk_kp_home: set_selection(0); return launcher_key_result::changed;
    case k_xk_end:
    case k_xk_kp_end:
        set_selection(count_ ? count_ - 1 : 0);
        return launcher_key_result::changed;
    default: return launcher_key_result::ignored;
    }
}


// ── Drawing: the launcher as an lx.ui tree ────────────────────────────────────

namespace {

constexpr lx::trace::category log_launcher{"shell.launcher"};
/// The search field's inset in its band (the field is the engine's 32 px on the 40 px row).
constexpr float k_lfield_px = 4.f;
/// The status line's text inset.
constexpr int k_lpad = 12;
/// The search field's element key in the tree (`field_node` finds it by it).
constexpr lx::ui::element_key k_field_key = 0x8100u;
/// The row's icon, logical px.
constexpr float k_licon_px = 16.f;
/// The gap between a row's icon and its name.
constexpr float k_licon_gap = 8.f;
/// The room the generic name keeps from the name (the fit rule).
constexpr int k_lgeneric_gap = 24;

lx::shell::launcher* g_describing = nullptr;
struct describing_guard {
    explicit describing_guard(lx::shell::launcher* v) : previous_{g_describing} {
        g_describing = v;
    }
    ~describing_guard() { g_describing = previous_; }
    describing_guard(const describing_guard&) = delete;
    describing_guard& operator=(const describing_guard&) = delete;

private:
    lx::shell::launcher* previous_;
};

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

/// The theme's UI font at the label size (what the row and status labels draw with).
[[nodiscard]] lx::text::font_desc launcher_ui_font() {
    lx::text::font_desc f{};
    f.family = lx::ui::active_theme().fonts().ui_family;
    f.size_pt = lx::ui::label_size_px(lx::ui::label_size::normal, lx::ui::ui_rem_px()) * 0.75f;
    return f;
}

/// The text's width at `scale` (device px over it), for the generic-name fit.
[[nodiscard]] int launcher_text_width(const char* text, float scale) {
    lx::text::font_desc f = launcher_ui_font();
    f.size_pt = static_cast<float>(f.size_pt * scale);
    lx::text::shaped_line line{};
    if (!lx::text::shape_line(text, f, line))
        return 0;
    return static_cast<int>(std::ceil(line.width_px / scale));
}

/// A row cell's boxes, themed like a list item: the selection on `rest`, the engine's
/// hover and press over it (a selected row keeps its selection under the pointer).
[[nodiscard]] lx::ui::quad_style row_quad(lx::ui::tok which) {
    lx::ui::quad_style q{};
    q.background = lx::ui::active_theme().color(which);
    for (float& cr : q.corner_radii)
        cr = lx::ui::radius_px(lx::ui::radius::sm);
    return q;
}

/// A row cell's paint for its state.
[[nodiscard]] lx::shell::shell_box_props row_props(lx::rect2i cell, bool selected) {
    lx::shell::shell_box_props row{};
    row.control.layout = at(cell).flex_row().items_center().overflow_hidden()
                             .gap(lx::layout::px(k_licon_gap))
                             .pl(lx::layout::px(8.f))
                             .pr(lx::layout::px(8.f));
    row.rest = selected ? row_quad(lx::ui::tok::ghost_element_selected) : lx::ui::quad_style{};
    row.hover = row_quad(selected ? lx::ui::tok::ghost_element_selected
                                  : lx::ui::tok::ghost_element_hover);
    row.active = row_quad(lx::ui::tok::ghost_element_active);
    return row;
}

} // namespace

namespace lx::shell {

/// The root: the launcher's background, and the tree's focused node — every key the
/// surface's keyboard produces goes through it into the model's own handling, and the
/// wheel over whatever no row took scrolls the page.
struct launcher_root_props {
    shell_box_props box{};
    launcher* self = nullptr;
};

class launcher_root_node final : public shell_box_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "launcher_root"; }

    bool apply(const launcher_root_props& p) {
        self_ = p.self;
        return shell_box_node::apply(p.box);
    }

    bool on_focus_key(lx::ui::key_event event, lx::ui::focus_manager& focus) override {
        (void)focus;
        if (self_)
            self_->on_root_key(event);
        return true; // the launcher owns every key while it has the keyboard
    }

    bool on_pointer(const lx::ui::pointer_event& e) override {
        if (self_)
            self_->on_root_pointer(e);
        return shell_box_node::on_pointer(e);
    }

private:
    launcher* self_ = nullptr;
};

} // namespace lx::shell

namespace {

} // namespace

lx::ui::child lx::shell::launcher_describe(lx::ui::build_context&) {
    lx::shell::launcher* v = g_describing;
    return v ? v->build_ui() : nullptr;
}

void lx::shell::launcher::make_view(launcher_view& v) const {
    v.matches = model_.matches();
    v.count = model_.count();
    v.selection = model_.selection();
    v.first = model_.first();
    v.hover = -1; // the engine's hover states paint it now
    v.pressed = pressed_;
    v.query_rev = model_.query_rev();
    v.notice_rev = notice_rev_;
    v.notice = notice_[0] != '\0';
    v.catalog_gen = catalog_ ? catalog_->generation() : 0;
}

lx::ui::child lx::shell::launcher::build_ui() {
    model_.sync_query();
    make_view(layout_view_);
    layout_launcher(layout_view_, width_, height_, rows_, layout_);

    const unsigned first = layout_.first;
    lx::ui::child kids[3 + k_launcher_max_rows];
    unsigned n = 0;
    // The scale icons and the generic-name fit measure at: the surface's own.
    const float measure_scale = surface_.valid() ? surface_.scale() : 1.f;
    const int icon_px = static_cast<int>(std::lround(k_licon_px * measure_scale));

    // The search row: the field on the model's buffer — the engine's caret, selection and
    // editing; Enter submits, and every key it declines (the arrows, Page keys, Escape, the
    // Menu key) bubbles to the root and into the model's navigation.
    {
        lx::ui::child field =
            lx::ui::text_input()
                .buffer(query_buffer_)
                .placeholder("Type to search")
                .max_bytes(launcher_model::k_max_query)
                .on_change([self = this](int) { self->on_query_changed(); })
                .on_submit([self = this](int) { self->on_submit(); })
                .layout(lx::ui::style{}.w_full())
                .key(k_field_key);
        const lx::ui::child search[1] = {field};
        lx::shell::shell_box_props band{};
        band.control.layout = at(launcher_search_rect(layout_))
                                  .flex_row()
                                  .items_center()
                                  .pl(lx::layout::px(k_lfield_px))
                                  .pr(lx::layout::px(k_lfield_px));
        kids[n++] = lx::shell::shell_box(band, search, 0x8101u);
    }

    // A page of rows: each an interactive cell (hover and press are the engine's states;
    // the selection is the model's), the entry's icon, the name and — when both fit whole —
    // the dimmed generic name at the row's end.
    for (unsigned i = 0; i < layout_.rows; ++i) {
        const launcher_row& r = layout_.row[i];
        const lx::rect2i box = launcher_row_rect(layout_, i);
        const lx::rect2i cell{box.x + 4, box.y + 2, box.width - 8, box.height - 4};
        row_refs_[i] = {this, first + i};
        const desktop_entry* e =
            catalog_ && r.entry != ~0u ? catalog_->entry(r.entry) : nullptr;
        if (!e) {
            lx::shell::shell_box_props empty{};
            empty.control.layout = at(cell);
            kids[n++] = lx::shell::shell_box(empty, {}, 0x8200'0000u + i);
            continue;
        }
        const bool sel = (r.flags & row_selected) != 0;

        lx::ui::child inside[3];
        unsigned m = 0;
        if (icon_px > 0 && !e->icon.empty()) {
            char icon_name[128];
            std::snprintf(icon_name, sizeof(icon_name), "%.*s",
                          static_cast<int>(e->icon.size()), e->icon.data());
            if (const lx::gfx::atlas_tile_id tile = icon_tile(icon_name, icon_px); tile.valid())
                inside[m++] = lx::ui::image(surface_.atlas(), tile, {icon_px, icon_px},
                                            0x8200'0000u + i);
        }
        // The name takes the room left of the generic; alone it truncates with an ellipsis.
        const int inner_w = cell.width - 16 - (m ? icon_px + static_cast<int>(k_licon_gap) : 0);
        int gw = 0;
        if (!e->generic_name.empty() && e->generic_name != e->name) {
            gw = launcher_text_width(e->generic_name.data(), measure_scale);
            const int nw = launcher_text_width(e->name.data(), measure_scale);
            // GenericName right-aligned and dimmed, only when both fit whole.
            if (gw <= 0 || nw <= 0 || nw + k_lgeneric_gap + gw > inner_w)
                gw = 0;
        }
        lx::ui::child name_label = lx::ui::label(e->name.data())
                                       .single_line()
                                       .truncate()
                                       .key(0x8201'0000u + i);
        lx::ui::child name_box = lx::ui::div(
            lx::ui::style{}.flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                .overflow_hidden(),
            {&name_label, 1});
        if (gw > 0) {
            lx::ui::child generic_label = lx::ui::label(e->generic_name.data())
                                              .single_line()
                                              .color(lx::ui::tok::text_muted)
                                              .key(0x8202'0000u + i);
            lx::ui::child generic_box = lx::ui::div(lx::ui::style{}.flex_none(),
                                                    {&generic_label, 1});
            inside[m++] = name_box;
            inside[m++] = generic_box;
        } else {
            inside[m++] = name_box;
        }

        lx::shell::shell_box_props row = row_props(cell, sel);
        row.control.interactive = true;
        row.notify = [](void* user, bool hovered) {
            static_cast<row_ref*>(user)->self->on_row_hover(*static_cast<row_ref*>(user),
                                                             hovered);
        };
        row.event = [](void* user, const lx::ui::pointer_event& e) {
            static_cast<row_ref*>(user)->self->on_row_event(*static_cast<row_ref*>(user), e);
        };
        row.user = &row_refs_[i];
        kids[n++] = lx::shell::shell_box(row, {inside, m}, 0x8203'0000u + i);
    }

    // The status row: the notice until the query changes, else the count.
    {
        char line[224];
        const char* text = line;
        if (layout_.notice) {
            text = notice_;
        } else {
            std::snprintf(line, sizeof(line),
                          layout_.total == 1 ? "%u application" : "%u applications",
                          layout_.total);
        }
        lx::ui::child status = lx::ui::label(text)
                                   .size(lx::ui::label_size::small)
                                   .single_line()
                                   .color(layout_.notice ? lx::ui::tok::warning
                                                         : lx::ui::tok::text_muted)
                                   .key(0x8300u);
        const lx::ui::child status_row[1] = {status};
        lx::shell::shell_box_props box{};
        box.control.layout = at(launcher_status_rect(layout_))
                                 .flex_row()
                                 .items_center()
                                 .pl(lx::layout::px(static_cast<float>(k_lpad)));
        kids[n++] = lx::shell::shell_box(box, status_row, 0x8301u);
    }

    // The root: the launcher's surface over everything (the field holds the tree's focus;
    // a row click finds no focusable ancestor and blurs nothing), the wheel over the page
    // scrolls it, and every key the field declines is the model's.
    launcher_root_props root{};
    root.box.control.layout = lx::ui::style{}
                                  .w(lx::layout::percent(100.f))
                                  .h(lx::layout::percent(100.f));
    root.box.control.focusable = false;
    lx::ui::quad_style bg{};
    bg.background = lx::ui::active_theme().color(lx::ui::tok::elevated_surface_background);
    root.box.rest = bg;
    root.self = this;
    root_ref_ = {this};
    return lx::ui::with_children(
        lx::ui::describe<launcher_root_node, launcher_root_props>(root, 0x8000u), {kids, n});
}

void lx::shell::launcher::set_notice(const char* text) {
    const char* t = text ? text : "";
    if (std::strcmp(t, notice_) == 0)
        return;
    std::snprintf(notice_, sizeof(notice_), "%s", t);
    ++notice_rev_;
    dirty_ = true;
}

void lx::shell::launcher::catalog_changed() {
    model_.refresh();
    dirty_ = true;
}

// ── Input, forwarded from the tree ────────────────────────────────────────────

void lx::shell::launcher::on_root_key(const lx::ui::key_event& e) {
    if (!is_open() || !e.pressed)
        return;
    // The keysyms the model knows: the navigation keys the field declined (its editing
    // keys never get here), the Menu key and F10 by their Linux keycodes.
    unsigned sym = 0;
    const unsigned kc = e.keycode;
    switch (e.key) {
    case lx::ui::key::escape: sym = k_xk_escape; break;
    case lx::ui::key::up: sym = k_xk_up; break;
    case lx::ui::key::down: sym = k_xk_down; break;
    case lx::ui::key::page_up: sym = k_xk_page_up; break;
    case lx::ui::key::page_down: sym = k_xk_page_down; break;
    default:
        if (kc == 139)
            sym = k_xk_menu; // Menu
        else if (kc == 76)
            sym = k_xk_f10; // F10
        break;
    }
    if (!sym)
        return;
    const bool shift = (e.modifiers & lx::ui::k_mod_shift) != 0;
    const unsigned serial = g_.ui ? g_.ui->last_press_serial() : 0u;
    switch (model_.key(sym, shift)) {
    case launcher_key_result::ignored: return;
    case launcher_key_result::changed: dirty_ = true; return;
    case launcher_key_result::cleared:
        // Escape with text: empty the buffer (the field re-reads it on the describe).
        query_storage_[0] = '\0';
        query_buffer_.size = 0;
        ++query_buffer_.revision;
        if (notice_[0])
            set_notice(nullptr);
        dirty_ = true;
        return;
    case launcher_key_result::close: closed_by_itself("escape"); return;
    case launcher_key_result::menu: menu_for(model_.selection(), serial); return;
    }
}

void lx::shell::launcher::launch_selected(unsigned serial) {
    const int e = model_.selected_entry();
    if (e >= 0 && on_launch_)
        on_launch_(static_cast<unsigned>(e), serial, user_);
}

void lx::shell::launcher::menu_for(unsigned match, unsigned serial) {
    // The selection is on the page (the model scrolls to it); its row is the anchor.
    const int e = model_.entry_at(match);
    if (e < 0 || !on_menu_ || match < layout_.first || match - layout_.first >= layout_.rows)
        return;
    on_menu_(static_cast<unsigned>(e), launcher_row_rect(layout_, match - layout_.first), serial,
             user_);
}

void lx::shell::launcher::on_root_pointer(const lx::ui::pointer_event& e) {
    // The wheel over whatever no row took: notches of three rows, as the shell's axis
    // handler always scrolled it (the rows re-judge the hover against the new page).
    if (e.kind != lx::ui::pointer_kind::wheel || e.dy == 0.f)
        return;
    model_.scroll((e.dy > 0.f ? 1 : -1) * 3);
    dirty_ = true;
}

void lx::shell::launcher::on_row_hover(const row_ref& ref, bool hovered) {
    (void)ref;
    (void)hovered;
    // Hover paints itself (the engine's states); the model selects on the press.
}

void lx::shell::launcher::on_row_event(const row_ref& ref, const lx::ui::pointer_event& e) {
    const bool left = e.button == lx::ui::mouse_button::left;
    switch (e.kind) {
    case lx::ui::pointer_kind::press:
        if (!left)
            return;
        pressed_ = static_cast<int>(ref.match);
        press_serial_ = g_.ui ? g_.ui->last_press_serial() : 0u;
        model_.select(ref.match);
        dirty_ = true;
        return;
    case lx::ui::pointer_kind::release:
        if (!left)
            return;
        if (pressed_ == static_cast<int>(ref.match)) {
            const unsigned serial = press_serial_;
            pressed_ = -1;
            dirty_ = true;
            const int entry = model_.entry_at(ref.match);
            // The press is the triggering event: its serial is the one a token may cite.
            if (entry >= 0 && on_launch_)
                on_launch_(static_cast<unsigned>(entry), serial, user_);
        } else {
            pressed_ = -1;
        }
        return;
    // The menu opens on the press, which its grab cites; the row becomes the selection.
    case lx::ui::pointer_kind::secondary: {
        model_.select(ref.match);
        dirty_ = true;
        menu_for(ref.match, g_.ui ? g_.ui->last_press_serial() : 0u);
        return;
    }
    default: return;
    }
}

void lx::shell::launcher::on_query_changed() {
    // Typing on clears a failure notice; the describe re-ranks (sync_query).
    if (notice_[0])
        set_notice(nullptr);
    dirty_ = true;
}

void lx::shell::launcher::on_submit() {
    launch_selected(g_.ui ? g_.ui->last_press_serial() : 0u);
}

lx::gfx::atlas_tile_id lx::shell::launcher::icon_tile(const char* name, int px) {
    if (!name || !name[0] || px <= 0 || px > 64)
        return {};
    icon_slot* free_slot = nullptr;
    for (icon_slot& s : icon_slots_) {
        if (s.px == px && std::strcmp(s.name, name) == 0) {
            lx::gfx::atlas_tile place{};
            if (s.tile.valid() && surface_.atlas().lookup(s.tile, place))
                return s.tile;
            break;
        }
        if (!free_slot && s.px == 0)
            free_slot = &s;
    }
    if (!g_.icons)
        return {};
    const std::uint32_t* pixels = g_.icons(name, px, g_.icons_user);
    if (!pixels)
        return {};
    std::uint8_t rgba[64 * 64 * 4];
    for (int i = 0; i < px * px; ++i) {
        const std::uint32_t v = pixels[i];
        rgba[i * 4 + 0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(v & 0xFF);
        rgba[i * 4 + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    }
    auto made = surface_.atlas().insert(
        lx::gfx::atlas_format::rgba8, static_cast<unsigned>(px), static_cast<unsigned>(px),
        std::span<const std::uint8_t>{rgba, sizeof(rgba)}, static_cast<unsigned>(px * 4));
    if (!made)
        return {};
    icon_slot& s = free_slot ? *free_slot : icon_slots_[0];
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.px = px;
    s.tile = made.value();
    return s.tile;
}

lx::ui::ui_node* lx::shell::launcher::field_node() {
    lx::ui::ui_node* tree = surface_.valid() ? surface_.root().tree() : nullptr;
    if (!tree)
        return nullptr;
    lx::ui::ui_node* found = nullptr;
    auto walk = [&](auto&& self, lx::ui::ui_node* n) -> void {
        if (found || !n)
            return;
        if (n->key() == k_field_key && n->type() == lx::ui::text_input_node::static_type()) {
            found = n;
            return;
        }
        for (unsigned i = 0; i < n->child_count(); ++i)
            self(self, n->child_at(i));
    };
    walk(walk, tree);
    return found;
}

void lx::shell::launcher::focus_field() {
    if (lx::ui::ui_node* f = field_node())
        (void)surface_.root().focus().focus(*f, lx::ui::focus_cause::programmatic);
}

void lx::shell::launcher::on_keyboard_focus(bool entered) {
    if (entered) {
        had_keyboard_ = true;
        // The field takes the tree's focus (programmatic — no ring): it edits the query,
        // and the navigation keys bubble from it to the root. Nothing focusable but the
        // field exists, so a row click blurs nothing.
        focus_field();
        return;
    }
    const bool had = had_keyboard_;
    had_keyboard_ = false;
    // Kickoff's close-on-deactivate: a click elsewhere or a window taking the keyboard —
    // unless the launcher's own menu took it (its popup grabs, and returns it when done).
    if (is_open() && had && !(keyboard_stay_ && keyboard_stay_(user_)))
        closed_by_itself("keyboard leave");
}

void lx::shell::launcher::closed_by_itself(const char* why) {
    if (log_launcher.enabled(lx::trace::level::debug))
        log_launcher.debug("launcher: close ({})", why);
    close();
    if (on_closed_)
        on_closed_(user_);
}

#if defined(LUMEN_SHELL_WAYLAND)

void lx::shell::launcher::init(launcher_globals g, app_catalog& catalog, launch_fn on_launch,
                               closed_fn on_closed, void* user) {
    g_ = g;
    catalog_ = &catalog;
    on_launch_ = on_launch;
    on_closed_ = on_closed;
    user_ = user;
    query_buffer_.storage = {query_storage_, sizeof query_storage_};
    model_.bind(&catalog, &query_buffer_);
}

bool lx::shell::launcher::open(int output_w, int output_h, int panel_h, unsigned output_slot) {
    if (is_open())
        return true;
    if (!g_.ui || !g_.ui->started() || output_w <= 0)
        return false;
    const launcher_geometry geo = launcher_size(output_w, output_h > 0 ? output_h : 600, panel_h);
    lx::ui::host::surface_options so{};
    so.describe = &launcher_describe;
    so.size = {geo.width, geo.height};
    so.opaque = true; // a solid panel of colour, as the canvas's opaque region said
    so.frame_records = false;
    if (auto made = surface_.create(*g_.ui, so); !made) {
        log_launcher.error(made.get_error());
        return false;
    }
    lx::ui::host::layer_role_options lo{};
    lo.layer.output_slot = output_slot;
    lo.layer.level = lx::wayland::layer::overlay;
    lo.layer.name_space = "lumen-launcher";
    lo.size = {geo.width, geo.height};
    lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_left;
    // Zone 0: placed beneath the panel's exclusive zone, reserving none of its own.
    lo.exclusive_zone = 0;
    lo.margin[0] = k_launcher_margin; // top
    lo.margin[3] = k_launcher_margin; // left
    lo.keyboard = lx::wayland::keyboard_interactivity::on_demand;
    if (auto made = role_.create(surface_, lo); !made) {
        log_launcher.error(made.get_error());
        surface_.destroy();
        return false;
    }
    role_.set_closed_handler(
        [](void* user) {
            static_cast<launcher*>(user)->closed_by_itself("closed by the compositor");
        },
        this);
    surface_.set_keyboard_focus_handler(
        [](bool entered, void* user) { static_cast<launcher*>(user)->on_keyboard_focus(entered); },
        this);
    if (auto shown = surface_.show(); !shown) {
        log_launcher.error(shown.get_error());
        role_.destroy();
        surface_.destroy();
        return false;
    }
    width_ = geo.width;
    height_ = geo.height;
    rows_ = geo.rows;
    had_keyboard_ = false;
    pressed_ = -1;
    notice_[0] = '\0';
    ++notice_rev_;
    query_storage_[0] = '\0';
    query_buffer_.size = 0;
    ++query_buffer_.revision;
    for (icon_slot& s : icon_slots_)
        s = {};
    model_.reset();
    model_.set_rows(rows_);
    layout_ = {};
    dirty_ = true;
    log_launcher.info("launcher: open ({} applications, {}x{}, {} rows)", model_.count(),
                      geo.width, geo.height, geo.rows);
    return true;
}

void lx::shell::launcher::prepare(int, int, int, unsigned) {
    // Nothing to warm: the glyphs and their atlas pages are the context's, shared with the
    // panel and the notification stack that have already shaped these fonts.
}

void lx::shell::launcher::close() {
    if (!is_open())
        return;
    role_.destroy();
    surface_.destroy(); // the swapchain with it: an idle shell holds none of it
    had_keyboard_ = false;
    pressed_ = -1;
    layout_ = {};
    query_storage_[0] = '\0';
    query_buffer_.size = 0;
    ++query_buffer_.revision;
    for (icon_slot& s : icon_slots_)
        s = {}; // the tiles went with the surface's atlas
    model_.reset();
    log_launcher.info("launcher: close");
}

void lx::shell::launcher::request_repaint() {
    if (!surface_.valid())
        return;
    surface_.root().set_describer(&launcher_describe);
    surface_.root().add_repaint({0, 0, width_, height_});
}

void lx::shell::launcher::after_dispatch() {
    if (!is_open())
        return;
    if (dirty_) {
        request_repaint();
        dirty_ = false;
    }
    // The surface's frame loop: a frame when its frame callback allows one and the ledger
    // has work (our repaint, the engine's hover damage, a configure, the caret's blink).
    if (surface_.frame_due()) {
        describing_guard guard{this};
        surface_.run_frame();
        // The keyboard may have arrived before a frame described the field.
        if (had_keyboard_ && !surface_.root().focus().focused())
            focus_field();
        if (log_launcher.enabled(lx::trace::level::debug) && surface_.stats().frames > 0)
            log_launcher.debug("launcher: draw matches={} selection={} first={} damage={}",
                               layout_.total, model_.selection(), layout_.first,
                               surface_.stats().last_damage_rects);
    }
}

#else // !LUMEN_SHELL_WAYLAND

void lx::shell::launcher::init(launcher_globals g, app_catalog& catalog, launch_fn on_launch,
                               closed_fn on_closed, void* user) {
    g_ = g;
    catalog_ = &catalog;
    on_launch_ = on_launch;
    on_closed_ = on_closed;
    user_ = user;
    query_buffer_.storage = {query_storage_, sizeof query_storage_};
    model_.bind(&catalog, &query_buffer_);
}
bool lx::shell::launcher::open(int, int, int, unsigned) { return false; }
void lx::shell::launcher::close() {}
void lx::shell::launcher::prepare(int, int, int, unsigned) {}
void lx::shell::launcher::after_dispatch() {}
lx::ui::child lx::shell::launcher::build_ui() { return nullptr; }
void lx::shell::launcher::on_root_key(const lx::ui::key_event&) {}
void lx::shell::launcher::on_root_pointer(const lx::ui::pointer_event&) {}
void lx::shell::launcher::on_row_event(const row_ref&, const lx::ui::pointer_event&) {}
void lx::shell::launcher::on_row_hover(const row_ref&, bool) {}
void lx::shell::launcher::on_keyboard_focus(bool) {}
void lx::shell::launcher::launch_selected(unsigned) {}
void lx::shell::launcher::menu_for(unsigned, unsigned) {}
void lx::shell::launcher::closed_by_itself(const char*) {}

#endif

lx::shell::launcher::~launcher() { close(); }
