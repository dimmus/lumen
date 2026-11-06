module;

export module lx.config:edit;

import std;

import lx.foundation;
import :parse;
import :schema;
import :toml;

// ── ADR-013 §4.3: the user layer's splice editor ────────────────────────────────────────
//
// The settings application writes one key at a time into the user's config.toml by
// splicing the text: the key's line is replaced in place, a missing key is added under its
// section header, a missing section is appended — and every other byte (the user's
// comments, ordering, unknown keys) is preserved. Regenerating the file would destroy
// hand-written layers and produce spurious diffs in dotfile repos, so the fresh-document
// `toml::writer` stays what it is and this partition edits instead.
//
// The caller verifies the edit before it writes (the settings application re-parses the
// old and new text and asserts the snapshot changed only in the intended key) and writes
// atomically (temp file, fsync, rename — output_config_store's pattern). The compositor's
// watch picks the file up and reloads.
//
// Generic splices refuse array-of-table sections. Device helpers below select an exact
// decoded device name, leaving other devices and sections byte-identical.

export namespace lx::config {

/// A value a splice writes, as the file reads it: a boolean, an integer, a number, a
/// quoted string, a bare enumeration word, or an array of quoted strings.
struct edit_value {
    enum class kind : unsigned char { boolean, integer, number, string, word, string_list };

    kind type = kind::boolean;
    bool b = false;
    long long i = 0;
    double d = 0.0;
    std::string_view text{};
    std::span<const std::string_view> items{};

    [[nodiscard]] static edit_value boolean(bool v) {
        edit_value e{};
        e.type = kind::boolean;
        e.b = v;
        return e;
    }
    [[nodiscard]] static edit_value integer(long long v) {
        edit_value e{};
        e.type = kind::integer;
        e.i = v;
        return e;
    }
    [[nodiscard]] static edit_value number(double v) {
        edit_value e{};
        e.type = kind::number;
        e.d = v;
        return e;
    }
    /// A quoted basic string.
    [[nodiscard]] static edit_value string(std::string_view v) {
        edit_value e{};
        e.type = kind::string;
        e.text = v;
        return e;
    }
    /// An enumeration's value, written as the quoted word the file uses.
    [[nodiscard]] static edit_value word(std::string_view v) {
        edit_value e{};
        e.type = kind::word;
        e.text = v;
        return e;
    }
    /// An array of quoted strings (the workspace names).
    [[nodiscard]] static edit_value string_list(std::span<const std::string_view> v) {
        edit_value e{};
        e.type = kind::string_list;
        e.items = v;
        return e;
    }
};

enum class edit_status : unsigned char {
    ok,
    /// An array-of-table section (its keys are per-entry; not a splice).
    array_section,
    /// A bare word was asked for a value that is not one.
    bad_word,
    /// The text grew past `k_max_layer_bytes`.
    too_big,
    /// Invalid device selector or pointer key.
    invalid_device,
    /// The original text has invalid TOML syntax.
    invalid_text,
};

struct edit_report {
    edit_status status = edit_status::ok;
    /// The changed line, 1-based (in the edited text; 0 when nothing matched).
    unsigned line = 0;
};

/// `section.key = value` in `text`, the rest byte-identical. The key's existing line is
/// replaced; else the key is inserted under its section header (marked
/// `# set by lumen-settings`); else the section is appended at the end.
[[nodiscard]] std::string splice_key(std::string_view text, std::string_view section,
                                     std::string_view key, const edit_value& value,
                                     edit_report& report);

/// `section.key`'s line goes away (the key resets to whatever the lower layers say); a
/// `# set by lumen-settings` marker line directly above it goes too (splice_key added the
/// pair). The other bytes stand. False when there was no such line (report line 0).
[[nodiscard]] std::string remove_key(std::string_view text, std::string_view section,
                                     std::string_view key, edit_report& report);

/// Edit one exact-name [[input.device]] override; unrelated bytes remain unchanged.
/// Missing entries are appended. Repeated entries are resolved in file order.
[[nodiscard]] std::string splice_device_key(std::string_view text, std::string_view device_name,
    std::string_view key, const edit_value& value, edit_report& report);
/// Remove this key from every matching entry, revealing the lower layers/class settings.
[[nodiscard]] std::string remove_device_key(std::string_view text, std::string_view device_name,
    std::string_view key, edit_report& report);
/// Remove every table belonging to this exact device name.
[[nodiscard]] std::string remove_device_override(std::string_view text,
    std::string_view device_name, edit_report& report);

} // namespace lx::config

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

/// A TOML basic string, the same escapes the reader accepts and `toml::writer` writes.
void append_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (uc < 0x20 || uc == 0x7f) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", uc);
                out += esc;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

/// The line's value: `key = value` with the same wording `toml::writer` uses.
void append_value(std::string& out, const lx::config::edit_value& v,
                  lx::config::edit_status& status) {
    using kind = lx::config::edit_value::kind;
    switch (v.type) {
    case kind::boolean:
        out += v.b ? "true" : "false";
        return;
    case kind::integer: {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld", v.i);
        out += buf;
        return;
    }
    case kind::number: {
        char buf[40];
        const auto r = std::to_chars(buf, buf + sizeof(buf) - 1, v.d);
        *r.ptr = '\0';
        out += buf;
        return;
    }
    case kind::string:
        append_string(out, v.text);
        return;
    case kind::word: {
        // The file's enumeration values are quoted words (`layout = "floating"`); a splice
        // refuses text that is not one word.
        const bool ok = !v.text.empty() &&
                        std::all_of(v.text.begin(), v.text.end(), [](char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '-' || c == '_';
                        });
        if (!ok) {
            status = lx::config::edit_status::bad_word;
            return;
        }
        append_string(out, v.text);
        return;
    }
    case kind::string_list: {
        out += '[';
        bool first = true;
        for (const std::string_view item : v.items) {
            if (!first)
                out += ", ";
            first = false;
            append_string(out, item);
        }
        out += ']';
        return;
    }
    }
}

/// The header line this table opens (`[input.keyboard]`), or empty for the top.
[[nodiscard]] std::string_view header_of(std::string_view line) {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos || line[first] != '[')
        return {};
    // An array of tables ([[]]) is a different beast; its header is still recognised so the
    // scan does not mistake its keys for the previous table's.
    const auto last = line.rfind(']');
    if (last == std::string_view::npos || last < first)
        return {};
    return line.substr(first + 1, last - first - 1);
}

[[nodiscard]] std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

/// Is this line `key = …` in the table `section` names (an exact bare key)?
[[nodiscard]] bool is_key_line(std::string_view line, std::string_view key) {
    std::string_view t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '[')
        return false;
    if (t.size() <= key.size() || t.compare(0, key.size(), key) != 0)
        return false;
    std::string_view rest = trim(t.substr(key.size()));
    return !rest.empty() && rest[0] == '=';
}

/// `section` is one a splice refuses (`[[input.device]]`, `[[output]]`: the array tables).
[[nodiscard]] bool is_array_section(std::string_view section) {
    return section == "input.device" || section == "output";
}

constexpr std::string_view k_marker = "# set by lumen-settings";

} // namespace

std::string lx::config::splice_key(std::string_view text, std::string_view section,
                                   std::string_view key, const edit_value& value,
                                   edit_report& report) {
    report = {};
    if (is_array_section(section)) {
        report.status = edit_status::array_section;
        return std::string(text);
    }

    std::string value_line;
    value_line.reserve(key.size() + 64);
    value_line += key;
    value_line += " = ";
    append_value(value_line, value, report.status);
    if (report.status != edit_status::ok)
        return std::string(text);
    const std::string marked = std::string(k_marker) + "\n";

    // Pass 1: the key's line, in its section.
    std::string_view header;
    std::size_t offset = 0;
    std::size_t section_header_at = std::string_view::npos;
    while (offset <= text.size()) {
        const auto eol = text.find('\n', offset);
        const std::string_view line =
            text.substr(offset, eol == std::string_view::npos ? std::string_view::npos
                                                              : eol - offset);
        const std::string_view h = header_of(line);
        if (!h.empty()) {
            header = h;
            if (header == section && section_header_at == std::string_view::npos)
                section_header_at = offset;
        } else if (header == section && is_key_line(line, key)) {
            std::string out;
            out.reserve(text.size() + value_line.size() + marked.size());
            out += text.substr(0, offset);
            out += value_line;
            out += '\n';
            if (eol != std::string_view::npos)
                out += text.substr(eol + 1);
            report.line = static_cast<unsigned>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n')) + 1u;
            if (out.size() > k_max_layer_bytes) {
                // A longer line than the one it replaces can cross the cap too.
                report.status = edit_status::too_big;
                return std::string(text);
            }
            return out;
        }
        if (eol == std::string_view::npos)
            break;
        offset = eol + 1;
    }

    // Pass 2: insert under the section's header.
    if (section_header_at != std::string_view::npos) {
        const auto eol = text.find('\n', section_header_at);
        const std::size_t insert_at = eol == std::string_view::npos ? text.size() : eol + 1;
        std::string out;
        out.reserve(text.size() + value_line.size() + marked.size());
        out += text.substr(0, insert_at);
        out += marked;
        out += value_line;
        out += '\n';
        out += text.substr(insert_at);
        report.line = static_cast<unsigned>(
                          std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(insert_at), '\n')) +
                      1u;
        if (out.size() > k_max_layer_bytes) {
            report.status = edit_status::too_big;
            return std::string(text);
        }
        return out;
    }

    // Pass 3: append the section.
    std::string out;
    out.reserve(text.size() + section.size() + value_line.size() + marked.size() + 8);
    out += text;
    if (!out.empty() && out.back() != '\n')
        out += '\n';
    if (!out.empty())
        out += '\n';
    out += '[';
    out += section;
    out += "]\n";
    out += marked;
    out += value_line;
    out += '\n';
    report.line = static_cast<unsigned>(
        std::count(out.begin(), out.end(), '\n'));
    if (out.size() > k_max_layer_bytes) {
        report.status = edit_status::too_big;
        return std::string(text);
    }
    return out;
}

std::string lx::config::remove_key(std::string_view text, std::string_view section,
                                   std::string_view key, edit_report& report) {
    report = {};
    if (is_array_section(section)) {
        report.status = edit_status::array_section;
        return std::string(text);
    }
    std::string_view header;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto eol = text.find('\n', offset);
        const std::string_view line =
            text.substr(offset, eol == std::string_view::npos ? std::string_view::npos
                                                              : eol - offset);
        const std::string_view h = header_of(line);
        if (!h.empty()) {
            header = h;
        } else if (header == section && is_key_line(line, key)) {
            // The start of what goes: the key's line, and splice_key's marker directly
            // above it (anything else — the user's comments — stays). start−1 is the
            // newline ending the previous line; the one to find ends the line before it.
            std::size_t start = offset;
            if (start >= 2) {
                const auto prev_eol = text.rfind('\n', start - 2);
                const std::size_t prev_start =
                    prev_eol == std::string_view::npos ? 0 : prev_eol + 1;
                const std::string_view prev =
                    trim(text.substr(prev_start, start - 1 - prev_start));
                if (prev == k_marker)
                    start = prev_start;
            }
            std::string out;
            out.reserve(text.size());
            out += text.substr(0, start);
            if (eol != std::string_view::npos)
                out += text.substr(eol + 1);
            else if (start)
                out.pop_back(); // the removed line ended the file: its newline goes too
            report.line = static_cast<unsigned>(
                std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n')) +
                          1u;
            return out;
        }
        if (eol == std::string_view::npos)
            break;
        offset = eol + 1;
    }
    return std::string(text); // nothing matched: the text stands
}

namespace {
struct device_text_table {
    std::size_t begin = 0, body = 0, end = 0;
    std::string name{};
    std::vector<std::pair<std::string, std::size_t>> keys{};
};

// Use the TOML reader for decoded selectors/keys and table boundaries. In particular,
// a bracket in a comment or a multiline array must never become a table boundary.
bool device_tables(std::string_view text, std::vector<device_text_table>& tables) {
    struct scan {
        std::string_view text;
        std::vector<std::size_t> lines{0};
        std::vector<device_text_table>* tables;
        bool device = false;
        bool in_inline = false;
        std::size_t at(unsigned line) const { return lines[line - 1]; }
    } s{text, {0}, &tables};
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') s.lines.push_back(i + 1);
    lx::config::toml::handler h{};
    h.user = &s;
    h.table = [](void* u, const char* name, bool array, lx::config::toml::parse_error& e) {
        auto& s = *static_cast<scan*>(u);
        const auto at = s.at(e.line);
        if (s.device) s.tables->back().end = at;
        s.device = array && std::string_view(name) == "input.device";
        if (s.device) {
            const auto nl = s.text.find('\n', at);
            s.tables->push_back({at, nl == std::string_view::npos ? s.text.size() : nl + 1,
                                 s.text.size(), {}, {}});
        }
        return true;
    };
    h.key_value = [](void* u, const char* key, const lx::config::toml::value& v,
                     lx::config::toml::parse_error& e) {
        auto& s = *static_cast<scan*>(u);
        if (!s.device || s.in_inline) return true;
        auto& t = s.tables->back();
        if (std::string_view(key) == "name") {
            if (v.type == lx::config::toml::value::kind::string) t.name = v.str;
        } else t.keys.emplace_back(key, s.at(e.line));
        return true;
    };
    h.inline_table = [](void* u, const char*, bool begin, lx::config::toml::parse_error&) {
        static_cast<scan*>(u)->in_inline = begin;
        return true;
    };
    lx::config::toml::parse_error e{};
    return lx::config::toml::parse(text.data(), text.size(), h, e);
}

bool valid_device_edit(std::string_view text, std::string_view name, std::string_view key,
                       lx::config::edit_report& r) {
    r = {};
    if (text.size() > lx::config::k_max_layer_bytes) {
        r.status = lx::config::edit_status::too_big;
        return false;
    }
    if (name.empty() || name.size() >= 128 || name.find('\0') != std::string_view::npos ||
        (!key.empty() && !std::ranges::any_of(lx::config::pointer_keys(),
            [&](const auto& k) { return key == k.key; }))) {
        r.status = lx::config::edit_status::invalid_device;
        return false;
    }
    return true;
}

std::size_t after_line(std::string_view text, std::size_t at) {
    const auto nl = text.find('\n', at);
    return nl == std::string_view::npos ? text.size() : nl + 1;
}

// Preserve the indentation, spelling of the key, and trailing comment on replacement.
std::string replace_device_value(std::string_view line, std::string_view value) {
    char quote = 0;
    bool escape = false;
    std::size_t equal = std::string_view::npos, comment = line.size();
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) { escape = false; continue; }
        if (quote) {
            if (quote == '"' && c == '\\') escape = true;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') quote = c;
        else if (c == '=' && equal == std::string_view::npos) equal = i;
        else if (c == '#') { comment = i; break; }
    }
    auto begin = line.find_first_not_of(" \t", equal + 1);
    if (begin == std::string_view::npos) begin = equal + 1;
    auto end = comment;
    while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                          line[end - 1] == '\r' || line[end - 1] == '\n')) --end;
    return std::string(line.substr(0, begin)) + std::string(value) + std::string(line.substr(end));
}
} // namespace

std::string lx::config::splice_device_key(std::string_view text, std::string_view name,
    std::string_view key, const edit_value& value, edit_report& report) {
    if (!valid_device_edit(text, name, key, report) || key.empty()) {
        report.status = key.empty() ? edit_status::invalid_device : report.status;
        return std::string(text);
    }
    std::vector<device_text_table> tables;
    if (!device_tables(text, tables)) {
        report.status = edit_status::invalid_text;
        return std::string(text);
    }
    std::string encoded;
    append_value(encoded, value, report.status);
    if (report.status != edit_status::ok) return std::string(text);
    std::size_t at = text.size(), end = at;
    const device_text_table* selected = nullptr;
    bool existing = false;
    for (const auto& t : tables) {
        if (t.name != name) continue;
        selected = &t;
        for (const auto& [k, pos] : t.keys)
            if (k == key) { at = pos; end = after_line(text, pos); existing = true; }
    }
    std::string insertion;
    if (existing) insertion = replace_device_value(text.substr(at, end - at), encoded);
    else {
        if (selected) at = end = selected->body;
        if (at && text[at - 1] != '\n') insertion += '\n';
        if (!selected) {
            insertion += "\n[[input.device]]\nname = ";
            append_string(insertion, name);
            insertion += '\n';
        }
        insertion += std::string(k_marker) + "\n" + std::string(key) + " = " + encoded + "\n";
    }
    std::string out(text);
    out.replace(at, end - at, insertion);
    if (out.size() > k_max_layer_bytes) {
        report.status = edit_status::too_big;
        return std::string(text);
    }
    report.line = static_cast<unsigned>(std::count(text.begin(), text.begin() + at, '\n')) + 1u;
    return out;
}

std::string lx::config::remove_device_key(std::string_view text, std::string_view name,
    std::string_view key, edit_report& report) {
    if (!valid_device_edit(text, name, key, report)) return std::string(text);
    std::vector<device_text_table> tables;
    if (!device_tables(text, tables)) {
        report.status = edit_status::invalid_text;
        return std::string(text);
    }
    std::string out(text);
    // Reverse order keeps every recorded byte offset valid.
    for (auto ti = tables.rbegin(); ti != tables.rend(); ++ti) {
        if (ti->name != name) continue;
        if (key.empty()) {
            out.erase(ti->begin, ti->end - ti->begin);
            report.line = static_cast<unsigned>(std::count(text.begin(), text.begin() + ti->begin, '\n')) + 1u;
            continue;
        }
        for (auto ki = ti->keys.rbegin(); ki != ti->keys.rend(); ++ki) {
            if (ki->first != key) continue;
            auto start = ki->second;
            if (start > ti->body) {
                const auto prev = start < 2 ? std::string_view::npos : text.rfind('\n', start - 2);
                const auto p = prev == std::string_view::npos ? 0 : prev + 1;
                if (trim(text.substr(p, start - p - 1)) == k_marker) start = p;
            }
            out.erase(start, after_line(text, ki->second) - start);
            report.line = static_cast<unsigned>(std::count(text.begin(), text.begin() + start, '\n')) + 1u;
        }
    }
    return out;
}

std::string lx::config::remove_device_override(std::string_view text, std::string_view name,
    edit_report& report) {
    return remove_device_key(text, name, {}, report);
}
