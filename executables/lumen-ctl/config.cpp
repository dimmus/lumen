// lumen-ctl's offline commands (design-session-and-wm C.6): `config check [FILE]` reads and
// validates the configuration as the compositor would — every layer, the schema, the
// bindings, and whether the keymap compiles — without a running session; `config defaults`
// prints the documented default file; `action --list` lists the actions bindings may name.
// The only part of lumen-ctl that uses a Lumen module (lx.config): main.cpp stays plain
// C++ on libsystemd.

#include <stdio.h>

import std;
import lx.config;

namespace {

constexpr const char k_defaults[] =
#include "lumen_default_config.inc"
    ;

void print_diagnostics(const lx::config::diagnostics& d, const lx::config::config_text& t) {
    for (const auto& x : d.items()) {
        char line[512];
        lx::config::format_diagnostic(x, t.path_of(x.layer), true, line, sizeof(line));
        std::printf("%s: %s\n", lx::config::severity_name(x.sev), line);
    }
    if (d.dropped())
        std::printf("(%u more not shown)\n", d.dropped());
}

} // namespace

int lumen_ctl_config(int argc, char** argv) {
    const std::string_view sub = argc > 0 ? argv[0] : "";
    if (sub == "defaults") {
        std::fputs(k_defaults, stdout);
        return 0;
    }
    if (sub != "check") {
        std::fprintf(stderr, "lumen-ctl: config check [FILE] | config defaults\n");
        return 2;
    }
    lx::config::config_location where{};
    if (argc > 1) {
        where.explicit_file = true;
        lx::config::config_path p{};
        std::snprintf(p.path, sizeof(p.path), "%s", argv[1]);
        where.files.push_back(p);
        where.user_file = p;
    } else {
        where = lx::config::locate_config();
    }
    lx::config::config_text text{};
    lx::config::diagnostics diag{};
    bool ok = lx::config::read_config(where, text, diag);
    if (ok && argc > 1 && text.layers.empty()) {
        std::fprintf(stderr, "lumen-ctl: %s: no such file\n", argv[1]);
        return 1;
    }
    auto snapshot = std::make_unique<lx::config::config_snapshot>(lx::config::default_config());
    if (ok)
        ok = lx::config::parse_config(text, *snapshot, diag, nullptr);
    if (ok)
        (void)lx::config::check_keymap(snapshot->keyboard, diag);
    print_diagnostics(diag, text);
    if (text.layers.empty())
        std::printf("no configuration file (every default)");
    else
        for (std::size_t i = 0; i < text.layers.size(); ++i)
            std::printf("%s%s", i ? ", " : "", text.layers[i].path);
    std::printf(": %s, %u error%s, %u warning%s, %zu bindings\n",
                ok ? "usable" : "NOT usable (the previous configuration would stay)",
                diag.errors(), diag.errors() == 1 ? "" : "s", diag.warnings(),
                diag.warnings() == 1 ? "" : "s", snapshot->bindings.size());
    return ok && diag.errors() == 0 ? 0 : 1;
}

int lumen_ctl_list_actions() {
    for (const auto& a : lx::config::actions()) {
        if (a.id == lx::config::action_id::none)
            continue;
        std::printf("%-32s %-10s %s%s%s\n", a.name,
                    a.owner == lx::config::action_owner::shell ? "shell" : "compositor",
                    a.summary, a.arg == lx::config::action_arg::index ? " (argument: N)" : "",
                    a.available ? "" : " [not available yet]");
    }
    return 0;
}
