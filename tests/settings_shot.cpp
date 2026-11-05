// lumen-settings' own face, without a compositor: the app's real `describe()` (the same
// session the tests render) drawn by the CPU UI renderer into a PNG — the dev-aid eye on
// the settings surface between releases, beside the widget gallery's panes.
//
//   lumen-settings-shot [out.png] [one-dark|one-light] [pane 0-7]
//
// LUMEN_SHOT_SIZE=WxH sets the window (default 960x680, lumen-settings' own opening size);
// LUMEN_SHOT_SCALE renders it at a device scale (2: a HiDPI panel), the way to put it beside
// a screenshot of Zed's settings window taken on one.

#include "ui_golden.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.gfx;
import lx.ui;
import lumen.settings_ui;

namespace {

int k_w = 960;
int k_h = 680;
float k_scale = 1.f;

/// The caches every pane draws with (the gallery's): the bundled fonts, one atlas for
/// glyphs and icons.
struct shot_env {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    lx::gfx::atlas atlas{};
    lx::text::glyph_cache glyphs{atlas};
    lx::ui::icon_cache icons{atlas};

    shot_env() {
        (void)faces.add_font_dir(LUMEN_TEST_FONT_DIR);
        lx::ui::ui_resources r{};
        r.text = {{&faces, &shapes, &items}, &glyphs, nullptr};
        r.icons = &icons;
        lx::ui::set_ui_resources(r);
    }
    ~shot_env() { lx::ui::set_ui_resources({}); }
};

[[nodiscard]] bool use_theme(const char* name) {
    if (auto t = lx::ui::load_theme_file(name)) {
        lx::ui::set_active_theme(std::move(t).value());
        return true;
    }
    if (std::strcmp(name, "one-dark") == 0)
        lx::ui::set_active_builtin_theme(lx::ui::appearance::dark);
    else if (std::strcmp(name, "one-light") == 0)
        lx::ui::set_active_builtin_theme(lx::ui::appearance::light);
    else
        return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    if (const char* sz = std::getenv("LUMEN_SHOT_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(sz, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            k_w = w;
            k_h = h;
        }
    }
    if (const char* sc = std::getenv("LUMEN_SHOT_SCALE")) {
        const float v = std::strtof(sc, nullptr);
        if (v >= 1.f && v <= 4.f)
            k_scale = v;
    }
    const char* out_path = argc > 1 ? argv[1] : "settings-shot.png";
    const char* theme_name = argc > 2 ? argv[2] : "one-dark";
    shot_env env{};
    if (!use_theme(theme_name)) {
        std::fprintf(stderr, "lumen-settings-shot: unknown theme %s\n", theme_name);
        return 2;
    }

    // The app's state: the defaults, no file editing (nothing to click here). An optional
    // pane index opens that pane (the search stays empty).
    settings::session the_session{};
    // LUMEN_SHOT_QUERY types a search (the results page, the sidebar's clear button).
    if (const char* q = std::getenv("LUMEN_SHOT_QUERY"); q && q[0]) {
        (void)the_session.query_buffer().assign(q);
        the_session.refresh();
    }
    if (std::getenv("LUMEN_SHOT_ALL")) // every pane expanded: the sidebar overflows
        for (unsigned i = 0; i < settings::k_pane_count; ++i)
            the_session.set_current(static_cast<settings::pane_id>(i));
    the_session.set_current(static_cast<settings::pane_id>(
        argc > 3 ? std::strtol(argv[3], nullptr, 10) : 0));
    if (argc > 3) {
        const long pane = std::strtol(argv[3], nullptr, 10);
        if (pane < 0 || pane >= static_cast<long>(settings::k_pane_count)) {
            std::fprintf(stderr, "lumen-settings-shot: pane %s out of range\n", argv[3]);
            return 2;
        }
        the_session.set_current(static_cast<settings::pane_id>(pane));
    }
    settings::set_active_session(&the_session);

    lx::runtime::memory_arena arena{1024 * 1024};
    lx::ui::ui_root root{arena};
    root.set_describer(&settings::describe);
    root.set_bounds({0, 0, k_w, k_h});
    root.set_scale(k_scale);
    auto list = std::make_unique<lx::scene::draw_list>();
    if (!root.tick(*list)) {
        std::fprintf(stderr, "lumen-settings-shot: the first tick changed nothing\n");
        return 1;
    }
    list->batch_for_render();
    if (std::getenv("LUMEN_SHOT_BOXES")) {
        // Where every row's control landed: bounds by element key (a layout check aid).
        const std::vector<settings::row_ref>& rows = settings::catalog();
        std::function<void(lx::ui::ui_node*)> walk = [&](lx::ui::ui_node* n) {
            if (!n)
                return;
            const lx::rect2i b = n->bounds();
            if (n->key() >= settings::k_row_control_base &&
                n->key() < settings::k_row_control_base + rows.size()) {
                const unsigned i = n->key() - settings::k_row_control_base;
                std::printf("  row %u (%s %s): %dx%d at (%d,%d)\n", i,
                            rows[i].desc ? rows[i].desc->section : "", rows[i].label,
                            b.width, b.height, b.x, b.y);
            }
            for (unsigned c = 0; c < n->child_count(); ++c)
                walk(n->child_at(c));
        };
        walk(root.tree());
    }

    // The draw list is in device px: the bounds projected at the scale.
    const lx::size2i dev = root.device_size();
    const int dw = dev.width > 0 ? dev.width : k_w;
    const int dh = dev.height > 0 ? dev.height : k_h;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(dw) * dh, 0u);
    lx::gfx::pixel_surface surface{};
    surface.pixels = reinterpret_cast<unsigned char*>(px.data());
    surface.width = dw;
    surface.height = dh;
    surface.stride = static_cast<unsigned>(dw) * 4u;
    surface.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    lx::gfx::damage_region full{};
    full.set_full();
    lx::gfx::ui_cpu_renderer renderer{};
    const lx::color clear = lx::ui::active_theme().color(lx::ui::tok::background);
    if (!renderer.draw(surface, ui_golden::stream_of(*list), &env.atlas, full,
                       ui_golden::premultiplied(clear))) {
        std::fprintf(stderr, "lumen-settings-shot: the renderer refused the frame\n");
        return 1;
    }

    const ui_golden::rgb_image image = ui_golden::to_rgb(px.data(), dw, dh, dw);
    if (!ui_golden::write_png(out_path, image)) {
        std::fprintf(stderr, "lumen-settings-shot: cannot write %s\n", out_path);
        return 1;
    }
    std::printf("%s: %d x %d, theme %s\n", out_path, dw, dh, theme_name);
    settings::set_active_session(nullptr);
    return 0;
}
