// The chrome layer (ui-architecture §9's design-system item): the palette's named values
// are the ones the canvas views drew, and the components lay out.

#include "lumen_test.hpp"

import std;

import lx.foundation;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.ui;

namespace {

struct ui_harness {
    lx::runtime::memory_arena arena{256 * 1024};
    lx::ui::ui_root root{arena};
    lx::scene::draw_list draws{};

    ui_harness(lx::ui::describe_fn describe, lx::rect2i bounds) {
        lx::ui::ui_damage().clear();
        root.set_describer(describe);
        root.set_bounds(bounds);
        (void)root.tick(draws);
    }
    [[nodiscard]] lx::ui::ui_node& node() const { return *root.tree(); }
};

[[nodiscard]] bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct describe_toolbar_t {
    static lx::ui::child build(lx::ui::build_context&) {
        lx::ui::child a = lx::ui::chrome::row(28, false, {});
        lx::ui::child b = lx::ui::chrome::row(28, true, {});
        lx::ui::child rows[2] = {a, b};
        // The toolbar inside a full-window column: the root takes the window bounds by
        // contract, so the toolbar's own 36 px height shows as its parent's centered item.
        lx::layout::flex_style root_st{};
        root_st.width = lx::layout::percent(100.f);
        root_st.height = lx::layout::percent(100.f);
        root_st.direction = lx::layout::flex_direction::column;
        lx::ui::child toolbar = lx::ui::chrome::toolbar(64, rows);
        return lx::ui::flex(root_st, {&toolbar, 1});
    }
};

} // namespace

LUMEN_TEST(chrome_palette_holds_the_canvas_values) {
    // The exact values the canvas views drew (the smoke suites pin them pixel for pixel):
    // the switcher's background, the selection, the notification card, the menu's text.
    LUMEN_CHECK(lx::ui::chrome::palette.background == 0xFF1E1E22u);
    LUMEN_CHECK(lx::ui::chrome::palette.selected == 0xFF3D6FB4u);
    LUMEN_CHECK(lx::ui::chrome::palette.card == 0xFF24242Au);
    LUMEN_CHECK(lx::ui::chrome::palette.text == 0xFFECECEEu);
    LUMEN_CHECK(lx::ui::chrome::color(0xFF3D6FB4u).b > 0.7f);
}

LUMEN_TEST(chrome_toolbar_and_rows_lay_out) {
    // A 64 px toolbar holding two 28 px rows stretched to its width, stacked from the top
    // (justify: flex-start), the first row inset 4 px by the toolbar's centering.
    ui_harness h{&describe_toolbar_t::build, {0, 0, 300, 100}};
    lx::ui::ui_node& root = h.node();
    LUMEN_CHECK(root.child_count() == 1);
    lx::ui::ui_node& toolbar = *root.child_at(0);
    LUMEN_CHECK(same(toolbar.bounds(), {0, 0, 300, 64}));
    LUMEN_CHECK(toolbar.child_count() == 2);
    LUMEN_CHECK(same(toolbar.child_at(0)->bounds(), {0, 4, 300, 28}));
    LUMEN_CHECK(same(toolbar.child_at(1)->bounds(), {0, 32, 300, 28}));
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
