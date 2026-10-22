module;

#include <limits.h>
#include <stdint.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import lx.foundation;
import lx.trace;

export module lx.shell:desktop_entry;

import std;

export namespace lx::shell {

// ── Desktop entries (Desktop Entry Specification 1.5) ─────────────────────────
//
// Pure parsing and Exec expansion: no directory walking (that is `:app_catalog`), no
// Wayland. Every kept string lives in a `string_arena`, so an entry is a set of views that
// stay valid for as long as the catalog generation that owns the arena.

/// Strings for one catalog generation, in chunks that never move: a view handed out stays
/// valid until `clear()` or destruction. Every stored string is NUL-terminated, so
/// `data()` of a stored view is a C string.
class string_arena {
public:
    static constexpr std::size_t k_chunk = 16 * 1024;

    [[nodiscard]] std::string_view store(std::string_view s);
    /// Room for `n` bytes plus a NUL, uninitialised.
    [[nodiscard]] char* allocate(std::size_t n);
    void clear();
    [[nodiscard]] std::size_t bytes() const { return bytes_; }

private:
    struct chunk {
        std::unique_ptr<char[]> data;
        std::size_t size = 0;
        std::size_t used = 0;
    };
    std::vector<chunk> chunks_;
    std::size_t bytes_ = 0;
};

/// The locale a localestring is matched against: LC_ALL, else LC_MESSAGES, else LANG, as
/// `lang_COUNTRY.ENCODING@MODIFIER` with the encoding dropped. Candidates in match order:
/// lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang.
struct locale_keys {
    static constexpr unsigned k_max = 4;
    char key[k_max][64]{};
    unsigned count = 0;

    [[nodiscard]] static locale_keys from(const char* lc_all, const char* lc_messages,
                                          const char* lang);
    [[nodiscard]] static locale_keys from_env();
    /// 0 for the best match … count-1; -1 when `locale` is none of the candidates.
    [[nodiscard]] int rank(std::string_view locale) const;
};

/// $XDG_CURRENT_DESKTOP: colon-separated, compared case-sensitively. Unset or empty means
/// "Lumen".
struct desktop_list {
    static constexpr unsigned k_max = 8;
    char buf[256]{};
    /// Offsets into `buf` (not views: a copied list must not point into the original).
    unsigned short off[k_max]{};
    unsigned short len[k_max]{};
    unsigned count = 0;

    [[nodiscard]] std::string_view name(unsigned i) const { return {buf + off[i], len[i]}; }
    [[nodiscard]] static desktop_list from(const char* xdg_current_desktop);
    [[nodiscard]] static desktop_list from_env();
    [[nodiscard]] bool contains(std::string_view desktop) const;
};

/// Why an entry is not listed (`none`: listed).
enum class entry_skip : unsigned char {
    none,
    not_application,
    hidden,
    no_display,
    not_shown_here,
    try_exec_missing,
    no_exec,
    invalid_exec,
    invalid_file,
    too_large,
};
[[nodiscard]] const char* entry_skip_name(entry_skip s);

struct desktop_entry {
    /// Desktop file ID ("kde-foo.desktop") and the file it came from.
    std::string_view id;
    std::string_view file;
    /// Localised where the spec has a localestring.
    std::string_view name;
    std::string_view generic_name;
    std::string_view comment;
    /// List values (`;`-separated, `\;` still escaped): iterate with `for_each_list_item`.
    std::string_view keywords;
    std::string_view icon;
    /// String-unescaped; quoting and field codes are `expand_exec`'s.
    std::string_view exec;
    std::string_view try_exec;
    std::string_view path;
    std::string_view categories;
    std::string_view only_show_in;
    std::string_view not_show_in;
    std::string_view startup_wm_class;
    /// X-TerminalArgExec (Default Terminal Execution spec), for a terminal entry.
    std::string_view terminal_arg_exec;
    /// ASCII-folded copies for sorting and search; `exec_name` is argv[0]'s basename.
    std::string_view name_fold;
    std::string_view generic_fold;
    std::string_view keywords_fold;
    std::string_view exec_name;
    bool application = false;
    bool terminal = false;
    bool no_display = false;
    bool hidden = false;
    bool dbus_activatable = false;
    entry_skip skip = entry_skip::invalid_file;

    [[nodiscard]] bool listed() const { return skip == entry_skip::none; }
};

/// What `parse_desktop_entry` matches against.
struct parse_context {
    const locale_keys* locale = nullptr;
    const desktop_list* current = nullptr;
    /// $PATH for TryExec; checked only when `check_try_exec`.
    std::string_view path_env;
    bool check_try_exec = true;
};

/// Parses `text` (the whole file) into `out`, storing strings in `arena`. `out.skip` says
/// whether it is listed, and why not. `id` and `file` are stored as given.
void parse_desktop_entry(std::string_view text, std::string_view id, std::string_view file,
                         const parse_context& ctx, string_arena& arena, desktop_entry& out);

/// Calls `fn(item, user)` for each item of a list value, `\;` unescaped. Stops when `fn`
/// returns false. Items are at most 255 bytes (longer ones are cut).
void for_each_list_item(std::string_view list, bool (*fn)(std::string_view item, void* user),
                        void* user);
[[nodiscard]] bool list_contains(std::string_view list, std::string_view item);

/// An expanded Exec line: fixed storage, so building it allocates nothing.
struct exec_argv {
    static constexpr unsigned k_max_args = 64;
    static constexpr std::size_t k_buf = 4096;
    char buf[k_buf]{};
    std::size_t used = 0;
    const char* argv[k_max_args + 1]{};
    unsigned argc = 0;

    void clear() {
        used = 0;
        argc = 0;
        argv[0] = nullptr;
    }
    /// Appends one argument; false when it does not fit.
    [[nodiscard]] bool push(std::string_view arg);
};

/// Exec → argv: quoting (double quotes with \" \` \$ \\; single quotes literally, as GLib
/// and KDE accept), field codes (%f %F %u %U drop their whole token; %i → --icon <Icon>;
/// %c → Name; %k → the file; %% → %; deprecated and unknown codes vanish). argv[0] with a
/// '/' must be absolute. `invalid_argument` errors: unterminated quote, empty, too long,
/// relative path.
[[nodiscard]] lx::result<void> expand_exec(const desktop_entry& e, exec_argv& out);

/// `prog` as an executable regular file: itself when it contains '/', else the first
/// match in the colon-separated `path_env` (empty or relative entries skipped). The
/// absolute path goes to `out`.
[[nodiscard]] bool resolve_program(std::string_view prog, std::string_view path_env,
                                   char (&out)[PATH_MAX]);

/// ASCII lower-case, other bytes as they are.
[[nodiscard]] inline char ascii_fold(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace lx::shell

// ── Implementation ───────────────────────────────────────────────────────────

namespace {

constexpr const char* k_entry_category = "shell.apps";

[[nodiscard]] bool entry_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void entry_log_debug(std::string_view file, const char* what) {
    if (!entry_debug())
        return;
    lx::trace::logger::global().logf(lx::trace::level::debug, k_entry_category,
                                     "desktop file {}: {}", file, what);
}

[[nodiscard]] bool valid_utf8(std::string_view s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        unsigned len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n)
            return false;
        for (unsigned k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] bool key_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

/// String-value escapes: \s \n \t \r \\. With `list`, `\;` stays escaped (the list
/// splitter takes it). Unknown escapes keep both characters.
[[nodiscard]] std::string_view unescape(std::string_view v, bool list, lx::shell::string_arena& a) {
    if (v.find('\\') == std::string_view::npos)
        return a.store(v);
    char* out = a.allocate(v.size());
    std::size_t o = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c != '\\' || i + 1 == v.size()) {
            out[o++] = c;
            continue;
        }
        const char n = v[i + 1];
        char r = 0;
        switch (n) {
        case 's': r = ' '; break;
        case 'n': r = '\n'; break;
        case 't': r = '\t'; break;
        case 'r': r = '\r'; break;
        case '\\': r = '\\'; break;
        default: break;
        }
        if (r && !(list && n == '\\')) {
            out[o++] = r;
            ++i;
        } else if (list && n == '\\') {
            // Kept escaped in a list, so "\\;" is a backslash and a separator.
            out[o++] = '\\';
            out[o++] = '\\';
            ++i;
        } else {
            out[o++] = c; // unknown escape (or \; in a list): both characters
        }
    }
    out[o] = '\0';
    return {out, o};
}

[[nodiscard]] std::string_view fold(std::string_view s, lx::shell::string_arena& a) {
    char* out = a.allocate(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        out[i] = lx::shell::ascii_fold(s[i]);
    out[s.size()] = '\0';
    return {out, s.size()};
}

/// The keys an entry keeps; localestrings take the best-ranked locale.
enum key_id : unsigned {
    k_type,
    k_name,
    k_generic_name,
    k_comment,
    k_keywords,
    k_icon,
    k_exec,
    k_try_exec,
    k_path,
    k_terminal,
    k_no_display,
    k_hidden,
    k_only_show_in,
    k_not_show_in,
    k_categories,
    k_startup_wm_class,
    k_dbus_activatable,
    k_terminal_arg_exec,
    k_key_count,
};

struct key_def {
    std::string_view name;
    bool localestring;
    bool list;
};

constexpr key_def k_keys[k_key_count] = {
    {"Type", false, false},
    {"Name", true, false},
    {"GenericName", true, false},
    {"Comment", true, false},
    {"Keywords", true, true},
    {"Icon", false, false},
    {"Exec", false, false},
    {"TryExec", false, false},
    {"Path", false, false},
    {"Terminal", false, false},
    {"NoDisplay", false, false},
    {"Hidden", false, false},
    {"OnlyShowIn", false, true},
    {"NotShowIn", false, true},
    {"Categories", false, true},
    {"StartupWMClass", false, false},
    {"DBusActivatable", false, false},
    {"X-TerminalArgExec", false, false},
};

[[nodiscard]] int find_key(std::string_view k) {
    for (unsigned i = 0; i < k_key_count; ++i)
        if (k_keys[i].name == k)
            return static_cast<int>(i);
    return -1;
}

[[nodiscard]] bool parse_bool(std::string_view v, std::string_view file, const char* key) {
    if (v == "true")
        return true;
    if (v != "false" && entry_debug()) {
        char what[96];
        std::snprintf(what, sizeof(what), "%s is not true or false: read as false", key);
        entry_log_debug(file, what);
    }
    return false;
}

struct list_hit {
    const lx::shell::desktop_list* current;
    bool hit;
};

[[nodiscard]] bool any_current(std::string_view list, const lx::shell::desktop_list& current) {
    list_hit h{&current, false};
    lx::shell::for_each_list_item(
        list,
        [](std::string_view item, void* user) {
            auto& hh = *static_cast<list_hit*>(user);
            if (hh.current->contains(item)) {
                hh.hit = true;
                return false;
            }
            return true;
        },
        &h);
    return h.hit;
}

[[nodiscard]] std::string_view basename_of(std::string_view p) {
    const std::size_t s = p.rfind('/');
    return s == std::string_view::npos ? p : p.substr(s + 1);
}

} // namespace

// ── string_arena ──

char* lx::shell::string_arena::allocate(std::size_t n) {
    const std::size_t need = n + 1;
    if (chunks_.empty() || chunks_.back().size - chunks_.back().used < need) {
        const std::size_t size = need > k_chunk ? need : k_chunk;
        chunks_.push_back({std::make_unique_for_overwrite<char[]>(size), size, 0});
    }
    chunk& c = chunks_.back();
    char* p = c.data.get() + c.used;
    c.used += need;
    bytes_ += need;
    return p;
}

std::string_view lx::shell::string_arena::store(std::string_view s) {
    char* p = allocate(s.size());
    if (!s.empty())
        std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return {p, s.size()};
}

void lx::shell::string_arena::clear() {
    chunks_.clear();
    bytes_ = 0;
}

// ── locale_keys ──

lx::shell::locale_keys lx::shell::locale_keys::from(const char* lc_all, const char* lc_messages,
                                                    const char* lang) {
    locale_keys k{};
    const char* v = (lc_all && *lc_all) ? lc_all
                    : (lc_messages && *lc_messages) ? lc_messages
                                                    : lang;
    if (!v || !*v)
        return k;
    std::string_view s{v};
    if (s == "C" || s == "POSIX" || s.starts_with("C."))
        return k;
    std::string_view modifier;
    if (const std::size_t at = s.find('@'); at != std::string_view::npos) {
        modifier = s.substr(at + 1);
        s = s.substr(0, at);
    }
    if (const std::size_t dot = s.find('.'); dot != std::string_view::npos)
        s = s.substr(0, dot);
    std::string_view language = s;
    std::string_view country;
    if (const std::size_t us = s.find('_'); us != std::string_view::npos) {
        language = s.substr(0, us);
        country = s.substr(us + 1);
    }
    if (language.empty() || language.size() > 20 || country.size() > 20 || modifier.size() > 20)
        return k;
    auto add = [&](std::string_view a, std::string_view b, std::string_view c) {
        std::snprintf(k.key[k.count], sizeof(k.key[0]), "%.*s%s%.*s%s%.*s",
                      static_cast<int>(a.size()), a.data(), b.empty() ? "" : "_",
                      static_cast<int>(b.size()), b.data(), c.empty() ? "" : "@",
                      static_cast<int>(c.size()), c.data());
        ++k.count;
    };
    if (!country.empty() && !modifier.empty())
        add(language, country, modifier);
    if (!country.empty())
        add(language, country, {});
    if (!modifier.empty())
        add(language, {}, modifier);
    add(language, {}, {});
    return k;
}

lx::shell::locale_keys lx::shell::locale_keys::from_env() {
    return from(std::getenv("LC_ALL"), std::getenv("LC_MESSAGES"), std::getenv("LANG"));
}

int lx::shell::locale_keys::rank(std::string_view locale) const {
    for (unsigned i = 0; i < count; ++i)
        if (locale == key[i])
            return static_cast<int>(i);
    return -1;
}

// ── desktop_list ──

lx::shell::desktop_list lx::shell::desktop_list::from(const char* v) {
    desktop_list d{};
    if (!v || !*v)
        v = "Lumen";
    std::snprintf(d.buf, sizeof(d.buf), "%s", v);
    std::string_view s{d.buf};
    while (!s.empty() && d.count < k_max) {
        const std::size_t c = s.find(':');
        const std::string_view item = s.substr(0, c);
        if (!item.empty()) {
            d.off[d.count] = static_cast<unsigned short>(item.data() - d.buf);
            d.len[d.count] = static_cast<unsigned short>(item.size());
            ++d.count;
        }
        if (c == std::string_view::npos)
            break;
        s.remove_prefix(c + 1);
    }
    return d;
}

lx::shell::desktop_list lx::shell::desktop_list::from_env() {
    return from(std::getenv("XDG_CURRENT_DESKTOP"));
}

bool lx::shell::desktop_list::contains(std::string_view desktop) const {
    for (unsigned i = 0; i < count; ++i)
        if (name(i) == desktop)
            return true;
    return false;
}

const char* lx::shell::entry_skip_name(entry_skip s) {
    switch (s) {
    case entry_skip::none: return "listed";
    case entry_skip::not_application: return "not an application";
    case entry_skip::hidden: return "Hidden";
    case entry_skip::no_display: return "NoDisplay";
    case entry_skip::not_shown_here: return "OnlyShowIn/NotShowIn";
    case entry_skip::try_exec_missing: return "TryExec not found";
    case entry_skip::no_exec: return "no Exec";
    case entry_skip::invalid_exec: return "invalid Exec";
    case entry_skip::invalid_file: return "invalid file";
    case entry_skip::too_large: return "too large";
    }
    return "?";
}

// ── Lists ──

void lx::shell::for_each_list_item(std::string_view list,
                                   bool (*fn)(std::string_view item, void* user), void* user) {
    char item[256];
    std::size_t n = 0;
    bool pending = false;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const char c = list[i];
        if (c == '\\' && i + 1 < list.size() && (list[i + 1] == ';' || list[i + 1] == '\\')) {
            if (n < sizeof(item))
                item[n++] = list[i + 1];
            ++i;
            pending = true;
            continue;
        }
        if (c == ';') {
            if (!fn({item, n}, user))
                return;
            n = 0;
            pending = false;
            continue;
        }
        if (n < sizeof(item))
            item[n++] = c;
        pending = true;
    }
    // The trailing ';' is optional.
    if (pending)
        (void)fn({item, n}, user);
}

bool lx::shell::list_contains(std::string_view list, std::string_view want) {
    struct ctx {
        std::string_view want;
        bool hit;
    } c{want, false};
    for_each_list_item(
        list,
        [](std::string_view item, void* user) {
            auto& cc = *static_cast<ctx*>(user);
            if (item == cc.want) {
                cc.hit = true;
                return false;
            }
            return true;
        },
        &c);
    return c.hit;
}

// ── Program lookup ──

bool lx::shell::resolve_program(std::string_view prog, std::string_view path_env,
                                char (&out)[PATH_MAX]) {
    auto usable = [](const char* p) {
        struct stat st{};
        return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
    };
    if (prog.empty() || prog.size() >= PATH_MAX)
        return false;
    if (prog.find('/') != std::string_view::npos) {
        if (prog.front() != '/')
            return false;
        std::memcpy(out, prog.data(), prog.size());
        out[prog.size()] = '\0';
        return usable(out);
    }
    while (!path_env.empty()) {
        const std::size_t c = path_env.find(':');
        const std::string_view dir = path_env.substr(0, c);
        path_env.remove_prefix(c == std::string_view::npos ? path_env.size() : c + 1);
        // Empty and relative entries would search the working directory: skipped.
        if (dir.empty() || dir.front() != '/' || dir.size() + 1 + prog.size() >= PATH_MAX)
            continue;
        std::memcpy(out, dir.data(), dir.size());
        std::size_t n = dir.size();
        if (out[n - 1] != '/')
            out[n++] = '/';
        std::memcpy(out + n, prog.data(), prog.size());
        out[n + prog.size()] = '\0';
        if (usable(out))
            return true;
    }
    return false;
}

// ── Exec ──

bool lx::shell::exec_argv::push(std::string_view arg) {
    if (argc >= k_max_args || used + arg.size() + 1 > k_buf)
        return false;
    char* p = buf + used;
    if (!arg.empty())
        std::memcpy(p, arg.data(), arg.size());
    p[arg.size()] = '\0';
    used += arg.size() + 1;
    argv[argc++] = p;
    argv[argc] = nullptr;
    return true;
}

lx::result<void> lx::shell::expand_exec(const desktop_entry& e, exec_argv& out) {
    out.clear();
    const std::string_view s = e.exec;
    auto too_long = [] {
        return lx::make_error(lx::error_domain::invalid_argument, 0, "Exec too long");
    };
    // One token at a time into `tok`; `drop` when a %f/%F/%u/%U was in it.
    char tok[exec_argv::k_buf];
    std::size_t tn = 0;
    bool in_token = false;
    bool drop = false;
    bool explicit_empty = false; // "" or '' written out: kept as an empty argument
    bool expanded = false;       // a field code expanded inside this token
    auto put = [&](char c) {
        if (tn < sizeof(tok))
            tok[tn++] = c;
    };
    auto put_sv = [&](std::string_view v) {
        for (char c : v)
            put(c);
    };
    auto end_token = [&]() -> bool {
        if (!in_token)
            return true;
        const bool keep = !drop && (tn > 0 || (explicit_empty && !expanded));
        bool ok = true;
        if (keep)
            ok = out.push({tok, tn});
        tn = 0;
        in_token = drop = explicit_empty = expanded = false;
        return ok;
    };
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!end_token())
                return too_long();
            ++i;
            continue;
        }
        in_token = true;
        if (c == '"') {
            ++i;
            explicit_empty = true;
            bool closed = false;
            while (i < s.size()) {
                const char d = s[i];
                if (d == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                if (d == '\\' && i + 1 < s.size() &&
                    (s[i + 1] == '"' || s[i + 1] == '`' || s[i + 1] == '$' || s[i + 1] == '\\')) {
                    put(s[i + 1]);
                    i += 2;
                    continue;
                }
                if (d == '%' && i + 1 < s.size() && s[i + 1] == '%') {
                    put('%');
                    i += 2;
                    continue;
                }
                put(d);
                ++i;
            }
            if (!closed)
                return lx::make_error(lx::error_domain::invalid_argument, 0,
                                      "Exec: unterminated double quote");
            continue;
        }
        if (c == '\'') {
            // Not in the spec; GLib and KDE accept it and real entries use it (sh -c '…').
            ++i;
            explicit_empty = true;
            const std::size_t close = s.find('\'', i);
            if (close == std::string_view::npos)
                return lx::make_error(lx::error_domain::invalid_argument, 0,
                                      "Exec: unterminated single quote");
            put_sv(s.substr(i, close - i));
            i = close + 1;
            continue;
        }
        if (c == '\\' && i + 1 < s.size()) {
            put(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '%' && i + 1 < s.size()) {
            const char code = s[i + 1];
            const bool whole = tn == 0 && !explicit_empty &&
                               (i + 2 == s.size() || s[i + 2] == ' ' || s[i + 2] == '\t');
            i += 2;
            switch (code) {
            case 'f':
            case 'F':
            case 'u':
            case 'U':
                drop = true; // no files or URLs from a launcher: the whole token goes
                break;
            case 'i':
                if (whole) {
                    if (!e.icon.empty() && (!out.push("--icon") || !out.push(e.icon)))
                        return too_long();
                    in_token = false; // consumed as its own arguments
                } else {
                    expanded = true;
                }
                break;
            case 'c':
                put_sv(e.name);
                expanded = true;
                break;
            case 'k':
                put_sv(e.file);
                expanded = true;
                break;
            case '%':
                put('%');
                break;
            case 'd':
            case 'D':
            case 'n':
            case 'N':
            case 'v':
            case 'm':
                expanded = true; // deprecated: removed
                break;
            default:
                expanded = true;
                entry_log_debug(e.file, "unknown field code in Exec: removed");
                break;
            }
            continue;
        }
        put(c);
        ++i;
    }
    if (!end_token())
        return too_long();
    if (out.argc == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "Exec is empty");
    const std::string_view a0{out.argv[0]};
    if (a0.find('/') != std::string_view::npos && a0.front() != '/')
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "Exec: a program path must be absolute");
    return {};
}

// ── Parsing ──

void lx::shell::parse_desktop_entry(std::string_view text, std::string_view id,
                                    std::string_view file, const parse_context& ctx,
                                    string_arena& arena, desktop_entry& out) {
    out = {};
    out.id = arena.store(id);
    out.file = arena.store(file);
    out.skip = entry_skip::invalid_file;
    if (!valid_utf8(text)) {
        entry_log_debug(file, "not UTF-8: skipped");
        return;
    }
    std::string_view value[k_key_count]{};
    int rank[k_key_count];
    for (int& r : rank)
        r = INT_MAX;
    const int unlocalised = ctx.locale ? static_cast<int>(ctx.locale->count) : 0;
    bool in_group = false;   // inside [Desktop Entry]
    bool seen_group = false; // any group header seen
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string_view::npos) {
                entry_log_debug(file, "malformed group header: skipped");
                return;
            }
            const std::string_view group = line.substr(1, close - 1);
            if (!seen_group && group != "Desktop Entry") {
                entry_log_debug(file, "first group is not [Desktop Entry]: skipped");
                return;
            }
            if (seen_group && group == "Desktop Entry") {
                entry_log_debug(file, "duplicate [Desktop Entry]: skipped");
                return;
            }
            in_group = group == "Desktop Entry";
            seen_group = true;
            if (!in_group)
                break; // actions and extension groups follow; nothing more is read
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (!trim(line).empty()) {
                entry_log_debug(file, "a line that is neither key, group nor comment: skipped");
                return;
            }
            continue;
        }
        if (!seen_group) {
            entry_log_debug(file, "a key before any group: skipped");
            return;
        }
        std::string_view key = trim(line.substr(0, eq));
        const std::string_view val = trim(line.substr(eq + 1));
        std::string_view locale;
        if (const std::size_t lb = key.find('['); lb != std::string_view::npos) {
            if (key.back() != ']')
                continue;
            locale = key.substr(lb + 1, key.size() - lb - 2);
            key = key.substr(0, lb);
        }
        bool ok_key = !key.empty();
        for (char c : key)
            ok_key = ok_key && key_char(c);
        if (!ok_key)
            continue;
        const int k = find_key(key);
        if (k < 0)
            continue;
        int r = unlocalised;
        if (!locale.empty()) {
            if (!k_keys[k].localestring || !ctx.locale)
                continue;
            r = ctx.locale->rank(locale);
            if (r < 0)
                continue;
        }
        if (r == rank[k]) {
            entry_log_debug(file, "duplicate key: the first one wins");
            continue;
        }
        if (r > rank[k])
            continue;
        rank[k] = r;
        value[k] = val;
    }
    if (!seen_group) {
        entry_log_debug(file, "no [Desktop Entry] group: skipped");
        return;
    }
    auto str = [&](unsigned k) {
        return rank[k] == INT_MAX ? std::string_view{} : unescape(value[k], k_keys[k].list, arena);
    };
    const std::string_view type = rank[k_type] == INT_MAX ? std::string_view{} : value[k_type];
    out.application = type == "Application";
    out.name = str(k_name);
    out.generic_name = str(k_generic_name);
    out.comment = str(k_comment);
    out.keywords = str(k_keywords);
    out.icon = str(k_icon);
    out.exec = str(k_exec);
    out.try_exec = str(k_try_exec);
    out.path = str(k_path);
    out.categories = str(k_categories);
    out.only_show_in = str(k_only_show_in);
    out.not_show_in = str(k_not_show_in);
    out.startup_wm_class = str(k_startup_wm_class);
    out.terminal_arg_exec = str(k_terminal_arg_exec);
    out.terminal = rank[k_terminal] != INT_MAX && parse_bool(value[k_terminal], file, "Terminal");
    out.no_display =
        rank[k_no_display] != INT_MAX && parse_bool(value[k_no_display], file, "NoDisplay");
    out.hidden = rank[k_hidden] != INT_MAX && parse_bool(value[k_hidden], file, "Hidden");
    out.dbus_activatable = rank[k_dbus_activatable] != INT_MAX &&
                           parse_bool(value[k_dbus_activatable], file, "DBusActivatable");
    out.name_fold = fold(out.name, arena);
    out.generic_fold = fold(out.generic_name, arena);
    out.keywords_fold = fold(out.keywords, arena);

    if (type.empty() || out.name.empty()) {
        entry_log_debug(file, "Type or Name missing: skipped");
        out.skip = entry_skip::invalid_file;
        return;
    }
    if (!out.application) {
        out.skip = entry_skip::not_application;
        return;
    }
    if (out.hidden) {
        out.skip = entry_skip::hidden;
        return;
    }
    if (out.exec.empty()) {
        // DBusActivatable-only entries are not launched (D-Bus activation is out of scope).
        out.skip = entry_skip::no_exec;
        return;
    }
    {
        static exec_argv probe; // one thread (the shell's); 4.6 KiB kept off the stack
        if (auto ok = expand_exec(out, probe); !ok) {
            entry_log_debug(file, ok.get_error().message);
            out.skip = entry_skip::invalid_exec;
            return;
        }
        out.exec_name = fold(basename_of(probe.argv[0]), arena);
    }
    if (out.no_display) {
        out.skip = entry_skip::no_display;
        return;
    }
    if (ctx.current) {
        if (!out.only_show_in.empty() && !any_current(out.only_show_in, *ctx.current)) {
            out.skip = entry_skip::not_shown_here;
            return;
        }
        if (!out.not_show_in.empty() && any_current(out.not_show_in, *ctx.current)) {
            out.skip = entry_skip::not_shown_here;
            return;
        }
    }
    if (ctx.check_try_exec && !out.try_exec.empty()) {
        char resolved[PATH_MAX];
        if (!resolve_program(out.try_exec, ctx.path_env, resolved)) {
            out.skip = entry_skip::try_exec_missing;
            return;
        }
    }
    out.skip = entry_skip::none;
}
