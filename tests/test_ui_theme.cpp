// ADR-010 §4.8 and §5: the theme compiler over the bundled Zed One themes, the .lxtheme
// format's checks, the built-in fallback, the packed style and its resolution per density,
// elevation and radius, styled boxes, theme changes, the theme watcher, and Lucide icons
// through the atlas.
//
// The icon tests need librsvg (dlopen'd) and the bundled SVGs; without them they print SKIP
// and pass.

#include "lumen_test.hpp"

#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.ui;

#ifndef LUMEN_THEME_SOURCE_JSON
#define LUMEN_THEME_SOURCE_JSON "assets/themes/one/one.json"
#endif
#ifndef LUMEN_TEST_THEME_DIR
#define LUMEN_TEST_THEME_DIR "build/release/themes"
#endif

namespace {

using lx::ui::tok;

[[nodiscard]] std::string read_file(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[65536];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const char* path) {
    const std::string s = read_file(path);
    return {s.begin(), s.end()};
}

/// "#rrggbb" or "#rrggbbaa" as the rgba8 the theme stores (R in the low byte).
[[nodiscard]] std::uint32_t hex(std::string_view h) {
    const auto c = lx::ui::parse_theme_color(h);
    LUMEN_CHECK(c.has_value());
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(std::lround((*c)[i] * 255.f)) << (8 * i);
    return v;
}

[[nodiscard]] std::vector<std::uint8_t> compile(const char* name, lx::ui::compile_report* report = nullptr) {
    static const std::string json = read_file(LUMEN_THEME_SOURCE_JSON);
    LUMEN_CHECK(!json.empty());
    std::vector<std::uint8_t> bytes;
    const lx::ui::theme_compiler c{};
    auto r = c.compile_json(json, name, bytes);
    LUMEN_CHECK(r.has_value());
    if (report)
        *report = *r;
    return bytes;
}

[[nodiscard]] lx::ui::theme theme_of(const std::vector<std::uint8_t>& bytes) {
    auto t = lx::ui::theme::from_bytes(bytes);
    LUMEN_CHECK(t.has_value());
    return std::move(*t);
}

/// One theme's section of one.json: from its "name" to the next theme's.
[[nodiscard]] std::string_view section(const std::string& json, const char* name) {
    const std::string needle = std::string{"\"name\": \""} + name + "\"";
    const std::size_t at = json.find(needle);
    LUMEN_CHECK(at != std::string::npos);
    const std::size_t next = json.find("\"appearance\"", json.find("\"appearance\"", at) + 1);
    return std::string_view{json}.substr(at, next == std::string::npos ? std::string::npos : next - at);
}

/// The string value of `"key": "..."` in `text` (at `from` or after), or empty.
[[nodiscard]] std::string_view value_of(std::string_view text, std::string_view key, std::size_t from = 0) {
    const std::string needle = "\"" + std::string{key} + "\": \"";
    const std::size_t at = text.find(needle, from);
    if (at == std::string_view::npos)
        return {};
    const std::size_t start = at + needle.size();
    return text.substr(start, text.find('"', start) - start);
}

struct expect {
    tok t;
    const char* hex;
};

/// ADR-010 §5.1, One Dark.
constexpr expect k_one_dark[] = {
    {tok::background, "#3b414d"},
    {tok::surface_background, "#2f343e"},
    {tok::elevated_surface_background, "#2f343e"},
    {tok::editor_background, "#282c33"},
    {tok::element_background, "#2e343e"},
    {tok::element_hover, "#363c46"},
    {tok::element_active, "#454a56"},
    {tok::element_selected, "#454a56"},
    {tok::element_disabled, "#2e343e"},
    {tok::ghost_element_background, "#00000000"},
    {tok::ghost_element_hover, "#363c46"},
    {tok::ghost_element_active, "#454a56"},
    {tok::ghost_element_selected, "#454a56"},
    {tok::border, "#464b57"},
    {tok::border_variant, "#363c46"},
    {tok::border_focused, "#47679e"},
    {tok::border_selected, "#293b5b"},
    {tok::border_disabled, "#414754"},
    {tok::text, "#dce0e5"},
    {tok::text_muted, "#a9afbc"},
    {tok::text_placeholder, "#878a98"},
    {tok::text_disabled, "#878a98"},
    {tok::text_accent, "#74ade8"},
    {tok::icon, "#dce0e5"},
    {tok::icon_muted, "#a9afbc"},
    {tok::icon_disabled, "#878a98"},
    {tok::icon_accent, "#74ade8"},
    {tok::drop_target_background, "#83899480"},
    {tok::scrollbar_thumb_background, "#c8ccd44c"},
    {tok::scrollbar_thumb_border, "#363c46"},
    {tok::scrollbar_track_background, "#00000000"},
    {tok::scrollbar_track_border, "#2e333c"},
    {tok::search_match_background, "#74ade866"},
    {tok::error, "#d07277"},
    {tok::error_background, "#d072771a"},
    {tok::error_border, "#4c2b2c"},
    {tok::warning, "#dec184"},
    {tok::warning_background, "#dec1841a"},
    {tok::warning_border, "#5d4c2f"},
    {tok::modified, "#dec184"},
    {tok::conflict, "#dec184"},
    {tok::success, "#a1c181"},
    {tok::success_background, "#a1c1811a"},
    {tok::success_border, "#38482f"},
    {tok::created, "#a1c181"},
    {tok::info, "#74ade8"},
    {tok::info_background, "#74ade81a"},
    {tok::info_border, "#293b5b"},
    {tok::renamed, "#74ade8"},
    {tok::hint, "#788ca6"},
    {tok::hint_background, "#5a6f891a"},
    {tok::hint_border, "#293b5b"},
    {tok::hidden, "#878a98"},
    {tok::ignored, "#878a98"},
    {tok::unreachable, "#a9afbc"},
    {tok::predictive, "#5a6a87"},
    {tok::player0_cursor, "#74ade8"},
    {tok::player1_cursor, "#be5046"},
    {tok::player2_cursor, "#bf956a"},
    {tok::player3_cursor, "#b477cf"},
    {tok::player4_cursor, "#6eb4bf"},
    {tok::player5_cursor, "#d07277"},
    {tok::player6_cursor, "#dec184"},
    {tok::player7_cursor, "#a1c181"},
    {tok::player0_selection, "#74ade83d"},
    {tok::player7_selection, "#a1c1813d"},
    {tok::terminal_ansi_black, "#282c34"},
    {tok::terminal_ansi_red, "#e06c75"},
    {tok::terminal_ansi_green, "#98c379"},
    {tok::terminal_ansi_yellow, "#e5c07b"},
    {tok::terminal_ansi_blue, "#61afef"},
    {tok::terminal_ansi_magenta, "#c678dd"},
    {tok::terminal_ansi_cyan, "#56b6c2"},
    {tok::terminal_ansi_white, "#abb2bf"},
    {tok::syntax_keyword, "#b477cf"},
    {tok::syntax_string, "#a1c181"},
    {tok::syntax_function, "#73ade9"},
    {tok::syntax_type, "#6eb4bf"},
    {tok::syntax_comment, "#5d636f"},
    {tok::syntax_number, "#bf956a"},
    {tok::syntax_boolean, "#bf956a"},
    {tok::syntax_property, "#d07277"},
};

/// ADR-010 §5.1, One Light.
constexpr expect k_one_light[] = {
    {tok::background, "#dcdcdd"},
    {tok::surface_background, "#ebebec"},
    {tok::elevated_surface_background, "#ebebec"},
    {tok::element_background, "#ebebec"},
    {tok::element_hover, "#dfdfe0"},
    {tok::element_active, "#cacaca"},
    {tok::element_selected, "#cacaca"},
    {tok::element_disabled, "#ebebec"},
    {tok::ghost_element_background, "#00000000"},
    {tok::border, "#c9c9ca"},
    {tok::border_variant, "#dfdfe0"},
    {tok::border_focused, "#7d82e8"},
    {tok::border_selected, "#cbcdf6"},
    {tok::border_disabled, "#d3d3d4"},
    {tok::text, "#242529"},
    {tok::text_muted, "#58585a"},
    {tok::text_placeholder, "#7e8086"},
    {tok::text_disabled, "#7e8086"},
    {tok::text_accent, "#5c78e2"},
    {tok::icon, "#242529"},
    {tok::icon_accent, "#5c78e2"},
    {tok::error, "#d36151"},
    {tok::error_background, "#fbdfd9"},
    {tok::error_border, "#f6c6bd"},
    {tok::warning, "#a48819"},
    {tok::warning_background, "#faf2e6"},
    {tok::warning_border, "#f4e7d1"},
    {tok::modified, "#a48819"},
    {tok::conflict, "#a48819"},
    {tok::success, "#669f59"},
    {tok::success_background, "#dfeadb"},
    {tok::success_border, "#c8dcc1"},
    {tok::created, "#669f59"},
    {tok::info, "#5c78e2"},
    {tok::info_background, "#e2e2fa"},
    {tok::info_border, "#cbcdf6"},
    {tok::renamed, "#5c78e2"},
    {tok::hint, "#7274a7"},
    {tok::hint_background, "#e2e2fa"},
    {tok::hint_border, "#cbcdf6"},
    {tok::predictive, "#9b9ec6"},
};

void check_table(const lx::ui::theme& t, std::span<const expect> table) {
    unsigned bad = 0;
    for (const expect& e : table) {
        if (t.rgba8(e.t) != hex(e.hex)) {
            ++bad;
            std::printf("    %s: %.*s is %08x, §5.1 says %s\n", t.name(),
                        static_cast<int>(lx::ui::token_key(e.t).size()),
                        lx::ui::token_key(e.t).data(), t.rgba8(e.t), e.hex);
        }
    }
    LUMEN_CHECK(bad == 0);
}

/// Every colour the source gives (style keys, players, syntax colours) is the compiled one.
unsigned check_source_values(const lx::ui::theme& t, std::string_view src) {
    unsigned checked = 0;
    unsigned bad = 0;
    const std::size_t syntax_at = src.find("\"syntax\"");
    const std::size_t players_at = src.find("\"players\"");
    for (unsigned i = 0; i < lx::ui::k_token_count; ++i) {
        const auto token = static_cast<tok>(i);
        const std::string_view key = lx::ui::token_key(token);
        std::string_view v{};
        if (token >= tok::syntax_attribute) {
            const std::string name{key.substr(7)};
            const std::string needle = "\"" + name + "\": {";
            const std::size_t at = src.find(needle, syntax_at);
            if (at != std::string_view::npos)
                v = value_of(src, "color", at);
        } else if (token >= tok::player0_cursor) {
            const unsigned p = (i - static_cast<unsigned>(tok::player0_cursor)) / 3;
            const unsigned f = (i - static_cast<unsigned>(tok::player0_cursor)) % 3;
            std::size_t at = players_at;
            for (unsigned k = 0; k <= p && at != std::string_view::npos; ++k)
                at = src.find("\"cursor\"", at + 1);
            const char* field[3] = {"cursor", "background", "selection"};
            if (at != std::string_view::npos)
                v = value_of(src, field[f], at - 1);
        } else {
            const std::size_t at = src.find("\"" + std::string{key} + "\": \"");
            if (at != std::string_view::npos && at < players_at)
                v = value_of(src, key);
        }
        if (v.empty())
            continue;
        ++checked;
        if (t.rgba8(token) != hex(v)) {
            ++bad;
            std::printf("    %s: %.*s is %08x, the source says %.*s\n", t.name(),
                        static_cast<int>(key.size()), key.data(), t.rgba8(token),
                        static_cast<int>(v.size()), v.data());
        }
    }
    LUMEN_CHECK(bad == 0);
    return checked;
}

} // namespace

// ── The compiler and the format ──

LUMEN_TEST(compiles_both_bundled_themes) {
    for (const char* name : {"One Dark", "One Light"}) {
        lx::ui::compile_report report{};
        const std::vector<std::uint8_t> bytes = compile(name, &report);
        const lx::ui::theme t = theme_of(bytes);
        LUMEN_CHECK(std::strcmp(t.name(), name) == 0);
        LUMEN_CHECK((t.appearance_mode() == lx::ui::appearance::light) == (name[4] == 'L'));
        LUMEN_CHECK(report.tokens_from_source + report.tokens_derived + report.tokens_from_base ==
                    lx::ui::k_token_count);
        LUMEN_CHECK(report.invalid_colors == 0 && report.ignored_keys == 0);
        LUMEN_CHECK(report.themes.size() == 2);
        LUMEN_CHECK(report.fallbacks.size() == report.tokens_derived + report.tokens_from_base);
        std::printf("    %s: %zu bytes, %u tokens from the file, %u derived, %u from Zed's base\n",
                    name, bytes.size(), report.tokens_from_source, report.tokens_derived,
                    report.tokens_from_base);
        // The linear table is the sRGB table decoded and premultiplied.
        for (unsigned i = 0; i < lx::ui::k_token_count; ++i) {
            const lx::color c = t.color(static_cast<tok>(i));
            const lx::gfx::packed_rgba want = lx::gfx::premultiplied_half(
                lx::color::rgb(lx::srgb_to_linear(c.r), lx::srgb_to_linear(c.g),
                               lx::srgb_to_linear(c.b), c.a));
            const lx::gfx::packed_rgba got = t.linear(static_cast<tok>(i));
            LUMEN_CHECK(got.r == want.r && got.g == want.g && got.b == want.b && got.a == want.a);
        }
    }
}

LUMEN_TEST(section_5_1_values_and_every_source_value) {
    const std::string json = read_file(LUMEN_THEME_SOURCE_JSON);
    const lx::ui::theme dark = theme_of(compile("One Dark"));
    const lx::ui::theme light = theme_of(compile("One Light"));
    check_table(dark, k_one_dark);
    check_table(light, k_one_light);
    const unsigned nd = check_source_values(dark, section(json, "One Dark"));
    const unsigned nl = check_source_values(light, section(json, "One Light"));
    std::printf("    source values checked: One Dark %u, One Light %u\n", nd, nl);
    LUMEN_CHECK(nd > 200 && nl > 200);
    // The status and player tables name the triples.
    const lx::ui::status_colors err = dark.status(lx::ui::status_kind::error);
    LUMEN_CHECK(err.color == tok::error && err.background == tok::error_background &&
                err.border == tok::error_border);
    LUMEN_CHECK(dark.player(3).cursor == tok::player3_cursor);
    // Syntax styles: the weight of `emphasis.strong`, a colour given.
    const unsigned strong = static_cast<unsigned>(tok::syntax_emphasis_strong) -
                            static_cast<unsigned>(tok::syntax_attribute);
    LUMEN_CHECK(dark.syntax(strong).weight == 700 && dark.syntax(strong).has_color);
    // Fonts and density: Lumen's defaults (§5.3–§5.4).
    LUMEN_CHECK(std::strcmp(dark.fonts().ui_family, "IBM Plex Sans") == 0);
    LUMEN_CHECK(std::strcmp(dark.fonts().buffer_family, "Lilex") == 0);
    LUMEN_CHECK(dark.fonts().ui_size_px == 16.f && dark.fonts().buffer_size_px == 15.f);
    LUMEN_CHECK(dark.fonts().text_contrast == lx::gfx::k_text_contrast_dark);
    LUMEN_CHECK(light.fonts().text_contrast == lx::gfx::k_text_contrast_light);
    LUMEN_CHECK(dark.default_density() == lx::ui::density::standard);
}

LUMEN_TEST(bundled_files_and_builtin_match_the_compiler) {
    for (const auto& [file, name, mode] :
         {std::tuple{"one-dark", "One Dark", lx::ui::appearance::dark},
          std::tuple{"one-light", "One Light", lx::ui::appearance::light}}) {
        const std::vector<std::uint8_t> compiled = compile(name);
        const std::string path = std::string{LUMEN_TEST_THEME_DIR} + "/" + file + ".lxtheme";
        auto loaded = lx::ui::theme::load(path.c_str());
        LUMEN_CHECK(loaded.has_value());
        LUMEN_CHECK(std::ranges::equal(loaded->bytes(), compiled));
        // The fallback compiled into the binary is the same theme, byte for byte.
        const lx::ui::theme& builtin = lx::ui::theme::builtin(mode);
        LUMEN_CHECK(builtin.valid());
        if (!std::ranges::equal(builtin.bytes(), compiled))
            std::printf("    theme_builtin.cpp is stale: rerun lumen-theme --emit-cpp\n");
        LUMEN_CHECK(std::ranges::equal(builtin.bytes(), compiled));
    }
    // One read and a bounds check: what a load costs.
    const std::string path = std::string{LUMEN_TEST_THEME_DIR} + "/one-dark.lxtheme";
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        auto t = lx::ui::theme::load(path.c_str());
        LUMEN_CHECK(t.has_value());
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("    theme::load: %.1f us per load (3816 B, 200 loads)\n",
                std::chrono::duration<double, std::micro>(t1 - t0).count() / 200.0);
    // With nothing set, the active theme is the built-in One Dark.
    LUMEN_CHECK(std::strcmp(lx::ui::active_theme().name(), "One Dark") == 0);
    LUMEN_CHECK(lx::ui::ui_rem_px() == 16.f);
}

LUMEN_TEST(load_rejects_truncated_wrong_magic_and_version) {
    const std::vector<std::uint8_t> good = compile("One Dark");
    const auto code_of = [](const lx::result<lx::ui::theme>& r) {
        return r ? -1 : r.get_error().code;
    };
    const auto domain_of = [](const lx::result<lx::ui::theme>& r) {
        return r ? lx::error_domain::none : r.get_error().domain;
    };
    const int format = static_cast<int>(lx::ui_err::theme_format);
    const int version = static_cast<int>(lx::ui_err::theme_version);
    for (std::size_t cut : {std::size_t{0}, std::size_t{4}, std::size_t{127}, std::size_t{128},
                            std::size_t{1000}, good.size() - 1}) {
        std::vector<std::uint8_t> b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto r = lx::ui::theme::from_bytes(b);
        LUMEN_CHECK(domain_of(r) == lx::error_domain::ui && code_of(r) == format);
    }
    std::vector<std::uint8_t> b = good;
    b[0] = 'X';
    LUMEN_CHECK(code_of(lx::ui::theme::from_bytes(b)) == format);
    b = good;
    b[4] = 2; // version 2
    LUMEN_CHECK(code_of(lx::ui::theme::from_bytes(b)) == version);
    b = good;
    b[good.size() - 200] ^= 0x40; // payload: the checksum catches it
    LUMEN_CHECK(code_of(lx::ui::theme::from_bytes(b)) == format);
    b = good;
    const std::uint32_t far = 0x7fffff00u; // the sRGB table's offset out of bounds
    std::memcpy(b.data() + offsetof(lx::ui::theme_file_header, srgb_offset), &far, 4);
    LUMEN_CHECK(code_of(lx::ui::theme::from_bytes(b)) == format);
    b = good;
    const std::uint16_t fewer = static_cast<std::uint16_t>(lx::ui::k_token_count - 1);
    std::memcpy(b.data() + offsetof(lx::ui::theme_file_header, token_count), &fewer, 2);
    LUMEN_CHECK(code_of(lx::ui::theme::from_bytes(b)) == version);
    // From a file: missing, truncated.
    const auto missing = lx::ui::theme::load("/nonexistent/lumen/one-dark.lxtheme");
    LUMEN_CHECK(domain_of(missing) == lx::error_domain::io &&
                code_of(missing) == static_cast<int>(lx::io_err::not_found));
    char path[] = "/tmp/lumen-theme-XXXXXX";
    const int fd = ::mkstemp(path);
    LUMEN_CHECK(fd >= 0);
    LUMEN_CHECK(::write(fd, good.data(), 200) == 200);
    ::close(fd);
    LUMEN_CHECK(code_of(lx::ui::theme::load(path)) == format);
    ::unlink(path);
}

LUMEN_TEST(compiler_rejects_bad_sources) {
    const lx::ui::theme_compiler c{};
    std::vector<std::uint8_t> out;
    const auto source_error = [&](std::string_view json, const char* name = "") {
        auto r = c.compile_json(json, name, out);
        return !r && r.get_error().domain == lx::error_domain::ui &&
               r.get_error().code == static_cast<int>(lx::ui_err::theme_source);
    };
    LUMEN_CHECK(source_error(R"({"themes": [{"name": "a", "appearance": "dark", "style": {},}]})"));
    LUMEN_CHECK(c.last_error_offset() > 0);
    LUMEN_CHECK(source_error(R"({"name": "x"})"));
    LUMEN_CHECK(source_error(R"({"themes": [{"name": "a", "style": {}}]})"));
    LUMEN_CHECK(source_error(R"({"themes": [{"name": "a", "appearance": "dim", "style": {}}]})"));
    LUMEN_CHECK(source_error(R"({"themes": [{"name": "a", "appearance": "dark", "style": {}}]})", "b"));
    LUMEN_CHECK(source_error(R"({"a": 1, "a": 2})"));
    LUMEN_CHECK(source_error(R"([1, 2] x)"));
    LUMEN_CHECK(source_error("{\"s\": \"\x01\"}"));
    LUMEN_CHECK(c.compile_json(R"({"themes": [{"name": "a", "appearance": "dark", "style": {}}]})",
                               "", out).has_value());
}

// A theme that gives almost nothing: Zed's derived rules, then its base theme, and the report.
LUMEN_TEST(compiler_falls_back_by_zeds_rules) {
    const std::string_view json = R"({
      "$schema": "https://zed.dev/schema/themes/v0.2.0.json",
      "name": "T", "author": "x",
      "themes": [{
        "name": "Sparse", "appearance": "dark", "extra": 1,
        "style": {
          "error": "#ff0000",
          "created": "#00aa00",
          "border": "#112233",
          "search.match_background": "#abc",
          "scrollbar_thumb.background": "#10203040",
          "panel.background": "#202020",
          "element.hover": "#ffffff80",
          "text": "not a colour",
          "vim.normal.background": "#000000",
          "no.such.key": "#ffffff",
          "players": [{"selection": "#00ff00ff"}],
          "syntax": {"keyword": {"color": "#123456", "font_style": "italic"}, "no_such": {}}
        }
      }]
    })";
    const lx::ui::theme_compiler c{};
    std::vector<std::uint8_t> bytes;
    auto r = c.compile_json(json, "", bytes);
    LUMEN_CHECK(r.has_value());
    const lx::ui::theme t = theme_of(bytes);
    // A status background missing: its colour at 25 %.
    LUMEN_CHECK(t.rgba8(tok::error_background) == hex("#ff000040"));
    // Derived from other keys.
    LUMEN_CHECK(t.rgba8(tok::pane_group_border) == hex("#112233"));
    LUMEN_CHECK(t.rgba8(tok::search_active_match_background) == hex("#aabbcc"));
    LUMEN_CHECK(t.rgba8(tok::scrollbar_thumb_background) == hex("#10203040")); // deprecated key
    LUMEN_CHECK(t.rgba8(tok::scrollbar_thumb_active_background) == hex("#10203040"));
    LUMEN_CHECK(t.rgba8(tok::panel_overlay_background) == hex("#202020"));
    // panel + hover at alpha 0.5, opaque.
    LUMEN_CHECK(t.rgba8(tok::panel_overlay_hover) == hex("#909090"));
    // Player 0's selection is opaque: 25 % for the selection background; player 0's cursor
    // from Zed's base players.
    LUMEN_CHECK(t.rgba8(tok::element_selection_background) == hex("#00ff0040"));
    LUMEN_CHECK(t.rgba8(tok::player0_cursor) == hex("#0090ff"));
    // Version control from the status colours; with no status colour, Zed's base.
    LUMEN_CHECK(t.rgba8(tok::version_control_added) == hex("#00aa00"));
    LUMEN_CHECK(t.rgba8(tok::created_background) == hex("#00aa0040"));
    LUMEN_CHECK(t.rgba8(tok::editor_diff_hunk_added_background) == hex("#00aa001f")); // 12 %
    LUMEN_CHECK(t.rgba8(tok::version_control_deleted) == hex("#78081a"));
    // Zed's base for the rest; an invalid colour counts and falls back.
    LUMEN_CHECK(t.rgba8(tok::background) == hex("#111110"));
    LUMEN_CHECK(t.rgba8(tok::text) == hex("#eeeeecff"));
    LUMEN_CHECK(r->invalid_colors == 1);
    // Ignored: the theme's "extra", vim, an unknown key, an unknown syntax key.
    LUMEN_CHECK(r->ignored_keys == 4);
    // Syntax: the keyword's own, italic; a highlight without a colour takes the editor's.
    const lx::ui::syntax_style kw = t.syntax(static_cast<unsigned>(tok::syntax_keyword) -
                                             static_cast<unsigned>(tok::syntax_attribute));
    LUMEN_CHECK(kw.has_color && kw.style == lx::ui::font_style::italic);
    LUMEN_CHECK(t.rgba8(tok::syntax_keyword) == hex("#123456"));
    LUMEN_CHECK(t.rgba8(tok::syntax_string) == t.rgba8(tok::editor_foreground));
    bool reported = false;
    for (const std::string& f : r->fallbacks)
        reported = reported || f == "error.background: status colour at 25 % opacity";
    LUMEN_CHECK(reported);
}

// ── Style ──

LUMEN_TEST(style_packs_and_resolves_per_density) {
    using namespace lx::ui;
    std::printf("    sizeof(style) = %zu, sizeof(styled_node) = %zu\n", sizeof(style),
                sizeof(styled_node));
    const style s = style{}.flex_col().gap(sp::b08).px(sp::b12).py(lx::layout::px(3)).h(px(32))
                        .justify_between().items_center().flex_grow(2.f).bg(tok::surface_background)
                        .border_1(tok::border_variant).rounded(radius::md)
                        .shadow(elevation::elevated_surface).opacity(0.5f).text_sm();
    const float gaps[3] = {4.f, 8.f, 10.f};
    const float pads[3] = {10.f, 12.f, 14.f};
    for (unsigned d = 0; d < 3; ++d) {
        const lx::layout::flex_style f =
            resolve_layout(s, {static_cast<density>(d), 16.f});
        LUMEN_CHECK(f.row_gap == lx::layout::px(gaps[d]) && f.column_gap == lx::layout::px(gaps[d]));
        LUMEN_CHECK(f.padding.left == lx::layout::px(pads[d]) &&
                    f.padding.right == lx::layout::px(pads[d]));
        LUMEN_CHECK(f.padding.top == lx::layout::px(3.f));
        LUMEN_CHECK(f.height == lx::layout::px(32.f) && f.width.is_auto());
        LUMEN_CHECK(f.direction == lx::layout::flex_direction::column);
        LUMEN_CHECK(f.justify_content == lx::layout::content_align::space_between);
        LUMEN_CHECK(f.align_items == lx::layout::item_align::center);
        LUMEN_CHECK(f.align_content == lx::layout::content_align::stretch);
        LUMEN_CHECK(f.flex_grow == 2.f && f.flex_shrink == 1.f);
        LUMEN_CHECK(f.border.top == lx::layout::px(1.f) && f.border.left == lx::layout::px(1.f));
    }
    // The CSS initial values, untouched.
    const lx::layout::flex_style plain = resolve_layout(style{}, {});
    LUMEN_CHECK(plain == lx::layout::flex_style{});
    // §5.3: every step at every density.
    LUMEN_CHECK(spacing_px(sp::b48, density::compact) == 44.f);
    LUMEN_CHECK(spacing_px(sp::b02, density::comfortable) == 4.f);
    LUMEN_CHECK(spacing_px(sp::b16, density::standard, 20.f) == 20.f); // scales with the rem
    // §5.5 radii and §5.4 text sizes.
    LUMEN_CHECK(radius_px(radius::xs) == 2.f && radius_px(radius::sm) == 4.f &&
                radius_px(radius::md) == 6.f && radius_px(radius::lg) == 8.f &&
                radius_px(radius::xl) == 12.f && radius_px(radius::full) >= 1000.f);
    const theme& t = theme::builtin(appearance::dark);
    LUMEN_CHECK(text_size_px(text_size::normal, t, 16.f) == 14.f &&
                text_size_px(text_size::small, t, 16.f) == 12.f &&
                text_size_px(text_size::xsmall, t, 16.f) == 10.f &&
                text_size_px(text_size::large, t, 16.f) == 16.f);
    const resolved_text rt = resolve_text(s, t, 16.f);
    LUMEN_CHECK(rt.has_size && rt.size_px == 12.f && !rt.has_color && rt.line_height == k_phi);
    // Equality is bitwise; a spacing step is not the px it resolves to.
    LUMEN_CHECK(style{}.gap(sp::b08) != style{}.gap(px(8)));
    LUMEN_CHECK(style{}.gap(sp::b08) == style{}.gap(sp::b08));
}

LUMEN_TEST(elevation_resolves_to_shadows_and_a_border) {
    using namespace lx::ui;
    for (appearance mode : {appearance::dark, appearance::light}) {
        const theme& t = theme::builtin(mode);
        const resolved_paint e =
            resolve_paint(style{}.rounded(radius::md).shadow(elevation::elevated_surface), t, 16.f);
        LUMEN_CHECK(e.shadow_count == 2 && e.has_quad);
        LUMEN_CHECK(e.shadows[0].offset_y == 2.f && e.shadow(0).blur_sigma == 3.f &&
                    e.shadow(0).color.a == 0.12f);
        LUMEN_CHECK(e.shadows[1].offset_y == 1.f && e.shadow(1).blur_sigma == 0.f &&
                    e.shadow(1).color.a == (mode == appearance::dark ? 0.06f : 0.03f));
        LUMEN_CHECK(e.shadow(0).corner_radii[0] == 6.f && e.shadow(1).corner_radii[3] == 6.f);
        // The elevated surface's background and its 1 px border.variant border.
        quad_style q{};
        q.background = t.color(tok::elevated_surface_background);
        q.border_color = t.color(tok::border_variant);
        for (unsigned i = 0; i < 4; ++i) {
            q.border_widths[i] = 1.f;
            q.corner_radii[i] = 6.f;
        }
        const packed_quad_style want = pack_quad_style(q);
        LUMEN_CHECK(std::memcmp(&want, &e.quad, sizeof(want)) == 0);
        const resolved_paint m =
            resolve_paint(style{}.rounded(radius::lg).shadow(elevation::modal_surface), t, 16.f);
        LUMEN_CHECK(m.shadow_count == 4);
        const float alphas_dark[4] = {0.12f, 0.08f, 0.04f, 0.12f};
        const float alphas_light[4] = {0.06f, 0.06f, 0.04f, 0.04f};
        const int dy[4] = {2, 3, 6, 1};
        const float blur[4] = {3.f, 6.f, 12.f, 0.f};
        for (unsigned i = 0; i < 4; ++i) {
            LUMEN_CHECK(m.shadows[i].offset_y == static_cast<float>(dy[i]) &&
                        m.shadow(i).blur_sigma == blur[i]);
            LUMEN_CHECK(m.shadow(i).color.a ==
                        (mode == appearance::dark ? alphas_dark[i] : alphas_light[i]));
        }
        // Flat: no shadow, no quad.
        const resolved_paint f = resolve_paint(style{}, t, 16.f);
        LUMEN_CHECK(f.shadow_count == 0 && !f.has_quad && f.opacity == 1.f);
    }
}

namespace {

struct fixed_props {
    int w = 0;
    int h = 0;
};

class fixed_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    bool apply(const fixed_props& p) {
        const bool changed = p.w != size_.width || p.h != size_.height;
        size_ = {p.w, p.h};
        return changed;
    }
    [[nodiscard]] lx::size2i measure(lx::layout::constraints, lx::ui::size_hint) const override {
        return size_;
    }

private:
    lx::size2i size_{};
};

[[nodiscard]] lx::ui::child fixed(int w, int h) {
    return lx::ui::describe<fixed_node, fixed_props>({w, h});
}

lx::ui::style g_row_style{};

lx::ui::child describe_row(lx::ui::build_context&) {
    using namespace lx::ui;
    const child kids[3] = {fixed(20, 10), fixed(30, 10), fixed(40, 10)};
    return div(g_row_style, kids);
}

} // namespace

LUMEN_TEST(styled_box_lays_out_paints_and_follows_the_theme) {
    using namespace lx::ui;
    set_active_builtin_theme(appearance::dark);
    set_ui_density(density::standard);
    g_row_style = style{}.flex_row().gap(sp::b08).p(sp::b04).bg(tok::surface_background)
                      .border_1(tok::border).rounded(radius::sm).items_center();
    lx::runtime::memory_arena arena{64 * 1024};
    ui_root root{arena};
    root.set_describer(describe_row);
    root.set_bounds({0, 0, 400, 100});
    auto list = std::make_unique<lx::scene::draw_list>();
    LUMEN_CHECK(root.tick(*list));
    ui_node* box = root.tree();
    LUMEN_CHECK(box && box->type() == styled_node::static_type() && box->child_count() == 3);
    // 1 px border + 4 px padding, then 8 px gaps.
    LUMEN_CHECK(box->child_at(0)->bounds().x == 5 && box->child_at(1)->bounds().x == 33 &&
                box->child_at(2)->bounds().x == 71);
    // The box paints one quad, in surface.background.
    LUMEN_CHECK(list->quad_instances().size() == 1 && list->shadow_instances().empty());
    const lx::gfx::packed_rgba bg_dark =
        lx::gfx::premultiplied_half(theme::builtin(appearance::dark).color(tok::surface_background));
    LUMEN_CHECK(list->quad_instances()[0].background[0] == bg_dark.r);
    // Idle: no frame.
    LUMEN_CHECK(!root.tick(*list));
    // G5: repainting the styled tree allocates nothing.
    const std::size_t heap_before = mallinfo2().uordblks;
    for (int i = 0; i < 200; ++i) {
        ui_damage().add({0, 0, 10, 10});
        LUMEN_CHECK(root.tick(*list));
    }
    LUMEN_CHECK(mallinfo2().uordblks == heap_before);
    // A density change: the gaps follow (compact b08 = 4, b04 = 2).
    set_ui_density(density::compact);
    LUMEN_CHECK(root.tick(*list));
    LUMEN_CHECK(box->child_at(0)->bounds().x == 3 && box->child_at(1)->bounds().x == 27);
    // A theme change: repainted with One Light's colour, no describe needed from the app.
    set_active_builtin_theme(appearance::light);
    LUMEN_CHECK(root.tick(*list));
    const lx::gfx::packed_rgba bg_light =
        lx::gfx::premultiplied_half(theme::builtin(appearance::light).color(tok::surface_background));
    LUMEN_CHECK(list->quad_instances()[0].background[0] == bg_light.r);
    LUMEN_CHECK(!root.tick(*list));
    // A styled box over a flex container takes its children and key.
    {
        lx::ui::build_context ctx{arena};
        lx::ui::build_scope scope{ctx};
        const child kids[2] = {fixed(1, 1), fixed(2, 2)};
        const child f = flex(lx::layout::flex_style{}, kids) | styled(style{}.p(sp::b02));
        LUMEN_CHECK(f->type == styled_node::static_type() && f->child_count == 2);
        const child wrapped = fixed(3, 3) | styled(style{});
        LUMEN_CHECK(wrapped->type == styled_node::static_type() && wrapped->child_count == 1);
    }
    set_active_builtin_theme(appearance::dark);
    set_ui_density(density::standard);
}

// ── Hot reload ──

namespace {

/// The worker affinity: a thread draining the executor's worker strand until stopped.
class worker_thread {
public:
    worker_thread()
        : thread_{[this] {
              while (!stop_.load(std::memory_order_acquire)) {
                  lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
                  ::usleep(200);
              }
              lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
          }} {}
    ~worker_thread() {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

template<typename F>
bool pump(F done) {
    for (int i = 0; i < 20000; ++i) {
        lx::runtime::executor::global().drain(lx::runtime::affinity::ui);
        if (done())
            return true;
        ::usleep(1000);
    }
    return false;
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    LUMEN_CHECK(f != nullptr);
    LUMEN_CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
    std::fclose(f);
    LUMEN_CHECK(std::rename(tmp.c_str(), path.c_str()) == 0);
}

} // namespace

LUMEN_TEST(theme_watcher_reloads_on_the_worker) {
    using namespace lx::ui;
    char dir[] = "/tmp/lumen-theme-watch-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const std::string path = std::string{dir} + "/current.lxtheme";
    write_file(path, compile("One Dark"));
    set_active_builtin_theme(appearance::dark);
    worker_thread worker{};
    {
        theme_watcher w{};
        LUMEN_CHECK(w.watch(path.c_str()).has_value());
        LUMEN_CHECK(w.fd() >= 0);
        LUMEN_CHECK(!w.poll()); // nothing changed yet
        const std::uint64_t before = theme_generation();
        write_file(path, compile("One Light"));
        LUMEN_CHECK(pump([&] { return w.poll() || w.counters().posted > 0; }));
        LUMEN_CHECK(pump([&] { return w.counters().applied == 1; }));
        LUMEN_CHECK(std::strcmp(active_theme().name(), "One Light") == 0);
        LUMEN_CHECK(theme_generation() != before);
        // A broken file leaves the active theme as it was.
        std::vector<std::uint8_t> broken = compile("One Dark");
        broken.resize(100);
        write_file(path, broken);
        LUMEN_CHECK(pump([&] { return w.poll() || w.counters().posted > 1; }));
        LUMEN_CHECK(pump([&] { return w.counters().failed == 1; }));
        LUMEN_CHECK(std::strcmp(active_theme().name(), "One Light") == 0);
    }
    set_active_builtin_theme(appearance::dark);
    ::unlink(path.c_str());
    ::rmdir(dir);
}

// ── Icons ──

LUMEN_TEST(icon_cache_hits_misses_and_coverage) {
    using namespace lx::ui;
    if (!icons_available()) {
        std::printf("    SKIP: librsvg or the bundled icons are not available\n");
        return;
    }
    // Every bundled icon rasterises.
    for (unsigned i = 0; i < k_icon_count; ++i) {
        char path[768];
        const std::string_view n = icon_name(static_cast<icon>(i));
        std::snprintf(path, sizeof(path), "%s/%.*s.svg", icon_dir(), static_cast<int>(n.size()),
                      n.data());
        std::uint8_t cov[16 * 16];
        LUMEN_CHECK(rasterize_icon(path, 16, cov).has_value());
    }
    // "x" at 16 px: two strokes from (6, 6) to (18, 18) in 24 units, 2 wide, round caps —
    // ink from 5 to 19 units, 3.3 to 12.7 px.
    char path[768];
    std::snprintf(path, sizeof(path), "%s/x.svg", icon_dir());
    std::uint8_t cov[16 * 16]{};
    LUMEN_CHECK(rasterize_icon(path, 16, cov).has_value());
    int x0 = 16, y0 = 16, x1 = -1, y1 = -1;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            if (cov[y * 16 + x] > 0) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    std::printf("    x.svg at 16 px: coverage box (%d, %d)–(%d, %d), centre %u\n", x0, y0, x1, y1,
                static_cast<unsigned>(cov[8 * 16 + 8]));
    LUMEN_CHECK(x0 == 3 && y0 == 3 && x1 == 12 && y1 == 12);
    LUMEN_CHECK(cov[8 * 16 + 8] >= 200 && cov[0] == 0 && cov[15 * 16 + 15] == 0);
    {
        std::uint8_t c16[16 * 16];
        const auto r0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i)
            LUMEN_CHECK(rasterize_icon(path, 16, c16).has_value());
        const auto r1 = std::chrono::steady_clock::now();
        std::printf("    rasterize_icon x.svg at 16 px: %.1f us each (librsvg, 50 runs)\n",
                    std::chrono::duration<double, std::micro>(r1 - r0).count() / 50.0);
    }
    // The cache: a miss rasterises once, then hits; another size is another raster.
    auto atlas = std::make_unique<lx::gfx::atlas>();
    atlas->begin_frame(1);
    icon_cache cache{*atlas};
    lx::gfx::atlas_tile t1{}, t2{}, t3{};
    LUMEN_CHECK(cache.place(icon::x, 16, t1));
    LUMEN_CHECK(cache.place(icon::x, 16, t2));
    LUMEN_CHECK(cache.place(icon::x, 14, t3));
    LUMEN_CHECK(t1.w == 16 && t1.h == 16 && t1.x == t2.x && t1.y == t2.y && t3.w == 14);
    icon_cache_counters c = cache.counters();
    LUMEN_CHECK(c.misses == 2 && c.hits == 1 && c.rasterized == 2 && c.failures == 0);
    // The tile holds the coverage.
    const lx::gfx::atlas_page_view page = atlas->page(t1.page);
    unsigned same = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            same += page.pixels[(t1.y + y) * page.stride + t1.x + x] == cov[y * 16 + x];
    LUMEN_CHECK(same == 256);
    // A steady frame: hits only.
    atlas->begin_frame(2);
    for (int i = 0; i < 10; ++i)
        LUMEN_CHECK(cache.place(icon::x, 16, t1));
    LUMEN_CHECK(cache.counters().rasterized == 2 && cache.counters().hits == 11);
}

LUMEN_TEST(icon_rasters_on_the_worker_when_async) {
    using namespace lx::ui;
    if (!icons_available()) {
        std::printf("    SKIP: librsvg or the bundled icons are not available\n");
        return;
    }
    auto atlas = std::make_unique<lx::gfx::atlas>();
    atlas->begin_frame(1);
    worker_thread worker{};
    {
        icon_cache cache{*atlas};
        cache.set_async(&lx::runtime::executor::global());
        ui_damage().clear();
        lx::gfx::atlas_tile t{};
        LUMEN_CHECK(!cache.place(icon::bell, 16, t, {10, 20, 16, 16}));
        LUMEN_CHECK(!cache.place(icon::bell, 16, t, {40, 20, 16, 16})); // still pending
        LUMEN_CHECK(cache.counters().async_posted == 1);
        LUMEN_CHECK(pump([&] { return cache.counters().async_landed == 1; }));
        LUMEN_CHECK(cache.place(icon::bell, 16, t) && t.w == 16);
        // The landing damaged both places that asked.
        LUMEN_CHECK(ui_damage().needs_paint() && ui_damage().count() >= 1);
        const lx::rect2i u = ui_damage().union_bounds();
        LUMEN_CHECK(u.x == 10 && u.width == 46);
        ui_damage().clear();
        // Destroyed with a job in flight: the landing frees it.
        LUMEN_CHECK(!cache.place(icon::moon, 16, t));
    }
    LUMEN_CHECK(pump([] { return true; }));
    ::usleep(20000);
    lx::runtime::executor::global().drain(lx::runtime::affinity::ui);
    ui_damage().clear();
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
