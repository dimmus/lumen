// The `LUMEN_THEME` the compositor exports into the session environment (ADR-013 §4.7):
// `resolved_ui_theme` turns [ui] into the value applications start with — the configured
// name or path as written, and the built-in the appearance selects when no theme is named
// (the app-side default ignores appearance, so the export is what carries it). A reload
// re-exports only on a changed resolved value; the [ui] bits of `diff_config` are what
// trigger that check. `env_reexports` is that export's bookkeeping: what a busy user bus
// held back and what the manager already carries (the compositor wires it to
// `user_environment`'s one-export-at-a-time bus, which a unit test cannot open).
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

using which = lx::compositor::env_reexports::which;

/// What a `drain` sent, in order — the observer the compositor's `retry_owed_env_exports`
/// plays in real life (`export_cursor_environment` before `export_theme_environment`).
std::vector<which> drained(lx::compositor::env_reexports& re) {
    std::vector<which> sent;
    re.drain([&sent](which w) { sent.push_back(w); });
    return sent;
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

LUMEN_TEST(an_owed_reexport_leaves_when_the_reply_frees_the_bus_cursor_first) {
    // A reload changed [cursor] and [ui] while the start-up export was still in flight:
    // both re-exports are owed, and the reply sends them in the reload's order.
    lx::compositor::env_reexports re;
    re.owe(which::cursor);
    re.owe(which::theme);
    const std::vector<which> sent = drained(re);
    LUMEN_CHECK(sent.size() == 2);
    LUMEN_CHECK(sent[0] == which::cursor);
    LUMEN_CHECK(sent[1] == which::theme);
    // Nothing is owed anymore: a later reply (e.g. the cursor re-export's own one) is quiet.
    LUMEN_CHECK(drained(re).empty());
}

LUMEN_TEST(an_owed_reexport_that_defers_again_is_not_lost) {
    // The reply freed the bus, but the cursor re-export went out first, so the theme one
    // finds the bus busy again (`export_pending`) and re-owes itself — the cursor
    // re-export's own reply then sends it: not lost.
    lx::compositor::env_reexports re;
    re.owe(which::cursor);
    re.owe(which::theme);
    bool busy = false; // an export is in flight (user_environment::export_pending)
    std::vector<which> sent;
    const auto attempt = [&](which w) {
        if (busy) { // what export_cursor/theme_environment do on a pending bus
            re.owe(w);
            return;
        }
        sent.push_back(w);
        busy = true; // this export occupies the bus until its reply
    };
    re.drain(attempt);
    LUMEN_CHECK(sent.size() == 1 && sent[0] == which::cursor);
    busy = false; // the cursor re-export's reply arrived
    re.drain(attempt);
    LUMEN_CHECK(sent.size() == 2 && sent[1] == which::theme);
}

LUMEN_TEST(teardown_drops_owed_reexports_and_the_remembered_theme) {
    // end_session: an export whose reply never came must not re-export into an environment
    // unset_now is clearing, and the remembered theme value leaves with the session.
    lx::compositor::env_reexports re;
    re.theme_sent("one-light");
    re.owe(which::theme);
    re.drop();
    LUMEN_CHECK(drained(re).empty());
    LUMEN_CHECK(re.theme_changed("one-light")); // nothing counts as sent anymore
}

LUMEN_TEST(only_a_changed_resolved_theme_value_reexports) {
    // Start-up exported one-dark (the appearance fallback); density and font-size edits
    // resolve to the same value and do not churn the user bus — a changed one does.
    lx::compositor::env_reexports re;
    LUMEN_CHECK(re.theme_changed("one-dark")); // nothing sent yet
    re.theme_sent("one-dark");
    LUMEN_CHECK(!re.theme_changed("one-dark"));
    LUMEN_CHECK(re.theme_changed("one-light"));
    re.theme_sent("one-light");
    LUMEN_CHECK(!re.theme_changed("one-light"));
}

LUMEN_TEST(a_refused_export_does_not_count_as_carried) {
    // The user manager refused the start-up export (or never answered): it does not carry
    // the value, so a reload that resolves to the very same theme must still re-export it —
    // recording the value before the outcome is known is what `forget` undoes.
    lx::compositor::env_reexports re;
    re.theme_sent("one-light");
    re.theme_forget();
    LUMEN_CHECK(re.theme_changed("one-light"));
    // Once a retry really leaves, the gate closes again.
    re.theme_sent("one-light");
    LUMEN_CHECK(!re.theme_changed("one-light"));
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
