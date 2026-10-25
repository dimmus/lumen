module;

export module lx.ui:style;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;

import :theme;

// ── The style value (ADR-010 §4.8) ─────────────────────────────────────────────────────
//
// A `style` is a packed value carried in element props: a box's layout as a flex container
// and item, and its paint — background, border, corner radii, elevation shadows, opacity —
// plus the text and cursor its contents inherit. Colours are theme tokens (`tok`), spacing is
// either a length or a step of the density-dependent scale (`sp`), radii are GPUI's scale
// (`radius`), so a style is the same value under every theme and density: the `styled` node
// resolves it against the active theme when it is applied. The builder reads like GPUI:
//
//     style{}.flex_row().gap(sp::b08).px(sp::b12).h(px(32))
//            .bg(tok::surface_background).border_1(tok::border_variant)
//            .rounded(radius::md).shadow(elevation::elevated_surface)
//
// Lengths are `layout::length`s (32 bits: a two-bit unit and a 30-bit float). A spacing step
// rides in a length of unit `automatic` with a non-zero value (step + 1); plain `auto` is
// value 0, so a style's lengths stay one 32-bit word each and compare bitwise.
//
// Size: 21 lengths (sizes, basis, gaps, padding, margin, insets) are 84 B of the 112; the
// flex enums pack into one word, grow and shrink are halves, the paint and text fields are
// bytes and u16 tokens. `sizeof(style)` is asserted below.

export namespace lx::ui {

using lx::layout::length;
using lx::layout::percent;
using lx::layout::px;
using lx::layout::rem;

/// A length or a spacing step: what the spacing setters take.
struct dim {
    length value{};
    constexpr dim(length l) : value{l} {} // NOLINT(google-explicit-constructor)
    constexpr dim(sp step) // NOLINT(google-explicit-constructor)
        : value{length::make(lx::layout::unit::automatic, static_cast<float>(1 + static_cast<int>(step)))} {}
};

/// A spacing step stored in a length, or empty for a plain length.
[[nodiscard]] constexpr std::optional<sp> spacing_of(length l) {
    if (l.kind() != lx::layout::unit::automatic || l.value() <= 0.f)
        return std::nullopt;
    return static_cast<sp>(static_cast<int>(l.value()) - 1);
}

/// The pointer shape over a box (wp_cursor_shape names).
enum class cursor_style : std::uint8_t {
    automatic,
    arrow,
    pointer,
    text,
    move,
    grab,
    grabbing,
    not_allowed,
    ew_resize,
    ns_resize,
    crosshair,
};

struct style {
    // ── layout (the flex engine's, §4.7) ──
    length width = lx::layout::auto_length;
    length height = lx::layout::auto_length;
    length min_width = lx::layout::auto_length;
    length min_height = lx::layout::auto_length;
    length max_width = lx::layout::auto_length;
    length max_height = lx::layout::auto_length;
    length basis = lx::layout::auto_length;
    length row_gap{};
    length column_gap{};
    length padding[4]{};  ///< top, right, bottom, left
    length margin[4]{};
    length inset[4]{lx::layout::auto_length, lx::layout::auto_length, lx::layout::auto_length,
                    lx::layout::auto_length};
    std::uint16_t grow = 0;       ///< f16
    std::uint16_t shrink = 0x3C00; ///< f16, 1
    /// direction 2, wrap 2, justify 4, align-content 4, align-items 3, align-self 3,
    /// position 1, overflow-x 2, overflow-y 2 (bits from the low end). The CSS initial values.
    std::uint32_t flex_bits = k_flex_initial;

    // ── paint (and the text colour, a token beside the others) ──
    tok bg_token = tok::none;
    tok border_token = tok::none;
    /// Inherited by the box's text.
    tok text_token = tok::none;
    std::uint8_t border_widths[4]{}; ///< px: top, right, bottom, left
    radius corners[4]{};             ///< top-left, top-right, bottom-right, bottom-left
    elevation elevation_level = elevation::flat;
    std::uint8_t opacity_255 = 255;
    cursor_style cursor_kind = cursor_style::automatic;

    // ── text (inherited by the box's text) ──
    lx::ui::text_size text_step = lx::ui::text_size::inherit;
    /// CSS weight / 100 (4 regular, 7 bold); 0 inherits.
    std::uint8_t weight_100 = 0;
    /// Line height × 64 relative to the size; 0 is φ (GPUI's default).
    std::uint8_t line_height_64 = 0;

    static constexpr std::uint32_t k_flex_initial =
        (static_cast<std::uint32_t>(lx::layout::content_align::stretch) << 8) |
        (static_cast<std::uint32_t>(lx::layout::item_align::stretch) << 12);

    constexpr bool operator==(const style&) const = default;

    // ── flex container ──
    constexpr style& flex_row() { return dir(lx::layout::flex_direction::row); }
    constexpr style& flex_col() { return dir(lx::layout::flex_direction::column); }
    constexpr style& flex_row_reverse() { return dir(lx::layout::flex_direction::row_reverse); }
    constexpr style& flex_col_reverse() { return dir(lx::layout::flex_direction::column_reverse); }
    constexpr style& flex_wrap() { return bits(2, 2, 1); }
    constexpr style& flex_wrap_reverse() { return bits(2, 2, 2); }
    constexpr style& flex_nowrap() { return bits(2, 2, 0); }
    constexpr style& gap(dim d) { row_gap = column_gap = d.value; return *this; }
    constexpr style& gap_x(dim d) { column_gap = d.value; return *this; }
    constexpr style& gap_y(dim d) { row_gap = d.value; return *this; }
    constexpr style& justify(lx::layout::content_align a) { return bits(4, 4, static_cast<unsigned>(a)); }
    constexpr style& justify_start() { return justify(lx::layout::content_align::flex_start); }
    constexpr style& justify_end() { return justify(lx::layout::content_align::flex_end); }
    constexpr style& justify_center() { return justify(lx::layout::content_align::center); }
    constexpr style& justify_between() { return justify(lx::layout::content_align::space_between); }
    constexpr style& justify_around() { return justify(lx::layout::content_align::space_around); }
    constexpr style& justify_evenly() { return justify(lx::layout::content_align::space_evenly); }
    constexpr style& content(lx::layout::content_align a) { return bits(8, 4, static_cast<unsigned>(a)); }
    constexpr style& items(lx::layout::item_align a) { return bits(12, 3, static_cast<unsigned>(a)); }
    constexpr style& items_start() { return items(lx::layout::item_align::flex_start); }
    constexpr style& items_end() { return items(lx::layout::item_align::flex_end); }
    constexpr style& items_center() { return items(lx::layout::item_align::center); }
    constexpr style& items_stretch() { return items(lx::layout::item_align::stretch); }

    // ── flex item ──
    constexpr style& self_align(lx::layout::item_align a) { return bits(15, 3, static_cast<unsigned>(a)); }
    constexpr style& self_start() { return self_align(lx::layout::item_align::flex_start); }
    constexpr style& self_end() { return self_align(lx::layout::item_align::flex_end); }
    constexpr style& self_center() { return self_align(lx::layout::item_align::center); }
    constexpr style& self_stretch() { return self_align(lx::layout::item_align::stretch); }
    style& flex_grow(float f = 1.f);
    style& flex_shrink(float f = 1.f);
    constexpr style& flex_shrink_0() { shrink = 0; return *this; }
    constexpr style& flex_basis(dim d) { basis = d.value; return *this; }
    /// `flex: 1 1 0%`.
    style& flex_1();
    /// `flex: 0 0 auto`.
    constexpr style& flex_none() { grow = 0; shrink = 0; basis = lx::layout::auto_length; return *this; }

    // ── size ──
    constexpr style& w(dim d) { width = d.value; return *this; }
    constexpr style& h(dim d) { height = d.value; return *this; }
    constexpr style& size(dim d) { width = height = d.value; return *this; }
    constexpr style& min_w(dim d) { min_width = d.value; return *this; }
    constexpr style& min_h(dim d) { min_height = d.value; return *this; }
    constexpr style& max_w(dim d) { max_width = d.value; return *this; }
    constexpr style& max_h(dim d) { max_height = d.value; return *this; }
    constexpr style& w_full() { return w(lx::layout::percent(100.f)); }
    constexpr style& h_full() { return h(lx::layout::percent(100.f)); }
    constexpr style& size_full() { return w_full().h_full(); }

    // ── padding, margin ──
    constexpr style& p(dim d) { return edges(padding, d, d, d, d); }
    constexpr style& px(dim d) { padding[1] = padding[3] = d.value; return *this; }
    constexpr style& py(dim d) { padding[0] = padding[2] = d.value; return *this; }
    constexpr style& pt(dim d) { padding[0] = d.value; return *this; }
    constexpr style& pr(dim d) { padding[1] = d.value; return *this; }
    constexpr style& pb(dim d) { padding[2] = d.value; return *this; }
    constexpr style& pl(dim d) { padding[3] = d.value; return *this; }
    constexpr style& m(dim d) { return edges(margin, d, d, d, d); }
    constexpr style& mx(dim d) { margin[1] = margin[3] = d.value; return *this; }
    constexpr style& my(dim d) { margin[0] = margin[2] = d.value; return *this; }
    constexpr style& mt(dim d) { margin[0] = d.value; return *this; }
    constexpr style& mr(dim d) { margin[1] = d.value; return *this; }
    constexpr style& mb(dim d) { margin[2] = d.value; return *this; }
    constexpr style& ml(dim d) { margin[3] = d.value; return *this; }
    /// Auto horizontal margins: centres the box on a row's main axis.
    constexpr style& mx_auto() { margin[1] = margin[3] = lx::layout::auto_length; return *this; }

    // ── position ──
    constexpr style& relative() { return bits(18, 1, 0); }
    constexpr style& absolute() { return bits(18, 1, 1); }
    constexpr style& top(dim d) { inset[0] = d.value; return *this; }
    constexpr style& right(dim d) { inset[1] = d.value; return *this; }
    constexpr style& bottom(dim d) { inset[2] = d.value; return *this; }
    constexpr style& left(dim d) { inset[3] = d.value; return *this; }
    constexpr style& inset_all(dim d) { return edges(inset, d, d, d, d); }

    // ── overflow ──
    constexpr style& overflow_hidden() { return overflow_x(lx::layout::overflow::hidden).overflow_y(lx::layout::overflow::hidden); }
    constexpr style& overflow_scroll() { return overflow_x(lx::layout::overflow::scroll).overflow_y(lx::layout::overflow::scroll); }
    constexpr style& overflow_x(lx::layout::overflow o) { return bits(19, 2, static_cast<unsigned>(o)); }
    constexpr style& overflow_y(lx::layout::overflow o) { return bits(21, 2, static_cast<unsigned>(o)); }

    // ── paint ──
    constexpr style& bg(tok t) { bg_token = t; return *this; }
    /// Every edge `width` px in `t`.
    constexpr style& border(unsigned width, tok t) {
        const auto w8 = static_cast<std::uint8_t>(width < 255 ? width : 255);
        border_widths[0] = border_widths[1] = border_widths[2] = border_widths[3] = w8;
        border_token = t;
        return *this;
    }
    /// Zed's borders are always 1 px (§5.5).
    constexpr style& border_1(tok t) { return border(1, t); }
    constexpr style& border_t_1(tok t) { border_widths[0] = 1; border_token = t; return *this; }
    constexpr style& border_r_1(tok t) { border_widths[1] = 1; border_token = t; return *this; }
    constexpr style& border_b_1(tok t) { border_widths[2] = 1; border_token = t; return *this; }
    constexpr style& border_l_1(tok t) { border_widths[3] = 1; border_token = t; return *this; }
    constexpr style& border_color(tok t) { border_token = t; return *this; }
    constexpr style& rounded(radius r) { corners[0] = corners[1] = corners[2] = corners[3] = r; return *this; }
    constexpr style& rounded_t(radius r) { corners[0] = corners[1] = r; return *this; }
    constexpr style& rounded_b(radius r) { corners[2] = corners[3] = r; return *this; }
    constexpr style& rounded_l(radius r) { corners[0] = corners[3] = r; return *this; }
    constexpr style& rounded_r(radius r) { corners[1] = corners[2] = r; return *this; }
    /// The elevation's shadows (§5.2); elevated and modal surfaces also take a 1 px
    /// `border.variant` border when no border is set.
    constexpr style& shadow(elevation e) { elevation_level = e; return *this; }
    style& opacity(float o);
    constexpr style& cursor(cursor_style c) { cursor_kind = c; return *this; }

    // ── text ──
    constexpr style& text_color(tok t) { text_token = t; return *this; }
    constexpr style& text_size(lx::ui::text_size s) { text_step = s; return *this; }
    constexpr style& text_xs() { text_step = lx::ui::text_size::xsmall; return *this; }
    constexpr style& text_sm() { text_step = lx::ui::text_size::small; return *this; }
    constexpr style& text_base() { text_step = lx::ui::text_size::normal; return *this; }
    constexpr style& text_lg() { text_step = lx::ui::text_size::large; return *this; }
    constexpr style& font_weight(unsigned css_weight) {
        weight_100 = static_cast<std::uint8_t>(std::clamp(css_weight, 100u, 900u) / 100u);
        return *this;
    }
    style& line_height(float relative);

    // ── reading the packed fields ──
    [[nodiscard]] constexpr lx::layout::flex_direction direction() const { return static_cast<lx::layout::flex_direction>(field(0, 2)); }
    [[nodiscard]] constexpr lx::layout::flex_wrap wrap() const { return static_cast<lx::layout::flex_wrap>(field(2, 2)); }
    [[nodiscard]] constexpr lx::layout::content_align justify_content() const { return static_cast<lx::layout::content_align>(field(4, 4)); }
    [[nodiscard]] constexpr lx::layout::content_align align_content() const { return static_cast<lx::layout::content_align>(field(8, 4)); }
    [[nodiscard]] constexpr lx::layout::item_align align_items() const { return static_cast<lx::layout::item_align>(field(12, 3)); }
    [[nodiscard]] constexpr lx::layout::item_align align_self() const { return static_cast<lx::layout::item_align>(field(15, 3)); }
    [[nodiscard]] constexpr lx::layout::positioning position() const { return static_cast<lx::layout::positioning>(field(18, 1)); }
    [[nodiscard]] constexpr lx::layout::overflow overflow_x_mode() const { return static_cast<lx::layout::overflow>(field(19, 2)); }
    [[nodiscard]] constexpr lx::layout::overflow overflow_y_mode() const { return static_cast<lx::layout::overflow>(field(21, 2)); }

private:
    constexpr style& dir(lx::layout::flex_direction d) { return bits(0, 2, static_cast<unsigned>(d)); }
    constexpr style& bits(unsigned shift, unsigned width, unsigned value) {
        const std::uint32_t mask = ((1u << width) - 1u) << shift;
        flex_bits = (flex_bits & ~mask) | ((static_cast<std::uint32_t>(value) << shift) & mask);
        return *this;
    }
    [[nodiscard]] constexpr unsigned field(unsigned shift, unsigned width) const {
        return (flex_bits >> shift) & ((1u << width) - 1u);
    }
    constexpr style& edges(length (&e)[4], dim t, dim r, dim b, dim l) {
        e[0] = t.value;
        e[1] = r.value;
        e[2] = b.value;
        e[3] = l.value;
        return *this;
    }
};

static_assert(sizeof(style) == 112, "the packed style is 112 bytes (ADR-010 §4.8 asks ≈96)");
static_assert(std::is_trivially_copyable_v<style>);

/// What a style's lengths, spacing steps and text size resolve against.
struct style_context {
    density spacing = density::standard;
    float rem_px = 16.f;
};

/// The style's layout as the flex engine takes it: spacing steps resolved through the
/// density into px, everything else as it is.
[[nodiscard]] lx::layout::flex_style resolve_layout(const style& s, const style_context& ctx);
/// A length with a spacing step resolved (other lengths unchanged).
[[nodiscard]] length resolve_length(length l, const style_context& ctx);
/// The text size in px (0 for `inherit`).
[[nodiscard]] float text_size_px(text_size size, const theme& t, float rem_px);

// ── Legacy string-keyed style (the old `lx.ui:widgets` tree; removed with it in Phase 5) ──

enum class state { normal, hovered, pressed, focused, disabled };

struct style_property {
    const char* key = "";
    lx::color value{};
    float number = 0.f;
};

class style_ref {
public:
    style_ref() = default;
    explicit style_ref(const theme* t, const char* widget_type);

    style_ref& set(const char* property, lx::color value);
    style_ref& set(const char* property, float value);
    /// The property unset: the old widgets read nothing from a theme.
    [[nodiscard]] style_property get(state st, const char* property) const;

private:
    const theme* theme_ = nullptr;
    const char* widget_type_ = "widget";
};

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

lx::ui::style& lx::ui::style::flex_grow(float f) {
    grow = lx::gfx::to_half(f > 0.f ? f : 0.f);
    return *this;
}

lx::ui::style& lx::ui::style::flex_shrink(float f) {
    shrink = lx::gfx::to_half(f > 0.f ? f : 0.f);
    return *this;
}

lx::ui::style& lx::ui::style::flex_1() {
    grow = shrink = lx::gfx::to_half(1.f);
    basis = lx::layout::percent(0.f);
    return *this;
}

lx::ui::style& lx::ui::style::opacity(float o) {
    opacity_255 = static_cast<std::uint8_t>(std::lround(std::clamp(o, 0.f, 1.f) * 255.f));
    return *this;
}

lx::ui::style& lx::ui::style::line_height(float relative) {
    const long v = std::lround(std::clamp(relative, 0.f, 3.98f) * 64.f);
    line_height_64 = static_cast<std::uint8_t>(v < 1 ? 1 : v);
    return *this;
}

lx::layout::length lx::ui::resolve_length(length l, const style_context& ctx) {
    if (const std::optional<sp> step = spacing_of(l))
        return lx::layout::px(spacing_px(*step, ctx.spacing, ctx.rem_px));
    return l;
}

lx::layout::flex_style lx::ui::resolve_layout(const style& s, const style_context& ctx) {
    const auto r = [&ctx](length l) { return resolve_length(l, ctx); };
    lx::layout::flex_style f{};
    f.direction = s.direction();
    f.wrap = s.wrap();
    f.justify_content = s.justify_content();
    f.align_content = s.align_content();
    f.align_items = s.align_items();
    f.align_self = s.align_self();
    f.position = s.position();
    f.overflow_x = s.overflow_x_mode();
    f.overflow_y = s.overflow_y_mode();
    f.flex_grow = lx::gfx::from_half(s.grow);
    f.flex_shrink = lx::gfx::from_half(s.shrink);
    f.flex_basis = r(s.basis);
    f.width = r(s.width);
    f.height = r(s.height);
    f.min_width = r(s.min_width);
    f.min_height = r(s.min_height);
    f.max_width = r(s.max_width);
    f.max_height = r(s.max_height);
    f.row_gap = r(s.row_gap);
    f.column_gap = r(s.column_gap);
    f.padding = {r(s.padding[0]), r(s.padding[1]), r(s.padding[2]), r(s.padding[3])};
    f.margin = {r(s.margin[0]), r(s.margin[1]), r(s.margin[2]), r(s.margin[3])};
    f.inset = {r(s.inset[0]), r(s.inset[1]), r(s.inset[2]), r(s.inset[3])};
    f.border = {lx::layout::px(s.border_widths[0]), lx::layout::px(s.border_widths[1]),
                lx::layout::px(s.border_widths[2]), lx::layout::px(s.border_widths[3])};
    return f;
}

float lx::ui::text_size_px(lx::ui::text_size size, const theme& t, float rem_px) {
    switch (size) {
    case text_size::xsmall: return 0.625f * rem_px;
    case text_size::small: return 0.75f * rem_px;
    case text_size::normal: return 0.875f * rem_px;
    case text_size::large: return rem_px;
    case text_size::ui: return t.fonts().ui_size_px;
    case text_size::editor: return t.fonts().buffer_size_px;
    default: return 0.f;
    }
}

lx::ui::style_ref::style_ref(const theme* t, const char* type) : theme_{t}, widget_type_{type} {}
lx::ui::style_ref& lx::ui::style_ref::set(const char*, lx::color) { return *this; }
lx::ui::style_ref& lx::ui::style_ref::set(const char*, float) { return *this; }
lx::ui::style_property lx::ui::style_ref::get(state, const char* property) const {
    (void)theme_;
    (void)widget_type_;
    return {property, {}, 0.f};
}
