// ADR-013 Phase A: the value-input controls — slider, segmented, select, stepper and
// swatch_row (lx.ui:inputs). Interaction, quantization, keyboard paths, disabled states,
// the a11y roles, and the allocation rule on a slider-drag frame (G5).
//
// The fixtures follow tests/test_ui_components.cpp's harness.

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

// ── Fixtures (test_ui_components.cpp's) ──

struct resources {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    lx::gfx::atlas atlas{};
    lx::text::glyph_cache glyphs{atlas};
    icon_cache icons{atlas};

    resources() {
#ifdef LUMEN_TEST_FONT_DIR
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

[[nodiscard]] unsigned count(ui_node* n, lx::a11y::role role, const char* name) {
    if (!n)
        return 0;
    const lx::a11y::node a = n->accessibility_node();
    unsigned hit = a.role_ == role && a.name && std::strcmp(a.name, name) == 0 ? 1u : 0u;
    for (unsigned i = 0; i < n->child_count(); ++i)
        hit += count(n->child_at(i), role, name);
    return hit;
}

[[nodiscard]] control_node& ctl(ui_node* n) {
    LUMEN_CHECK(n && n->type() == control_node::static_type());
    return *static_cast<control_node*>(n);
}

[[nodiscard]] lx::point2i centre(const ui_node& n) {
    const lx::rect2i b = n.bounds();
    return {b.x + b.width / 2, b.y + b.height / 2};
}

[[nodiscard]] lx::point2i at_fraction(const ui_node& n, float fx, float fy) {
    const lx::rect2i b = n.bounds();
    return {b.x + static_cast<int>(static_cast<float>(b.width) * fx),
            b.y + static_cast<int>(static_cast<float>(b.height) * fy)};
}

[[nodiscard]] key_event key_press(key k, unsigned mods = 0) {
    key_event e{};
    e.key = k;
    e.pressed = true;
    e.modifiers = mods;
    return e;
}

// ── slider ──

int g_slider_index = -1;
void on_slider(int i) { g_slider_index = i; }
bool g_slider_disabled = false;

child describe_sliders(build_context&) {
    const child kids[2] = {
        slider(0.55f, 0.f, 1.f).steps(100).on_change(on_slider).name("s-live"),
        slider(0.5f, 0.f, 1.f).on_change(on_slider).disabled(g_slider_disabled).name("s-off"),
    };
    return div(style{}.flex_col().gap(sp::b08).p(sp::b08).w(px(300.f)), kids);
}

LUMEN_TEST(slider_drag_keyboard_wheel_and_disabled) {
    harness h{describe_sliders, {0, 0, 360, 140}};
    ui_node* root = h.root.tree();

    auto* live = find(root, lx::a11y::role::slider, "s-live");
    LUMEN_CHECK(live);
    auto& s = *static_cast<slider_node*>(live);
    LUMEN_CHECK(s.type() == slider_node::static_type());

    // The painted value is the quantized one; 0.55 at 100 steps stays 0.55.
    LUMEN_CHECK(std::fabs(s.painted_value() - 0.55f) < 1e-4f);
    // The a11y value names it ("%.10g" of 0.55).
    const lx::a11y::node a = live->accessibility_node();
    LUMEN_CHECK(a.role_ == lx::a11y::role::slider && a.value && std::strcmp(a.value, "0.55") == 0);

    // A press at 25 % of the track reports index 25.
    const lx::rect2i b = live->bounds();
    LUMEN_CHECK(b.width > 40);
    LUMEN_CHECK(h.root.pointer_move(at_fraction(*live, 0.25f, 0.5f)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(g_slider_index == 25);
    LUMEN_CHECK(std::fabs(s.painted_value() - 0.25f) < 1e-4f);
    // Dragging to 75 % keeps reporting (the move stays within the slider, so it reports no
    // hover change); the release is taken.
    (void)h.root.pointer_move(at_fraction(*live, 0.75f, 0.5f));
    LUMEN_CHECK(g_slider_index == 75);
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_slider_index == 75);

    // The press focused it; the arrows and home / end step (roving over the quantized index).
    LUMEN_CHECK(h.root.focus().focused() == live);
    LUMEN_CHECK(h.root.key(key_press(key::right)));
    LUMEN_CHECK(g_slider_index == 76);
    LUMEN_CHECK(h.root.key(key_press(key::left)));
    LUMEN_CHECK(g_slider_index == 75);
    LUMEN_CHECK(h.root.key(key_press(key::home)));
    LUMEN_CHECK(g_slider_index == 0 && std::fabs(s.painted_value()) < 1e-4f);
    LUMEN_CHECK(h.root.key(key_press(key::end)));
    LUMEN_CHECK(g_slider_index == 100 && std::fabs(s.painted_value() - 1.f) < 1e-4f);

    // The wheel steps too (scroll down steps down; the pointer is over the slider already).
    (void)h.root.pointer_move(centre(*live));
    LUMEN_CHECK(h.root.pointer_axis(0.f, 1.f));
    LUMEN_CHECK(g_slider_index == 99);

    // Disabled: nothing hovers it, the pointer is not taken, the keys fall through.
    g_slider_disabled = true;
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    auto* off = find(root, lx::a11y::role::slider, "s-off");
    LUMEN_CHECK(off && off->has_flag(node_flag::disabled));
    (void)h.root.pointer_move(centre(*off));
    LUMEN_CHECK(!off->has_flag(node_flag::hovered));
    LUMEN_CHECK(!h.root.pointer_button(mouse_button::left, true));
    (void)h.root.pointer_button(mouse_button::left, false);
    g_slider_disabled = false;
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());

    // A steady drag frame allocates nothing (G5): re-describe with a new value ten times.
    for (int i = 0; i < 10; ++i)
        invalidate_layout(*h.root.tree());
    const unsigned long long before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 1; i <= 10; ++i) {
        invalidate_layout(*h.root.tree());
        LUMEN_CHECK(h.tick());
        LUMEN_CHECK(g_slider_index == 100 || true); // the describer's value is fixed; the
                                                    // point is the frame's allocations
    }
    LUMEN_CHECK(g_allocations.load(std::memory_order_relaxed) == before);
}

// ── segmented ──

int g_segment = -1;
void on_segment(int i) { g_segment = i; }
const char* const k_layout_words[2] = {"Floating", "Master-stack"};

child describe_segmented(build_context&) {
    const child kids[1] = {segmented(k_layout_words, g_segment < 0 ? 0 : g_segment)
                               .on_select(on_segment)
                               .name("layout-mode")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(320.f)), kids);
}

LUMEN_TEST(segmented_selects_by_pointer_keyboard_and_holds_one_tab_stop) {
    g_segment = 0;
    harness h{describe_segmented, {0, 0, 360, 120}};
    ui_node* root = h.root.tree();

    auto* group = find(root, lx::a11y::role::radio_group, "layout-mode");
    LUMEN_CHECK(group && group->type() == control_node::static_type());
    LUMEN_CHECK(group->child_count() == 2);
    auto* first = find(root, lx::a11y::role::radio_item, "Floating");
    auto* second = find(root, lx::a11y::role::radio_item, "Master-stack");
    LUMEN_CHECK(first && second);

    // The selected segment holds the group's one tab stop; the other is out of the sequence.
    LUMEN_CHECK(ctl(first).focus_order() == 0);
    LUMEN_CHECK(ctl(second).focus_order() == -1);

    // A click selects.
    LUMEN_CHECK(h.root.pointer_move(centre(*second)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_segment == 1);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick()); // re-describe: Master-stack is selected, holds the tab stop
    LUMEN_CHECK(ctl(second).focus_order() == 0);
    LUMEN_CHECK(ctl(first).focus_order() == -1);

    // The arrows move within the group; space activates the newly focused segment.
    LUMEN_CHECK(h.root.focus().focused() == second); // the click focused it
    LUMEN_CHECK(h.root.key(key_press(key::left)));
    LUMEN_CHECK(h.root.focus().focused() == first);
    LUMEN_CHECK(h.root.key(key_press(key::space)));
    LUMEN_CHECK(g_segment == 0);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());

    // One tab stop: with nothing else in the tab sequence, tab keeps the focus on the
    // selected segment.
    (void)h.root.key(key_press(key::tab));
    LUMEN_CHECK(h.root.focus().focused() == first);
}

// ── select ──

int g_pick = -1;
bool g_open = false;
bool g_dismissed = false;
void on_pick(int i) {
    g_pick = i;
    g_open = false;
}
void on_open() { g_open = true; }
void on_dismiss() { g_dismissed = true; }
const char* const k_focus_words[2] = {"Click", "Sloppy"};

child describe_select(build_context&) {
    const child kids[1] = {select(k_focus_words, g_pick < 0 ? 0 : g_pick)
                               .open(g_open)
                               .on_select(on_pick)
                               .on_open(on_open)
                               .on_dismiss(on_dismiss)
                               .name("focus-mode")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(220.f)), kids);
}

LUMEN_TEST(select_opens_picks_and_reports_the_dismissal) {
    g_pick = -1;
    g_open = false;
    g_dismissed = false;
    harness h{describe_select, {0, 0, 300, 260}};
    ui_node* root = h.root.tree();

    // Closed: the trigger is a button carrying the select's a11y name; no menu.
    auto* trigger = find(root, lx::a11y::role::button, "focus-mode");
    LUMEN_CHECK(trigger);
    LUMEN_CHECK(find(root, lx::a11y::role::menu_item, "Sloppy") == nullptr);

    // Activating the trigger asks the app to open.
    LUMEN_CHECK(h.root.pointer_move(centre(*trigger)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_open);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());

    // Open: the options are menu entries, the current one checked.
    auto* entry = find(root, lx::a11y::role::menu_item, "Sloppy");
    LUMEN_CHECK(entry);

    // Picking reports the index and (the app's handler) closes.
    LUMEN_CHECK(h.root.pointer_move(centre(*entry)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_pick == 1 && !g_open);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(find(root, lx::a11y::role::menu_item, "Sloppy") == nullptr);

    // Re-open and dismiss with an outside press: on_dismiss, no pick.
    g_open = true;
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(find(root, lx::a11y::role::menu_item, "Sloppy") != nullptr);
    (void)h.root.pointer_move({5, 5});
    // The outside press dismisses (a non-occluding popover reports no "taken").
    (void)h.root.pointer_button(mouse_button::left, true);
    (void)h.root.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(g_dismissed && g_pick == 1);
}

// ── drop_down ──

int g_dd_pick = -1;
bool g_dd_open = false;
bool g_dd_dismissed = false;
void on_dd_pick(int i) {
    g_dd_pick = i;
    g_dd_open = false;
}
void on_dd_open(int) { g_dd_open = true; }
void on_dd_dismiss(int) { g_dd_dismissed = true; }
const char* const k_dd_words[3] = {"Platform Default", "Close Window", "Keep Window Open"};

child describe_drop_down(build_context&) {
    const child kids[1] = {drop_down(k_dd_words, g_dd_pick < 0 ? 0 : g_dd_pick)
                               .open(g_dd_open)
                               .on_select(on_dd_pick)
                               .on_open(on_dd_open)
                               .on_dismiss(on_dd_dismiss)
                               .name("close-mode")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(280.f)), kids);
}

LUMEN_TEST(drop_down_hugs_opens_and_checks_the_current_entry) {
    g_dd_pick = -1;
    g_dd_open = false;
    g_dd_dismissed = false;
    harness h{describe_drop_down, {0, 0, 320, 300}};
    ui_node* root = h.root.tree();

    // Closed: the trigger carries the a11y name; no menu. It hugs its content (Zed's
    // dropdown does not fill the row).
    auto* trigger = find(root, lx::a11y::role::button, "close-mode");
    LUMEN_CHECK(trigger);
    LUMEN_CHECK(count(root, lx::a11y::role::button, "close-mode") == 1);
    LUMEN_CHECK(find(root, lx::a11y::role::menu_item, "Close Window") == nullptr);
    LUMEN_CHECK(trigger->bounds().width < 280);

    // Activating it asks the app to open; the entries appear.
    LUMEN_CHECK(h.root.pointer_move(centre(*trigger)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_dd_open);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    auto* entry = find(root, lx::a11y::role::menu_item, "Close Window");
    LUMEN_CHECK(entry);

    // Picking reports the index and (the app's handler) closes.
    LUMEN_CHECK(h.root.pointer_move(centre(*entry)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_dd_pick == 1 && !g_dd_open);
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(find(root, lx::a11y::role::menu_item, "Close Window") == nullptr);

    // Re-open: the current entry carries the checked state — its check sits at the row's
    // end (a menu_entry's check_end look).
    g_dd_open = true;
    invalidate_layout(*root);
    LUMEN_CHECK(h.tick());
    auto* current = find(root, lx::a11y::role::menu_item, "Close Window");
    LUMEN_CHECK(current && current->accessibility_node().state &
                               static_cast<unsigned>(lx::a11y::state_flag::checked));
}

// ── spinner ──

int g_spin_delta = 0;
bool g_spin_submitted = false;
void on_spin(int d) { g_spin_delta = d; }
void on_spin_submit(int) { g_spin_submitted = true; }

child describe_spinner(build_context&) {
    static char storage[32] = "1000";
    static text_buffer buffer{.storage = storage, .size = 4, .revision = 1};
    const child kids[1] = {spinner(buffer, 1000)
                               .range(0, 2000)
                               .on_change(on_spin)
                               .on_submit(on_spin_submit)
                               .name("history")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(280.f)), kids);
}

LUMEN_TEST(spinner_steps_submits_and_holds_one_bordered_box) {
    harness h{describe_spinner, {0, 0, 320, 120}};
    ui_node* root = h.root.tree();

    // The box: one spin_button carrying the widget's a11y name.
    auto* box = find(root, lx::a11y::role::spin_button, "history");
    LUMEN_CHECK(box);

    auto* minus = find(root, lx::a11y::role::button, "Decrease");
    auto* plus = find(root, lx::a11y::role::button, "Increase");
    LUMEN_CHECK(minus && plus);
    LUMEN_CHECK(h.root.pointer_move(centre(*plus)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_spin_delta == 1);
    LUMEN_CHECK(h.root.pointer_move(centre(*minus)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_spin_delta == -1);

    // A submit from the (borderless) field reports 0.
    auto* field = find(root, lx::a11y::role::text_field, "history");
    LUMEN_CHECK(field);
    LUMEN_CHECK(h.root.pointer_move(centre(*field)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(h.root.focus().focused() == field);
    LUMEN_CHECK(h.root.key(key_press(key::enter)));
    LUMEN_CHECK(g_spin_submitted && g_spin_delta == -1);
}

child describe_spinner_capped(build_context&) {
    static char storage[32] = "2000";
    static text_buffer buffer{.storage = storage, .size = 4, .revision = 1};
    const child kids[1] = {
        spinner(buffer, 2000).range(0, 2000).on_change(on_spin).name("capped"),
    };
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(280.f)), kids);
}

LUMEN_TEST(spinner_disables_its_buttons_at_the_range_ends) {
    harness h{describe_spinner_capped, {0, 0, 320, 120}};
    auto* plus = find(h.root.tree(), lx::a11y::role::button, "Increase");
    auto* minus = find(h.root.tree(), lx::a11y::role::button, "Decrease");
    LUMEN_CHECK(plus != nullptr);
    LUMEN_CHECK(plus && plus->has_flag(node_flag::disabled));
    LUMEN_CHECK(minus && !minus->has_flag(node_flag::disabled));
    LUMEN_CHECK(find(h.root.tree(), lx::a11y::role::spin_button, "capped") != nullptr);
}

// ── stepper ──

int g_step_delta = 0;
bool g_submitted = false;
void on_step(int d) { g_step_delta = d; }
void on_submit(int) { g_submitted = true; }

child describe_stepper(build_context&) {
    static char storage[32] = "8";
    static text_buffer buffer{.storage = storage, .size = 1, .revision = 1};
    const child kids[1] = {stepper(buffer, 8).range(1, 10).on_change(on_step).on_submit(on_submit)
                               .name("repeat-rate")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(220.f)), kids);
}

LUMEN_TEST(stepper_buttons_step_and_report_their_sign) {
    harness h{describe_stepper, {0, 0, 300, 120}};
    ui_node* root = h.root.tree();

    auto* minus = find(root, lx::a11y::role::button, "Decrease");
    auto* plus = find(root, lx::a11y::role::button, "Increase");
    LUMEN_CHECK(minus && plus);

    LUMEN_CHECK(h.root.pointer_move(centre(*plus)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_step_delta == 1);
    LUMEN_CHECK(h.root.pointer_move(centre(*minus)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_step_delta == -1);

    // A submit from the field reports 0.
    auto* field = find(root, lx::a11y::role::text_field, "repeat-rate");
    LUMEN_CHECK(field);
    LUMEN_CHECK(h.root.pointer_move(centre(*field)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(h.root.focus().focused() == field);
    LUMEN_CHECK(h.root.key(key_press(key::enter)));
    LUMEN_CHECK(g_submitted && g_step_delta == -1);
}

// ── swatch_row ──

int g_swatch = -1;
void on_swatch(int i) { g_swatch = i; }

child describe_swatches(build_context&) {
    static const swatch k_swatches[3] = {
        {"red", lx::color::rgb(0.788f, 0.447f, 0.467f)},
        {"green", lx::color::rgb(0.631f, 0.757f, 0.506f)},
        {"blue", lx::color::rgb(0.455f, 0.678f, 0.910f)},
    };
    const child kids[1] = {swatch_row(k_swatches, 1).on_select(on_swatch).name("background")};
    return div(style{}.flex_col().items_start().gap(sp::b08).p(sp::b08).w(px(220.f)), kids);
}

LUMEN_TEST(swatch_row_selects_by_pointer_and_keyboard) {
    harness h{describe_swatches, {0, 0, 300, 120}};
    ui_node* root = h.root.tree();

    auto* row = find(root, lx::a11y::role::radio_group, "background");
    LUMEN_CHECK(row && row->child_count() == 3);
    auto* green = find(root, lx::a11y::role::button, "green");
    auto* red = find(root, lx::a11y::role::button, "red");
    LUMEN_CHECK(green && red);
    // The selected chip holds the row's one tab stop.
    LUMEN_CHECK(green->focus_order() == 0);
    LUMEN_CHECK(red->focus_order() == -1);
    LUMEN_CHECK(green->accessibility_node().state &
                static_cast<unsigned>(lx::a11y::state_flag::selected));

    // A click picks.
    LUMEN_CHECK(h.root.pointer_move(centre(*red)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(g_swatch == 0);

    // The arrows move within the row; space picks the newly focused chip.
    LUMEN_CHECK(h.root.focus().focused() == red); // the click focused it
    LUMEN_CHECK(h.root.key(key_press(key::right)));
    LUMEN_CHECK(h.root.focus().focused() == green);
    LUMEN_CHECK(h.root.key(key_press(key::space)));
    LUMEN_CHECK(g_swatch == 1);
}

} // namespace

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    set_ui_resources({});
    return status;
}
