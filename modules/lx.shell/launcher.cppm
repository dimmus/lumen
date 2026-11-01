module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#endif

import lx.foundation;
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

/// `menu`: the selected application's menu (the Menu key, Shift+F10).
enum class launcher_key_result : unsigned char { ignored, changed, launch, close, menu };

/// Query, matches, selection and scroll. Keys are xkb keysyms plus the UTF-8 they produce.
class launcher_model {
public:
    static constexpr std::size_t k_max_query = 128;

    void bind(const app_catalog* catalog) { catalog_ = catalog; }
    /// Empty query, selection on the first match, scrolled to the top.
    void reset();
    /// Re-ranks against the catalog (the query or the catalog changed); keeps the
    /// selection on the first match.
    void refresh();
    void set_rows(unsigned rows);
    [[nodiscard]] launcher_key_result key(unsigned keysym, const char* utf8, bool ctrl, bool shift);
    /// Scrolls by `rows` (the selection stays where it is, possibly off the page).
    void scroll(int rows);
    void select(unsigned match);

    [[nodiscard]] const char* query() const { return query_; }
    [[nodiscard]] unsigned query_rev() const { return query_rev_; }
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
    void changed_query();

    const app_catalog* catalog_ = nullptr;
    char query_[k_max_query + 1]{};
    std::size_t query_len_ = 0;
    unsigned query_rev_ = 0;
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

struct launcher_globals {
    /// The shell's lx.ui.host context (null: the launcher cannot be created).
    lx::ui::host::context* ui = nullptr;
};

/// The application launcher: an overlay-layer surface under the panel's left end
/// (keyboard on_demand — the compositor focuses it on map), with a search field, the
/// matching applications and a status row. It exists only while open: open creates the
/// surface, close frees it, so an idle shell holds no launcher memory. Closes on Escape
/// (with an empty query), on its toggle, on launch, and when it loses the keyboard (a
/// click elsewhere, a window taking focus).
///
/// The view is an lx.ui tree on an lx.ui.host surface (ADR-010 Phase 5 P5-7): the same
/// `layout_launcher` geometry, described into the engine's retained tree with the canvas's
/// exact palette (view_ui's literal boxes). The root is the tree's focused node — every key
/// arrives at it and goes through the model's own key handling (editing, the arrows, the
/// word shortcuts), so the semantics are the canvas's; the rows are the tree's interactive
/// nodes (hover and press ride the engine's states, a release on the pressed row launches,
/// a right press opens its menu), and the wheel scrolls the page.
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

    launcher_globals g_{};
    app_catalog* catalog_ = nullptr;
    launch_fn on_launch_ = nullptr;
    closed_fn on_closed_ = nullptr;
    menu_fn on_menu_ = nullptr;
    bool (*keyboard_stay_)(void* user) = nullptr;
    void* user_ = nullptr;
    launcher_model model_{};
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
constexpr unsigned k_xk_backspace = 0xff08, k_xk_tab = 0xff09, k_xk_return = 0xff0d,
                   k_xk_escape = 0xff1b, k_xk_home = 0xff50, k_xk_up = 0xff52,
                   k_xk_down = 0xff54, k_xk_page_up = 0xff55, k_xk_page_down = 0xff56,
                   k_xk_end = 0xff57, k_xk_kp_enter = 0xff8d, k_xk_kp_home = 0xff95,
                   k_xk_kp_up = 0xff97, k_xk_kp_down = 0xff99, k_xk_kp_page_up = 0xff9a,
                   k_xk_kp_page_down = 0xff9b, k_xk_kp_end = 0xff9c, k_xk_iso_left_tab = 0xfe20;
constexpr unsigned k_xk_menu = 0xff67, k_xk_f10 = 0xffc7;

} // namespace

void lx::shell::launcher_model::reset() {
    query_[0] = '\0';
    query_len_ = 0;
    ++query_rev_;
    refresh();
}

void lx::shell::launcher_model::refresh() {
    count_ = 0;
    if (catalog_) {
        const std::size_t cap = catalog_->listed().size();
        if (matches_.size() < cap)
            matches_.resize(cap);
        count_ = catalog_->rank({query_, query_len_}, matches_.data(),
                                static_cast<unsigned>(matches_.size()));
    }
    selection_ = 0;
    first_ = 0;
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

void lx::shell::launcher_model::changed_query() {
    ++query_rev_;
    refresh();
}

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

lx::shell::launcher_key_result lx::shell::launcher_model::key(unsigned sym, const char* utf8,
                                                              bool ctrl, bool shift) {
    switch (sym) {
    case k_xk_return:
    case k_xk_kp_enter: return count_ ? launcher_key_result::launch : launcher_key_result::ignored;
    case k_xk_menu:
        return count_ ? launcher_key_result::menu : launcher_key_result::ignored;
    case k_xk_f10:
        return count_ && shift ? launcher_key_result::menu : launcher_key_result::ignored;
    case k_xk_escape:
        if (query_len_ == 0)
            return launcher_key_result::close;
        query_[0] = '\0';
        query_len_ = 0;
        changed_query();
        return launcher_key_result::changed;
    case k_xk_up:
    case k_xk_kp_up:
        set_selection(selection_ ? selection_ - 1 : 0);
        return launcher_key_result::changed;
    case k_xk_down:
    case k_xk_kp_down: set_selection(selection_ + 1); return launcher_key_result::changed;
    case k_xk_tab:
        if (shift)
            set_selection(selection_ ? selection_ - 1 : 0);
        else
            set_selection(selection_ + 1);
        return launcher_key_result::changed;
    case k_xk_iso_left_tab:
        set_selection(selection_ ? selection_ - 1 : 0);
        return launcher_key_result::changed;
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
    case k_xk_backspace:
        if (query_len_ == 0)
            return launcher_key_result::ignored;
        if (ctrl) {
            // A word: trailing spaces, then up to the previous space.
            while (query_len_ && query_[query_len_ - 1] == ' ')
                --query_len_;
            while (query_len_ && query_[query_len_ - 1] != ' ')
                --query_len_;
        } else {
            // One code point: continuation bytes, then the lead byte.
            while (query_len_ && (static_cast<unsigned char>(query_[query_len_ - 1]) & 0xC0) == 0x80)
                --query_len_;
            if (query_len_)
                --query_len_;
        }
        query_[query_len_] = '\0';
        changed_query();
        return launcher_key_result::changed;
    default: break;
    }
    if (ctrl) {
        if (sym == 'u' || sym == 'U') {
            if (query_len_ == 0)
                return launcher_key_result::ignored;
            query_len_ = 0;
            query_[0] = '\0';
            changed_query();
            return launcher_key_result::changed;
        }
        if (sym == 'w' || sym == 'W')
            return key(k_xk_backspace, nullptr, true, false);
        return launcher_key_result::ignored;
    }
    if (!utf8 || !utf8[0])
        return launcher_key_result::ignored;
    const std::size_t n = std::strlen(utf8);
    // Control characters are not text.
    if (static_cast<unsigned char>(utf8[0]) < 0x20 || utf8[0] == 0x7f)
        return launcher_key_result::ignored;
    if (query_len_ + n > k_max_query)
        return launcher_key_result::ignored;
    std::memcpy(query_ + query_len_, utf8, n);
    query_len_ += n;
    query_[query_len_] = '\0';
    changed_query();
    return launcher_key_result::changed;
}


// ── Drawing: the launcher as an lx.ui tree ────────────────────────────────────

namespace {

constexpr lx::trace::category log_launcher{"shell.launcher"};
constexpr std::uint32_t k_lbg = 0xFF26262Bu;
constexpr std::uint32_t k_lsearch = 0xFF1E1E22u;
constexpr std::uint32_t k_lhover = 0xFF34343Au;
constexpr std::uint32_t k_lpressed = 0xFF2A2A30u;
constexpr std::uint32_t k_lselected = 0xFF3D6FB4u;
constexpr std::uint32_t k_lcaret = 0xFFE6E6E6u;
constexpr std::uint32_t k_ltext = 0xFFE6E6E6u;
constexpr std::uint32_t k_ltext_selected = 0xFFFFFFFFu;
constexpr std::uint32_t k_ltext_dim = 0xFFA8A8ACu;
constexpr std::uint32_t k_ltext_dim_selected = 0xFFD0DCEEu;
constexpr std::uint32_t k_ltext_warning = 0xFFF0B45Au;
constexpr lx::text::font_desc k_lfont{"Sans", 10.f, lx::text::weight::normal,
                                      lx::text::style::normal};
constexpr lx::text::font_desc k_lfont_search{"Sans", 11.f, lx::text::weight::normal,
                                             lx::text::style::normal};
constexpr int k_lpad = 12;

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

/// One line of text in the launcher's fonts.
[[nodiscard]] lx::ui::child launcher_text(const char* text, std::uint32_t argb,
                                          const lx::text::font_desc& font,
                                          lx::ui::element_key key) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::shell::shell_color(argb);
    t.wrap = lx::text::text_wrap::none;
    t.context = lx::ui::active_ui_resources().text;
    return lx::ui::text_block(t, key);
}

/// The text's width at `scale`, as the canvas measured it for the generic-name fit.
[[nodiscard]] int launcher_text_width(const char* text, const lx::text::font_desc& font,
                                      float scale) {
    lx::text::font_desc f = font;
    f.size_pt = static_cast<float>(f.size_pt * scale);
    lx::text::shaped_line line{};
    if (!lx::text::shape_line(text, f, line))
        return 0;
    return static_cast<int>(std::ceil(line.width_px / scale));
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
    make_view(layout_view_);
    layout_launcher(layout_view_, width_, height_, rows_, layout_);

    const unsigned first = layout_.first;
    lx::ui::child kids[3 + k_launcher_max_rows];
    unsigned n = 0;

    // The search row: the prompt, the query (or the placeholder) and a static caret after
    // it — no blinking, so no timer (as the canvas drew it).
    {
        const char* q = model_.query();
        lx::ui::child search[3];
        unsigned s = 0;
        search[s++] = launcher_text("\xE2\x80\xBA", k_ltext_dim, k_lfont_search, 0x8100u);
        const int pw = launcher_text_width("\xE2\x80\xBA", k_lfont_search, 1.f);
        const int tx = k_lpad + (pw > 0 ? pw : 8) + 8;
        const int tw = layout_.width - tx - k_lpad;
        lx::ui::child caret{};
        if (!q[0]) {
            search[s++] = launcher_text("Type to search", k_ltext_dim, k_lfont_search, 0x8101u);
            lx::ui::quad_style c{};
            c.background = shell_color(k_lcaret);
            lx::shell::shell_box_props cb{};
            cb.control.layout = lx::ui::style{}
                                    .absolute()
                                    .top(lx::layout::px(12.f))
                                    .left(lx::layout::px(static_cast<float>(tx - 1)))
                                    .w(lx::layout::px(2.f))
                                    .h(lx::layout::px(static_cast<float>(k_launcher_search_h - 24)));
            cb.rest = c;
            search[s++] = lx::shell::shell_box(cb, {}, 0x8102u);
        } else {
            search[s++] = launcher_text(q, k_ltext, k_lfont_search, 0x8103u);
            int qw = launcher_text_width(q, k_lfont_search, 1.f);
            qw = qw < 0 ? 0 : (qw > tw - 2 ? tw - 2 : qw);
            lx::ui::quad_style c{};
            c.background = shell_color(k_lcaret);
            lx::shell::shell_box_props cb{};
            cb.control.layout = lx::ui::style{}
                                    .absolute()
                                    .top(lx::layout::px(12.f))
                                    .left(lx::layout::px(static_cast<float>(tx + qw + 1)))
                                    .w(lx::layout::px(2.f))
                                    .h(lx::layout::px(static_cast<float>(k_launcher_search_h - 24)));
            cb.rest = c;
            search[s++] = lx::shell::shell_box(cb, {}, 0x8104u);
        }
        lx::ui::quad_style sb{};
        sb.background = shell_color(k_lsearch);
        lx::shell::shell_box_props box{};
        box.control.layout = at(launcher_search_rect(layout_));
        box.rest = sb;
        kids[n++] = lx::shell::shell_box(box, {search, s}, 0x8105u);
    }

    // A page of rows: each an interactive cell (hover and press are the engine's states;
    // the selection is the model's), the name and — when both fit whole — the dimmed
    // generic name right-aligned.
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
        const int inner_x = cell.x + 8;
        const int inner_w = cell.width - 16;
        int name_w = inner_w;
        int gw = 0;
        if (!e->generic_name.empty() && e->generic_name != e->name) {
            gw = launcher_text_width(e->generic_name.data(), k_lfont, 1.f);
            const int nw = launcher_text_width(e->name.data(), k_lfont, 1.f);
            // GenericName right-aligned and dimmed, only when both fit whole.
            if (gw > 0 && nw > 0 && nw + 24 + gw <= inner_w)
                name_w = inner_w - gw - 24;
            else
                gw = 0;
        }
        lx::ui::child inside[3];
        unsigned m = 0;
        inside[m++] = launcher_text(e->name.data(), sel ? k_ltext_selected : k_ltext, k_lfont,
                                    0x8201'0000u + i);
        if (gw > 0) {
            lx::ui::text_block_props t{};
            t.text = e->generic_name.data();
            t.font = k_lfont;
            t.color = shell_color(sel ? k_ltext_dim_selected : k_ltext_dim);
            t.wrap = lx::text::text_wrap::none;
            t.align = lx::text::text_align::end;
            t.context = lx::ui::active_ui_resources().text;
            inside[m++] = lx::ui::text_block(t, 0x8202'0000u + i);
        }
        lx::shell::shell_box_props row{};
        row.control.layout = at(cell).flex_row().items_center().overflow_hidden();
        row.control.interactive = true;
        // Pressed over selected over hover, as the canvas painted (the engine's states).
        lx::ui::quad_style sel_q{}, hover_q{}, press_q{};
        sel_q.background = shell_color(k_lselected);
        hover_q.background = shell_color(k_lhover);
        press_q.background = shell_color(k_lpressed);
        for (float& cr : sel_q.corner_radii)
            cr = 4.f;
        for (float& cr : hover_q.corner_radii)
            cr = 4.f;
        for (float& cr : press_q.corner_radii)
            cr = 4.f;
        if (sel) {
            row.rest = sel_q;
            row.hover = sel_q;
        } else {
            row.hover = hover_q;
        }
        row.active = press_q;
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
        const lx::ui::child status_text[1] = {launcher_text(
            text, layout_.notice ? k_ltext_warning : k_ltext_dim, k_lfont, 0x8300u)};
        lx::ui::quad_style sb{};
        sb.background = shell_color(k_lsearch);
        lx::shell::shell_box_props box{};
        box.control.layout = at(launcher_status_rect(layout_));
        box.rest = sb;
        kids[n++] = lx::shell::shell_box(box, status_text, 0x8301u);
    }

    // The root: the launcher's background over everything, focused (every key is the
    // model's), the wheel over the page scrolls it.
    launcher_root_props root{};
    root.box.control.layout = lx::ui::style{}
                                  .w(lx::layout::percent(100.f))
                                  .h(lx::layout::percent(100.f));
    root.box.control.focusable = true;
    lx::ui::quad_style bg{};
    bg.background = shell_color(k_lbg);
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
    // The keysyms the model knows, from the engine's key event: the named keys, the
    // keypad's enter, the Menu key and F10 by their Linux keycodes, a character's
    // codepoint as the printable keysym.
    unsigned sym = 0;
    char utf8[16]{};
    const unsigned kc = e.keycode;
    switch (e.key) {
    case lx::ui::key::enter: sym = kc == 96 ? 0xff8d : 0xff0d; break;
    case lx::ui::key::escape: sym = 0xff1b; break;
    case lx::ui::key::up: sym = 0xff52; break;
    case lx::ui::key::down: sym = 0xff54; break;
    case lx::ui::key::tab: sym = 0xff09; break;
    case lx::ui::key::page_up: sym = 0xff55; break;
    case lx::ui::key::page_down: sym = 0xff56; break;
    case lx::ui::key::home: sym = 0xff50; break;
    case lx::ui::key::end: sym = 0xff57; break;
    case lx::ui::key::backspace: sym = 0xff08; break;
    default:
        if (kc == 139)
            sym = 0xff67; // Menu
        else if (kc == 76)
            sym = 0xffc7; // F10
        else if (e.codepoint >= 0x20 && e.codepoint != 0x7f) {
            sym = e.codepoint < 0x80 ? e.codepoint : 0;
            char* out = utf8;
            char32_t cp = e.codepoint;
            if (cp < 0x80) {
                *out++ = static_cast<char>(cp);
            } else if (cp < 0x800) {
                *out++ = static_cast<char>(0xC0 | (cp >> 6));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *out++ = static_cast<char>(0xE0 | (cp >> 12));
                *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                *out++ = static_cast<char>(0xF0 | (cp >> 18));
                *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *out++ = static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        break;
    }
    if (!sym && !utf8[0])
        return;
    const bool ctrl = (e.modifiers & lx::ui::k_mod_ctrl) != 0;
    const bool shift = (e.modifiers & lx::ui::k_mod_shift) != 0;
    const unsigned rev = model_.query_rev();
    const unsigned serial = g_.ui ? g_.ui->last_press_serial() : 0u;
    switch (model_.key(sym, utf8, ctrl, shift)) {
    case launcher_key_result::ignored: return;
    case launcher_key_result::changed:
        if (model_.query_rev() != rev && notice_[0])
            set_notice(nullptr); // typing on clears a failure notice
        dirty_ = true;
        return;
    case launcher_key_result::close: closed_by_itself("escape"); return;
    case launcher_key_result::launch: launch_selected(serial); return;
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

void lx::shell::launcher::on_keyboard_focus(bool entered) {
    if (entered) {
        had_keyboard_ = true;
        // The tree's keys are the focused node's, and nothing is focused until something
        // focuses it: the root takes it on the keyboard's arrival (programmatic — no ring).
        if (lx::ui::ui_node* tree = surface_.valid() ? surface_.root().tree() : nullptr)
            (void)surface_.root().focus().focus(*tree, lx::ui::focus_cause::programmatic);
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
    model_.bind(&catalog);
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
    // has work (our repaint, the engine's hover damage, a configure).
    if (surface_.frame_due()) {
        describing_guard guard{this};
        surface_.run_frame();
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
    model_.bind(&catalog);
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
