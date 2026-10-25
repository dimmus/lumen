module;

import lx.foundation;
import lx.layout;
import lx.scene;

export module lx.ui:flex;

import std;

import :element;
import :node;
import :theme;

// ── Flex containers on ui_node (ADR-010 §4.7) ────────────────────────────────────────
//
// `flex_node` lays its children out with lx.layout:flex through ui_node's two-pass contract:
// `measure` solves for a size and keeps nothing, `arrange` solves at the bounds and places
// every child. Children answer the engine's measurements through their measure caches
// (`ui_node::measure_cached`), and supply their item style through `flex_item_style()` —
// a `grow` decorator's grow factor, a `sizing` decorator's min and max, a nested flex
// node's own style. A child whose style sets a width, height, min or max is measured for
// its content (`sizing_mode::content`): the engine applies those itself.
//
// Percentages: the engine resolves an item's against this container's content box, and the
// container's own (padding, border, and size, min and max when it is not a flex item)
// against `current_percent_base` — what its parent set (`percent_base_scope`): the engine's
// `child_query::parent_*` while the parent measures it, the parent's arranged content box
// while the parent places it.
//
// Sizes are whole pixels at the ui_node boundary: a known size is rounded before it is
// passed down as a constraint, and each child's rect is rounded edge by edge (Taffy's
// rule) so neighbours neither gap nor overlap. A constraint's max at
// `layout::constraints`' 99 999 means unbounded. The engine's per-item and per-line state
// lives on the stack, `ui_node::k_max_children` of each (≈10 KB a container on the stack
// while it solves; nested containers solve one at a time down the measure recursion), so
// a solve allocates nothing.

export namespace lx::ui {

/// The default rem a flex style's `rem` lengths resolve against, in the node's pixels. A
/// solve takes the active theme's (`ui_rem_px`: its UI font size, 16 px in the bundled ones).
inline constexpr float k_layout_rem_px = 16.f;

struct flex_props {
    lx::layout::flex_style style{};
    /// Painted over the border box; transparent: none.
    lx::color fill = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
};

/// A box laid out as a CSS flex container, and laid out as a flex item by its parent.
/// `overflow: hidden | scroll` clips the children to the bounds; scrolling is the
/// owner's (`content_size` minus the padding box is the range).
class flex_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the style changed (the fill alone does not touch layout).
    bool apply(const flex_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;

    /// The children's extent plus the end padding, from the padding box's origin, as of the
    /// last arrange (`layout::flex_output`).
    [[nodiscard]] lx::size2i content_size() const;

private:
    flex_props props_{};
    lx::size2i content_size_{};
};

/// A flex container over `children`.
[[nodiscard]] child flex(const lx::layout::flex_style& style, std::span<const child> children,
                         lx::color fill = lx::color::rgb(0.f, 0.f, 0.f, 0.f));

} // namespace lx::ui

namespace lx::ui {

/// `measure` of a flex container with `style` over `self`'s children; `content` ignores the
/// container's own size styles.
[[nodiscard]] lx::size2i flex_container_measure(const ui_node& self,
                                                const lx::layout::flex_style& style,
                                                lx::layout::constraints c, size_hint hint,
                                                bool content);
/// `arrange`: places every child; returns the content size.
lx::size2i flex_container_arrange(ui_node& self, const lx::layout::flex_style& style,
                                  lx::rect2i bounds);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────

namespace {

constexpr int k_unbounded = lx::layout::constraints{}.max.width; // 99 999

[[nodiscard]] int lx_ui_round(float v) {
    return static_cast<int>(std::lround(v));
}

/// Sets a width, height, min or max: such an item is measured for its content.
[[nodiscard]] bool lx_ui_sizes_itself(const lx::layout::flex_style* s) {
    return s && (!s->width.is_auto() || !s->height.is_auto() || !s->min_width.is_auto() ||
                 !s->min_height.is_auto() || !s->max_width.is_auto() || !s->max_height.is_auto() ||
                 s->aspect_ratio > 0.f);
}

const lx::layout::flex_style* lx_ui_child_style(void* ctx, unsigned i) {
    const lx::ui::ui_node* child = static_cast<const lx::ui::ui_node*>(ctx)->child_at(i);
    return child ? child->flex_item_style() : nullptr;
}

lx::layout::flex_size lx_ui_child_measure(void* ctx, unsigned i, const lx::layout::child_query& q) {
    const lx::ui::ui_node* child = static_cast<const lx::ui::ui_node*>(ctx)->child_at(i);
    if (!child)
        return {q.known_width == q.known_width ? q.known_width : 0.f,
                q.known_height == q.known_height ? q.known_height : 0.f};
    const auto axis = [](float known, float available, int& lo, int& hi) {
        if (known == known) {
            lo = hi = std::max(lx_ui_round(known), 0);
        } else {
            lo = 0;
            hi = available < static_cast<float>(k_unbounded) ? std::max(lx_ui_round(available), 0)
                                                             : k_unbounded;
        }
    };
    lx::layout::constraints c{};
    axis(q.known_width, q.available_width, c.min.width, c.max.width);
    axis(q.known_height, q.available_height, c.min.height, c.max.height);
    const lx::ui::size_hint hint =
        q.min_content_width ? lx::ui::size_hint::min : lx::ui::size_hint::preferred;
    const lx::ui::sizing_mode mode = q.content_size && lx_ui_sizes_itself(child->flex_item_style())
                                         ? lx::ui::sizing_mode::content
                                         : lx::ui::sizing_mode::inherent;
    // The child's own percentages resolve against this container's content box.
    const lx::ui::percent_base_scope base{q.parent_width, q.parent_height};
    const lx::size2i s = child->measure_cached(c, hint, mode);
    return {static_cast<float>(s.width), static_cast<float>(s.height)};
}

/// One solve over `self`'s children with the scratch on the stack.
struct lx_ui_flex_solve {
    lx::rect2f rects[lx::ui::ui_node::k_max_children];
    lx::layout::flex_item_state items[lx::ui::ui_node::k_max_children];
    lx::layout::flex_line_state lines[lx::ui::ui_node::k_max_children];

    [[nodiscard]] lx::layout::flex_output run(const lx::ui::ui_node& self,
                                              const lx::layout::flex_style& style,
                                              const lx::layout::flex_input& in) {
        const unsigned n = std::min(self.child_count(), lx::ui::ui_node::k_max_children);
        lx::layout::flex_children ch{const_cast<lx::ui::ui_node*>(&self), n, lx_ui_child_style,
                                     lx_ui_child_measure};
        // The spans are the child cap long: the engine cannot refuse them.
        const lx::result<lx::layout::flex_output> r =
            lx::layout::solve_flex(style, in, ch, rects, {items, lines});
        return r.ok() ? *r : lx::layout::flex_output{};
    }
};

} // namespace

lx::size2i lx::ui::flex_container_measure(const ui_node& self, const lx::layout::flex_style& style,
                                          lx::layout::constraints c, size_hint hint, bool content) {
    lx::layout::flex_input in{};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    in.known_width = c.min.width == c.max.width ? static_cast<float>(c.max.width) : nan;
    in.known_height = c.min.height == c.max.height ? static_cast<float>(c.max.height) : nan;
    in.available_width = c.max.width >= k_unbounded ? inf : static_cast<float>(c.max.width);
    in.available_height = c.max.height >= k_unbounded ? inf : static_cast<float>(c.max.height);
    in.min_content_width = hint == size_hint::min;
    in.content_size = content;
    in.rem_px = ui_rem_px();
    const percent_base base = current_percent_base();
    in.parent_width = base.width;
    in.parent_height = base.height;
    if (style_reads_percent_base(style, content))
        note_percent_base_read();
    lx_ui_flex_solve solve;
    const lx::layout::flex_output o = solve.run(self, style, in);
    lx::size2i out{lx_ui_round(o.width), lx_ui_round(o.height)};
    out.width = std::clamp(out.width, c.min.width, std::max(c.min.width, c.max.width));
    out.height = std::clamp(out.height, c.min.height, std::max(c.min.height, c.max.height));
    return out;
}

lx::size2i lx::ui::flex_container_arrange(ui_node& self, const lx::layout::flex_style& style,
                                          lx::rect2i bounds) {
    lx::layout::flex_input in{};
    in.known_width = static_cast<float>(bounds.width);
    in.known_height = static_cast<float>(bounds.height);
    in.available_width = in.known_width;
    in.available_height = in.known_height;
    in.rem_px = ui_rem_px();
    const percent_base base = current_percent_base();
    in.parent_width = base.width;
    in.parent_height = base.height;
    lx_ui_flex_solve solve;
    const lx::layout::flex_output o = solve.run(self, style, in);
    // The children's percent base while they arrange: this container's content box.
    const percent_base_scope inner{o.inner_width, o.inner_height};
    const unsigned n = std::min(self.child_count(), ui_node::k_max_children);
    for (unsigned i = 0; i < n; ++i) {
        ui_node* child = self.child_at(i);
        if (!child)
            continue;
        const lx::rect2f& r = solve.rects[i];
        const int x0 = lx_ui_round(r.x);
        const int y0 = lx_ui_round(r.y);
        child->place({bounds.x + x0, bounds.y + y0, lx_ui_round(r.x + r.width) - x0,
                      lx_ui_round(r.y + r.height) - y0});
    }
    return {lx_ui_round(o.content_width), lx_ui_round(o.content_height)};
}

// ── flex_node ───────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::flex_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::flex_node::type() const {
    return static_type();
}
const char* lx::ui::flex_node::type_name() const {
    return "flex";
}

bool lx::ui::flex_node::apply(const flex_props& props) {
    const bool layout_changed = !(props.style == props_.style);
    const lx::color& a = props.fill;
    const lx::color& b = props_.fill;
    const bool repaint = a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
    props_ = props;
    // A new fill alone repaints the box; a layout change is damaged by the reconciler.
    if (repaint && !layout_changed)
        detail::damage_node(*this, ink_bounds(*this));
    return layout_changed;
}

lx::size2i lx::ui::flex_node::measure(lx::layout::constraints c, size_hint hint) const {
    return flex_container_measure(*this, props_.style, c, hint, false);
}

lx::size2i lx::ui::flex_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    return flex_container_measure(*this, props_.style, c, hint, true);
}

void lx::ui::flex_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    content_size_ = flex_container_arrange(*this, props_.style, bounds);
}

void lx::ui::flex_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    if (props_.fill.a > 0.f)
        out.fill(bounds_, props_.fill);
    const bool clip = props_.style.overflow_x != lx::layout::overflow::visible ||
                      props_.style.overflow_y != lx::layout::overflow::visible;
    if (clip)
        out.push_clip_local(bounds_);
    ui_node::emit(out);
    if (clip)
        out.pop();
}

const lx::layout::flex_style* lx::ui::flex_node::flex_item_style() const {
    return &props_.style;
}

lx::size2i lx::ui::flex_node::content_size() const {
    return content_size_;
}

lx::ui::child lx::ui::flex(const lx::layout::flex_style& style, std::span<const child> children,
                           lx::color fill) {
    return with_children(describe<flex_node, flex_props>({style, fill}), children);
}
