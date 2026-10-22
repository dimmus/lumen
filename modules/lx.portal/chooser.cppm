module;

// The source chooser's input (design-app-compat-capture E.2): xdg-desktop-portal-wlr runs
// `xdg-desktop-portal-lumen --choose` as its dmenu-type chooser (chooser_type=dmenu) and
// writes one line per source on its stdin — "Monitor: NAME DESCRIPTION" for each output,
// then "Window: TITLE (IDENTIFIER)" for each toplevel of ext-foreign-toplevel-list — and
// expects the chosen line, verbatim, on stdout (nothing, or exit status 1, declines).
// Parsing only decides how a line is shown; what goes back is always the line itself.

export module lx.portal:chooser;

import std;

export namespace lx::portal {

struct chooser_entry {
    enum class kind : unsigned char { monitor, window, other };
    kind type = kind::other;
    std::string line;   // returned verbatim
    std::string title;  // shown first: the output's name, the window's title
    std::string detail; // shown dimmed: the output's description, the window's identifier
};

[[nodiscard]] chooser_entry parse_chooser_line(std::string_view line);

/// Every non-empty line of `text` (a trailing '\r' dropped), at most `max` of them.
[[nodiscard]] std::vector<chooser_entry> parse_chooser_input(std::string_view text,
                                                             std::size_t max = 64);

} // namespace lx::portal
