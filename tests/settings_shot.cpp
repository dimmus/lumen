// lumen-settings' own face, without a compositor: the app's real `describe()` (the same
// session the tests render) drawn by the CPU UI renderer into a PNG — the dev-aid eye on
// the settings surface between releases, beside the widget gallery's panes.
//
//   lumen-settings-shot [out.png] [one-dark|one-light]

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

constexpr int k_w = 960;
constexpr int k_h = 680;

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
    const char* out_path = argc > 1 ? argv[1] : "settings-shot.png";
    const char* theme_name = argc > 2 ? argv[2] : "one-dark";
    shot_env env{};
    if (!use_theme(theme_name)) {
        std::fprintf(stderr, "lumen-settings-shot: unknown theme %s\n", theme_name);
        return 2;
    }

    // The app's state: the defaults, no file editing (nothing to click here).
    settings::session the_session{};
    settings::set_active_session(&the_session);

    lx::runtime::memory_arena arena{1024 * 1024};
    lx::ui::ui_root root{arena};
    root.set_describer(&settings::describe);
    root.set_bounds({0, 0, k_w, k_h});
    auto list = std::make_unique<lx::scene::draw_list>();
    if (!root.tick(*list)) {
        std::fprintf(stderr, "lumen-settings-shot: the first tick changed nothing\n");
        return 1;
    }
    list->batch_for_render();

    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_w) * k_h, 0u);
    lx::gfx::pixel_surface surface{};
    surface.pixels = reinterpret_cast<unsigned char*>(px.data());
    surface.width = k_w;
    surface.height = k_h;
    surface.stride = k_w * 4u;
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

    const ui_golden::rgb_image image = ui_golden::to_rgb(px.data(), k_w, k_h, k_w);
    if (!ui_golden::write_png(out_path, image)) {
        std::fprintf(stderr, "lumen-settings-shot: cannot write %s\n", out_path);
        return 1;
    }
    std::printf("%s: %d x %d, theme %s\n", out_path, k_w, k_h, theme_name);
    settings::set_active_session(nullptr);
    return 0;
}
