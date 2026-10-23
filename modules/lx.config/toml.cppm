module;

// A strict TOML subset for Lumen's own configuration files (`config.toml`, design-session-
// and-wm Part C; `outputs.toml`, ADR 008 §C.5): comments, `key = value` with basic and
// literal strings, integers, floats, booleans and arrays of those (one line or several,
// trailing comma allowed), `[table]` and `[[array.of.tables]]` headers with dotted names,
// and single-line inline tables (`k = { a = 1, b = "x" }`) for a handler that asks for them.
// Not supported, and reported as an error with its line rather than misread: dotted keys,
// nested inline tables, inline tables in arrays, multi-line strings, dates. Pure: the reader
// streams what it finds to a callback (no document tree, no allocation), the writer escapes.
//
// Moved here from lx.compositor (`:toml_lite`) so that the compositor, the shell and
// lumen-ctl read the configuration with the same code. Member bodies are in toml.cpp
// (Clang 23 module-partition workaround, as lx.compositor's session_lock.cpp).

export module lx.config:toml;

import std;

export namespace lx::config::toml {

/// A scalar or an array of scalars. Strings point into the reader's scratch and are valid
/// for the callback only.
struct value {
    enum class kind : unsigned char { string, integer, floating, boolean, array };
    kind type = kind::string;
    const char* str = "";
    long long integer = 0;
    double floating = 0.0;
    bool boolean = false;
    /// kind::array: the elements (scalars only; nested arrays are an error).
    static constexpr unsigned k_max_items = 32;
    const value* items = nullptr;
    unsigned count = 0;

    [[nodiscard]] bool is_number() const {
        return type == kind::integer || type == kind::floating;
    }
    [[nodiscard]] double number() const {
        return type == kind::integer ? static_cast<double>(integer) : floating;
    }
};

struct parse_error {
    unsigned line = 0;
    char message[96]{};
};

/// What the reader reports, in file order. `err.line` is the line of what is reported; a
/// callback that refuses it returns false after `fail(err, why)`.
struct handler {
    void* user = nullptr;
    /// `[name]` (array false) or `[[name]]` (array true): later keys belong to it. `name` is
    /// the dotted path with quoting removed ("setup.output").
    bool (*table)(void* user, const char* name, bool array, parse_error& err) = nullptr;
    /// `key = value` in the current table ("" at the top), or in the inline table open.
    bool (*key_value)(void* user, const char* key, const value& v, parse_error& err) = nullptr;
    /// `key = { … }`: called with `begin` true and the key, then `key_value` for each of its
    /// members, then with `begin` false (same key). Null: an inline table is a syntax error.
    bool (*inline_table)(void* user, const char* key, bool begin, parse_error& err) = nullptr;
};

/// Reads `text` (`length` bytes, need not be NUL-terminated). False at the first error, with
/// its line (1-based) and what was wrong; everything before it was reported.
[[nodiscard]] bool parse(const char* text, std::size_t length, const handler& h, parse_error& err);
/// Lets a handler refuse what it was given: sets the message (the line is set already).
void fail(parse_error& err, const char* message);

/// Appends TOML text. Strings are written as basic strings with every character TOML
/// requires escaped.
class writer {
public:
    void comment(const char* text);
    void blank();
    void table(const char* name);
    void table_array(const char* name);
    void key(const char* k, const char* string_value);
    void key(const char* k, long long integer_value);
    void key(const char* k, double floating_value);
    void key_bool(const char* k, bool v);
    void key_strings(const char* k, const char* const* items, unsigned count);
    void key_ints(const char* k, const long long* items, unsigned count);
    /// Indents the following lines by two spaces (cosmetic, for nested tables).
    void indent(bool on) { indent_ = on; }
    [[nodiscard]] const std::string& text() const { return out_; }

private:
    void put_key(const char* k);
    void put_string(const char* s);
    std::string out_;
    bool indent_ = false;
};

} // namespace lx::config::toml
