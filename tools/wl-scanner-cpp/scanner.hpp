#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wlgen {

struct EnumEntry {
    std::string name;
    unsigned value = 0;
};

struct EnumDef {
    std::string name;
    std::vector<EnumEntry> entries;
};

struct ArgDef {
    std::string name;
    std::string type;
    std::string interface;
    std::string enum_name;
    std::string summary;
    bool allow_null = false;
};

struct MessageDef {
    std::string name;
    std::string kind; // request | event
    bool destructor = false;
    std::vector<ArgDef> args;
};

struct InterfaceDef {
    std::string name;
    int version = 1;
    std::string description;
    std::vector<EnumDef> enums;
    std::vector<MessageDef> requests;
    std::vector<MessageDef> events;
};

struct ProtocolDef {
    std::string name;
    std::vector<InterfaceDef> interfaces;
};

inline std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

inline std::string attr(const std::string& tag, std::string_view key) {
    const std::string needle = std::string(key) + "=\"";
    auto pos = tag.find(needle);
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    auto end = tag.find('"', pos);
    if (end == std::string::npos)
        return {};
    return tag.substr(pos, end - pos);
}

inline bool has_attr(const std::string& tag, std::string_view key, std::string_view val) {
    return attr(tag, key) == val;
}

inline unsigned parse_int_value(const std::string& value) {
    if (value.empty())
        return 0;
    return static_cast<unsigned>(std::stoul(value, nullptr, 0));
}

/// Strip XML comments (`<!-- ... -->`), including multi-line.
inline std::string strip_xml_comments(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    std::size_t i = 0;
    while (i < xml.size()) {
        if (i + 3 < xml.size() && xml[i] == '<' && xml[i + 1] == '!' && xml[i + 2] == '-' &&
            xml[i + 3] == '-') {
            i += 4;
            while (i + 2 < xml.size() &&
                   !(xml[i] == '-' && xml[i + 1] == '-' && xml[i + 2] == '>'))
                ++i;
            if (i + 2 < xml.size())
                i += 3; // skip "-->"
            continue;
        }
        out.push_back(xml[i]);
        ++i;
    }
    return out;
}

/// Split into tags / text nodes. Handles multi-line tags and quoted `>` safely.
inline std::vector<std::string> split_tags(const std::string& xml_in) {
    const std::string xml = strip_xml_comments(xml_in);
    std::vector<std::string> tags;
    std::string current;
    bool in_tag = false;
    bool in_quote = false;
    char quote_ch = 0;

    for (std::size_t i = 0; i < xml.size(); ++i) {
        const char c = xml[i];

        if (in_tag) {
            current.push_back(c);
            if (in_quote) {
                if (c == quote_ch)
                    in_quote = false;
                continue;
            }
            if (c == '"' || c == '\'') {
                in_quote = true;
                quote_ch = c;
                continue;
            }
            if (c == '>') {
                tags.push_back(current);
                current.clear();
                in_tag = false;
            }
            continue;
        }

        // Outside tags: accumulate text until next '<'.
        if (c == '<') {
            if (!current.empty()) {
                tags.push_back(current);
                current.clear();
            }
            in_tag = true;
            current.push_back(c);
            continue;
        }
        current.push_back(c);
    }

    if (!current.empty())
        tags.push_back(current);
    return tags;
}

inline ProtocolDef parse_protocol_xml(const std::string& xml) {
    ProtocolDef proto;
    InterfaceDef* iface = nullptr;
    EnumDef* en = nullptr;
    MessageDef* msg = nullptr;

    for (const auto& tag : split_tags(xml)) {
        if (!tag.starts_with('<'))
            continue;
        if (tag.starts_with("<?") || tag.starts_with("<!"))
            continue;

        if (tag.starts_with("<protocol ") || tag.starts_with("<protocol>")) {
            proto.name = attr(tag, "name");
            continue;
        }

        if (tag.starts_with("<interface ")) {
            proto.interfaces.push_back({});
            iface = &proto.interfaces.back();
            iface->name = attr(tag, "name");
            const auto ver = attr(tag, "version");
            iface->version = static_cast<int>(parse_int_value(ver.empty() ? "1" : ver));
            en = nullptr;
            msg = nullptr;
            continue;
        }

        if (!iface)
            continue;

        if (tag.starts_with("<description ") && !msg) {
            iface->description = attr(tag, "summary");
            continue;
        }

        if (tag.starts_with("<enum ")) {
            iface->enums.push_back({});
            en = &iface->enums.back();
            en->name = attr(tag, "name");
            msg = nullptr;
            continue;
        }

        if (tag.starts_with("</enum")) {
            en = nullptr;
            continue;
        }

        if (tag.starts_with("<entry ") && en) {
            en->entries.push_back({attr(tag, "name"), parse_int_value(attr(tag, "value"))});
            continue;
        }

        if (tag.starts_with("<request ")) {
            iface->requests.push_back(
                {attr(tag, "name"), "request", has_attr(tag, "type", "destructor"), {}});
            msg = &iface->requests.back();
            en = nullptr;
            continue;
        }

        if (tag.starts_with("<event ")) {
            iface->events.push_back({attr(tag, "name"), "event", false, {}});
            msg = &iface->events.back();
            en = nullptr;
            continue;
        }

        if (tag.starts_with("</request") || tag.starts_with("</event")) {
            msg = nullptr;
            continue;
        }

        if (tag.starts_with("<arg ") && msg) {
            ArgDef a;
            a.name = attr(tag, "name");
            a.type = attr(tag, "type");
            a.interface = attr(tag, "interface");
            a.enum_name = attr(tag, "enum");
            a.summary = attr(tag, "summary");
            a.allow_null = has_attr(tag, "allow-null", "true");
            msg->args.push_back(std::move(a));
            continue;
        }

        if (tag.starts_with("</interface")) {
            iface = nullptr;
            en = nullptr;
            msg = nullptr;
        }
    }

    return proto;
}

inline bool is_cpp_keyword(std::string_view name) {
    static const std::unordered_set<std::string_view> keywords = {
        "alignas",     "alignof",   "and",         "and_eq",     "asm",
        "auto",        "bitand",    "bitor",       "bool",       "break",
        "case",        "catch",     "char",        "char8_t",    "char16_t",
        "char32_t",    "class",     "compl",       "concept",    "const",
        "consteval",   "constexpr", "constinit",   "const_cast", "continue",
        "co_await",    "co_return", "co_yield",    "decltype",   "default",
        "delete",      "do",        "double",      "dynamic_cast",
        "else",        "enum",      "explicit",    "export",     "extern",
        "false",       "float",     "for",         "friend",     "goto",
        "if",          "inline",    "int",         "long",       "mutable",
        "namespace",   "new",       "noexcept",    "not",        "not_eq",
        "nullptr",     "operator",  "or",          "or_eq",      "private",
        "protected",   "public",    "register",    "reinterpret_cast",
        "requires",    "return",    "short",       "signed",     "sizeof",
        "static",      "static_assert", "static_cast", "struct", "switch",
        "template",    "this",      "thread_local","throw",      "true",
        "try",         "typedef",   "typeid",      "typename",   "union",
        "unsigned",    "using",     "virtual",     "void",       "volatile",
        "wchar_t",     "while",     "xor",         "xor_eq",
        "module",      "import",    "override",    "final",
    };
    return keywords.contains(name);
}

inline std::string sanitize(std::string s) {
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    return s;
}

/// Make a valid C++ identifier; keywords and names starting with a digit get an `e_` prefix.
inline std::string safe_ident(std::string name) {
    name = sanitize(std::move(name));
    if (name.empty())
        return "e_unnamed";
    const bool needs_prefix =
        is_cpp_keyword(name) || std::isdigit(static_cast<unsigned char>(name.front()));
    if (needs_prefix)
        return "e_" + name;
    return name;
}

inline std::string safe_enum_entry(std::string name) {
    return safe_ident(std::move(name));
}

inline std::string iface_struct_name(const std::string& iface) {
    return sanitize(iface);
}

inline std::string linkage_symbol_name(const std::string& protocol_name) {
    return "lx_wayland_protocol_" + sanitize(protocol_name) + "_generated";
}

inline void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("cannot write: " + path.string());
    out << content;
}

inline void generate_cppm(const std::filesystem::path& path, const std::string& module_name,
                          const ProtocolDef& proto) {
    std::ostringstream o;
    o << "// Generated by wl-scanner-cpp — do not edit\n";
    o << "module;\n\n";
    o << "export module " << module_name << ";\n\n";
    o << "export namespace lx::wayland::protocols {\n\n";
    o << "inline constexpr const char* protocol_name = \"" << proto.name << "\";\n\n";

    for (const auto& iface : proto.interfaces) {
        const auto iname = iface_struct_name(iface.name);
        o << "struct " << iname << " {\n";
        o << "    static constexpr const char* interface_name = \"" << iface.name << "\";\n";
        o << "    static constexpr int version = " << iface.version << ";\n\n";

        for (const auto& e : iface.enums) {
            o << "    enum class " << safe_ident(e.name) << " : unsigned {\n";
            for (const auto& entry : e.entries)
                o << "        " << safe_enum_entry(entry.name) << " = " << entry.value << ",\n";
            o << "    };\n\n";
        }

        if (!iface.requests.empty()) {
            o << "    enum class request : unsigned {\n";
            unsigned opcode = 0;
            for (const auto& r : iface.requests)
                o << "        " << safe_ident(r.name) << " = " << opcode++ << ",\n";
            o << "    };\n";
            o << "    static constexpr unsigned request_count = " << iface.requests.size() << ";\n\n";
        }

        if (!iface.events.empty()) {
            o << "    enum class event : unsigned {\n";
            unsigned opcode = 0;
            for (const auto& e : iface.events)
                o << "        " << safe_ident(e.name) << " = " << opcode++ << ",\n";
            o << "    };\n";
            o << "    static constexpr unsigned event_count = " << iface.events.size() << ";\n\n";
        }

        o << "};\n\n";
    }

    o << "} // namespace lx::wayland::protocols\n";
    write_file(path, o.str());
}

inline void generate_dispatch_cppm(const std::filesystem::path& path,
                                   const std::string& module_name, const ProtocolDef& proto) {
    std::ostringstream o;
    const std::string dispatch_module = module_name + ".dispatch";
    o << "// Generated by wl-scanner-cpp — do not edit\n";
    o << "module;\n\n";
    o << "#include <cstdint>\n\n";
    o << "export module " << dispatch_module << ";\n\n";
    o << "import " << module_name << ";\n\n";
    o << "export namespace lx::wayland::dispatch {\n\n";

    o << "struct message_info {\n";
    o << "    const char* name;\n";
    o << "    unsigned opcode;\n";
    o << "    unsigned arg_count;\n";
    o << "};\n\n";

    for (const auto& iface : proto.interfaces) {
        const auto iname = iface_struct_name(iface.name);
        o << "struct " << iname << "_requests {\n";
        o << "    static constexpr message_info table[] = {\n";
        unsigned opcode = 0;
        for (const auto& r : iface.requests)
            o << "        {\"" << r.name << "\", " << opcode++ << ", " << r.args.size() << "},\n";
        o << "    };\n";
        o << "    static constexpr unsigned count = " << iface.requests.size() << ";\n";
        o << "};\n\n";

        o << "struct " << iname << "_events {\n";
        o << "    static constexpr message_info table[] = {\n";
        opcode = 0;
        for (const auto& e : iface.events)
            o << "        {\"" << e.name << "\", " << opcode++ << ", " << e.args.size() << "},\n";
        o << "    };\n";
        o << "    static constexpr unsigned count = " << iface.events.size() << ";\n";
        o << "};\n\n";
    }

    // Opcode routing uses libwayland wl_resource_set_implementation vtables;
    // typed message_info tables remain for documentation / tooling.
    o << "} // namespace lx::wayland::dispatch\n";
    write_file(path, o.str());
}

/// Minimal non-module TU for linkage / compile smoke tests (no duplicate interface_name defs).
inline void generate_gen_cpp(const std::filesystem::path& path, const std::string& /*module_name*/,
                             const ProtocolDef& proto) {
    std::ostringstream o;
    o << "// Generated by wl-scanner-cpp — do not edit\n";
    o << "// Linkage marker only; interface_name lives inline in the .cppm.\n";
    o << "const char* " << linkage_symbol_name(proto.name) << " = \"" << proto.name << "\";\n";
    write_file(path, o.str());
}

} // namespace wlgen
