// The shell menu's row anatomy in vitro: rows and their cells as absolutely positioned
// boxes (menu_popup's at()-style arithmetic) over lx.ui — the labels must land at their
// rect's width, whatever their text, and the whole list must read top to bottom. The
// in-app symptom this guards: some rows' labels collapsing to slivers while the first
// reads fine.

#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.a11y;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;
import lx.shell;

namespace {

using namespace lx::ui;

/// menu_popup's at(): an absolutely placed box of exactly this rect.
[[nodiscard]] style at(lx::rect2i r) {
    return style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[maybe_unused]] int g_labels = 0; // the labels the damaged popup would have drawn
const char* const g_words[4] = {"Do not disturb", "Clear all", "No notifications",
                                "14:32  org.lumen.Settings: saved"};

child describe_menu_rows(build_context&) {
    // The menu: 200 wide, 26-px rows at 4-px padding, a label from x 28 to ~row width.
    constexpr int w = 200, row_h = 26, pad = 4;
    const auto label_at = [&](int /*row*/, const char* text) {
        text_block_props t{};
        t.text = text;
        t.font = lx::shell::menu_font();
        t.wrap = lx::text::text_wrap::none;
        t.ellipsize = true;
        t.context = active_ui_resources().text;
        const child inside[1] = {text_block(t)};
        control_props box{};
        box.layout = at({28, 0, w - pad - 28 - 8, row_h}).flex_row().items_center()
                         .overflow_hidden();
        return control(box, inside);
    };
    child rows[4];
    for (unsigned i = 0; i < 4; ++i) {
        const child cells[1] = {label_at(i * (row_h + 2), g_words[i])};
        control_props row{};
        row.layout = at({pad, static_cast<int>(i) * (row_h + 2), w - 2 * pad, row_h})
                         .flex_row().items_center().overflow_hidden();
        rows[i] = control(row, cells);
    }
    control_props list{};
    list.layout = style{}.flex_col().relative().w(px(static_cast<float>(w)))
                         .h(px(static_cast<float>(4 * (row_h + 2) + 2 * pad)));
    const child kids[4] = {rows[0], rows[1], rows[2], rows[3]};
    return control(list, kids);
}

LUMEN_TEST(popup_font_uses_active_theme_family_and_ui_scale) {
    const lx::text::font_desc font = lx::shell::menu_font();
    const auto& themed = active_theme().fonts();
    LUMEN_CHECK((font.family == nullptr && themed.ui_family[0] == '\0') ||
                (font.family && std::strcmp(font.family, themed.ui_family) == 0));
    LUMEN_CHECK(font.size_pt ==
                text_size_px(text_size::ui, active_theme(), ui_rem_px()) * 0.75f);
}

LUMEN_TEST(menu_row_labels_keep_their_rect_widths) {
    lx::runtime::memory_arena arena{256 * 1024};
    ui_root root{arena};
    root.set_describer(&describe_menu_rows);
    root.set_bounds({0, 0, 200, 124});
    auto list = std::make_unique<lx::scene::draw_list>();
    LUMEN_CHECK(root.tick(*list));

    // Every label cell sits at its absolute rect: x 28+4, width 160, its row's y.
    unsigned found = 0;
    std::function<void(lx::ui::ui_node*, int)> walk = [&](lx::ui::ui_node* n, int depth) {
        if (!n)
            return;
        const lx::rect2i b = n->bounds();
        if (n->type() == control_node::static_type() && b.width >= 150 && b.width <= 170 &&
            b.height >= 24 && b.height <= 28) {
            ++found;
            LUMEN_CHECK(b.x == 32);
        }
        for (unsigned i = 0; i < n->child_count(); ++i)
            walk(n->child_at(i), depth + 1);
    };
    walk(root.tree(), 0);
    LUMEN_CHECK(found == 4); // one label cell per row, none collapsed
}

} // namespace

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    return status;
}
