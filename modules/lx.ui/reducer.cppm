module;

import lx.foundation;
import lx.layout;

export module lx.ui:reducer;

import :element;
import :node;
import :reconcile;
import :invalidate;

export namespace lx::ui {

/// Host for a piece of screen state.
///
/// State lives here; the retained tree below is derived from it. A dispatched action
/// mutates state, the build function re-describes the subtree, and the reconciler folds
/// the description onto the existing nodes. Nothing outside holds a node pointer, which
/// is what keeps "UI is a function of state" true instead of aspirational.
template<typename State, typename Action>
class reducer_node final : public ui_node {
public:
    using reduce_fn = void (*)(State& state, const Action& action);
    using build_fn = child (*)(const State& state, reducer_node& host);

    [[nodiscard]] static element_type_id static_type() {
        // One id per (State, Action) instantiation, assigned on first use.
        static const element_type_id id = next_element_type_id();
        return id;
    }

    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "reducer"; }

    struct props {
        State initial{};
        reduce_fn reduce = nullptr;
        build_fn build = nullptr;
    };

    void apply(const props& p) {
        reduce_ = p.reduce;
        // The initial state seeds the first mount only — a rebuild must not stomp
        // state the user has since changed.
        if (!seeded_) {
            state_ = p.initial;
            seeded_ = true;
        }
        if (build_ != p.build) {
            build_ = p.build;
            dirty_ = true;
        }
    }

    /// Apply an action and mark the subtree for rebuild on the next UI tick.
    void dispatch(const Action& action) {
        if (!reduce_)
            return;
        reduce_(state_, action);
        dirty_ = true;
        invalidate_layout(*this);
    }

    [[nodiscard]] const State& state() const { return state_; }

    /// Re-describe and reconcile when state changed. Called by the root before layout.
    void rebuild_if_dirty(build_context& ctx, reconciler& rec) {
        if (!dirty_ || !build_)
            return;
        build_scope scope{ctx};
        child described = build_(state_, *this);
        if (ctx.overflowed())
            return; // keep the previous tree rather than reconcile a truncated one
        ui_node* settled = rec.apply(child_at(0), described);
        place_child(0, settled);
        set_child_count(settled ? 1u : 0u);
        dirty_ = false;
    }

    [[nodiscard]] bool dirty() const { return dirty_; }

private:
    State state_{};
    reduce_fn reduce_ = nullptr;
    build_fn build_ = nullptr;
    bool seeded_ = false;
    bool dirty_ = true;
};

/// Describe a reducer-hosted subtree.
template<typename State, typename Action>
[[nodiscard]] child reducer(State initial,
                            typename reducer_node<State, Action>::reduce_fn reduce,
                            typename reducer_node<State, Action>::build_fn build,
                            element_key key = 0) {
    using host = reducer_node<State, Action>;
    return describe<host, typename host::props>({initial, reduce, build}, key);
}

/// Bind an action to a plain callback signature. Copying the action by value keeps the
/// binding trivially copyable, so no allocation happens per event handler per frame.
template<typename State, typename Action>
struct action_binding {
    reducer_node<State, Action>* host = nullptr;
    Action action{};

    void operator()() const {
        if (host)
            host->dispatch(action);
    }
};

template<typename State, typename Action>
[[nodiscard]] action_binding<State, Action> bind(reducer_node<State, Action>& host,
                                                 Action action) {
    return {&host, action};
}

} // namespace lx::ui
