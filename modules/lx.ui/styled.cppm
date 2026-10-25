module;

export module lx.ui:styled;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;

import :element;
import :flex;
import :invalidate;
import :node;
import :style;
import :theme;
import :widgets_decl;

// ── Styled boxes (ADR-010 §4.8) ────────────────────────────────────────────────────────
//
// `styled_node` is the flex container of `lx.ui:flex` with a `style`: the style's layout goes
// to the flex engine (spacing steps resolved through the density), its paint to `emit` —
// the elevation's shadows, then one quad for the background, border and corner radii, then
// the children (clipped when the overflow is not visible), all under the style's opacity.
//
// Resolution happens in `apply`, against the active theme: tokens become colours, the quad
// is packed once (`pack_quad_style`), and `emit` copies it. A reconcile that re-applies the
// same style under the same theme generation costs one 112-byte comparison; a new theme or
// density (`theme_generation`) resolves again. A paint-only change damages the node's bounds
// and leaves the layout clean; a change the engine reads marks it dirty.
//
// `styled(style)` as a decorator turns the container it is piped onto — `flex(...)`,
// `box(...)` or another styled box — into a styled box over the same children and key (its
// own props are dropped: the style says the direction and gap); anything else is wrapped as
// the styled box's one child.

export namespace lx::ui {

struct styled_props {
    style value{};
};

/// A style's paint resolved against a theme: the packed quad, the corner radii in px (the
/// shadows' too), the elevation's shadows (black at their alpha, cast by the box moved by
/// their offset) and the opacity.
struct resolved_paint {
    packed_quad_style quad{};
    float radii[4]{};
    elevation_shadow shadows[4]{};
    std::uint8_t shadow_count = 0;
    bool has_quad = false;
    float opacity = 1.f;

    /// Shadow `i` as `draw_scope::shadow` takes it.
    [[nodiscard]] shadow_style shadow(unsigned i) const;
};

[[nodiscard]] resolved_paint resolve_paint(const style& s, const theme& t, float rem_px);

/// A style's text fields resolved against a theme; a field the style leaves to inheritance
/// says so (`has_*` false).
struct resolved_text {
    lx::color color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    float size_px = 0.f;
    float line_height = k_phi;
    std::uint16_t weight = 0;
    bool has_color = false;
    bool has_size = false;
    bool has_line_height = false;
};

[[nodiscard]] resolved_text resolve_text(const style& s, const theme& t, float rem_px);

class styled_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the resolved layout changed; a paint-only change damages the bounds.
    bool apply(const styled_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;

    [[nodiscard]] const lx::ui::style& value() const { return style_; }
    [[nodiscard]] const resolved_paint& paint() const { return paint_; }
    [[nodiscard]] const resolved_text& text() const { return text_; }
    [[nodiscard]] lx::size2i content_size() const { return content_size_; }

private:
    lx::ui::style style_{};
    std::uint64_t generation_ = 0;
    lx::layout::flex_style flex_{};
    resolved_paint paint_{};
    resolved_text text_{};
    lx::size2i content_size_{};
};

/// The text style a node inherits: each field from the nearest styled ancestor (itself
/// included) that sets it, else the theme's UI text (`text`, 14 px, φ, 400). Walks the
/// parent chain: call it where parents are set (measure, arrange, emit).
[[nodiscard]] resolved_text inherited_text(const ui_node& node);

/// A styled flex container over `children`.
[[nodiscard]] child div(const style& s, std::span<const child> children);
/// `content` styled: a flex, box or styled container becomes a styled box over the same
/// children; anything else becomes its one child.
[[nodiscard]] child styled(const style& s, child content);
[[nodiscard]] inline auto styled(const style& s) {
    return [s](child content) { return lx::ui::styled(s, content); };
}

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

lx::ui::resolved_paint lx::ui::resolve_paint(const style& s, const theme& t, float rem_px) {
    resolved_paint p{};
    quad_style q{};
    tok bg = s.bg_token;
    if (bg == tok::none)
        bg = elevation_background(s.elevation_level);
    if (bg != tok::none)
        q.background = t.color(bg);
    bool any_border = false;
    for (unsigned i = 0; i < 4; ++i) {
        q.border_widths[i] = static_cast<float>(s.border_widths[i]);
        any_border = any_border || s.border_widths[i] != 0;
        q.corner_radii[i] = radius_px(s.corners[i], rem_px);
    }
    tok border = s.border_token;
    if (!any_border && elevation_bordered(s.elevation_level)) {
        for (float& w : q.border_widths)
            w = 1.f;
        any_border = true;
        if (border == tok::none)
            border = tok::border_variant;
    }
    if (any_border)
        q.border_color = t.color(border == tok::none ? tok::border : border);
    p.has_quad = q.background.a > 0.f || (any_border && q.border_color.a > 0.f);
    if (p.has_quad)
        p.quad = pack_quad_style(q);

    for (unsigned i = 0; i < 4; ++i)
        p.radii[i] = q.corner_radii[i];
    for (const elevation_shadow& e : elevation_shadows(s.elevation_level, t.appearance_mode())) {
        if (p.shadow_count == 4)
            break;
        p.shadows[p.shadow_count++] = e;
    }
    p.opacity = static_cast<float>(s.opacity_255) / 255.f;
    return p;
}

lx::ui::shadow_style lx::ui::resolved_paint::shadow(unsigned i) const {
    shadow_style sh{};
    if (i >= shadow_count)
        return sh;
    sh.color = lx::color::rgb(0.f, 0.f, 0.f, shadows[i].alpha);
    for (unsigned k = 0; k < 4; ++k)
        sh.corner_radii[k] = radii[k];
    sh.blur_sigma = shadows[i].blur;
    return sh;
}

lx::ui::resolved_text lx::ui::resolve_text(const style& s, const theme& t, float rem_px) {
    resolved_text r{};
    if (s.text_token != tok::none) {
        r.color = t.color(s.text_token);
        r.has_color = true;
    }
    if (s.text_step != text_size::inherit) {
        r.size_px = text_size_px(s.text_step, t, rem_px);
        r.has_size = r.size_px > 0.f;
    }
    if (s.line_height_64 != 0) {
        r.line_height = static_cast<float>(s.line_height_64) / 64.f;
        r.has_line_height = true;
    }
    r.weight = static_cast<std::uint16_t>(s.weight_100 * 100u);
    return r;
}

lx::ui::resolved_text lx::ui::inherited_text(const ui_node& node) {
    resolved_text out{};
    bool color = false;
    bool size = false;
    bool line = false;
    bool weight = false;
    for (const ui_node* n = &node; n && !(color && size && line && weight); n = n->parent()) {
        if (n->type() != styled_node::static_type())
            continue;
        const resolved_text& t = static_cast<const styled_node*>(n)->text();
        if (!color && t.has_color) {
            out.color = t.color;
            color = true;
        }
        if (!size && t.has_size) {
            out.size_px = t.size_px;
            size = true;
        }
        if (!line && t.has_line_height) {
            out.line_height = t.line_height;
            line = true;
        }
        if (!weight && t.weight != 0) {
            out.weight = t.weight;
            weight = true;
        }
    }
    const theme& th = active_theme();
    if (!color)
        out.color = th.color(tok::text);
    if (!size)
        out.size_px = text_size_px(text_size::normal, th, ui_rem_px());
    if (!weight)
        out.weight = 400;
    out.has_color = out.has_size = out.has_line_height = true;
    return out;
}

lx::ui::element_type_id lx::ui::styled_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::styled_node::type() const {
    return static_type();
}
const char* lx::ui::styled_node::type_name() const {
    return "styled";
}

bool lx::ui::styled_node::apply(const styled_props& props) {
    const std::uint64_t generation = theme_generation();
    if (generation == generation_ && props.value == style_)
        return false;
    style_ = props.value;
    generation_ = generation;
    const theme& t = active_theme();
    const float rem = ui_rem_px();
    const lx::layout::flex_style flex = resolve_layout(style_, {ui_density(), rem});
    paint_ = resolve_paint(style_, t, rem);
    text_ = resolve_text(style_, t, rem);
    const bool layout_changed = !(flex == flex_);
    flex_ = flex;
    if (!layout_changed)
        invalidate_paint(*this);
    return layout_changed;
}

lx::size2i lx::ui::styled_node::measure(lx::layout::constraints c, size_hint hint) const {
    return flex_container_measure(*this, flex_, c, hint, false);
}

lx::size2i lx::ui::styled_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    return flex_container_measure(*this, flex_, c, hint, true);
}

void lx::ui::styled_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    content_size_ = flex_container_arrange(*this, flex_, bounds);
}

void lx::ui::styled_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    const bool fade = paint_.opacity < 1.f;
    if (fade)
        out.push_opacity(paint_.opacity);
    for (unsigned i = 0; i < paint_.shadow_count; ++i) {
        const elevation_shadow& e = paint_.shadows[i];
        out.shadow({bounds_.x + static_cast<int>(e.offset_x), bounds_.y + static_cast<int>(e.offset_y),
                    bounds_.width, bounds_.height},
                   paint_.shadow(i));
    }
    if (paint_.has_quad)
        out.quad(bounds_, paint_.quad);
    const bool clip = flex_.overflow_x != lx::layout::overflow::visible ||
                      flex_.overflow_y != lx::layout::overflow::visible;
    if (clip)
        out.push_clip(bounds_);
    ui_node::emit(out);
    if (clip)
        out.pop();
    if (fade)
        out.pop();
}

const lx::layout::flex_style* lx::ui::styled_node::flex_item_style() const {
    return &flex_;
}

lx::ui::child lx::ui::div(const style& s, std::span<const child> children) {
    return with_children(describe<styled_node, styled_props>({s}), children);
}

lx::ui::child lx::ui::styled(const style& s, child content) {
    if (!content)
        return nullptr;
    const element_type_id t = content->type;
    if (t == flex_node::static_type() || t == styled_node::static_type() ||
        t == box_node::static_type()) {
        // A container: the styled box takes its place over the same children.
        const child c = describe<styled_node, styled_props>({s}, content->key);
        return with_children(c, {content->children, content->child_count});
    }
    const child one[1] = {content};
    return with_children(describe<styled_node, styled_props>({s}), one);
}
