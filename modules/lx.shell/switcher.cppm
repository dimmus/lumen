module;

// The window switcher (design-session-and-wm K.1, K.2): Alt+Tab in lumen-shell.
//
// The compositor matches the binding and gives the shell a *hold* (zlm_shell_v1 v7
// held_action): while the binding's modifier stays down every key press goes to the shell
// instead of the focused window, which keeps the keyboard focus throughout — a switch the
// user cancels leaves the application exactly as it was (no leave/enter, no focus churn).
// When the modifier goes up the compositor says `released` and the selection is activated
// (zlm_policy_bridge_v1.request_activate: the compositor unminimizes it and switches to its
// workspace).
//
// Most recently used order (the task model's MRU), the current workspaces of every output by
// default ([switcher] scope), icons and titles. Nothing is drawn for a quick Alt+Tab: the
// view (an overlay layer surface, centred on the focused window's output) appears only once
// the hold has lasted `open-delay` (150 ms, GNOME's and niri's). No thumbnails: a capture
// readback per window costs ~12 ms each on this hardware class; a compositor-drawn mirror is
// the designed follow-up (ADR 011).
//
// The model and the layout are pure (tests/test_switcher.cpp); the view is the only part
// with Wayland in it. Shell thread only; nothing allocates while the hold lasts beyond the
// view's buffers when it opens.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
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

export module lx.shell:switcher;

import std;

import :task_model;
import :shm_canvas;

export namespace lx::shell {

/// The keysyms the switcher understands (xkbcommon-keysyms.h values).
namespace switcher_keysym {
inline constexpr unsigned tab = 0xff09;
inline constexpr unsigned iso_left_tab = 0xfe20;
inline constexpr unsigned left = 0xff51;
inline constexpr unsigned up = 0xff52;
inline constexpr unsigned right = 0xff53;
inline constexpr unsigned down = 0xff54;
inline constexpr unsigned home = 0xff50;
inline constexpr unsigned end = 0xff57;
inline constexpr unsigned escape = 0xff1b;
inline constexpr unsigned return_key = 0xff0d;
inline constexpr unsigned kp_enter = 0xff8d;
inline constexpr unsigned space = 0x20;
} // namespace switcher_keysym

/// zlm_key_grab_v1.modifier shift.
inline constexpr unsigned k_switcher_mod_shift = 0x1;
/// Windows a switcher lists at most.
inline constexpr unsigned k_switcher_max = 64;
/// The view's geometry (logical px): 96 px cells with 64 px icons, the selected window's
/// title under the row.
inline constexpr int k_switcher_cell = 96;
inline constexpr int k_switcher_icon = 64;
inline constexpr int k_switcher_pad = 12;
inline constexpr int k_switcher_title_h = 36;
/// Kept free around the view on its output.
inline constexpr int k_switcher_margin = 32;
/// The view is at least this wide (the output allowing), so the selected window's title has
/// room with two or three windows (the cells are centred in it).
inline constexpr int k_switcher_min_width = 400;

/// config.toml [switcher].
struct switcher_options {
    /// Every window; else (scope "workspace") those on the active workspace of their output
    /// and the sticky ones.
    bool all = false;
    bool include_minimized = true;
    long long open_delay_ns = 150'000'000;
};

/// Whether a window is on a shown workspace (the shell's `task_on_shown_workspace`).
using switcher_shown_fn = bool (*)(const task_entry& e, void* user);

enum class switcher_key_result : unsigned char {
    /// Not the switcher's key.
    none,
    /// The selection moved.
    changed,
    /// Return: activate the selection now.
    commit,
    /// Escape: close, activate nothing.
    cancel,
};

/// One hold's list and selection.
class switcher_model {
public:
    /// Starts a session: the task model's windows in MRU order, filtered (scope through
    /// `shown`, minimized ones per the options, modal children — their parent stands for
    /// them — and for `same_app` only the focused window's application), at most
    /// `k_switcher_max`. The selection starts on the next window forward (index 1 when the
    /// first is the focused one, else 0) or the last one backward. Returns the count; 0 means
    /// there is nothing to switch to.
    unsigned begin(const task_model& m, bool forward, bool same_app, const switcher_options& o,
                   switcher_shown_fn shown, void* user, long long now_ns);
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] unsigned count() const { return static_cast<unsigned>(entries_.size()); }
    [[nodiscard]] task_handle entry(unsigned i) const {
        return i < entries_.size() ? entries_[i] : task_handle{};
    }
    [[nodiscard]] unsigned selection() const { return sel_; }
    [[nodiscard]] task_handle selected() const { return entry(sel_); }
    /// Moves the selection by `dir` (wrapping).
    void step(int dir);
    void select(unsigned i);
    /// The view's columns: Up and Down move by a row (0: by one).
    void set_columns(unsigned columns) { columns_ = columns; }
    /// A key the compositor sent during the hold: `binding` (the action its configured binding
    /// names) first — the user's own keys for next and previous — then Tab / Shift+Tab, the
    /// arrows, Home/End, Return (commit) and Escape (cancel).
    [[nodiscard]] switcher_key_result key(unsigned keysym, unsigned mods, std::string_view binding);
    /// When the view should appear (0: it has, or no session).
    [[nodiscard]] long long open_at() const { return active_ && !opened_ ? open_at_ : 0; }
    void mark_opened() { opened_ = true; }
    [[nodiscard]] bool opened() const { return opened_; }
    /// Drops entries whose window went (the selection stays on its window, else the one now
    /// in its place). True when something was dropped.
    bool prune(const task_model& m);
    /// Ends the session: the window chosen when `commit`, else null.
    task_handle finish(bool commit);
    /// Bumped by every change of the list or the selection (the view redraws on it).
    [[nodiscard]] unsigned revision() const { return rev_; }

private:
    std::inplace_vector<task_handle, k_switcher_max> entries_{};
    unsigned sel_ = 0;
    unsigned columns_ = 0;
    bool active_ = false;
    bool opened_ = false;
    long long open_at_ = 0;
    unsigned rev_ = 0;
};

// ── Layout (pure) ─────────────────────────────────────────────────────────────

struct switcher_layout {
    int width = 0;
    int height = 0;
    unsigned columns = 0;
    unsigned rows = 0;
    /// The first entry shown (a page of columns × rows when they do not all fit) and how many.
    unsigned first = 0;
    unsigned shown = 0;
    /// The grid's left edge (the cells are centred when the view is wider than they are).
    int grid_x = 0;
};
/// The view for `count` windows with `selection` on an output of `output_w`×`output_h`
/// (logical): as many columns as fit (a single row when it can), rows wrapping, a page that
/// holds the selection when even that is not enough.
[[nodiscard]] switcher_layout layout_switcher(unsigned count, unsigned selection, int output_w,
                                              int output_h);
/// The cell of the `i`th entry shown (0 = `first`).
[[nodiscard]] lx::rect2i switcher_cell(const switcher_layout& l, unsigned i);
[[nodiscard]] lx::rect2i switcher_title_rect(const switcher_layout& l);
/// The entry (model index) under (x, y), or -1.
[[nodiscard]] int switcher_hit(const switcher_layout& l, double x, double y);

// ── The view ──────────────────────────────────────────────────────────────────

struct switcher_globals {
    void* compositor = nullptr;  // wl_compositor
    void* shm = nullptr;         // wl_shm
    void* layer_shell = nullptr; // zwlr_layer_shell_v1
    void* viewporter = nullptr;  // wp_viewporter (with fractional: sharp at any scale)
    void* fractional = nullptr;  // wp_fractional_scale_manager_v1
};

/// A window's icon at `px`² pixels (premultiplied ARGB8888), null for a letter tile: the
/// panel's resolver (theme name, the window's own pixels, its desktop entry).
using switcher_icon_fn = const std::uint32_t* (*)(task_handle h, const task_entry& e, int px,
                                                  void* user);

/// The switcher's surface: an overlay-layer surface centred on one output, no keyboard
/// interactivity (the hold has the keys), placed over panels (exclusive zone -1). It exists
/// only while shown: open creates it and its buffers, close frees them.
class switcher_view {
public:
    switcher_view() = default;
    ~switcher_view();
    switcher_view(const switcher_view&) = delete;
    switcher_view& operator=(const switcher_view&) = delete;

    void init(switcher_globals g, switcher_icon_fn icons, void* user);
    /// Opens on `output` (wl_output*) of `output_w`×`output_h` logical px for `m`'s list.
    bool open(void* output, int output_w, int output_h, const switcher_model& m);
    void close();
    [[nodiscard]] bool is_open() const { return surface_ != nullptr; }
    [[nodiscard]] void* surface() const { return surface_; }
    [[nodiscard]] const switcher_layout& layout() const { return layout_; }
    /// Draws what changed since the last frame (the selection's two cells and the title, or
    /// everything after a list change), once the compositor took the last one.
    void after_dispatch(const switcher_model& m, const task_model& tasks);
    /// The entry under the surface-local point, or -1.
    [[nodiscard]] int entry_at(double x, double y) const { return switcher_hit(layout_, x, y); }
    /// Frames committed since it opened (tests, logs).
    [[nodiscard]] unsigned frames() const { return frames_; }

    struct events;

private:
    friend struct events;
    /// What one buffer shows.
    struct drawn_state {
        bool valid = false;
        unsigned first = 0;
        unsigned count = 0;
        unsigned selection = 0;
        unsigned title_rev = 0;
        task_handle selected{};
    };
    void draw(const switcher_model& m, const task_model& tasks, bool force);
    void paint_cell(int bi, const switcher_model& m, const task_model& tasks, unsigned i,
                    bool selected);
    void paint_title(int bi, const switcher_model& m, const task_model& tasks);

    switcher_globals g_{};
    switcher_icon_fn icons_ = nullptr;
    void* user_ = nullptr;
    shm_canvas canvas_{};
    switcher_layout layout_{};
    int output_w_ = 0;
    int output_h_ = 0;
    drawn_state drawn_[2]{};
    drawn_state committed_{};
    void* surface_ = nullptr; // wl_surface*
    void* layer_ = nullptr;   // zwlr_layer_surface_v1*
    bool configured_ = false;
    unsigned frames_ = 0;
};

} // namespace lx::shell

// ── Model ────────────────────────────────────────────────────────────────────

unsigned lx::shell::switcher_model::begin(const task_model& m, bool forward, bool same_app,
                                          const switcher_options& o, switcher_shown_fn shown,
                                          void* user, long long now_ns) {
    entries_.clear();
    sel_ = 0;
    opened_ = false;
    active_ = true;
    ++rev_;
    open_at_ = now_ns + (o.open_delay_ns > 0 ? o.open_delay_ns : 0);
    if (open_at_ == 0)
        open_at_ = 1;
    const task_entry* focused = m.get(m.focused());
    const char* app = focused ? focused->app_id : "";
    task_handle all[k_max_tasks];
    const unsigned n = m.mru(all, k_max_tasks);
    for (unsigned i = 0; i < n && entries_.size() < k_switcher_max; ++i) {
        const task_entry* e = m.get(all[i]);
        if (!e || task_is_modal_child(m, *e))
            continue;
        if (!o.include_minimized && e->state == k_task_state_minimized)
            continue;
        if (!o.all && shown && !shown(*e, user))
            continue;
        if (same_app && (!app[0] || std::strcmp(e->app_id, app) != 0))
            continue;
        entries_.push_back(all[i]);
    }
    if (entries_.empty())
        return 0;
    const bool first_is_focused = entries_[0] == m.focused();
    if (forward)
        sel_ = first_is_focused && entries_.size() > 1 ? 1 : 0;
    else
        sel_ = static_cast<unsigned>(entries_.size()) - 1;
    return count();
}

void lx::shell::switcher_model::step(int dir) {
    const auto n = static_cast<long long>(entries_.size());
    if (n == 0 || dir == 0)
        return;
    long long s = (static_cast<long long>(sel_) + dir) % n;
    if (s < 0)
        s += n;
    if (static_cast<unsigned>(s) != sel_) {
        sel_ = static_cast<unsigned>(s);
        ++rev_;
    }
}

void lx::shell::switcher_model::select(unsigned i) {
    if (i < entries_.size() && i != sel_) {
        sel_ = i;
        ++rev_;
    }
}

lx::shell::switcher_key_result lx::shell::switcher_model::key(unsigned keysym, unsigned mods,
                                                              std::string_view binding) {
    using namespace switcher_keysym;
    if (!active_)
        return switcher_key_result::none;
    const unsigned before = sel_;
    const auto moved = [&] {
        return sel_ != before ? switcher_key_result::changed : switcher_key_result::none;
    };
    // The user's bindings first: whatever keys next and previous are bound to step.
    if (binding == "switcher.next" || binding == "switcher.next-app") {
        step(1);
        return moved();
    }
    if (binding == "switcher.previous" || binding == "switcher.previous-app") {
        step(-1);
        return moved();
    }
    const int row = columns_ > 0 ? static_cast<int>(columns_) : 1;
    switch (keysym) {
    case tab: step((mods & k_switcher_mod_shift) ? -1 : 1); return moved();
    case iso_left_tab: step(-1); return moved();
    case right: step(1); return moved();
    case left: step(-1); return moved();
    case down:
        if (sel_ + static_cast<unsigned>(row) < entries_.size())
            select(sel_ + static_cast<unsigned>(row));
        else
            step(1);
        return moved();
    case up:
        if (sel_ >= static_cast<unsigned>(row))
            select(sel_ - static_cast<unsigned>(row));
        else
            step(-1);
        return moved();
    case home: select(0); return moved();
    case end:
        if (!entries_.empty())
            select(static_cast<unsigned>(entries_.size()) - 1);
        return moved();
    case escape: return switcher_key_result::cancel;
    case return_key:
    case kp_enter:
    case space: return switcher_key_result::commit;
    default: return switcher_key_result::none;
    }
}

bool lx::shell::switcher_model::prune(const task_model& m) {
    if (!active_)
        return false;
    const task_handle keep = selected();
    unsigned out = 0;
    bool dropped = false;
    for (unsigned i = 0; i < entries_.size(); ++i) {
        if (!m.get(entries_[i])) {
            dropped = true;
            continue;
        }
        entries_[out++] = entries_[i];
    }
    if (!dropped)
        return false;
    while (entries_.size() > out)
        entries_.pop_back();
    unsigned s = 0;
    bool found = false;
    for (unsigned i = 0; i < entries_.size(); ++i)
        if (entries_[i] == keep) {
            s = i;
            found = true;
        }
    if (!found)
        s = sel_ < entries_.size() ? sel_ : (entries_.empty() ? 0 : static_cast<unsigned>(entries_.size()) - 1);
    sel_ = s;
    ++rev_;
    return true;
}

lx::shell::task_handle lx::shell::switcher_model::finish(bool commit) {
    const task_handle h = commit && active_ ? selected() : task_handle{};
    active_ = false;
    opened_ = false;
    open_at_ = 0;
    entries_.clear();
    sel_ = 0;
    ++rev_;
    return h;
}

// ── Layout ───────────────────────────────────────────────────────────────────

lx::shell::switcher_layout lx::shell::layout_switcher(unsigned count, unsigned selection,
                                                      int output_w, int output_h) {
    switcher_layout l{};
    if (count == 0)
        return l;
    const int room_w = output_w - 2 * k_switcher_margin - 2 * k_switcher_pad;
    const int room_h = output_h - 2 * k_switcher_margin - 2 * k_switcher_pad - k_switcher_title_h;
    const unsigned max_cols = room_w >= k_switcher_cell ? static_cast<unsigned>(room_w / k_switcher_cell) : 1u;
    const unsigned max_rows = room_h >= k_switcher_cell ? static_cast<unsigned>(room_h / k_switcher_cell) : 1u;
    l.columns = count < max_cols ? count : max_cols;
    const unsigned need_rows = (count + l.columns - 1) / l.columns;
    l.rows = need_rows < max_rows ? need_rows : max_rows;
    const unsigned page = l.columns * l.rows;
    l.first = selection < count ? (selection / page) * page : 0;
    l.shown = count - l.first < page ? count - l.first : page;
    const int grid_w = static_cast<int>(l.columns) * k_switcher_cell;
    int min_w = k_switcher_min_width;
    if (min_w > output_w - 2 * k_switcher_margin)
        min_w = output_w - 2 * k_switcher_margin;
    l.width = grid_w + 2 * k_switcher_pad > min_w ? grid_w + 2 * k_switcher_pad : min_w;
    l.grid_x = (l.width - grid_w) / 2;
    l.height = static_cast<int>(l.rows) * k_switcher_cell + 2 * k_switcher_pad + k_switcher_title_h;
    return l;
}

lx::rect2i lx::shell::switcher_cell(const switcher_layout& l, unsigned i) {
    if (l.columns == 0)
        return {};
    return {l.grid_x + static_cast<int>(i % l.columns) * k_switcher_cell,
            k_switcher_pad + static_cast<int>(i / l.columns) * k_switcher_cell, k_switcher_cell,
            k_switcher_cell};
}

lx::rect2i lx::shell::switcher_title_rect(const switcher_layout& l) {
    return {k_switcher_pad, k_switcher_pad + static_cast<int>(l.rows) * k_switcher_cell,
            l.width - 2 * k_switcher_pad, k_switcher_title_h};
}

int lx::shell::switcher_hit(const switcher_layout& l, double x, double y) {
    for (unsigned i = 0; i < l.shown; ++i) {
        const lx::rect2i c = switcher_cell(l, i);
        if (x >= c.x && y >= c.y && x < c.x + c.width && y < c.y + c.height)
            return static_cast<int>(l.first + i);
    }
    return -1;
}

// ── View ─────────────────────────────────────────────────────────────────────

namespace {

constexpr const char* k_switcher_category = "shell.switcher";
constexpr std::uint32_t k_sw_bg = 0xFF1E1E22u; // the panel's colour
constexpr std::uint32_t k_sw_selected = 0xFF3D6FB4u;
constexpr std::uint32_t k_sw_tile = 0xFF5A5A66u;
constexpr int k_sw_radius = 12;
constexpr lx::text::raster_color k_sw_text{0xF0, 0xF0, 0xF0, 0xFF};
constexpr lx::text::raster_color k_sw_text_dim{0xA8, 0xA8, 0xAC, 0xFF};
constexpr lx::text::font_desc k_sw_font{"Sans", 11.f, lx::text::weight::normal,
                                        lx::text::style::normal};
constexpr lx::text::font_desc k_sw_letter_font{"Sans", 26.f, lx::text::weight::bold,
                                               lx::text::style::normal};

[[nodiscard]] bool switcher_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}


} // namespace

void lx::shell::switcher_view::init(switcher_globals g, switcher_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    user_ = user;
    canvas_.set_alpha(true);
}

void lx::shell::switcher_view::paint_cell(int bi, const switcher_model& m, const task_model& tasks,
                                          unsigned i, bool selected) {
    const lx::rect2i cell = switcher_cell(layout_, i);
    const std::uint32_t bg = k_sw_bg;
    canvas_.fill(bi, cell, bg);
    if (selected)
        canvas_.fill_rounded(bi, {cell.x + 4, cell.y + 4, cell.width - 8, cell.height - 8},
                             k_sw_selected, bg, 8);
    const task_handle h = m.entry(layout_.first + i);
    const task_entry* e = tasks.get(h);
    if (!e)
        return;
    const lx::rect2i icon_box{cell.x + (cell.width - k_switcher_icon) / 2,
                              cell.y + (cell.height - k_switcher_icon) / 2, k_switcher_icon,
                              k_switcher_icon};
    const int px = canvas_.pixel_rect(icon_box).width;
    const std::uint32_t* pixels = icons_ ? icons_(h, *e, px, user_) : nullptr;
    if (pixels) {
        canvas_.blit(bi, icon_box, pixels, px);
        return;
    }
    // A letter tile, as the panel draws for a window with no icon.
    canvas_.fill_rounded(bi, icon_box, k_sw_tile, selected ? k_sw_selected : bg, 10);
    const char* src = e->app_id[0] ? e->app_id : (e->title[0] ? e->title : "?");
    if (const char* dot = std::strrchr(src, '.'); dot && dot[1])
        src = dot + 1;
    char letter[2] = {src[0] >= 'a' && src[0] <= 'z' ? static_cast<char>(src[0] - 32) : src[0], 0};
    const int lw = canvas_.text_width(letter, k_sw_letter_font);
    const int lx0 = icon_box.x + (lw > 0 && lw < icon_box.width ? (icon_box.width - lw) / 2 : 8);
    (void)canvas_.text(bi, {lx0, icon_box.y, icon_box.x + icon_box.width - lx0, icon_box.height},
                       letter, k_sw_text, k_sw_letter_font);
}

void lx::shell::switcher_view::paint_title(int bi, const switcher_model& m, const task_model& tasks) {
    const lx::rect2i box = switcher_title_rect(layout_);
    canvas_.fill(bi, box, k_sw_bg);
    const task_entry* e = tasks.get(m.selected());
    if (!e)
        return;
    const char* title = e->title[0] ? e->title : (e->app_id[0] ? e->app_id : "untitled");
    const int tw = canvas_.text_width(title, k_sw_font);
    const int x = tw > 0 && tw < box.width ? box.x + (box.width - tw) / 2 : box.x;
    (void)canvas_.text(bi, {x, box.y, box.x + box.width - x, box.height}, title,
                       e->state == k_task_state_minimized ? k_sw_text_dim : k_sw_text, k_sw_font);
}

void lx::shell::switcher_view::draw(const switcher_model& m, const task_model& tasks, bool force) {
    const int bi = canvas_.acquire();
    if (bi < 0)
        return; // a release re-runs this
    // The size is fixed while open (a window that goes leaves its cell empty); the page
    // follows the selection, and a page change or a list change repaints everything.
    const unsigned page = layout_.columns * layout_.rows;
    layout_.first = page && m.selection() < m.count() ? (m.selection() / page) * page : 0;
    layout_.shown = layout_.first < m.count()
                        ? (m.count() - layout_.first < page ? m.count() - layout_.first : page)
                        : 0;
    const task_entry* sel = tasks.get(m.selected());
    drawn_state next{true, layout_.first, m.count(), m.selection(), sel ? sel->label_rev : 0u,
                     m.selected()};
    const auto same_frame = [](const drawn_state& a, const drawn_state& b) {
        return a.valid && b.valid && a.first == b.first && a.count == b.count;
    };
    const auto same = [&](const drawn_state& a, const drawn_state& b) {
        return same_frame(a, b) && a.selection == b.selection && a.title_rev == b.title_rev &&
               a.selected == b.selected;
    };
    if (!force && same(committed_, next))
        return;
    // Repaint this buffer up to `next` from what it showed; damage what changed since the
    // last commit.
    const auto changed_rects = [&](const drawn_state& from, lx::rect2i* out) -> unsigned {
        if (!same_frame(from, next)) {
            out[0] = {0, 0, layout_.width, layout_.height};
            return 1;
        }
        unsigned n = 0;
        if (from.selection != next.selection) {
            if (from.selection >= next.first && from.selection - next.first < layout_.shown)
                out[n++] = switcher_cell(layout_, from.selection - next.first);
            out[n++] = switcher_cell(layout_, next.selection - next.first);
        }
        if (from.selection != next.selection || from.title_rev != next.title_rev ||
            !(from.selected == next.selected))
            out[n++] = switcher_title_rect(layout_);
        return n;
    };
    lx::rect2i repaint[3];
    const unsigned nr = force ? 0u : changed_rects(drawn_[bi], repaint);
    if (force || (nr == 1 && repaint[0].width == layout_.width && repaint[0].height == layout_.height)) {
        canvas_.clear(bi);
        canvas_.fill_rounded(bi, {0, 0, layout_.width, layout_.height}, k_sw_bg, 0u,
                             k_sw_radius);
        for (unsigned i = 0; i < layout_.shown; ++i)
            paint_cell(bi, m, tasks, i, layout_.first + i == m.selection());
        paint_title(bi, m, tasks);
    } else {
        for (unsigned r = 0; r < nr; ++r) {
            if (repaint[r].y >= switcher_title_rect(layout_).y) {
                paint_title(bi, m, tasks);
                continue;
            }
            for (unsigned i = 0; i < layout_.shown; ++i) {
                const lx::rect2i c = switcher_cell(layout_, i);
                if (c.x == repaint[r].x && c.y == repaint[r].y)
                    paint_cell(bi, m, tasks, i, layout_.first + i == m.selection());
            }
        }
    }
    drawn_[bi] = next;
    lx::rect2i damage[3];
    const unsigned nd = force ? 1u : changed_rects(committed_, damage);
    if (force)
        damage[0] = {0, 0, layout_.width, layout_.height};
    canvas_.commit(surface_, bi, damage, nd);
    committed_ = next;
    ++frames_;
    if (switcher_debug()) {
        lx::trace::logger::global().logf(lx::trace::level::debug, k_switcher_category,
                                         "switcher: draw selection={} first={} damage={}",
                                         m.selection(), layout_.first, nd);
    }
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::switcher_view::events {
    static switcher_view& self(void* data) { return *static_cast<switcher_view*>(data); }
    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t w,
                          uint32_t h) {
        auto& v = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        const int nw = w ? static_cast<int>(w) : v.layout_.width;
        const int nh = h ? static_cast<int>(h) : v.layout_.height;
        if (nw != v.canvas_.width() || nh != v.canvas_.height() || !v.canvas_.allocated()) {
            if (!v.canvas_.allocate(v.g_.shm, nw, nh, "lumen-switcher")) {
                lx::trace::logger::global().log(lx::trace::level::error, k_switcher_category,
                                                "cannot allocate switcher buffers");
                v.close();
                return;
            }
            (void)v.canvas_.take_rescale();
            v.drawn_[0] = v.drawn_[1] = v.committed_ = {};
        }
        v.configured_ = true;
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        // Its output went: the switch goes on without the view.
        self(data).close();
    }
};

namespace {

const zwlr_layer_surface_v1_listener k_switcher_layer_listener = {
    .configure = lx::shell::switcher_view::events::configure,
    .closed = lx::shell::switcher_view::events::closed,
};

} // namespace

bool lx::shell::switcher_view::open(void* output, int output_w, int output_h,
                                    const switcher_model& m) {
    if (is_open())
        return true;
    if (!g_.compositor || !g_.layer_shell || !g_.shm || m.count() == 0)
        return false;
    output_w_ = output_w > 0 ? output_w : 1024;
    output_h_ = output_h > 0 ? output_h : 768;
    layout_ = layout_switcher(m.count(), m.selection(), output_w_, output_h_);
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return false;
    surface_ = surface;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(
        static_cast<zwlr_layer_shell_v1*>(g_.layer_shell), surface,
        static_cast<wl_output*>(output), ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lumen-switcher");
    layer_ = layer;
    zwlr_layer_surface_v1_add_listener(layer, &k_switcher_layer_listener, this);
    canvas_.attach_scale(g_.viewporter, g_.fractional, surface);
    // No anchor: centred on the output. Zone -1: over panels, whatever they reserve.
    zwlr_layer_surface_v1_set_size(layer, static_cast<uint32_t>(layout_.width),
                                   static_cast<uint32_t>(layout_.height));
    zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        // Opaque inside the rounded corners: what is under it need not be drawn.
        wl_region_add(region, k_sw_radius, 0, layout_.width - 2 * k_sw_radius, layout_.height);
        wl_region_add(region, 0, k_sw_radius, layout_.width, layout_.height - 2 * k_sw_radius);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface);
    configured_ = false;
    frames_ = 0;
    drawn_[0] = drawn_[1] = committed_ = {};
    lx::trace::logger::global().logf(lx::trace::level::info, k_switcher_category,
                                     "switcher: open ({} windows, {}x{}, {} column{})", m.count(),
                                     layout_.width, layout_.height, layout_.columns,
                                     layout_.columns == 1 ? "" : "s");
    return true;
}

void lx::shell::switcher_view::close() {
    if (!is_open())
        return;
    canvas_.free();
    canvas_.release_scale(); // before the wl_surface it extends
    (void)canvas_.take_rescale();
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    if (surface_)
        wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
    configured_ = false;
    drawn_[0] = drawn_[1] = committed_ = {};
    lx::trace::logger::global().logf(lx::trace::level::info, k_switcher_category,
                                     "switcher: close ({} frame{})", frames_,
                                     frames_ == 1 ? "" : "s");
}

void lx::shell::switcher_view::after_dispatch(const switcher_model& m, const task_model& tasks) {
    if (!is_open() || !configured_)
        return;
    if (canvas_.take_rescale()) {
        // A new preferred scale: new buffers at the same logical size, all repainted.
        if (!canvas_.allocate(g_.shm, layout_.width, layout_.height, "lumen-switcher")) {
            close();
            return;
        }
        drawn_[0] = drawn_[1] = committed_ = {};
    }
    if (!committed_.valid) {
        draw(m, tasks, true);
        return;
    }
    if (!canvas_.frame_pending())
        draw(m, tasks, false);
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::switcher_view::events {};
bool lx::shell::switcher_view::open(void*, int, int, const switcher_model&) { return false; }
void lx::shell::switcher_view::close() {}
void lx::shell::switcher_view::after_dispatch(const switcher_model&, const task_model&) {}

#endif

lx::shell::switcher_view::~switcher_view() { close(); }
