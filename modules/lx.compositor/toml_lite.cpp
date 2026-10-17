module;

// The TOML subset reader and writer (`:toml_lite`). See the partition for what is accepted.

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

module lx.compositor;

import :toml_lite;

namespace lx::compositor::toml {

namespace {

constexpr unsigned k_scratch = 8192;

struct reader {
    const char* p = nullptr;
    const char* end = nullptr;
    unsigned line = 1;
    parse_error* err = nullptr;
    /// Strings of the value being read (and of the key), NUL-separated.
    char scratch[k_scratch]{};
    unsigned used = 0;
    value items[value::k_max_items]{};

    [[nodiscard]] bool at_end() const { return p >= end; }
    [[nodiscard]] char peek(unsigned ahead = 0) const {
        return p + ahead < end ? p[ahead] : '\0';
    }
    bool error(const char* msg) {
        err->line = line;
        std::snprintf(err->message, sizeof(err->message), "%s", msg);
        return false;
    }
    void skip_space() {
        while (!at_end() && (*p == ' ' || *p == '\t'))
            ++p;
    }
    /// Space, then an optional comment; stops before the line break.
    void skip_space_comment() {
        skip_space();
        if (!at_end() && *p == '#')
            while (!at_end() && *p != '\n')
                ++p;
    }
    /// At a line break (or the end): consumes it. False when something else is there.
    bool end_line() {
        skip_space_comment();
        if (at_end())
            return true;
        if (*p == '\r' && peek(1) == '\n')
            ++p;
        if (*p != '\n')
            return false;
        ++p;
        ++line;
        return true;
    }
    /// Space, comments and line breaks (inside arrays).
    void skip_blank() {
        for (;;) {
            skip_space_comment();
            if (at_end())
                return;
            if (*p == '\r' && peek(1) == '\n')
                ++p;
            if (*p != '\n')
                return;
            ++p;
            ++line;
        }
    }
    char* alloc(unsigned n) {
        if (used + n > k_scratch)
            return nullptr;
        char* out = scratch + used;
        used += n;
        return out;
    }
};

[[nodiscard]] bool bare_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

void put_utf8(char*& w, unsigned long cp) {
    if (cp < 0x80) {
        *w++ = static_cast<char>(cp);
    } else if (cp < 0x800) {
        *w++ = static_cast<char>(0xc0 | (cp >> 6));
        *w++ = static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *w++ = static_cast<char>(0xe0 | (cp >> 12));
        *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *w++ = static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        *w++ = static_cast<char>(0xf0 | (cp >> 18));
        *w++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        *w++ = static_cast<char>(0x80 | (cp & 0x3f));
    }
}

/// A basic ("…") or literal ('…') string at `r.p`, into scratch. Null on error.
const char* read_string(reader& r) {
    const char quote = *r.p;
    if (r.peek(1) == quote && r.peek(2) == quote) {
        (void)r.error("multi-line strings are not supported");
        return nullptr;
    }
    ++r.p;
    // Worst case: every byte kept, plus the NUL (escapes only shrink, \u grows ≤ 4 from 6).
    const char* start = r.p;
    unsigned n = 0;
    while (start + n < r.end && start[n] != quote && start[n] != '\n')
        n += (quote == '"' && start[n] == '\\' && start + n + 1 < r.end) ? 2 : 1;
    char* out = r.alloc(n + 1);
    if (!out) {
        (void)r.error("value too long");
        return nullptr;
    }
    char* w = out;
    for (;;) {
        if (r.at_end() || *r.p == '\n' || *r.p == '\r') {
            (void)r.error("unterminated string");
            return nullptr;
        }
        const char c = *r.p;
        if (c == quote) {
            ++r.p;
            break;
        }
        const auto uc = static_cast<unsigned char>(c);
        if ((uc < 0x20 && c != '\t') || uc == 0x7f) {
            (void)r.error("control character in a string");
            return nullptr;
        }
        if (quote == '\'' || c != '\\') {
            *w++ = c;
            ++r.p;
            continue;
        }
        ++r.p;
        const char e = r.peek();
        ++r.p;
        switch (e) {
        case 'b': *w++ = '\b'; break;
        case 't': *w++ = '\t'; break;
        case 'n': *w++ = '\n'; break;
        case 'f': *w++ = '\f'; break;
        case 'r': *w++ = '\r'; break;
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case 'u':
        case 'U': {
            const unsigned digits = e == 'u' ? 4u : 8u;
            unsigned long cp = 0;
            for (unsigned i = 0; i < digits; ++i) {
                const char h = r.peek();
                unsigned v = 0;
                if (h >= '0' && h <= '9')
                    v = static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f')
                    v = static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    v = static_cast<unsigned>(h - 'A' + 10);
                else {
                    (void)r.error("bad \\u escape");
                    return nullptr;
                }
                cp = cp * 16 + v;
                ++r.p;
            }
            if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp == 0) {
                (void)r.error("bad \\u escape");
                return nullptr;
            }
            put_utf8(w, cp);
            break;
        }
        default:
            (void)r.error("unknown escape in a string");
            return nullptr;
        }
    }
    *w = '\0';
    return out;
}

/// A key or one part of a table name: bare or quoted. Null on error.
const char* read_key(reader& r) {
    if (r.at_end())
        return nullptr;
    if (*r.p == '"' || *r.p == '\'')
        return read_string(r);
    const char* start = r.p;
    while (!r.at_end() && bare_char(*r.p))
        ++r.p;
    const auto n = static_cast<unsigned>(r.p - start);
    if (n == 0) {
        (void)r.error("expected a key");
        return nullptr;
    }
    char* out = r.alloc(n + 1);
    if (!out) {
        (void)r.error("key too long");
        return nullptr;
    }
    std::memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

bool read_number(reader& r, value& v) {
    // Copy the token without underscores (allowed only between digits).
    char tok[64];
    unsigned n = 0;
    const char* s = r.p;
    char prev = '\0';
    while (!r.at_end()) {
        const char c = *r.p;
        if (!(bare_char(c) || c == '+' || c == '.'))
            break;
        if (c == '_') {
            if (!(prev >= '0' && prev <= '9') || !(r.peek(1) >= '0' && r.peek(1) <= '9'))
                return r.error("misplaced '_' in a number");
        } else {
            if (n + 1 >= sizeof(tok))
                return r.error("number too long");
            tok[n++] = c;
        }
        prev = c;
        ++r.p;
    }
    tok[n] = '\0';
    (void)s;
    const char* digits = tok + ((tok[0] == '+' || tok[0] == '-') ? 1 : 0);
    if (!std::strcmp(digits, "inf") || !std::strcmp(digits, "nan")) {
        v.type = value::kind::floating;
        v.floating = digits[0] == 'i' ? (tok[0] == '-' ? -HUGE_VAL : HUGE_VAL) : std::nan("");
        return true;
    }
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
        if (digits != tok)
            return r.error("a sign before a hex, octal or binary number");
        const int base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
        char* e = nullptr;
        const long long x = std::strtoll(digits + 2, &e, base);
        if (!e || *e || e == digits + 2)
            return r.error("bad number");
        v.type = value::kind::integer;
        v.integer = x;
        return true;
    }
    if (!(digits[0] >= '0' && digits[0] <= '9'))
        return r.error("bad value");
    if (digits[0] == '0' && digits[1] >= '0' && digits[1] <= '9')
        return r.error("leading zero in a number");
    const bool is_float = std::strpbrk(digits, ".eE") != nullptr;
    char* e = nullptr;
    if (is_float) {
        const char* dot = std::strchr(digits, '.');
        if (dot && !(dot[1] >= '0' && dot[1] <= '9'))
            return r.error("a float needs digits after '.'");
        const double d = std::strtod(tok, &e);
        if (!e || *e)
            return r.error("bad number");
        v.type = value::kind::floating;
        v.floating = d;
        return true;
    }
    const long long x = std::strtoll(tok, &e, 10);
    if (!e || *e)
        return r.error("bad number");
    v.type = value::kind::integer;
    v.integer = x;
    return true;
}

bool read_scalar(reader& r, value& v) {
    const char c = r.peek();
    if (c == '"' || c == '\'') {
        const char* s = read_string(r);
        if (!s)
            return false;
        v.type = value::kind::string;
        v.str = s;
        return true;
    }
    if (c == '{')
        return r.error("inline tables are not supported");
    if (c == '[')
        return r.error("nested arrays are not supported");
    if (r.end - r.p >= 4 && !std::strncmp(r.p, "true", 4) && !bare_char(r.peek(4))) {
        r.p += 4;
        v.type = value::kind::boolean;
        v.boolean = true;
        return true;
    }
    if (r.end - r.p >= 5 && !std::strncmp(r.p, "false", 5) && !bare_char(r.peek(5))) {
        r.p += 5;
        v.type = value::kind::boolean;
        v.boolean = false;
        return true;
    }
    if (c == '+' || c == '-' || (c >= '0' && c <= '9') || c == 'i' || c == 'n')
        return read_number(r, v);
    return r.error("expected a value");
}

bool read_value(reader& r, value& v) {
    if (r.peek() != '[')
        return read_scalar(r, v);
    ++r.p;
    v = value{};
    v.type = value::kind::array;
    v.items = r.items;
    unsigned n = 0;
    for (;;) {
        r.skip_blank();
        if (r.at_end())
            return r.error("unterminated array");
        if (*r.p == ']') {
            ++r.p;
            break;
        }
        if (n >= value::k_max_items)
            return r.error("array too long");
        if (!read_scalar(r, r.items[n]))
            return false;
        ++n;
        r.skip_blank();
        if (r.peek() == ',') {
            ++r.p;
            continue;
        }
        if (r.peek() == ']') {
            ++r.p;
            break;
        }
        return r.error("expected ',' or ']' in an array");
    }
    v.count = n;
    return true;
}

} // namespace

void fail(parse_error& err, const char* message) {
    std::snprintf(err.message, sizeof(err.message), "%s", message ? message : "invalid");
}

bool parse(const char* text, std::size_t length, const handler& h, parse_error& err) {
    err = parse_error{};
    reader r{};
    r.p = text;
    r.end = text + length;
    r.err = &err;
    // A UTF-8 byte-order mark is allowed at the very start.
    if (length >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        r.p += 3;
    while (!r.at_end()) {
        r.used = 0;
        r.skip_space_comment();
        if (r.at_end())
            break;
        if (*r.p == '\n' || *r.p == '\r') {
            if (!r.end_line())
                return r.error("stray carriage return");
            continue;
        }
        if (*r.p == '[') {
            const bool array = r.peek(1) == '[';
            r.p += array ? 2 : 1;
            char name[256];
            unsigned len = 0;
            for (;;) {
                r.skip_space();
                const char* part = read_key(r);
                if (!part)
                    return r.error("bad table name");
                const auto pl = static_cast<unsigned>(std::strlen(part));
                if (len + pl + 2 > sizeof(name))
                    return r.error("table name too long");
                if (len)
                    name[len++] = '.';
                std::memcpy(name + len, part, pl);
                len += pl;
                r.skip_space();
                if (r.peek() == '.') {
                    ++r.p;
                    continue;
                }
                break;
            }
            name[len] = '\0';
            if (r.peek() != ']' || (array && r.peek(1) != ']'))
                return r.error(array ? "expected ']]'" : "expected ']'");
            r.p += array ? 2 : 1;
            if (!r.end_line())
                return r.error("text after a table header");
            err.line = r.line - 1;
            if (h.table && !h.table(h.user, name, array, err))
                return false;
            continue;
        }
        const unsigned key_line = r.line;
        const char* key = read_key(r);
        if (!key)
            return false;
        r.skip_space();
        if (r.peek() == '.')
            return r.error("dotted keys are not supported");
        if (r.peek() != '=')
            return r.error("expected '=' after a key");
        ++r.p;
        r.skip_space();
        value v{};
        if (!read_value(r, v))
            return false;
        if (!r.end_line())
            return r.error("text after a value");
        err.line = key_line;
        if (h.key_value && !h.key_value(h.user, key, v, err))
            return false;
    }
    err = parse_error{};
    return true;
}

// ── writer ──

void writer::comment(const char* text) {
    if (indent_)
        out_ += "  ";
    out_ += "# ";
    out_ += text ? text : "";
    out_ += '\n';
}

void writer::blank() { out_ += '\n'; }

void writer::table(const char* name) {
    if (indent_)
        out_ += "  ";
    out_ += '[';
    out_ += name;
    out_ += "]\n";
}

void writer::table_array(const char* name) {
    if (indent_)
        out_ += "  ";
    out_ += "[[";
    out_ += name;
    out_ += "]]\n";
}

void writer::put_key(const char* k) {
    if (indent_)
        out_ += "  ";
    bool bare = k && *k;
    for (const char* c = k; bare && *c; ++c)
        bare = bare_char(*c);
    if (bare)
        out_ += k;
    else
        put_string(k ? k : "");
    out_ += " = ";
}

void writer::put_string(const char* s) {
    out_ += '"';
    for (const char* c = s; *c; ++c) {
        const auto uc = static_cast<unsigned char>(*c);
        switch (*c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\t': out_ += "\\t"; break;
        case '\r': out_ += "\\r"; break;
        default:
            if (uc < 0x20 || uc == 0x7f) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", uc);
                out_ += esc;
            } else {
                out_ += *c;
            }
        }
    }
    out_ += '"';
}

void writer::key(const char* k, const char* s) {
    put_key(k);
    put_string(s ? s : "");
    out_ += '\n';
}

void writer::key(const char* k, long long v) {
    put_key(k);
    out_ += std::to_string(v);
    out_ += '\n';
}

void writer::key(const char* k, double v) {
    put_key(k);
    char buf[64];
    // Shortest text that reads back as the same double.
    auto res = std::to_chars(buf, buf + sizeof(buf) - 3, v);
    *res.ptr = '\0';
    if (!std::strpbrk(buf, ".eEn")) // TOML floats need a '.' or an exponent (inf/nan aside)
        std::strcat(buf, ".0");
    out_ += buf;
    out_ += '\n';
}

void writer::key_bool(const char* k, bool v) {
    put_key(k);
    out_ += v ? "true" : "false";
    out_ += '\n';
}

void writer::key_strings(const char* k, const char* const* items, unsigned count) {
    put_key(k);
    out_ += '[';
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            out_ += ", ";
        put_string(items[i] ? items[i] : "");
    }
    out_ += "]\n";
}

void writer::key_ints(const char* k, const long long* items, unsigned count) {
    put_key(k);
    out_ += '[';
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            out_ += ", ";
        out_ += std::to_string(items[i]);
    }
    out_ += "]\n";
}

} // namespace lx::compositor::toml
