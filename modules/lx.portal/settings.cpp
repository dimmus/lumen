module;

// Bodies of `:settings`. See settings.cppm for the model.

module lx.portal;

import std;

import lx.trace;
import lx.config;
import :settings;

namespace lx::portal {

namespace {
constexpr lx::trace::category log_settings{"portal.settings"};
}

appearance_values appearance_of(const lx::config::config_snapshot& c) {
    appearance_values v{};
    v.color_scheme =
        c.ui.appearance == lx::config::ui_appearance::light ? k_color_scheme_light : k_color_scheme_dark;
    v.contrast = 0;
    return v;
}

bool namespace_matches(std::string_view ns, const std::vector<std::string>& patterns) {
    if (patterns.empty())
        return true;
    for (const std::string& pattern : patterns) {
        if (pattern.empty())
            return true; // an empty pattern matches all
        if (ns == pattern)
            return true;
        if (pattern.back() == '*' && ns.size() >= pattern.size() - 1 &&
            ns.substr(0, pattern.size() - 1) == std::string_view{pattern}.substr(0, pattern.size() - 1))
            return true; // a trailing '*' matches its prefix
    }
    return false;
}

settings_state::settings_state(lx::config::env_lookup env) : env_{env} {
    load();
    // LUMEN_CONFIG=0: no watch (the defaults are served); no inotify: read at start-up
    // only, like every lx.config user degrades.
    (void)watch_.open(where_);
}

void settings_state::load() {
    // The settings application's truth: locate the layers, read them, parse them; missing
    // or invalid layers leave the defaults in place (the compositor keeps its previous
    // configuration too).
    where_ = lx::config::locate_config(env_);
    lx::config::config_text text{};
    lx::config::diagnostics d{};
    lx::config::config_snapshot parsed{};
    if (where_.disabled)
        return;
    if (lx::config::read_config(where_, text, d) && !text.layers.empty() &&
        lx::config::parse_config(text, parsed, d))
        values_ = appearance_of(parsed);
}

appearance_flip settings_state::poll_changed() {
    if (fd() < 0 || !watch_.drain())
        return {};
    const appearance_values before = values_;
    load();
    const appearance_flip f = flips(before, values_);
    if (f.color_scheme)
        log_settings.info("color-scheme {} -> {}", before.color_scheme, values_.color_scheme);
    return f;
}

} // namespace lx::portal
