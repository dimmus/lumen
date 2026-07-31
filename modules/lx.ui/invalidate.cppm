module;

import lx.foundation;

export module lx.ui:invalidate;

import :node;

export namespace lx::ui {

/// What a node is asking the root to redo. Kept separate because a relayout does not
/// imply a full-surface repaint — only the affected subtree's old and new bounds.
enum class invalidation : unsigned char {
    none = 0,
    paint = 1 << 0,
    layout = 1 << 1,
    animate = 1 << 2,
};

/// Bounded damage accumulator. Rects are merged so the compositor receives a fixed
/// number of regions no matter how many widgets invalidate in one frame; the array
/// never grows, matching the overflow-cap rule for per-frame structures.
class damage_ledger {
public:
    static constexpr unsigned k_capacity = 16;

    void add(lx::rect2i region);
    void request(invalidation what);

    [[nodiscard]] unsigned count() const;
    [[nodiscard]] const lx::rect2i* rects() const;

    /// Union of every accumulated region — what a full-surface path would repaint.
    [[nodiscard]] lx::rect2i union_bounds() const;

    [[nodiscard]] bool needs_paint() const;
    [[nodiscard]] bool needs_layout() const;
    [[nodiscard]] bool needs_animate() const;
    [[nodiscard]] bool empty() const;

    void clear();

private:
    /// Merge the pair whose combined area wastes the least, freeing one slot.
    void compact();

    lx::rect2i rects_[k_capacity]{};
    unsigned count_ = 0;
    unsigned flags_ = 0;
};

/// Walk to the root of a retained tree.
[[nodiscard]] ui_node* root_of(ui_node& node);

/// Repaint just this node's bounds.
void invalidate_paint(ui_node& node);
/// Repaint an explicit sub-region (text caret, hover ring) without the whole node.
void invalidate_paint(ui_node& node, lx::rect2i region);
/// Re-run measure/arrange for the subtree, damaging its current bounds. The new bounds
/// are damaged after arrange, so growing widgets repaint both old and new areas.
void invalidate_layout(ui_node& node);
/// Ask for a frame tick while an animation is in flight.
void invalidate_animate(ui_node& node);

/// The ledger the invalidate_* helpers write into. UI affinity only.
[[nodiscard]] damage_ledger& ui_damage();

} // namespace lx::ui


namespace {

lx::ui::damage_ledger g_ui_damage{};

[[nodiscard]] lx::rect2i lx_ui_union(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = (a.x + a.width) > (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    const int y1 = (a.y + a.height) > (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

[[nodiscard]] long long lx_ui_area(lx::rect2i r) {
    if (r.width <= 0 || r.height <= 0)
        return 0;
    return static_cast<long long>(r.width) * static_cast<long long>(r.height);
}

} // namespace

void lx::ui::damage_ledger::add(lx::rect2i region) {
    if (region.width <= 0 || region.height <= 0)
        return;
    flags_ |= static_cast<unsigned>(invalidation::paint);

    // Absorb into an existing rect when the union costs nothing extra — the common
    // case of a widget invalidating repeatedly within one frame.
    for (unsigned i = 0; i < count_; ++i) {
        const lx::rect2i merged = lx_ui_union(rects_[i], region);
        if (lx_ui_area(merged) <= lx_ui_area(rects_[i]) + lx_ui_area(region)) {
            rects_[i] = merged;
            return;
        }
    }

    if (count_ == k_capacity)
        compact();
    rects_[count_++] = region;
}

void lx::ui::damage_ledger::compact() {
    if (count_ < 2)
        return;
    unsigned best_a = 0;
    unsigned best_b = 1;
    long long best_waste = -1;
    for (unsigned i = 0; i < count_; ++i) {
        for (unsigned j = i + 1; j < count_; ++j) {
            const long long waste = lx_ui_area(lx_ui_union(rects_[i], rects_[j])) -
                                    lx_ui_area(rects_[i]) - lx_ui_area(rects_[j]);
            if (best_waste < 0 || waste < best_waste) {
                best_waste = waste;
                best_a = i;
                best_b = j;
            }
        }
    }
    rects_[best_a] = lx_ui_union(rects_[best_a], rects_[best_b]);
    rects_[best_b] = rects_[count_ - 1];
    --count_;
}

void lx::ui::damage_ledger::request(invalidation what) {
    flags_ |= static_cast<unsigned>(what);
}

unsigned lx::ui::damage_ledger::count() const { return count_; }
const lx::rect2i* lx::ui::damage_ledger::rects() const { return rects_; }

lx::rect2i lx::ui::damage_ledger::union_bounds() const {
    lx::rect2i out{};
    for (unsigned i = 0; i < count_; ++i)
        out = lx_ui_union(out, rects_[i]);
    return out;
}

bool lx::ui::damage_ledger::needs_paint() const {
    return (flags_ & static_cast<unsigned>(invalidation::paint)) != 0;
}
bool lx::ui::damage_ledger::needs_layout() const {
    return (flags_ & static_cast<unsigned>(invalidation::layout)) != 0;
}
bool lx::ui::damage_ledger::needs_animate() const {
    return (flags_ & static_cast<unsigned>(invalidation::animate)) != 0;
}
bool lx::ui::damage_ledger::empty() const { return count_ == 0 && flags_ == 0; }

void lx::ui::damage_ledger::clear() {
    count_ = 0;
    flags_ = 0;
}

lx::ui::damage_ledger& lx::ui::ui_damage() { return g_ui_damage; }

lx::ui::ui_node* lx::ui::root_of(ui_node& node) {
    ui_node* cursor = &node;
    while (cursor->parent() != nullptr)
        cursor = cursor->parent();
    return cursor;
}

void lx::ui::invalidate_paint(ui_node& node) { ui_damage().add(node.bounds()); }

void lx::ui::invalidate_paint(ui_node& node, lx::rect2i region) {
    lx::rect2i placed = region;
    const lx::rect2i b = node.bounds();
    placed.x += b.x;
    placed.y += b.y;
    ui_damage().add(placed);
}

void lx::ui::invalidate_layout(ui_node& node) {
    node.mark_layout_dirty();
    // Damage the pre-layout bounds now; the post-layout bounds are added by the root
    // after arrange, so a resize repaints both the vacated and the newly covered area.
    ui_damage().add(node.bounds());
    ui_damage().request(invalidation::layout);
}

void lx::ui::invalidate_animate(ui_node& node) {
    (void)node;
    ui_damage().request(invalidation::animate);
}
