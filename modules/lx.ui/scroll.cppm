module;

export module lx.ui:scroll;

import std;
import lx.foundation;
import lx.layout;
import lx.a11y;
import lx.runtime;

import :element;
import :events;
import :flex;
import :interact;
import :invalidate;
import :node;
import :style;
import :styled;
import :theme;

// ── Scroll areas and Zed's scrollbar (ADR-010 §5.6) ──────────────────────────────────────
//
// A `scroll_node` is a viewport over one content child: the content is laid out once at its
// full size along the scrolled axes (measured unbounded there) at the viewport's origin, and
// drawn shifted by the scroll offset under a clip to the viewport (`push_clip_local`,
// `push_origin`). Scrolling changes the offset and nothing else: no measure, no arrange; the
// damage is the viewport (as the surface shows it). The node is a `child_viewport` for its
// content, so hit testing, damage and `surface_rect` of anything inside see where it is drawn.
//
// The scrollbar (`scrollbar_node`, one per axis, children after the content) is Zed's
// `ScrollbarStyle::Regular` from crates/ui/src/components/scrollbar.rs (zed a5c4ca8): a track
// strip `width + 2 × SCROLLBAR_PADDING` = 6 + 2 × 4 = 14 px along the viewport's end edge,
// the thumb inside it inset by the 4 px padding (6 px thick), its length the viewport's share
// of the content but at least `MINIMUM_THUMB_SIZE` = 25 px, fully rounded, placed by
// `thumb_ranges` (start = offset / max offset × (viewport − thumb), as a fraction of the
// viewport, times the inset track). A press on the thumb drags it (the grab point kept); a
// press on the track centres the thumb there and drags on (`ScrollbarMouseEvent::TrackClick`,
// `thumb_size / 2`). Paint is Zed's (scrollbar.rs, the regular style with no track): no track
// strip is drawn; the thumb is borderless, its colour `surface.background` — its alpha capped
// at 0.7 unless the bar is hovered (`MAXIMUM_OPACITY`) — blended with the state's
// `scrollbar.thumb.background` (hovered `.hover_background`, dragged `.active_background`) by
// GPUI's `Hsla::blend`. Departure from Zed: the bar shows whenever the content overflows
// (Zed's `ShowScrollbar::Always`; its autohide fade is not done).

export namespace lx::ui {

enum class scroll_axes : std::uint8_t { vertical, horizontal, both };

/// Zed's `ScrollbarStyle::Regular` (scrollbar.rs): the thumb's thickness, the padding around
/// it, the minimum thumb length.
inline constexpr int k_scrollbar_thumb_px = 6;
inline constexpr int k_scrollbar_padding_px = 4;
inline constexpr int k_scrollbar_track_px = k_scrollbar_thumb_px + 2 * k_scrollbar_padding_px;
inline constexpr int k_scrollbar_min_thumb_px = 25;

/// Where a scrollbar draws, in its viewport's space.
struct scrollbar_geometry {
    lx::rect2i strip{}; ///< the whole track strip (14 px across)
    lx::rect2i track{}; ///< the strip inset by the padding: where the thumb moves
    lx::rect2i thumb{};
    bool visible = false; ///< the content overflows along the axis
};

/// The bar along `axis` (0 horizontal, 1 vertical) of `viewport` for `m` (Zed's
/// `thumb_ranges` + prepaint). `both` leaves the corner to neither strip's track.
[[nodiscard]] scrollbar_geometry scrollbar_layout(lx::rect2i viewport, unsigned axis,
                                                  scroll_metrics m, bool both = false);
/// The offset a press at `along` (px on the axis, viewport space) asks for, the thumb held
/// `grab` px from its start (Zed's `compute_click_offset`).
[[nodiscard]] float scrollbar_offset_for(const scrollbar_geometry& g, unsigned axis,
                                         scroll_metrics m, int along, int grab);

struct scrollbar_props {
    std::uint8_t axis = 1;
    constexpr bool operator==(const scrollbar_props&) const = default;
};

/// One axis's scrollbar: a child of a scroll container (`ui_node::as_scroll_target`), placed
/// by it over the viewport's end strip. Interactive while the content overflows.
class scrollbar_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    bool apply(const scrollbar_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    bool on_pointer(const pointer_event& event) override;
    [[nodiscard]] a11y::node accessibility_node() const override;

    /// Sets visibility and interactivity from the container's metrics (its arrange calls it).
    void update(lx::rect2i viewport, bool both);
    [[nodiscard]] scrollbar_geometry geometry() const;
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] unsigned axis() const { return axis_; }

private:
    [[nodiscard]] scroll_target* target() const;
    lx::rect2i viewport_{};
    bool both_ = false;
    std::uint8_t axis_ = 1;
    bool dragging_ = false;
    int grab_ = 0;
};

struct scroll_props {
    /// The viewport's layout as a flex item (its paint fields are not read).
    lx::ui::style layout{};
    scroll_axes axes = scroll_axes::vertical;
    const char* name = nullptr;
};

class scroll_node final : public ui_node, public scroll_target {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    scroll_node();
    ~scroll_node() override;

    bool apply(const scroll_props& props);
    /// The content's size, unbounded along the scrolled axes, clamped to `c`.
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    [[nodiscard]] bool child_viewport(const ui_node& child, lx::point2i& offset,
                                      lx::rect2i& clip) const override;
    [[nodiscard]] scroll_target* as_scroll_target() override { return this; }
    /// Wheel: scrolls by the event's delta along the scrolled axes; false when it could not
    /// move (the ancestors may).
    bool on_pointer(const pointer_event& event) override;
    [[nodiscard]] a11y::node accessibility_node() const override;

    [[nodiscard]] scroll_metrics scroll_metrics_along(unsigned axis) const override;
    void scroll_to(unsigned axis, float offset) override;
    /// Scrolls by (dx, dy); true when the offset moved.
    bool scroll_by(float dx, float dy);
    /// Scrolls the least that shows `r` (content coordinates, the content's layout space).
    void scroll_into_view(lx::rect2i r);
    [[nodiscard]] lx::point2i offset() const;
    [[nodiscard]] lx::size2i content_size() const { return content_; }

private:
    [[nodiscard]] bool scrolls(unsigned axis) const;
    [[nodiscard]] lx::layout::constraints content_constraints(int width, int height) const;
    void clamp_offsets();

    lx::ui::style layout_{};
    lx::layout::flex_style flex_{};
    scroll_axes axes_ = scroll_axes::vertical;
    float off_[2]{};
    lx::size2i content_{};
    std::uint64_t generation_ = 0;
    bool applied_ = false;
    char name_[32]{};
};

/// A viewport over `children` (laid out in a flex container of `content`'s style) scrolled
/// along `axes`, with a scrollbar per axis.
[[nodiscard]] child scroll_area(const style& viewport, std::span<const child> children,
                                scroll_axes axes = scroll_axes::vertical,
                                const style& content = style{}.flex_col(), element_key key = 0);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

constexpr int k_scroll_unbounded = lx::layout::constraints{}.max.width; // 99 999

[[nodiscard]] int lx_ui_along(lx::rect2i r, unsigned axis) { return axis ? r.y : r.x; }
[[nodiscard]] int lx_ui_len(lx::rect2i r, unsigned axis) { return axis ? r.height : r.width; }

} // namespace

lx::ui::scrollbar_geometry lx::ui::scrollbar_layout(lx::rect2i v, unsigned axis, scroll_metrics m,
                                                    bool both) {
    scrollbar_geometry g{};
    const int t = k_scrollbar_track_px;
    // Two bars: each strip stops short of the corner (Zed's `additional_padding`).
    const int corner = both ? t : 0;
    if (axis) {
        g.strip = {v.x + v.width - t, v.y, t, std::max(v.height - corner, 0)};
    } else {
        g.strip = {v.x, v.y + v.height - t, std::max(v.width - corner, 0), t};
    }
    const int pad = k_scrollbar_padding_px;
    g.track = {g.strip.x + pad, g.strip.y + pad, std::max(g.strip.width - 2 * pad, 0),
               std::max(g.strip.height - 2 * pad, 0)};
    const float max_offset = m.max_offset();
    if (max_offset <= 0.f || m.viewport <= 0.f)
        return g;
    // scrollbar.rs `thumb_ranges`: a fraction of the viewport, mapped onto the inset track.
    const float visible = m.viewport / m.content;
    const float thumb = std::max(static_cast<float>(k_scrollbar_min_thumb_px), m.viewport * visible);
    if (thumb > m.viewport)
        return g;
    const float offset = std::clamp(m.offset, 0.f, max_offset);
    const float start = offset / max_offset * (m.viewport - thumb);
    const float f0 = start / m.viewport;
    const float f1 = (start + thumb) / m.viewport;
    const int len = lx_ui_len(g.track, axis);
    const int a = static_cast<int>(std::lround(f0 * static_cast<float>(len)));
    const int b = static_cast<int>(std::lround(f1 * static_cast<float>(len)));
    if (axis)
        g.thumb = {g.track.x, g.track.y + a, g.track.width, b - a};
    else
        g.thumb = {g.track.x + a, g.track.y, b - a, g.track.height};
    g.visible = true;
    return g;
}

float lx::ui::scrollbar_offset_for(const scrollbar_geometry& g, unsigned axis, scroll_metrics m,
                                   int along, int grab) {
    const int track = lx_ui_len(g.track, axis);
    const int thumb = lx_ui_len(g.thumb, axis);
    if (track <= thumb)
        return 0.f;
    const int start = std::clamp(along - lx_ui_along(g.track, axis) - grab, 0, track - thumb);
    return static_cast<float>(start) / static_cast<float>(track - thumb) * m.max_offset();
}

// ── scrollbar_node ──

lx::ui::element_type_id lx::ui::scrollbar_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::scrollbar_node::type() const { return static_type(); }
const char* lx::ui::scrollbar_node::type_name() const { return "scrollbar"; }

bool lx::ui::scrollbar_node::apply(const scrollbar_props& props) {
    const bool changed = props.axis != axis_;
    axis_ = props.axis ? 1 : 0;
    return changed;
}

lx::size2i lx::ui::scrollbar_node::measure(lx::layout::constraints c, size_hint) const {
    return c.min;
}

void lx::ui::scrollbar_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
}

const lx::layout::flex_style* lx::ui::scrollbar_node::flex_item_style() const {
    static const lx::layout::flex_style k_absolute = [] {
        lx::layout::flex_style s{};
        s.position = lx::layout::positioning::absolute;
        s.width = s.height = lx::layout::px(0.f);
        return s;
    }();
    return &k_absolute;
}

lx::ui::scroll_target* lx::ui::scrollbar_node::target() const {
    ui_node* p = parent();
    return p ? p->as_scroll_target() : nullptr;
}

void lx::ui::scrollbar_node::update(lx::rect2i viewport, bool both) {
    viewport_ = viewport;
    both_ = both;
    const scrollbar_geometry g = geometry();
    set_visible(g.visible);
    set_flag(node_flag::interactive, g.visible);
    if (!g.visible && dragging_)
        dragging_ = false;
}

lx::ui::scrollbar_geometry lx::ui::scrollbar_node::geometry() const {
    const scroll_target* t = target();
    if (!t)
        return {};
    return scrollbar_layout(viewport_, axis_, t->scroll_metrics_along(axis_), both_);
}

void lx::ui::scrollbar_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    const scrollbar_geometry g = geometry();
    if (!g.visible)
        return;
    const theme& th = active_theme();
    // The thumb, rounded fully and borderless: scrollbar.rs blends the state's thumb colour
    // over `surface.background` (its alpha capped at `MAXIMUM_OPACITY` = 0.7 unless hovered;
    // a dragged thumb is not "hovered" there) with `Hsla::blend`: an opaque thumb colour is
    // itself, a transparent one the surface, else the RGB mixed by the thumb's alpha under the
    // surface's alpha. No track: the regular style paints one only when it is given colours.
    const bool hovered = has_flag(node_flag::hovered) && !dragging_;
    const tok bg = dragging_  ? tok::scrollbar_thumb_active_background
                   : hovered  ? tok::scrollbar_thumb_hover_background
                              : tok::scrollbar_thumb_background;
    lx::color under = th.color(tok::surface_background);
    if (!hovered)
        under.a = std::min(under.a, 0.7f);
    const lx::color over = th.color(bg);
    lx::color c = under;
    if (over.a >= 1.f) {
        c = over;
    } else if (over.a > 0.f) {
        c.r = under.r * (1.f - over.a) + over.r * over.a;
        c.g = under.g * (1.f - over.a) + over.g * over.a;
        c.b = under.b * (1.f - over.a) + over.b * over.a;
    }
    quad_style thumb{};
    thumb.background = c;
    const float r = static_cast<float>(std::min(g.thumb.width, g.thumb.height)) * 0.5f;
    for (unsigned i = 0; i < 4; ++i)
        thumb.corner_radii[i] = r;
    out.quad(g.thumb, thumb);
}

bool lx::ui::scrollbar_node::on_pointer(const pointer_event& e) {
    scroll_target* t = target();
    if (!t)
        return false;
    const lx::point2i p = local_point(*this, e.at);
    const int along = axis_ ? p.y : p.x;
    switch (e.kind) {
    case pointer_kind::press: {
        const scrollbar_geometry g = geometry();
        if (!g.visible)
            return false;
        const int thumb_len = lx_ui_len(g.thumb, axis_);
        if (g.thumb.contains(p)) {
            grab_ = along - lx_ui_along(g.thumb, axis_);
        } else {
            // A press on the track: the thumb's centre goes there.
            grab_ = thumb_len / 2;
            t->scroll_to(axis_, scrollbar_offset_for(g, axis_, t->scroll_metrics_along(axis_),
                                                     along, grab_));
        }
        dragging_ = true;
        invalidate_paint(*this);
        return true;
    }
    case pointer_kind::drag: {
        if (!dragging_)
            return false;
        const scrollbar_geometry g = geometry();
        t->scroll_to(axis_, scrollbar_offset_for(g, axis_, t->scroll_metrics_along(axis_), along,
                                                 grab_));
        return true;
    }
    case pointer_kind::release:
        if (dragging_) {
            dragging_ = false;
            invalidate_paint(*this);
        }
        return true;
    default:
        return false;
    }
}

lx::a11y::node lx::ui::scrollbar_node::accessibility_node() const {
    return {a11y::role::scrollbar, axis_ ? "vertical" : "horizontal", "", "", 0,
            const_cast<scrollbar_node*>(this)};
}

// ── scroll_node ──

lx::ui::element_type_id lx::ui::scroll_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::scroll_node::type() const { return static_type(); }
const char* lx::ui::scroll_node::type_name() const { return "scroll"; }

lx::ui::scroll_node::scroll_node() { detail::viewport_created(); }
lx::ui::scroll_node::~scroll_node() { detail::viewport_destroyed(); }

bool lx::ui::scroll_node::apply(const scroll_props& p) {
    const std::uint64_t generation = theme_generation();
    bool changed = !applied_ || generation != generation_ || !(p.layout == layout_) ||
                   p.axes != axes_;
    if (changed) {
        lx::layout::flex_style flex = resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        // A scroll container: its automatic minimum size is 0 along the scrolled axes.
        if (p.axes != scroll_axes::horizontal)
            flex.overflow_y = lx::layout::overflow::scroll;
        if (p.axes != scroll_axes::vertical)
            flex.overflow_x = lx::layout::overflow::scroll;
        changed = !applied_ || !(flex == flex_) || p.axes != axes_;
        flex_ = flex;
        layout_ = p.layout;
        axes_ = p.axes;
        generation_ = generation;
    }
    std::size_t n = p.name ? std::strlen(p.name) : 0;
    n = std::min(n, sizeof(name_) - 1);
    if (n)
        std::memcpy(name_, p.name, n);
    name_[n] = '\0';
    applied_ = true;
    return changed;
}

bool lx::ui::scroll_node::scrolls(unsigned axis) const {
    return axis ? axes_ != scroll_axes::horizontal : axes_ != scroll_axes::vertical;
}

lx::layout::constraints lx::ui::scroll_node::content_constraints(int width, int height) const {
    lx::layout::constraints c{};
    if (scrolls(0)) {
        c.min.width = 0;
        c.max.width = k_scroll_unbounded;
    } else {
        c.min.width = width;
        c.max.width = width;
    }
    if (scrolls(1)) {
        c.min.height = 0;
        c.max.height = k_scroll_unbounded;
    } else {
        c.min.height = height;
        c.max.height = height;
    }
    return c;
}

lx::size2i lx::ui::scroll_node::measure(lx::layout::constraints c, size_hint hint) const {
    const ui_node* content = child_at(0);
    lx::size2i s{};
    if (content) {
        lx::layout::constraints cc{};
        // Across the scroll the content takes what is offered; along it, all it wants.
        cc.max.width = scrolls(0) ? k_scroll_unbounded : c.max.width;
        cc.min.width = scrolls(0) ? 0 : (c.min.width == c.max.width ? c.min.width : 0);
        cc.max.height = scrolls(1) ? k_scroll_unbounded : c.max.height;
        cc.min.height = scrolls(1) ? 0 : (c.min.height == c.max.height ? c.min.height : 0);
        // The content's percent base: the viewport, where the constraint fixes it.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const percent_base_scope base{c.min.width == c.max.width ? static_cast<float>(c.max.width) : nan,
                                      c.min.height == c.max.height ? static_cast<float>(c.max.height) : nan};
        s = content->measure_cached(cc, hint);
    }
    s.width = std::clamp(s.width, c.min.width, std::max(c.min.width, c.max.width));
    s.height = std::clamp(s.height, c.min.height, std::max(c.min.height, c.max.height));
    return s;
}

lx::size2i lx::ui::scroll_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    return measure(c, hint);
}

void lx::ui::scroll_node::clamp_offsets() {
    off_[0] = std::clamp(off_[0], 0.f, std::max(static_cast<float>(content_.width - bounds_.width), 0.f));
    off_[1] = std::clamp(off_[1], 0.f, std::max(static_cast<float>(content_.height - bounds_.height), 0.f));
}

void lx::ui::scroll_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    ui_node* content = child_at(0);
    if (content) {
        const percent_base_scope base{static_cast<float>(bounds.width), static_cast<float>(bounds.height)};
        // Known across the scroll: the viewport's size; along it, the content's own.
        lx::layout::constraints cc = content_constraints(bounds.width, bounds.height);
        if (scrolls(1) && !scrolls(0))
            cc.min.width = cc.max.width = bounds.width;
        if (scrolls(0) && !scrolls(1))
            cc.min.height = cc.max.height = bounds.height;
        const lx::size2i s = content->measure_cached(cc, size_hint::preferred);
        content_ = {std::max(s.width, bounds.width), std::max(s.height, bounds.height)};
        content->place({bounds.x, bounds.y, content_.width, content_.height});
    } else {
        content_ = {bounds.width, bounds.height};
    }
    clamp_offsets();
    const bool both = axes_ == scroll_axes::both;
    for (unsigned i = 1; i < child_count(); ++i) {
        ui_node* c = child_at(i);
        if (!c || c->type() != scrollbar_node::static_type())
            continue;
        auto* bar = static_cast<scrollbar_node*>(c);
        bar->update(bounds, both);
        bar->place(bar->geometry().strip);
    }
}

void lx::ui::scroll_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    if (const ui_node* content = child_at(0)) {
        out.push_clip_local(bounds_);
        const lx::point2i o = offset();
        out.push_origin({-o.x, -o.y});
        content->emit(out);
        out.pop();
        out.pop();
    }
    for (unsigned i = 1; i < child_count(); ++i)
        if (const ui_node* c = child_at(i))
            c->emit(out);
}

const lx::layout::flex_style* lx::ui::scroll_node::flex_item_style() const { return &flex_; }

bool lx::ui::scroll_node::child_viewport(const ui_node& child, lx::point2i& offset,
                                         lx::rect2i& clip) const {
    if (&child != child_at(0))
        return false;
    const lx::point2i o = this->offset();
    offset = {-o.x, -o.y};
    clip = bounds_;
    return true;
}

lx::point2i lx::ui::scroll_node::offset() const {
    return {static_cast<int>(std::lround(off_[0])), static_cast<int>(std::lround(off_[1]))};
}

lx::ui::scroll_metrics lx::ui::scroll_node::scroll_metrics_along(unsigned axis) const {
    scroll_metrics m{};
    m.offset = off_[axis ? 1 : 0];
    m.viewport = static_cast<float>(axis ? bounds_.height : bounds_.width);
    m.content = static_cast<float>(axis ? content_.height : content_.width);
    if (!scrolls(axis))
        m.content = m.viewport;
    return m;
}

void lx::ui::scroll_node::scroll_to(unsigned axis, float offset) {
    axis = axis ? 1 : 0;
    if (!scrolls(axis))
        return;
    const lx::point2i before = this->offset();
    off_[axis] = offset;
    clamp_offsets();
    const lx::point2i after = this->offset();
    if (before.x == after.x && before.y == after.y)
        return;
    // The viewport repaints (its content moved under the clip, the scrollbars with it);
    // nothing is laid out again.
    invalidate_paint(*this);
}

bool lx::ui::scroll_node::scroll_by(float dx, float dy) {
    const lx::point2i before = offset();
    if (dx != 0.f)
        scroll_to(0, off_[0] + dx);
    if (dy != 0.f)
        scroll_to(1, off_[1] + dy);
    const lx::point2i after = offset();
    return before.x != after.x || before.y != after.y;
}

void lx::ui::scroll_node::scroll_into_view(lx::rect2i r) {
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (!scrolls(axis))
            continue;
        const float vp = static_cast<float>(axis ? bounds_.height : bounds_.width);
        const float origin = static_cast<float>(axis ? bounds_.y : bounds_.x);
        const float a = static_cast<float>(axis ? r.y : r.x) - origin;
        const float b = a + static_cast<float>(axis ? r.height : r.width);
        float o = off_[axis];
        if (a < o)
            o = a;
        else if (b > o + vp)
            o = b - vp;
        scroll_to(axis, o);
    }
}

bool lx::ui::scroll_node::on_pointer(const pointer_event& e) {
    if (e.kind != pointer_kind::wheel)
        return false;
    return scroll_by(scrolls(0) ? e.dx : 0.f, scrolls(1) ? e.dy : 0.f);
}

lx::a11y::node lx::ui::scroll_node::accessibility_node() const {
    return {a11y::role::scroll_area, name_, "", "", 0, const_cast<scroll_node*>(this)};
}

lx::ui::child lx::ui::scroll_area(const style& viewport, std::span<const child> children,
                                  scroll_axes axes, const style& content, element_key key) {
    scroll_props p{};
    p.layout = viewport;
    p.axes = axes;
    const child inner = div(content, children);
    child kids[3] = {inner, nullptr, nullptr};
    unsigned n = 1;
    if (axes != scroll_axes::horizontal)
        kids[n++] = describe<scrollbar_node, scrollbar_props>({1});
    if (axes != scroll_axes::vertical)
        kids[n++] = describe<scrollbar_node, scrollbar_props>({0});
    return with_children(describe<scroll_node, scroll_props>(p, key), {kids, n});
}
