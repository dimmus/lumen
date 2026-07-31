module;

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scheduler;

export module lx.scene;

export import :snapshot;

export namespace lx::scene {

enum class node_kind { container, widget, surface, layer };

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

    [[nodiscard]] lx::rect2i bounds() const;
    [[nodiscard]] bool is_dirty() const;
    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] lx::point2i origin() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] float opacity() const;

    /// Optional draw emission hook (surface + widget nodes).
    virtual void emit_draws(draw_list& out) const;

protected:
    lx::point2i origin_{};
    float scale_ = 1.f;
    float opacity_ = 1.f;
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

private:
    lx::layout::flex_node layout_{};
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
    void set_bounds(lx::rect2i bounds);

    [[nodiscard]] lx::surface_id surface() const;
    [[nodiscard]] lx::texture_id texture() const;
    [[nodiscard]] lx::gfx::imported_image image() const;
    void emit_draws(draw_list& out) const override;

private:
    lx::surface_id surface_{};
    lx::gfx::imported_image image_{};
    lx::texture_id texture_{};
};

class scene_graph {
public:
    [[nodiscard]] container_node& root();

    /// UI thread: mutate scene, layout, mark damage.
    void update(double dt_seconds);
    void collect_damage(lx::rect2i& total_damage);
    void note_damage(lx::rect2i region);

    /// UI thread: traverse scene graph into target draw list (zero interim copy).
    void build_draw_list_into(draw_list& out);

    /// UI thread: build draw list in write slot, sort, publish (index swap only).
    [[nodiscard]] bool commit_frame(unsigned frame_index);

    /// Render thread: const ref to latest snapshot (no copy). Takes a reader lease.
    [[nodiscard]] const immutable_frame_snapshot& acquire_render_snapshot();

    /// Render thread: after present — releases in-flight slot.
    void release_render_frame(const immutable_frame_snapshot& snapshot);

    [[nodiscard]] snapshot_buffer& snapshots();

private:
    void emit_node(scene_node* node, draw_list& out);

    container_node root_{};
    snapshot_buffer snapshots_{};
    lx::rect2i scratch_damage_{};
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
void lx::scene::scene_node::emit_draws(draw_list&) const {}

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
lx::surface_id lx::scene::surface_node::surface() const { return surface_; }
lx::texture_id lx::scene::surface_node::texture() const { return texture_; }
lx::gfx::imported_image lx::scene::surface_node::image() const { return image_; }

void lx::scene::surface_node::emit_draws(draw_list& out) const {
    if (!texture() || !is_visible())
        return;

    lx::rect2i dst = bounds();
    if (dst.width <= 0 || dst.height <= 0)
        dst = {0, 0, 1, 1};

    const lx::point2i origin = this->origin();
    dst.x += origin.x;
    dst.y += origin.y;
    if (scale() != 1.f) {
        dst.width = static_cast<int>(static_cast<float>(dst.width) * scale());
        dst.height = static_cast<int>(static_cast<float>(dst.height) * scale());
    }

    draw_command cmd{};
    cmd.texture = texture();
    cmd.dst = dst;
    cmd.src = {0, 0, dst.width, dst.height};
    cmd.transform = lx::transform2d::translate(origin.x, origin.y);
    if (scale() != 1.f)
        cmd.transform = lx::transform2d::scale(scale(), scale());
    cmd.opacity = opacity();
    cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, opacity());
    cmd.blend = opacity() >= 1.f ? lx::blend_mode::opaque : lx::blend_mode::premultiplied;
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
    d = scratch_damage_;
}

void lx::scene::scene_graph::note_damage(lx::rect2i region) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (region.width <= 0 || region.height <= 0)
        return;
    if (scratch_damage_.width <= 0 || scratch_damage_.height <= 0) {
        scratch_damage_ = region;
        return;
    }
    const int x0 = scratch_damage_.x < region.x ? scratch_damage_.x : region.x;
    const int y0 = scratch_damage_.y < region.y ? scratch_damage_.y : region.y;
    const int x1 = (scratch_damage_.x + scratch_damage_.width) > (region.x + region.width)
                       ? (scratch_damage_.x + scratch_damage_.width)
                       : (region.x + region.width);
    const int y1 = (scratch_damage_.y + scratch_damage_.height) > (region.y + region.height)
                       ? (scratch_damage_.y + scratch_damage_.height)
                       : (region.y + region.height);
    scratch_damage_ = {x0, y0, x1 - x0, y1 - y0};
}

void lx::scene::scene_graph::emit_node(scene_node* node, draw_list& out) {
    if (!node || !node->is_visible())
        return;

    if (node->kind() == node_kind::container) {
        auto* container = static_cast<container_node*>(node);
        for (unsigned i = 0; i < container->child_count(); ++i)
            emit_node(container->child_at(i), out);
        return;
    }

    node->emit_draws(out);
}

void lx::scene::scene_graph::build_draw_list_into(draw_list& out) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    out.clear();
    emit_node(&root_, out);
}

bool lx::scene::scene_graph::commit_frame(unsigned frame_index) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    auto& draws = snapshots_.write_draw_list();
    build_draw_list_into(draws);
    draws.batch_for_render();
    const bool published = snapshots_.try_publish(scratch_damage_, frame_index);
    // Damage accumulates per frame. Retain it when the publish was dropped, otherwise the
    // region would be lost and the next presented frame would under-report its damage.
    if (published)
        scratch_damage_ = {};
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
