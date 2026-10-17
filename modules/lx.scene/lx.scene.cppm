module;

#include <cmath>

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scheduler;

export module lx.scene;

export import :snapshot;

export namespace lx::scene {

enum class node_kind { container, widget, surface, layer };

/// How layout (logical) coordinates reach one output's pixels: `origin` is the output's
/// top-left in the layout, `scale` its scale (n/120 for a fractional one). The scene, the
/// window manager and input all work in logical coordinates; this is applied in exactly one
/// place, when the scene emits draw commands and converts damage. At scale 1 it is a pure
/// translation, so every draw and damage rect is what it was before scales existed.
struct output_projection {
    lx::point2i origin{};
    double scale = 1.0;
    /// The output's mode in pixels: what the visibility pass clips draws to. Empty means
    /// unbounded (tests, one-off emission).
    lx::size2i size{};
};

/// The output-pixel rect of a node of `size` at logical `node`, in a tree whose root sits at
/// logical `root`: the root is anchored on the pixel grid (`R = round((root − origin)·s)`)
/// and every edge is rounded relative to it (`x0 = R + round((node − root)·s)`, `x1 = R +
/// round((node − root + w)·s)`). A fractional-scale client renders `round(w·s)` pixels for a
/// `w`-wide surface; with the root on whole pixels its draw is then exactly that wide — a
/// 1:1 texel mapping, pixel-exact — and the nodes of one window tile without gaps (wlroots'
/// `scale_box`). `round` is half away from zero, fractional-scale-v1's rule.
[[nodiscard]] lx::rect2i project_node(const output_projection& p, lx::point2i root,
                                      lx::point2i node, lx::size2i size);
/// A logical damage rect in output pixels: start floored, end ceiled, then one pixel wider
/// on every side at a non-integer scale (edge rounding moves a node's edges by up to half a
/// pixel either way relative to the exact product).
[[nodiscard]] lx::rect2i project_damage(const output_projection& p, lx::rect2i logical);

/// The visibility pass, run once per published frame on draws in paint order (bottom
/// first), before they are batched: walks them top-down keeping up to eight opaque rects
/// (the largest, when more are found) seeded from opaque draws — `blend_mode::opaque`,
/// opacity and tint alpha 1, no linear transform — and marks a draw `occluded` when it lies
/// fully inside one of them, or entirely outside `output` (when that is not empty). Every
/// backend skips occluded draws; the pixels are identical, so damage and buffer age are
/// untouched. `shown[i]`, when given, receives whether draw `i` reaches the screen (not
/// occluded). Returns the number of draws marked. O(count × 8), no allocation.
unsigned mark_occlusion(draw_command* draws, unsigned count, lx::rect2i output, bool* shown);

/// What `emit_draws` needs to place a node: the output projection, and the logical point
/// its tree rounds edges relative to (a window's root surface origin) when it has one.
struct emit_context {
    output_projection projection{};
    lx::point2i anchor{};
    bool anchored = false;
};

class scene_node {
public:
    // Out-of-line so the Itanium key function (and thus the vtable) lives in exactly one
    // object file. An in-class `= default` makes Clang emit a strong vtable in every
    // importer under C++ modules, which then fails at link time.
    virtual ~scene_node();
    [[nodiscard]] virtual node_kind kind() const = 0;

    void set_transform(lx::point2i origin, float scale = 1.f, float opacity = 1.f);
    void set_visible(bool visible);
    void mark_dirty(lx::rect2i region);
    /// Colour multiplier for what the node draws (white: none). A dimmed window (a client
    /// that stopped answering pings) is drawn through a grey tint; every backend applies
    /// `draw_command::tint`. The caller damages what changed.
    void set_tint(lx::color tint);

    [[nodiscard]] lx::rect2i bounds() const;
    [[nodiscard]] bool is_dirty() const;
    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] lx::point2i origin() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] float opacity() const;
    [[nodiscard]] lx::color tint() const;

    /// Optional draw emission hook (surface + widget nodes): appends draw commands with
    /// `dst` in output pixels, placed through `ctx`.
    virtual void emit_draws(draw_list& out, const emit_context& ctx) const;
    /// The same at the identity projection (tests, one-off emission).
    void emit_draws(draw_list& out) const { emit_draws(out, emit_context{}); }
    /// The visibility pass found a draw of this node on screen in published frame `frame`.
    virtual void on_shown(unsigned frame) { (void)frame; }

protected:
    lx::point2i origin_{};
    float scale_ = 1.f;
    float opacity_ = 1.f;
    lx::color tint_ = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    bool visible_ = true;
    lx::rect2i bounds_{};
    bool dirty_ = true;
};

class container_node : public scene_node {
public:
    [[nodiscard]] node_kind kind() const override;
    [[nodiscard]] lx::runtime::overflow_action child_overflow_policy() const;
    void set_child_overflow_policy(lx::runtime::overflow_action policy);
    [[nodiscard]] bool add(scene_node* child);
    /// Unlinks `child` so the caller can destroy it. Without this a freed surface node
    /// stays reachable from the graph and the next traversal walks freed memory.
    bool remove(scene_node* child);
    [[nodiscard]] lx::layout::layout_node& layout();

    [[nodiscard]] unsigned child_count() const;
    [[nodiscard]] scene_node* child_at(unsigned index) const;

    /// Moves `child` to paint position `index` (0 = bottom), clamped to the last slot,
    /// preserving the relative order of the others. False if `child` is not a child.
    bool move_to(scene_node* child, unsigned index);
    /// `move_to` the top (painted last). False if `child` is not a child.
    bool raise_to_top(scene_node* child);

    /// An anchor container (one window's surface tree): its origin is the logical point
    /// every node inside rounds its edges relative to (see `project_node`), so the window's
    /// surfaces and frame land on the pixel grid together.
    void set_anchor(bool anchor) { anchor_ = anchor; }
    [[nodiscard]] bool anchor() const { return anchor_; }

private:
    lx::layout::flex_node layout_{};
    bool anchor_ = false;
    scene_node* children_[128]{};
    unsigned count_ = 0;
    lx::runtime::overflow_action child_overflow_ = lx::runtime::overflow_action::drop_newest;
};

class surface_node : public scene_node {
public:
    explicit surface_node(lx::surface_id surface);
    [[nodiscard]] node_kind kind() const override;

    void attach(lx::gfx::imported_image image);
    void set_source_damage(lx::rect2i region);
    /// Logical extents (`{0, 0, surface size}`): what the surface covers in the layout.
    void set_bounds(lx::rect2i bounds);
    /// What of the buffer is shown and how: the source box in buffer pixels (empty: the
    /// whole buffer) and the buffer transform (wl_surface + wp_viewport).
    void set_content(lx::rect2f src, lx::buffer_transform transform);
    [[nodiscard]] lx::rect2f source() const { return src_; }
    [[nodiscard]] lx::buffer_transform buffer_transform() const { return transform_; }

    [[nodiscard]] lx::surface_id surface() const;
    [[nodiscard]] lx::texture_id texture() const;
    [[nodiscard]] lx::gfx::imported_image image() const;
    /// The content covers its bounds with no translucency: a format without alpha, or an
    /// opaque region spanning the whole buffer. Only then may a draw skip blending, so the
    /// default is false — an ARGB client's transparent pixels (shadows, rounded corners)
    /// must blend, never be drawn as black.
    void set_opaque(bool opaque);
    [[nodiscard]] bool opaque() const;
    using scene_node::emit_draws;
    void emit_draws(draw_list& out, const emit_context& ctx) const override;
    void on_shown(unsigned frame) override { visible_frame_ = frame; }
    /// Whether this surface was on screen in published frame `frame`: its draw was emitted,
    /// reaches the output and is not covered by an opaque draw above it. O(1). What
    /// presentation feedback samples and frame callbacks are paced by.
    [[nodiscard]] bool visible_in(unsigned frame) const {
        return visible_frame_ == frame && frame != k_never_shown;
    }
    static constexpr unsigned k_never_shown = ~0u;

private:
    unsigned visible_frame_ = k_never_shown;
    lx::surface_id surface_{};
    lx::gfx::imported_image image_{};
    lx::texture_id texture_{};
    lx::rect2f src_{};
    lx::buffer_transform transform_ = lx::buffer_transform::normal;
    bool opaque_ = false;
};

class scene_graph {
public:
    [[nodiscard]] container_node& root();

    /// UI thread: mutate scene, layout, mark damage.
    void update(double dt_seconds);
    /// Bounding box of the damage noted since the last publish.
    void collect_damage(lx::rect2i& total_damage);
    /// Layout (logical) coordinates, projected to output pixels at once (`project_damage`).
    /// `margin_px` widens the result on every side: a resampled draw's linear filter reaches
    /// up to half a source texel past its own pixels. Kept as separate rects, merged only
    /// where cheap — see `gfx::damage_region`.
    void note_damage(lx::rect2i region, int margin_px = 0);
    /// Damage already in output pixels (the cursor, which is placed in pixels).
    void note_damage_physical(lx::rect2i region);
    /// The whole output changed (mode change, first frame).
    void note_full_damage();
    /// The output projection draws and damage use. A change means every pixel moved: the
    /// caller notes full damage with it.
    void set_projection(output_projection projection) { projection_ = projection; }
    [[nodiscard]] const output_projection& projection() const { return projection_; }
    /// What every published snapshot shows where no draw does (the session lock's solid
    /// colour; black otherwise). A change repaints: the caller notes full damage with it.
    void set_clear_color(lx::color c) { clear_color_ = c; }
    [[nodiscard]] lx::color clear_color() const { return clear_color_; }
    /// Damage noted since the last publish. Empty means the next snapshot changes nothing.
    [[nodiscard]] const lx::gfx::damage_region& pending_damage() const;

    /// UI thread: traverse scene graph into target draw list (zero interim copy).
    void build_draw_list_into(draw_list& out);

    /// UI thread: build draw list in write slot, run the visibility pass (`mark_occlusion`;
    /// every shown node learns `frame_index`), sort, publish (index swap only).
    [[nodiscard]] bool commit_frame(unsigned frame_index);
    /// Draws the last visibility pass marked occluded (skipped by the backend).
    [[nodiscard]] unsigned last_culled() const { return last_culled_; }

    /// Render thread: const ref to latest snapshot (no copy). Takes a reader lease.
    [[nodiscard]] const immutable_frame_snapshot& acquire_render_snapshot();

    /// Render thread: after present — releases in-flight slot.
    void release_render_frame(const immutable_frame_snapshot& snapshot);

    [[nodiscard]] snapshot_buffer& snapshots();

private:
    void emit_node(scene_node* node, draw_list& out, const emit_context& ctx);

    container_node root_{};
    output_projection projection_{};
    lx::color clear_color_ = lx::color::rgb(0.f, 0.f, 0.f);
    snapshot_buffer snapshots_{};
    lx::gfx::damage_region scratch_damage_{};
    /// UI scratch of the visibility pass, parallel to the draw list being built: the node
    /// that emitted each draw, and whether the pass found it on screen. Never leaves the UI
    /// thread (the snapshot carries only `draw_command::occluded`).
    scene_node* emitters_[k_max_draw_commands]{};
    bool shown_[k_max_draw_commands]{};
    unsigned last_culled_ = 0;
};

} // namespace lx::scene

module :private;

lx::scene::scene_node::~scene_node() = default;

lx::scene::node_kind lx::scene::container_node::kind() const {
    return node_kind::container;
}

lx::scene::node_kind lx::scene::surface_node::kind() const {
    return node_kind::surface;
}

void lx::scene::scene_node::set_transform(lx::point2i o, float s, float a) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    origin_ = o;
    scale_ = s;
    opacity_ = a;
    dirty_ = true;
}
void lx::scene::scene_node::set_visible(bool v) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    visible_ = v;
    dirty_ = true;
}
void lx::scene::scene_node::mark_dirty(lx::rect2i) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    dirty_ = true;
}
lx::rect2i lx::scene::scene_node::bounds() const { return bounds_; }
bool lx::scene::scene_node::is_dirty() const { return dirty_; }
bool lx::scene::scene_node::is_visible() const { return visible_; }
lx::point2i lx::scene::scene_node::origin() const { return origin_; }
float lx::scene::scene_node::scale() const { return scale_; }
float lx::scene::scene_node::opacity() const { return opacity_; }
lx::color lx::scene::scene_node::tint() const { return tint_; }
void lx::scene::scene_node::set_tint(lx::color t) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    tint_ = t;
    dirty_ = true;
}
void lx::scene::scene_node::emit_draws(draw_list&, const emit_context&) const {}

lx::rect2i lx::scene::project_node(const output_projection& p, lx::point2i root,
                                   lx::point2i node, lx::size2i size) {
    if (p.scale == 1.0)
        return {node.x - p.origin.x, node.y - p.origin.y, size.width, size.height};
    const double s = p.scale;
    const long rx = std::lround(static_cast<double>(root.x - p.origin.x) * s);
    const long ry = std::lround(static_cast<double>(root.y - p.origin.y) * s);
    const double dx = static_cast<double>(node.x) - root.x;
    const double dy = static_cast<double>(node.y) - root.y;
    const long x0 = rx + std::lround(dx * s);
    const long y0 = ry + std::lround(dy * s);
    const long x1 = rx + std::lround((dx + size.width) * s);
    const long y1 = ry + std::lround((dy + size.height) * s);
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

lx::rect2i lx::scene::project_damage(const output_projection& p, lx::rect2i r) {
    if (r.width <= 0 || r.height <= 0)
        return {};
    if (p.scale == 1.0)
        return {r.x - p.origin.x, r.y - p.origin.y, r.width, r.height};
    const double s = p.scale;
    constexpr double lo = -1.0e9, hi = 1.0e9;
    const auto clamp = [](double v) { return v < lo ? lo : (v > hi ? hi : v); };
    const double fx0 = std::floor(clamp((static_cast<double>(r.x) - p.origin.x) * s));
    const double fy0 = std::floor(clamp((static_cast<double>(r.y) - p.origin.y) * s));
    const double fx1 =
        std::ceil(clamp((static_cast<double>(r.x) + r.width - p.origin.x) * s));
    const double fy1 =
        std::ceil(clamp((static_cast<double>(r.y) + r.height - p.origin.y) * s));
    const int widen = s == std::floor(s) ? 0 : 1;
    const long x0 = static_cast<long>(fx0) - widen;
    const long y0 = static_cast<long>(fy0) - widen;
    const long x1 = static_cast<long>(fx1) + widen;
    const long y1 = static_cast<long>(fy1) + widen;
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

void lx::scene::container_node::set_child_overflow_policy(lx::runtime::overflow_action policy) {
    child_overflow_ = policy;
}
lx::runtime::overflow_action lx::scene::container_node::child_overflow_policy() const {
    return child_overflow_;
}
bool lx::scene::container_node::add(scene_node* c) {
    if (count_ < 128) {
        children_[count_++] = c;
        return true;
    }
    if (child_overflow_ == lx::runtime::overflow_action::drop_oldest && count_ > 0) {
        for (unsigned i = 1; i < count_; ++i) children_[i - 1] = children_[i];
        children_[count_ - 1] = c;
        return true;
    }
    return false;
}
bool lx::scene::container_node::remove(scene_node* c) {
    if (!c)
        return false;
    for (unsigned i = 0; i < count_; ++i) {
        if (children_[i] != c)
            continue;
        // Preserve stacking order: shift the tail down rather than swapping with the end.
        for (unsigned j = i + 1; j < count_; ++j)
            children_[j - 1] = children_[j];
        children_[--count_] = nullptr;
        return true;
    }
    return false;
}
lx::layout::layout_node& lx::scene::container_node::layout() { return layout_; }
unsigned lx::scene::container_node::child_count() const { return count_; }
lx::scene::scene_node* lx::scene::container_node::child_at(unsigned index) const {
    return index < count_ ? children_[index] : nullptr;
}
bool lx::scene::container_node::move_to(scene_node* c, unsigned index) {
    if (!c || count_ == 0)
        return false;
    unsigned from = count_;
    for (unsigned i = 0; i < count_; ++i) {
        if (children_[i] == c) {
            from = i;
            break;
        }
    }
    if (from == count_)
        return false;
    const unsigned to = index < count_ ? index : count_ - 1;
    // Order-preserving shift of the span between the two positions.
    if (from < to) {
        for (unsigned i = from; i < to; ++i)
            children_[i] = children_[i + 1];
    } else {
        for (unsigned i = from; i > to; --i)
            children_[i] = children_[i - 1];
    }
    children_[to] = c;
    return true;
}
bool lx::scene::container_node::raise_to_top(scene_node* c) {
    return count_ > 0 && move_to(c, count_ - 1);
}

lx::scene::surface_node::surface_node(lx::surface_id s) : surface_{s} {}
void lx::scene::surface_node::attach(lx::gfx::imported_image img) {
    image_ = img;
    texture_ = lx::texture_id{img.image_id};
    dirty_ = true;
}
void lx::scene::surface_node::set_source_damage(lx::rect2i) { dirty_ = true; }
void lx::scene::surface_node::set_bounds(lx::rect2i b) {
    bounds_ = b;
    dirty_ = true;
}
void lx::scene::surface_node::set_content(lx::rect2f src, lx::buffer_transform transform) {
    src_ = src;
    transform_ = transform;
    dirty_ = true;
}
lx::surface_id lx::scene::surface_node::surface() const { return surface_; }
lx::texture_id lx::scene::surface_node::texture() const { return texture_; }
lx::gfx::imported_image lx::scene::surface_node::image() const { return image_; }
void lx::scene::surface_node::set_opaque(bool opaque) {
    if (opaque_ != opaque) {
        opaque_ = opaque;
        dirty_ = true;
    }
}
bool lx::scene::surface_node::opaque() const { return opaque_; }

void lx::scene::surface_node::emit_draws(draw_list& out, const emit_context& ctx) const {
    if (!texture() || !is_visible())
        return;

    lx::rect2i b = bounds();
    if (b.width <= 0 || b.height <= 0)
        b = {0, 0, 1, 1};
    const lx::point2i origin = this->origin();
    const lx::point2i at{origin.x + b.x, origin.y + b.y};
    lx::size2i size{b.width, b.height};
    if (scale() != 1.f) {
        size.width = static_cast<int>(static_cast<float>(size.width) * scale());
        size.height = static_cast<int>(static_cast<float>(size.height) * scale());
    }

    draw_command cmd{};
    cmd.texture = texture();
    // A root on its own rounds relative to itself; inside a window, relative to the window.
    cmd.dst = project_node(ctx.projection, ctx.anchored ? ctx.anchor : at, at, size);
    cmd.src = src_;
    cmd.buffer_xform = transform_;
    // 1:1 or resampled: the source box, turned the way the transform turns it, against dst.
    {
        float sw = src_.width;
        float sh = src_.height;
        if (src_.empty()) {
            sw = static_cast<float>(image_.width);
            sh = static_cast<float>(image_.height);
        }
        if ((static_cast<unsigned>(transform_) & 1u) != 0) {
            const float t = sw;
            sw = sh;
            sh = t;
        }
        // A crop off whole texels samples between them too.
        const bool whole = src_.x == std::floor(src_.x) && src_.y == std::floor(src_.y);
        cmd.scaled = !whole || sw != static_cast<float>(cmd.dst.width) ||
                     sh != static_cast<float>(cmd.dst.height);
    }
    cmd.transform = lx::transform2d::translate(cmd.dst.x, cmd.dst.y);
    cmd.opacity = opacity();
    cmd.tint = lx::color::rgb(tint_.r, tint_.g, tint_.b, tint_.a * opacity());
    // Blend from content, not from opacity: an ARGB buffer at opacity 1 still has to blend.
    cmd.blend = opaque_ && opacity() >= 1.f ? lx::blend_mode::opaque
                                            : lx::blend_mode::premultiplied;
    cmd.sort_key = lx::draw_sort_key::make(0, out.size());
    out.push(cmd);
}

lx::scene::container_node& lx::scene::scene_graph::root() { return root_; }
void lx::scene::scene_graph::update(double) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    lx::layout::layout_engine engine{};
    lx::layout::constraints c{{0, 0}, {1920, 1080}};
    engine.solve(root_.layout(), c);
}
void lx::scene::scene_graph::collect_damage(lx::rect2i& d) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    d = scratch_damage_.bounds();
}

void lx::scene::scene_graph::note_damage(lx::rect2i region, int margin_px) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    lx::rect2i r = project_damage(projection_, region);
    if (r.width <= 0 || r.height <= 0)
        return;
    if (margin_px > 0)
        r = {r.x - margin_px, r.y - margin_px, r.width + 2 * margin_px, r.height + 2 * margin_px};
    scratch_damage_.add(r);
}

void lx::scene::scene_graph::note_damage_physical(lx::rect2i region) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    scratch_damage_.add(region);
}

void lx::scene::scene_graph::note_full_damage() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    scratch_damage_.set_full();
}

const lx::gfx::damage_region& lx::scene::scene_graph::pending_damage() const {
    return scratch_damage_;
}

void lx::scene::scene_graph::emit_node(scene_node* node, draw_list& out,
                                       const emit_context& ctx) {
    if (!node || !node->is_visible())
        return;

    if (node->kind() == node_kind::container) {
        auto* container = static_cast<container_node*>(node);
        if (container->anchor()) {
            emit_context inner = ctx;
            inner.anchor = container->origin();
            inner.anchored = true;
            for (unsigned i = 0; i < container->child_count(); ++i)
                emit_node(container->child_at(i), out, inner);
            return;
        }
        for (unsigned i = 0; i < container->child_count(); ++i)
            emit_node(container->child_at(i), out, ctx);
        return;
    }

    const unsigned before = out.size();
    node->emit_draws(out, ctx);
    for (unsigned i = before; i < out.size() && i < k_max_draw_commands; ++i)
        emitters_[i] = node;
}

namespace {

[[nodiscard]] lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    const int y1 = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

[[nodiscard]] bool inside(lx::rect2i inner, lx::rect2i outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Covers every pixel of its (clipped) dst with full alpha: opaque blending of a buffer
/// whose content is opaque, at full opacity and tint, placed by translation only.
[[nodiscard]] bool covers_opaquely(const lx::scene::draw_command& d) {
    return d.blend == lx::blend_mode::opaque && d.opacity >= 1.f && d.tint.a >= 1.f &&
           d.transform.m0 == 1.f && d.transform.m1 == 0.f && d.transform.m2 == 0.f &&
           d.transform.m3 == 1.f;
}

} // namespace

unsigned lx::scene::mark_occlusion(draw_command* draws, unsigned count, lx::rect2i output,
                                   bool* shown) {
    constexpr unsigned k_max_occluders = 8;
    lx::rect2i occluders[k_max_occluders]{};
    long long areas[k_max_occluders]{};
    unsigned n = 0;
    unsigned marked = 0;
    const bool bounded = output.width > 0 && output.height > 0;
    for (unsigned i = count; i-- > 0;) {
        draw_command& d = draws[i];
        lx::rect2i r = d.dst;
        if (d.clip.width > 0 && d.clip.height > 0)
            r = intersect(r, d.clip);
        if (bounded)
            r = intersect(r, output);
        bool on_screen = r.width > 0 && r.height > 0;
        for (unsigned k = 0; on_screen && k < n; ++k)
            if (inside(r, occluders[k]))
                on_screen = false;
        d.occluded = !on_screen;
        if (shown)
            shown[i] = on_screen;
        if (!on_screen) {
            ++marked;
            continue;
        }
        if (!covers_opaquely(d))
            continue;
        const long long area = static_cast<long long>(r.width) * r.height;
        if (n < k_max_occluders) {
            occluders[n] = r;
            areas[n] = area;
            ++n;
            continue;
        }
        // Full: the eight largest are kept — they hide the most.
        unsigned smallest = 0;
        for (unsigned k = 1; k < n; ++k)
            if (areas[k] < areas[smallest])
                smallest = k;
        if (area > areas[smallest]) {
            occluders[smallest] = r;
            areas[smallest] = area;
        }
    }
    return marked;
}

void lx::scene::scene_graph::build_draw_list_into(draw_list& out) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    out.clear();
    emit_context ctx{};
    ctx.projection = projection_;
    emit_node(&root_, out, ctx);
}

bool lx::scene::scene_graph::commit_frame(unsigned frame_index) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);

    // Take the slot before building. Beyond correctness this skips the whole scene walk
    // and sort when there is nowhere to publish, instead of doing the work and throwing
    // the result away.
    auto lease = snapshots_.begin_frame();
    if (!lease.valid())
        return false;

    build_draw_list_into(lease.draws());
    // The visibility pass, in paint order (before batching reorders opaque draws): what is
    // covered is skipped by the backend, and every node with a draw on screen learns this
    // frame — presentation feedback and frame-callback pacing ask it.
    draw_list& draws = lease.draws();
    const unsigned n = draws.size() < k_max_draw_commands ? draws.size() : k_max_draw_commands;
    const lx::rect2i output{0, 0, projection_.size.width, projection_.size.height};
    last_culled_ = mark_occlusion(draws.data(), n, output, shown_);
    for (unsigned i = 0; i < n; ++i)
        if (shown_[i] && emitters_[i])
            emitters_[i]->on_shown(frame_index);
    draws.batch_for_render();

    const bool published = snapshots_.publish(lease, scratch_damage_, frame_index, clear_color_);
    // Damage accumulates per frame. Retain it when the publish was dropped, otherwise the
    // region would be lost and the next presented frame would under-report its damage.
    if (published)
        scratch_damage_.clear();
    return published;
}

const lx::scene::immutable_frame_snapshot& lx::scene::scene_graph::acquire_render_snapshot() {
    lx::runtime::assert_affinity(lx::runtime::affinity::render);
    return snapshots_.acquire();
}

void lx::scene::scene_graph::release_render_frame(const immutable_frame_snapshot& snapshot) {
    lx::runtime::assert_affinity(lx::runtime::affinity::render);
    snapshots_.release(snapshot);
}
lx::scene::snapshot_buffer& lx::scene::scene_graph::snapshots() { return snapshots_; }
