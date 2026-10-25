module;

export module lx.ui:control;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.a11y;
import lx.text;

import :element;
import :events;
import :flex;
import :focus;
import :icon;
import :interact;
import :invalidate;
import :node;
import :style;
import :text_node;
import :theme;

// ── Controls: the node every component is built from (ADR-010 §5.5, §5.6) ───────────────
//
// A `control_node` is a flex container (its layout a `style`, resolved through the density
// as a styled box's is) that paints one rounded, bordered box whose colours depend on the
// pointer: a `control_paint` gives the box at rest, hovered and pressed, each a background and
// a border `paint_color` — a theme token, faded, or blended with a second token (GPUI's
// `Hsla::blend`, what Zed's switch and key hints use), or darkened (`Theme::darken`, a tinted
// button's hover). Disabled and selected are props: the component picks the rest paint and the
// label colours for them when it describes, so a hover never touches a label (§5.5).
//
// Interaction is per node: `interactive` takes hover and press (`pointer_state`), `focusable`
// takes the focus (`focus_manager`), space and enter activate it, a press-release activates
// it, and `on_activate` gets the control's `value` (a toggle's next state, a tab's index). A
// node that only paints follows the nearest interactive ancestor's state (`group_paint`:
// GPUI's `group_hover`, e.g. a checkbox's box inside its clickable row). The focus ring is the
// node's own rounded box with a 1 px `border.focused` border, drawn over its children when the
// state source shows the ring. A node with `nav` set moves the focus among its focusable
// descendants with the arrow keys, home and end (a tab bar, a status bar).
//
// Resolution happens in `apply`, against the active theme (a new theme or density describes
// again): the three boxes are packed once (`pack_quad_style`), `emit` copies one. A state
// change repaints the node's bounds and nothing else; it allocates nothing.

export namespace lx::ui {

/// A colour from the theme: `base` at `base_alpha`, darkened by `darken` (HSL lightness, in
/// 1/255), with `over` at `over_alpha` composited on top. `tok::none` base is transparent.
struct paint_color {
    tok base = tok::none;
    tok over = tok::none;
    std::uint8_t base_alpha = 255;
    std::uint8_t over_alpha = 0;
    std::uint8_t darken = 0;
    constexpr bool operator==(const paint_color&) const = default;
};

[[nodiscard]] constexpr std::uint8_t alpha_255(float a) {
    return static_cast<std::uint8_t>(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
}
/// `t` at `alpha`.
[[nodiscard]] constexpr paint_color paint(tok t, float alpha = 1.f) {
    return {t, tok::none, alpha_255(alpha), 0, 0};
}
/// `base` with `over` at `over_alpha` on top (GPUI `base.blend(over.opacity(a))`).
[[nodiscard]] constexpr paint_color paint_blend(tok base, tok over, float over_alpha,
                                                float base_alpha = 1.f) {
    return {base, over, alpha_255(base_alpha), alpha_255(over_alpha), 0};
}
/// `t` with its HSL lightness lowered by `amount` (0–1; Zed's `Theme::darken`).
[[nodiscard]] constexpr paint_color paint_darkened(tok t, float amount) {
    return {t, tok::none, 255, 0, alpha_255(amount)};
}
inline constexpr paint_color k_transparent{};

[[nodiscard]] lx::color resolve_color(const paint_color& c, const theme& t);

/// A control's box in one pointer state.
struct box_paint {
    paint_color bg{};
    paint_color border{};
    constexpr bool operator==(const box_paint&) const = default;
};

/// What a control paints: its box at rest, hovered and pressed; border widths (px) and
/// corner radii shared by the three; one optional drop shadow; an opacity over everything.
struct control_paint {
    box_paint rest{};
    box_paint hover{};
    box_paint active{};
    std::uint8_t border_widths[4]{}; ///< top, right, bottom, left
    radius corners[4]{};             ///< top-left, top-right, bottom-right, bottom-left
    std::int8_t shadow_y = 0;        ///< offset of the shadow, px
    std::uint8_t shadow_blur = 0;    ///< σ, px; 0 is a hard edge
    paint_color shadow{};            ///< none: no shadow
    std::uint8_t opacity = 255;
    constexpr bool operator==(const control_paint&) const = default;

    /// The same box in every state.
    [[nodiscard]] static constexpr control_paint fixed(box_paint b) {
        control_paint p{};
        p.rest = p.hover = p.active = b;
        return p;
    }
};

struct control_props {
    /// Layout as a flex container and as a flex item (its paint fields are not read).
    lx::ui::style layout{};
    control_paint paint{};
    handler on_activate{};
    /// What `on_activate` receives.
    int value = 0;
    /// `ui_node::focus_order`: 0 tree order, > 0 first, < 0 out of the tab sequence.
    std::int16_t tab_index = 0;
    a11y::role role = a11y::role::unknown;
    /// The accessible name (copied, up to 47 bytes).
    const char* name = nullptr;
    /// a11y `state_flag` bits beyond focus and disabled (checked, selected, pressed).
    unsigned a11y_state = 0;
    bool interactive = false;
    bool focusable = false;
    bool disabled = false;
    bool selected = false;
    /// Paint from the nearest interactive ancestor's hover / press / ring.
    bool group_paint = false;
    /// Draw the focus ring when the state source shows it.
    bool focus_ring = false;
    /// Wants `on_timer(k_timer_hover_intent)` after a 500 ms hover (a tooltip).
    bool hover_intent = false;
    /// Arrow keys move the focus among the focusable descendants along this axis.
    nav_axis nav = nav_axis::none;
    bool nav_wrap = true;
};

class control_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the layout changed; a paint, state or handler change damages the bounds.
    bool apply(const control_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;

    [[nodiscard]] int focus_order() const override { return tab_index_; }
    bool on_focus_key(key_event event, focus_manager& focus) override;
    bool activate() override;
    [[nodiscard]] a11y::node accessibility_node() const override;

    /// Which box `emit` paints now: 0 rest, 1 hover, 2 pressed.
    [[nodiscard]] unsigned paint_state() const;
    /// The background `emit` paints now, straight sRGB (tests).
    [[nodiscard]] lx::color background() const;
    [[nodiscard]] bool selected() const { return selected_; }
    [[nodiscard]] int value() const { return value_; }
    /// Activations since construction (tests).
    [[nodiscard]] std::uint64_t activations() const { return activations_; }
    /// The node whose hover / press / ring this one paints (itself, or the nearest
    /// interactive ancestor with `group_paint`).
    [[nodiscard]] const ui_node& state_source() const;

private:
    void resolve();

    lx::ui::style layout_{};
    lx::layout::flex_style flex_{};
    control_paint paint_{};
    std::uint64_t generation_ = 0;
    packed_quad_style quads_[3]{};
    lx::color backgrounds_[3]{};
    bool has_quad_[3]{};
    float radii_[4]{};
    shadow_style shadow_{};
    bool has_shadow_ = false;
    handler on_activate_{};
    int value_ = 0;
    std::uint64_t activations_ = 0;
    std::int16_t tab_index_ = 0;
    a11y::role role_ = a11y::role::unknown;
    unsigned a11y_state_ = 0;
    nav_axis nav_ = nav_axis::none;
    bool nav_wrap_ = true;
    bool selected_ = false;
    bool group_paint_ = false;
    bool ring_ = false;
    bool applied_ = false;
    char name_[48]{};
};

/// A control over `children`.
[[nodiscard]] child control(const control_props& props, std::span<const child> children,
                            element_key key = 0);

/// An atlas image (an avatar's picture), drawn as a `poly_sprite` with rounded corners,
/// scaled to its bounds. The tile is looked up and stamped used each frame.
struct image_props {
    lx::gfx::atlas* atlas = nullptr;
    lx::gfx::atlas_tile_id tile{};
    float corner_radius = 0.f;
};

class image_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;
    bool apply(const image_props& props);
    void emit(draw_scope& out) const override;
    [[nodiscard]] a11y::node accessibility_node() const override;

private:
    image_props props_{};
};

// ── What components draw text and icons with ──

/// The text engine and glyph cache labels lay out and rasterise with, and the icon cache
/// icons come from: set once by the app (UI affinity). Without a glyph cache, labels lay out
/// but draw nothing; without an icon cache, icons draw nothing.
struct ui_resources {
    text_raster_context text{};
    icon_cache* icons = nullptr;
};

void set_ui_resources(const ui_resources& resources);
[[nodiscard]] const ui_resources& active_ui_resources();

/// An icon from the active resources' cache.
[[nodiscard]] child icon_view(icon id, float size_px = 16.f, tok color = tok::icon);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

/// sRGB (straight) ↔ HSL, as GPUI's `Hsla` does it.
struct lx_ui_hsl {
    float h = 0.f;
    float s = 0.f;
    float l = 0.f;
};

[[nodiscard]] lx_ui_hsl lx_ui_to_hsl(lx::color c) {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    lx_ui_hsl o{};
    o.l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d <= 0.f)
        return o;
    o.s = o.l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
    if (mx == c.r)
        o.h = (c.g - c.b) / d + (c.g < c.b ? 6.f : 0.f);
    else if (mx == c.g)
        o.h = (c.b - c.r) / d + 2.f;
    else
        o.h = (c.r - c.g) / d + 4.f;
    o.h /= 6.f;
    return o;
}

[[nodiscard]] float lx_ui_hue(float p, float q, float t) {
    if (t < 0.f)
        t += 1.f;
    if (t > 1.f)
        t -= 1.f;
    if (t < 1.f / 6.f)
        return p + (q - p) * 6.f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.f / 3.f)
        return p + (q - p) * (2.f / 3.f - t) * 6.f;
    return p;
}

[[nodiscard]] lx::color lx_ui_from_hsl(lx_ui_hsl h, float a) {
    if (h.s <= 0.f)
        return lx::color::rgb(h.l, h.l, h.l, a);
    const float q = h.l < 0.5f ? h.l * (1.f + h.s) : h.l + h.s - h.l * h.s;
    const float p = 2.f * h.l - q;
    return lx::color::rgb(lx_ui_hue(p, q, h.h + 1.f / 3.f), lx_ui_hue(p, q, h.h),
                          lx_ui_hue(p, q, h.h - 1.f / 3.f), a);
}

/// `over` composited onto `base`, straight alpha in and out.
[[nodiscard]] lx::color lx_ui_over(lx::color base, lx::color over) {
    const float a = over.a + base.a * (1.f - over.a);
    if (a <= 0.f)
        return lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    const auto ch = [&](float b, float o) { return (o * over.a + b * base.a * (1.f - over.a)) / a; };
    return lx::color::rgb(ch(base.r, over.r), ch(base.g, over.g), ch(base.b, over.b), a);
}

lx::ui::ui_resources g_ui_resources{};

} // namespace

lx::color lx::ui::resolve_color(const paint_color& c, const theme& t) {
    lx::color out = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    if (c.base != tok::none) {
        out = t.color(c.base);
        out.a *= static_cast<float>(c.base_alpha) / 255.f;
        if (c.darken) {
            lx_ui_hsl h = lx_ui_to_hsl(out);
            h.l = std::max(h.l - static_cast<float>(c.darken) / 255.f, 0.f);
            out = lx_ui_from_hsl(h, out.a);
        }
    }
    if (c.over != tok::none && c.over_alpha) {
        lx::color o = t.color(c.over);
        o.a *= static_cast<float>(c.over_alpha) / 255.f;
        out = lx_ui_over(out, o);
    }
    return out;
}

// ── control_node ──

lx::ui::element_type_id lx::ui::control_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::control_node::type() const { return static_type(); }
const char* lx::ui::control_node::type_name() const { return "control"; }

void lx::ui::control_node::resolve() {
    const theme& t = active_theme();
    const float rem = ui_rem_px();
    bool any_border = false;
    quad_style q{};
    for (unsigned i = 0; i < 4; ++i) {
        q.border_widths[i] = static_cast<float>(paint_.border_widths[i]);
        any_border = any_border || paint_.border_widths[i] != 0;
        q.corner_radii[i] = radii_[i] = radius_px(paint_.corners[i], rem);
    }
    const box_paint* boxes[3] = {&paint_.rest, &paint_.hover, &paint_.active};
    for (unsigned s = 0; s < 3; ++s) {
        q.background = resolve_color(boxes[s]->bg, t);
        q.border_color = any_border ? resolve_color(boxes[s]->border, t)
                                    : lx::color::rgb(0.f, 0.f, 0.f, 0.f);
        backgrounds_[s] = q.background;
        has_quad_[s] = q.background.a > 0.f || q.border_color.a > 0.f;
        quads_[s] = has_quad_[s] ? pack_quad_style(q) : packed_quad_style{};
    }
    has_shadow_ = paint_.shadow.base != tok::none;
    shadow_ = {};
    if (has_shadow_) {
        shadow_.color = resolve_color(paint_.shadow, t);
        for (unsigned i = 0; i < 4; ++i)
            shadow_.corner_radii[i] = radii_[i];
        shadow_.blur_sigma = static_cast<float>(paint_.shadow_blur);
    }
}

bool lx::ui::control_node::apply(const control_props& p) {
    const std::uint64_t generation = theme_generation();
    const bool new_theme = generation != generation_ || !applied_;
    bool repaint = new_theme || !(p.paint == paint_) || p.selected != selected_ ||
                   p.disabled != has_flag(node_flag::disabled) ||
                   p.group_paint != group_paint_ || p.focus_ring != ring_;
    // Layout: resolved through the density, compared as the engine reads it.
    bool layout_changed = false;
    const bool borders_changed = std::memcmp(p.paint.border_widths, paint_.border_widths,
                                             sizeof(paint_.border_widths)) != 0;
    if (new_theme || borders_changed || !(p.layout == layout_)) {
        lx::layout::flex_style flex = resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        // The painted border is the box's layout border too (CSS border-box, GPUI's).
        flex.border = {lx::layout::px(p.paint.border_widths[0]), lx::layout::px(p.paint.border_widths[1]),
                       lx::layout::px(p.paint.border_widths[2]), lx::layout::px(p.paint.border_widths[3])};
        layout_changed = !(flex == flex_) || !applied_;
        flex_ = flex;
        layout_ = p.layout;
    }
    if (repaint) {
        paint_ = p.paint;
        generation_ = generation;
        resolve();
    }
    selected_ = p.selected;
    group_paint_ = p.group_paint;
    ring_ = p.focus_ring;
    on_activate_ = p.on_activate;
    value_ = p.value;
    tab_index_ = p.tab_index;
    role_ = p.role;
    a11y_state_ = p.a11y_state;
    nav_ = p.nav;
    nav_wrap_ = p.nav_wrap;
    set_flag(node_flag::interactive, p.interactive);
    set_flag(node_flag::focusable, p.focusable);
    set_flag(node_flag::disabled, p.disabled);
    set_flag(node_flag::hover_intent, p.hover_intent);
    if (p.disabled) {
        // A disabled control shows no hover or press (`button_like.rs`: hover and active
        // apply only `when(!self.disabled)`).
        set_flag(node_flag::hovered, false);
        set_flag(node_flag::pressed, false);
    }
    std::size_t n = p.name ? std::strlen(p.name) : 0;
    if (n >= sizeof(name_))
        n = sizeof(name_) - 1;
    if (n)
        std::memcpy(name_, p.name, n);
    name_[n] = '\0';
    applied_ = true;
    if (repaint && !layout_changed)
        invalidate_paint(*this);
    return layout_changed;
}

lx::size2i lx::ui::control_node::measure(lx::layout::constraints c, size_hint hint) const {
    return flex_container_measure(*this, flex_, c, hint, false);
}

lx::size2i lx::ui::control_node::measure_content(lx::layout::constraints c,
                                                 size_hint hint) const {
    return flex_container_measure(*this, flex_, c, hint, true);
}

void lx::ui::control_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    (void)flex_container_arrange(*this, flex_, bounds);
}

const lx::layout::flex_style* lx::ui::control_node::flex_item_style() const { return &flex_; }

const lx::ui::ui_node& lx::ui::control_node::state_source() const {
    if (!group_paint_)
        return *this;
    for (const ui_node* n = parent(); n; n = n->parent())
        if (n->has_flag(node_flag::interactive))
            return *n;
    return *this;
}

unsigned lx::ui::control_node::paint_state() const {
    const ui_node& src = state_source();
    if (has_flag(node_flag::disabled) || src.has_flag(node_flag::disabled))
        return 0;
    if (src.has_flag(node_flag::pressed))
        return 2;
    if (src.has_flag(node_flag::hovered))
        return 1;
    return 0;
}

lx::color lx::ui::control_node::background() const { return backgrounds_[paint_state()]; }

void lx::ui::control_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    const bool fade = paint_.opacity < 255;
    if (fade)
        out.push_opacity(static_cast<float>(paint_.opacity) / 255.f);
    if (has_shadow_)
        out.shadow({bounds_.x, bounds_.y + paint_.shadow_y, bounds_.width, bounds_.height}, shadow_);
    const unsigned s = paint_state();
    if (has_quad_[s])
        out.quad(bounds_, quads_[s]);
    const bool clip = flex_.overflow_x != lx::layout::overflow::visible ||
                      flex_.overflow_y != lx::layout::overflow::visible;
    if (clip)
        out.push_clip(bounds_);
    ui_node::emit(out);
    if (clip)
        out.pop();
    if (ring_ && state_source().has_flag(node_flag::focus_ring))
        draw_focus_ring(out, bounds_, radii_);
    if (fade)
        out.pop();
}

bool lx::ui::control_node::on_focus_key(key_event event, focus_manager& focus) {
    if (!event.pressed)
        return false;
    if (has_flag(node_flag::interactive) && has_flag(node_flag::focused) &&
        (event.key == key::space || event.key == key::enter))
        return activate();
    if (nav_ == nav_axis::none)
        return false;
    const bool h = nav_ == nav_axis::horizontal || nav_ == nav_axis::both;
    const bool v = nav_ == nav_axis::vertical || nav_ == nav_axis::both;
    switch (event.key) {
    case key::left:
        return h && focus.move_within(*this, -1, nav_wrap_);
    case key::right:
        return h && focus.move_within(*this, 1, nav_wrap_);
    case key::up:
        return v && focus.move_within(*this, -1, nav_wrap_);
    case key::down:
        return v && focus.move_within(*this, 1, nav_wrap_);
    case key::home:
        return focus.move_to_edge(*this, false);
    case key::end:
        return focus.move_to_edge(*this, true);
    default:
        return false;
    }
}

bool lx::ui::control_node::activate() {
    if (!has_flag(node_flag::interactive) || has_flag(node_flag::disabled))
        return false;
    ++activations_;
    if (on_activate_)
        on_activate_(value_);
    return true;
}

lx::a11y::node lx::ui::control_node::accessibility_node() const {
    unsigned state = a11y_state_;
    if (has_flag(node_flag::focused))
        state |= static_cast<unsigned>(a11y::state_flag::focused);
    if (has_flag(node_flag::disabled))
        state |= static_cast<unsigned>(a11y::state_flag::disabled);
    return {role_, name_, "", "", state, const_cast<control_node*>(this)};
}

lx::ui::child lx::ui::control(const control_props& props, std::span<const child> children,
                              element_key key) {
    return with_children(describe<control_node, control_props>(props, key), children);
}

// ── image_node ──

lx::ui::element_type_id lx::ui::image_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::image_node::type() const { return static_type(); }
const char* lx::ui::image_node::type_name() const { return "image"; }

bool lx::ui::image_node::apply(const image_props& props) {
    if (props.atlas != props_.atlas || props.tile.value != props_.tile.value ||
        props.corner_radius != props_.corner_radius)
        invalidate_paint(*this);
    props_ = props;
    return false;
}

void lx::ui::image_node::emit(draw_scope& out) const {
    if (!visible_ || !props_.atlas)
        return;
    lx::gfx::atlas_tile tile{};
    if (!props_.atlas->use(props_.tile, tile))
        return;
    const float r = props_.corner_radius;
    out.sprite(lx::gfx::primitive_kind::poly_sprite, tile.page, bounds_, tile.uv(),
               lx::color::rgb(1.f, 1.f, 1.f, 1.f), {r, r, r, r});
}

lx::a11y::node lx::ui::image_node::accessibility_node() const {
    return {a11y::role::image, "", "", "", 0, const_cast<image_node*>(this)};
}

// ── resources ──

void lx::ui::set_ui_resources(const ui_resources& resources) { g_ui_resources = resources; }

const lx::ui::ui_resources& lx::ui::active_ui_resources() { return g_ui_resources; }

lx::ui::child lx::ui::icon_view(icon id, float size_px, tok color) {
    return icon_view(id, g_ui_resources.icons, size_px, color, 1.f);
}
