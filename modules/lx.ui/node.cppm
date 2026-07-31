module;

import lx.foundation;
import lx.layout;
import lx.scene;
import lx.a11y;

export module lx.ui:node;

import :element;
import :events;
import :style;

export namespace lx::ui {

/// Which end of the constraint range a measure pass is asking about. Measuring for
/// `min` and `max` separately is what lets flex distribute slack without guessing.
enum class size_hint : unsigned char { min, preferred, max };

/// Result of routing a point through the tree — window chrome needs to distinguish
/// "content" from "drag me" and "resize me" before the event reaches a widget.
enum class hit_result : unsigned char {
    pass,
    content,
    drag,
    resize_top,
    resize_bottom,
    resize_left,
    resize_right,
};

/// Draw-emission target. Nodes append `scene::draw_command`s; they never rasterize.
/// The clip/opacity/origin stack is applied on the way in so children cannot escape
/// their parent's bounds.
class draw_scope {
public:
    draw_scope(lx::scene::draw_list& out, lx::rect2i clip);

    void push_clip(lx::rect2i clip);
    void push_opacity(float opacity);
    void push_origin(lx::point2i delta);
    void pop();

    /// Append a textured quad. Rejected silently when fully clipped away.
    void quad(lx::texture_id texture, lx::rect2i dst, lx::rect2i src, lx::color tint);
    /// Append a solid fill — no texture binding needed on the render side.
    void fill(lx::rect2i dst, lx::color color);

    [[nodiscard]] lx::rect2i clip() const;
    [[nodiscard]] float opacity() const;
    [[nodiscard]] unsigned emitted() const;

private:
    struct frame {
        lx::rect2i clip{};
        float opacity = 1.f;
        lx::point2i origin{};
    };

    void push_frame(frame f);
    [[nodiscard]] frame& top();
    [[nodiscard]] const frame& top() const;

    static constexpr unsigned k_max_depth = 32;

    lx::scene::draw_list* out_ = nullptr;
    frame stack_[k_max_depth]{};
    unsigned depth_ = 1;
    unsigned emitted_ = 0;
};

/// Retained UI node. Lives across frames and owns its children; descriptors are
/// reconciled onto it rather than replacing it, so scroll offsets, carets, and
/// animation state survive a rebuild.
class ui_node {
public:
    virtual ~ui_node();

    ui_node(const ui_node&) = delete;
    ui_node& operator=(const ui_node&) = delete;

    [[nodiscard]] virtual element_type_id type() const = 0;
    [[nodiscard]] virtual const char* type_name() const;

    /// Pass 1. Must not mutate observable state — it is called repeatedly per pass.
    [[nodiscard]] virtual lx::size2i measure(layout::constraints c, size_hint hint) const;
    /// Pass 2. Assigns final geometry and recurses.
    virtual void arrange(lx::rect2i bounds);
    /// Pass 3. Appends draw commands for this node and its children.
    virtual void emit(draw_scope& out) const;

    virtual bool on_mouse(mouse_event event);
    virtual bool on_key(key_event event);
    [[nodiscard]] virtual hit_result hit_test(lx::point2i p) const;

    [[nodiscard]] virtual a11y::node accessibility_node() const;

    static constexpr unsigned k_max_children = 64;

    // ── Tree structure (owned children) ──────────────────────────────────────
    void adopt(ui_node* node);
    void set_child_at(unsigned index, ui_node* node);
    void truncate_children(unsigned count);
    void release_children();

    // ── Reconciler primitives ────────────────────────────────────────────────
    // These move children without destroying them so a keyed rearrangement cannot
    // free a node it is about to reuse. Ownership passes to the caller and back.

    /// Move every child into `out` and leave this node childless. Returns how many.
    [[nodiscard]] unsigned detach_children(ui_node** out, unsigned capacity);
    /// Assign a slot without deleting the previous occupant.
    void place_child(unsigned index, ui_node* node);
    void set_child_count(unsigned count);

    [[nodiscard]] unsigned child_count() const;
    [[nodiscard]] ui_node* child_at(unsigned index) const;
    [[nodiscard]] ui_node* parent() const;

    // ── Identity / state ─────────────────────────────────────────────────────
    void set_key(element_key key);
    [[nodiscard]] element_key key() const;

    void set_visible(bool visible);
    [[nodiscard]] bool visible() const;

    void set_style(style_ref style);
    [[nodiscard]] style_ref style() const;

    [[nodiscard]] lx::rect2i bounds() const;

    /// Set by the reconciler when layout must run again before the next emit.
    void mark_layout_dirty();
    [[nodiscard]] bool layout_dirty() const;
    void clear_layout_dirty();

protected:
    ui_node() = default;

    ui_node* parent_ = nullptr;
    ui_node* children_[k_max_children]{};
    unsigned child_count_ = 0;
    element_key key_ = 0;
    lx::rect2i bounds_{};
    style_ref style_{};
    bool visible_ = true;
    bool layout_dirty_ = true;
};

/// Describe a retained node of type `NodeT` carrying `PropsT`. Props are copied into
/// the frame arena; `apply` writes them onto the retained node during reconciliation.
template<typename NodeT, typename PropsT>
[[nodiscard]] child describe(const PropsT& props, element_key key = 0) {
    build_context* ctx = build_context::current();
    if (!ctx)
        return nullptr;
    auto* stored = ctx->alloc<PropsT>();
    auto* node = ctx->alloc<element>();
    if (!stored || !node)
        return nullptr;
    *stored = props;
    node->type = NodeT::static_type();
    node->key = key;
    node->props = stored;
    node->construct = []() -> ui_node* { return new NodeT{}; };
    node->apply = [](ui_node& target, const void* raw) {
        static_cast<NodeT&>(target).apply(*static_cast<const PropsT*>(raw));
    };
    return node;
}

/// Attach children to an already-described element.
[[nodiscard]] child with_children(child parent, const child* items, unsigned count);

} // namespace lx::ui


// ── draw_scope ──────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] lx::rect2i lx_ui_intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    const int y1 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace

lx::ui::draw_scope::draw_scope(lx::scene::draw_list& out, lx::rect2i clip) : out_{&out} {
    stack_[0] = {clip, 1.f, {}};
}

void lx::ui::draw_scope::push_frame(frame f) {
    if (depth_ >= k_max_depth) {
        // Deeper nesting than the fixed stack allows: keep the innermost frame rather
        // than growing, so a pathological tree cannot allocate during a frame.
        stack_[k_max_depth - 1] = f;
        return;
    }
    stack_[depth_++] = f;
}

lx::ui::draw_scope::frame& lx::ui::draw_scope::top() { return stack_[depth_ - 1]; }
const lx::ui::draw_scope::frame& lx::ui::draw_scope::top() const { return stack_[depth_ - 1]; }

void lx::ui::draw_scope::push_clip(lx::rect2i clip) {
    frame next = top();
    next.clip = lx_ui_intersect(next.clip, clip);
    push_frame(next);
}

void lx::ui::draw_scope::push_opacity(float opacity) {
    frame next = top();
    next.opacity *= opacity;
    push_frame(next);
}

void lx::ui::draw_scope::push_origin(lx::point2i delta) {
    frame next = top();
    next.origin = next.origin + delta;
    push_frame(next);
}

void lx::ui::draw_scope::pop() {
    if (depth_ > 1)
        --depth_;
}

void lx::ui::draw_scope::quad(lx::texture_id texture, lx::rect2i dst, lx::rect2i src,
                              lx::color tint) {
    if (!out_)
        return;
    const frame& f = top();
    lx::rect2i placed = dst;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    const lx::rect2i visible = lx_ui_intersect(placed, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::scene::draw_command cmd{};
    cmd.texture = texture;
    cmd.src = src;
    cmd.dst = placed;
    cmd.clip = f.clip;
    cmd.tint = tint;
    cmd.opacity = f.opacity;
    cmd.sort_key = lx::draw_sort_key::make(1, out_->size());
    out_->push(cmd);
    ++emitted_;
}

void lx::ui::draw_scope::fill(lx::rect2i dst, lx::color color) {
    // A null texture id means "solid quad" to the render side; no binding required.
    quad(lx::texture_id{}, dst, {0, 0, dst.width, dst.height}, color);
}

lx::rect2i lx::ui::draw_scope::clip() const { return top().clip; }
float lx::ui::draw_scope::opacity() const { return top().opacity; }
unsigned lx::ui::draw_scope::emitted() const { return emitted_; }

// ── ui_node ─────────────────────────────────────────────────────────────────────

lx::ui::ui_node::~ui_node() { release_children(); }

const char* lx::ui::ui_node::type_name() const { return "ui_node"; }

lx::size2i lx::ui::ui_node::measure(layout::constraints c, size_hint hint) const {
    // Default: wrap the largest child measurement, clamped to the constraint range.
    lx::size2i out{};
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i])
            continue;
        const auto child_size = children_[i]->measure(c, hint);
        if (child_size.width > out.width)
            out.width = child_size.width;
        if (child_size.height > out.height)
            out.height = child_size.height;
    }
    if (out.width < c.min.width) out.width = c.min.width;
    if (out.height < c.min.height) out.height = c.min.height;
    if (out.width > c.max.width) out.width = c.max.width;
    if (out.height > c.max.height) out.height = c.max.height;
    return out;
}

void lx::ui::ui_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i])
            children_[i]->arrange(bounds);
    }
}

void lx::ui::ui_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i])
            children_[i]->emit(out);
    }
}

bool lx::ui::ui_node::on_mouse(mouse_event event) {
    // Topmost-first: later siblings paint over earlier ones, so they get the event first.
    for (unsigned i = child_count_; i > 0; --i) {
        ui_node* c = children_[i - 1];
        if (!c || !c->visible_)
            continue;
        if (c->bounds_.contains(event.position) && c->on_mouse(event))
            return true;
    }
    return false;
}

bool lx::ui::ui_node::on_key(key_event event) {
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i] && children_[i]->on_key(event))
            return true;
    }
    return false;
}

lx::ui::hit_result lx::ui::ui_node::hit_test(lx::point2i p) const {
    for (unsigned i = child_count_; i > 0; --i) {
        const ui_node* c = children_[i - 1];
        if (!c || !c->visible_ || !c->bounds_.contains(p))
            continue;
        if (const auto hit = c->hit_test(p); hit != hit_result::pass)
            return hit;
    }
    return hit_result::pass;
}

lx::a11y::node lx::ui::ui_node::accessibility_node() const {
    return {a11y::role::unknown, "", "", "", 0, const_cast<ui_node*>(this)};
}

void lx::ui::ui_node::adopt(ui_node* node) {
    if (!node || child_count_ >= k_max_children)
        return;
    node->parent_ = this;
    children_[child_count_++] = node;
    layout_dirty_ = true;
}

void lx::ui::ui_node::set_child_at(unsigned index, ui_node* node) {
    if (index >= k_max_children)
        return;
    if (children_[index] && children_[index] != node)
        delete children_[index];
    children_[index] = node;
    if (node)
        node->parent_ = this;
    if (index >= child_count_)
        child_count_ = index + 1;
    layout_dirty_ = true;
}

void lx::ui::ui_node::truncate_children(unsigned count) {
    for (unsigned i = count; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    if (count < child_count_) {
        child_count_ = count;
        layout_dirty_ = true;
    }
}

void lx::ui::ui_node::release_children() {
    for (unsigned i = 0; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    child_count_ = 0;
}

unsigned lx::ui::ui_node::detach_children(ui_node** out, unsigned capacity) {
    if (!out)
        return 0;
    const unsigned moved = child_count_ < capacity ? child_count_ : capacity;
    for (unsigned i = 0; i < moved; ++i) {
        out[i] = children_[i];
        children_[i] = nullptr;
    }
    // Anything beyond the caller's capacity would leak, so destroy it here.
    for (unsigned i = moved; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    child_count_ = 0;
    return moved;
}

void lx::ui::ui_node::place_child(unsigned index, ui_node* node) {
    if (index >= k_max_children)
        return;
    children_[index] = node;
    if (node)
        node->parent_ = this;
}

void lx::ui::ui_node::set_child_count(unsigned count) {
    child_count_ = count < k_max_children ? count : k_max_children;
    layout_dirty_ = true;
}

unsigned lx::ui::ui_node::child_count() const { return child_count_; }

lx::ui::ui_node* lx::ui::ui_node::child_at(unsigned index) const {
    return index < child_count_ ? children_[index] : nullptr;
}

lx::ui::ui_node* lx::ui::ui_node::parent() const { return parent_; }

void lx::ui::ui_node::set_key(element_key key) { key_ = key; }
lx::ui::element_key lx::ui::ui_node::key() const { return key_; }
void lx::ui::ui_node::set_visible(bool visible) { visible_ = visible; }
bool lx::ui::ui_node::visible() const { return visible_; }
void lx::ui::ui_node::set_style(style_ref style) { style_ = style; }
lx::ui::style_ref lx::ui::ui_node::style() const { return style_; }
lx::rect2i lx::ui::ui_node::bounds() const { return bounds_; }

void lx::ui::ui_node::mark_layout_dirty() { layout_dirty_ = true; }
bool lx::ui::ui_node::layout_dirty() const { return layout_dirty_; }
void lx::ui::ui_node::clear_layout_dirty() { layout_dirty_ = false; }

lx::ui::child lx::ui::with_children(child parent, const child* items, unsigned count) {
    if (!parent || count == 0)
        return parent;
    build_context* ctx = build_context::current();
    if (!ctx)
        return parent;
    const element** slots = ctx->alloc_children(count);
    if (!slots)
        return parent;
    for (unsigned i = 0; i < count; ++i)
        slots[i] = items[i];
    // Descriptors are arena-owned and single-use, so patching in place is safe.
    auto* mutable_parent = const_cast<element*>(parent);
    mutable_parent->children = slots;
    mutable_parent->child_count = count;
    return parent;
}
