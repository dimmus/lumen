module;

import lx.foundation;
import lx.layout;
import lx.scene;

export module lx.ui:grid;

import std;

import :element;
import :node;
import :theme;

// ── Grid containers on ui_node (ADR-010 §4.7) ─────────────────────────────────────────
//
// `grid_node` lays its children out with lx.layout:grid through the same two-pass contract
// as `flex_node`: `measure` solves for a size and keeps nothing, `arrange` solves at the
// bounds and places every child. Placement arrives per child through the child's
// `grid_placement()` (null: auto-placed, row-major, sparse) — the `placed` decorator
// supplies one; items stretch across their whole cell.
//
// Whole pixels at the ui_node boundary, scratch on the stack, nothing allocated: as
// `flex_node` (see its header for the percent-base and rounding rules).

export namespace lx::ui {

struct grid_props {
    lx::layout::grid_style style{};
    /// Painted over the border box; transparent: none.
    lx::color fill = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
};

/// A box laid out as a CSS grid container, and laid out as a flex item by its parent.
class grid_node : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the style changed (the fill alone does not touch layout).
    bool apply(const grid_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;

    /// The children's extent plus the end padding, from the padding box's origin, as of
    /// the last arrange.
    [[nodiscard]] lx::size2i content_size() const;

private:
    grid_props props_{};
    lx::size2i content_size_{};
};

/// A grid container over `children`.
[[nodiscard]] child grid(const lx::layout::grid_style& style, std::span<const child> children,
                         lx::color fill = lx::color::rgb(0.f, 0.f, 0.f, 0.f));

/// Places the wrapped child at an explicit cell (1-based; 0 or the default: auto on that
/// axis). The grid container reads it through the child's `grid_placement()`.
struct placed_props {
    lx::layout::grid_placement placement{};
};

class placed_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    bool apply(const placed_props& props);
    [[nodiscard]] const lx::layout::grid_placement* grid_placement() const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;

private:
    placed_props props_{};
};

/// Wraps `content` in an explicit grid placement.
[[nodiscard]] child placed(lx::layout::grid_placement placement, child content);

} // namespace lx::ui

namespace lx::ui {

/// `measure` / `arrange` of a grid container with `style` over `self`'s children —
/// `:flex`'s helpers, grid-shaped.
[[nodiscard]] lx::size2i grid_container_measure(const ui_node& self,
                                                const lx::layout::grid_style& style,
                                                lx::layout::constraints c, size_hint hint,
                                                bool content);
lx::size2i grid_container_arrange(ui_node& self, const lx::layout::grid_style& style,
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

const lx::layout::grid_placement* lx_ui_child_placement(void* ctx, unsigned i) {
    const lx::ui::ui_node* child = static_cast<const lx::ui::ui_node*>(ctx)->child_at(i);
    return child ? child->grid_placement() : nullptr;
}

lx::layout::flex_size lx_ui_child_measure(void* ctx, unsigned i, const lx::layout::child_query& q) {
    const lx::ui::ui_node* child = static_cast<const lx::ui::ui_node*>(ctx)->child_at(i);
    if (!child)
        return {q.known_width == q.known_width ? q.known_width : 0.f,
                q.known_height == q.known_height ? q.known_height : 0.f};
    lx::layout::constraints c{};
    const auto axis = [](float known, float available, int& lo, int& hi) {
        if (known == known) {
            lo = hi = std::max(lx_ui_round(known), 0);
        } else {
            lo = 0;
            hi = available < static_cast<float>(k_unbounded) ? std::max(lx_ui_round(available), 0)
                                                             : k_unbounded;
        }
    };
    axis(q.known_width, q.available_width, c.min.width, c.max.width);
    axis(q.known_height, q.available_height, c.min.height, c.max.height);
    const lx::ui::size_hint hint =
        q.min_content_width ? lx::ui::size_hint::min : lx::ui::size_hint::preferred;
    const lx::ui::sizing_mode mode =
        q.content_size && lx_ui_sizes_itself(child->flex_item_style())
            ? lx::ui::sizing_mode::content
            : lx::ui::sizing_mode::inherent;
    const lx::ui::percent_base_scope base{q.parent_width, q.parent_height};
    const lx::size2i s = child->measure_cached(c, hint, mode);
    return {static_cast<float>(s.width), static_cast<float>(s.height)};
}

/// One solve over `self`'s children with the scratch on the stack.
struct lx_ui_grid_solve {
    lx::rect2f rects[lx::ui::ui_node::k_max_children];
    lx::layout::grid_item_state items[lx::ui::ui_node::k_max_children];
    lx::layout::grid_row_state rows[lx::ui::ui_node::k_max_children];

    [[nodiscard]] lx::layout::grid_output run(const lx::ui::ui_node& self,
                                              const lx::layout::grid_style& style,
                                              const lx::layout::grid_input& in) {
        const unsigned n = std::min(self.child_count(), lx::ui::ui_node::k_max_children);
        lx::layout::grid_children ch{const_cast<lx::ui::ui_node*>(&self), n,
                                     lx_ui_child_placement, lx_ui_child_measure};
        const lx::result<lx::layout::grid_output> r =
            lx::layout::solve_grid(style, in, ch, rects, {items, rects, rows});
        return r.ok() ? *r : lx::layout::grid_output{};
    }
};

} // namespace

lx::size2i lx::ui::grid_container_measure(const ui_node& self,
                                          const lx::layout::grid_style& style,
                                          lx::layout::constraints c, size_hint hint,
                                          bool content) {
    lx::layout::grid_input in{};
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
    lx_ui_grid_solve solve;
    const lx::layout::grid_output o = solve.run(self, style, in);
    lx::size2i out{lx_ui_round(o.width), lx_ui_round(o.height)};
    out.width = std::clamp(out.width, c.min.width, std::max(c.min.width, c.max.width));
    out.height = std::clamp(out.height, c.min.height, std::max(c.min.height, c.max.height));
    return out;
}

lx::size2i lx::ui::grid_container_arrange(ui_node& self, const lx::layout::grid_style& style,
                                          lx::rect2i bounds) {
    lx::layout::grid_input in{};
    in.known_width = static_cast<float>(bounds.width);
    in.known_height = static_cast<float>(bounds.height);
    in.available_width = in.known_width;
    in.available_height = in.known_height;
    in.rem_px = ui_rem_px();
    const percent_base base = current_percent_base();
    in.parent_width = base.width;
    in.parent_height = base.height;
    lx_ui_grid_solve solve;
    const lx::layout::grid_output o = solve.run(self, style, in);
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

lx::ui::element_type_id lx::ui::grid_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::grid_node::type() const { return static_type(); }
const char* lx::ui::grid_node::type_name() const { return "grid"; }

bool lx::ui::grid_node::apply(const grid_props& props) {
    const bool layout_changed = !(props.style == props_.style);
    const lx::color& a = props.fill;
    const lx::color& b = props_.fill;
    const bool repaint = a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
    props_ = props;
    if (repaint && !layout_changed)
        detail::damage_node(*this, ink_bounds(*this));
    return layout_changed;
}

lx::size2i lx::ui::grid_node::measure(lx::layout::constraints c, size_hint hint) const {
    return grid_container_measure(*this, props_.style, c, hint, false);
}

lx::size2i lx::ui::grid_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    return grid_container_measure(*this, props_.style, c, hint, true);
}

void lx::ui::grid_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    content_size_ = grid_container_arrange(*this, props_.style, bounds);
}

void lx::ui::grid_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    if (props_.fill.a > 0.f)
        out.fill(bounds_, props_.fill);
    ui_node::emit(out);
}

const lx::layout::flex_style* lx::ui::grid_node::flex_item_style() const {
    return nullptr; // a grid is not a flex container: no item style to give
}

lx::size2i lx::ui::grid_node::content_size() const { return content_size_; }

lx::ui::child lx::ui::grid(const lx::layout::grid_style& style, std::span<const child> children,
                           lx::color fill) {
    return with_children(describe<grid_node, grid_props>({style, fill}), children);
}

// ── placed ──────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::placed_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::placed_node::type() const { return static_type(); }
const char* lx::ui::placed_node::type_name() const { return "placed"; }

bool lx::ui::placed_node::apply(const placed_props& props) {
    const bool changed = !(props.placement == props_.placement);
    props_ = props;
    return changed;
}

const lx::layout::grid_placement* lx::ui::placed_node::grid_placement() const {
    return &props_.placement;
}

lx::size2i lx::ui::placed_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    const ui_node* content = child_at(0);
    return content ? content->measure_cached(c, hint, lx::ui::sizing_mode::content)
                   : lx::size2i{};
}

lx::ui::child lx::ui::placed(lx::layout::grid_placement placement, child content) {
    return with_children(describe<placed_node, placed_props>({placement}), {&content, 1});
}
