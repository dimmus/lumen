module;

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

export module lx.ui:theme.compile;

import std;
import lx.foundation;
import lx.gfx;

import :theme;
import :theme_defaults;

// ── The theme compiler (ADR-010 §4.8) ─────────────────────────────────────────────────
//
// Turns a theme family in Zed's JSON schema (https://zed.dev/schema/themes/v0.2.0.json:
// `{ "name", "author", "themes": [ { "name", "appearance", "style": { … } } ] }`) into the
// `.lxtheme` format `theme::load` reads. Build-time code — `tools/lumen-theme` runs it — so
// it uses std containers freely; nothing here runs on a frame.
//
// The JSON reader is strict RFC 8259 (no comments, no trailing commas, no duplicate keys in
// an object, nesting at most 64 deep, input at most 16 MiB). A theme is refined the way Zed
// refines it (crates/theme_settings/src/theme_settings.rs `refine_theme`): a colour key the
// file leaves out, sets to null or to a string that is not a colour takes a derived rule
// where one exists, so a Zed theme resolves as it does in Zed (a status background from its
// colour at 25 % opacity, the active scrollbar thumb from the thumb, version-control colours
// from the status colours, the selection from player 0, …) and otherwise the value of its
// appearance in Lumen's base theme (`base_theme`, theme_defaults.cppm: One Dark / One Light
// from assets/themes/one/one.json plus Lumen's own picks). Players merge over the base
// theme's eight. Every fallback is reported, and
// keys the compiler does not keep (vim mode colours, accents, window background appearance,
// keys it does not know) are counted as ignored.

export namespace lx::ui {

struct theme_source {
    const char* input_path = "";
    /// Where `compile` writes the result (written to `<path>.tmp`, then renamed over it).
    /// Empty: not written.
    const char* output_path = "";
    /// The theme of the family to compile, by its `name`; null or empty: the first.
    const char* theme_name = "";
};

/// What a Zed theme does not carry: Lumen's defaults (§5.3–§5.4).
struct compile_options {
    density default_density = density::standard;
    const char* ui_family = "IBM Plex Sans";
    float ui_size_px = 16.f;
    float ui_line_height = k_phi;
    const char* buffer_family = "Lilex";
    float buffer_size_px = 15.f;
    float buffer_line_height = k_phi;
    float buffer_line_height_standard = 1.3f;
    /// The linear-light text coverage exponent; 0 takes the appearance's
    /// (`gfx::k_text_contrast_dark` / `_light`).
    float text_contrast = 0.f;
};

struct compile_report {
    std::string name{};
    appearance mode = appearance::dark;
    /// The names of every theme in the family.
    std::vector<std::string> themes{};
    unsigned tokens_from_source = 0;
    unsigned tokens_derived = 0;   ///< from a Zed derived rule
    unsigned tokens_from_base = 0; ///< from Lumen's base theme of the appearance
    unsigned invalid_colors = 0;   ///< strings that are not colours (treated as missing)
    unsigned ignored_keys = 0;
    /// One line per token that fell back: "key: <rule>".
    std::vector<std::string> fallbacks{};
    /// The keys counted in `ignored_keys`.
    std::vector<std::string> ignored{};
    std::size_t bytes = 0;
    const char* output_path = "";
    bool ok = false;
};

/// Compiles Zed theme JSON into `.lxtheme` (invoked by `lumen-theme`).
class theme_compiler {
public:
    explicit theme_compiler(compile_options options = {});

    /// Reads `source.input_path`, compiles the selected theme, writes `source.output_path`.
    /// Errors: io (reading or writing), `ui_err::theme_source` (not JSON, not a theme family,
    /// no such theme, no appearance; `last_error_offset` has the JSON byte offset).
    [[nodiscard]] lx::result<compile_report> compile(const theme_source& source) const;
    /// The same from JSON text in memory, the compiled bytes into `out`.
    [[nodiscard]] lx::result<compile_report> compile_json(std::string_view json,
                                                          const char* theme_name,
                                                          std::vector<std::uint8_t>& out) const;
    [[nodiscard]] lx::result<theme> load_compiled(const char* compiled_path) const;
    /// The byte offset of the last JSON syntax error (0 when the last failure was not one).
    [[nodiscard]] std::size_t last_error_offset() const { return error_offset_; }

private:
    compile_options options_{};
    mutable std::size_t error_offset_ = 0;
};

/// C++ source for theme_builtin.cpp: the two compiled themes as byte arrays behind
/// `builtin_theme_bytes` (`lumen-theme --emit-cpp`).
[[nodiscard]] std::string builtin_theme_source(std::span<const std::uint8_t> dark,
                                               std::span<const std::uint8_t> light,
                                               std::string_view origin);

/// "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa" (GPUI's `Rgba::try_from`) as straight sRGB
/// floats; empty when it is not one.
[[nodiscard]] std::optional<std::array<float, 4>> parse_theme_color(std::string_view text);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

// ── JSON ──

struct lx_json_member;

struct lx_json {
    enum class kind : std::uint8_t { null, boolean, number, string, array, object };
    kind k = kind::null;
    bool b = false;
    double n = 0.0;
    std::string s{};
    std::vector<lx_json> items;
    std::vector<lx_json_member> members;

    [[nodiscard]] const lx_json* get(std::string_view key) const;
};

struct lx_json_member {
    std::string first{};
    lx_json second{};
};

const lx_json* lx_json::get(std::string_view key) const {
    if (k != kind::object)
        return nullptr;
    for (const auto& [name, value] : members)
        if (name == key)
            return &value;
    return nullptr;
}

class lx_json_reader {
public:
    explicit lx_json_reader(std::string_view text) : t_{text} {}

    [[nodiscard]] bool parse(lx_json& out) {
        skip();
        if (!value(out, 0))
            return false;
        skip();
        if (p_ != t_.size())
            return fail("trailing characters after the JSON value");
        return true;
    }
    [[nodiscard]] std::size_t offset() const { return p_; }
    [[nodiscard]] const char* message() const { return message_; }

private:
    static constexpr unsigned k_max_depth = 64;

    bool fail(const char* m) {
        if (!message_)
            message_ = m;
        return false;
    }
    void skip() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r'))
            ++p_;
    }
    bool literal(std::string_view word) {
        if (t_.substr(p_, word.size()) != word)
            return fail("invalid literal");
        p_ += word.size();
        return true;
    }
    bool value(lx_json& out, unsigned depth) {
        if (depth > k_max_depth)
            return fail("nesting too deep");
        if (p_ >= t_.size())
            return fail("unexpected end of input");
        switch (t_[p_]) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"':
            out.k = lx_json::kind::string;
            return string(out.s);
        case 't':
            out.k = lx_json::kind::boolean;
            out.b = true;
            return literal("true");
        case 'f':
            out.k = lx_json::kind::boolean;
            out.b = false;
            return literal("false");
        case 'n':
            out.k = lx_json::kind::null;
            return literal("null");
        default: return number(out);
        }
    }
    bool object(lx_json& out, unsigned depth) {
        out.k = lx_json::kind::object;
        ++p_;
        skip();
        if (p_ < t_.size() && t_[p_] == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            skip();
            if (p_ >= t_.size() || t_[p_] != '"')
                return fail("expected a string key");
            std::string key;
            if (!string(key))
                return false;
            for (const auto& m : out.members)
                if (m.first == key)
                    return fail("duplicate key in an object");
            skip();
            if (p_ >= t_.size() || t_[p_] != ':')
                return fail("expected ':'");
            ++p_;
            skip();
            out.members.push_back({std::move(key), lx_json{}});
            if (!value(out.members.back().second, depth + 1))
                return false;
            skip();
            if (p_ < t_.size() && t_[p_] == ',') {
                ++p_;
                continue;
            }
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
    bool array(lx_json& out, unsigned depth) {
        out.k = lx_json::kind::array;
        ++p_;
        skip();
        if (p_ < t_.size() && t_[p_] == ']') {
            ++p_;
            return true;
        }
        for (;;) {
            skip();
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1))
                return false;
            skip();
            if (p_ < t_.size() && t_[p_] == ',') {
                ++p_;
                continue;
            }
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
    static int hex(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
    bool hex4(std::uint32_t& out) {
        if (p_ + 4 > t_.size())
            return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex(t_[p_++]);
            if (h < 0)
                return fail("bad \\u escape");
            out = (out << 4) | static_cast<std::uint32_t>(h);
        }
        return true;
    }
    static void utf8(std::string& s, std::uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool string(std::string& out) {
        ++p_; // the opening quote
        for (;;) {
            if (p_ >= t_.size())
                return fail("unterminated string");
            const auto c = static_cast<unsigned char>(t_[p_]);
            if (c == '"') {
                ++p_;
                return true;
            }
            if (c < 0x20)
                return fail("control character in a string");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++p_;
                continue;
            }
            ++p_;
            if (p_ >= t_.size())
                return fail("unterminated escape");
            const char e = t_[p_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    std::uint32_t lo = 0;
                    if (t_.substr(p_, 2) != "\\u")
                        return fail("unpaired surrogate");
                    p_ += 2;
                    if (!hex4(lo))
                        return false;
                    if (lo < 0xDC00 || lo >= 0xE000)
                        return fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("unpaired surrogate");
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }
    bool number(lx_json& out) {
        const std::size_t start = p_;
        if (p_ < t_.size() && t_[p_] == '-')
            ++p_;
        if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9')
            return fail("invalid value");
        if (t_[p_] == '0') {
            ++p_;
        } else {
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9')
                ++p_;
        }
        if (p_ < t_.size() && t_[p_] == '.') {
            ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9')
                return fail("invalid number");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9')
                ++p_;
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-'))
                ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9')
                return fail("invalid number");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9')
                ++p_;
        }
        out.k = lx_json::kind::number;
        const auto r = std::from_chars(t_.data() + start, t_.data() + p_, out.n);
        if (r.ec != std::errc{})
            return fail("number out of range");
        return true;
    }

    std::string_view t_;
    std::size_t p_ = 0;
    const char* message_ = nullptr;
};

// ── colours ──

using rgba = std::array<float, 4>;

[[nodiscard]] rgba lx_ui_opacity(rgba c, float f) {
    c[3] *= std::clamp(f, 0.f, 1.f);
    return c;
}

[[nodiscard]] rgba lx_ui_with_alpha(rgba c, float a) {
    c[3] = std::clamp(a, 0.f, 1.f);
    return c;
}

/// GPUI's `Hsla::blend`: `over` on top of `under`, keeping `under`'s alpha.
[[nodiscard]] rgba lx_ui_blend(rgba under, rgba over) {
    const float a = over[3];
    if (a >= 1.f)
        return over;
    if (a <= 0.f)
        return under;
    return {under[0] * (1.f - a) + over[0] * a, under[1] * (1.f - a) + over[1] * a,
            under[2] * (1.f - a) + over[2] * a, under[3]};
}

[[nodiscard]] std::uint8_t lx_ui_byte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
}

[[nodiscard]] std::uint32_t lx_ui_pack(rgba c) {
    return static_cast<std::uint32_t>(lx_ui_byte(c[0])) |
           (static_cast<std::uint32_t>(lx_ui_byte(c[1])) << 8) |
           (static_cast<std::uint32_t>(lx_ui_byte(c[2])) << 16) |
           (static_cast<std::uint32_t>(lx_ui_byte(c[3])) << 24);
}

/// The compile of one theme: source values, refinements, then the tables.
struct lx_ui_theme_build {
    lx::ui::appearance mode = lx::ui::appearance::dark;
    std::array<std::optional<rgba>, lx::ui::k_token_count> src{};
    std::array<rgba, lx::ui::k_token_count> out{};
    std::array<std::uint8_t, lx::ui::k_token_count> origin{}; // 0 source, 1 derived, 2 base
    std::array<lx::ui::theme_file_syntax, lx::ui::k_syntax_count> syntax{};
    lx::ui::compile_report* report = nullptr;

    [[nodiscard]] static constexpr unsigned idx(lx::ui::tok t) { return static_cast<unsigned>(t); }
    [[nodiscard]] const std::optional<rgba>& s(lx::ui::tok t) const { return src[idx(t)]; }

    [[nodiscard]] rgba base(lx::ui::tok t) const {
        const std::string_view key = lx::ui::token_key(t);
        const bool light = mode == lx::ui::appearance::light;
        for (const auto& e : lx::ui::base_theme::k_colors)
            if (key == e.key)
                return *lx::ui::parse_theme_color(light ? e.light : e.dark);
        if (t >= lx::ui::tok::player0_cursor && t < lx::ui::tok::syntax_attribute) {
            const unsigned i = idx(t) - idx(lx::ui::tok::player0_cursor);
            return *lx::ui::parse_theme_color(lx::ui::base_theme::k_players[light ? 1 : 0][i / 3][i % 3]);
        }
        return {0.f, 0.f, 0.f, 0.f};
    }

    void set(lx::ui::tok t, rgba c, std::uint8_t from, std::string_view rule = {}) {
        out[idx(t)] = c;
        origin[idx(t)] = from;
        if (from != 0 && report)
            report->fallbacks.push_back(std::string{lx::ui::token_key(t)} + ": " + std::string{rule});
    }

    /// The source value, else `derived` (with its rule), else the base theme's.
    void resolve(lx::ui::tok t, std::optional<rgba> derived = std::nullopt,
                 std::string_view rule = {}) {
        if (s(t))
            set(t, *s(t), 0);
        else if (derived)
            set(t, *derived, 1, rule);
        else
            set(t, base(t), 2, "base theme");
    }
};

void lx_ui_put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t v) {
    std::memcpy(out.data() + at, &v, 2);
}

} // namespace

std::optional<std::array<float, 4>> lx::ui::parse_theme_color(std::string_view text) {
    if (text.empty() || text[0] != '#')
        return std::nullopt;
    text.remove_prefix(1);
    const auto hx = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int v[8]{};
    for (std::size_t i = 0; i < text.size() && i < 8; ++i)
        if ((v[i] = hx(text[i])) < 0)
            return std::nullopt;
    std::array<float, 4> c{0.f, 0.f, 0.f, 1.f};
    switch (text.size()) {
    case 3:
    case 4:
        for (std::size_t i = 0; i < text.size(); ++i)
            c[i] = static_cast<float>(v[i] * 17) / 255.f;
        return c;
    case 6:
    case 8:
        for (std::size_t i = 0; i < text.size() / 2; ++i)
            c[i] = static_cast<float>(v[2 * i] * 16 + v[2 * i + 1]) / 255.f;
        return c;
    default:
        return std::nullopt;
    }
}

lx::ui::theme_compiler::theme_compiler(compile_options options) : options_{options} {}

lx::result<lx::ui::theme> lx::ui::theme_compiler::load_compiled(const char* compiled_path) const {
    return theme::load(compiled_path);
}

lx::result<lx::ui::compile_report>
lx::ui::theme_compiler::compile_json(std::string_view json, const char* theme_name,
                                     std::vector<std::uint8_t>& out) const {
    error_offset_ = 0;
    if (json.size() > (std::size_t{16} << 20))
        return lx::make_error(lx::ui_err::theme_source, "theme source: larger than 16 MiB");
    lx_json root{};
    lx_json_reader reader{json};
    if (!reader.parse(root)) {
        error_offset_ = reader.offset();
        return lx::make_error(lx::ui_err::theme_source, reader.message());
    }
    const lx_json* themes = root.get("themes");
    if (!themes || themes->k != lx_json::kind::array || themes->items.empty())
        return lx::make_error(lx::ui_err::theme_source,
                              "theme source: not a Zed theme family (no \"themes\" array)");

    compile_report report{};
    const lx_json* chosen = nullptr;
    for (const lx_json& t : themes->items) {
        const lx_json* n = t.get("name");
        const std::string name = n && n->k == lx_json::kind::string ? n->s : std::string{};
        report.themes.push_back(name);
        if (!chosen && (!theme_name || !theme_name[0] || name == theme_name))
            chosen = &t;
    }
    if (!chosen)
        return lx::make_error(lx::ui_err::theme_source, "theme source: no theme of that name");
    const lx_json* name = chosen->get("name");
    const lx_json* app = chosen->get("appearance");
    const lx_json* style = chosen->get("style");
    if (!name || name->k != lx_json::kind::string || name->s.size() >= 64)
        return lx::make_error(lx::ui_err::theme_source, "theme source: missing or long \"name\"");
    if (!app || app->k != lx_json::kind::string || (app->s != "dark" && app->s != "light"))
        return lx::make_error(lx::ui_err::theme_source,
                              "theme source: \"appearance\" must be \"dark\" or \"light\"");
    if (!style || style->k != lx_json::kind::object)
        return lx::make_error(lx::ui_err::theme_source, "theme source: missing \"style\" object");
    report.name = name->s;
    report.mode = app->s == "light" ? appearance::light : appearance::dark;
    for (const auto& [key, value] : chosen->members)
        if (key != "name" && key != "appearance" && key != "style") {
            ++report.ignored_keys;
            report.ignored.push_back(key);
        }

    lx_ui_theme_build b{};
    b.mode = report.mode;
    b.report = &report;

    const auto colour_of = [&](const lx_json& v) -> std::optional<rgba> {
        if (v.k == lx_json::kind::null)
            return std::nullopt;
        if (v.k == lx_json::kind::string)
            if (auto c = parse_theme_color(v.s))
                return *c;
        ++report.invalid_colors;
        return std::nullopt;
    };

    // ── source values ──
    const lx_json* players = nullptr;
    const lx_json* syntax = nullptr;
    std::optional<rgba> deprecated_thumb{};
    std::optional<rgba> deprecated_ours{};
    std::optional<rgba> deprecated_theirs{};
    for (const auto& [key, value] : style->members) {
        if (key == "players") {
            players = &value;
            continue;
        }
        if (key == "syntax") {
            syntax = &value;
            continue;
        }
        if (key == "scrollbar_thumb.background") {
            deprecated_thumb = colour_of(value);
            continue;
        }
        if (key == "version_control_conflict_ours_background") {
            deprecated_ours = colour_of(value);
            continue;
        }
        if (key == "version_control_conflict_theirs_background") {
            deprecated_theirs = colour_of(value);
            continue;
        }
        const tok t = token_from_key(key);
        if (t == tok::none || t >= tok::player0_cursor) {
            ++report.ignored_keys;
            report.ignored.push_back(key);
            continue;
        }
        b.src[lx_ui_theme_build::idx(t)] = colour_of(value);
    }
    if (!b.s(tok::scrollbar_thumb_background) && deprecated_thumb)
        b.src[lx_ui_theme_build::idx(tok::scrollbar_thumb_background)] = deprecated_thumb;
    if (!b.s(tok::version_control_conflict_marker_ours) && deprecated_ours)
        b.src[lx_ui_theme_build::idx(tok::version_control_conflict_marker_ours)] = deprecated_ours;
    if (!b.s(tok::version_control_conflict_marker_theirs) && deprecated_theirs)
        b.src[lx_ui_theme_build::idx(tok::version_control_conflict_marker_theirs)] = deprecated_theirs;

    // ── status: a missing background of six statuses is their colour at 25 % ──
    std::array<std::optional<rgba>, k_token_count> status_ref = b.src;
    for (status_kind k : {status_kind::deleted, status_kind::created, status_kind::modified,
                          status_kind::conflict, status_kind::error, status_kind::hidden}) {
        const status_colors st = status_tokens(k);
        auto& bg = status_ref[lx_ui_theme_build::idx(st.background)];
        const auto& fg = status_ref[lx_ui_theme_build::idx(st.color)];
        if (!bg && fg)
            bg = lx_ui_opacity(*fg, 0.25f);
    }
    for (unsigned k = 0; k < k_status_count; ++k) {
        const status_colors st = status_tokens(static_cast<status_kind>(k));
        for (tok t : {st.color, st.background, st.border}) {
            const auto& v = status_ref[lx_ui_theme_build::idx(t)];
            if (b.s(t))
                b.resolve(t);
            else if (v)
                b.set(t, *v, 1, "status colour at 25 % opacity");
            else
                b.resolve(t);
        }
    }

    // ── players merge over the base eight ──
    std::array<std::optional<rgba>, 3 * k_player_count> player_src{};
    if (players && players->k == lx_json::kind::array) {
        for (std::size_t i = 0; i < players->items.size(); ++i) {
            const lx_json& p = players->items[i];
            if (i >= k_player_count || p.k != lx_json::kind::object) {
                ++report.ignored_keys;
                report.ignored.push_back("players[" + std::to_string(i) + "]");
                continue;
            }
            const char* fields[3] = {"cursor", "background", "selection"};
            for (unsigned f = 0; f < 3; ++f)
                if (const lx_json* v = p.get(fields[f]))
                    player_src[3 * i + f] = colour_of(*v);
        }
    } else if (players) {
        ++report.ignored_keys;
        report.ignored.push_back("players");
    }
    for (unsigned i = 0; i < 3 * k_player_count; ++i) {
        const auto t = static_cast<tok>(lx_ui_theme_build::idx(tok::player0_cursor) + i);
        b.src[lx_ui_theme_build::idx(t)] = player_src[i];
        b.resolve(t);
    }

    // ── theme colours: Zed's derived rules, else the base ──
    const auto src = [&](tok t) { return b.s(t); };
    const auto opaque = [](std::optional<rgba> c) -> std::optional<rgba> {
        if (c)
            (*c)[3] = 1.f;
        return c;
    };
    const auto non_opaque = [](std::optional<rgba> c) -> std::optional<rgba> {
        if (c && (*c)[3] > 0.7f)
            (*c)[3] = 0.7f;
        return c;
    };
    const auto stat = [&](tok t) { return status_ref[lx_ui_theme_build::idx(t)]; };
    const bool light = report.mode == appearance::light;
    const float hunk_fill = light ? 0.16f : 0.12f;
    const float hunk_hollow_bg = light ? 0.08f : 0.06f;
    const float hunk_hollow_border = light ? 0.48f : 0.36f;
    const auto fade = [](std::optional<rgba> c, float f) -> std::optional<rgba> {
        if (c)
            return lx_ui_opacity(*c, f);
        return c;
    };
    const std::optional<rgba> vc_added = src(tok::version_control_added)
                                             ? src(tok::version_control_added)
                                             : stat(tok::created);
    const std::optional<rgba> vc_deleted = src(tok::version_control_deleted)
                                               ? src(tok::version_control_deleted)
                                               : stat(tok::deleted);
    const std::optional<rgba> thumb_active = src(tok::scrollbar_thumb_active_background)
                                                 ? src(tok::scrollbar_thumb_active_background)
                                                 : src(tok::scrollbar_thumb_background);
    std::optional<rgba> overlay_hover{};
    if (src(tok::panel_background) && src(tok::element_hover))
        overlay_hover = opaque(lx_ui_blend(*src(tok::panel_background), *src(tok::element_hover)));
    // GPUI's `apply_theme_color_defaults`: the selection is player 0's, at 25 % when opaque.
    rgba selection = b.out[lx_ui_theme_build::idx(tok::player0_selection)];
    if (selection[3] >= 1.f)
        selection[3] = 0.25f;

    struct derived_rule {
        tok token;
        std::optional<rgba> value;
        const char* rule;
    };
    const derived_rule rules[] = {
        {tok::element_selection_background, selection, "player 0 selection"},
        {tok::panel_overlay_background, opaque(src(tok::panel_background)), "panel.background, opaque"},
        {tok::panel_overlay_hover, overlay_hover, "panel.background + element.hover, opaque"},
        {tok::pane_group_border, src(tok::border), "border"},
        {tok::scrollbar_thumb_active_background, src(tok::scrollbar_thumb_background),
         "scrollbar.thumb.background"},
        {tok::minimap_thumb_background, non_opaque(src(tok::scrollbar_thumb_background)),
         "scrollbar.thumb.background, alpha <= 0.7"},
        {tok::minimap_thumb_hover_background, non_opaque(src(tok::scrollbar_thumb_hover_background)),
         "scrollbar.thumb.hover_background, alpha <= 0.7"},
        {tok::minimap_thumb_active_background, non_opaque(thumb_active),
         "scrollbar.thumb.active_background, alpha <= 0.7"},
        {tok::minimap_thumb_border, src(tok::scrollbar_thumb_border), "scrollbar.thumb.border"},
        {tok::search_active_match_background, src(tok::search_match_background),
         "search.match_background"},
        {tok::editor_document_highlight_bracket_background,
         src(tok::editor_document_highlight_read_background),
         "editor.document_highlight.read_background"},
        {tok::editor_diff_hunk_added_background, fade(vc_added, hunk_fill), "version_control.added, faded"},
        {tok::editor_diff_hunk_added_hollow_background, fade(vc_added, hunk_hollow_bg),
         "version_control.added, faded"},
        {tok::editor_diff_hunk_added_hollow_border, fade(vc_added, hunk_hollow_border),
         "version_control.added, faded"},
        {tok::editor_diff_hunk_deleted_background, fade(vc_deleted, hunk_fill),
         "version_control.deleted, faded"},
        {tok::editor_diff_hunk_deleted_hollow_background, fade(vc_deleted, hunk_hollow_bg),
         "version_control.deleted, faded"},
        {tok::editor_diff_hunk_deleted_hollow_border, fade(vc_deleted, hunk_hollow_border),
         "version_control.deleted, faded"},
        {tok::version_control_added, stat(tok::created), "created"},
        {tok::version_control_deleted, stat(tok::deleted), "deleted"},
        {tok::version_control_modified, stat(tok::modified), "modified"},
        {tok::version_control_renamed, stat(tok::modified), "modified"},
        {tok::version_control_conflict, stat(tok::ignored), "ignored"},
        {tok::version_control_ignored, stat(tok::ignored), "ignored"},
    };
    // The colour tokens before the status triples (resolved above).
    for (unsigned i = 0; i < lx_ui_theme_build::idx(tok::conflict); ++i) {
        const auto t = static_cast<tok>(i);
        const derived_rule* r = nullptr;
        for (const derived_rule& d : rules)
            if (d.token == t)
                r = &d;
        if (r)
            b.resolve(t, r->value, r->rule);
        else
            b.resolve(t);
    }

    // ── syntax ──
    for (unsigned i = 0; i < k_syntax_count; ++i) {
        const tok t = syntax_token(i);
        const std::string_view key = token_key(t).substr(7); // after "syntax."
        theme_file_syntax& st = b.syntax[i];
        st.color = static_cast<std::uint16_t>(t);
        const lx_json* entry = nullptr;
        if (syntax && syntax->k == lx_json::kind::object)
            entry = syntax->get(key);
        std::optional<rgba> colour{};
        if (entry && entry->k == lx_json::kind::object) {
            if (const lx_json* c = entry->get("color"))
                colour = colour_of(*c);
            if (const lx_json* w = entry->get("font_weight"); w && w->k == lx_json::kind::number) {
                st.weight = static_cast<std::uint16_t>(std::clamp(w->n, 1.0, 1000.0));
            }
            if (const lx_json* s = entry->get("font_style"); s && s->k == lx_json::kind::string) {
                st.flags |= 2u;
                st.style = s->s == "italic" ? 1 : (s->s == "oblique" ? 2 : 0);
            }
            if (const lx_json* bg = entry->get("background_color"))
                if (auto c = colour_of(*bg)) {
                    st.flags |= 4u;
                    st.background = lx_ui_pack(*c);
                }
        }
        if (colour) {
            st.flags |= 1u;
            b.set(t, *colour, 0);
        } else {
            b.set(t, b.out[lx_ui_theme_build::idx(tok::editor_foreground)], 1, "editor.foreground");
        }
    }
    if (syntax && syntax->k == lx_json::kind::object) {
        for (const auto& [key, value] : syntax->members)
            if (token_from_key("syntax." + key) == tok::none) {
                ++report.ignored_keys;
                report.ignored.push_back("syntax." + key);
            }
    } else if (syntax) {
        ++report.ignored_keys;
        report.ignored.push_back("syntax");
    }

    for (unsigned i = 0; i < k_token_count; ++i) {
        if (b.origin[i] == 0)
            ++report.tokens_from_source;
        else if (b.origin[i] == 1)
            ++report.tokens_derived;
        else
            ++report.tokens_from_base;
    }

    // ── the file ──
    const std::size_t srgb_at = sizeof(theme_file_header);
    const std::size_t linear_at = srgb_at + 4u * k_token_count;
    const std::size_t status_at = linear_at + 8u * k_token_count;
    const std::size_t player_at = status_at + 6u * k_status_count;
    const std::size_t syntax_at = (player_at + 6u * k_player_count + 3u) & ~std::size_t{3};
    const std::size_t fonts_at = syntax_at + sizeof(theme_file_syntax) * k_syntax_count;
    const std::size_t total = fonts_at + sizeof(theme_file_fonts);
    out.assign(total, 0);

    theme_file_header h{};
    h.file_bytes = static_cast<std::uint32_t>(total);
    h.appearance = light ? 1 : 0;
    h.density = static_cast<std::uint8_t>(options_.default_density);
    h.token_count = static_cast<std::uint16_t>(k_token_count);
    h.status_count = static_cast<std::uint16_t>(k_status_count);
    h.player_count = static_cast<std::uint16_t>(k_player_count);
    h.syntax_count = static_cast<std::uint16_t>(k_syntax_count);
    h.srgb_offset = static_cast<std::uint32_t>(srgb_at);
    h.linear_offset = static_cast<std::uint32_t>(linear_at);
    h.status_offset = static_cast<std::uint32_t>(status_at);
    h.player_offset = static_cast<std::uint32_t>(player_at);
    h.syntax_offset = static_cast<std::uint32_t>(syntax_at);
    h.fonts_offset = static_cast<std::uint32_t>(fonts_at);
    std::memcpy(h.name, report.name.data(), report.name.size());

    for (unsigned i = 0; i < k_token_count; ++i) {
        const std::uint32_t packed = lx_ui_pack(b.out[i]);
        std::memcpy(out.data() + srgb_at + 4u * i, &packed, 4);
        // The linear table from the same bytes, so both encodings are one colour.
        const float a = static_cast<float>(packed >> 24) / 255.f;
        const auto lin = [&](unsigned shift) {
            return lx::srgb_to_linear(static_cast<float>((packed >> shift) & 0xFFu) / 255.f) * a;
        };
        lx_ui_put16(out, linear_at + 8u * i, lx::gfx::to_half(lin(0)));
        lx_ui_put16(out, linear_at + 8u * i + 2, lx::gfx::to_half(lin(8)));
        lx_ui_put16(out, linear_at + 8u * i + 4, lx::gfx::to_half(lin(16)));
        lx_ui_put16(out, linear_at + 8u * i + 6, lx::gfx::to_half(a));
    }
    for (unsigned k = 0; k < k_status_count; ++k) {
        const status_colors st = status_tokens(static_cast<status_kind>(k));
        lx_ui_put16(out, status_at + 6u * k, static_cast<std::uint16_t>(st.color));
        lx_ui_put16(out, status_at + 6u * k + 2, static_cast<std::uint16_t>(st.background));
        lx_ui_put16(out, status_at + 6u * k + 4, static_cast<std::uint16_t>(st.border));
    }
    for (unsigned p = 0; p < k_player_count; ++p) {
        const player_colors pc = player_tokens(p);
        lx_ui_put16(out, player_at + 6u * p, static_cast<std::uint16_t>(pc.cursor));
        lx_ui_put16(out, player_at + 6u * p + 2, static_cast<std::uint16_t>(pc.background));
        lx_ui_put16(out, player_at + 6u * p + 4, static_cast<std::uint16_t>(pc.selection));
    }
    std::memcpy(out.data() + syntax_at, b.syntax.data(), sizeof(theme_file_syntax) * k_syntax_count);

    theme_file_fonts f{};
    const auto name_copy = [](char (&dst)[64], const char* s) {
        const std::size_t n = s ? std::min<std::size_t>(std::strlen(s), 63) : 0;
        if (n)
            std::memcpy(dst, s, n);
    };
    name_copy(f.ui_family, options_.ui_family);
    name_copy(f.buffer_family, options_.buffer_family);
    f.ui_size_px = options_.ui_size_px;
    f.ui_line_height = options_.ui_line_height;
    f.buffer_size_px = options_.buffer_size_px;
    f.buffer_line_height = options_.buffer_line_height;
    f.buffer_line_height_standard = options_.buffer_line_height_standard;
    f.text_contrast = options_.text_contrast > 0.f
                          ? options_.text_contrast
                          : (light ? lx::gfx::k_text_contrast_light : lx::gfx::k_text_contrast_dark);
    std::memcpy(out.data() + fonts_at, &f, sizeof(f));

    h.checksum = theme_checksum({out.data() + sizeof(h), total - sizeof(h)});
    std::memcpy(out.data(), &h, sizeof(h));

    report.bytes = total;
    report.ok = true;
    return report;
}

lx::result<lx::ui::compile_report>
lx::ui::theme_compiler::compile(const theme_source& source) const {
    error_offset_ = 0;
    if (!source.input_path || !source.input_path[0])
        return lx::invalid_argument("theme_compiler: no input path");
    std::string json;
    {
        const int fd = ::open(source.input_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return lx::make_error(errno == ENOENT ? lx::io_err::not_found
                                                  : lx::io_err::permission_denied,
                                  "theme_compiler: cannot open the input");
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                ::close(fd);
                return lx::make_error(lx::io_err::broken_pipe, "theme_compiler: read failed");
            }
            if (n == 0)
                break;
            json.append(buf, static_cast<std::size_t>(n));
            if (json.size() > (std::size_t{16} << 20))
                break;
        }
        ::close(fd);
    }
    std::vector<std::uint8_t> bytes;
    auto report = compile_json(json, source.theme_name, bytes);
    if (!report)
        return report;
    report->output_path = source.output_path ? source.output_path : "";
    if (!source.output_path || !source.output_path[0])
        return report;
    // Written beside the target and renamed over it: a watcher never reads half a file.
    const std::string tmp = std::string{source.output_path} + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return lx::make_error(lx::io_err::permission_denied, "theme_compiler: cannot create the output");
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += static_cast<std::size_t>(n);
    }
    const bool closed = ::close(fd) == 0;
    if (done != bytes.size() || !closed || ::rename(tmp.c_str(), source.output_path) != 0) {
        ::unlink(tmp.c_str());
        return lx::make_error(lx::io_err::broken_pipe, "theme_compiler: writing the output failed");
    }
    return report;
}

std::string lx::ui::builtin_theme_source(std::span<const std::uint8_t> dark,
                                         std::span<const std::uint8_t> light,
                                         std::string_view origin) {
    std::string s;
    s += "// Generated by `lumen-theme --emit-cpp` from ";
    s += origin;
    s += ".\n// Do not edit: rerun the tool (see tools/lumen-theme/main.cpp). test_ui_theme checks that\n"
         "// these bytes are what the build compiles from the bundled theme files.\n\n"
         "module lx.ui;\n\nimport std;\n\nnamespace {\n\n";
    const auto array = [&s](const char* name, std::span<const std::uint8_t> bytes) {
        s += "alignas(8) constexpr std::uint8_t ";
        s += name;
        s += "[] = {";
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i % 16 == 0)
                s += "\n   ";
            char b[8];
            std::snprintf(b, sizeof(b), " 0x%02x,", bytes[i]);
            s += b;
        }
        s += "\n};\n\n";
    };
    array("k_dark", dark);
    array("k_light", light);
    s += "} // namespace\n\n"
         "std::span<const std::uint8_t> lx::ui::builtin_theme_bytes(appearance a) {\n"
         "    if (a == appearance::light)\n"
         "        return k_light;\n"
         "    return k_dark;\n"
         "}\n";
    return s;
}
