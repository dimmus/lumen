module;

import lx.foundation;
import lx.runtime;
import lx.scene;
import lx.layout;

export module lx.ui:root;

import :element;
import :node;
import :reconcile;
import :invalidate;

export namespace lx::ui {

using describe_fn = child (*)(build_context& ctx);

/// Owns the retained tree and drives one UI frame: rebuild, reconcile, layout, emit.
class ui_root {
public:
    explicit ui_root(lx::runtime::memory_arena& arena);

    void set_describer(describe_fn fn);
    void set_bounds(lx::rect2i bounds);

    /// Rebuild when dirty, then measure/arrange/emit into `draws`.
    void tick(lx::scene::draw_list& draws);

    [[nodiscard]] ui_node* tree() const;
    [[nodiscard]] damage_ledger& damage();

private:
    lx::runtime::memory_arena* arena_ = nullptr;
    describe_fn describe_ = nullptr;
    ui_node* root_ = nullptr;
    reconciler reconciler_{};
    lx::rect2i bounds_{};
    bool dirty_ = true;
};

} // namespace lx::ui


lx::ui::ui_root::ui_root(lx::runtime::memory_arena& arena) : arena_{&arena} {}

void lx::ui::ui_root::set_describer(describe_fn fn) {
    describe_ = fn;
    dirty_ = true;
}

void lx::ui::ui_root::set_bounds(lx::rect2i bounds) {
    bounds_ = bounds;
    dirty_ = true;
}

lx::ui::damage_ledger& lx::ui::ui_root::damage() { return ui_damage(); }

void lx::ui::ui_root::tick(lx::scene::draw_list& draws) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!arena_ || !describe_)
        return;

    arena_->reset();
    build_context ctx{*arena_};
    build_scope scope{ctx};

    if (dirty_ || ui_damage().needs_layout()) {
        child described = describe_(ctx);
        if (!ctx.overflowed()) {
            root_ = reconciler_.apply(root_, described);
            dirty_ = false;
        }
    }

    if (!root_)
        return;

    const lx::layout::constraints constraints{{0, 0}, {bounds_.width, bounds_.height}};
    (void)root_->measure(constraints, size_hint::preferred);
    root_->arrange(bounds_);

    // The list is rebuilt from scratch each tick; appending would grow it without bound.
    draws.clear();
    draw_scope scope_out{draws, bounds_};
    root_->emit(scope_out);

    if (ui_damage().needs_paint())
        ui_damage().clear();
}

lx::ui::ui_node* lx::ui::ui_root::tree() const { return root_; }
