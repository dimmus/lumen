module;

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

export module lx.ui:theme;

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.trace;

// ── Themes (ADR-010 §4.8, §5) ──────────────────────────────────────────────────────
//
// A theme is a compiled `.lxtheme` file: `tools/lumen-theme` turns a theme JSON in Zed's
// schema into it at build time (`theme_compiler`), and `theme::load` reads it back with one
// read and a bounds check — nothing is parsed at run time. Colours are tokens (`tok`), u16
// indices into the file's colour table, which holds every colour twice (§4.5): straight
// sRGB-encoded bytes, what the UI blends in, and premultiplied linear-light halves, what the
// compositor's linear pass takes. A token's colour is one array index.
//
// The tokens are every colour key of Zed's theme schema (crates/settings_content/src/theme.rs)
// except the vim mode indicators — window chrome, elements, text, icons, panels, scrollbars,
// the editor, the terminal's ANSI palette, version control — then the fourteen status
// triples (colour, background, border), eight players (cursor, background, selection) and the
// syntax highlight colours of Zed's One themes. `k_token_keys` names each by its Zed key.
//
// A theme is immutable once loaded. The UI thread has one active theme (`active_theme`):
// the built-in One Dark until something else is set. Setting one bumps `theme_generation`,
// which makes the next `ui_root::tick` describe again and lay out and repaint everything.
// `theme_watcher` reloads a theme file when it changes on disk (ADR §4.8's debug hot
// reload): the file is read and checked on the worker affinity, the result swapped in on UI.

export namespace lx::ui {

// ── Colour tokens ──

enum class tok : std::uint16_t {
    border,
    border_variant,
    border_focused,
    border_selected,
    border_transparent,
    border_disabled,
    elevated_surface_background,
    surface_background,
    background,
    element_background,
    element_hover,
    element_active,
    element_selected,
    element_disabled,
    element_selection_background,
    drop_target_background,
    drop_target_border,
    ghost_element_background,
    ghost_element_hover,
    ghost_element_active,
    ghost_element_selected,
    ghost_element_disabled,
    text,
    text_muted,
    text_placeholder,
    text_disabled,
    text_accent,
    icon,
    icon_muted,
    icon_disabled,
    icon_placeholder,
    icon_accent,
    debugger_accent,
    status_bar_background,
    title_bar_background,
    title_bar_inactive_background,
    toolbar_background,
    tab_bar_background,
    tab_inactive_background,
    tab_active_background,
    search_match_background,
    search_active_match_background,
    panel_background,
    panel_focused_border,
    panel_indent_guide,
    panel_indent_guide_hover,
    panel_indent_guide_active,
    panel_overlay_background,
    panel_overlay_hover,
    pane_focused_border,
    pane_group_border,
    scrollbar_thumb_background,
    scrollbar_thumb_hover_background,
    scrollbar_thumb_active_background,
    scrollbar_thumb_border,
    scrollbar_track_background,
    scrollbar_track_border,
    minimap_thumb_background,
    minimap_thumb_hover_background,
    minimap_thumb_active_background,
    minimap_thumb_border,
    editor_foreground,
    editor_background,
    editor_gutter_background,
    editor_subheader_background,
    editor_active_line_background,
    editor_highlighted_line_background,
    editor_debugger_active_line_background,
    editor_line_number,
    editor_active_line_number,
    editor_hover_line_number,
    editor_invisible,
    editor_wrap_guide,
    editor_active_wrap_guide,
    editor_indent_guide,
    editor_indent_guide_active,
    editor_document_highlight_read_background,
    editor_document_highlight_write_background,
    editor_document_highlight_bracket_background,
    editor_diff_hunk_added_background,
    editor_diff_hunk_added_hollow_background,
    editor_diff_hunk_added_hollow_border,
    editor_diff_hunk_deleted_background,
    editor_diff_hunk_deleted_hollow_background,
    editor_diff_hunk_deleted_hollow_border,
    terminal_background,
    terminal_foreground,
    terminal_ansi_background,
    terminal_bright_foreground,
    terminal_dim_foreground,
    terminal_ansi_black,
    terminal_ansi_bright_black,
    terminal_ansi_dim_black,
    terminal_ansi_red,
    terminal_ansi_bright_red,
    terminal_ansi_dim_red,
    terminal_ansi_green,
    terminal_ansi_bright_green,
    terminal_ansi_dim_green,
    terminal_ansi_yellow,
    terminal_ansi_bright_yellow,
    terminal_ansi_dim_yellow,
    terminal_ansi_blue,
    terminal_ansi_bright_blue,
    terminal_ansi_dim_blue,
    terminal_ansi_magenta,
    terminal_ansi_bright_magenta,
    terminal_ansi_dim_magenta,
    terminal_ansi_cyan,
    terminal_ansi_bright_cyan,
    terminal_ansi_dim_cyan,
    terminal_ansi_white,
    terminal_ansi_bright_white,
    terminal_ansi_dim_white,
    link_text_hover,
    version_control_added,
    version_control_deleted,
    version_control_modified,
    version_control_renamed,
    version_control_conflict,
    version_control_ignored,
    version_control_word_added,
    version_control_word_deleted,
    version_control_conflict_marker_ours,
    version_control_conflict_marker_theirs,
    conflict,
    conflict_background,
    conflict_border,
    created,
    created_background,
    created_border,
    deleted,
    deleted_background,
    deleted_border,
    error,
    error_background,
    error_border,
    hidden,
    hidden_background,
    hidden_border,
    hint,
    hint_background,
    hint_border,
    ignored,
    ignored_background,
    ignored_border,
    info,
    info_background,
    info_border,
    modified,
    modified_background,
    modified_border,
    predictive,
    predictive_background,
    predictive_border,
    renamed,
    renamed_background,
    renamed_border,
    success,
    success_background,
    success_border,
    unreachable,
    unreachable_background,
    unreachable_border,
    warning,
    warning_background,
    warning_border,
    player0_cursor,
    player0_background,
    player0_selection,
    player1_cursor,
    player1_background,
    player1_selection,
    player2_cursor,
    player2_background,
    player2_selection,
    player3_cursor,
    player3_background,
    player3_selection,
    player4_cursor,
    player4_background,
    player4_selection,
    player5_cursor,
    player5_background,
    player5_selection,
    player6_cursor,
    player6_background,
    player6_selection,
    player7_cursor,
    player7_background,
    player7_selection,
    syntax_attribute,
    syntax_boolean,
    syntax_comment,
    syntax_comment_doc,
    syntax_constant,
    syntax_constructor,
    syntax_embedded,
    syntax_emphasis,
    syntax_emphasis_strong,
    syntax_enum,
    syntax_function,
    syntax_hint,
    syntax_keyword,
    syntax_label,
    syntax_link_text,
    syntax_link_uri,
    syntax_namespace,
    syntax_number,
    syntax_operator,
    syntax_predictive,
    syntax_preproc,
    syntax_primary,
    syntax_property,
    syntax_punctuation,
    syntax_punctuation_bracket,
    syntax_punctuation_delimiter,
    syntax_punctuation_list_marker,
    syntax_punctuation_markup,
    syntax_punctuation_special,
    syntax_selector,
    syntax_selector_pseudo,
    syntax_string,
    syntax_string_escape,
    syntax_string_regex,
    syntax_string_special,
    syntax_string_special_symbol,
    syntax_tag,
    syntax_text_literal,
    syntax_title,
    syntax_type,
    syntax_variable,
    syntax_variable_parameter,
    syntax_variable_special,
    syntax_variant,
    syntax_diff_plus,
    syntax_diff_minus,
    count,
    /// No colour: nothing is painted.
    none = 0xFFFF,
};

inline constexpr std::string_view k_token_keys[] = {
    "border", "border.variant", "border.focused", "border.selected", "border.transparent",
    "border.disabled", "elevated_surface.background", "surface.background", "background",
    "element.background", "element.hover", "element.active", "element.selected",
    "element.disabled", "element.selection_background", "drop_target.background",
    "drop_target.border", "ghost_element.background", "ghost_element.hover",
    "ghost_element.active", "ghost_element.selected", "ghost_element.disabled", "text",
    "text.muted", "text.placeholder", "text.disabled", "text.accent", "icon", "icon.muted",
    "icon.disabled", "icon.placeholder", "icon.accent", "debugger.accent",
    "status_bar.background", "title_bar.background", "title_bar.inactive_background",
    "toolbar.background", "tab_bar.background", "tab.inactive_background",
    "tab.active_background", "search.match_background", "search.active_match_background",
    "panel.background", "panel.focused_border", "panel.indent_guide",
    "panel.indent_guide_hover", "panel.indent_guide_active", "panel.overlay_background",
    "panel.overlay_hover", "pane.focused_border", "pane_group.border",
    "scrollbar.thumb.background", "scrollbar.thumb.hover_background",
    "scrollbar.thumb.active_background", "scrollbar.thumb.border", "scrollbar.track.background",
    "scrollbar.track.border", "minimap.thumb.background", "minimap.thumb.hover_background",
    "minimap.thumb.active_background", "minimap.thumb.border", "editor.foreground",
    "editor.background", "editor.gutter.background", "editor.subheader.background",
    "editor.active_line.background", "editor.highlighted_line.background",
    "editor.debugger_active_line.background", "editor.line_number", "editor.active_line_number",
    "editor.hover_line_number", "editor.invisible", "editor.wrap_guide",
    "editor.active_wrap_guide", "editor.indent_guide", "editor.indent_guide_active",
    "editor.document_highlight.read_background", "editor.document_highlight.write_background",
    "editor.document_highlight.bracket_background", "editor.diff_hunk.added.background",
    "editor.diff_hunk.added.hollow_background", "editor.diff_hunk.added.hollow_border",
    "editor.diff_hunk.deleted.background", "editor.diff_hunk.deleted.hollow_background",
    "editor.diff_hunk.deleted.hollow_border", "terminal.background", "terminal.foreground",
    "terminal.ansi.background", "terminal.bright_foreground", "terminal.dim_foreground",
    "terminal.ansi.black", "terminal.ansi.bright_black", "terminal.ansi.dim_black",
    "terminal.ansi.red", "terminal.ansi.bright_red", "terminal.ansi.dim_red",
    "terminal.ansi.green", "terminal.ansi.bright_green", "terminal.ansi.dim_green",
    "terminal.ansi.yellow", "terminal.ansi.bright_yellow", "terminal.ansi.dim_yellow",
    "terminal.ansi.blue", "terminal.ansi.bright_blue", "terminal.ansi.dim_blue",
    "terminal.ansi.magenta", "terminal.ansi.bright_magenta", "terminal.ansi.dim_magenta",
    "terminal.ansi.cyan", "terminal.ansi.bright_cyan", "terminal.ansi.dim_cyan",
    "terminal.ansi.white", "terminal.ansi.bright_white", "terminal.ansi.dim_white",
    "link_text.hover", "version_control.added", "version_control.deleted",
    "version_control.modified", "version_control.renamed", "version_control.conflict",
    "version_control.ignored", "version_control.word_added", "version_control.word_deleted",
    "version_control.conflict_marker.ours", "version_control.conflict_marker.theirs",
    "conflict", "conflict.background", "conflict.border", "created", "created.background",
    "created.border", "deleted", "deleted.background", "deleted.border", "error",
    "error.background", "error.border", "hidden", "hidden.background", "hidden.border", "hint",
    "hint.background", "hint.border", "ignored", "ignored.background", "ignored.border", "info",
    "info.background", "info.border", "modified", "modified.background", "modified.border",
    "predictive", "predictive.background", "predictive.border", "renamed", "renamed.background",
    "renamed.border", "success", "success.background", "success.border", "unreachable",
    "unreachable.background", "unreachable.border", "warning", "warning.background",
    "warning.border", "players[0].cursor", "players[0].background", "players[0].selection",
    "players[1].cursor", "players[1].background", "players[1].selection", "players[2].cursor",
    "players[2].background", "players[2].selection", "players[3].cursor",
    "players[3].background", "players[3].selection", "players[4].cursor",
    "players[4].background", "players[4].selection", "players[5].cursor",
    "players[5].background", "players[5].selection", "players[6].cursor",
    "players[6].background", "players[6].selection", "players[7].cursor",
    "players[7].background", "players[7].selection", "syntax.attribute", "syntax.boolean",
    "syntax.comment", "syntax.comment.doc", "syntax.constant", "syntax.constructor",
    "syntax.embedded", "syntax.emphasis", "syntax.emphasis.strong", "syntax.enum",
    "syntax.function", "syntax.hint", "syntax.keyword", "syntax.label", "syntax.link_text",
    "syntax.link_uri", "syntax.namespace", "syntax.number", "syntax.operator",
    "syntax.predictive", "syntax.preproc", "syntax.primary", "syntax.property",
    "syntax.punctuation", "syntax.punctuation.bracket", "syntax.punctuation.delimiter",
    "syntax.punctuation.list_marker", "syntax.punctuation.markup", "syntax.punctuation.special",
    "syntax.selector", "syntax.selector.pseudo", "syntax.string", "syntax.string.escape",
    "syntax.string.regex", "syntax.string.special", "syntax.string.special.symbol",
    "syntax.tag", "syntax.text.literal", "syntax.title", "syntax.type", "syntax.variable",
    "syntax.variable.parameter", "syntax.variable.special", "syntax.variant",
    "syntax.diff.plus", "syntax.diff.minus",
};

inline constexpr unsigned k_token_count = static_cast<unsigned>(tok::count);
static_assert(std::size(k_token_keys) == k_token_count);

/// The Zed key of `t` ("surface.background", "players[0].cursor", "syntax.keyword").
[[nodiscard]] constexpr std::string_view token_key(tok t) {
    const auto i = static_cast<unsigned>(t);
    return i < k_token_count ? k_token_keys[i] : std::string_view{};
}

/// The token named by a Zed key, or `tok::none`.
[[nodiscard]] constexpr tok token_from_key(std::string_view key) {
    for (unsigned i = 0; i < k_token_count; ++i)
        if (k_token_keys[i] == key)
            return static_cast<tok>(i);
    return tok::none;
}

/// Zed's status colours, in its schema's order. Each is a triple of tokens.
enum class status_kind : std::uint8_t {
    conflict,
    created,
    deleted,
    error,
    hidden,
    hint,
    ignored,
    info,
    modified,
    predictive,
    renamed,
    success,
    unreachable,
    warning,
    count,
};
inline constexpr unsigned k_status_count = static_cast<unsigned>(status_kind::count);

struct status_colors {
    tok color = tok::none;
    tok background = tok::none;
    tok border = tok::none;
};

/// The status triple's tokens (the theme's status table stores the same).
[[nodiscard]] constexpr status_colors status_tokens(status_kind k) {
    const auto first = static_cast<std::uint16_t>(tok::conflict) +
                       static_cast<std::uint16_t>(3u * static_cast<unsigned>(k));
    return {static_cast<tok>(first), static_cast<tok>(first + 1), static_cast<tok>(first + 2)};
}

/// Collaborators' colours: Zed's `players`. Player 0 is the local user.
inline constexpr unsigned k_player_count = 8;

struct player_colors {
    tok cursor = tok::none;
    tok background = tok::none;
    tok selection = tok::none;
};

[[nodiscard]] constexpr player_colors player_tokens(unsigned player) {
    const auto first = static_cast<std::uint16_t>(tok::player0_cursor) +
                       static_cast<std::uint16_t>(3u * (player % k_player_count));
    return {static_cast<tok>(first), static_cast<tok>(first + 1), static_cast<tok>(first + 2)};
}

/// Syntax highlight styles: one per `tok::syntax_*` token, in token order.
inline constexpr unsigned k_syntax_count =
    static_cast<unsigned>(tok::count) - static_cast<unsigned>(tok::syntax_attribute);

[[nodiscard]] constexpr tok syntax_token(unsigned index) {
    return static_cast<tok>(static_cast<unsigned>(tok::syntax_attribute) + index);
}

enum class font_style : std::uint8_t { normal, italic, oblique };

struct syntax_style {
    /// The highlight's colour token (the theme's own value; `has_color` false when the file
    /// gave none and it fell back to `editor.foreground`).
    tok color = tok::none;
    /// CSS weight (100–900); 0 when the file gave none (the text's own).
    std::uint16_t weight = 0;
    font_style style = font_style::normal;
    bool has_color = false;
    bool has_style = false;
    bool has_background = false;
    /// Straight sRGB R, G, B, A bytes (R in the low byte), when `has_background`.
    std::uint32_t background = 0;
};

// ── Appearance, density, spacing, radii, elevation, text sizes (§5.2–§5.5) ──

enum class appearance : std::uint8_t { dark, light };

/// Zed's `ui_density`: how tight the spacing scale is.
enum class density : std::uint8_t { compact, standard, comfortable };

/// Zed's `DynamicSpacing` steps; their px depend on the density (§5.3).
enum class sp : std::uint8_t { b00, b01, b02, b03, b04, b06, b08, b12, b16, b20, b24, b32, b40, b48 };
inline constexpr unsigned k_spacing_steps = 14;

/// §5.3, px at a 16 px rem: [density][step].
inline constexpr std::uint8_t k_spacing_px[3][k_spacing_steps] = {
    {0, 1, 1, 2, 2, 3, 4, 10, 14, 18, 20, 28, 36, 44},
    {0, 1, 2, 3, 4, 6, 8, 12, 16, 20, 24, 32, 40, 48},
    {0, 2, 4, 4, 6, 8, 10, 14, 18, 22, 28, 36, 44, 52},
};

/// `step` in px at `d`, scaled with the rem as Zed's rem-based spacing is.
[[nodiscard]] constexpr float spacing_px(sp step, density d, float rem_px = 16.f) {
    return static_cast<float>(
               k_spacing_px[static_cast<unsigned>(d)][static_cast<unsigned>(step)]) *
           rem_px / 16.f;
}

/// GPUI's corner radii (§5.5): xs 2, sm 4, md 6, lg 8, xl 12 px at a 16 px rem; `full` is
/// a pill (half the shorter side, which the quad's SDF clamps to).
enum class radius : std::uint8_t { none, xs, sm, md, lg, xl, full };

inline constexpr float k_radius_full_px = 4096.f;

[[nodiscard]] constexpr float radius_px(radius r, float rem_px = 16.f) {
    constexpr float k_px[] = {0.f, 2.f, 4.f, 6.f, 8.f, 12.f};
    const auto i = static_cast<unsigned>(r);
    return i < 6 ? k_px[i] * rem_px / 16.f : k_radius_full_px;
}

/// Zed's `ElevationIndex` (§5.2). `flat` draws no shadow and sets no background.
enum class elevation : std::uint8_t {
    flat,
    background,
    surface,
    editor_surface,
    elevated_surface,
    modal_surface,
};

/// One shadow of an elevation: offset, blur (the Gaussian σ GPUI's `blur_radius` is), black
/// at `alpha`.
struct elevation_shadow {
    float offset_x = 0.f;
    float offset_y = 0.f;
    float blur = 0.f;
    float alpha = 0.f;
};

/// The shadows of `e` in paint order (§5.2), for a theme of appearance `a`. Elevated and
/// modal surfaces also carry a 1 px `border.variant` border.
[[nodiscard]] std::span<const elevation_shadow> elevation_shadows(elevation e, appearance a);
/// The background token of an elevation (`tok::none` for `flat`).
[[nodiscard]] tok elevation_background(elevation e);
/// Whether an elevation carries the 1 px `border.variant` border.
[[nodiscard]] constexpr bool elevation_bordered(elevation e) {
    return e == elevation::elevated_surface || e == elevation::modal_surface;
}

/// Zed's `TextSize` (§5.4) in rem: `normal` 0.875 (14 px), `small` 0.75, `xsmall` 0.625,
/// `large` 1; `ui` is the theme's UI font size, `editor` its buffer font size. `inherit`:
/// the size of the text's surroundings.
enum class text_size : std::uint8_t { inherit, xsmall, small, normal, large, ui, editor };

/// GPUI's default line height, relative to the font size: φ.
inline constexpr float k_phi = 1.6180339887f;

/// Fonts (§5.4): the compiler fills them from Lumen's defaults (Zed's theme files carry none).
struct font_settings {
    char ui_family[64]{};
    char buffer_family[64]{};
    /// The UI font size in px: the rem everything scales with (16).
    float ui_size_px = 16.f;
    /// Relative to the size: φ.
    float ui_line_height = k_phi;
    float buffer_size_px = 15.f;
    /// `comfortable` (φ); `standard` is `buffer_line_height_standard` (1.3).
    float buffer_line_height = k_phi;
    float buffer_line_height_standard = 1.3f;
    /// The coverage exponent for text in a linear-light pass (ADR §4.5).
    float text_contrast = 1.f;
};

// ── The compiled format (.lxtheme, little-endian) ──
//
//   header (128 B) │ sRGB table: u32 × tokens │ linear table: f16 × 4 × tokens │
//   status table: u16 × 3 × 14 │ player table: u16 × 3 × 8 │ syntax table: 12 B × styles │
//   fonts (160 B)
//
// Every table starts 4-byte aligned at the offset the header gives. `checksum` is FNV-1a
// over everything after the header. A reader accepts exactly `k_theme_version` and the
// counts this build has.

inline constexpr std::uint32_t k_theme_magic = 0x4854584Cu; // "LXTH"
inline constexpr std::uint16_t k_theme_version = 1;

struct theme_file_header {
    std::uint32_t magic = k_theme_magic;
    std::uint16_t version = k_theme_version;
    std::uint16_t header_bytes = 128;
    std::uint32_t file_bytes = 0;
    std::uint32_t checksum = 0;
    std::uint8_t appearance = 0;
    std::uint8_t density = 1;
    std::uint16_t token_count = 0;
    std::uint16_t status_count = 0;
    std::uint16_t player_count = 0;
    std::uint16_t syntax_count = 0;
    std::uint16_t reserved0 = 0;
    std::uint32_t srgb_offset = 0;
    std::uint32_t linear_offset = 0;
    std::uint32_t status_offset = 0;
    std::uint32_t player_offset = 0;
    std::uint32_t syntax_offset = 0;
    std::uint32_t fonts_offset = 0;
    char name[64]{};
    std::uint32_t reserved1[3]{};
};
static_assert(sizeof(theme_file_header) == 128);

struct theme_file_syntax {
    std::uint16_t color = 0;
    std::uint16_t weight = 0;
    std::uint8_t style = 0;
    /// bit 0 colour given, bit 1 style given, bit 2 background given.
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t background = 0;
};
static_assert(sizeof(theme_file_syntax) == 12);

struct theme_file_fonts {
    char ui_family[64]{};
    char buffer_family[64]{};
    float ui_size_px = 16.f;
    float ui_line_height = k_phi;
    float buffer_size_px = 15.f;
    float buffer_line_height = k_phi;
    float buffer_line_height_standard = 1.3f;
    float text_contrast = 1.f;
    std::uint32_t reserved[2]{};
};
static_assert(sizeof(theme_file_fonts) == 160);

/// FNV-1a, 32 bits: the format's checksum.
[[nodiscard]] constexpr std::uint32_t theme_checksum(std::span<const std::uint8_t> bytes) {
    std::uint32_t h = 2166136261u;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= 16777619u;
    }
    return h;
}

/// A compiled theme, immutable once loaded. Move-only: it owns its bytes, or views bytes
/// that outlive it (`builtin`).
class theme {
public:
    theme() = default;
    theme(theme&&) noexcept = default;
    theme& operator=(theme&&) noexcept = default;
    theme(const theme&) = delete;
    theme& operator=(const theme&) = delete;

    /// Reads `path` (one read) and checks it. Errors: `io_err::not_found` /
    /// `permission_denied` / `broken_pipe` (a read failed), `ui_err::theme_format` (not a
    /// compiled theme: truncated, wrong magic, a table out of bounds, a bad checksum),
    /// `ui_err::theme_version` (another format version or token set).
    [[nodiscard]] static lx::result<theme> load(const char* path);
    /// The same checks over bytes in memory. With `copy` false the bytes must outlive the
    /// theme (and be 4-byte aligned).
    [[nodiscard]] static lx::result<theme> from_bytes(std::span<const std::uint8_t> bytes,
                                                      bool copy = true);
    /// One Dark or One Light, compiled into the binary from the same source as the bundled
    /// files (theme_builtin.cpp): what renders when no theme file is found.
    [[nodiscard]] static const theme& builtin(appearance a = appearance::dark);

    [[nodiscard]] bool valid() const { return header_ != nullptr; }
    [[nodiscard]] const char* name() const { return header_ ? header_->name : ""; }
    [[nodiscard]] lx::ui::appearance appearance_mode() const;
    /// The density the theme was compiled with (`ui_density` starts from it).
    [[nodiscard]] density default_density() const;
    [[nodiscard]] const font_settings& fonts() const { return fonts_; }

    /// Straight sRGB-encoded colour (what a UI surface blends in). Transparent black for
    /// `tok::none` or an invalid theme.
    [[nodiscard]] lx::color color(tok t) const;
    /// R, G, B, A bytes, R in the low byte.
    [[nodiscard]] std::uint32_t rgba8(tok t) const;
    /// Premultiplied linear-light halves (the compositor's linear pass).
    [[nodiscard]] lx::gfx::packed_rgba linear(tok t) const;

    [[nodiscard]] status_colors status(status_kind k) const;
    [[nodiscard]] player_colors player(unsigned index) const;
    [[nodiscard]] syntax_style syntax(unsigned index) const;

    /// The file's bytes (for tests and tools).
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return {data_, size_}; }

private:
    [[nodiscard]] static lx::result<theme> check(const std::uint8_t* data, std::size_t size);

    std::unique_ptr<std::uint64_t[]> owned_{};
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    const theme_file_header* header_ = nullptr;
    const std::uint32_t* srgb_ = nullptr;
    const std::uint16_t* linear_ = nullptr;
    const std::uint16_t* status_ = nullptr;
    const std::uint16_t* players_ = nullptr;
    const theme_file_syntax* syntax_ = nullptr;
    font_settings fonts_{};
};

/// The compiled bytes of the built-in theme of `a` (theme_builtin.cpp, generated by
/// `lumen-theme --emit-cpp` from the bundled files).
[[nodiscard]] std::span<const std::uint8_t> builtin_theme_bytes(appearance a);

// ── The active theme (UI affinity) ──

/// The theme UI nodes resolve tokens against: the built-in One Dark until one is set.
[[nodiscard]] const theme& active_theme();
/// Makes `t` the active theme (an invalid one restores the built-in One Dark); the density
/// follows the theme's. Bumps `theme_generation`.
void set_active_theme(theme t);
/// Uses the built-in theme of `a` (no copy).
void set_active_builtin_theme(appearance a);
/// Changes with every `set_active_theme`, `set_active_builtin_theme` and `set_ui_density`:
/// `ui_root` lays out and repaints everything when it moves.
[[nodiscard]] std::uint64_t theme_generation();
[[nodiscard]] density ui_density();
void set_ui_density(density d);
/// The rem, in px: the active theme's UI font size.
[[nodiscard]] float ui_rem_px();

// ── Theme files ──

/// The directory compiled themes are found in, from the first of: `LUMEN_THEME_DIR`, the
/// build tree's themes, `<exe>/../share/lumen/themes` (a relocated install), the configured
/// install. "" when none exists.
[[nodiscard]] const char* theme_dir();
/// `<theme_dir>/<file_name>.lxtheme` ("one-dark", "one-light").
[[nodiscard]] lx::result<theme> load_theme_file(const char* file_name);

/// Reloads a theme file when it changes on disk and makes it the active theme. The file is
/// read and checked on the worker affinity (`executor`'s worker strand, null: the global
/// executor's); the result is applied on the UI strand. A file that fails to load leaves
/// the active theme as it was (logged once per change). UI affinity.
class theme_watcher {
public:
    theme_watcher() = default;
    ~theme_watcher();
    theme_watcher(const theme_watcher&) = delete;
    theme_watcher& operator=(const theme_watcher&) = delete;

    /// Watches `path` through its directory (an editor that writes a new file and renames
    /// it over the old one is seen too). Errors: io (inotify), invalid_argument (path).
    [[nodiscard]] lx::result<void> watch(const char* path,
                                         lx::runtime::executor* executor = nullptr);
    /// The inotify descriptor, for the owner's event loop (-1 before `watch`).
    [[nodiscard]] int fd() const { return fd_; }
    /// Drains the descriptor (non-blocking); when the file changed, posts a reload to the
    /// worker. True when one was posted. Call when `fd` is readable, or from a timer.
    bool poll();

    struct stats {
        unsigned posted = 0;  ///< reloads sent to the worker
        unsigned applied = 0; ///< themes made active
        unsigned failed = 0;  ///< reloads whose file did not load
    };
    [[nodiscard]] stats counters() const { return stats_; }

private:
    struct job;
    static void run(void* job);
    static void land(void* job);

    int fd_ = -1;
    int wd_ = -1;
    char dir_[512]{};
    char file_[256]{};
    char path_[768]{};
    lx::runtime::executor* executor_ = nullptr;
    job* job_ = nullptr;
    bool again_ = false;
    stats stats_{};
};

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

static_assert(std::endian::native == std::endian::little,
              "the .lxtheme format is little-endian and read in place");

constexpr lx::ui::elevation_shadow k_elevated_dark[] = {{0.f, 2.f, 3.f, 0.12f}, {0.f, 1.f, 0.f, 0.06f}};
constexpr lx::ui::elevation_shadow k_elevated_light[] = {{0.f, 2.f, 3.f, 0.12f}, {0.f, 1.f, 0.f, 0.03f}};
constexpr lx::ui::elevation_shadow k_modal_dark[] = {
    {0.f, 2.f, 3.f, 0.12f}, {0.f, 3.f, 6.f, 0.08f}, {0.f, 6.f, 12.f, 0.04f}, {0.f, 1.f, 0.f, 0.12f}};
constexpr lx::ui::elevation_shadow k_modal_light[] = {
    {0.f, 2.f, 3.f, 0.06f}, {0.f, 3.f, 6.f, 0.06f}, {0.f, 6.f, 12.f, 0.04f}, {0.f, 1.f, 0.f, 0.04f}};

[[nodiscard]] bool lx_ui_is_dir(const char* path) {
    struct stat st{};
    return path && path[0] && ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void lx_ui_copy_cut(char* out, std::size_t capacity, const char* in) {
    std::size_t n = in ? std::strlen(in) : 0;
    if (n >= capacity)
        n = capacity - 1;
    if (n)
        std::memcpy(out, in, n);
    out[n] = '\0';
}

/// The active theme's state. UI affinity: one thread reads and writes it.
struct lx_ui_theme_state {
    lx::ui::theme owned{};
    const lx::ui::theme* active = nullptr;
    lx::ui::density density = lx::ui::density::standard;
    std::uint64_t generation = 1;
    bool density_set = false;
};

[[nodiscard]] lx_ui_theme_state& lx_ui_themes() {
    // Never destroyed: nodes may resolve tokens during static destruction.
    static lx_ui_theme_state* const state = new lx_ui_theme_state{};
    return *state;
}

} // namespace

std::span<const lx::ui::elevation_shadow> lx::ui::elevation_shadows(elevation e, appearance a) {
    const bool dark = a == appearance::dark;
    switch (e) {
    case elevation::elevated_surface:
        return dark ? std::span<const elevation_shadow>{k_elevated_dark}
                    : std::span<const elevation_shadow>{k_elevated_light};
    case elevation::modal_surface:
        return dark ? std::span<const elevation_shadow>{k_modal_dark}
                    : std::span<const elevation_shadow>{k_modal_light};
    default:
        return {};
    }
}

lx::ui::tok lx::ui::elevation_background(elevation e) {
    switch (e) {
    case elevation::background: return tok::background;
    case elevation::surface: return tok::surface_background;
    case elevation::editor_surface: return tok::editor_background;
    case elevation::elevated_surface:
    case elevation::modal_surface: return tok::elevated_surface_background;
    default: return tok::none;
    }
}

// ── theme ──

lx::result<lx::ui::theme> lx::ui::theme::check(const std::uint8_t* data, std::size_t size) {
    if (!data || size < sizeof(theme_file_header))
        return lx::make_error(lx::ui_err::theme_format, "theme: shorter than its header");
    if ((reinterpret_cast<std::uintptr_t>(data) & 3u) != 0)
        return lx::invalid_argument("theme: bytes not 4-byte aligned");
    const auto* h = reinterpret_cast<const theme_file_header*>(data);
    if (h->magic != k_theme_magic)
        return lx::make_error(lx::ui_err::theme_format, "theme: not a compiled theme (magic)");
    if (h->version != k_theme_version)
        return lx::make_error(lx::ui_err::theme_version, "theme: unsupported format version");
    if (h->header_bytes != sizeof(theme_file_header) || h->file_bytes != size)
        return lx::make_error(lx::ui_err::theme_format, "theme: size does not match its header");
    if (h->token_count != k_token_count || h->status_count != k_status_count ||
        h->player_count != k_player_count || h->syntax_count != k_syntax_count)
        return lx::make_error(lx::ui_err::theme_version,
                              "theme: compiled for another token set (recompile it)");
    const auto table = [&](std::uint32_t offset, std::size_t bytes) {
        return offset >= sizeof(theme_file_header) && (offset & 3u) == 0 && offset <= size &&
               bytes <= size - offset;
    };
    if (!table(h->srgb_offset, std::size_t{4} * k_token_count) ||
        !table(h->linear_offset, std::size_t{8} * k_token_count) ||
        !table(h->status_offset, std::size_t{6} * k_status_count) ||
        !table(h->player_offset, std::size_t{6} * k_player_count) ||
        !table(h->syntax_offset, sizeof(theme_file_syntax) * k_syntax_count) ||
        !table(h->fonts_offset, sizeof(theme_file_fonts)))
        return lx::make_error(lx::ui_err::theme_format, "theme: a table is out of bounds");
    if (theme_checksum({data + sizeof(theme_file_header), size - sizeof(theme_file_header)}) !=
        h->checksum)
        return lx::make_error(lx::ui_err::theme_format, "theme: checksum mismatch");
    if (h->name[sizeof(h->name) - 1] != '\0' || h->appearance > 1 || h->density > 2)
        return lx::make_error(lx::ui_err::theme_format, "theme: bad header field");
    const auto* fonts = reinterpret_cast<const theme_file_fonts*>(data + h->fonts_offset);
    if (fonts->ui_family[63] != '\0' || fonts->buffer_family[63] != '\0' ||
        !(fonts->ui_size_px > 0.f && fonts->ui_size_px < 256.f) ||
        !(fonts->buffer_size_px > 0.f && fonts->buffer_size_px < 256.f))
        return lx::make_error(lx::ui_err::theme_format, "theme: bad font settings");
    const auto* status = reinterpret_cast<const std::uint16_t*>(data + h->status_offset);
    const auto* players = reinterpret_cast<const std::uint16_t*>(data + h->player_offset);
    const auto* syntax = reinterpret_cast<const theme_file_syntax*>(data + h->syntax_offset);
    for (unsigned i = 0; i < 3 * k_status_count; ++i)
        if (status[i] >= k_token_count)
            return lx::make_error(lx::ui_err::theme_format, "theme: status token out of range");
    for (unsigned i = 0; i < 3 * k_player_count; ++i)
        if (players[i] >= k_token_count)
            return lx::make_error(lx::ui_err::theme_format, "theme: player token out of range");
    for (unsigned i = 0; i < k_syntax_count; ++i)
        if (syntax[i].color >= k_token_count || syntax[i].style > 2)
            return lx::make_error(lx::ui_err::theme_format, "theme: bad syntax style");

    theme t{};
    t.data_ = data;
    t.size_ = size;
    t.header_ = h;
    t.srgb_ = reinterpret_cast<const std::uint32_t*>(data + h->srgb_offset);
    t.linear_ = reinterpret_cast<const std::uint16_t*>(data + h->linear_offset);
    t.status_ = status;
    t.players_ = players;
    t.syntax_ = syntax;
    std::memcpy(t.fonts_.ui_family, fonts->ui_family, sizeof(t.fonts_.ui_family));
    std::memcpy(t.fonts_.buffer_family, fonts->buffer_family, sizeof(t.fonts_.buffer_family));
    t.fonts_.ui_size_px = fonts->ui_size_px;
    t.fonts_.ui_line_height = fonts->ui_line_height;
    t.fonts_.buffer_size_px = fonts->buffer_size_px;
    t.fonts_.buffer_line_height = fonts->buffer_line_height;
    t.fonts_.buffer_line_height_standard = fonts->buffer_line_height_standard;
    t.fonts_.text_contrast = fonts->text_contrast;
    return t;
}

lx::result<lx::ui::theme> lx::ui::theme::from_bytes(std::span<const std::uint8_t> bytes,
                                                    bool copy) {
    if (!copy)
        return check(bytes.data(), bytes.size());
    if (bytes.size() < sizeof(theme_file_header) || bytes.size() > (std::size_t{1} << 24))
        return lx::make_error(lx::ui_err::theme_format, "theme: implausible size");
    auto owned = std::make_unique_for_overwrite<std::uint64_t[]>((bytes.size() + 7) / 8);
    std::memcpy(owned.get(), bytes.data(), bytes.size());
    auto r = check(reinterpret_cast<const std::uint8_t*>(owned.get()), bytes.size());
    if (r)
        r->owned_ = std::move(owned);
    return r;
}

lx::result<lx::ui::theme> lx::ui::theme::load(const char* path) {
    if (!path || !path[0])
        return lx::invalid_argument("theme::load: empty path");
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return lx::make_error(lx::io_err::not_found, "theme::load: no such file");
        if (errno == EACCES)
            return lx::make_error(lx::io_err::permission_denied, "theme::load: permission denied");
        return lx::make_error(lx::io_err::broken_pipe, "theme::load: open failed");
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return lx::make_error(lx::ui_err::theme_format, "theme::load: not a regular file");
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    if (size < sizeof(theme_file_header) || size > (std::size_t{1} << 24)) {
        ::close(fd);
        return lx::make_error(lx::ui_err::theme_format, "theme::load: implausible size");
    }
    auto owned = std::make_unique_for_overwrite<std::uint64_t[]>((size + 7) / 8);
    auto* bytes = reinterpret_cast<std::uint8_t*>(owned.get());
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, bytes + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != size)
        return lx::make_error(lx::ui_err::theme_format, "theme::load: short read");
    auto r = check(bytes, size);
    if (r)
        r->owned_ = std::move(owned);
    return r;
}

const lx::ui::theme& lx::ui::theme::builtin(lx::ui::appearance a) {
    // Never destroyed: static-lifetime views over the compiled-in bytes.
    static const theme* const themes = [] {
        auto* t = new theme[2]{};
        for (unsigned i = 0; i < 2; ++i) {
            auto r = from_bytes(builtin_theme_bytes(static_cast<lx::ui::appearance>(i)), false);
            if (r)
                t[i] = std::move(*r);
            else
                lx::trace::logger::global().log_error(r.get_error(), "ui.theme");
        }
        return t;
    }();
    return themes[a == lx::ui::appearance::light ? 1 : 0];
}

lx::ui::appearance lx::ui::theme::appearance_mode() const {
    return header_ && header_->appearance == 1 ? lx::ui::appearance::light
                                               : lx::ui::appearance::dark;
}

lx::ui::density lx::ui::theme::default_density() const {
    return header_ ? static_cast<density>(header_->density) : density::standard;
}

std::uint32_t lx::ui::theme::rgba8(tok t) const {
    const auto i = static_cast<unsigned>(t);
    return srgb_ && i < k_token_count ? srgb_[i] : 0u;
}

lx::color lx::ui::theme::color(tok t) const {
    const std::uint32_t v = rgba8(t);
    constexpr float k = 1.f / 255.f;
    return lx::color::rgb(static_cast<float>(v & 0xFFu) * k, static_cast<float>((v >> 8) & 0xFFu) * k,
                          static_cast<float>((v >> 16) & 0xFFu) * k,
                          static_cast<float>(v >> 24) * k);
}

lx::gfx::packed_rgba lx::ui::theme::linear(tok t) const {
    const auto i = static_cast<unsigned>(t);
    if (!linear_ || i >= k_token_count)
        return {};
    const std::uint16_t* p = linear_ + 4u * i;
    return {p[0], p[1], p[2], p[3]};
}

lx::ui::status_colors lx::ui::theme::status(status_kind k) const {
    const auto i = static_cast<unsigned>(k);
    if (!status_ || i >= k_status_count)
        return {};
    const std::uint16_t* p = status_ + 3u * i;
    return {static_cast<tok>(p[0]), static_cast<tok>(p[1]), static_cast<tok>(p[2])};
}

lx::ui::player_colors lx::ui::theme::player(unsigned index) const {
    if (!players_)
        return {};
    const std::uint16_t* p = players_ + 3u * (index % k_player_count);
    return {static_cast<tok>(p[0]), static_cast<tok>(p[1]), static_cast<tok>(p[2])};
}

lx::ui::syntax_style lx::ui::theme::syntax(unsigned index) const {
    if (!syntax_ || index >= k_syntax_count)
        return {};
    const theme_file_syntax& s = syntax_[index];
    syntax_style out{};
    out.color = static_cast<tok>(s.color);
    out.weight = s.weight;
    out.style = static_cast<font_style>(s.style);
    out.has_color = (s.flags & 1u) != 0;
    out.has_style = (s.flags & 2u) != 0;
    out.has_background = (s.flags & 4u) != 0;
    out.background = s.background;
    return out;
}

// ── the active theme ──

const lx::ui::theme& lx::ui::active_theme() {
    const lx_ui_theme_state& s = lx_ui_themes();
    return s.active ? *s.active : theme::builtin(appearance::dark);
}

void lx::ui::set_active_theme(theme t) {
    lx_ui_theme_state& s = lx_ui_themes();
    if (!t.valid()) {
        s.active = nullptr;
    } else {
        s.owned = std::move(t);
        s.active = &s.owned;
    }
    if (!s.density_set)
        s.density = active_theme().default_density();
    ++s.generation;
}

void lx::ui::set_active_builtin_theme(appearance a) {
    lx_ui_theme_state& s = lx_ui_themes();
    s.active = &theme::builtin(a);
    if (!s.density_set)
        s.density = s.active->default_density();
    ++s.generation;
}

std::uint64_t lx::ui::theme_generation() {
    return lx_ui_themes().generation;
}

lx::ui::density lx::ui::ui_density() {
    return lx_ui_themes().density;
}

void lx::ui::set_ui_density(density d) {
    lx_ui_theme_state& s = lx_ui_themes();
    s.density_set = true;
    if (s.density != d) {
        s.density = d;
        ++s.generation;
    }
}

float lx::ui::ui_rem_px() {
    const float rem = active_theme().fonts().ui_size_px;
    return rem > 0.f ? rem : 16.f;
}

// ── theme files ──

const char* lx::ui::theme_dir() {
    static char dir[512]{};
    static bool resolved = false;
    if (resolved)
        return dir;
    resolved = true;
    if (const char* env = ::getenv("LUMEN_THEME_DIR"); env && env[0]) {
        lx_ui_copy_cut(dir, sizeof(dir), env);
        return dir;
    }
#if defined(LUMEN_THEME_DIR_BUILD)
    if (lx_ui_is_dir(LUMEN_THEME_DIR_BUILD)) {
        lx_ui_copy_cut(dir, sizeof(dir), LUMEN_THEME_DIR_BUILD);
        return dir;
    }
#endif
#if defined(LUMEN_THEME_DIR_INSTALL_RELATIVE)
    {
        char exe[512]{};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            for (int cut = 0; cut < 2; ++cut)
                if (char* slash = std::strrchr(exe, '/'))
                    *slash = '\0';
            char rel[512]{};
            const int w = std::snprintf(rel, sizeof(rel), "%s/%s", exe,
                                        LUMEN_THEME_DIR_INSTALL_RELATIVE);
            if (w > 0 && static_cast<std::size_t>(w) < sizeof(rel) && lx_ui_is_dir(rel)) {
                lx_ui_copy_cut(dir, sizeof(dir), rel);
                return dir;
            }
        }
    }
#endif
#if defined(LUMEN_THEME_DIR_INSTALL)
    if (lx_ui_is_dir(LUMEN_THEME_DIR_INSTALL))
        lx_ui_copy_cut(dir, sizeof(dir), LUMEN_THEME_DIR_INSTALL);
#endif
    return dir;
}

lx::result<lx::ui::theme> lx::ui::load_theme_file(const char* file_name) {
    if (!file_name || !file_name[0] || std::strchr(file_name, '/'))
        return lx::invalid_argument("load_theme_file: bad name");
    const char* dir = theme_dir();
    if (!dir[0])
        return lx::make_error(lx::io_err::not_found, "load_theme_file: no theme directory");
    char path[768]{};
    const int w = std::snprintf(path, sizeof(path), "%s/%s.lxtheme", dir, file_name);
    if (w <= 0 || static_cast<std::size_t>(w) >= sizeof(path))
        return lx::invalid_argument("load_theme_file: path too long");
    return theme::load(path);
}

// ── theme_watcher ──

struct lx::ui::theme_watcher::job {
    char path[768]{};
    std::optional<lx::result<theme>> loaded{};
    lx::runtime::executor* executor = nullptr;
    /// Null once the watcher is gone (UI affinity).
    theme_watcher* owner = nullptr;
    bool running = false;
};

lx::ui::theme_watcher::~theme_watcher() {
    if (fd_ >= 0)
        ::close(fd_);
    if (job_) {
        if (job_->running)
            job_->owner = nullptr; // `land` frees it
        else
            delete job_;
    }
}

lx::result<void> lx::ui::theme_watcher::watch(const char* path, lx::runtime::executor* executor) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!path || !path[0] || std::strlen(path) >= sizeof(path_))
        return lx::invalid_argument("theme_watcher: bad path");
    const char* slash = std::strrchr(path, '/');
    if (slash) {
        const auto n = static_cast<std::size_t>(slash - path);
        if (n >= sizeof(dir_) || std::strlen(slash + 1) >= sizeof(file_))
            return lx::invalid_argument("theme_watcher: path too long");
        std::memcpy(dir_, path, n);
        dir_[n == 0 ? 1 : n] = '\0';
        if (n == 0)
            dir_[0] = '/';
        lx_ui_copy_cut(file_, sizeof(file_), slash + 1);
    } else {
        lx_ui_copy_cut(dir_, sizeof(dir_), ".");
        lx_ui_copy_cut(file_, sizeof(file_), path);
    }
    lx_ui_copy_cut(path_, sizeof(path_), path);
    if (fd_ < 0) {
        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0)
            return lx::make_error(lx::io_err::broken_pipe, "theme_watcher: inotify_init1 failed");
    }
    wd_ = ::inotify_add_watch(fd_, dir_, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd_ < 0)
        return lx::make_error(errno == ENOENT ? lx::io_err::not_found : lx::io_err::broken_pipe,
                              "theme_watcher: cannot watch the directory");
    executor_ = executor;
    return {};
}

bool lx::ui::theme_watcher::poll() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (fd_ < 0)
        return false;
    alignas(inotify_event) char buf[4096];
    bool changed = false;
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (ssize_t off = 0; off < n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            if (ev->wd == wd_ && ev->len > 0 && std::strcmp(ev->name, file_) == 0)
                changed = true;
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
        }
    }
    if (!changed)
        return false;
    if (!job_) {
        job_ = new job{};
        job_->owner = this;
    }
    if (job_->running) {
        again_ = true; // load once more when this one lands
        return false;
    }
    job& j = *job_;
    lx_ui_copy_cut(j.path, sizeof(j.path), path_);
    j.executor = executor_ ? executor_ : &lx::runtime::executor::global();
    j.running = true;
    if (!j.executor->post(lx::runtime::affinity::worker, lx::runtime::task{&theme_watcher::run, &j})) {
        j.running = false;
        lx::trace::logger::global().log(lx::trace::level::warn, "ui.theme",
                                        "theme_watcher: worker strand full, reload skipped");
        return false;
    }
    ++stats_.posted;
    return true;
}

void lx::ui::theme_watcher::run(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::worker);
    auto* j = static_cast<job*>(raw);
    j->loaded.emplace(theme::load(j->path));
    const lx::runtime::task back{&theme_watcher::land, j};
    for (unsigned attempt = 0; !j->executor->post(lx::runtime::affinity::ui, back); ++attempt) {
        if (attempt == 100000) {
            lx::trace::logger::global().log(lx::trace::level::error, "ui.theme",
                                            "theme_watcher: UI strand full, reload lost");
            return;
        }
        std::this_thread::yield();
    }
}

void lx::ui::theme_watcher::land(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    auto* j = static_cast<job*>(raw);
    j->running = false;
    theme_watcher* w = j->owner;
    if (!w) {
        delete j;
        return;
    }
    if (j->loaded && *j->loaded) {
        set_active_theme(std::move(**j->loaded));
        ++w->stats_.applied;
        lx::trace::logger::global().logf(lx::trace::level::info, "ui.theme", "theme reloaded: {}",
                                         active_theme().name());
    } else {
        ++w->stats_.failed;
        if (j->loaded)
            lx::trace::logger::global().log_error(j->loaded->get_error(), "ui.theme");
    }
    j->loaded.reset();
    if (w->again_) {
        w->again_ = false;
        lx_ui_copy_cut(j->path, sizeof(j->path), w->path_);
        j->running = true;
        if (j->executor->post(lx::runtime::affinity::worker, lx::runtime::task{&theme_watcher::run, j}))
            ++w->stats_.posted;
        else
            j->running = false;
    }
}
