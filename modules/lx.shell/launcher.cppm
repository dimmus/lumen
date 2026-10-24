module;

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

#if defined(LUMEN_HAS_XKBCOMMON)
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#endif

import lx.foundation;
import lx.text;
import lx.trace;

export module lx.shell:launcher;

import std;

import :desktop_entry;
import :app_catalog;
import :shm_canvas;

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

enum class launcher_key_result : unsigned char { ignored, changed, launch, close };

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

// ── Keyboard (xkbcommon) ──────────────────────────────────────────────────────

/// One translated key press.
struct key_press {
    unsigned keysym = 0;
    char utf8[16]{};
    bool repeats = false;
};

/// The shell's wl_keyboard state: the compositor's keymap and modifier state. Without
/// LUMEN_HAS_XKBCOMMON every key is ignored (the launcher is then pointer-only).
class xkb_keyboard {
public:
    xkb_keyboard() = default;
    ~xkb_keyboard();
    xkb_keyboard(const xkb_keyboard&) = delete;
    xkb_keyboard& operator=(const xkb_keyboard&) = delete;

    /// wl_keyboard.keymap (XKB_V1). Takes ownership of `fd`.
    bool set_keymap(int fd, std::uint32_t size);
    void update_mods(std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                     std::uint32_t group);
    /// A key press (`code` is the evdev code, as wl_keyboard.key sends it).
    [[nodiscard]] bool press(std::uint32_t code, key_press& out) const;
    [[nodiscard]] bool ctrl() const;
    [[nodiscard]] bool shift() const;
    [[nodiscard]] bool alt() const;
    [[nodiscard]] bool ready() const { return state_ != nullptr; }

private:
    void* ctx_ = nullptr;    // xkb_context*
    void* keymap_ = nullptr; // xkb_keymap*
    void* state_ = nullptr;  // xkb_state*
};

// ── The launcher ──────────────────────────────────────────────────────────────

struct launcher_globals {
    void* compositor = nullptr;  // wl_compositor
    void* shm = nullptr;         // wl_shm
    void* layer_shell = nullptr; // zwlr_layer_shell_v1
    /// Optional: with both, the launcher renders at the output's fractional scale.
    void* viewporter = nullptr;  // wp_viewporter
    void* fractional = nullptr;  // wp_fractional_scale_manager_v1
};

/// The application launcher: an overlay-layer surface under the panel's left end
/// (keyboard on_demand — the compositor focuses it on map), with a search field, the
/// matching applications and a status row. It exists only while open: open creates the
/// surface and its buffers, close frees them, so an idle shell holds no launcher memory.
/// Closes on Escape (with an empty query), on its toggle, on launch, and when it loses
/// the keyboard (a click elsewhere, a window taking focus).
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

    void init(launcher_globals g, app_catalog& catalog, launch_fn on_launch, closed_fn on_closed,
              void* user);
    /// Opens on `output` (wl_output*; null: the compositor picks), `output_w`×`output_h`
    /// logical; false when it cannot.
    bool open(int output_w, int output_h, int panel_h, void* output = nullptr);
    void close();
    [[nodiscard]] bool is_open() const { return surface_ != nullptr; }
    [[nodiscard]] void* surface() const { return surface_; }
    [[nodiscard]] launcher_model& model() { return model_; }
    /// Shows `text` in the status row until the query changes; null clears it.
    void set_notice(const char* text);
    /// The catalog was rescanned: re-rank.
    void catalog_changed();
    /// Draws if something changed and the last frame was shown.
    void after_dispatch();

    // ── Input, routed here by the shell ──
    void keyboard_enter();
    void keyboard_leave();
    void key(unsigned serial, const key_press& k, bool ctrl, bool shift);
    void pointer_enter(double x, double y);
    void pointer_leave();
    void pointer_motion(double x, double y);
    void pointer_button(unsigned serial, unsigned button, bool pressed);
    /// Wheel notches (positive: down).
    void pointer_scroll_steps(int steps);

    struct events;

private:
    friend struct events;
    void draw(bool force);
    void paint(int bi, const launcher_layout& l, const lx::rect2i* rects, unsigned n);
    void paint_search(int bi, const launcher_layout& l);
    void paint_row(int bi, const launcher_layout& l, unsigned i);
    void paint_status(int bi, const launcher_layout& l);
    void make_view(launcher_view& v) const;
    void closed_by_itself(const char* why);

    launcher_globals g_{};
    app_catalog* catalog_ = nullptr;
    launch_fn on_launch_ = nullptr;
    closed_fn on_closed_ = nullptr;
    void* user_ = nullptr;
    launcher_model model_{};
    shm_canvas canvas_{};
    launcher_layout drawn_[2]{};
    launcher_layout committed_{};
    launcher_layout next_{};
    void* surface_ = nullptr; // wl_surface*
    void* layer_ = nullptr;   // zwlr_layer_surface_v1*
    int width_ = 0;
    int height_ = 0;
    unsigned rows_ = k_launcher_min_rows;
    bool configured_ = false;
    bool must_commit_ = false;
    bool dirty_ = false;
    bool had_keyboard_ = false;
    double pointer_x_ = -1.0;
    double pointer_y_ = -1.0;
    int hover_ = -1;
    int pressed_ = -1;
    unsigned press_serial_ = 0;
    char notice_[192]{};
    unsigned notice_rev_ = 0;
};

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

// ── Keyboard ─────────────────────────────────────────────────────────────────

#if defined(LUMEN_HAS_XKBCOMMON)

bool lx::shell::xkb_keyboard::set_keymap(int fd, std::uint32_t size) {
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return false;
    if (!ctx_)
        ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* km = ctx_ ? xkb_keymap_new_from_string(static_cast<xkb_context*>(ctx_),
                                                       static_cast<const char*>(map),
                                                       XKB_KEYMAP_FORMAT_TEXT_V1,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS)
                          : nullptr;
    munmap(map, size);
    if (!km)
        return false;
    xkb_state* st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return false;
    }
    if (state_)
        xkb_state_unref(static_cast<xkb_state*>(state_));
    if (keymap_)
        xkb_keymap_unref(static_cast<xkb_keymap*>(keymap_));
    keymap_ = km;
    state_ = st;
    return true;
}

void lx::shell::xkb_keyboard::update_mods(std::uint32_t depressed, std::uint32_t latched,
                                          std::uint32_t locked, std::uint32_t group) {
    if (state_)
        xkb_state_update_mask(static_cast<xkb_state*>(state_), depressed, latched, locked, 0, 0,
                              group);
}

bool lx::shell::xkb_keyboard::press(std::uint32_t code, key_press& out) const {
    if (!state_)
        return false;
    auto* st = static_cast<xkb_state*>(state_);
    const xkb_keycode_t kc = code + 8;
    out.keysym = xkb_state_key_get_one_sym(st, kc);
    out.utf8[0] = '\0';
    (void)xkb_state_key_get_utf8(st, kc, out.utf8, sizeof(out.utf8));
    out.repeats = xkb_keymap_key_repeats(static_cast<xkb_keymap*>(keymap_), kc) != 0;
    return out.keysym != XKB_KEY_NoSymbol;
}

bool lx::shell::xkb_keyboard::ctrl() const {
    return state_ && xkb_state_mod_name_is_active(static_cast<xkb_state*>(state_),
                                                  XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
}
bool lx::shell::xkb_keyboard::shift() const {
    return state_ && xkb_state_mod_name_is_active(static_cast<xkb_state*>(state_),
                                                  XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
}
bool lx::shell::xkb_keyboard::alt() const {
    return state_ && xkb_state_mod_name_is_active(static_cast<xkb_state*>(state_),
                                                  XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
}

lx::shell::xkb_keyboard::~xkb_keyboard() {
    if (state_)
        xkb_state_unref(static_cast<xkb_state*>(state_));
    if (keymap_)
        xkb_keymap_unref(static_cast<xkb_keymap*>(keymap_));
    if (ctx_)
        xkb_context_unref(static_cast<xkb_context*>(ctx_));
}

#else

bool lx::shell::xkb_keyboard::set_keymap(int fd, std::uint32_t) {
    if (fd >= 0)
        ::close(fd);
    return false;
}
void lx::shell::xkb_keyboard::update_mods(std::uint32_t, std::uint32_t, std::uint32_t,
                                          std::uint32_t) {}
bool lx::shell::xkb_keyboard::press(std::uint32_t, key_press&) const { return false; }
bool lx::shell::xkb_keyboard::ctrl() const { return false; }
bool lx::shell::xkb_keyboard::shift() const { return false; }
bool lx::shell::xkb_keyboard::alt() const { return false; }
lx::shell::xkb_keyboard::~xkb_keyboard() = default;

#endif

// ── Drawing ──────────────────────────────────────────────────────────────────

namespace {

constexpr const char* k_launcher_category = "shell.launcher";
constexpr std::uint32_t k_lbg = 0xFF26262Bu;
constexpr std::uint32_t k_lsearch = 0xFF1E1E22u;
constexpr std::uint32_t k_lhover = 0xFF34343Au;
constexpr std::uint32_t k_lpressed = 0xFF2A2A30u;
constexpr std::uint32_t k_lselected = 0xFF3D6FB4u;
constexpr std::uint32_t k_lcaret = 0xFFE6E6E6u;
constexpr lx::text::raster_color k_ltext{0xE6, 0xE6, 0xE6, 0xFF};
constexpr lx::text::raster_color k_ltext_selected{0xFF, 0xFF, 0xFF, 0xFF};
constexpr lx::text::raster_color k_ltext_dim{0xA8, 0xA8, 0xAC, 0xFF};
constexpr lx::text::raster_color k_ltext_dim_selected{0xD0, 0xDC, 0xEE, 0xFF};
constexpr lx::text::raster_color k_ltext_warning{0xF0, 0xB4, 0x5A, 0xFF};
constexpr lx::text::font_desc k_lfont{"Sans", 10.f, lx::text::weight::normal,
                                      lx::text::style::normal};
constexpr lx::text::font_desc k_lfont_search{"Sans", 11.f, lx::text::weight::normal,
                                             lx::text::style::normal};
constexpr int k_lpad = 12;

void launcher_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_launcher_category, line);
}

template <typename... Args>
void launcher_log(lx::trace::level lvl, std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lvl, k_launcher_category, fmt, std::forward<Args>(args)...);
}

[[nodiscard]] bool launcher_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

} // namespace

void lx::shell::launcher::paint_search(int bi, const launcher_layout& l) {
    const lx::rect2i box = launcher_search_rect(l);
    canvas_.fill(bi, box, k_lsearch);
    const char* q = model_.query();
    const int x0 = k_lpad;
    const int pw = canvas_.text_width("\xE2\x80\xBA", k_lfont_search);
    (void)canvas_.text(bi, {x0, 0, 16, k_launcher_search_h}, "\xE2\x80\xBA", k_ltext_dim,
                       k_lfont_search);
    const int tx = x0 + (pw > 0 ? pw : 8) + 8;
    const int tw = l.width - tx - k_lpad;
    if (!q[0]) {
        (void)canvas_.text(bi, {tx, 0, tw, k_launcher_search_h}, "Type to search", k_ltext_dim,
                           k_lfont_search);
        canvas_.fill(bi, {tx - 1, 12, 2, k_launcher_search_h - 24}, k_lcaret);
        return;
    }
    (void)canvas_.text(bi, {tx, 0, tw, k_launcher_search_h}, q, k_ltext, k_lfont_search);
    int qw = canvas_.text_width(q, k_lfont_search);
    qw = qw < 0 ? 0 : (qw > tw - 2 ? tw - 2 : qw);
    // A static caret after the text: no blinking, so no timer.
    canvas_.fill(bi, {tx + qw + 1, 12, 2, k_launcher_search_h - 24}, k_lcaret);
}

void lx::shell::launcher::paint_row(int bi, const launcher_layout& l, unsigned i) {
    const lx::rect2i box = launcher_row_rect(l, i);
    canvas_.fill(bi, box, k_lbg);
    const launcher_row& r = l.row[i];
    const desktop_entry* e = catalog_ && r.entry != ~0u ? catalog_->entry(r.entry) : nullptr;
    if (!e)
        return;
    const lx::rect2i cell{box.x + 4, box.y + 2, box.width - 8, box.height - 4};
    std::uint32_t bgc = k_lbg;
    if (r.flags & row_pressed)
        bgc = k_lpressed;
    else if (r.flags & row_selected)
        bgc = k_lselected;
    else if (r.flags & row_hover)
        bgc = k_lhover;
    if (bgc != k_lbg)
        canvas_.fill_rounded(bi, cell, bgc, k_lbg);
    const bool sel = (r.flags & row_selected) != 0;
    const int inner_x = cell.x + 8;
    const int inner_w = cell.width - 16;
    int name_w = inner_w;
    int gw = 0;
    if (!e->generic_name.empty() && e->generic_name != e->name) {
        gw = canvas_.text_width(e->generic_name.data(), k_lfont);
        const int nw = canvas_.text_width(e->name.data(), k_lfont);
        // GenericName right-aligned and dimmed, only when both fit whole.
        if (gw > 0 && nw > 0 && nw + 24 + gw <= inner_w)
            name_w = inner_w - gw - 24;
        else
            gw = 0;
    }
    // The name first: it is what the canvas shaped last.
    (void)canvas_.text(bi, {inner_x, cell.y, name_w, cell.height}, e->name.data(),
                       sel ? k_ltext_selected : k_ltext, k_lfont);
    if (gw > 0)
        (void)canvas_.text(bi, {inner_x + inner_w - gw, cell.y, gw, cell.height},
                           e->generic_name.data(), sel ? k_ltext_dim_selected : k_ltext_dim,
                           k_lfont, shm_canvas::align::right);
}

void lx::shell::launcher::paint_status(int bi, const launcher_layout& l) {
    const lx::rect2i box = launcher_status_rect(l);
    canvas_.fill(bi, box, k_lsearch);
    char line[224];
    if (l.notice) {
        (void)canvas_.text(bi, {k_lpad, box.y, l.width - 2 * k_lpad, box.height}, notice_,
                           k_ltext_warning, k_lfont);
        return;
    }
    std::snprintf(line, sizeof(line), l.total == 1 ? "%u application" : "%u applications", l.total);
    (void)canvas_.text(bi, {k_lpad, box.y, l.width - 2 * k_lpad, box.height}, line, k_ltext_dim,
                       k_lfont);
}

void lx::shell::launcher::paint(int bi, const launcher_layout& l, const lx::rect2i* rects,
                                unsigned n) {
    for (unsigned r = 0; r < n; ++r) {
        const lx::rect2i& rect = rects[r];
        const bool whole = rect.width == l.width && rect.height == l.height;
        if (whole || rect.y < k_launcher_search_h)
            paint_search(bi, l);
        for (unsigned i = 0; i < l.rows; ++i) {
            const lx::rect2i rr = launcher_row_rect(l, i);
            if (whole || (rect.y < rr.y + rr.height && rect.y + rect.height > rr.y))
                paint_row(bi, l, i);
        }
        const lx::rect2i sr = launcher_status_rect(l);
        if (whole || rect.y + rect.height > sr.y)
            paint_status(bi, l);
        if (whole)
            return;
    }
}

void lx::shell::launcher::make_view(launcher_view& v) const {
    v.matches = model_.matches();
    v.count = model_.count();
    v.selection = model_.selection();
    v.first = model_.first();
    v.hover = hover_;
    v.pressed = pressed_;
    v.query_rev = model_.query_rev();
    v.notice_rev = notice_rev_;
    v.notice = notice_[0] != '\0';
    v.catalog_gen = catalog_ ? catalog_->generation() : 0;
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
    hover_ = pressed_ = -1;
    dirty_ = true;
}

void lx::shell::launcher::key(unsigned serial, const key_press& k, bool ctrl, bool shift) {
    if (!is_open())
        return;
    const unsigned rev = model_.query_rev();
    switch (model_.key(k.keysym, k.utf8, ctrl, shift)) {
    case launcher_key_result::ignored: return;
    case launcher_key_result::changed:
        if (model_.query_rev() != rev && notice_[0])
            set_notice(nullptr); // typing on clears a failure notice
        dirty_ = true;
        return;
    case launcher_key_result::close: closed_by_itself("escape"); return;
    case launcher_key_result::launch: {
        const int e = model_.selected_entry();
        if (e >= 0 && on_launch_)
            on_launch_(static_cast<unsigned>(e), serial, user_);
        return;
    }
    }
}

void lx::shell::launcher::keyboard_enter() { had_keyboard_ = true; }

void lx::shell::launcher::keyboard_leave() {
    // Kickoff's close-on-deactivate: a click elsewhere or a window taking the keyboard.
    if (is_open() && had_keyboard_)
        closed_by_itself("keyboard leave");
}

void lx::shell::launcher::pointer_enter(double x, double y) { pointer_motion(x, y); }

void lx::shell::launcher::pointer_leave() {
    if (hover_ >= 0 || pressed_ >= 0)
        dirty_ = true;
    hover_ = pressed_ = -1;
    pointer_x_ = pointer_y_ = -1.0;
}

void lx::shell::launcher::pointer_motion(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
    const int h = launcher_hit(committed_, x, y);
    if (h != hover_) {
        hover_ = h;
        dirty_ = true;
    }
}

void lx::shell::launcher::pointer_button(unsigned serial, unsigned button, bool down) {
    constexpr unsigned k_btn_left = 0x110;
    if (button != k_btn_left || !is_open())
        return;
    const int h = launcher_hit(committed_, pointer_x_, pointer_y_);
    if (down) {
        pressed_ = h;
        press_serial_ = serial;
        if (h >= 0)
            model_.select(static_cast<unsigned>(h));
        dirty_ = true;
        return;
    }
    const int p = pressed_;
    pressed_ = -1;
    dirty_ = true;
    if (p >= 0 && p == h) {
        const int e = model_.entry_at(static_cast<unsigned>(p));
        // The press is the triggering event: its serial is the one a token may cite.
        if (e >= 0 && on_launch_)
            on_launch_(static_cast<unsigned>(e), press_serial_, user_);
    }
}

void lx::shell::launcher::pointer_scroll_steps(int steps) {
    if (!is_open() || steps == 0)
        return;
    model_.scroll(steps * 3);
    hover_ = -1; // re-judged at the next motion against the new page
    dirty_ = true;
}

void lx::shell::launcher::closed_by_itself(const char* why) {
    if (launcher_debug()) {
        launcher_log(lx::trace::level::debug, "launcher: close ({})", why);
    }
    close();
    if (on_closed_)
        on_closed_(user_);
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::launcher::events {
    static launcher& self(void* data) { return *static_cast<launcher*>(data); }
    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t w,
                          uint32_t h) {
        auto& l = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        const int nw = w ? static_cast<int>(w) : l.width_;
        const int nh = h ? static_cast<int>(h) : l.height_;
        if (nw != l.width_ || nh != l.height_ || !l.canvas_.allocated()) {
            l.width_ = nw;
            l.height_ = nh;
            l.committed_.width = 0;
            l.drawn_[0].width = l.drawn_[1].width = 0;
            if (!l.canvas_.allocate(l.g_.shm, nw, nh, "lumen-launcher")) {
                launcher_log(lx::trace::level::error, "cannot allocate launcher buffers");
                l.closed_by_itself("no buffers");
                return;
            }
            // Allocated at the scale known now (it arrives before the first configure).
            (void)l.canvas_.take_rescale();
        }
        l.configured_ = true;
        l.must_commit_ = true;
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        self(data).closed_by_itself("closed by the compositor");
    }
};

namespace {

const zwlr_layer_surface_v1_listener k_launcher_layer_listener = {
    .configure = lx::shell::launcher::events::configure,
    .closed = lx::shell::launcher::events::closed,
};

} // namespace

void lx::shell::launcher::init(launcher_globals g, app_catalog& catalog, launch_fn on_launch,
                               closed_fn on_closed, void* user) {
    g_ = g;
    catalog_ = &catalog;
    on_launch_ = on_launch;
    on_closed_ = on_closed;
    user_ = user;
    model_.bind(&catalog);
}

bool lx::shell::launcher::open(int output_w, int output_h, int panel_h, void* output) {
    if (is_open())
        return true;
    if (!g_.compositor || !g_.layer_shell || !g_.shm || output_w <= 0)
        return false;
    const launcher_geometry geo = launcher_size(output_w, output_h > 0 ? output_h : 600, panel_h);
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return false;
    surface_ = surface;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(
        static_cast<zwlr_layer_shell_v1*>(g_.layer_shell), surface,
        static_cast<wl_output*>(output), ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lumen-launcher");
    layer_ = layer;
    zwlr_layer_surface_v1_add_listener(layer, &k_launcher_layer_listener, this);
    canvas_.attach_scale(g_.viewporter, g_.fractional, surface);
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_size(layer, static_cast<uint32_t>(geo.width),
                                   static_cast<uint32_t>(geo.height));
    // Zone 0: placed beneath the panel's exclusive zone, reserving none of its own.
    zwlr_layer_surface_v1_set_exclusive_zone(layer, 0);
    zwlr_layer_surface_v1_set_margin(layer, k_launcher_margin, 0, 0, k_launcher_margin);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_region_add(region, 0, 0, geo.width, geo.height);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface);
    width_ = geo.width;
    height_ = geo.height;
    rows_ = geo.rows;
    configured_ = false;
    must_commit_ = false;
    had_keyboard_ = false;
    hover_ = pressed_ = -1;
    notice_[0] = '\0';
    ++notice_rev_;
    model_.reset();
    model_.set_rows(rows_);
    committed_.width = 0;
    dirty_ = true;
    launcher_log(lx::trace::level::info, "launcher: open ({} applications, {}x{}, {} rows)",
                 model_.count(), geo.width, geo.height, geo.rows);
    return true;
}

void lx::shell::launcher::close() {
    if (!is_open())
        return;
    canvas_.free(); // ~1.9 MB at 420×560: an idle shell holds none of it
    canvas_.release_scale(); // before the wl_surface it extends
    (void)canvas_.take_rescale();
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    if (surface_)
        wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
    configured_ = false;
    had_keyboard_ = false;
    committed_.width = 0;
    drawn_[0].width = drawn_[1].width = 0;
    model_.reset();
    launcher_log(lx::trace::level::info, "launcher: close");
}

void lx::shell::launcher::after_dispatch() {
    if (!is_open() || !configured_)
        return;
    if (canvas_.take_rescale()) {
        // A new preferred scale: new buffers at the same logical size, all repainted.
        committed_.width = 0;
        drawn_[0].width = drawn_[1].width = 0;
        if (!canvas_.allocate(g_.shm, width_, height_, "lumen-launcher")) {
            launcher_log(lx::trace::level::error, "cannot allocate launcher buffers");
            closed_by_itself("no buffers");
            return;
        }
        must_commit_ = true;
    }
    if (must_commit_) {
        draw(true);
        return;
    }
    if (dirty_ && !canvas_.frame_pending())
        draw(false);
}

void lx::shell::launcher::draw(bool force) {
    const int bi = canvas_.acquire();
    if (bi < 0)
        return; // a release re-runs this (dirty_ / must_commit_ stay set)
    launcher_view v;
    make_view(v);
    layout_launcher(v, width_, height_, rows_, next_);
    lx::rect2i damage[k_launcher_max_rows + 2];
    const unsigned nd = launcher_diff(committed_, next_, damage, k_launcher_max_rows + 2);
    if (nd == 0 && !force) {
        dirty_ = false;
        return;
    }
    lx::rect2i repaint[k_launcher_max_rows + 2];
    const unsigned nr = launcher_diff(drawn_[bi], next_, repaint, k_launcher_max_rows + 2);
    paint(bi, next_, repaint, nr);
    drawn_[bi] = next_;
    canvas_.commit(surface_, bi, damage, nd);
    committed_ = next_;
    dirty_ = false;
    must_commit_ = false;
    if (launcher_debug()) {
        launcher_log(lx::trace::level::debug,
                     "launcher: draw matches={} selection={} first={} damage={}", next_.total,
                     model_.selection(), next_.first, nd);
    }
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::launcher::events {};
void lx::shell::launcher::init(launcher_globals g, app_catalog& catalog, launch_fn on_launch,
                               closed_fn on_closed, void* user) {
    g_ = g;
    catalog_ = &catalog;
    on_launch_ = on_launch;
    on_closed_ = on_closed;
    user_ = user;
    model_.bind(&catalog);
}
bool lx::shell::launcher::open(int, int, int, void*) { return false; }
void lx::shell::launcher::close() {}
void lx::shell::launcher::after_dispatch() {}
void lx::shell::launcher::draw(bool) {}

#endif

lx::shell::launcher::~launcher() { close(); }
