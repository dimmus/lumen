module;

// Popup menus and tooltips (`:menu_popup`). See the partition.

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

module lx.shell;

import std;
import lx.foundation;
import lx.text;
import lx.trace;
import :menu_popup;
import :shm_canvas;

namespace {

constexpr const char* k_menu_category = "shell.menu";
constexpr std::uint32_t k_menu_bg = 0xFF24242Au;
constexpr std::uint32_t k_menu_selected = 0xFF3D6FB4u;
constexpr std::uint32_t k_menu_separator = 0xFF3A3A42u;
constexpr int k_menu_radius = 8;
constexpr int k_menu_indicator = 28;
constexpr int k_menu_arrow = 24;
constexpr lx::text::raster_color k_menu_text{0xEC, 0xEC, 0xEE, 0xFF};
constexpr lx::text::raster_color k_menu_text_selected{0xFF, 0xFF, 0xFF, 0xFF};
constexpr lx::text::raster_color k_menu_text_dim{0x8C, 0x8C, 0x94, 0xFF};
constexpr lx::text::font_desc k_menu_font{"Sans", 10.f, lx::text::weight::normal,
                                          lx::text::style::normal};
constexpr lx::text::font_desc k_menu_header_font{"Sans", 9.f, lx::text::weight::bold,
                                                 lx::text::style::normal};

namespace keysym {
constexpr unsigned up = 0xff52, down = 0xff54, left = 0xff51, right = 0xff53;
constexpr unsigned home = 0xff50, end = 0xff57;
constexpr unsigned ret = 0xff0d, kp_enter = 0xff8d, space = 0x20, escape = 0xff1b;
} // namespace keysym

[[nodiscard]] bool selectable(const lx::shell::menu_entry& e) {
    return e.enabled && !e.separator && !e.header;
}

[[nodiscard]] char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

} // namespace

std::string lx::shell::menu_label(std::string_view label, int* mnemonic) {
    std::string out;
    int mn = -1;
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_') {
                out += '_';
                ++i;
                continue;
            }
            if (mn < 0 && i + 1 < label.size())
                mn = static_cast<int>(out.size());
            continue;
        }
        out += label[i];
    }
    if (mnemonic)
        *mnemonic = mn;
    return out;
}

void lx::shell::layout_menu(const std::vector<menu_entry>& items,
                            int (*text_width)(const char*, void*), void* user, menu_rows& out) {
    out.count = 0;
    int widest = 0;
    int y = k_menu_pad;
    for (const menu_entry& e : items) {
        if (out.count >= k_menu_max_rows)
            break;
        const int h = e.separator ? k_menu_separator_h : k_menu_row_h;
        out.row[out.count++] = {k_menu_pad, y, 0, h};
        y += h;
        if (e.separator || !text_width)
            continue;
        const std::string label = menu_label(e.label);
        int w = text_width(label.c_str(), user);
        if (!e.shortcut.empty())
            w += 24 + text_width(e.shortcut.c_str(), user);
        widest = w > widest ? w : widest;
    }
    int width = 2 * k_menu_pad + k_menu_indicator + widest + k_menu_arrow;
    width = width < k_menu_min_w ? k_menu_min_w : (width > k_menu_max_w ? k_menu_max_w : width);
    out.width = width;
    out.height = y + k_menu_pad;
    for (unsigned i = 0; i < out.count; ++i)
        out.row[i].width = width - 2 * k_menu_pad;
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::menu_popup::events {
    static level& lvl(void* data) { return *static_cast<level*>(data); }
    static void configure(void* data, xdg_surface* xdg, uint32_t serial) {
        xdg_surface_ack_configure(xdg, serial);
        auto& l = lvl(data);
        l.configured = true;
        l.dirty = true;
    }
    static void popup_configure(void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {}
    static void popup_done(void* data, xdg_popup*) {
        auto& l = lvl(data);
        menu_popup& m = *l.owner;
        const unsigned index = static_cast<unsigned>(&l - m.level_);
        if (index == 0)
            m.close();
        else
            m.close_from(index);
    }
    static void repositioned(void*, xdg_popup*, uint32_t) {}
};

struct lx::shell::tooltip_popup::events {
    static void configure(void* data, xdg_surface* xdg, uint32_t serial) {
        xdg_surface_ack_configure(xdg, serial);
        static_cast<tooltip_popup*>(data)->configured_ = true;
    }
    static void popup_done(void* data, xdg_popup*) { static_cast<tooltip_popup*>(data)->close(); }
};

namespace {

const xdg_surface_listener k_menu_xdg_listener = {
    .configure = lx::shell::menu_popup::events::configure,
};
const xdg_popup_listener k_menu_popup_listener = {
    .configure = lx::shell::menu_popup::events::popup_configure,
    .popup_done = lx::shell::menu_popup::events::popup_done,
    .repositioned = lx::shell::menu_popup::events::repositioned,
};
const xdg_surface_listener k_tip_xdg_listener = {
    .configure = lx::shell::tooltip_popup::events::configure,
};
const xdg_popup_listener k_tip_popup_listener = {
    .configure = lx::shell::menu_popup::events::popup_configure,
    .popup_done = lx::shell::tooltip_popup::events::popup_done,
    .repositioned = lx::shell::menu_popup::events::repositioned,
};

} // namespace

void lx::shell::menu_popup::init(menu_globals g, menu_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    icons_user_ = user;
    for (auto& l : level_) {
        l.owner = this;
        l.canvas.set_alpha(true);
    }
}

int lx::shell::menu_popup::level_of(void* surface) const {
    for (unsigned i = 0; i < levels_; ++i)
        if (level_[i].surface == surface)
            return static_cast<int>(i);
    return -1;
}

bool lx::shell::menu_popup::owns(void* surface) const { return surface && level_of(surface) >= 0; }

bool lx::shell::menu_popup::open(void* layer, lx::rect2i anchor, std::vector<menu_entry> items,
                                 unsigned serial, activate_fn on_activate, closed_fn on_closed,
                                 submenu_fn on_submenu, void* user, const char* name) {
    if (is_open())
        close();
    ++generation_;
    if (!g_.compositor || !g_.wm_base || !g_.seat || !g_.shm || !layer || items.empty())
        return false;
    root_ = std::move(items);
    on_activate_ = on_activate;
    on_closed_ = on_closed;
    on_submenu_ = on_submenu;
    user_ = user;
    last_serial_ = serial;
    std::snprintf(name_, sizeof(name_), "%s", name ? name : "menu");
    if (!open_level(0, nullptr, layer, anchor, &root_, serial, false)) {
        close();
        return false;
    }
    lx::trace::logger::global().logf(lx::trace::level::info, k_menu_category,
                                     "menu {}: open ({} items, {}x{})", name_, root_.size(),
                                     level_[0].rows.width, level_[0].rows.height);
    return true;
}

bool lx::shell::menu_popup::open_level(unsigned index, void* parent_xdg, void* parent_layer,
                                       lx::rect2i anchor, std::vector<menu_entry>* items,
                                       unsigned serial, bool submenu) {
    if (index >= k_menu_max_levels)
        return false;
    level& l = level_[index];
    l.items = items;
    l.selected = -1;
    l.configured = false;
    l.dirty = true;
    layout_menu(
        *items,
        [](const char* s, void* user) {
            const int w = static_cast<level*>(user)->canvas.text_width(s, k_menu_font);
            return w > 0 ? w : 0;
        },
        &l, l.rows);
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return false;
    l.surface = surface;
    auto* xdg = xdg_wm_base_get_xdg_surface(static_cast<xdg_wm_base*>(g_.wm_base), surface);
    l.xdg = xdg;
    xdg_surface_add_listener(xdg, &k_menu_xdg_listener, &l);
    auto* pos = xdg_wm_base_create_positioner(static_cast<xdg_wm_base*>(g_.wm_base));
    xdg_positioner_set_size(pos, l.rows.width, l.rows.height);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, anchor.width > 0 ? anchor.width : 1,
                                   anchor.height > 0 ? anchor.height : 1);
    if (submenu) {
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_offset(pos, 0, -k_menu_pad);
        xdg_positioner_set_constraint_adjustment(
            pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
    } else {
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    }
    auto* popup = xdg_surface_get_popup(xdg, static_cast<xdg_surface*>(parent_xdg), pos);
    xdg_positioner_destroy(pos);
    l.popup = popup;
    xdg_popup_add_listener(popup, &k_menu_popup_listener, &l);
    if (!parent_xdg && parent_layer)
        zwlr_layer_surface_v1_get_popup(static_cast<zwlr_layer_surface_v1*>(parent_layer), popup);
    xdg_popup_grab(popup, static_cast<wl_seat*>(g_.seat), serial);
    l.canvas.attach_scale(g_.viewporter, g_.fractional, surface);
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_region_add(region, k_menu_radius, 0, l.rows.width - 2 * k_menu_radius, l.rows.height);
        wl_region_add(region, 0, k_menu_radius, l.rows.width, l.rows.height - 2 * k_menu_radius);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface);
    levels_ = index + 1;
    return true;
}

void lx::shell::menu_popup::close_from(unsigned index) {
    // Top-down: an xdg_popup that is not the topmost must not be destroyed first.
    while (levels_ > index) {
        level& l = level_[levels_ - 1];
        l.canvas.free();
        l.canvas.release_scale();
        (void)l.canvas.take_rescale();
        if (l.popup)
            xdg_popup_destroy(static_cast<xdg_popup*>(l.popup));
        if (l.xdg)
            xdg_surface_destroy(static_cast<xdg_surface*>(l.xdg));
        if (l.surface)
            wl_surface_destroy(static_cast<wl_surface*>(l.surface));
        l.popup = l.xdg = l.surface = nullptr;
        l.items = nullptr;
        l.configured = false;
        l.selected = -1;
        if (pointer_level_ == static_cast<int>(levels_ - 1))
            pointer_level_ = -1;
        --levels_;
    }
    if (index > 0 && levels_ > 0)
        level_[levels_ - 1].dirty = true;
}

void lx::shell::menu_popup::close() {
    if (!is_open())
        return;
    close_from(0);
    lx::trace::logger::global().logf(lx::trace::level::info, k_menu_category, "menu {}: closed",
                                     name_);
    root_.clear();
    const closed_fn fn = on_closed_;
    void* user = user_;
    on_closed_ = nullptr;
    on_activate_ = nullptr;
    on_submenu_ = nullptr;
    if (fn)
        fn(user);
}

void lx::shell::menu_popup::replace_items(std::vector<menu_entry> items) {
    if (!is_open())
        return;
    close_from(1);
    root_ = std::move(items);
    level& l = level_[0];
    l.items = &root_;
    const int w = l.rows.width, h = l.rows.height;
    layout_menu(
        root_,
        [](const char* s, void* user) {
            const int tw = static_cast<level*>(user)->canvas.text_width(s, k_menu_font);
            return tw > 0 ? tw : 0;
        },
        &l, l.rows);
    // The surface keeps its size (a popup cannot grow without a reposition): rows that do
    // not fit are dropped.
    l.rows.width = w;
    for (unsigned i = 0; i < l.rows.count; ++i)
        l.rows.row[i].width = w - 2 * k_menu_pad;
    while (l.rows.count > 0 && l.rows.row[l.rows.count - 1].y + l.rows.row[l.rows.count - 1].height >
                                   h - k_menu_pad)
        --l.rows.count;
    l.rows.height = h;
    if (l.selected >= static_cast<int>(l.rows.count))
        l.selected = -1;
    l.dirty = true;
}

int lx::shell::menu_popup::step(const level& l, int from, int dir) const {
    const int n = static_cast<int>(l.rows.count);
    if (n == 0 || !l.items)
        return -1;
    int i = from;
    for (int k = 0; k < n; ++k) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (selectable((*l.items)[static_cast<unsigned>(i)]))
            return i;
    }
    return -1;
}

void lx::shell::menu_popup::select(unsigned index, int row) {
    level& l = level_[index];
    if (l.selected == row)
        return;
    l.selected = row;
    l.dirty = true;
}

void lx::shell::menu_popup::activate(unsigned index, int row, unsigned serial) {
    level& l = level_[index];
    if (row < 0 || static_cast<unsigned>(row) >= l.rows.count || !l.items)
        return;
    const menu_entry& e = (*l.items)[static_cast<unsigned>(row)];
    if (!selectable(e))
        return;
    if (e.submenu) {
        close_from(index + 1);
        if (on_submenu_)
            on_submenu_(e.id, user_);
        if (e.children.empty())
            return;
        const lx::rect2i r = l.rows.row[row];
        // The children vector lives in the tree (`root_`), stable while the menu is open.
        auto* children = const_cast<std::vector<menu_entry>*>(&e.children);
        if (open_level(index + 1, l.xdg, nullptr, r, children, serial ? serial : last_serial_,
                       true))
            select(index + 1, step(level_[index + 1], -1, 1));
        return;
    }
    const int id = e.id;
    lx::trace::logger::global().logf(lx::trace::level::info, k_menu_category,
                                     "menu {}: activate id={} \"{}\"", name_, id,
                                     menu_label(e.label));
    const unsigned gen = generation_;
    if (on_activate_)
        on_activate_(id, user_);
    if (gen == generation_)
        close();
}

void lx::shell::menu_popup::pointer_enter(void* surface, double x, double y) {
    pointer_level_ = level_of(surface);
    pointer_motion(surface, x, y);
}

void lx::shell::menu_popup::pointer_leave(void* surface) {
    const int l = level_of(surface);
    if (l >= 0 && l == pointer_level_) {
        // The selection stays where a submenu hangs from; otherwise it goes with the pointer.
        if (static_cast<unsigned>(l) + 1 >= levels_)
            select(static_cast<unsigned>(l), -1);
        pointer_level_ = -1;
    }
}

void lx::shell::menu_popup::pointer_motion(void* surface, double x, double y) {
    const int li = level_of(surface);
    if (li < 0)
        return;
    level& l = level_[li];
    int row = -1;
    for (unsigned i = 0; i < l.rows.count; ++i) {
        const lx::rect2i& r = l.rows.row[i];
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
            row = static_cast<int>(i);
    }
    if (row >= 0 && l.items && !selectable((*l.items)[static_cast<unsigned>(row)]))
        row = -1;
    if (row == l.selected)
        return;
    select(static_cast<unsigned>(li), row);
    // Hovering a submenu row opens it; any other row closes the deeper levels.
    if (row >= 0 && (*l.items)[static_cast<unsigned>(row)].submenu)
        activate(static_cast<unsigned>(li), row, last_serial_);
    else
        close_from(static_cast<unsigned>(li) + 1);
}

void lx::shell::menu_popup::pointer_button(void* surface, unsigned button, bool down,
                                           unsigned serial) {
    const int li = level_of(surface);
    if (li < 0)
        return;
    if (down) {
        last_serial_ = serial;
        return;
    }
    constexpr unsigned k_left = 0x110, k_right = 0x111;
    if (button != k_left && button != k_right)
        return;
    level& l = level_[li];
    if (l.selected >= 0 && !(*l.items)[static_cast<unsigned>(l.selected)].submenu)
        activate(static_cast<unsigned>(li), l.selected, serial);
}

void lx::shell::menu_popup::key(unsigned sym, const char* utf8, unsigned serial) {
    if (!is_open())
        return;
    const unsigned top = levels_ - 1;
    level& l = level_[top];
    switch (sym) {
    case keysym::down: select(top, step(l, l.selected, 1)); return;
    case keysym::up: select(top, step(l, l.selected, -1)); return;
    case keysym::home: select(top, step(l, -1, 1)); return;
    case keysym::end: select(top, step(l, -1, -1)); return;
    case keysym::escape:
        if (top == 0)
            close();
        else
            close_from(top);
        return;
    case keysym::left:
        if (top > 0)
            close_from(top);
        return;
    case keysym::right:
        if (l.selected >= 0 && (*l.items)[static_cast<unsigned>(l.selected)].submenu)
            activate(top, l.selected, serial);
        return;
    case keysym::ret:
    case keysym::kp_enter:
    case keysym::space:
        if (l.selected >= 0)
            activate(top, l.selected, serial);
        return;
    default: break;
    }
    // A mnemonic letter.
    if (!utf8 || !utf8[0] || utf8[1])
        return;
    const char want = ascii_lower(utf8[0]);
    for (unsigned i = 0; i < l.rows.count; ++i) {
        const menu_entry& e = (*l.items)[i];
        if (!selectable(e))
            continue;
        int mn = -1;
        const std::string label = menu_label(e.label, &mn);
        if (mn >= 0 && ascii_lower(label[static_cast<unsigned>(mn)]) == want) {
            select(top, static_cast<int>(i));
            activate(top, static_cast<int>(i), serial);
            return;
        }
    }
}

void lx::shell::menu_popup::draw(level& l) {
    shm_canvas& c = l.canvas;
    if (!c.allocated() || c.width() != l.rows.width || c.height() != l.rows.height || c.take_rescale()) {
        if (!c.allocate(g_.shm, l.rows.width, l.rows.height, "lumen-menu")) {
            lx::trace::logger::global().log(lx::trace::level::error, k_menu_category,
                                            "cannot allocate menu buffers");
            close();
            return;
        }
    }
    const int bi = c.acquire();
    if (bi < 0)
        return;
    c.clear(bi);
    c.fill_rounded(bi, {0, 0, l.rows.width, l.rows.height}, k_menu_bg, 0u, k_menu_radius);
    for (unsigned i = 0; i < l.rows.count; ++i) {
        const menu_entry& e = (*l.items)[i];
        const lx::rect2i& r = l.rows.row[i];
        if (e.separator) {
            c.fill(bi, {r.x + 6, r.y + r.height / 2, r.width - 12, 1}, k_menu_separator);
            continue;
        }
        const bool sel = static_cast<int>(i) == l.selected && selectable(e);
        if (sel)
            c.fill_rounded(bi, r, k_menu_selected, k_menu_bg, 5);
        const lx::text::raster_color color =
            e.header || !e.enabled ? k_menu_text_dim : (sel ? k_menu_text_selected : k_menu_text);
        // The indicator column: a check, a radio, or the icon.
        const lx::rect2i ind{r.x + 6, r.y, k_menu_indicator - 8, r.height};
        if (e.toggle_kind == menu_entry::toggle::check && e.toggled) {
            (void)c.text(bi, ind, "\xE2\x9C\x93", color, k_menu_font); // ✓
        } else if (e.toggle_kind == menu_entry::toggle::radio) {
            (void)c.text(bi, ind, e.toggled ? "\xE2\x97\x8F" : "\xE2\x97\x8B", color,
                         k_menu_font); // ● ○
        } else if (!e.icon_name.empty() && icons_) {
            const lx::rect2i box{r.x + 4, r.y + (r.height - 16) / 2, 16, 16};
            const int px = c.pixel_rect(box).width;
            if (const std::uint32_t* p = icons_(e.icon_name.c_str(), px, icons_user_))
                c.blit(bi, box, p, px);
        }
        int mn = -1;
        const std::string label = menu_label(e.label, &mn);
        const int lx0 = r.x + k_menu_indicator;
        int right = r.x + r.width - (e.submenu ? k_menu_arrow : 8);
        if (!e.shortcut.empty()) {
            const int sw = c.text_width(e.shortcut.c_str(), k_menu_font);
            if (sw > 0 && right - sw > lx0 + 24) {
                (void)c.text(bi, {right - sw, r.y, sw, r.height}, e.shortcut.c_str(),
                             k_menu_text_dim, k_menu_font);
                right -= sw + 16;
            }
        }
        (void)c.text(bi, {lx0, r.y, right - lx0, r.height}, label.c_str(), color,
                     e.header ? k_menu_header_font : k_menu_font);
        if (mn >= 0 && !e.header) {
            // The mnemonic letter underlined.
            const int pre = mn > 0 ? c.text_width(label.substr(0, static_cast<unsigned>(mn)).c_str(), k_menu_font) : 0;
            std::size_t end = static_cast<std::size_t>(mn) + 1;
            while (end < label.size() && (static_cast<unsigned char>(label[end]) & 0xC0u) == 0x80u)
                ++end;
            const int lw = c.text_width(label.substr(static_cast<unsigned>(mn), end - static_cast<std::size_t>(mn)).c_str(), k_menu_font);
            if (pre >= 0 && lw > 0 && lx0 + pre + lw < right)
                c.fill(bi, {lx0 + pre, r.y + r.height / 2 + 7, lw, 1},
                       (static_cast<std::uint32_t>(color.a) << 24) |
                           (static_cast<std::uint32_t>(color.r) << 16) |
                           (static_cast<std::uint32_t>(color.g) << 8) | color.b);
        }
        if (e.submenu)
            (void)c.text(bi, {r.x + r.width - k_menu_arrow + 6, r.y, k_menu_arrow - 6, r.height},
                         "\xE2\x96\xB8", color, k_menu_font); // ▸
    }
    const lx::rect2i whole{0, 0, l.rows.width, l.rows.height};
    c.commit(l.surface, bi, &whole, 1);
    l.dirty = false;
}

void lx::shell::menu_popup::after_dispatch() {
    for (unsigned i = 0; i < levels_; ++i) {
        level& l = level_[i];
        if (!l.configured || l.canvas.frame_pending())
            continue;
        if (l.dirty)
            draw(l);
        if (!is_open())
            return;
    }
}

lx::shell::menu_popup::~menu_popup() {
    on_closed_ = nullptr;
    close();
}

// ── Tooltip ──────────────────────────────────────────────────────────────────

bool lx::shell::tooltip_popup::open(void* layer, lx::rect2i anchor, std::string_view title,
                                    std::string_view text) {
    close();
    if (!g_.compositor || !g_.wm_base || !g_.shm || !layer || (title.empty() && text.empty()))
        return false;
    canvas_.set_alpha(true);
    title_ = std::string{title};
    body_ = std::string{text};
    constexpr int k_tip_w = 320, k_tip_pad = 8;
    const int tw = title_.empty() ? 0 : canvas_.text_width(title_.c_str(), k_menu_header_font);
    int bh = 0;
    if (!body_.empty())
        bh = canvas_.wrap(body_, {}, k_menu_font, k_tip_w - 2 * k_tip_pad, 4, text_);
    const int body_w = body_.empty() ? 0 : static_cast<int>(std::ceil(text_.width_px / canvas_.scale_factor()));
    int w = (tw > body_w ? tw : body_w) + 2 * k_tip_pad + 2;
    w = w > k_tip_w ? k_tip_w : (w < 40 ? 40 : w);
    width_ = w;
    height_ = k_tip_pad * 2 + (title_.empty() ? 0 : 18) + (bh > 0 ? bh + (title_.empty() ? 0 : 2) : 0);
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return false;
    surface_ = surface;
    auto* xdg = xdg_wm_base_get_xdg_surface(static_cast<xdg_wm_base*>(g_.wm_base), surface);
    xdg_ = xdg;
    xdg_surface_add_listener(xdg, &k_tip_xdg_listener, this);
    auto* pos = xdg_wm_base_create_positioner(static_cast<xdg_wm_base*>(g_.wm_base));
    xdg_positioner_set_size(pos, width_, height_);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, anchor.width > 0 ? anchor.width : 1,
                                   anchor.height > 0 ? anchor.height : 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM);
    xdg_positioner_set_offset(pos, 0, 4);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    auto* popup = xdg_surface_get_popup(xdg, nullptr, pos);
    xdg_positioner_destroy(pos);
    popup_ = popup;
    xdg_popup_add_listener(popup, &k_tip_popup_listener, this);
    zwlr_layer_surface_v1_get_popup(static_cast<zwlr_layer_surface_v1*>(layer), popup);
    canvas_.attach_scale(g_.viewporter, g_.fractional, surface);
    // No input: the pointer stays with what is under it.
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_surface_set_input_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface);
    configured_ = false;
    drawn_ = false;
    return true;
}

void lx::shell::tooltip_popup::draw() {
    if (!canvas_.allocated() || canvas_.take_rescale()) {
        if (!body_.empty())
            (void)canvas_.wrap(body_, {}, k_menu_font, width_ - 16, 4, text_);
        if (!canvas_.allocate(g_.shm, width_, height_, "lumen-tooltip")) {
            close();
            return;
        }
    }
    const int bi = canvas_.acquire();
    if (bi < 0)
        return;
    canvas_.clear(bi);
    canvas_.fill_rounded(bi, {0, 0, width_, height_}, k_menu_bg, 0u, 6);
    int y = 8;
    if (!title_.empty()) {
        (void)canvas_.text(bi, {8, y, width_ - 16, 18}, title_.c_str(), k_menu_text,
                           k_menu_header_font);
        y += 20;
    }
    if (!body_.empty())
        canvas_.draw_wrapped(bi, {8, y, width_ - 16, height_ - y - 6}, body_, {}, text_,
                             k_menu_font, nullptr, nullptr);
    const lx::rect2i whole{0, 0, width_, height_};
    canvas_.commit(surface_, bi, &whole, 1);
    drawn_ = true;
}

void lx::shell::tooltip_popup::after_dispatch() {
    if (!surface_ || !configured_ || canvas_.frame_pending())
        return;
    if (!drawn_ || canvas_.take_rescale())
        draw();
}

void lx::shell::tooltip_popup::close() {
    if (!surface_)
        return;
    canvas_.free();
    canvas_.release_scale();
    (void)canvas_.take_rescale();
    if (popup_)
        xdg_popup_destroy(static_cast<xdg_popup*>(popup_));
    if (xdg_)
        xdg_surface_destroy(static_cast<xdg_surface*>(xdg_));
    wl_surface_destroy(static_cast<wl_surface*>(surface_));
    popup_ = xdg_ = surface_ = nullptr;
    configured_ = false;
}

lx::shell::tooltip_popup::~tooltip_popup() { close(); }

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::menu_popup::events {};
struct lx::shell::tooltip_popup::events {};
void lx::shell::menu_popup::init(menu_globals g, menu_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    icons_user_ = user;
}
bool lx::shell::menu_popup::open(void*, lx::rect2i, std::vector<menu_entry>, unsigned, activate_fn,
                                 closed_fn, submenu_fn, void*, const char*) {
    return false;
}
void lx::shell::menu_popup::close() {}
void lx::shell::menu_popup::close_from(unsigned) {}
bool lx::shell::menu_popup::owns(void*) const { return false; }
void lx::shell::menu_popup::replace_items(std::vector<menu_entry>) {}
void lx::shell::menu_popup::pointer_enter(void*, double, double) {}
void lx::shell::menu_popup::pointer_leave(void*) {}
void lx::shell::menu_popup::pointer_motion(void*, double, double) {}
void lx::shell::menu_popup::pointer_button(void*, unsigned, bool, unsigned) {}
void lx::shell::menu_popup::key(unsigned, const char*, unsigned) {}
void lx::shell::menu_popup::after_dispatch() {}
lx::shell::menu_popup::~menu_popup() = default;
bool lx::shell::tooltip_popup::open(void*, lx::rect2i, std::string_view, std::string_view) {
    return false;
}
void lx::shell::tooltip_popup::close() {}
void lx::shell::tooltip_popup::after_dispatch() {}
lx::shell::tooltip_popup::~tooltip_popup() = default;

#endif
