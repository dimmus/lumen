module;

// Bodies of `:chooser` (the parsing; the list itself is the overlay's).

module lx.portal;

import std;

import :chooser;

namespace lx::portal {

chooser_entry parse_chooser_line(std::string_view line) {
    chooser_entry e;
    e.line.assign(line);
    constexpr std::string_view k_monitor = "Monitor: ";
    constexpr std::string_view k_window = "Window: ";
    if (line.starts_with(k_monitor)) {
        e.type = chooser_entry::kind::monitor;
        std::string_view rest = line.substr(k_monitor.size());
        const std::size_t sp = rest.find(' ');
        e.title.assign(rest.substr(0, sp));
        if (sp != std::string_view::npos)
            e.detail.assign(rest.substr(sp + 1));
        return e;
    }
    if (line.starts_with(k_window)) {
        e.type = chooser_entry::kind::window;
        std::string_view rest = line.substr(k_window.size());
        // The identifier is the last parenthesised word; a title may hold parentheses too.
        const std::size_t open = rest.rfind(" (");
        if (open != std::string_view::npos && rest.ends_with(")")) {
            e.title.assign(rest.substr(0, open));
            e.detail.assign(rest.substr(open + 2, rest.size() - open - 3));
        } else {
            e.title.assign(rest);
        }
        if (e.title.empty())
            e.title = "Untitled window";
        return e;
    }
    e.title.assign(line);
    return e;
}

std::vector<chooser_entry> parse_chooser_input(std::string_view text, std::size_t max) {
    std::vector<chooser_entry> out;
    while (!text.empty() && out.size() < max) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty())
            out.push_back(parse_chooser_line(line));
    }
    return out;
}

} // namespace lx::portal
