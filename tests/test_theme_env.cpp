// The `LUMEN_THEME` the compositor exports into the session environment (ADR-013 §4.7):
// `resolved_ui_theme` turns [ui] into the value applications start with — the configured
// name or path as written, and the built-in the appearance selects when no theme is named
// (the app-side default ignores appearance, so the export is what carries it). A reload
// re-exports only on a changed resolved value; the [ui] bits of `diff_config` are what
// trigger that check.
#include "lumen_test.hpp"

import std;

import lx.config;
import lx.compositor;

namespace {

lx::config::ui_settings ui_with(const char* theme, lx::config::ui_appearance look) {
    lx::config::ui_settings ui{};
    std::snprintf(ui.theme, sizeof(ui.theme), "%s", theme);
    ui.appearance = look;
    return ui;
}

} // namespace

LUMEN_TEST(resolved_theme_follows_the_appearance_when_none_is_named) {
    // The default: no theme, dark look.
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(lx::config::default_config().ui) ==
                "one-dark");
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("", lx::config::ui_appearance::light)) == "one-light");
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("", lx::config::ui_appearance::dark)) == "one-dark");
}

LUMEN_TEST(resolved_theme_passes_a_configured_name_or_path_through) {
    // A named theme wins over the appearance, as the palette job resolves it too.
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("one-dark", lx::config::ui_appearance::light)) == "one-dark");
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("one-light", lx::config::ui_appearance::dark)) == "one-light");
    // Any other name or a .lxtheme path: unchanged, the value the user wrote.
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("my-theme", lx::config::ui_appearance::light)) == "my-theme");
    LUMEN_CHECK(lx::compositor::resolved_ui_theme(
                    ui_with("/home/dimmus/.config/lumen/custom.lxtheme",
                            lx::config::ui_appearance::dark)) ==
                "/home/dimmus/.config/lumen/custom.lxtheme");
}

LUMEN_TEST(a_ui_change_that_moves_the_resolved_theme_reports_cfg_ui) {
    namespace cfg = lx::config;
    const auto flip = [](cfg::config_snapshot c, const char* theme, cfg::ui_appearance look) {
        std::snprintf(c.ui.theme, sizeof(c.ui.theme), "%s", theme);
        c.ui.appearance = look;
        return c;
    };
    const cfg::config_snapshot base = cfg::default_config(); // no theme, dark
    // The appearance flip of the seat check: no theme named, dark → light.
    LUMEN_CHECK(lx::compositor::diff_config(base, flip(base, "", cfg::ui_appearance::light)) ==
                lx::compositor::cfg_ui);
    // Naming a theme (or clearing one) moves the resolved value the same way.
    LUMEN_CHECK(lx::compositor::diff_config(base, flip(base, "one-light", cfg::ui_appearance::dark)) ==
                lx::compositor::cfg_ui);
    const cfg::config_snapshot named = flip(base, "one-light", cfg::ui_appearance::dark);
    LUMEN_CHECK(lx::compositor::diff_config(named, flip(named, "", cfg::ui_appearance::light)) ==
                lx::compositor::cfg_ui);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
