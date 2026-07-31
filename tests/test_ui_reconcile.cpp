#include "lumen_test.hpp"

import lx.foundation;
import lx.runtime;
import lx.layout;
import lx.scene;
import lx.ui;

namespace {

/// A node carrying observable per-node state, so a test can tell reuse from rebuild:
/// `scroll` survives reconciliation but a freshly constructed node starts at zero.
struct row_props {
    int label = 0;
};

class row_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "row"; }

    void apply(const row_props& props) { label_ = props.label; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints, lx::ui::size_hint) const override {
        return {100, 20};
    }

    void emit(lx::ui::draw_scope& out) const override {
        out.fill(bounds(), lx::color::rgb(1.f, 0.f, 0.f));
    }

    [[nodiscard]] int label() const { return label_; }

    int scroll = 0;

private:
    int label_ = 0;
};

/// Distinct type so a type mismatch at the same position forces a rebuild.
class other_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    void apply(const row_props&) {}
};

class list_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    void apply(const row_props&) {}
};

[[nodiscard]] lx::ui::child row(int label, lx::ui::element_key key = 0) {
    return lx::ui::describe<row_node, row_props>({label}, key);
}

} // namespace

LUMEN_TEST(distinct_types_never_share_an_id) {
    // A shared id would let the reconciler cast a node to the wrong type in `apply`.
    LUMEN_CHECK(row_node::static_type() != other_node::static_type());
    LUMEN_CHECK(row_node::static_type() != list_node::static_type());
    LUMEN_CHECK(row_node::static_type() != 0u);
}

LUMEN_TEST(matching_descriptor_reuses_the_retained_node) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* first = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        first = rec.apply(nullptr, row(1));
    }
    LUMEN_CHECK(first != nullptr);
    LUMEN_CHECK(rec.stats().constructed == 1);

    // Per-node state that a rebuild must not discard.
    static_cast<row_node*>(first)->scroll = 7;

    arena.reset();
    lx::ui::ui_node* second = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        second = rec.apply(first, row(2));
    }

    LUMEN_CHECK(second == first);
    LUMEN_CHECK(rec.stats().constructed == 1);
    LUMEN_CHECK(rec.stats().reused == 1);
    LUMEN_CHECK(static_cast<row_node*>(second)->scroll == 7);
    // Props still get re-applied on a reused node.
    LUMEN_CHECK(static_cast<row_node*>(second)->label() == 2);

    delete second;
}

LUMEN_TEST(type_mismatch_rebuilds_instead_of_reusing) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* retained = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        retained = rec.apply(nullptr, row(1));
    }
    LUMEN_CHECK(retained != nullptr);

    arena.reset();
    lx::ui::ui_node* replaced = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        replaced = rec.apply(retained, lx::ui::describe<other_node, row_props>({0}));
    }

    LUMEN_CHECK(replaced != nullptr);
    LUMEN_CHECK(replaced->type() == other_node::static_type());
    LUMEN_CHECK(rec.stats().destroyed == 1);

    delete replaced;
}

LUMEN_TEST(keyed_rows_keep_state_across_reorder) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* list = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        lx::ui::child parent = lx::ui::describe<list_node, row_props>({0});
        const lx::ui::child kids[3] = {row(10, 1), row(20, 2), row(30, 3)};
        list = rec.apply(nullptr, lx::ui::with_children(parent, kids, 3));
    }
    LUMEN_CHECK(list != nullptr);
    LUMEN_CHECK(list->child_count() == 3);

    // Tag each row so we can prove identity followed the key, not the position.
    for (unsigned i = 0; i < 3; ++i)
        static_cast<row_node*>(list->child_at(i))->scroll = static_cast<int>(100 + i);

    lx::ui::ui_node* was_key2 = list->child_at(1);

    // Reverse the order; every row keeps its key.
    arena.reset();
    rec.reset_stats();
    {
        lx::ui::build_scope scope{ctx};
        lx::ui::child parent = lx::ui::describe<list_node, row_props>({0});
        const lx::ui::child kids[3] = {row(30, 3), row(20, 2), row(10, 1)};
        list = rec.apply(list, lx::ui::with_children(parent, kids, 3));
    }

    LUMEN_CHECK(list->child_count() == 3);
    // Nothing was rebuilt: reordering must not destroy and recreate rows.
    LUMEN_CHECK(rec.stats().constructed == 0);
    LUMEN_CHECK(rec.stats().destroyed == 0);

    // Key 2 stayed in the middle and kept its state; keys 1 and 3 swapped ends.
    LUMEN_CHECK(list->child_at(1) == was_key2);
    LUMEN_CHECK(static_cast<row_node*>(list->child_at(0))->scroll == 102);
    LUMEN_CHECK(static_cast<row_node*>(list->child_at(1))->scroll == 101);
    LUMEN_CHECK(static_cast<row_node*>(list->child_at(2))->scroll == 100);

    // Shrinking the list releases the rows that no longer have a descriptor.
    arena.reset();
    rec.reset_stats();
    {
        lx::ui::build_scope scope{ctx};
        lx::ui::child parent = lx::ui::describe<list_node, row_props>({0});
        const lx::ui::child kids[1] = {row(20, 2)};
        list = rec.apply(list, lx::ui::with_children(parent, kids, 1));
    }
    LUMEN_CHECK(list->child_count() == 1);
    LUMEN_CHECK(list->child_at(0) == was_key2);
    LUMEN_CHECK(rec.stats().destroyed == 2);

    delete list;
}

LUMEN_TEST(decorators_compose_without_shared_state) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::build_scope scope{ctx};

    // Two independent pipes built from the same modifiers must not interfere — the
    // failure mode of a shared fluent builder.
    lx::ui::child a = row(1) | lx::ui::insets(8) | lx::ui::center();
    lx::ui::child b = row(2) | lx::ui::insets(4);

    LUMEN_CHECK(a != nullptr);
    LUMEN_CHECK(b != nullptr);
    LUMEN_CHECK(a != b);
    LUMEN_CHECK(a->child_count == 1);
    LUMEN_CHECK(b->child_count == 1);
    // Outermost wrapper differs: `a` ends in align, `b` in insets.
    LUMEN_CHECK(a->type != b->type);
}

LUMEN_TEST(insets_and_align_place_the_child) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* tree = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        tree = rec.apply(nullptr, row(1) | lx::ui::insets(10));
    }
    LUMEN_CHECK(tree != nullptr);

    // insets adds its padding to the child's measurement.
    lx::layout::constraints c{};
    c.max = {500, 500};
    const auto measured = tree->measure(c, lx::ui::size_hint::preferred);
    LUMEN_CHECK(measured.width == 120);  // 100 + 10 left + 10 right
    LUMEN_CHECK(measured.height == 40);  // 20 + 10 top + 10 bottom

    tree->arrange({0, 0, 200, 200});
    const auto inner = tree->child_at(0)->bounds();
    LUMEN_CHECK(inner.x == 10);
    LUMEN_CHECK(inner.y == 10);
    LUMEN_CHECK(inner.width == 180);
    LUMEN_CHECK(inner.height == 180);

    delete tree;
}

LUMEN_TEST(emit_produces_draw_commands_clipped_to_bounds) {
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* tree = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        tree = rec.apply(nullptr, row(1));
    }
    LUMEN_CHECK(tree != nullptr);
    tree->arrange({0, 0, 100, 20});

    lx::scene::draw_list list{};
    lx::ui::draw_scope out{list, {0, 0, 100, 20}};
    tree->emit(out);

    // The node emitted real geometry rather than painting into a placeholder pass.
    LUMEN_CHECK(out.emitted() == 1);
    LUMEN_CHECK(list.size() == 1);

    // A node entirely outside the clip contributes nothing.
    lx::scene::draw_list empty{};
    lx::ui::draw_scope offscreen{empty, {500, 500, 10, 10}};
    tree->emit(offscreen);
    LUMEN_CHECK(offscreen.emitted() == 0);
    LUMEN_CHECK(empty.size() == 0);

    delete tree;
}

LUMEN_TEST(damage_ledger_merges_and_stays_bounded) {
    lx::ui::damage_ledger ledger{};
    LUMEN_CHECK(ledger.empty());

    // Far more rects than the fixed capacity: the ledger must merge, never grow.
    for (int i = 0; i < 200; ++i)
        ledger.add({i * 40, i * 40, 10, 10});

    LUMEN_CHECK(ledger.count() <= lx::ui::damage_ledger::k_capacity);
    LUMEN_CHECK(ledger.needs_paint());

    // Merging is conservative: the union still covers every rect that went in.
    const auto bounds = ledger.union_bounds();
    LUMEN_CHECK(bounds.x <= 0);
    LUMEN_CHECK(bounds.y <= 0);
    LUMEN_CHECK(bounds.x + bounds.width >= 199 * 40 + 10);
    LUMEN_CHECK(bounds.y + bounds.height >= 199 * 40 + 10);

    // Repeating one region does not add slots.
    lx::ui::damage_ledger repeat{};
    for (int i = 0; i < 50; ++i)
        repeat.add({5, 5, 20, 20});
    LUMEN_CHECK(repeat.count() == 1);

    // Empty regions are ignored rather than recorded as damage.
    lx::ui::damage_ledger degenerate{};
    degenerate.add({0, 0, 0, 0});
    LUMEN_CHECK(degenerate.count() == 0);

    ledger.clear();
    LUMEN_CHECK(ledger.empty());
}

LUMEN_TEST(layout_invalidation_does_not_damage_the_whole_surface) {
    // karm forces a full-window repaint on any relayout; Lumen must scope it to the
    // subtree so the compositor gets useful damage.
    lx::runtime::memory_arena arena{64 * 1024};
    lx::ui::build_context ctx{arena};
    lx::ui::reconciler rec{};

    lx::ui::ui_node* tree = nullptr;
    {
        lx::ui::build_scope scope{ctx};
        tree = rec.apply(nullptr, row(1));
    }
    tree->arrange({30, 40, 100, 20});

    lx::ui::ui_damage().clear();
    lx::ui::invalidate_layout(*tree);

    LUMEN_CHECK(lx::ui::ui_damage().needs_layout());
    LUMEN_CHECK(tree->layout_dirty());
    const auto damaged = lx::ui::ui_damage().union_bounds();
    LUMEN_CHECK(damaged.x == 30);
    LUMEN_CHECK(damaged.y == 40);
    LUMEN_CHECK(damaged.width == 100);
    LUMEN_CHECK(damaged.height == 20);

    lx::ui::ui_damage().clear();
    delete tree;
}

LUMEN_TEST(arena_exhaustion_is_reported_not_ignored) {
    // A tiny arena cannot hold the descriptor; the context must say so instead of
    // silently returning a truncated tree.
    lx::runtime::memory_arena tiny{8};
    lx::ui::build_context ctx{tiny};
    lx::ui::build_scope scope{ctx};

    lx::ui::child described = row(1);
    LUMEN_CHECK(described == nullptr);
    LUMEN_CHECK(ctx.overflowed());
}

int main(int argc, char** argv) {
    // Rebuilds assert UI affinity; this test drives them directly on the main thread.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
