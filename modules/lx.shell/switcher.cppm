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
// The view is an lx.ui tree on an lx.ui.host surface (ADR-010 Phase 5 P5-7): the same
// geometry `layout_switcher` always produced, described into the engine's retained tree with
// the canvas's exact palette (view_ui's literal boxes); pointer events arrive through the
// context's seat router (the cells are the tree's interactive nodes) and keys still come the
// way they always did — the hold has them, not the surface.
//
// The model and the layout are pure (tests/test_switcher.cpp); the view is the only part
// with Wayland in it. Shell thread only; nothing allocates while the hold lasts beyond the
// surface's buffers when it opens.

#include <stdint.h>

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;

export module lx.shell:switcher;

import std;

import :task_model;
import :view_ui;

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
    /// The shell's lx.ui.host context (null: the view cannot be created).
    lx::ui::host::context* ui = nullptr;
};

/// A window's icon at `px`² pixels (premultiplied ARGB8888), null for a letter tile: the
/// panel's resolver (theme name, the window's own pixels, its desktop entry).
using switcher_icon_fn = const std::uint32_t* (*)(task_handle h, const task_entry& e, int px,
                                                  void* user);

/// The cells' pointer input, forwarded from the tree's interactive cells (the context's seat
/// router): `hover` when a cell's hover or press state changed (the pointer's first landing
/// is one call — the shell decides when it has really moved), `pick` on a press and on its
/// release on the same cell.
struct switcher_input {
    void (*hover)(unsigned entry, void* user) = nullptr;
    void (*pick)(unsigned entry, bool pressed, void* user) = nullptr;
    void* user = nullptr;
};

/// The switcher's describing function (the surface's frame loop calls it inside
/// `run_frame`; `switcher_view::after_dispatch` holds the guard that names the view).
[[nodiscard]] lx::ui::child switcher_describe(lx::ui::build_context& ctx);

/// The switcher's surface: an overlay-layer lx.ui.host surface centred on one output, no
/// keyboard interactivity (the hold has the keys), placed over panels (exclusive zone -1).
/// It exists only while shown: open creates it and its buffers, close frees them.
class switcher_view {
public:
    switcher_view() = default;
    ~switcher_view();
    switcher_view(const switcher_view&) = delete;
    switcher_view& operator=(const switcher_view&) = delete;

    void init(switcher_globals g, switcher_icon_fn icons, void* user);
    /// The cells' input, as the tree forwards it (`set_input`).
    void set_input(const switcher_input& in) { input_ = in; }
    /// Opens on output slot `output_slot` (`display::output_at`; ~0u: the compositor picks)
    /// of `output_w`×`output_h` logical px for `m`'s list.
    bool open(unsigned output_slot, int output_w, int output_h, const switcher_model& m);
    void close();
    [[nodiscard]] bool is_open() const { return surface_.valid(); }
    [[nodiscard]] void* surface() const {
        return surface_.valid() ? surface_.wl().native() : nullptr;
    }
    [[nodiscard]] const switcher_layout& layout() const { return layout_; }
    /// Describes what changed since the last frame (the selection's cells and the title, or
    /// everything after a list change) into the tree and runs the surface's frame loop.
    void after_dispatch(const switcher_model& m, const task_model& tasks);
    /// The entry under the surface-local point, or -1.
    [[nodiscard]] int entry_at(double x, double y) const { return switcher_hit(layout_, x, y); }
    /// Frames committed since it opened (tests, logs).
    [[nodiscard]] unsigned frames() const {
        return surface_.valid() ? static_cast<unsigned>(surface_.stats().frames) : 0u;
    }

private:
    friend lx::ui::child switcher_describe(lx::ui::build_context& ctx);

    /// What the described tree shows; a change re-describes.
    struct drawn_state {
        bool valid = false;
        unsigned first = 0;
        unsigned count = 0;
        unsigned selection = 0;
        unsigned title_rev = 0;
        task_handle selected{};
        constexpr bool operator==(const drawn_state&) const = default;
    };
    /// One cell's stable identity for the tree's callbacks (`cell_refs_[i]`).
    struct cell_ref {
        switcher_view* view = nullptr;
        unsigned entry = 0;
    };
    /// An icon's atlas slot, re-resolved when the window or its size changes.
    struct icon_slot {
        task_handle task{};
        unsigned rev = 0;
        int px = 0;
        lx::gfx::atlas_tile_id tile{};
    };

    [[nodiscard]] lx::ui::child build_ui();
    [[nodiscard]] lx::gfx::atlas_tile_id icon_tile(unsigned cell, task_handle task, unsigned rev,
                                                   const std::uint32_t* pixels, int px);
    static void cell_notify(void* user);
    static void cell_event(void* user, const lx::ui::pointer_event& e);
    void on_layer_closed();

    switcher_globals g_{};
    switcher_icon_fn icons_ = nullptr;
    void* user_ = nullptr;
    switcher_input input_{};
    lx::ui::host::surface surface_{};
    lx::ui::host::layer_role role_{};
    switcher_layout layout_{};
    int output_w_ = 0;
    int output_h_ = 0;
    drawn_state drawn_{};
    /// The models the describing frame reads (set by `after_dispatch`, read inside
    /// `run_frame` in the same iteration).
    const switcher_model* model_ = nullptr;
    const task_model* tasks_ = nullptr;
    static constexpr unsigned k_icon_slots = 16;
    icon_slot icon_slots_[k_icon_slots]{};
    cell_ref cell_refs_[k_switcher_max]{};
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

constexpr lx::trace::category log_switcher{"shell.switcher"};
constexpr std::uint32_t k_sw_bg = 0xFF1E1E22u; // the panel's colour
constexpr std::uint32_t k_sw_selected = 0xFF3D6FB4u;
constexpr std::uint32_t k_sw_tile = 0xFF5A5A66u;
constexpr std::uint32_t k_sw_text = 0xFFF0F0F0u;
constexpr std::uint32_t k_sw_text_dim = 0xFFA8A8ACu;
constexpr int k_sw_radius = 12;
constexpr int k_sw_selected_radius = 8;
constexpr int k_sw_tile_radius = 10;
constexpr lx::text::font_desc k_sw_font{"Sans", 11.f, lx::text::weight::normal,
                                        lx::text::style::normal};
constexpr lx::text::font_desc k_sw_letter_font{"Sans", 26.f, lx::text::weight::bold,
                                               lx::text::style::normal};

lx::shell::switcher_view* g_describing = nullptr;
struct describing_guard {
    explicit describing_guard(lx::shell::switcher_view* v) : previous_{g_describing} {
        g_describing = v;
    }
    ~describing_guard() { g_describing = previous_; }
    describing_guard(const describing_guard&) = delete;
    describing_guard& operator=(const describing_guard&) = delete;

private:
    lx::shell::switcher_view* previous_;
};

} // namespace

lx::ui::child lx::shell::switcher_describe(lx::ui::build_context&) {
    lx::shell::switcher_view* v = g_describing;
    return v ? v->build_ui() : nullptr;
}

namespace {

/// One line of text in the switcher's fonts, clipped to its box and centred in it.
[[nodiscard]] lx::ui::child switcher_text(const char* text, std::uint32_t argb,
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

} // namespace

void lx::shell::switcher_view::init(switcher_globals g, switcher_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    user_ = user;
}

#if defined(LUMEN_SHELL_WAYLAND)

bool lx::shell::switcher_view::open(unsigned output_slot, int output_w, int output_h,
                                    const switcher_model& m) {
    if (is_open())
        return true;
    if (!g_.ui || !g_.ui->started() || m.count() == 0)
        return false;
    output_w_ = output_w > 0 ? output_w : 1024;
    output_h_ = output_h > 0 ? output_h : 768;
    layout_ = layout_switcher(m.count(), m.selection(), output_w_, output_h_);
    lx::ui::host::surface_options so{};
    so.describe = &switcher_describe;
    so.size = {layout_.width, layout_.height};
    // Rounded corners over whatever is beneath: an alpha surface, no opaque region.
    so.opaque = false;
    so.frame_records = false;
    if (auto made = surface_.create(*g_.ui, so); !made) {
        log_switcher.error(made.get_error());
        return false;
    }
    lx::ui::host::layer_role_options lo{};
    lo.layer.output_slot = output_slot;
    lo.layer.level = lx::wayland::layer::overlay;
    lo.layer.name_space = "lumen-switcher";
    // No anchor: centred on the output. Zone -1: over panels, whatever they reserve.
    lo.size = {layout_.width, layout_.height};
    lo.exclusive_zone = -1;
    lo.keyboard = lx::wayland::keyboard_interactivity::none;
    if (auto made = role_.create(surface_, lo); !made) {
        log_switcher.error(made.get_error());
        surface_.destroy();
        return false;
    }
    role_.set_closed_handler(
        [](void* user) { static_cast<switcher_view*>(user)->on_layer_closed(); }, this);
    if (auto shown = surface_.show(); !shown) {
        log_switcher.error(shown.get_error());
        role_.destroy();
        surface_.destroy();
        return false;
    }
    drawn_ = {};
    log_switcher.info("switcher: open ({} windows, {}x{}, {} column{})", m.count(), layout_.width,
                      layout_.height, layout_.columns, layout_.columns == 1 ? "" : "s");
    return true;
}

void lx::shell::switcher_view::close() {
    if (!is_open())
        return;
    const unsigned frames = static_cast<unsigned>(surface_.stats().frames);
    role_.destroy();
    surface_.destroy();
    model_ = nullptr;
    tasks_ = nullptr;
    for (icon_slot& s : icon_slots_)
        s = {};
    drawn_ = {};
    log_switcher.info("switcher: close ({} frame{})", frames, frames == 1 ? "" : "s");
}

void lx::shell::switcher_view::on_layer_closed() {
    // Its output went: the switch goes on without the view.
    close();
}

lx::gfx::atlas_tile_id lx::shell::switcher_view::icon_tile(unsigned cell, task_handle task,
                                                           unsigned rev,
                                                           const std::uint32_t* pixels, int px) {
    if (!pixels || px <= 0 || !surface_.valid())
        return {};
    icon_slot& s = icon_slots_[cell % k_icon_slots];
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
        log_switcher.warn("icon tile insert failed: {}", made.error().message);
        return {};
    }
    s.task = task;
    s.rev = rev;
    s.px = px;
    s.tile = made.value();
    return s.tile;
}

lx::ui::child lx::shell::switcher_view::build_ui() {
    const unsigned selection = drawn_.selection;
    lx::ui::child cells[k_switcher_max];
    unsigned n = 0;
    // Icons are resolved at the pixels they land on, drawn 1:1 on whole device pixels.
    const float scale = surface_.valid() ? surface_.scale() : 1.f;
    const int icon_px = static_cast<int>(std::lround(k_switcher_icon * scale));

    for (unsigned i = 0; i < layout_.shown && n < k_switcher_max; ++i) {
        const unsigned entry = layout_.first + i;
        const bool selected = entry == selection;
        const lx::rect2i cell = switcher_cell(layout_, i);
        cell_refs_[i] = {this, entry};

        lx::ui::child parts[2];
        unsigned p = 0;
        if (selected) {
            // The selection's rounded highlight, inset into the cell; its corners blend into
            // the view's background beneath (the canvas rounded them the same way).
            lx::ui::quad_style q{};
            q.background = shell_color(k_sw_selected);
            for (float& r : q.corner_radii)
                r = static_cast<float>(k_sw_selected_radius);
            shell_box_props sel{};
            sel.control.layout = lx::ui::style{}
                                     .absolute()
                                     .top(lx::layout::px(4.f))
                                     .left(lx::layout::px(4.f))
                                     .w(lx::layout::px(static_cast<float>(cell.width - 8)))
                                     .h(lx::layout::px(static_cast<float>(cell.height - 8)));
            sel.rest = q;
            parts[p++] = shell_box(sel, {}, 0x5200'0000u + i);
        }
        // The window's icon, or a letter tile as the panel draws for a window without one.
        const task_handle h = model_->entry(entry);
        const task_entry* e = tasks_ ? tasks_->get(h) : nullptr;
        if (e) {
            const std::uint32_t* pixels = icons_ ? icons_(h, *e, icon_px, user_) : nullptr;
            const lx::gfx::atlas_tile_id tile =
                pixels ? icon_tile(i, h, e->icon_rev, pixels, icon_px) : lx::gfx::atlas_tile_id{};
            if (tile.valid()) {
                parts[p++] = lx::ui::image(surface_.atlas(), tile, {icon_px, icon_px},
                                           0x5201'0000u + i);
            } else {
                const char* src = e->app_id[0] ? e->app_id : (e->title[0] ? e->title : "?");
                if (const char* dot = std::strrchr(src, '.'); dot && dot[1])
                    src = dot + 1;
                char letter[2] = {src[0] >= 'a' && src[0] <= 'z' ? static_cast<char>(src[0] - 32)
                                                                : src[0],
                                  0};
                const lx::ui::child glyph[1] = {switcher_text(letter[0] ? letter : "?", k_sw_text,
                                                              k_sw_letter_font, 0x5202'0000u + i)};
                lx::ui::quad_style q{};
                q.background = shell_color(k_sw_tile);
                for (float& r : q.corner_radii)
                    r = static_cast<float>(k_sw_tile_radius);
                shell_box_props tile_box{};
                tile_box.control.layout = lx::ui::style{}
                                              .absolute()
                                              .top(lx::layout::px(
                                                  static_cast<float>((cell.width - k_switcher_icon) / 2)))
                                              .left(lx::layout::px(
                                                  static_cast<float>((cell.width - k_switcher_icon) / 2)))
                                              .w(lx::layout::px(static_cast<float>(k_switcher_icon)))
                                              .h(lx::layout::px(static_cast<float>(k_switcher_icon)))
                                              .flex_row()
                                              .items_center()
                                              .justify_center()
                                              .overflow_hidden();
                tile_box.rest = q;
                parts[p++] = shell_box(tile_box, glyph, 0x5203'0000u + i);
            }
        }
        // The cell: the tree's interactive node, transparent itself.
        shell_box_props box{};
        box.control.layout = lx::ui::style{}
                                 .absolute()
                                 .top(lx::layout::px(static_cast<float>(cell.y)))
                                 .left(lx::layout::px(static_cast<float>(cell.x)))
                                 .w(lx::layout::px(static_cast<float>(cell.width)))
                                 .h(lx::layout::px(static_cast<float>(cell.height)));
        box.control.interactive = true;
        box.notify = &switcher_view::cell_notify;
        box.event = &switcher_view::cell_event;
        box.user = &cell_refs_[i];
        cells[n++] = shell_box(box, {parts, p}, 0x5100'0000u + i);
    }

    // The selected window's title, centred under the row.
    lx::ui::child title{nullptr};
    bool has_title = false;
    if (const task_entry* e = tasks_ ? tasks_->get(model_->selected()) : nullptr) {
        const char* text = e->title[0] ? e->title : (e->app_id[0] ? e->app_id : "untitled");
        const lx::rect2i tr = switcher_title_rect(layout_);
        const lx::ui::child line[1] = {switcher_text(
            text, e->state == k_task_state_minimized ? k_sw_text_dim : k_sw_text, k_sw_font,
            0x5300u)};
        shell_box_props tb{};
        tb.control.layout = lx::ui::style{}
                                .absolute()
                                .top(lx::layout::px(static_cast<float>(tr.y)))
                                .left(lx::layout::px(static_cast<float>(tr.x)))
                                .w(lx::layout::px(static_cast<float>(tr.width)))
                                .h(lx::layout::px(static_cast<float>(tr.height)))
                                .flex_row()
                                .items_center()
                                .justify_center()
                                .overflow_hidden();
        title = shell_box(tb, line, 0x5301u);
        has_title = true;
    }

    // The root: the view's rounded background over everything, the cells and the title on it.
    lx::ui::quad_style bg{};
    bg.background = shell_color(k_sw_bg);
    for (float& r : bg.corner_radii)
        r = static_cast<float>(k_sw_radius);
    lx::ui::child placed[k_switcher_max + 1];
    unsigned total = n;
    for (unsigned i = 0; i < n; ++i)
        placed[i] = cells[i];
    if (has_title)
        placed[total++] = title;
    shell_box_props root{};
    root.control.layout = lx::ui::style{}
                              .w(lx::layout::percent(100.f))
                              .h(lx::layout::percent(100.f));
    root.rest = bg;
    return shell_box(root, {placed, total}, 0x5000u);
}

void lx::shell::switcher_view::cell_notify(void* user) {
    const cell_ref& ref = *static_cast<cell_ref*>(user);
    if (ref.view && ref.view->input_.hover)
        ref.view->input_.hover(ref.entry, ref.view->input_.user);
}

void lx::shell::switcher_view::cell_event(void* user, const lx::ui::pointer_event& e) {
    const cell_ref& ref = *static_cast<cell_ref*>(user);
    if (!ref.view || !ref.view->input_.pick)
        return;
    if (e.kind == lx::ui::pointer_kind::press)
        ref.view->input_.pick(ref.entry, true, ref.view->input_.user);
    else if (e.kind == lx::ui::pointer_kind::release)
        ref.view->input_.pick(ref.entry, false, ref.view->input_.user);
}

void lx::shell::switcher_view::after_dispatch(const switcher_model& m, const task_model& tasks) {
    if (!surface_.valid())
        return;
    // The size is fixed while open (a window that goes leaves its cell empty); the page
    // follows the selection.
    const unsigned page = layout_.columns * layout_.rows;
    layout_.first = page && m.selection() < m.count() ? (m.selection() / page) * page : 0;
    layout_.shown = layout_.first < m.count()
                        ? (m.count() - layout_.first < page ? m.count() - layout_.first : page)
                        : 0;
    const task_entry* sel = tasks.get(m.selected());
    const drawn_state next{true, layout_.first, m.count(), m.selection(),
                           sel ? sel->label_rev : 0u, m.selected()};
    if (!(next == drawn_)) {
        drawn_ = next;
        // The whole view: the arrangement is a function of the switch's state, so a change
        // re-describes (`set_describer` re-arms the describe) and the repaint makes the
        // frame worth drawing.
        surface_.root().set_describer(&switcher_describe);
        surface_.root().add_repaint({0, 0, layout_.width, layout_.height});
    }
    // The surface's frame loop: a frame when its frame callback allows one and the ledger
    // has work (our repaint, the engine's hover damage, a configure).
    if (surface_.frame_due()) {
        model_ = &m;
        tasks_ = &tasks;
        describing_guard guard{this};
        surface_.run_frame();
        if (log_switcher.enabled(lx::trace::level::debug) && surface_.stats().frames > 0)
            log_switcher.debug("switcher: draw selection={} first={} damage={}", m.selection(),
                               layout_.first, surface_.stats().last_damage_rects);
    }
}

#else // !LUMEN_SHELL_WAYLAND

bool lx::shell::switcher_view::open(unsigned, int, int, const switcher_model&) { return false; }
void lx::shell::switcher_view::close() {}
void lx::shell::switcher_view::after_dispatch(const switcher_model&, const task_model&) {}
lx::ui::child lx::shell::switcher_view::build_ui() { return nullptr; }

#endif

lx::shell::switcher_view::~switcher_view() { close(); }
