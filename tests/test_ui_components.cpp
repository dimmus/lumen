// ADR-010 §7 Phase 3, stages 3 and 4: focus over the retained tree, the reducer wired into
// the root, pointer state and motion, and §5.6's components — the first half, then overlays
// (menus, popovers, tooltips, modals, toasts), scroll areas, virtualised lists and the text
// input with its IME interface; and stage 5's emission scoped to a frame's damage.
//
// Sizes are checked against Zed's numbers at the default density and the 16 px rem, from
// zed-industries/zed a5c4ca8 crates/ui/src (and crates/workspace for the status bar and the
// toolbar); each check names its file.
#include "lumen_test.hpp"

#include <malloc.h>
#include <stdlib.h>

import std;
import lx.foundation;
import lx.a11y;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;

namespace {
/// Every allocation through the replaced operator new (G5).
std::atomic<unsigned long long> g_allocations{0};
} // namespace

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace lx::ui;
using lx::layout::px;

// ── Shared fixtures ──

/// The text and icon caches components draw with: the bundled fonts, one atlas.
struct resources {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    lx::gfx::atlas atlas{};
    lx::text::glyph_cache glyphs{atlas};
    icon_cache icons{atlas};

    resources() {
#ifdef LUMEN_TEST_FONT_DIR
        // Lumen Test Arabic and Emoji: the text input's right-to-left and emoji cases.
        (void)faces.add_font_dir(LUMEN_TEST_FONT_DIR);
#endif
        ui_resources r{};
        r.text = {{&faces, &shapes, &items}, &glyphs, nullptr};
        r.icons = &icons;
        set_ui_resources(r);
    }
    ~resources() { set_ui_resources({}); }
};

[[nodiscard]] resources& env() {
    static const std::unique_ptr<resources> r = std::make_unique<resources>();
    return *r;
}

/// A root over `describer` at `bounds`, ticked once.
struct harness {
    lx::runtime::memory_arena arena{512 * 1024};
    ui_root root{arena};
    std::unique_ptr<lx::scene::draw_list> list = std::make_unique<lx::scene::draw_list>();

    harness(describe_fn describer, lx::rect2i bounds) {
        (void)env();
        ui_damage().clear();
        root.set_describer(describer);
        root.set_bounds(bounds);
        LUMEN_CHECK(root.tick(*list));
    }
    bool tick() { return root.tick(*list); }
};

[[nodiscard]] ui_node* find(ui_node* n, lx::a11y::role role, const char* name) {
    if (!n)
        return nullptr;
    const lx::a11y::node a = n->accessibility_node();
    if (a.role_ == role && (!name || (a.name && std::strcmp(a.name, name) == 0)))
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find(n->child_at(i), role, name))
            return hit;
    return nullptr;
}

[[nodiscard]] control_node& ctl(ui_node* n) {
    LUMEN_CHECK(n && n->type() == control_node::static_type());
    return *static_cast<control_node*>(n);
}

[[nodiscard]] lx::point2i centre(const ui_node& n) {
    const lx::rect2i b = n.bounds();
    return {b.x + b.width / 2, b.y + b.height / 2};
}

[[nodiscard]] bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] bool inside(lx::rect2i inner, lx::rect2i outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

[[nodiscard]] bool overlaps(lx::rect2i a, lx::rect2i b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

[[nodiscard]] bool same_colour(lx::color a, lx::color b) {
    const auto close = [](float x, float y) { return std::fabs(x - y) < 1e-4f; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b) && close(a.a, b.a);
}

[[nodiscard]] lx::color token(tok t, float alpha = 1.f) {
    lx::color c = active_theme().color(t);
    c.a *= static_cast<float>(alpha_255(alpha)) / 255.f;
    return c;
}

void print_rect(const char* what, lx::rect2i r) {
    std::printf("    %-22s %4d,%4d %4d x %3d\n", what, r.x, r.y, r.width, r.height);
}

// ── Sizes ──

const char* const k_ctrl_s[1] = {"Ctrl-S"};
const char* const k_enter[1] = {"Enter"};

child describe_sizes(build_context&) {
    child tab_kids[3];
    const char* tab_names[3] = {"one.rs", "two.rs", "three.rs"};
    for (unsigned i = 0; i < 3; ++i)
        tab_kids[i] = tab(tab_names[i]).position(i, 3, 1);
    const child status_left[1] = {button("main").name("status-main")};
    const child vrow[2] = {label("a"), vertical_divider()};
    const child prog[1] = {progress_bar(50.f, 100.f).name("progress")};
    const child kids[] = {
        button("Save"),                                                        // 0
        button("Compact").size(button_size::compact),                          // 1
        button("Medium").size(button_size::medium),                            // 2
        button("Large").size(button_size::large),                              // 3
        icon_button(icon::x).name("wide"),                                     // 4
        icon_button(icon::x).shape(icon_button_shape::square).name("square"),  // 5
        checkbox(true).label("Check"),                                         // 6
        toggle_switch(true).label("Switch"),                                   // 7
        toggle_switch(false).label("Off"),                                     // 8
        divider(),                                                             // 9
        div(style{}.flex_row().items_center(), vrow),                          // 10
        div(style{}.w(px(200.f)), prog),                                       // 11
        avatar("AB").name("avatar"),                                           // 12
        key_binding(k_ctrl_s),                                                 // 13
        key_binding_hint(k_enter, tok::surface_background).prefix("Press"),    // 14
        tab_bar(tab_kids),                                                     // 15
        status_bar(status_left, {}),                                           // 16
        headline("Title"),                                                     // 17
        label("Label"),                                                        // 18
        toggle_button("Grid").selected(true),                                  // 19
    };
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(600.f)), kids);
}

child describe_panel_only(build_context&) {
    const child toolbar_left[1] = {label("Files")};
    const child body = label("body");
    const child kids[1] = {panel(body).toolbar_start(toolbar_left).name("dock")};
    const child sized[1] = {div(style{}.w(px(300.f)).h(px(160.f)), kids)};
    return div(style{}.flex_col().items_start(), sized);
}

} // namespace

LUMEN_TEST(component_sizes_follow_zed) {
    set_active_builtin_theme(appearance::dark);
    set_ui_density(density::standard);
    harness h{describe_sizes, {0, 0, 640, 900}};
    ui_node* root = h.root.tree();
    LUMEN_CHECK(root != nullptr);

    // button_like.rs `ButtonSize::rems`: 22 default, 18 compact, 28 medium, 32 large; padding
    // `Base04` (default, compact) or `Base08` (medium, large); `gap(Base04)`.
    ui_node* save = find(root, lx::a11y::role::button, "Save");
    ui_node* compact = find(root, lx::a11y::role::button, "Compact");
    ui_node* medium = find(root, lx::a11y::role::button, "Medium");
    ui_node* large = find(root, lx::a11y::role::button, "Large");
    LUMEN_CHECK(save && compact && medium && large);
    print_rect("button default", save->bounds());
    LUMEN_CHECK(save->bounds().height == 22);
    LUMEN_CHECK(compact->bounds().height == 18);
    LUMEN_CHECK(medium->bounds().height == 28);
    LUMEN_CHECK(large->bounds().height == 32);
    LUMEN_CHECK(save->child_at(0)->bounds().x - save->bounds().x == 4);
    LUMEN_CHECK(save->bounds().width == save->child_at(0)->bounds().width + 8);
    LUMEN_CHECK(large->child_at(0)->bounds().x - large->bounds().x == 8);

    // icon_button.rs: `Wide` is 16 px + Base04 each side by the button's 22; `Square` is
    // `IconSize::square` = 16 + 2 × Base02 = 20 both ways.
    ui_node* wide = find(root, lx::a11y::role::button, "wide");
    ui_node* square = find(root, lx::a11y::role::button, "square");
    print_rect("icon_button wide", wide->bounds());
    print_rect("icon_button square", square->bounds());
    LUMEN_CHECK(wide->bounds().width == 24 && wide->bounds().height == 22);
    LUMEN_CHECK(square->bounds().width == 20 && square->bounds().height == 20);

    // toggle.rs `Checkbox`: a 20 px container (`container_size`), the box `size_4` (16) with
    // `m_1` (4) centred in it, `gap(Base06)` to the label.
    ui_node* check = find(root, lx::a11y::role::checkbox, "Check");
    ui_node* holder = check->child_at(0);
    ui_node* box = holder->child_at(0);
    print_rect("checkbox row", check->bounds());
    print_rect("checkbox box", box->bounds());
    LUMEN_CHECK(holder->bounds().width == 20 && holder->bounds().height == 20);
    LUMEN_CHECK(box->bounds().width == 16 && box->bounds().height == 16);
    LUMEN_CHECK(box->bounds().x == holder->bounds().x + 2);
    LUMEN_CHECK(check->child_at(1)->bounds().x == holder->bounds().x + 20 + 6);

    // toggle.rs `Switch`: `p(1px)` + `border_2` around a `Base32` × `Base20` track with
    // `px(Base02)` and a 1 px border; the `Base12` thumb at the end when on.
    ui_node* sw_on = find(root, lx::a11y::role::switch_control, "Switch");
    ui_node* sw_off = find(root, lx::a11y::role::switch_control, "Off");
    ui_node* sw = sw_on->child_at(0);
    ui_node* track = sw->child_at(0);
    ui_node* thumb = track->child_at(0);
    print_rect("switch", sw->bounds());
    LUMEN_CHECK(sw->bounds().width == 38 && sw->bounds().height == 26);
    LUMEN_CHECK(track->bounds().width == 32 && track->bounds().height == 20);
    LUMEN_CHECK(thumb->bounds().width == 12 && thumb->bounds().height == 12);
    LUMEN_CHECK(thumb->bounds().x == track->bounds().x + 32 - 1 - 2 - 12);
    LUMEN_CHECK(thumb->bounds().y == track->bounds().y + 4);
    ui_node* off_thumb = sw_off->child_at(0)->child_at(0)->child_at(0);
    LUMEN_CHECK(off_thumb->bounds().x == sw_off->child_at(0)->child_at(0)->bounds().x + 3);

    // divider.rs: `h_px().w_full()`; vertical `w_px().h_4()`.
    ui_node* rule = find(root, lx::a11y::role::separator, nullptr);
    print_rect("divider", rule->bounds());
    LUMEN_CHECK(rule->bounds().height == 1 && rule->bounds().width == 640 - 16);
    ui_node* vrule = nullptr;
    {
        ui_node* row = root->child_at(10);
        vrule = row->child_at(1);
    }
    LUMEN_CHECK(vrule->bounds().width == 1 && vrule->bounds().height == 16);

    // progress_bar.rs: `h_2()` (8) with `p_0p5()` (2); the fill `w(relative(0.5))` of the
    // content box.
    ui_node* bar = find(root, lx::a11y::role::progress_bar, "progress");
    print_rect("progress", bar->bounds());
    LUMEN_CHECK(bar->bounds().height == 8 && bar->bounds().width == 200);
    LUMEN_CHECK(bar->child_at(0)->bounds().height == 4 && bar->child_at(0)->bounds().width == 98);

    // avatar.rs: `rems(1.)` = 16 px, round.
    ui_node* av = find(root, lx::a11y::role::image, "avatar");
    LUMEN_CHECK(av->bounds().width == 16 && av->bounds().height == 16);

    // keybinding.rs: a keystroke `py_0p5()` around a `Key` `h(TextSize::Default = 14)`.
    ui_node* keys = find(root, lx::a11y::role::key_shortcut, "Ctrl-S");
    print_rect("key_binding", keys->bounds());
    LUMEN_CHECK(keys->bounds().height == 18);
    LUMEN_CHECK(keys->child_at(0)->child_at(0)->bounds().height == 14);
    // keybinding_hint.rs: the keys at `Small` − 2 = 10 px, `py_0p5`, a 1 px cap border.
    ui_node* hint_keys = find(root, lx::a11y::role::key_shortcut, "Enter");
    LUMEN_CHECK(hint_keys->bounds().height == 14);
    LUMEN_CHECK(hint_keys->parent()->bounds().height == 16);

    // tab.rs / tab_bar.rs: `Tab::container_height` = Base32, content Base32 − 1.
    ui_node* bar_tabs = find(root, lx::a11y::role::tab_list, nullptr);
    ui_node* two = find(root, lx::a11y::role::tab, "two.rs");
    print_rect("tab", two->bounds());
    LUMEN_CHECK(bar_tabs && bar_tabs->parent()->parent()->bounds().height == 32);
    LUMEN_CHECK(two->bounds().height == 32 && two->child_at(0)->bounds().height == 31);
    // The selected middle tab: `border_l_1().border_r_1().pb_px()`, content 4 px in.
    LUMEN_CHECK(two->child_at(0)->bounds().x == two->bounds().x + 1);
    LUMEN_CHECK(two->child_at(0)->child_at(0)->bounds().x == two->bounds().x + 1 + 4);
    LUMEN_CHECK(two->child_at(0)->child_at(0)->bounds().width == 12);

    // status_bar.rs: `p(Base04)` around a default button: 22 + 8.
    ui_node* status = find(root, lx::a11y::role::status_bar, nullptr);
    print_rect("status_bar", status->bounds());
    LUMEN_CHECK(status->bounds().height == 30 && status->bounds().width == 640 - 16);

    // typography.rs: a medium headline is 1.125 rem (18 px) on a 1.6 rem line (25.6 px); a
    // label 14 px on φ (22.65 px).
    ui_node* title = nullptr;
    ui_node* text = nullptr;
    for (unsigned i = 0; i < root->child_count(); ++i) {
        ui_node* c = root->child_at(i);
        if (c->type() == text_block_node::static_type()) {
            if (!title)
                title = c;
            else
                text = c;
        }
    }
    print_rect("headline", title->bounds());
    print_rect("label", text->bounds());
    LUMEN_CHECK(title->bounds().height >= 25 && title->bounds().height <= 26);
    LUMEN_CHECK(text->bounds().height >= 22 && text->bounds().height <= 23);

    // toggle_button.rs: a selected entry is `Tinted(Accent)`: `info.background`, an
    // `info.border` box, the label `text.accent`.
    ui_node* grid = find(root, lx::a11y::role::toggle_button, "Grid");
    LUMEN_CHECK(grid->bounds().height == 22);
    LUMEN_CHECK(same_colour(ctl(grid).background(), token(tok::info_background)));
}

LUMEN_TEST(panel_toolbar_follows_zed) {
    harness h{describe_panel_only, {0, 0, 400, 300}};
    ui_node* dock = find(h.root.tree(), lx::a11y::role::panel, "dock");
    ui_node* toolbar = find(dock, lx::a11y::role::toolbar, nullptr);
    print_rect("panel", dock->bounds());
    print_rect("toolbar", toolbar->bounds());
    // workspace/toolbar.rs: `py(Base06)` around a `min_h_8` (32) row, a 1 px rule below.
    LUMEN_CHECK(dock->bounds().width == 300 && dock->bounds().height == 160);
    LUMEN_CHECK(toolbar->bounds().height == 6 + 32 + 6 + 1);
    LUMEN_CHECK(same_colour(ctl(toolbar).background(), token(tok::toolbar_background)));
    LUMEN_CHECK(same_colour(ctl(dock).background(), token(tok::panel_background)));
    // The body fills the rest.
    LUMEN_CHECK(dock->child_at(1)->bounds().height == 160 - 45);
}

// ── Pointer states: background tokens, damage limited to the control ──

namespace {

int g_presses = 0;

void on_press_count() { ++g_presses; }

child describe_states(build_context&) {
    const child kids[] = {
        button("Filled").style(button_style::filled),
        button("Outlined").style(button_style::outlined),
        button("Ghost").on_press(on_press_count),
        button("Off").disabled(true).on_press(on_press_count),
        checkbox(false).label("Box"),
        toggle_switch(false).label("Sw"),
    };
    return div(style{}.flex_row().items_center().gap(sp::b08).p(sp::b08), kids);
}

} // namespace

LUMEN_TEST(pointer_states_change_background_tokens_and_damage_only_that_control) {
    harness h{describe_states, {0, 0, 640, 60}};
    ui_node* root = h.root.tree();
    control_node& filled = ctl(find(root, lx::a11y::role::button, "Filled"));
    control_node& outlined = ctl(find(root, lx::a11y::role::button, "Outlined"));
    control_node& ghost = ctl(find(root, lx::a11y::role::button, "Ghost"));
    control_node& off = ctl(find(root, lx::a11y::role::button, "Off"));
    // button_like.rs `ButtonStyle::enabled`: element.background, element.background with the
    // border.variant border, ghost (transparent).
    LUMEN_CHECK(same_colour(filled.background(), token(tok::element_background)));
    LUMEN_CHECK(same_colour(outlined.background(), token(tok::element_background)));
    LUMEN_CHECK(same_colour(ghost.background(), token(tok::ghost_element_background)));
    LUMEN_CHECK(!h.tick());

    // Hover: `ghost_element.hover`; the damage is the button and nothing else.
    ui_damage().clear();
    LUMEN_CHECK(h.root.pointer_move(centre(ghost)));
    LUMEN_CHECK(ghost.has_flag(node_flag::hovered));
    LUMEN_CHECK(same_colour(ghost.background(), token(tok::ghost_element_hover)));
    LUMEN_CHECK(ui_damage().count() == 1 && same_rect(ui_damage().union_bounds(), ghost.bounds()));
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(same_rect(h.root.frame_damage().union_bounds(), ghost.bounds()));
    LUMEN_CHECK(!h.tick());

    // Press: `ghost_element.active`; release over it: one press.
    g_presses = 0;
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(same_colour(ghost.background(), token(tok::ghost_element_active)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_presses == 1 && ghost.activations() == 1);
    LUMEN_CHECK(same_colour(ghost.background(), token(tok::ghost_element_hover)));
    // The press focused it without a ring (pointer focus).
    LUMEN_CHECK(h.root.focus().focused() == &ghost && !h.root.focus().focus_visible());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(same_rect(h.root.frame_damage().union_bounds(), ghost.bounds()));

    // Filled hover: `element.background` faded to half (`fade_out(0.5)`); outlined:
    // `ghost_element.hover` with a `border` border.
    ui_damage().clear();
    LUMEN_CHECK(h.root.pointer_move(centre(filled)));
    LUMEN_CHECK(same_colour(filled.background(), token(tok::element_background, 0.5f)));
    LUMEN_CHECK(same_colour(ghost.background(), token(tok::ghost_element_background)));
    LUMEN_CHECK(ui_damage().count() == 2);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.root.pointer_move(centre(outlined)));
    LUMEN_CHECK(same_colour(outlined.background(), token(tok::ghost_element_hover)));
    LUMEN_CHECK(h.tick());

    // Disabled: no hover, no press, no activation; the hover leaves the outlined button.
    ui_damage().clear();
    (void)h.root.pointer_move(centre(off));
    LUMEN_CHECK(!off.has_flag(node_flag::hovered) && h.root.pointer().hovered() == nullptr);
    LUMEN_CHECK(same_colour(off.background(), token(tok::ghost_element_background)));
    LUMEN_CHECK(ui_damage().count() == 1 && same_rect(ui_damage().union_bounds(), outlined.bounds()));
    g_presses = 0;
    (void)h.root.pointer_button(mouse_button::left, true);
    (void)h.root.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(g_presses == 0 && off.activations() == 0);
    LUMEN_CHECK(h.tick());

    // A checkbox: the row is what is hovered, the box paints from it (`group_hover`): its
    // border at 70 %; the damage is the row.
    control_node& row = ctl(find(root, lx::a11y::role::checkbox, "Box"));
    control_node& box = ctl(row.child_at(0)->child_at(0));
    ui_damage().clear();
    LUMEN_CHECK(h.root.pointer_move(centre(box)));
    LUMEN_CHECK(row.has_flag(node_flag::hovered) && !box.has_flag(node_flag::hovered));
    LUMEN_CHECK(box.paint_state() == 1 && &box.state_source() == &row);
    LUMEN_CHECK(ui_damage().count() == 1 && same_rect(ui_damage().union_bounds(), row.bounds()));
    // The switch track: `element.disabled` with `text` at 5 % on top.
    control_node& sw_row = ctl(find(root, lx::a11y::role::switch_control, "Sw"));
    control_node& track = ctl(sw_row.child_at(0)->child_at(0));
    const lx::color rest = track.background();
    LUMEN_CHECK(same_colour(rest, token(tok::element_disabled)));
    LUMEN_CHECK(h.root.pointer_move(centre(track)));
    const lx::color hover = track.background();
    LUMEN_CHECK(!same_colour(rest, hover) && hover.a == 1.f && hover.r > rest.r);
    h.root.pointer_leave();
    LUMEN_CHECK(h.root.pointer().hovered() == nullptr && same_colour(track.background(), rest));
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(!h.tick());
}

// ── Focus ──

namespace {

int g_activated = -1;
void on_value(int v) { g_activated = v; }
bool g_with_trap = true;
bool g_drop_b3 = false;

child describe_focus(build_context&) {
    child trap_kids[2] = {button("t0"), button("t1")};
    child kids[7]{};
    unsigned n = 0;
    kids[n++] = button("b0").on_press(on_value).key(10);
    kids[n++] = button("b1").tab_index(-1).key(11);
    kids[n++] = button("b2").disabled(true).key(12);
    if (!g_drop_b3)
        kids[n++] = button("b3").key(13);
    kids[n++] = button("b4").tab_index(1).key(14);
    if (g_with_trap)
        kids[n++] = div(style{}.flex_row().gap(sp::b04), trap_kids) | keyed(77);
    return div(style{}.flex_row().items_center().gap(sp::b08).p(sp::b08), {kids, n});
}

[[nodiscard]] key_event key_press(key k, unsigned mods = 0) {
    key_event e{};
    e.key = k;
    e.pressed = true;
    e.modifiers = mods;
    return e;
}

[[nodiscard]] const char* focused_name(ui_root& root) {
    ui_node* f = root.focus().focused();
    return f ? f->accessibility_node().name : "(none)";
}

/// Quads the last frame drew with the focus ring's colour as their border.
[[nodiscard]] unsigned ring_quads(const lx::scene::draw_list& list) {
    const lx::gfx::packed_rgba ring = lx::gfx::premultiplied_half(token(tok::border_focused));
    unsigned n = 0;
    for (const lx::gfx::quad_instance& q : list.quad_instances())
        if (q.border_color[0] == ring.r && q.border_color[1] == ring.g &&
            q.border_color[2] == ring.b && q.border_color[3] == ring.a && q.border_widths[0] != 0)
            ++n;
    return n;
}

} // namespace

LUMEN_TEST(focus_order_tab_scope_and_focus_visible) {
    g_with_trap = true;
    g_drop_b3 = false;
    harness h{describe_focus, {0, 0, 800, 60}};
    ui_root& r = h.root;
    focus_manager& f = r.focus();
    LUMEN_CHECK(f.focused() == nullptr && ring_quads(*h.list) == 0);

    // Tab order: the positive index first (b4), then tree order (b0, b3, t0, t1); b1
    // (negative) and b2 (disabled) are skipped; it wraps.
    const char* expected[] = {"b4", "b0", "b3", "t0", "t1", "b4"};
    for (const char* name : expected) {
        LUMEN_CHECK(r.key(key_press(key::tab)));
        LUMEN_CHECK(std::strcmp(focused_name(r), name) == 0);
        LUMEN_CHECK(f.focus_visible());
    }
    // Shift-tab goes back, wrapping.
    LUMEN_CHECK(r.key(key_press(key::tab, k_mod_shift)) && std::strcmp(focused_name(r), "t1") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab, k_mod_shift)) && std::strcmp(focused_name(r), "t0") == 0);

    // The ring: one 1 px `border.focused` box, drawn on the next frame; the damage is the
    // two buttons the focus moved between.
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ring_quads(*h.list) == 1);
    ui_node* t0 = f.focused();
    LUMEN_CHECK(t0->has_flag(node_flag::focus_ring));

    // A pointer press moves the focus without a ring (focus-visible is keyboard only).
    ui_node* b0 = find(r.tree(), lx::a11y::role::button, "b0");
    ui_damage().clear();
    (void)r.pointer_move(centre(*b0));
    LUMEN_CHECK(r.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(f.focused() == b0 && !f.focus_visible() && !b0->has_flag(node_flag::focus_ring));
    LUMEN_CHECK(!t0->has_flag(node_flag::focused));
    LUMEN_CHECK(r.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_activated == 0);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ring_quads(*h.list) == 0);
    // A key handled while focused brings the ring back (space activates it too).
    g_activated = -1;
    LUMEN_CHECK(r.key(key_press(key::space)));
    LUMEN_CHECK(g_activated == 0 && f.focus_visible());
    LUMEN_CHECK(h.tick() && ring_quads(*h.list) == 1);

    // Programmatic focus follows the last input: after a pointer press, no ring.
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
    ui_node* b3 = find(r.tree(), lx::a11y::role::button, "b3");
    LUMEN_CHECK(f.focus(*b3));
    LUMEN_CHECK(f.focused() == b3 && !f.focus_visible());
    // A disabled button cannot take the focus; a negative-order one can, by program.
    LUMEN_CHECK(!f.focus(*find(r.tree(), lx::a11y::role::button, "b2")));
    LUMEN_CHECK(f.focus(*find(r.tree(), lx::a11y::role::button, "b1")));

    // A trap confines tab to its subtree and gives the focus back when released.
    ui_node* group = find(r.tree(), lx::a11y::role::button, "t0")->parent();
    LUMEN_CHECK(f.focus(*b3, focus_cause::keyboard));
    LUMEN_CHECK(f.trap(*group));
    LUMEN_CHECK(std::strcmp(focused_name(r), "t0") == 0 && f.active_trap() == group);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "t1") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "t0") == 0);
    LUMEN_CHECK(!f.focus(*b0)); // outside the trap
    f.release(*group);
    LUMEN_CHECK(f.focused() == b3 && f.active_trap() == nullptr);
    // The trap's scope removed while active: the trap goes with it.
    LUMEN_CHECK(f.trap(*group));
    g_with_trap = false;
    invalidate_layout(*r.tree());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(f.active_trap() == nullptr && f.focused() == nullptr);

    // The focused node removed: the focus is dropped, nothing dangles.
    LUMEN_CHECK(f.focus(*find(r.tree(), lx::a11y::role::button, "b3")));
    g_drop_b3 = true;
    invalidate_layout(*r.tree());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(f.focused() == nullptr);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "b4") == 0);
    g_with_trap = true;
    g_drop_b3 = false;

    // Magnifier hook: called with the focused bounds.
    static lx::rect2i seen{};
    f.set_magnifier_hook([](void*, lx::rect2i b) { seen = b; }, nullptr);
    LUMEN_CHECK(r.key(key_press(key::tab)));
    LUMEN_CHECK(same_rect(seen, f.focused()->bounds()));
    f.set_magnifier_hook(nullptr, nullptr);
}

namespace {

int g_tab = -1;
void on_tab(int i) { g_tab = i; }

child describe_tabs(build_context&) {
    child tabs[3];
    const char* names[3] = {"a.rs", "b.rs", "c.rs"};
    for (unsigned i = 0; i < 3; ++i)
        tabs[i] = tab(names[i]).position(i, 3, 1).on_select(on_tab);
    const child left[2] = {button("x"), button("y")};
    const child kids[2] = {tab_bar(tabs), status_bar(left, {})};
    return div(style{}.flex_col().w(px(600.f)), kids);
}

} // namespace

LUMEN_TEST(arrow_keys_move_within_tab_bars_and_status_bars) {
    harness h{describe_tabs, {0, 0, 600, 100}};
    ui_root& r = h.root;
    // One stop for the bar: the selected tab.
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "b.rs") == 0);
    LUMEN_CHECK(r.key(key_press(key::right)) && std::strcmp(focused_name(r), "c.rs") == 0);
    LUMEN_CHECK(r.key(key_press(key::right)) && std::strcmp(focused_name(r), "a.rs") == 0); // wraps
    LUMEN_CHECK(r.key(key_press(key::left)) && std::strcmp(focused_name(r), "c.rs") == 0);
    LUMEN_CHECK(r.key(key_press(key::home)) && std::strcmp(focused_name(r), "a.rs") == 0);
    LUMEN_CHECK(r.key(key_press(key::end)) && std::strcmp(focused_name(r), "c.rs") == 0);
    // Enter selects: `on_select` gets the index.
    g_tab = -1;
    LUMEN_CHECK(r.key(key_press(key::enter)) && g_tab == 2);
    // Up / down are not the bar's: nothing takes them.
    LUMEN_CHECK(!r.key(key_press(key::down)));
    // Tab leaves the bar for the status bar; arrows move along it.
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "x") == 0);
    LUMEN_CHECK(r.key(key_press(key::right)) && std::strcmp(focused_name(r), "y") == 0);
}

// ── Keyboard activation ──

namespace {

int g_check = -1;
int g_switch = -1;
int g_toggle = -1;
void on_check(int v) { g_check = v; }
void on_switch(int v) { g_switch = v; }
void on_toggle_value(int v) { g_toggle = v; }
bool g_checked = false;

child describe_toggles(build_context&) {
    const child kids[] = {
        checkbox(g_checked).label("c").on_toggle(on_check),
        toggle_switch(g_checked).label("s").on_toggle(on_switch),
        toggle_button("t").selected(g_checked).on_toggle(on_toggle_value),
        button("off").disabled(true).on_press(on_check),
    };
    return div(style{}.flex_row().gap(sp::b08), kids);
}

} // namespace

LUMEN_TEST(space_and_enter_activate_controls) {
    g_checked = false;
    harness h{describe_toggles, {0, 0, 600, 40}};
    ui_root& r = h.root;
    // Unchecked: each toggle offers 1 (the next state).
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "c") == 0);
    LUMEN_CHECK(r.key(key_press(key::space)) && g_check == 1);
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::enter)) && g_switch == 1);
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::space)) && g_toggle == 1);
    // Checked: 0.
    g_checked = true;
    invalidate_layout(*r.tree());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "c") == 0);
    LUMEN_CHECK(r.key(key_press(key::space)) && g_check == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::space)) && g_switch == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::space)) && g_toggle == 0);
    // The disabled button is not in the sequence and cannot be activated.
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "c") == 0);
    ui_node* off = find(r.tree(), lx::a11y::role::button, "off");
    g_check = -1;
    LUMEN_CHECK(!off->activate() && g_check == -1);
    // A key release does nothing.
    key_event up = key_press(key::space);
    up.pressed = false;
    LUMEN_CHECK(!r.key(up));

    // Accessibility: role, name and state.
    const lx::a11y::node c = find(r.tree(), lx::a11y::role::checkbox, "c")->accessibility_node();
    LUMEN_CHECK((c.state & static_cast<unsigned>(lx::a11y::state_flag::checked)) != 0);
    LUMEN_CHECK((c.state & static_cast<unsigned>(lx::a11y::state_flag::focused)) != 0);
    const lx::a11y::node t = find(r.tree(), lx::a11y::role::toggle_button, "t")->accessibility_node();
    LUMEN_CHECK((t.state & static_cast<unsigned>(lx::a11y::state_flag::pressed)) != 0);
    LUMEN_CHECK((off->accessibility_node().state & static_cast<unsigned>(lx::a11y::state_flag::disabled)) != 0);
}

// ── Reducers wired into the root ──

namespace {

struct counter_state {
    int count = 0;
    int id = 0;
};
enum class counter_action { increment };

using counter_host = reducer_node<counter_state, counter_action>;

int g_root_describes = 0;
int g_builds[2] = {};
char g_count_text[2][16] = {};

void reduce_counter(counter_state& s, const counter_action&) { ++s.count; }

child build_counter(const counter_state& s, counter_host& host) {
    ++g_builds[s.id];
    std::snprintf(g_count_text[s.id], sizeof(g_count_text[s.id]), "n=%d", s.count);
    const child kids[3] = {
        button("+").on_press(bind(host, counter_action::increment)).name(s.id == 0 ? "inc-a" : "inc-b"),
        label(g_count_text[s.id]),
        div(style{}.size(px(10.f)).bg(tok::element_background), {}),
    };
    return div(style{}.flex_row().items_center().gap(sp::b04).w(px(160.f)), kids);
}

child describe_counters(build_context&) {
    ++g_root_describes;
    const child kids[2] = {
        reducer<counter_state, counter_action>({0, 0}, reduce_counter, build_counter, 1),
        reducer<counter_state, counter_action>({0, 1}, reduce_counter, build_counter, 2),
    };
    return div(style{}.flex_row().gap(sp::b16).p(sp::b08), kids);
}

} // namespace

LUMEN_TEST(reducer_dispatch_redescribes_only_its_subtree) {
    g_root_describes = 0;
    g_builds[0] = g_builds[1] = 0;
    harness h{describe_counters, {0, 0, 600, 60}};
    ui_root& r = h.root;
    LUMEN_CHECK(g_root_describes == 1 && g_builds[0] == 1 && g_builds[1] == 1);
    ui_node* a = r.tree()->child_at(0);
    ui_node* b = r.tree()->child_at(1);
    LUMEN_CHECK(a && b && a->child_count() == 1 && b->child_count() == 1);
    ui_node* inc_a = find(a, lx::a11y::role::button, "inc-a");
    ui_node* a_label = inc_a->parent()->child_at(1);
    ui_node* a_square = inc_a->parent()->child_at(2);
    ui_node* b_row = b->child_at(0);
    const lx::rect2i b_bounds = b->bounds();
    LUMEN_CHECK(!h.tick());

    // A click on A's button dispatches into A: the next frame describes A's subtree alone.
    (void)r.pointer_move(centre(*inc_a));
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(static_cast<counter_host*>(a)->state().count == 1);
    LUMEN_CHECK(ui_damage().needs_rebuild() && !ui_damage().needs_layout());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(g_root_describes == 1 && g_builds[0] == 2 && g_builds[1] == 1);
    LUMEN_CHECK(r.last_stage_times().reducers_rebuilt == 1 && !r.last_stage_times().described);
    // The reconcile reused every node of A (the label took new text in place) and left B
    // alone.
    LUMEN_CHECK(find(a, lx::a11y::role::button, "inc-a") == inc_a);
    LUMEN_CHECK(inc_a->parent()->child_at(1) == a_label && inc_a->parent()->child_at(2) == a_square);
    LUMEN_CHECK(b->child_at(0) == b_row);
    // Invalidation is exactly the changed nodes: the label's text (and the button the
    // release un-pressed), all inside A, nothing of B.
    const damage_ledger& d = r.frame_damage();
    LUMEN_CHECK(d.count() >= 1);
    for (unsigned i = 0; i < d.count(); ++i) {
        LUMEN_CHECK(inside(d.rects()[i], a->bounds()));
        LUMEN_CHECK(!overlaps(d.rects()[i], b_bounds));
    }
    LUMEN_CHECK(overlaps(d.union_bounds(), a_label->bounds()));
    LUMEN_CHECK(!h.tick());

    // Two dispatches before a frame: one rebuild.
    static_cast<counter_host*>(b)->dispatch(counter_action::increment);
    static_cast<counter_host*>(b)->dispatch(counter_action::increment);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(g_builds[0] == 2 && g_builds[1] == 2 && g_root_describes == 1);
    LUMEN_CHECK(static_cast<counter_host*>(b)->state().count == 2);

    // The tree around the reducers described again: their state and subtrees survive and
    // they are not rebuilt (the reconciler leaves a reducer's children to it).
    invalidate_layout(*r.tree());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(g_root_describes == 2 && g_builds[0] == 2 && g_builds[1] == 2);
    LUMEN_CHECK(r.tree()->child_at(0) == a && a->child_count() == 1);
    LUMEN_CHECK(find(a, lx::a11y::role::button, "inc-a") == inc_a);
    LUMEN_CHECK(static_cast<counter_host*>(a)->state().count == 1);
    LUMEN_CHECK(!h.tick());
}

// ── Motion: frames only while something moves ──

namespace {

std::uint64_t g_now_ns = 1'000'000'000ull;
std::uint64_t test_clock() { return g_now_ns; }

struct fade_props {
    int unused = 0;
};

/// A node that fades in when hovered for 500 ms, as a tooltip will.
class fade_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    bool apply(const fade_props&) {
        set_flag(node_flag::interactive, true);
        set_flag(node_flag::hover_intent, true);
        return false;
    }
    [[nodiscard]] lx::size2i measure(lx::layout::constraints, size_hint) const override {
        return {40, 20};
    }
    void on_timer(std::uint32_t tag) override {
        if (tag != k_timer_hover_intent)
            return;
        ++timers;
        fade.start(0.f, 1.f, k_motion_ms);
        invalidate_paint(*this);
    }
    void emit(draw_scope& out) const override {
        last = animate(*this, fade);
        quad_style q{};
        q.background = lx::color::rgb(1.f, 1.f, 1.f, last);
        out.quad(bounds(), q);
    }
    transition fade{};
    int timers = 0;
    mutable float last = 0.f;
};

child describe_fade(build_context&) {
    const child kids[1] = {describe<fade_node, fade_props>({})};
    return div(style{}.p(px(10.f)), kids);
}

} // namespace

LUMEN_TEST(animations_request_frames_only_while_they_run) {
    set_ui_clock(test_clock);
    set_reduced_motion(false);
    harness h{describe_fade, {0, 0, 200, 100}};
    ui_root& r = h.root;
    auto* node = static_cast<fade_node*>(r.tree()->child_at(0));
    // G1: an idle tree asks for nothing — no frame, no deadline.
    LUMEN_CHECK(!h.tick() && ui_damage().empty() && r.next_deadline_ns() == 0);

    // Hover: a repaint now, and a deadline 500 ms on (no frames in between).
    LUMEN_CHECK(r.pointer_move(centre(*node)));
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 500'000'000ull);
    LUMEN_CHECK(h.tick());
    for (int i = 0; i < 4; ++i) {
        g_now_ns += 100'000'000ull;
        LUMEN_CHECK(!h.tick());
    }
    // The deadline: the timer fires in the tick and the fade starts.
    g_now_ns += 100'000'000ull;
    LUMEN_CHECK(r.next_deadline_ns() <= g_now_ns);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(node->timers == 1 && r.next_deadline_ns() == 0);
    LUMEN_CHECK(node->last == 0.f && ui_damage().needs_animate());
    // Frames at 60 Hz while it runs, each repainting the node's bounds only.
    unsigned frames = 0;
    float previous = node->last;
    while (ui_damage().needs_animate() && frames < 100) {
        g_now_ns += 16'666'667ull;
        LUMEN_CHECK(h.tick());
        LUMEN_CHECK(same_rect(r.frame_damage().union_bounds(), node->bounds()));
        LUMEN_CHECK(node->last >= previous);
        previous = node->last;
        ++frames;
    }
    std::printf("    fade: %u frames over %u ms, final %.3f\n", frames, k_motion_ms, node->last);
    LUMEN_CHECK(frames >= 8 && frames <= 10);
    LUMEN_CHECK(node->last == 1.f);
    // Settled: nothing more is asked.
    LUMEN_CHECK(ui_damage().empty() && r.next_deadline_ns() == 0);
    g_now_ns += 1'000'000'000ull;
    LUMEN_CHECK(!h.tick());

    // Leaving cancels a pending hover timer.
    r.pointer_leave();
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(r.pointer_move(centre(*node)) && r.next_deadline_ns() != 0);
    r.pointer_leave();
    LUMEN_CHECK(r.next_deadline_ns() == 0);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(!h.tick());

    // Reduced motion: the fade lands at once — one repaint, no animation frames.
    set_reduced_motion(true);
    LUMEN_CHECK(r.pointer_move(centre(*node)));
    LUMEN_CHECK(h.tick());
    g_now_ns += 600'000'000ull;
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(node->timers == 2 && node->last == 1.f && !ui_damage().needs_animate());
    LUMEN_CHECK(!h.tick());
    set_reduced_motion(false);
    r.pointer_leave();
    (void)h.tick();

    // Easing: ease-out is ahead of linear midway; both reach the end.
    LUMEN_CHECK(ease(easing::ease_out, 0.5f) > 0.5f && ease(easing::ease_out, 1.f) == 1.f);
    LUMEN_CHECK(ease(easing::ease_in_out, 0.5f) == 0.5f);
    set_ui_clock(nullptr);
}

// ── Timers ──

LUMEN_TEST(timer_queue_is_bounded_and_forgets_dead_nodes) {
    set_ui_clock(test_clock);
    timer_queue& q = ui_timers();
    struct sink final : ui_node {
        [[nodiscard]] element_type_id type() const override { return 9999; }
        void on_timer(std::uint32_t tag) override { last = tag; }
        std::uint32_t last = 0;
    };
    std::unique_ptr<sink> nodes[timer_queue::k_capacity + 1];
    for (auto& n : nodes)
        n = std::make_unique<sink>();
    const unsigned before = q.size();
    unsigned accepted = 0;
    for (auto& n : nodes)
        accepted += q.schedule(*n, k_timer_user, 1000) ? 1u : 0u;
    LUMEN_CHECK(accepted == timer_queue::k_capacity - before);
    LUMEN_CHECK(q.rejected() >= 1);
    // A node destroyed takes its timer with it.
    nodes[0].reset();
    LUMEN_CHECK(q.size() == timer_queue::k_capacity - 1);
    g_now_ns += 2000;
    LUMEN_CHECK(q.fire_due(g_now_ns) >= timer_queue::k_capacity - 1 - before);
    LUMEN_CHECK(nodes[1]->last == k_timer_user);
    LUMEN_CHECK(q.size() == 0 && q.next_deadline() == 0);
    set_ui_clock(nullptr);
}

// ── Handlers ──

LUMEN_TEST(handlers_are_trivially_copyable_and_carry_bindings) {
    static int hit = 0;
    handler none{};
    none(3); // nothing
    LUMEN_CHECK(!none);
    handler plain{+[] { ++hit; }};
    handler valued{+[](int v) { hit += v; }};
    int* target = &hit;
    handler capturing{[target](int v) { *target = v * 10; }};
    hit = 0;
    plain();
    LUMEN_CHECK(hit == 1);
    valued(5);
    LUMEN_CHECK(hit == 6);
    capturing(4);
    LUMEN_CHECK(hit == 40);
    static_assert(std::is_trivially_copyable_v<handler>);
    static_assert(sizeof(handler) == 32);
}

// ── G5: a steady frame of every component allocates nothing ──

namespace {

child describe_gallery(build_context&) {
    static const char* const keys[2] = {"Ctrl-S", "Ctrl-Shift-P"};
    child tabs[3];
    const char* names[3] = {"main.rs", "lib.rs", "mod.rs"};
    for (unsigned i = 0; i < 3; ++i)
        tabs[i] = tab(names[i]).position(i, 3, 0);
    const child status_left[2] = {button("main").icon(icon::folder), label("Ln 4, Col 2").color(tok::text_muted)};
    const child status_right[1] = {icon_button(icon::bell)};
    const child tool_left[1] = {label("Project")};
    const child tool_right[1] = {icon_button(icon::plus).shape(icon_button_shape::square)};
    const child controls[] = {
        button("Save").style(button_style::filled).icon(icon::check),
        button("Open").style(button_style::outlined),
        button("Ghost"),
        icon_button(icon::settings),
        toggle_button("Grid").selected(true).icon(icon::menu),
        checkbox(true).label("Wrap"),
        checkbox(toggle_state::indeterminate),
        toggle_switch(true).label("Wi-Fi"),
    };
    const child misc[] = {
        avatar("LM"),
        avatar(nullptr),
        key_binding(keys),
        key_binding_hint(std::span<const char* const>{keys, 1}, tok::surface_background).prefix("Save").suffix("now"),
        vertical_divider(),
        label("muted").color(tok::text_muted).size(label_size::small),
    };
    const child body = label("Body text of the panel, wrapping at its width.");
    const child dock[1] = {panel(body).toolbar_start(tool_left).toolbar_end(tool_right)};
    const child kids[] = {
        headline("Components"),
        div(style{}.flex_row().items_center().gap(sp::b08), controls),
        divider(),
        div(style{}.flex_row().items_center().gap(sp::b08), misc),
        progress_bar(30.f, 100.f),
        tab_bar(tabs),
        div(style{}.w(px(400.f)).h(px(120.f)), dock),
        status_bar(status_left, status_right),
    };
    return div(style{}.flex_col().gap(sp::b08).p(sp::b08).w(px(720.f)), kids);
}

} // namespace

LUMEN_TEST(steady_frames_of_every_component_allocate_nothing) {
    set_active_builtin_theme(appearance::dark);
    harness h{describe_gallery, {0, 0, 760, 560}};
    ui_root& r = h.root;
    ui_node* save = find(r.tree(), lx::a11y::role::button, "Save");
    ui_node* open = find(r.tree(), lx::a11y::role::button, "Open");
    LUMEN_CHECK(save && open);
    const auto count = [](const auto& self, const ui_node* n) -> unsigned {
        unsigned c = 1;
        for (unsigned i = 0; i < n->child_count(); ++i)
            if (const ui_node* k = n->child_at(i))
                c += self(self, k);
        return c;
    };
    std::printf("    gallery: %u nodes\n", count(count, r.tree()));
    // Warm: glyphs and icons rasterised, every tile placed, the ring and both hovers drawn.
    for (int i = 0; i < 4; ++i) {
        ui_damage().add(r.tree()->bounds());
        (void)r.pointer_move(centre(i % 2 ? *save : *open));
        (void)r.key(key_press(key::tab));
        LUMEN_CHECK(h.tick());
    }
    const auto run = [&](const char* what, auto&& step) {
        const unsigned long long allocs = g_allocations.load();
        const std::size_t heap = mallinfo2().uordblks;
        std::vector<double> us;
        us.reserve(300);
        const unsigned long long allocs_reserved = g_allocations.load();
        for (int i = 0; i < 300; ++i) {
            step(i);
            const auto t0 = std::chrono::steady_clock::now();
            LUMEN_CHECK(h.tick());
            const auto t1 = std::chrono::steady_clock::now();
            us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        const unsigned long long during = g_allocations.load() - allocs_reserved;
        (void)allocs;
        LUMEN_CHECK(during == 0);
        LUMEN_CHECK(mallinfo2().uordblks <= heap + 300 * sizeof(double) + 64);
        std::sort(us.begin(), us.end());
        std::printf("    %-28s tick p50 %7.1f us  p95 %7.1f us  allocations %llu  primitives %u\n",
                    what, us[us.size() / 2], us[us.size() * 95 / 100], during,
                    h.list->primitive_count());
    };
    // A full repaint of the gallery (paint damage over the whole tree, no describe).
    run("full repaint", [&](int) { ui_damage().add(r.tree()->bounds()); });
    // A hover moving between two buttons: two controls repaint.
    run("hover", [&](int i) { (void)r.pointer_move(centre(i % 2 ? *save : *open)); });
    // Tab: the focus and its ring move.
    run("focus", [&](int) { (void)r.key(key_press(key::tab)); });
    // A press and release (an activation with no handler).
    run("press", [&](int i) { (void)r.pointer_button(mouse_button::left, i % 2 == 0); });
    // Everything described again over an unchanged tree: allocation-free too (arena only).
    run("describe", [&](int) { invalidate_layout(*r.tree()); });
}

// ════════════════════════════════════════════════════════════════════════════════════════
// Stage 4: overlays, scrolling, virtual lists, the text input and the remaining components.
// ════════════════════════════════════════════════════════════════════════════════════════

namespace {

[[nodiscard]] ui_node* find_type(ui_node* n, element_type_id t) {
    if (!n)
        return nullptr;
    if (n->type() == t)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find_type(n->child_at(i), t))
            return hit;
    return nullptr;
}

[[nodiscard]] unsigned count_nodes(const ui_node* n) {
    unsigned c = 1;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (const ui_node* k = n->child_at(i))
            c += count_nodes(k);
    return c;
}

[[nodiscard]] key_event text_key(char32_t c, unsigned mods = 0) {
    key_event e{};
    e.key = c == ' ' ? key::space : key::unknown;
    e.pressed = true;
    e.modifiers = mods;
    e.codepoint = c;
    return e;
}

void type_text(ui_root& r, const char* ascii) {
    for (const char* p = ascii; *p; ++p)
        LUMEN_CHECK(r.key(text_key(static_cast<char32_t>(*p))));
}

void click(ui_root& r, lx::point2i at) {
    (void)r.pointer_move(at);
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
}

[[nodiscard]] bool same(lx::rect2i a, lx::rect2i b) { return same_rect(a, b); }

[[nodiscard]] lx::point2i centre_of(lx::rect2i b) { return {b.x + b.width / 2, b.y + b.height / 2}; }

} // namespace

// ── Overlay placement ──

LUMEN_TEST(overlay_placement_flips_and_shifts_at_window_edges) {
    const lx::rect2i win{0, 0, 400, 300};
    const overlay_placement p{}; // below, start, 8 px margin
    // Room below: it hangs from the anchor's bottom-left corner.
    LUMEN_CHECK(same(place_overlay({20, 20, 60, 22}, {200, 100}, win, p), {20, 42, 200, 100}));
    // At the bottom: above the anchor.
    LUMEN_CHECK(same(place_overlay({20, 250, 60, 22}, {200, 100}, win, p), {20, 150, 200, 100}));
    // At the right edge: from the anchor's end instead (GPUI's `SwitchAnchor`).
    LUMEN_CHECK(same(place_overlay({300, 20, 60, 22}, {200, 100}, win, p), {160, 42, 200, 100}));
    // Fits on neither side: the side that overflows less, then shifted inside the margin.
    LUMEN_CHECK(same(place_overlay({20, 120, 60, 22}, {200, 250}, win, p), {20, 42, 200, 250}));
    // Wider than the window: its start edge at the margin.
    LUMEN_CHECK(place_overlay({20, 20, 60, 22}, {500, 100}, win, p).x == 8);
    // A point (a context menu) in the bottom-right corner: up and to the left of it.
    LUMEN_CHECK(same(place_overlay({390, 290, 0, 0}, {120, 80}, win, p), {270, 210, 120, 80}));
    // A submenu at the right edge opens to the left.
    overlay_placement sub{};
    sub.side = overlay_side::right;
    sub.gap = 5;
    LUMEN_CHECK(same(place_overlay({250, 40, 120, 23}, {150, 100}, win, sub), {95, 40, 150, 100}));
    // Shifted, not flipped, when flipping is off; nothing moves when shifting is off too.
    overlay_placement rigid{};
    rigid.flip = false;
    LUMEN_CHECK(place_overlay({20, 250, 60, 22}, {200, 100}, win, rigid).y == 192);
    rigid.shift = false;
    LUMEN_CHECK(place_overlay({20, 250, 60, 22}, {200, 100}, win, rigid).y == 272);
    // A modal 5 rem from the top, centred; toasts at the bottom.
    overlay_placement top{};
    top.side = overlay_side::window_top;
    LUMEN_CHECK(same(place_overlay({}, {200, 100}, win, top), {100, 80, 200, 100}));
    overlay_placement bottom{};
    bottom.side = overlay_side::window_bottom;
    LUMEN_CHECK(same(place_overlay({}, {200, 100}, win, bottom), {100, 192, 200, 100}));
}

// ── Popover menus: open, keyboard, activation, dismissal ──

namespace {

bool g_menu_open = false;
int g_menu_dismissed = 0;
int g_menu_action = -1;
lx::point2i g_trigger_at{20, 20};
const char* const k_copy_keys[1] = {"Ctrl-C"};
const char* const k_paste_keys[1] = {"Ctrl-V"};

void on_menu_dismiss(int) {
    g_menu_open = false;
    ++g_menu_dismissed;
}
void on_menu_action(int v) { g_menu_action = v; }
void toggle_menu() { g_menu_open = !g_menu_open; }

child describe_menu(build_context&) {
    static menu_entry sub[2];
    static menu_entry entries[7];
    sub[0] = menu_item("Zoom in", on_menu_action).with_value(20);
    sub[1] = menu_item("Zoom out", on_menu_action).with_value(21);
    entries[0] = menu_header("Edit");
    entries[1] = menu_item("Copy", on_menu_action).with_value(1).icon(icon::copy).keys(k_copy_keys);
    entries[2] = menu_item("Paste", on_menu_action).with_value(2).keys(k_paste_keys);
    entries[3] = menu_item("Delete", on_menu_action).with_value(3).disabled();
    entries[4] = menu_separator();
    entries[5] = menu_submenu("View", sub);
    entries[6] = menu_item("Wrap", on_menu_action).with_value(6).toggle(true);
    const child trigger = button("Menu").name("trigger").on_press(toggle_menu);
    const child menu_kids[1] = {popover_menu(trigger, entries).open(g_menu_open).on_dismiss(on_menu_dismiss)};
    const child kids[2] = {
        div(style{}.absolute().left(px(static_cast<float>(g_trigger_at.x)))
                .top(px(static_cast<float>(g_trigger_at.y))),
            menu_kids),
        div(style{}.absolute().left(px(200.f)).top(px(20.f)), {}),
    };
    return div(style{}.relative().size_full(), kids);
}

void redescribe(harness& h) {
    invalidate_layout(*h.root.tree());
    (void)h.tick();
}

[[nodiscard]] ui_node* menu_item_node(ui_root& r, const char* name) {
    if (overlay_node* top = ui_overlays().top())
        return find(top->content(), lx::a11y::role::menu_item, name);
    return find(r.tree(), lx::a11y::role::menu_item, name);
}

} // namespace

LUMEN_TEST(popover_menu_opens_navigates_and_dismisses) {
    set_active_builtin_theme(appearance::dark);
    set_reduced_motion(true);
    g_menu_open = false;
    g_menu_dismissed = 0;
    g_trigger_at = {20, 20};
    harness h{describe_menu, {0, 0, 800, 300}};
    ui_root& r = h.root;
    ui_node* trigger = find(r.tree(), lx::a11y::role::button, "trigger");
    LUMEN_CHECK(trigger && ui_overlays().open_count() == 0);
    const lx::rect2i tb = trigger->bounds();

    // Opened by the app: below the trigger, 5 px out for the menu's padding and border.
    g_menu_open = true;
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    overlay_node* ov = ui_overlays().top();
    LUMEN_CHECK(ov && ov->kind() == overlay_kind::menu && ov->is_open());
    const lx::rect2i mb = ov->content()->bounds();
    print_rect("menu", mb);
    LUMEN_CHECK(mb.y == tb.y + tb.height && mb.x == tb.x - 5);
    LUMEN_CHECK(mb.width >= 200); // `min_w(px(200.))`
    // Entries: 23 px rows inset 4 px, the separator 1 px between 6 px margins.
    ui_node* copy = menu_item_node(r, "Copy");
    ui_node* paste = menu_item_node(r, "Paste");
    LUMEN_CHECK(copy && paste && copy->bounds().height == 23);
    LUMEN_CHECK(copy->bounds().x == mb.x + 1 + 4);
    LUMEN_CHECK(paste->bounds().y == copy->bounds().y + 23);
    // Drawn after the tree: the frame's last primitives are the menu's.
    LUMEN_CHECK(r.frame_damage().count() >= 1 && overlaps(r.frame_damage().union_bounds(), mb));
    // The focus is trapped in it: the first entry, arrows and tab move inside.
    LUMEN_CHECK(r.focus().active_trap() == ov->content());
    LUMEN_CHECK(std::strcmp(focused_name(r), "Copy") == 0);
    LUMEN_CHECK(r.key(key_press(key::down)) && std::strcmp(focused_name(r), "Paste") == 0);
    // The disabled entry is skipped.
    LUMEN_CHECK(r.key(key_press(key::down)) && std::strcmp(focused_name(r), "View") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "Wrap") == 0);
    // The pointer: hovering an entry, nothing beneath.
    LUMEN_CHECK(r.pointer_move(centre(*paste)) && r.pointer().hovered() == paste);

    // Escape dismisses it: the app is told, the focus goes back.
    LUMEN_CHECK(r.key(key_press(key::escape)));
    LUMEN_CHECK(!g_menu_open && g_menu_dismissed == 1 && ui_overlays().open_count() == 0);
    LUMEN_CHECK(r.focus().active_trap() == nullptr);
    redescribe(h);

    // A press outside dismisses it; a press on the trigger is the trigger's (it toggles).
    g_menu_open = true;
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    click(r, centre(*trigger));
    LUMEN_CHECK(!g_menu_open && g_menu_dismissed == 1); // toggled by the trigger, not dismissed
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 0);
    g_menu_open = true;
    redescribe(h);
    (void)r.pointer_move({790, 290});
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(!g_menu_open && g_menu_dismissed == 2 && ui_overlays().open_count() == 0);
    redescribe(h);

    // Activating an entry runs it and closes the menu: a click, then enter.
    g_menu_open = true;
    redescribe(h);
    g_menu_action = -1;
    click(r, centre(*menu_item_node(r, "Paste")));
    LUMEN_CHECK(g_menu_action == 2 && !g_menu_open && g_menu_dismissed == 3);
    redescribe(h);
    g_menu_open = true;
    redescribe(h);
    LUMEN_CHECK(r.key(key_press(key::down)) && r.key(key_press(key::enter)));
    LUMEN_CHECK(g_menu_action == 2 && !g_menu_open && g_menu_dismissed == 4);
    redescribe(h);
    // A disabled entry does nothing; the menu stays.
    g_menu_open = true;
    redescribe(h);
    g_menu_action = -1;
    click(r, centre(*menu_item_node(r, "Delete")));
    LUMEN_CHECK(g_menu_action == -1 && g_menu_open && ui_overlays().open_count() == 1);

    // A submenu: the right arrow opens it (the focus goes in), left closes it.
    (void)r.focus().focus(*menu_item_node(r, "View"), focus_cause::keyboard);
    LUMEN_CHECK(r.key(key_press(key::right)));
    (void)h.tick();
    LUMEN_CHECK(ui_overlays().open_count() == 2);
    overlay_node* sub = ui_overlays().top();
    ui_node* view = find(ov->content(), lx::a11y::role::menu_item, "View");
    LUMEN_CHECK(sub && sub->nested() && std::strcmp(focused_name(r), "Zoom in") == 0);
    // Right of its item, its first entry level with it.
    LUMEN_CHECK(sub->content()->bounds().x == view->bounds().x + view->bounds().width + 5);
    LUMEN_CHECK(find(sub->content(), lx::a11y::role::menu_item, "Zoom in")->bounds().y == view->bounds().y);
    LUMEN_CHECK(r.key(key_press(key::left)) && ui_overlays().open_count() == 1);
    LUMEN_CHECK(std::strcmp(focused_name(r), "View") == 0);
    // Clicking the item opens it too; an entry in it closes the whole chain.
    click(r, centre(*view));
    (void)h.tick();
    LUMEN_CHECK(ui_overlays().open_count() == 2);
    click(r, centre(*find(ui_overlays().top()->content(), lx::a11y::role::menu_item, "Zoom out")));
    LUMEN_CHECK(g_menu_action == 21 && !g_menu_open && ui_overlays().open_count() == 0);
    redescribe(h);

    // At the window's bottom-right corner it opens above and leftwards.
    g_trigger_at = {730, 262};
    redescribe(h);
    g_menu_open = true;
    redescribe(h);
    ov = ui_overlays().top();
    trigger = find(r.tree(), lx::a11y::role::button, "trigger");
    const lx::rect2i flipped = ov->content()->bounds();
    print_rect("menu at the corner", flipped);
    LUMEN_CHECK(flipped.y + flipped.height <= trigger->bounds().y);
    LUMEN_CHECK(flipped.x + flipped.width <= 800 - 8 && flipped.x >= 8);
    g_menu_open = false;
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 0);
    set_reduced_motion(false);
}

// ── Context menus ──

namespace {

int g_ctx_action = -1;
void on_ctx_action(int v) { g_ctx_action = v; }

child describe_context(build_context&) {
    static menu_entry entries[3];
    entries[0] = menu_item("Open", on_ctx_action).with_value(1);
    entries[1] = menu_item("Rename", on_ctx_action).with_value(2);
    entries[2] = menu_item("Delete", on_ctx_action).with_value(3);
    const child area = div(style{}.size_full().bg(tok::editor_background), {});
    const child kids[1] = {context_menu(area, entries)};
    return div(style{}.size_full(), kids);
}

} // namespace

LUMEN_TEST(context_menu_opens_at_the_pointer) {
    set_reduced_motion(true);
    harness h{describe_context, {0, 0, 400, 300}};
    ui_root& r = h.root;
    (void)r.pointer_move({50, 60});
    LUMEN_CHECK(r.pointer_button(mouse_button::right, true));
    (void)r.pointer_button(mouse_button::right, false);
    (void)h.tick();
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    const lx::rect2i mb = ui_overlays().top()->content()->bounds();
    LUMEN_CHECK(mb.x == 50 && mb.y == 60);
    // A second secondary press moves it; near the corner it opens up and to the left.
    (void)r.pointer_move({380, 280});
    LUMEN_CHECK(r.pointer_button(mouse_button::right, true));
    (void)h.tick();
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    const lx::rect2i moved = ui_overlays().top()->content()->bounds();
    LUMEN_CHECK(moved.x + moved.width <= 380 && moved.y + moved.height <= 280);
    // An entry runs and closes it.
    g_ctx_action = -1;
    click(r, centre(*menu_item_node(r, "Rename")));
    LUMEN_CHECK(g_ctx_action == 2 && ui_overlays().open_count() == 0);
    (void)h.tick();
    set_reduced_motion(false);
}

// ── Modals ──

namespace {

bool g_modal_open = false;
int g_modal_dismissed = 0;
void on_modal_dismiss(int) {
    g_modal_open = false;
    ++g_modal_dismissed;
}

child describe_modal(build_context&) {
    const child body_kids[2] = {button("OK").name("ok"), button("Cancel").name("cancel")};
    const child body = div(style{}.flex_row().gap(sp::b08), body_kids);
    const child kids[3] = {
        button("Behind").name("behind"),
        button("Also behind").name("behind2"),
        modal(body).title("Confirm").open(g_modal_open).on_dismiss(on_modal_dismiss).width(320.f),
    };
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).size_full(), kids);
}

} // namespace

LUMEN_TEST(modal_traps_focus_scrims_the_tree_and_dismisses) {
    set_reduced_motion(true);
    g_modal_open = false;
    g_modal_dismissed = 0;
    harness h{describe_modal, {0, 0, 640, 400}};
    ui_root& r = h.root;
    ui_node* behind = find(r.tree(), lx::a11y::role::button, "behind");
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "behind") == 0);

    g_modal_open = true;
    redescribe(h);
    overlay_node* ov = ui_overlays().top();
    LUMEN_CHECK(ov && ov->kind() == overlay_kind::modal);
    const lx::rect2i box = ov->content()->bounds();
    print_rect("modal", box);
    // Centred, 5 rem from the top, 320 px wide.
    LUMEN_CHECK(box.y == 80 && box.width == 320 && box.x == (640 - 320) / 2);
    // The scrim: a quad over the whole window, `elevated_surface.background` at 80 %.
    bool scrim = false;
    const lx::color sc = token(tok::elevated_surface_background, 0.8f);
    const lx::gfx::packed_rgba want = lx::gfx::premultiplied_half(sc);
    for (const lx::gfx::quad_instance& q : h.list->quad_instances())
        scrim = scrim || (q.bounds[0] == 0.f && q.bounds[1] == 0.f && q.bounds[2] == 640.f &&
                          q.bounds[3] == 400.f && q.background[3] == want.a);
    LUMEN_CHECK(scrim);
    // Focus trapped: the header's close button, then OK, Cancel, and round again.
    LUMEN_CHECK(r.focus().active_trap() == ov->content());
    LUMEN_CHECK(std::strcmp(focused_name(r), "Close") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "ok") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "cancel") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab)) && std::strcmp(focused_name(r), "Close") == 0);
    LUMEN_CHECK(r.key(key_press(key::tab, k_mod_shift)) && std::strcmp(focused_name(r), "cancel") == 0);
    // The tree beneath takes no pointer: no hover, a press on the scrim dismisses (consumed).
    (void)r.pointer_move(centre(*behind));
    LUMEN_CHECK(r.pointer().hovered() == nullptr);
    LUMEN_CHECK(r.pointer_button(mouse_button::left, true));
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(!g_modal_open && g_modal_dismissed == 1 && ui_overlays().open_count() == 0);
    // The focus is back where it was.
    LUMEN_CHECK(std::strcmp(focused_name(r), "behind") == 0 && r.focus().active_trap() == nullptr);
    redescribe(h);
    // Escape, and the close button, dismiss it too.
    g_modal_open = true;
    redescribe(h);
    LUMEN_CHECK(r.key(key_press(key::escape)) && g_modal_dismissed == 2);
    redescribe(h);
    g_modal_open = true;
    redescribe(h);
    LUMEN_CHECK(r.key(key_press(key::enter)) && g_modal_dismissed == 3); // Close has the focus
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 0);
    set_reduced_motion(false);
}

// ── The overlay layer's cap ──

namespace {

unsigned g_cap_count = 0;

child describe_many_popovers(build_context&) {
    child kids[ui_node::k_max_children]{};
    const unsigned n = std::min(g_cap_count, ui_node::k_max_children);
    for (unsigned i = 0; i < n; ++i) {
        overlay_props o{};
        o.open = true;
        o.dismiss_outside = false;
        kids[i] = overlay(o, div(style{}.size(px(10.f)).bg(tok::element_background), {}));
    }
    return div(style{}.size_full(), {kids, n});
}

} // namespace

LUMEN_TEST(overlay_layer_is_bounded) {
    g_cap_count = overlay_layer::k_capacity + 3;
    const std::uint64_t before = ui_overlays().rejected();
    harness h{describe_many_popovers, {0, 0, 200, 200}};
    LUMEN_CHECK(ui_overlays().open_count() == overlay_layer::k_capacity);
    LUMEN_CHECK(ui_overlays().rejected() - before == 3); // overflow_action::reject
    g_cap_count = 0;
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 0);
}

// ── Scroll areas ──

namespace {

char g_scroll_names[30][8];

child describe_scroll(build_context&) {
    child rows[30];
    for (unsigned i = 0; i < 30; ++i) {
        std::snprintf(g_scroll_names[i], sizeof(g_scroll_names[i]), "r%u", i);
        rows[i] = button(g_scroll_names[i]).name(g_scroll_names[i]).full_width();
    }
    const child kids[1] = {scroll_area(style{}.w(px(200.f)).h(px(200.f)), rows)};
    return div(style{}.flex_col().p(px(10.f)), kids);
}

} // namespace

LUMEN_TEST(scroll_area_scrolls_without_relayout_and_damages_the_viewport) {
    set_active_builtin_theme(appearance::dark);
    harness h{describe_scroll, {0, 0, 320, 260}};
    ui_root& r = h.root;
    auto* sn = static_cast<scroll_node*>(find_type(r.tree(), scroll_node::static_type()));
    LUMEN_CHECK(sn != nullptr);
    const lx::rect2i vp = sn->bounds();
    LUMEN_CHECK(same(vp, {10, 10, 200, 200}));
    // 30 buttons of 22 px: 660 px of content.
    LUMEN_CHECK(sn->content_size().height == 660);
    // Zed's scrollbar: a 14 px strip, the thumb 6 px inside 4 px of padding, its length the
    // viewport's share (200 / 660 of the 192 px track: 58 px), at the top.
    auto* bar = static_cast<scrollbar_node*>(find_type(r.tree(), scrollbar_node::static_type()));
    LUMEN_CHECK(bar && bar->visible());
    const scrollbar_geometry g = bar->geometry();
    LUMEN_CHECK(same(g.strip, {10 + 200 - 14, 10, 14, 200}));
    LUMEN_CHECK(g.thumb.width == 6 && g.thumb.x == 10 + 200 - 10 && g.thumb.y == 14);
    LUMEN_CHECK(g.thumb.height == 58);
    // A long content keeps Zed's 25 px minimum thumb.
    const scrollbar_geometry tiny = scrollbar_layout({0, 0, 100, 100}, 1, {0.f, 100.f, 100000.f});
    LUMEN_CHECK(tiny.visible && tiny.thumb.height == 23); // 25 / 100 of the 92 px track

    // The wheel: the offset moves, nothing is measured or arranged, the viewport repaints.
    ui_node* r3 = find(r.tree(), lx::a11y::role::button, "r3");
    (void)r.pointer_move(centre(*r3));
    LUMEN_CHECK(r.pointer().hovered() == r3);
    LUMEN_CHECK(h.tick());
    ui_layout_counters() = {};
    LUMEN_CHECK(r.pointer_axis(0.f, 44.f));
    LUMEN_CHECK(sn->offset().y == 44);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ui_layout_counters().measures == 0 && ui_layout_counters().arranges == 0);
    const damage_ledger& d = r.frame_damage();
    for (unsigned i = 0; i < d.count(); ++i)
        LUMEN_CHECK(inside(d.rects()[i], vp));
    LUMEN_CHECK(same(d.union_bounds(), vp));
    // What is under the still pointer is the row two buttons on (44 px = two rows).
    ui_node* r5 = find(r.tree(), lx::a11y::role::button, "r5");
    LUMEN_CHECK(r.pointer().hovered() == r5);
    // The hover repaint of a scrolled row lands where it is drawn.
    (void)r.pointer_move(centre(*r3)); // layout coordinates of r3, now showing r5 + 0
    ui_damage().clear();
    (void)r.pointer_move({vp.x + 20, vp.y + 1});
    const lx::rect2i drawn = surface_rect(*r.pointer().hovered(), r.pointer().hovered()->bounds());
    LUMEN_CHECK(inside(drawn, vp) || drawn.y == vp.y);
    LUMEN_CHECK(overlaps(ui_damage().union_bounds(), drawn));
    (void)h.tick();

    // To the end and no further (the ancestors may take what it cannot).
    LUMEN_CHECK(r.pointer_axis(0.f, 10000.f) && sn->offset().y == 460);
    LUMEN_CHECK(!r.pointer_axis(0.f, 10.f));
    (void)h.tick();
    LUMEN_CHECK(bar->geometry().thumb.y + bar->geometry().thumb.height == 10 + 200 - 4);

    // Dragging the thumb: the offset follows, the grab point kept.
    sn->scroll_to(1, 0.f);
    (void)h.tick();
    const lx::rect2i thumb = bar->geometry().thumb;
    (void)r.pointer_move(centre_of(thumb));
    LUMEN_CHECK(r.pointer().hovered() == bar);
    LUMEN_CHECK(r.pointer_button(mouse_button::left, true) && bar->dragging());
    ui_layout_counters() = {};
    (void)r.pointer_move({centre_of(thumb).x, centre_of(thumb).y + 67});
    // 67 px of a 134 px travel (192 − 58): half the 460 px range.
    LUMEN_CHECK(sn->offset().y == 230);
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(!bar->dragging());
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ui_layout_counters().measures == 0 && ui_layout_counters().arranges == 0);
    // A press on the track centres the thumb there.
    (void)r.pointer_move({thumb.x + 3, 10 + 200 - 10});
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(sn->offset().y == 460);
    (void)h.tick();
}

// ── Lists ──

namespace {

int g_list_selected = -2;
int g_list_activated = -1;
void on_list_select(int i) { g_list_selected = i; }
void on_list_activate(int i) { g_list_activated = i; }
unsigned g_list_count = 10000;

child list_row_fn(void*, const list_row& row) {
    return list_item(frame_format("Row {}", row.index)).row(row).name(frame_format("row{}", row.index));
}

child describe_list(build_context&) {
    const child kids[2] = {
        button("before").name("before"),
        list(g_list_count, list_row_fn).layout(style{}.w(px(300.f)).h(px(400.f)))
            .on_select(on_list_select).on_activate(on_list_activate).name("rows"),
    };
    return div(style{}.flex_col().items_start().p(px(10.f)).gap(px(10.f)), kids);
}

[[nodiscard]] list_node& the_list(ui_root& r) {
    ui_node* n = find_type(r.tree(), list_node::static_type());
    LUMEN_CHECK(n != nullptr);
    return *static_cast<list_node*>(n);
}

} // namespace

LUMEN_TEST(list_virtualises_past_100_rows_and_navigates_by_keyboard) {
    set_active_builtin_theme(appearance::dark);
    // 80 rows: every row exists.
    g_list_count = 80;
    {
        harness small{describe_list, {0, 0, 400, 500}};
        list_node& l = the_list(small.root);
        LUMEN_CHECK(!l.virtualised() && l.live_rows() == 80);
        LUMEN_CHECK(l.row_node(79) != nullptr);
    }
    g_list_count = 10000;
    g_list_selected = -2;
    harness h{describe_list, {0, 0, 400, 500}};
    ui_root& r = h.root;
    list_node& l = the_list(r);
    // Zed's dense `ListItem`: a 14 px label on the φ line, 23 px.
    LUMEN_CHECK(l.row_height() == 23 && l.virtualised());
    // 400 / 23 visible rows ± 8, in 16-row chunks: at most 4 chunks.
    const unsigned visible = (400 + 22) / 23;
    std::printf("    10 000 rows: %u live (visible %u), %u nodes in the tree\n", l.live_rows(),
                visible, count_nodes(r.tree()));
    LUMEN_CHECK(l.live_rows() >= visible && l.live_rows() <= 64);
    ui_node* row0 = l.row_node(0);
    LUMEN_CHECK(row0 && row0->bounds().height == 23 && row0->bounds().width == 300);

    // One tab stop; the arrows move the selection, not the focus.
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::tab)));
    LUMEN_CHECK(r.focus().focused() == &l && l.has_flag(node_flag::focus_ring));
    LUMEN_CHECK(r.key(key_press(key::down)) && l.selected() == 0 && g_list_selected == 0);
    LUMEN_CHECK(r.key(key_press(key::down)) && l.selected() == 1 && g_list_selected == 1);
    (void)h.tick();
    // The selection is the row's `ghost_element.selected` background; the ring stays the list's.
    LUMEN_CHECK(ctl(l.row_node(1)).selected() && !ctl(l.row_node(0)).selected());
    LUMEN_CHECK(same_colour(ctl(l.row_node(1)).background(), token(tok::ghost_element_selected)));
    LUMEN_CHECK(!l.row_node(1)->has_flag(node_flag::focus_ring));
    // End: the last row, scrolled into view and in the pool.
    LUMEN_CHECK(r.key(key_press(key::end)) && l.selected() == 9999);
    (void)h.tick();
    LUMEN_CHECK(l.offset() == 10000 * 23 - 400);
    LUMEN_CHECK(l.row_node(9999) != nullptr && l.row_node(0) == nullptr);
    LUMEN_CHECK(l.live_rows() <= 64);
    // Home, page down (17 rows fit), up.
    LUMEN_CHECK(r.key(key_press(key::home)) && l.selected() == 0);
    (void)h.tick();
    LUMEN_CHECK(l.offset() == 0);
    LUMEN_CHECK(r.key(key_press(key::page_down)) && l.selected() == 17);
    LUMEN_CHECK(r.key(key_press(key::up)) && l.selected() == 16);
    // Enter activates the selection.
    g_list_activated = -1;
    LUMEN_CHECK(r.key(key_press(key::enter)) && g_list_activated == 16);
    (void)h.tick();
    // A click selects the row under the pointer and focuses the list without a ring.
    ui_node* row3 = l.row_node(3);
    click(r, centre_of(surface_rect(*row3, row3->bounds())));
    LUMEN_CHECK(l.selected() == 3 && r.focus().focused() == &l && !l.has_flag(node_flag::focus_ring));
    (void)h.tick();
}

LUMEN_TEST(list_hover_damages_two_rows_and_relays_out_nothing) {
    // ADR-010 §8.3 S5: a 1 000-row list, the pointer moving one row per frame.
    g_list_count = 1000;
    harness h{describe_list, {0, 0, 400, 500}};
    ui_root& r = h.root;
    list_node& l = the_list(r);
    (void)r.pointer_move(centre(*l.row_node(0)));
    (void)h.tick();
    std::vector<double> us;
    us.reserve(16);
    unsigned max_rects = 0;
    for (unsigned i = 1; i <= 15; ++i) {
        ui_node* prev = l.row_node(i - 1);
        ui_node* row = l.row_node(i);
        ui_layout_counters() = {};
        LUMEN_CHECK(r.pointer_move(centre(*row)) && r.pointer().hovered() == row);
        const auto t0 = std::chrono::steady_clock::now();
        LUMEN_CHECK(h.tick());
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        const damage_ledger& d = r.frame_damage();
        max_rects = std::max(max_rects, d.count());
        LUMEN_CHECK(d.count() <= 2);
        for (unsigned k = 0; k < d.count(); ++k)
            LUMEN_CHECK(same(d.rects()[k], surface_rect(*prev, prev->bounds())) ||
                        same(d.rects()[k], surface_rect(*row, row->bounds())) ||
                        inside(d.rects()[k], {prev->bounds().x, surface_rect(*prev, prev->bounds()).y,
                                              prev->bounds().width, 46}));
        LUMEN_CHECK(ui_layout_counters().measures == 0 && ui_layout_counters().arranges == 0);
    }
    std::sort(us.begin(), us.end());
    std::printf("    S5 hover: damage <= %u rects, 0 measures, 0 arranges; tick p50 %.1f us p95 %.1f us\n",
                max_rects, us[us.size() / 2], us[us.size() * 95 / 100]);
    // Scrolled, the same holds: the rows are found where they are drawn.
    LUMEN_CHECK(r.pointer_axis(0.f, 230.f));
    (void)h.tick();
    ui_node* under = r.pointer().hovered();
    LUMEN_CHECK(under != nullptr);
    const lx::rect2i drawn = surface_rect(*under, under->bounds());
    LUMEN_CHECK(drawn.contains(r.pointer().position()));
}

LUMEN_TEST(list_scroll_recycles_a_fixed_pool) {
    g_list_count = 10000;
    harness h{describe_list, {0, 0, 400, 500}};
    ui_root& r = h.root;
    list_node& l = the_list(r);
    (void)r.pointer_move(centre(*l.row_node(0)));
    (void)h.tick();
    const unsigned nodes = count_nodes(r.tree());
    // Warm-up: down 600 rows and back, 2 rows a step (the rows' text shaped, the pool full).
    for (int i = 0; i < 300; ++i) {
        (void)r.pointer_axis(0.f, 46.f);
        (void)h.tick();
    }
    for (int i = 0; i < 300; ++i) {
        (void)r.pointer_axis(0.f, -46.f);
        (void)h.tick();
    }
    LUMEN_CHECK(l.offset() == 0);
    LUMEN_CHECK(count_nodes(r.tree()) == nodes);
    const std::uint64_t rebuilds = l.rebuilds();
    const unsigned long long allocs = g_allocations.load();
    const std::size_t heap = mallinfo2().uordblks;
    std::vector<double> us;
    us.reserve(300);
    const unsigned long long allocs_reserved = g_allocations.load();
    unsigned max_live = 0;
    for (int i = 0; i < 300; ++i) {
        LUMEN_CHECK(r.pointer_axis(0.f, 46.f));
        const auto t0 = std::chrono::steady_clock::now();
        LUMEN_CHECK(h.tick());
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        max_live = std::max(max_live, l.live_rows());
        LUMEN_CHECK(l.row_node(static_cast<unsigned>(l.offset() / 23)) != nullptr);
    }
    const unsigned long long during = g_allocations.load() - allocs_reserved;
    (void)allocs;
    // The same nodes throughout: chunks re-described in place, nothing constructed.
    LUMEN_CHECK(count_nodes(r.tree()) == nodes);
    LUMEN_CHECK(l.rebuilds() > rebuilds);
    LUMEN_CHECK(max_live <= 64);
    std::sort(us.begin(), us.end());
    std::printf("    list scroll (2 rows/frame, %llu rebuilds): tick p50 %.1f us p95 %.1f us, %llu allocations, %u live rows max\n",
                static_cast<unsigned long long>(l.rebuilds() - rebuilds), us[us.size() / 2],
                us[us.size() * 95 / 100], during, max_live);
    LUMEN_CHECK(during == 0);
    LUMEN_CHECK(mallinfo2().uordblks <= heap + 300 * sizeof(double) + 64);
}

// ── Text input ──

namespace {

char g_text_storage[64];
text_buffer g_text{g_text_storage};
int g_text_changes = 0;
int g_text_submitted = -1;
void on_text_change(int) { ++g_text_changes; }
void on_text_submit(int n) { g_text_submitted = n; }
std::uint32_t g_text_max = 63;
lx::runtime::overflow_action g_text_overflow = lx::runtime::overflow_action::reject;

child describe_input(build_context&) {
    const child kids[2] = {
        text_input().buffer(g_text).placeholder("Search").name("search")
            .layout(style{}.w(px(240.f))).max_bytes(g_text_max).overflow(g_text_overflow)
            .on_change(on_text_change).on_submit(on_text_submit),
        button("after").name("after"),
    };
    return div(style{}.flex_col().items_start().p(px(10.f)).gap(px(8.f)), kids);
}

[[nodiscard]] text_input_node& the_input(ui_root& r) {
    ui_node* n = find_type(r.tree(), text_input_node::static_type());
    LUMEN_CHECK(n != nullptr);
    return *static_cast<text_input_node*>(n);
}

[[nodiscard]] bool text_is(const text_input_node& t, std::string_view s) { return t.text() == s; }

} // namespace

LUMEN_TEST(text_input_edits_by_grapheme_and_word) {
    set_active_builtin_theme(appearance::dark);
    (void)g_text.assign("");
    g_text_max = 63;
    g_text_overflow = lx::runtime::overflow_action::reject;
    harness h{describe_input, {0, 0, 400, 200}};
    ui_root& r = h.root;
    text_input_node& t = the_input(r);
    // Zed's `InputField`: 33 px (the 19 px standard line, 6 px padding, 1 px border).
    LUMEN_CHECK(t.bounds().height == 33 && t.bounds().width == 240);
    LUMEN_CHECK(t.text_node().layout().text() == "Search"); // the placeholder, muted
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.focus().focused() == &t);
    LUMEN_CHECK(r.ime() == static_cast<ime_target*>(&t));
    type_text(r, "hello");
    LUMEN_CHECK(text_is(t, "hello") && t.caret() == 5 && g_text.size == 5);
    LUMEN_CHECK(g_text_changes >= 5);
    (void)h.tick();
    LUMEN_CHECK(t.text_node().layout().text() == "hello");
    // Arrows, shift-selection, backspace over the selection.
    LUMEN_CHECK(r.key(key_press(key::left)) && r.key(key_press(key::left)) && t.caret() == 3);
    LUMEN_CHECK(r.key(key_press(key::left, k_mod_shift)) && t.anchor() == 3 && t.caret() == 2);
    LUMEN_CHECK(r.key(key_press(key::backspace)) && text_is(t, "helo") && t.caret() == 2);
    LUMEN_CHECK(r.key(key_press(key::delete_forward)) && text_is(t, "heo"));
    LUMEN_CHECK(r.key(key_press(key::end)) && t.caret() == 3);
    type_text(r, " big world");
    // Word jumps: back over "world", then "big"; forward to the end of "big".
    LUMEN_CHECK(r.key(key_press(key::left, k_mod_ctrl)) && t.caret() == 8);
    LUMEN_CHECK(r.key(key_press(key::left, k_mod_ctrl)) && t.caret() == 4);
    LUMEN_CHECK(r.key(key_press(key::right, k_mod_ctrl)) && t.caret() == 7);
    LUMEN_CHECK(r.key(key_press(key::backspace, k_mod_ctrl)) && text_is(t, "heo  world") && t.caret() == 4);
    LUMEN_CHECK(r.key(key_press(key::home, k_mod_shift)) && t.selection_begin() == 0 && t.selection_end() == 4);
    LUMEN_CHECK(r.key(key_press(key::right)) && t.caret() == 4 && !t.has_selection());
    // Clipboard: ctrl+a, ctrl+c, ctrl+x, ctrl+v.
    LUMEN_CHECK(r.key(text_key('a', k_mod_ctrl)) && t.selection_end() == 10 && t.selection_begin() == 0);
    LUMEN_CHECK(r.key(text_key('c', k_mod_ctrl)) && std::strcmp(clipboard::global().text(), "heo  world") == 0);
    LUMEN_CHECK(r.key(text_key('x', k_mod_ctrl)) && text_is(t, "") && t.caret() == 0);
    LUMEN_CHECK(r.key(text_key('v', k_mod_ctrl)) && r.key(text_key('v', k_mod_ctrl)));
    LUMEN_CHECK(text_is(t, "heo  worldheo  world"));
    // Enter submits.
    LUMEN_CHECK(r.key(key_press(key::enter)) && g_text_submitted == 20);
    (void)h.tick();

    // The capacity: reject refuses an insertion whole; drop_newest keeps what fits.
    (void)g_text.assign("");
    g_text_max = 8;
    redescribe(h);
    type_text(r, "12345678");
    const std::uint64_t refused = t.rejected();
    LUMEN_CHECK(r.key(text_key('9')) && text_is(t, "12345678") && t.rejected() == refused + 1);
    (void)g_text.assign("1234567");
    g_text_overflow = lx::runtime::overflow_action::drop_newest;
    redescribe(h);
    clipboard::global().set_text("abc");
    LUMEN_CHECK(r.key(text_key('v', k_mod_ctrl)) && text_is(t, "1234567a"));
    g_text_max = 63;
    g_text_overflow = lx::runtime::overflow_action::reject;

    // A ZWJ emoji sequence is one grapheme: one step, one backspace.
    const char* family = "a\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" "b";
    LUMEN_CHECK(g_text.assign(family));
    redescribe(h);
    LUMEN_CHECK(t.caret() == 20); // the app replaced the text: the caret at its end
    LUMEN_CHECK(r.key(key_press(key::home)) && r.key(key_press(key::right)) && t.caret() == 1);
    LUMEN_CHECK(r.key(key_press(key::right)) && t.caret() == 19);
    LUMEN_CHECK(r.key(key_press(key::right)) && t.caret() == 20);
    LUMEN_CHECK(r.key(key_press(key::left)) && r.key(key_press(key::backspace)) && text_is(t, "ab"));
    LUMEN_CHECK(t.caret() == 1);
    (void)h.tick();

    // Right to left: the caret starts at the right edge; left moves forward (leftwards).
    const char* arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7"; // مرحبا
    LUMEN_CHECK(g_text.assign(arabic));
    redescribe(h);
    LUMEN_CHECK(t.text_node().layout().direction() == lx::text::text_direction::rtl);
    LUMEN_CHECK(r.key(key_press(key::home)) && t.caret() == 0);
    (void)h.tick();
    const int x0 = t.caret_rect().x;
    LUMEN_CHECK(x0 > t.text_area().x + t.text_area().width / 2); // the start of an RTL line
    LUMEN_CHECK(r.key(key_press(key::left)) && t.caret() == 2);
    const int x1 = t.caret_rect().x;
    LUMEN_CHECK(r.key(key_press(key::left)) && t.caret() == 4);
    const int x2 = t.caret_rect().x;
    std::printf("    rtl caret x: %d -> %d -> %d\n", x0, x1, x2);
    LUMEN_CHECK(x1 < x0 && x2 < x1);
    LUMEN_CHECK(r.key(key_press(key::right)) && t.caret() == 2 && t.caret_rect().x == x1);
    (void)h.tick();

    // The pointer: a press places the caret (hit testing), a drag selects, a double press
    // selects a word.
    LUMEN_CHECK(g_text.assign("hello world"));
    redescribe(h);
    const lx::rect2i area = t.text_area();
    const int mid = area.y + area.height / 2;
    (void)r.pointer_move({area.x + 1, mid});
    (void)r.pointer_button(mouse_button::left, true);
    LUMEN_CHECK(t.caret() == 0);
    (void)r.pointer_move({area.x + area.width - 2, mid});
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(t.anchor() == 0 && t.caret() == 11);
    (void)h.tick();
    // A double click on "world".
    const lx::text::caret_pos w = t.text_node().layout().caret(8);
    const lx::point2i on_world{t.text_node().bounds().x + ((w.x + 32) >> 6), mid};
    click(r, on_world);
    click(r, on_world);
    LUMEN_CHECK(t.selection_begin() == 6 && t.selection_end() == 11);
    (void)h.tick();
}

LUMEN_TEST(text_input_ime_preedit_and_commit) {
    (void)g_text.assign("ab");
    g_text_max = 63;
    harness h{describe_input, {0, 0, 400, 200}};
    ui_root& r = h.root;
    text_input_node& t = the_input(r);
    LUMEN_CHECK(r.key(key_press(key::tab)));
    LUMEN_CHECK(r.key(key_press(key::home)) && r.key(key_press(key::right)) && t.caret() == 1);
    ime_target* ime = r.ime();
    LUMEN_CHECK(ime == static_cast<ime_target*>(&t));
    // A preedit: shown at the caret, underlined, not part of the text.
    ime_update u{};
    u.preedit = "\xE3\x81\xAB\xE3\x81\xBB"; // にほ
    u.preedit_cursor_begin = u.preedit_cursor_end = 6;
    ime->ime_apply(u);
    LUMEN_CHECK(text_is(t, "ab") && t.preedit() == u.preedit && t.caret() == 1);
    (void)h.tick();
    const lx::text::text_layout& l = t.text_node().layout();
    LUMEN_CHECK(l.text() == "a\xE3\x81\xAB\xE3\x81\xBB" "b");
    LUMEN_CHECK(l.attrs().size() == 1 && l.attrs()[0].begin == 1 && l.attrs()[0].end == 7 &&
                l.attrs()[0].underline == lx::text::text_underline::single);
    // The caret is drawn at the preedit's cursor (after it), and the state reports the text.
    LUMEN_CHECK(t.caret_visible());
    const ime_state s = ime->ime_query();
    LUMEN_CHECK(s.enabled && s.surrounding == "ab" && s.cursor == 1 && s.cursor_rect.width == 2);
    LUMEN_CHECK(s.cursor_rect.x > t.text_area().x + 8);
    // Keys belong to the input method while it composes.
    LUMEN_CHECK(!r.key(key_press(key::left)) && t.caret() == 1);
    // Commit: the preedit goes, the commit is inserted.
    ime_update c{};
    c.commit = "\xE6\x97\xA5\xE6\x9C\xAC"; // 日本
    ime->ime_apply(c);
    LUMEN_CHECK(text_is(t, "a\xE6\x97\xA5\xE6\x9C\xAC" "b") && t.caret() == 7 && t.preedit().empty());
    // delete_surrounding_text then commit, in the protocol's order.
    ime_update d{};
    d.delete_before = 3;
    d.commit = "x";
    ime->ime_apply(d);
    LUMEN_CHECK(text_is(t, "a\xE6\x97\xA5" "xb") && t.caret() == 5);
    (void)h.tick();
    // A reset drops the preedit; so does losing the focus.
    u.preedit_cursor_begin = u.preedit_cursor_end = -1;
    ime->ime_apply(u);
    LUMEN_CHECK(!t.preedit().empty() && !t.caret_visible());
    ime->ime_reset();
    LUMEN_CHECK(t.preedit().empty() && text_is(t, "a\xE6\x97\xA5" "xb"));
    ime->ime_apply(u);
    LUMEN_CHECK(r.key(key_press(key::tab)) && t.preedit().empty());
    LUMEN_CHECK(r.ime() == nullptr);
    // An over-long preedit is cut to its capacity at a character boundary.
    LUMEN_CHECK(r.key(key_press(key::tab, k_mod_shift)) && r.ime() != nullptr);
    std::string long_preedit;
    for (int i = 0; i < 120; ++i)
        long_preedit += "\xE3\x81\xAB";
    ime_update big{};
    big.preedit = long_preedit;
    ime->ime_apply(big);
    LUMEN_CHECK(t.preedit().size() == 255 && t.preedit().size() % 3 == 0);
    ime->ime_reset();
    (void)h.tick();
}

// ── Timers: the caret blink, the tooltip, toasts ──

namespace {

int g_tip_press = 0;
void on_tip_press() { ++g_tip_press; }

child describe_tip(build_context&) {
    const child kids[2] = {
        button("Save").name("save").tooltip("Save the file").on_press(on_tip_press),
        text_input().placeholder("Name").name("field").initial("abc"),
    };
    return div(style{}.flex_row().items_center().gap(px(20.f)).p(px(20.f)), kids);
}

} // namespace

LUMEN_TEST(caret_blink_and_tooltip_request_frames_only_while_needed) {
    set_ui_clock(test_clock);
    set_reduced_motion(false);
    harness h{describe_tip, {0, 0, 500, 200}};
    ui_root& r = h.root;
    LUMEN_CHECK(!h.tick() && r.next_deadline_ns() == 0);
    ui_node* save = find(r.tree(), lx::a11y::role::button, "save");
    text_input_node& field = the_input(r);

    // Focused: the caret shows and a blink is due in 500 ms; no frames until then.
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.key(key_press(key::tab)));
    LUMEN_CHECK(r.focus().focused() == &field && field.caret_visible());
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 500'000'000ull);
    LUMEN_CHECK(h.tick());
    for (int i = 0; i < 4; ++i) {
        g_now_ns += 100'000'000ull;
        LUMEN_CHECK(!h.tick());
    }
    g_now_ns += 100'000'000ull;
    LUMEN_CHECK(h.tick() && !field.caret_visible());
    // The blink repaints the caret alone.
    LUMEN_CHECK(r.frame_damage().union_bounds().width <= 4);
    g_now_ns += 500'000'000ull;
    LUMEN_CHECK(h.tick() && field.caret_visible());
    // Typing shows the caret solid and restarts the delay.
    LUMEN_CHECK(r.key(text_key('d')) && field.caret_visible());
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 500'000'000ull);
    LUMEN_CHECK(h.tick());
    // Unfocused: the blink stops — no deadline, no frames.
    LUMEN_CHECK(r.key(key_press(key::tab)) && r.focus().focused() != &field);
    LUMEN_CHECK(r.next_deadline_ns() == 0);
    LUMEN_CHECK(h.tick());
    g_now_ns += 2'000'000'000ull;
    LUMEN_CHECK(!h.tick());

    // A tooltip: 500 ms after the hover, at the pointer, fading in over 100 ms.
    r.focus().blur();
    (void)h.tick();
    const lx::point2i at = centre(*save);
    LUMEN_CHECK(r.pointer_move(at));
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 500'000'000ull);
    LUMEN_CHECK(h.tick());
    g_now_ns += 250'000'000ull;
    LUMEN_CHECK(!h.tick() && ui_overlays().open_count() == 0);
    g_now_ns += 250'000'000ull;
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ui_overlays().open_count() == 1 && ui_overlays().top()->kind() == overlay_kind::tooltip);
    const lx::rect2i tip = ui_overlays().top()->content()->bounds();
    LUMEN_CHECK(tip.x == at.x + 8 && tip.y == at.y + 10);
    unsigned frames = 0;
    while (ui_damage().needs_animate() && frames < 100) {
        g_now_ns += 16'666'667ull;
        LUMEN_CHECK(h.tick());
        ++frames;
    }
    std::printf("    tooltip fade: %u frames\n", frames);
    LUMEN_CHECK(frames >= 5 && frames <= 7);
    LUMEN_CHECK(!h.tick() && r.next_deadline_ns() == 0);
    // The pointer passes through it; a press closes it (and presses the button).
    LUMEN_CHECK(r.pointer().hovered() == save);
    (void)r.pointer_button(mouse_button::left, true);
    LUMEN_CHECK(ui_overlays().open_count() == 0);
    (void)r.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(g_tip_press == 1);
    (void)h.tick();
    // Leaving before the delay: no tooltip, no deadline.
    r.pointer_leave();
    (void)h.tick();
    LUMEN_CHECK(r.pointer_move(at) && r.next_deadline_ns() != 0);
    r.pointer_leave();
    LUMEN_CHECK(r.next_deadline_ns() == 0);
    (void)h.tick();
    g_now_ns += 1'000'000'000ull;
    LUMEN_CHECK(!h.tick() && ui_overlays().open_count() == 0);
    set_ui_clock(nullptr);
}

namespace {

bool g_toast_alive[2] = {true, true};
int g_toast_dismissed[2] = {};
void on_toast_dismiss(int i) {
    g_toast_alive[i] = false;
    ++g_toast_dismissed[i];
}

child describe_toasts(build_context&) {
    child toasts[2]{};
    unsigned n = 0;
    if (g_toast_alive[0])
        toasts[n++] = toast("Saved 3 files").status(status_kind::success).icon(icon::check)
                          .value(0).on_dismiss(on_toast_dismiss).key(1);
    if (g_toast_alive[1])
        toasts[n++] = toast("Push failed").status(status_kind::error).icon(icon::x)
                          .duration_ms(4000).value(1).on_dismiss(on_toast_dismiss).key(2);
    const child kids[2] = {label("content"), toast_stack({toasts, n})};
    return div(style{}.size_full().p(px(10.f)), kids);
}

} // namespace

LUMEN_TEST(toasts_stack_and_dismiss_themselves) {
    set_ui_clock(test_clock);
    set_reduced_motion(true);
    g_toast_alive[0] = g_toast_alive[1] = true;
    g_toast_dismissed[0] = g_toast_dismissed[1] = 0;
    harness h{describe_toasts, {0, 0, 500, 300}};
    ui_root& r = h.root;
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    overlay_node* layer = ui_overlays().top();
    auto* first = static_cast<toast_node*>(find_type(layer->content(), toast_node::static_type()));
    ui_node* second = layer->content()->child_at(1);
    LUMEN_CHECK(first && second);
    // Stacked at the bottom centre, 8 px apart, 8 px above the window's edge.
    const lx::rect2i a = first->bounds();
    const lx::rect2i b = second->bounds();
    print_rect("toast 1", a);
    print_rect("toast 2", b);
    LUMEN_CHECK(b.y == a.y + a.height + 8 && b.y + b.height == 300 - 8);
    LUMEN_CHECK(std::abs((a.x + a.width / 2) - 250) <= 1 && std::abs((b.x + b.width / 2) - 250) <= 1);
    // The earliest timer is the 4 s toast; the stack lays out again when it goes.
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 4'000'000'000ull);
    g_now_ns += 4'000'000'000ull;
    LUMEN_CHECK(h.tick() && g_toast_dismissed[1] == 1 && !g_toast_alive[1]);
    redescribe(h);
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    // Hovering pauses the other: past its 10 s it stays while the pointer is on it.
    first = static_cast<toast_node*>(find_type(ui_overlays().top()->content(), toast_node::static_type()));
    LUMEN_CHECK(r.pointer_move(centre(*first)) && r.pointer().hovered() == first);
    g_now_ns += 20'000'000'000ull;
    (void)h.tick();
    LUMEN_CHECK(g_toast_alive[0]);
    // Left, it resumes with what was left (6 s).
    r.pointer_leave();
    LUMEN_CHECK(r.next_deadline_ns() == g_now_ns + 6'000'000'000ull);
    g_now_ns += 6'000'000'000ull;
    (void)h.tick();
    LUMEN_CHECK(!g_toast_alive[0] && g_toast_dismissed[0] == 1);
    redescribe(h);
    // Nothing left: the layer closes, no timer, no frames.
    LUMEN_CHECK(ui_overlays().open_count() == 0 && r.next_deadline_ns() == 0);
    g_now_ns += 1'000'000'000ull;
    LUMEN_CHECK(!h.tick());
    set_reduced_motion(false);
    set_ui_clock(nullptr);
}

// ── G5: steady frames with overlays, lists and inputs allocate nothing ──

LUMEN_TEST(steady_frames_with_overlays_allocate_nothing) {
    set_active_builtin_theme(appearance::dark);
    set_reduced_motion(true);
    g_menu_open = true;
    g_trigger_at = {20, 20};
    harness h{describe_menu, {0, 0, 400, 300}};
    ui_root& r = h.root;
    LUMEN_CHECK(ui_overlays().open_count() == 1);
    ui_node* copy = menu_item_node(r, "Copy");
    ui_node* paste = menu_item_node(r, "Paste");
    for (int i = 0; i < 4; ++i) {
        ui_damage().add({0, 0, 400, 300});
        (void)r.pointer_move(centre(i % 2 ? *copy : *paste));
        (void)r.key(key_press(key::down));
        LUMEN_CHECK(h.tick());
    }
    // Idle with the menu open: no frame at all.
    LUMEN_CHECK(!h.tick());
    const auto run = [&](const char* what, auto&& step) {
        const std::size_t heap = mallinfo2().uordblks;
        std::vector<double> us;
        us.reserve(300);
        const unsigned long long before = g_allocations.load();
        for (int i = 0; i < 300; ++i) {
            step(i);
            const auto t0 = std::chrono::steady_clock::now();
            (void)h.tick();
            us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        }
        const unsigned long long during = g_allocations.load() - before;
        LUMEN_CHECK(during == 0);
        LUMEN_CHECK(mallinfo2().uordblks <= heap + 300 * sizeof(double) + 64);
        std::sort(us.begin(), us.end());
        std::printf("    %-30s tick p50 %7.1f us  p95 %7.1f us  allocations %llu\n", what,
                    us[us.size() / 2], us[us.size() * 95 / 100], during);
    };
    run("menu open, full repaint", [&](int) { ui_damage().add({0, 0, 400, 300}); });
    run("menu open, hover", [&](int i) { (void)r.pointer_move(centre(i % 2 ? *copy : *paste)); });
    run("menu open, arrow keys", [&](int) { (void)r.key(key_press(key::down)); });
    g_menu_open = false;
    redescribe(h);
    set_reduced_motion(false);

    // Typing into a field and deleting it again.
    (void)g_text.assign("");
    harness hi{describe_input, {0, 0, 400, 200}};
    LUMEN_CHECK(hi.root.key(key_press(key::tab)));
    for (int i = 0; i < 4; ++i) {
        type_text(hi.root, "lumen ");
        for (int k = 0; k < 6; ++k)
            (void)hi.root.key(key_press(key::backspace));
        (void)hi.tick();
    }
    const unsigned long long before = g_allocations.load();
    for (int i = 0; i < 100; ++i) {
        (void)hi.root.key(text_key(static_cast<char32_t>('a' + i % 6)));
        (void)hi.tick();
        (void)hi.root.key(key_press(key::backspace));
        (void)hi.tick();
    }
    const unsigned long long typing = g_allocations.load() - before;
    std::printf("    typing + backspace, 200 frames: %llu allocations\n", typing);
    LUMEN_CHECK(typing == 0);
}

// ── Damage-scoped emission (`emit_scope::damage`, ADR-010 §8.3 S2) ──

struct ticker_state {
    unsigned n = 0;
};
struct ticker_action {
    unsigned n = 0;
};
using ticker_host = reducer_node<ticker_state, ticker_action>;
void ticker_reduce(ticker_state& s, const ticker_action& a) { s.n = a.n; }
child ticker_build(const ticker_state& s, ticker_host&) {
    return label(frame_format("{:02}:{:02}", s.n / 60, s.n % 60)).single_line();
}
constexpr element_key k_ticker_key = 77;

/// A panel-like row: clipping sections of buttons, icons and labels either side of a
/// reducer's label, all three over a background.
child describe_damage_scope(build_context&) {
    const style row = style{}.flex_row().items_center();
    const child left[3] = {button("Files").icon(icon::folder), icon_button(icon::settings).tooltip("Settings"),
                           label("left").color(tok::text_muted)};
    const child ticker = reducer<ticker_state, ticker_action>({}, ticker_reduce, ticker_build, k_ticker_key);
    const child right[3] = {progress_bar(30.f, 100.f), label("right"), icon_button(icon::bell)};
    const child kids[3] = {
        div(style{row}.flex_1().gap(sp::b04).overflow_hidden(), left),
        div(style{row}.w(px(64.f)).justify_center(), {&ticker, 1u}),
        div(style{row}.flex_1().gap(sp::b04).justify_end().overflow_hidden(), right),
    };
    return div(style{row}.size_full().px(sp::b08).bg(tok::title_bar_background).border_b_1(tok::border), kids);
}

/// XRGB8888 pixels of a `w` × `h` target.
struct pixels {
    int w = 0;
    int h = 0;
    std::vector<std::uint32_t> px;
    pixels(int width, int height) : w{width}, h{height}, px(static_cast<std::size_t>(width) * height, 0u) {}
    [[nodiscard]] lx::gfx::pixel_surface surface() {
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(px.data());
        s.width = static_cast<unsigned>(w);
        s.height = static_cast<unsigned>(h);
        s.stride = static_cast<unsigned>(w) * 4u;
        s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
        return s;
    }
};

[[nodiscard]] ticker_host* find_ticker(ui_node* n) {
    if (!n)
        return nullptr;
    if (n->key() == k_ticker_key && n->type() == ticker_host::static_type())
        return static_cast<ticker_host*>(n);
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ticker_host* hit = find_ticker(n->child_at(i)))
            return hit;
    return nullptr;
}

void raster(const lx::scene::draw_list& list, pixels& into, const lx::gfx::damage_region& damage) {
    lx::gfx::ui_stream st{};
    st.refs = list.refs();
    st.batches = list.batches();
    st.quads = list.quad_instances();
    st.shadows = list.shadow_instances();
    st.sprites = list.sprite_instances();
    st.underlines = list.underline_instances();
    lx::gfx::ui_cpu_renderer r{};
    LUMEN_CHECK(r.draw(into.surface(), st, &env().atlas, damage, lx::color::rgb(0.f, 0.f, 0.f)).ok());
}

LUMEN_TEST(damage_scoped_emit_repaints_the_damage_exactly) {
    set_active_builtin_theme(appearance::dark);
    const lx::rect2i bounds{0, 0, 360, 36};
    harness h{describe_damage_scope, bounds};
    h.root.set_emit_scope(emit_scope::damage);
    // The first frame carried no damage rects: everything was emitted.
    h.list->batch_for_render();
    const unsigned full = h.list->primitive_count();
    LUMEN_CHECK(full > 10);
    lx::gfx::damage_region all{};
    all.set_full();
    pixels kept{bounds.width, bounds.height};
    raster(*h.list, kept, all);

    ticker_host* host = find_ticker(h.root.tree());
    LUMEN_CHECK(host != nullptr);
    if (!host)
        return;
    for (unsigned step = 1; step <= 3; ++step) {
        host->dispatch({step * 61});
        LUMEN_CHECK(h.tick());
        const damage_ledger& d = h.root.frame_damage();
        LUMEN_CHECK(d.count() == 1);
        const lx::rect2i u = d.union_bounds();
        LUMEN_CHECK(u.width > 0 && u.width < 64 && u.height <= bounds.height);
        h.list->batch_for_render();
        // Only what touches the damage: the background, the bar's border and the label.
        LUMEN_CHECK(h.list->primitive_count() < full / 2);
        for (const lx::gfx::quad_instance& q : h.list->quad_instances()) {
            LUMEN_CHECK(q.bounds[0] < u.x + u.width && q.bounds[0] + q.bounds[2] > u.x);
        }
        // The kept pixels with the damage repainted from the scoped list are the pixels of
        // the whole tree drawn again.
        lx::gfx::damage_region region{};
        region.add(u);
        raster(*h.list, kept, region);
        harness fresh{describe_damage_scope, bounds};
        ticker_host* fh = find_ticker(fresh.root.tree());
        LUMEN_CHECK(fh != nullptr);
        if (fh) {
            fh->dispatch({step * 61});
            (void)fresh.tick();
            ui_damage().add(bounds); // a full frame
            LUMEN_CHECK(fresh.tick());
        }
        fresh.list->batch_for_render();
        pixels whole{bounds.width, bounds.height};
        raster(*fresh.list, whole, all);
        LUMEN_CHECK(whole.px == kept.px);
    }
}

LUMEN_TEST(frame_format_formats_long_text_into_the_arena) {
    lx::runtime::memory_arena arena{4096};
    build_context ctx{arena};
    build_scope scope{ctx};
    const std::string long_text(300, 'x');
    const char* s = frame_format("[{}] {}", 42, long_text);
    LUMEN_CHECK(std::strlen(s) == 5 + 300);
    LUMEN_CHECK(std::strncmp(s, "[42] xxx", 8) == 0);
    LUMEN_CHECK(std::strcmp(frame_format("{:02}:{:02}", 9, 41), "09:41") == 0);
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    set_ui_resources({});
    return status;
}

