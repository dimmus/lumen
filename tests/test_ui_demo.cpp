#include "lumen_test.hpp"

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;
import lumen.ui_demo;

namespace {

using namespace lx::ui;
using ui_demo::page_id;

struct resources {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    lx::gfx::atlas atlas{};
    lx::text::glyph_cache glyphs{atlas};
    icon_cache icons{atlas};

    resources() {
        (void)faces.add_font_dir(LUMEN_TEST_FONT_DIR);
        ui_resources r{};
        r.text = {{&faces, &shapes, &items}, &glyphs, nullptr};
        r.icons = &icons;
        set_ui_resources(r);
    }
    ~resources() { set_ui_resources({}); }
};

[[nodiscard]] ui_node* find_key(ui_node* node, element_key wanted) {
    if (!node || node->key() == wanted)
        return node;
    for (unsigned i = 0; i < node->child_count(); ++i)
        if (ui_node* found = find_key(node->child_at(i), wanted))
            return found;
    return nullptr;
}

struct harness {
    resources assets{};
    lx::runtime::memory_arena arena{512 * 1024};
    ui_context context{};
    ui_root root{arena, context};
    std::unique_ptr<lx::scene::draw_list> draws = std::make_unique<lx::scene::draw_list>();

    harness() {
        ui_demo::bind_root(&root);
        root.set_describer(&ui_demo::describe);
        root.set_bounds({0, 0, 1040, 820});
    }
    ~harness() { ui_demo::bind_root(nullptr); }

    bool tick() { return root.tick(*draws); }

    [[nodiscard]] ui_node* find(element_key wanted) { return find_key(root.tree(), wanted); }

    [[nodiscard]] const ui_demo::demo_state& state() {
        auto* host = find(ui_demo::k_demo_root);
        LUMEN_CHECK(host && host->type() == ui_demo::demo_host::static_type());
        return static_cast<const ui_demo::demo_host&>(*host).state();
    }

    void click(element_key wanted, bool expect_handled = true) {
        auto* node = find(wanted);
        LUMEN_CHECK(node);
        const auto rect = surface_bounds(*node);
        LUMEN_CHECK(rect.width > 0 && rect.height > 0);
        (void)root.pointer_move({rect.x + rect.width / 2, rect.y + rect.height / 2});
        const bool pressed = root.pointer_button(mouse_button::left, true);
        const bool released = root.pointer_button(mouse_button::left, false);
        if (expect_handled)
            LUMEN_CHECK(pressed && released);
    }

    void press(key k, char32_t codepoint = 0, unsigned modifiers = 0) {
        key_event event{};
        event.key = k;
        event.codepoint = codepoint;
        event.modifiers = modifiers;
        event.pressed = true;
        (void)root.key(event);
        event.pressed = false;
        (void)root.key(event);
    }

    void type(std::u32string_view text) {
        for (const auto ch : text)
            press(key::unknown, ch);
    }
};

} // namespace

LUMEN_TEST(demo_catalog_contains_and_mounts_each_example_page) {
    // Keep this list separate from catalog(): omitting a page from the registry must fail.
    constexpr page_id expected[] = {page_id::buttons, page_id::toggles,     page_id::text,
                                    page_id::media,   page_id::inputs,      page_id::values,
                                    page_id::layouts, page_id::collections, page_id::overlays,
                                    page_id::coverage};
    harness h{};
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(ui_demo::catalog().size() == std::size(expected));
    for (page_id page : expected) {
        LUMEN_CHECK(std::ranges::count(ui_demo::catalog(), page, &ui_demo::page_info::id) == 1);
        h.click(ui_demo::nav_key(page));
        (void)h.tick();
        LUMEN_CHECK(h.state().page == page);
        auto* example = h.find(ui_demo::page_specimen_key(page));
        LUMEN_CHECK(example && example->child_count() > 0);
        auto* detail = h.find(ui_demo::k_detail);
        LUMEN_CHECK(detail && detail->bounds().width > 0 && detail->bounds().height > 0);
    }
}

LUMEN_TEST(demo_search_filters_pages_and_recovers_from_no_matches) {
    harness h{};
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::k_search);
    h.type(U"popover");
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.find(ui_demo::nav_key(page_id::overlays)));
    LUMEN_CHECK(!h.find(ui_demo::nav_key(page_id::buttons)));

    h.press(key::unknown, U'a', k_mod_ctrl);
    h.type(U"no_such_demo_page");
    LUMEN_CHECK(h.tick());
    for (const auto& entry : ui_demo::catalog())
        LUMEN_CHECK(!h.find(ui_demo::nav_key(entry.id)));

    h.press(key::unknown, U'a', k_mod_ctrl);
    h.press(key::backspace);
    LUMEN_CHECK(h.tick());
    for (const auto& entry : ui_demo::catalog())
        LUMEN_CHECK(h.find(ui_demo::nav_key(entry.id)));
}

LUMEN_TEST(demo_compact_navigation_preserves_example_state) {
    harness h{};
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.find(ui_demo::k_sidebar) && h.find(ui_demo::k_detail));
    LUMEN_CHECK(!h.find(ui_demo::k_back));
    h.click(ui_demo::k_primary_action);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().clicks == 1);

    h.click(ui_demo::k_disabled_action, false);
    (void)h.tick();
    LUMEN_CHECK(h.state().clicks == 1);

    h.root.set_bounds({0, 0, ui_demo::k_compact_width - 1, 820});
    LUMEN_CHECK(h.tick());
    if (h.find(ui_demo::k_detail)) {
        LUMEN_CHECK(!h.find(ui_demo::k_sidebar));
        h.click(ui_demo::k_back);
        LUMEN_CHECK(h.tick());
    }
    LUMEN_CHECK(h.find(ui_demo::k_sidebar) && !h.find(ui_demo::k_detail));
    h.click(ui_demo::nav_key(page_id::toggles));
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.find(ui_demo::k_detail) && !h.find(ui_demo::k_sidebar));
    LUMEN_CHECK(h.state().page == page_id::toggles);
    LUMEN_CHECK(h.root.focus().focused() == h.find(ui_demo::k_back));
    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.root.focus().focused() == h.find(ui_demo::nav_key(page_id::toggles)));
    h.click(ui_demo::nav_key(page_id::buttons));
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().clicks == 1);

    h.root.set_bounds({0, 0, 1040, 820});
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.find(ui_demo::k_sidebar) && h.find(ui_demo::k_detail));
    LUMEN_CHECK(!h.find(ui_demo::k_back));
    LUMEN_CHECK(h.state().clicks == 1);
}

LUMEN_TEST(demo_modal_takes_keyboard_focus_and_escape_returns_it) {
    harness h{};
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::nav_key(page_id::overlays));
    LUMEN_CHECK(h.tick());
    auto* trigger = h.find(ui_demo::k_open_modal);
    LUMEN_CHECK(trigger && h.root.focus().focus(*trigger, focus_cause::keyboard));
    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.find(ui_demo::k_modal_cancel) && h.find(ui_demo::k_modal_confirm));
    LUMEN_CHECK(h.root.focus().focused() && h.root.focus().focused() != trigger);
    h.press(key::escape);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.context.overlays().open_count() == 0);
    LUMEN_CHECK(h.root.focus().focused() == trigger);
    LUMEN_CHECK(h.state().modal_confirmations == 0);

    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::k_modal_cancel);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.context.overlays().open_count() == 0);
    LUMEN_CHECK(h.root.focus().focused() == trigger);

    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::k_modal_confirm);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(!h.find(ui_demo::k_modal_confirm));
    LUMEN_CHECK(h.state().modal_confirmations == 1 && h.state().archived);
    LUMEN_CHECK(h.root.focus().focused() == h.find(ui_demo::k_undo));
    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(!h.state().archived);
    LUMEN_CHECK(h.root.focus().focused() == trigger);
}

LUMEN_TEST(demo_theme_and_scale_changes_refresh_the_current_page) {
    struct restore_theme {
        ~restore_theme() { set_active_builtin_theme(appearance::dark); }
    } restore;
    set_active_builtin_theme(appearance::dark);
    harness h{};
    LUMEN_CHECK(h.tick());
    auto* host = static_cast<ui_demo::demo_host*>(h.find(ui_demo::k_demo_root));
    LUMEN_CHECK(host);
    auto count = host->rebuilds();
    set_active_builtin_theme(appearance::light);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(host->rebuilds() > count);

    count = host->rebuilds();
    h.root.set_scale(1.25f);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(host->rebuilds() > count);
    LUMEN_CHECK(h.root.device_size().width == 1300);
    LUMEN_CHECK(h.find(ui_demo::k_sidebar) && h.find(ui_demo::k_detail));
}

LUMEN_TEST(demo_paste_uses_the_first_line_and_edits_clear_submit_feedback) {
    harness h{};
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::nav_key(page_id::inputs));
    LUMEN_CHECK(h.tick());
    clipboard::global().set_system_text("first line\nsecond line");
    h.click(ui_demo::k_paste);
    LUMEN_CHECK(h.tick());
    clipboard::global().set_system_text("");
    LUMEN_CHECK(h.state().entry.get().view() == "first line");
    h.click(ui_demo::k_submit);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().input_feedback == ui_demo::input_status::submitted);
    h.click(ui_demo::k_message);
    h.type(U"!");
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().input_feedback != ui_demo::input_status::submitted);
}

LUMEN_TEST(demo_virtual_list_selection_and_activation_update_state) {
    harness h{};
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::nav_key(page_id::collections));
    LUMEN_CHECK(h.tick());
    auto* collection = h.find(ui_demo::k_collection);
    LUMEN_CHECK(collection && h.root.focus().focus(*collection, focus_cause::keyboard));
    h.press(key::end);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().row == 9999);
    h.press(key::enter);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(h.state().activated_row == 9999);
}

LUMEN_TEST(demo_compact_keyboard_traversal_reveals_offscreen_controls) {
    harness h{};
    h.root.set_bounds({0, 0, 340, 420});
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::k_back);
    LUMEN_CHECK(h.tick());
    h.click(ui_demo::nav_key(page_id::values));
    LUMEN_CHECK(h.tick());
    unsigned visible_stops = 0;
    for (unsigned i = 0; i < 40; ++i) {
        h.press(key::tab);
        (void)h.tick();
        auto* focused = h.root.focus().focused();
        LUMEN_CHECK(focused);
        if (focused->key() == ui_demo::k_back)
            break;
        const auto viewport = surface_bounds(*h.find(ui_demo::page_content_key(page_id::values)));
        const auto item = surface_bounds(*focused);
        LUMEN_CHECK(item.y + item.height > viewport.y && item.y < viewport.y + viewport.height);
        ++visible_stops;
    }
    LUMEN_CHECK(visible_stops >= 7 && visible_stops < 40);
}

LUMEN_TEST(demo_default_page_goes_idle) {
    harness h{};
    LUMEN_CHECK(h.tick());
    for (unsigned i = 0; i < 4 && h.root.wants_frame(); ++i)
        (void)h.tick();
    LUMEN_CHECK(!h.root.wants_frame());
    LUMEN_CHECK(!h.tick());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
