module;

import lx.foundation;
import lx.trace;

export module lx.ui:reconcile;

import :element;
import :node;
import :invalidate;

export namespace lx::ui {

struct reconcile_stats {
    unsigned reused = 0;
    unsigned constructed = 0;
    unsigned destroyed = 0;
};

/// Folds an ephemeral descriptor tree onto a retained node tree.
///
/// A retained node is reused when its type id and key both match the descriptor, in
/// which case only props are re-applied and per-node state (scroll offset, caret,
/// animation phase) survives. Any other outcome destroys the retained subtree and
/// constructs a fresh one, so retained-node allocation happens on tree *shape* change
/// only — never per frame.
class reconciler {
public:
    /// Reconcile `desired` into the slot currently holding `retained`. Returns the node
    /// that should occupy the slot; the caller must store it. Ownership of a replaced
    /// node is consumed here.
    [[nodiscard]] ui_node* apply(ui_node* retained, child desired);

    [[nodiscard]] reconcile_stats stats() const;
    void reset_stats();

private:
    [[nodiscard]] bool matches(const ui_node& retained, const element& desired) const;
    void reconcile_children(ui_node& parent, const element& desired);

    reconcile_stats stats_{};
};

} // namespace lx::ui


bool lx::ui::reconciler::matches(const ui_node& retained, const element& desired) const {
    return retained.type() == desired.type && retained.key() == desired.key;
}

lx::ui::ui_node* lx::ui::reconciler::apply(ui_node* retained, child desired) {
    if (!desired) {
        // No description means the slot is gone.
        if (retained) {
            ++stats_.destroyed;
            delete retained;
        }
        return nullptr;
    }

    if (retained && matches(*retained, *desired)) {
        if (desired->apply)
            desired->apply(*retained, desired->props);
        reconcile_children(*retained, *desired);
        ++stats_.reused;
        return retained;
    }

    if (retained) {
        ++stats_.destroyed;
        // The vacated area must repaint even though the node is going away.
        invalidate_paint(*retained);
        delete retained;
    }

    if (!desired->construct) {
        lx::trace::logger::global().log(lx::trace::level::warn, "ui",
                                        "descriptor without a construct hook");
        return nullptr;
    }

    ui_node* fresh = desired->construct();
    if (!fresh)
        return nullptr;
    fresh->set_key(desired->key);
    if (desired->apply)
        desired->apply(*fresh, desired->props);
    reconcile_children(*fresh, *desired);
    fresh->mark_layout_dirty();
    ++stats_.constructed;
    return fresh;
}

void lx::ui::reconciler::reconcile_children(ui_node& parent, const element& desired) {
    const unsigned wanted =
        desired.child_count < ui_node::k_max_children ? desired.child_count
                                                      : ui_node::k_max_children;

    // Take custody of the retained children first. Rearranging them in place risks
    // freeing a node that a keyed descriptor is about to claim.
    ui_node* pool[ui_node::k_max_children]{};
    const unsigned pooled = parent.detach_children(pool, ui_node::k_max_children);

    for (unsigned i = 0; i < wanted; ++i) {
        child want = desired.children ? desired.children[i] : nullptr;
        ui_node* claimed = nullptr;

        if (want && want->key != 0) {
            // Keyed: find this identity anywhere, so a reordered or filtered list
            // keeps its rows instead of rebuilding them all.
            for (unsigned j = 0; j < pooled; ++j) {
                if (pool[j] && pool[j]->key() == want->key && pool[j]->type() == want->type) {
                    claimed = pool[j];
                    pool[j] = nullptr;
                    break;
                }
            }
        } else if (want && i < pooled && pool[i] && pool[i]->key() == 0) {
            // Unkeyed: match by sibling position.
            claimed = pool[i];
            pool[i] = nullptr;
        }

        parent.place_child(i, apply(claimed, want));
    }

    parent.set_child_count(wanted);

    // Whatever no descriptor claimed has left the tree.
    for (unsigned j = 0; j < pooled; ++j) {
        if (!pool[j])
            continue;
        invalidate_paint(*pool[j]);
        ++stats_.destroyed;
        delete pool[j];
    }
}

lx::ui::reconcile_stats lx::ui::reconciler::stats() const { return stats_; }
void lx::ui::reconciler::reset_stats() { stats_ = {}; }
