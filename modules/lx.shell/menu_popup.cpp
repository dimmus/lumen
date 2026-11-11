module;

// Popup menus and tooltips (`:menu_popup`). See the partition.

#include <stdint.h>

module lx.shell;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import :menu_popup;
import :view_ui;

namespace {

constexpr lx::trace::category log_menu{"shell.menu"};
constexpr std::uint32_t k_menu_bg = 0xFF24242Au;
constexpr std::uint32_t k_menu_selected = 0xFF3D6FB4u;
constexpr std::uint32_t k_menu_separator = 0xFF3A3A42u;
constexpr int k_menu_radius = 8;
constexpr int k_menu_indicator = 28;
constexpr int k_menu_arrow = 24;
constexpr std::uint32_t k_menu_text = 0xFFECECEEu;
constexpr std::uint32_t k_menu_text_selected = 0xFFFFFFFFu;
constexpr std::uint32_t k_menu_text_dim = 0xFF8C8C94u;
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

lx::shell::menu_popup* g_menu_describing = nullptr;
unsigned g_menu_describing_index = 0;
struct menu_describing_guard {
    menu_describing_guard(lx::shell::menu_popup* m, unsigned i)
        : previous_{g_menu_describing}, previous_index_{g_menu_describing_index} {
        g_menu_describing = m;
        g_menu_describing_index = i;
    }
    ~menu_describing_guard() {
        g_menu_describing = previous_;
        g_menu_describing_index = previous_index_;
    }
    menu_describing_guard(const menu_describing_guard&) = delete;
    menu_describing_guard& operator=(const menu_describing_guard&) = delete;

private:
    lx::shell::menu_popup* previous_;
    unsigned previous_index_;
};

lx::shell::tooltip_popup* g_tip_describing = nullptr;
struct tip_describing_guard {
    explicit tip_describing_guard(lx::shell::tooltip_popup* t) : previous_{g_tip_describing} {
        g_tip_describing = t;
    }
    ~tip_describing_guard() { g_tip_describing = previous_; }
    tip_describing_guard(const tip_describing_guard&) = delete;
    tip_describing_guard& operator=(const tip_describing_guard&) = delete;

private:
    lx::shell::tooltip_popup* previous_;
};

} // namespace

lx::ui::child lx::shell::menu_describe(lx::ui::build_context&) {
    lx::shell::menu_popup* m = g_menu_describing;
    return m ? m->build_ui(g_menu_describing_index) : nullptr;
}

lx::ui::child lx::shell::tooltip_describe(lx::ui::build_context&) {
    lx::shell::tooltip_popup* t = g_tip_describing;
    return t ? t->build_ui() : nullptr;
}

// ── Root node: the level's focused node (its keys) ───────────────────────────

namespace lx::shell {

struct menu_root_props {
    shell_box_props box{};
    menu_popup* self = nullptr;
    unsigned index = 0;
};

class menu_root_node final : public shell_box_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "menu_root"; }

    bool apply(const menu_root_props& p) {
        self_ = p.self;
        index_ = p.index;
        return shell_box_node::apply(p.box);
    }

    bool on_focus_key(lx::ui::key_event event, lx::ui::focus_manager& focus) override {
        (void)focus;
        if (self_)
            self_->on_root_key(index_, event);
        return true; // the menu owns every key while its grab holds
    }

private:
    menu_popup* self_ = nullptr;
    unsigned index_ = 0;
};

} // namespace lx::shell

// ── The pure layout (as always) ───────────────────────────────────────────────

namespace {

[[nodiscard]] int menu_text_width(const char* s) {
    lx::text::shaped_line line{};
    if (!lx::text::shape_line(s, k_menu_font, line))
        return 0;
    return line.width_px > 0 ? line.width_px : 0;
}

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[nodiscard]] lx::ui::child menu_text(const char* text, std::uint32_t argb,
                                      const lx::text::font_desc& font, lx::ui::element_key key,
                                      std::span<const lx::text::text_attr> attrs = {}) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::shell::shell_color(argb);
    t.wrap = lx::text::text_wrap::none;
    t.attrs = attrs;
    t.context = lx::ui::active_ui_resources().text;
    return lx::ui::text_block(t, key);
}

/// A line of text centred vertically in and clipped to `r` (row-relative), ellipsized at
/// its width — the canvas's `text` in a box: the indicator column, the shortcut (its
/// caller right-aligns it), a submenu's arrow, and the label between them. A bare
/// `menu_text` in the row's flow would stack after its neighbours instead.
[[nodiscard]] lx::ui::child menu_text_at(lx::rect2i r, const char* text, std::uint32_t argb,
                                         const lx::text::font_desc& font,
                                         std::span<const lx::text::text_attr> attrs,
                                         lx::ui::element_key text_key,
                                         lx::ui::element_key box_key) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::shell::shell_color(argb);
    t.wrap = lx::text::text_wrap::none;
    t.ellipsize = true;
    t.attrs = attrs;
    t.context = lx::ui::active_ui_resources().text;
    const lx::ui::child inside[1] = {lx::ui::text_block(t, text_key)};
    lx::shell::shell_box_props box{};
    box.control.layout = at(r).flex_row().items_center().overflow_hidden();
    return lx::shell::shell_box(box, inside, box_key);
}

/// A label's mnemonic as the text_block's attrs: the letter underlined.
[[nodiscard]] std::span<const lx::text::text_attr> mnemonic_attrs(std::string_view label, int mn,
                                                                  std::vector<lx::text::text_attr>& out) {
    out.clear();
    if (mn < 0 && label.empty())
        return {out};
    std::size_t end = mn < 0 ? 0 : static_cast<std::size_t>(mn) + 1;
    if (mn >= 0) {
        while (end < label.size() && (static_cast<unsigned char>(label[end]) & 0xC0u) == 0x80u)
            ++end;
        lx::text::text_attr a{};
        a.begin = static_cast<std::uint32_t>(mn);
        a.end = static_cast<std::uint32_t>(end);
        a.underline = lx::text::text_underline::single;
        out.push_back(a);
    }
    return {out};
}

/// The UTF-8 of one codepoint (a mnemonic key's `utf8`).
void codepoint_utf8(char32_t cp, char* out) {
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
    *out = '\0';
}

/// The engine's key event as the menu's keysym and mnemonic text (the menu's own key
/// handling takes it from here — the same keysyms the unrouted seat path spelled).
void menu_key_of(const lx::ui::key_event& e, unsigned& sym, char* utf8) {
    sym = 0;
    utf8[0] = '\0';
    if (!e.pressed)
        return;
    using lx::ui::key;
    switch (e.key) {
    case key::up: sym = keysym::up; return;
    case key::down: sym = keysym::down; return;
    case key::left: sym = keysym::left; return;
    case key::right: sym = keysym::right; return;
    case key::home: sym = keysym::home; return;
    case key::end: sym = keysym::end; return;
    case key::enter: sym = e.keycode == 96 ? keysym::kp_enter : keysym::ret; return;
    case key::space: sym = keysym::space; return;
    case key::escape: sym = keysym::escape; return;
    default:
        if (e.codepoint >= 0x20 && e.codepoint != 0x7f) {
            sym = e.codepoint < 0x80 ? e.codepoint : 0;
            codepoint_utf8(e.codepoint, utf8);
        }
        return;
    }
}

/// xdg_positioner anchor / gravity enum values.
constexpr std::uint32_t pos_anchor_bottom_left = 6, pos_anchor_bottom_right = 7,
                         pos_gravity_bottom_right = 8, pos_anchor_bottom = 2,
                         pos_gravity_bottom = 2;
/// constraint_adjustment bits: slide_x 1, slide_y 2, flip_x 4, flip_y 8.
constexpr std::uint32_t k_submenu_constraints = 4 | 2 | 1;
constexpr std::uint32_t k_panel_popup_constraints = 1 | 2 | 8;
constexpr std::uint32_t k_panel_popup_gap = 4;
constexpr std::uint32_t k_tip_constraints = k_panel_popup_constraints;

void position_panel_popup(lx::wayland::positioner_desc& pos, lx::rect2i anchor) {
    pos.anchor_rect = {anchor.x, anchor.y, std::max(1, anchor.width), std::max(1, anchor.height)};
    pos.anchor = pos_anchor_bottom;
    pos.gravity = pos_gravity_bottom;
    pos.offset = {0, k_panel_popup_gap};
    pos.constraint_adjustment = k_panel_popup_constraints;
}

} // namespace

// ── Level construction and the tree ──────────────────────────────────────────

void lx::shell::menu_popup::init(menu_globals g, menu_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    icons_user_ = user;
}

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


bool lx::shell::menu_popup::open(lx::ui::host::layer_role& layer, lx::rect2i anchor,
                                 std::vector<menu_entry> items, unsigned serial,
                                 activate_fn on_activate, closed_fn on_closed,
                                 submenu_fn on_submenu, void* user, const char* name) {
    if (is_open())
        close();
    ++generation_;
    if (!g_.ui || !g_.ui->started() || !layer.valid() || items.empty())
        return false;
    root_ = std::move(items);
    on_activate_ = on_activate;
    on_closed_ = on_closed;
    on_submenu_ = on_submenu;
    user_ = user;
    last_serial_ = serial;
    std::snprintf(name_, sizeof(name_), "%s", name ? name : "menu");
    if (!open_level(0, nullptr, &layer, anchor, &root_, serial, false)) {
        close();
        return false;
    }
    log_menu.info("menu {}: open ({} items, {}x{})", name_, root_.size(), level_[0].rows.width,
                  level_[0].rows.height);
    return true;
}

bool lx::shell::menu_popup::open_level(unsigned index, lx::ui::host::popup_role* parent_popup,
                                       lx::ui::host::layer_role* parent_layer, lx::rect2i anchor,
                                       std::vector<menu_entry>* items, unsigned serial,
                                       bool submenu) {
    if (index >= k_menu_max_levels)
        return false;
    level& l = level_[index];
    l.items = items;
    l.selected = -1;
    l.dirty = true;
    layout_menu(
        *items,
        [](const char* s, void*) { return menu_text_width(s); },
        nullptr, l.rows);
    lx::ui::host::surface_options so{};
    so.describe = &menu_describe;
    so.size = {l.rows.width, l.rows.height};
    // Rounded corners over whatever is beneath.
    so.opaque = false;
    so.frame_records = false;
    if (auto made = l.surface.create(*g_.ui, so); !made) {
        log_menu.error(made.get_error());
        return false;
    }
    lx::wayland::positioner_desc pos{};
    pos.size = {l.rows.width, l.rows.height};
    if (submenu) {
        pos.anchor_rect = {anchor.x, anchor.y, anchor.width > 0 ? anchor.width : 1,
                           anchor.height > 0 ? anchor.height : 1};
        pos.anchor = pos_anchor_bottom_right;
        pos.gravity = pos_gravity_bottom_right;
        pos.offset = {0, -k_menu_pad};
        pos.constraint_adjustment = k_submenu_constraints;
    } else {
        position_panel_popup(pos, anchor);
    }
    if (parent_popup) {
        if (auto made = l.role.create(l.surface, *parent_popup, pos); !made) {
            log_menu.error(made.get_error());
            l.surface.destroy();
            return false;
        }
    } else {
        if (auto made = l.role.create(l.surface, *parent_layer, pos); !made) {
            log_menu.error(made.get_error());
            l.surface.destroy();
            return false;
        }
    }
    // The grab takes the keyboard (its focus enter follows the configure).
    l.role.grab(serial);
    l.role.set_done_handler(
        [](void* user) {
            const root_ref& r = *static_cast<root_ref*>(user);
            if (r.index == 0)
                r.self->close();
            else
                r.self->close_from(r.index);
        },
        &root_refs_[index]);
    root_refs_[index] = {this, index};
    l.surface.set_keyboard_focus_handler(
        [](bool entered, void* user) {
            const root_ref& r = *static_cast<root_ref*>(user);
            r.self->on_level_key_focus(r.index, entered);
        },
        &root_refs_[index]);
    if (auto shown = l.surface.show(); !shown) {
        log_menu.error(shown.get_error());
        l.role.destroy();
        l.surface.destroy();
        return false;
    }
    levels_ = index + 1;
    return true;
}

void lx::shell::menu_popup::close_from(unsigned index) {
    // Top-down: an xdg_popup that is not the topmost must not be destroyed first.
    while (levels_ > index) {
        level& l = level_[levels_ - 1];
        l.role.destroy();
        l.surface.destroy();
        l.items = nullptr;
        l.selected = -1;
        l.dirty = false;
        --levels_;
    }
    if (index > 0 && levels_ > 0)
        level_[levels_ - 1].dirty = true;
}

void lx::shell::menu_popup::close() {
    if (!is_open())
        return;
    close_from(0);
    log_menu.info("menu {}: closed", name_);
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
        [](const char* s, void*) { return menu_text_width(s); },
        nullptr, l.rows);
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
        if (open_level(index + 1, &level_[index].role, nullptr, r, children,
                       serial ? serial : last_serial_, true))
            select(index + 1, step(level_[index + 1], -1, 1));
        return;
    }
    const int id = e.id;
    log_menu.info("menu {}: activate id={} \"{}\"", name_, id, menu_label(e.label));
    const unsigned gen = generation_;
    if (on_activate_)
        on_activate_(id, user_);
    if (gen == generation_)
        close();
}

void lx::shell::menu_popup::on_row_hover(const row_ref& ref, bool hovered) {
    const unsigned li = ref.index;
    level& l = level_[li];
    if (hovered) {
        if (ref.row >= static_cast<int>(l.rows.count))
            return;
        // Hovering selects; a submenu's row opens it, any other row closes the deeper
        // levels (as the canvas's pointer motion did).
        const bool selectable_row = selectable((*l.items)[static_cast<unsigned>(ref.row)]);
        select(li, selectable_row ? ref.row : -1);
        if (selectable_row && (*l.items)[static_cast<unsigned>(ref.row)].submenu)
            activate(li, ref.row, last_serial_);
        else
            close_from(li + 1);
        return;
    }
    // The pointer left the row: the selection goes with it — unless a submenu hangs from
    // this level (the selection stays where the submenu hangs from).
    if (static_cast<unsigned>(li) + 1 >= levels_)
        select(li, -1);
}

void lx::shell::menu_popup::on_row_event(const row_ref& ref, const lx::ui::pointer_event& e) {
    const unsigned li = ref.index;
    level& l = level_[li];
    if (e.kind == lx::ui::pointer_kind::press) {
        last_serial_ = g_.ui ? g_.ui->last_press_serial() : last_serial_;
        return;
    }
    if (e.kind != lx::ui::pointer_kind::release)
        return;
    // A release on a row activates it (the selection is the row: hover selected it).
    const int row = ref.row < static_cast<int>(l.rows.count) ? ref.row : -1;
    if (l.selected >= 0 && l.selected == row &&
        !(*l.items)[static_cast<unsigned>(l.selected)].submenu)
        activate(li, l.selected, g_.ui ? g_.ui->last_press_serial() : last_serial_);
}

void lx::shell::menu_popup::on_root_key(unsigned index, const lx::ui::key_event& e) {
    unsigned sym = 0;
    char utf8[16]{};
    menu_key_of(e, sym, utf8);
    if (!sym && !utf8[0])
        return;
    key(index, sym, utf8, g_.ui ? g_.ui->last_press_serial() : last_serial_);
}

void lx::shell::menu_popup::on_level_key_focus(unsigned index, bool entered) {
    if (!entered)
        return;
    // The tree's keys are the focused node's, and nothing is focused until something is:
    // the level's root takes the grab's keyboard (programmatic — no ring).
    level& l = level_[index];
    if (!l.surface.valid())
        return;
    if (lx::ui::ui_node* tree = l.surface.root().tree())
        (void)l.surface.root().focus().focus(*tree, lx::ui::focus_cause::programmatic);
}

void lx::shell::menu_popup::key(unsigned index, unsigned sym, const char* utf8, unsigned serial) {
    if (!is_open())
        return;
    const unsigned top = levels_ - 1;
    level& l = level_[top];
    (void)index;
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

lx::gfx::atlas_tile_id lx::shell::menu_popup::icon_tile(unsigned level_index, const char* name,
                                                        int px) {
    icon_slot* free_slot = nullptr;
    for (icon_slot& s : icon_slots_[level_index]) {
        if (s.px == px && std::strcmp(s.name, name) == 0) {
            lx::gfx::atlas_tile place{};
            if (s.tile.valid() && level_[level_index].surface.atlas().lookup(s.tile, place))
                return s.tile;
            break;
        }
        if (!free_slot && s.px == 0)
            free_slot = &s;
    }
    if (!icons_)
        return {};
    const std::uint32_t* pixels = icons_(name, px, icons_user_);
    if (!pixels || px <= 0 || static_cast<unsigned>(px) > 64)
        return {};
    std::uint8_t rgba[64 * 64 * 4];
    for (int i = 0; i < px * px; ++i) {
        const std::uint32_t v = pixels[i];
        rgba[i * 4 + 0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(v & 0xFF);
        rgba[i * 4 + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    }
    auto made = level_[level_index].surface.atlas().insert(
        lx::gfx::atlas_format::rgba8, static_cast<unsigned>(px), static_cast<unsigned>(px),
        std::span<const std::uint8_t>{rgba, sizeof(rgba)}, static_cast<unsigned>(px * 4));
    if (!made)
        return {};
    icon_slot& s = free_slot ? *free_slot : icon_slots_[level_index][0];
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.px = px;
    s.tile = made.value();
    return s.tile;
}

lx::ui::child lx::shell::menu_popup::build_ui(unsigned index) {
    level& l = level_[index];
    if (!l.items)
        return nullptr;
    const float scale = l.surface.valid() ? l.surface.scale() : 1.f;
    const int icon_px = static_cast<int>(std::lround(16 * scale));
    lx::ui::child rows[k_menu_max_rows];
    unsigned n = 0;
    for (unsigned i = 0; i < l.rows.count; ++i) {
        const menu_entry& e = (*l.items)[i];
        const lx::rect2i& r = l.rows.row[i];
        row_refs_[index][i] = {this, index, static_cast<int>(i)};
        const bool sel = static_cast<int>(i) == l.selected && selectable(e);
        const std::uint32_t color = e.header || !e.enabled
                                        ? k_menu_text_dim
                                        : (sel ? k_menu_text_selected : k_menu_text);
        lx::ui::child parts[4];
        unsigned p = 0;

        if (e.separator) {
            // The hairline, centred vertically.
            lx::ui::quad_style q{};
            q.background = shell_color(k_menu_separator);
            shell_box_props sep{};
            sep.control.layout =
                at({6, r.height / 2, r.width - 12, 1});
            sep.rest = q;
            rows[n++] = shell_box(sep, {}, 0x9100'0000u + index * 128 + i);
            continue;
        }

        // The indicator column: a check, a radio, or the icon — centred in it (the canvas's
        // {6, 0, 20, h}; an icon at its own 16-px square, 4 in from the row).
        if (e.toggle_kind == menu_entry::toggle::check && e.toggled) {
            parts[p++] = menu_text_at({6, 0, k_menu_indicator - 8, r.height}, "\xE2\x9C\x93",
                                      color, k_menu_font, mnemonic_attrs("", -1, mnemonic_scratch_),
                                      0x9101'0000u + index * 128 + i, 0x9101'8000u + index * 128 + i);
        } else if (e.toggle_kind == menu_entry::toggle::radio) {
            parts[p++] = menu_text_at({6, 0, k_menu_indicator - 8, r.height},
                                      e.toggled ? "\xE2\x97\x8F" : "\xE2\x97\x8B", color,
                                      k_menu_font, mnemonic_attrs("", -1, mnemonic_scratch_),
                                      0x9102'0000u + index * 128 + i, 0x9102'8000u + index * 128 + i);
        } else if (!e.icon_name.empty()) {
            if (const lx::gfx::atlas_tile_id tile = icon_tile(index, e.icon_name.c_str(), icon_px);
                tile.valid()) {
                // The raster at the pixels it lands on (no clip: at a scale over 1 it is the
                // square's own device pixels).
                const lx::ui::child image[1] = {
                    lx::ui::image(l.surface.atlas(), tile, {icon_px, icon_px},
                                  0x9103'0000u + index * 128 + i)};
                shell_box_props icon_at{};
                icon_at.control.layout = at({4, (r.height - 16) / 2, 16, 16});
                parts[p++] = shell_box(icon_at, image, 0x9103'8000u + index * 128 + i);
            }
        }

        // The label between the indicator column and the shortcut, its mnemonic underlined;
        // the shortcut right-aligned before the arrow room, and the label's right edge
        // 16 past it — the canvas's own arithmetic.
        int right = static_cast<int>(r.width) - (e.submenu ? k_menu_arrow : 8);
        if (!e.shortcut.empty()) {
            const int sw = menu_text_width(e.shortcut.c_str());
            if (sw > 0 && right - sw > k_menu_indicator + 24) {
                parts[p++] = menu_text_at({right - sw, 0, sw, r.height}, e.shortcut.c_str(),
                                          k_menu_text_dim, k_menu_font,
                                          mnemonic_attrs("", -1, mnemonic_scratch_),
                                          0x9106'0000u + index * 128 + i,
                                          0x9106'8000u + index * 128 + i);
                right -= sw + 16;
            }
        }
        {
            int mn = -1;
            const std::string label = menu_label(e.label, &mn);
            parts[p++] = menu_text_at(
                {k_menu_indicator, 0, right - k_menu_indicator, r.height}, label.c_str(), color,
                e.header ? k_menu_header_font : k_menu_font, mnemonic_attrs(label, mn, mnemonic_scratch_),
                0x9104'0000u + index * 128 + i, 0x9104'8000u + index * 128 + i);
        }
        if (e.submenu)
            parts[p++] = menu_text_at({static_cast<int>(r.width) - k_menu_arrow + 6, 0,
                                       k_menu_arrow - 6, r.height},
                                      "\xE2\x96\xB8", color, k_menu_font,
                                      mnemonic_attrs("", -1, mnemonic_scratch_),
                                      0x9107'0000u + index * 128 + i,
                                      0x9107'8000u + index * 128 + i);

        // The row: the tree's interactive node; its selection the model's (hover paints
        // through the hover quad, the keyboard's selection through the rest quad).
        shell_box_props row{};
        row.control.layout = at(r).flex_row().items_center().overflow_hidden();
        row.control.interactive = true;
        lx::ui::quad_style sel_q{};
        sel_q.background = shell_color(k_menu_selected);
        for (float& cr : sel_q.corner_radii)
            cr = 5.f;
        if (sel)
            row.rest = sel_q;
        row.hover = sel_q; // hovering selects (the notify below selects with it)
        row.notify = [](void* user, bool hovered) {
            static_cast<row_ref*>(user)->self->on_row_hover(*static_cast<row_ref*>(user), hovered);
        };
        row.event = [](void* user, const lx::ui::pointer_event& e) {
            static_cast<row_ref*>(user)->self->on_row_event(*static_cast<row_ref*>(user), e);
        };
        row.user = &row_refs_[index][i];
        rows[n++] = shell_box(row, {parts, p}, 0x9108'0000u + index * 128 + i);
    }

    // The root: the menu's rounded background, the rows on it, and the grab's keyboard.
    lx::ui::quad_style bg{};
    bg.background = shell_color(k_menu_bg);
    for (float& cr : bg.corner_radii)
        cr = static_cast<float>(k_menu_radius);
    menu_root_props root{};
    root.box.control.layout = lx::ui::style{}.w(lx::layout::percent(100.f)).h(
        lx::layout::percent(100.f));
    root.box.control.focusable = true;
    root.box.rest = bg;
    root.self = this;
    root.index = index;
    root_refs_[index] = {this, index};
    return lx::ui::with_children(
        lx::ui::describe<menu_root_node, menu_root_props>(root, 0x9000u + index), {rows, n});
}

void lx::shell::menu_popup::describe_level(unsigned index) {
    level& l = level_[index];
    if (!l.surface.valid())
        return;
    l.surface.root().set_describer(&menu_describe);
    l.surface.root().add_repaint({0, 0, l.rows.width, l.rows.height});
}

void lx::shell::menu_popup::after_dispatch() {
    for (unsigned i = 0; i < levels_; ++i) {
        level& l = level_[i];
        if (!l.surface.valid())
            continue;
        if (l.dirty) {
            describe_level(i);
            l.dirty = false;
        }
        // The surface's frame loop: a frame when its frame callback allows one and the
        // ledger has work (our repaint, a configure).
        if (l.surface.frame_due()) {
            menu_describing_guard guard{this, i};
            l.surface.run_frame();
        }
        if (!is_open())
            return;
    }
}

lx::shell::menu_popup::~menu_popup() {
    on_closed_ = nullptr;
    close();
}

// ── Tooltip ──────────────────────────────────────────────────────────────────

bool lx::shell::tooltip_popup::open(lx::ui::host::layer_role& layer, lx::rect2i anchor,
                                    std::string_view title, std::string_view text) {
    close();
    if (!g_.ui || !g_.ui->started() || !layer.valid() || (title.empty() && text.empty()))
        return false;
    title_ = std::string{title};
    body_ = std::string{text};
    constexpr int k_tip_w = 320, k_tip_pad = 8;
    const int tw = title_.empty() ? 0 : menu_text_width(title_.c_str());
    int bh = 0;
    if (!body_.empty()) {
        lx::text::wrapped_text w{};
        if (lx::text::wrap_text(body_, {}, k_menu_font, k_tip_w - 2 * k_tip_pad, 4, w) &&
            w.line_count > 0)
            bh = static_cast<int>(w.line_count) * w.line_height_px;
        text_ = w;
    }
    const int body_w = body_.empty() ? 0 : static_cast<int>(text_.width_px);
    int w = (tw > body_w ? tw : body_w) + 2 * k_tip_pad + 2;
    w = w > k_tip_w ? k_tip_w : (w < 40 ? 40 : w);
    width_ = w;
    height_ = k_tip_pad * 2 + (title_.empty() ? 0 : 18) + (bh > 0 ? bh + (title_.empty() ? 0 : 2) : 0);
    lx::ui::host::surface_options so{};
    so.describe = &tooltip_describe;
    so.size = {width_, height_};
    so.opaque = false; // rounded corners
    so.frame_records = false;
    if (auto made = surface_.create(*g_.ui, so); !made) {
        log_menu.error(made.get_error());
        return false;
    }
    lx::wayland::positioner_desc pos{};
    pos.size = {width_, height_};
    position_panel_popup(pos, anchor);
    if (auto made = role_.create(surface_, layer, pos); !made) {
        log_menu.error(made.get_error());
        surface_.destroy();
        return false;
    }
    // No grab: the pointer stays with what is under it; the tooltip takes no input.
    surface_.set_input_region({});
    if (auto shown = surface_.show(); !shown) {
        log_menu.error(shown.get_error());
        role_.destroy();
        surface_.destroy();
        return false;
    }
    drawn_ = false;
    return true;
}

lx::ui::child lx::shell::tooltip_popup::build_ui() {
    lx::ui::child kids[2];
    unsigned n = 0;
    int y = 8;
    if (!title_.empty()) {
        const lx::ui::child title[1] = {menu_text(title_.c_str(), k_menu_text, k_menu_header_font,
                                                  0x9200u,
                                                  mnemonic_attrs("", -1, mnemonic_scratch_))};
        shell_box_props tb{};
        tb.control.layout = at({8, y, width_ - 16, 18}).overflow_hidden();
        kids[n++] = shell_box(tb, title, 0x9201u);
        y += 20;
    }
    if (!body_.empty()) {
        lx::ui::text_block_props t{};
        t.text = body_.c_str();
        t.font = k_menu_font;
        t.color = shell_color(k_menu_text);
        t.wrap = lx::text::text_wrap::word;
        t.max_lines = 4;
        t.context = lx::ui::active_ui_resources().text;
        lx::ui::child body_child = lx::ui::text_block(t, 0x9202u);
        shell_box_props bb{};
        bb.control.layout = at({8, y, width_ - 16, height_ - y - 6}).overflow_hidden();
        kids[n++] = shell_box(bb, {&body_child, 1}, 0x9203u);
    }
    lx::ui::quad_style bg{};
    bg.background = shell_color(k_menu_bg);
    for (float& cr : bg.corner_radii)
        cr = 6.f;
    shell_box_props root{};
    root.control.layout = lx::ui::style{}.w(lx::layout::percent(100.f)).h(
        lx::layout::percent(100.f));
    root.rest = bg;
    return shell_box(root, {kids, n}, 0x9204u);
}

void lx::shell::tooltip_popup::request_draw() {
    if (!surface_.valid())
        return;
    surface_.root().set_describer(&tooltip_describe);
    surface_.root().add_repaint({0, 0, width_, height_});
}

void lx::shell::tooltip_popup::after_dispatch() {
    if (!surface_.valid() || drawn_)
        return;
    request_draw();
    if (surface_.frame_due()) {
        tip_describing_guard guard{this};
        surface_.run_frame();
        drawn_ = surface_.stats().frames > 0;
    }
}

void lx::shell::tooltip_popup::close() {
    if (!surface_.valid())
        return;
    role_.destroy();
    surface_.destroy();
    drawn_ = false;
}

lx::shell::tooltip_popup::~tooltip_popup() { close(); }

