module;

import lx.foundation;
import lx.layout;
import lx.runtime;

export module lx.ui:reducer;

import std;

import :element;
import :node;
import :reconcile;
import :invalidate;

export namespace lx::ui {

/// The non-template half of `reducer_node`: a node whose subtree is described by its own
/// build function from its own state, and rebuilt by `ui_root` on the frame after a dispatch
/// — only that subtree (ADR-010 §4.9: action → reducer → describe the reducer's subtree →
/// reconcile, which reuses every unchanged node).
///
/// The reconciler leaves a reducer's children alone (`node_flag::opaque_children`): an
/// enclosing describe that reaches the reducer re-applies its props and nothing below, so
/// the subtree and its state survive any rebuild of the tree around it. A reducer with a
/// rebuild owed sits in the UI thread's pending list (intrusive: no allocation, no cap);
/// `rebuild_pending_reducers` drains it, including reducers a rebuild constructs.
class reducer_host : public ui_node {
public:
    ~reducer_host() override;

    /// Describe and reconcile the subtree again from the current state.
    virtual void rebuild(build_context& ctx, reconciler& rec) = 0;

    /// Queue this host for `rebuild_pending_reducers` and ask the root for a frame
    /// (`invalidation::rebuild`). Idempotent until the rebuild runs.
    void request_rebuild();
    [[nodiscard]] bool rebuild_pending() const { return pending_; }

    /// Rebuilds since construction (tests: a dispatch re-describes exactly its reducer).
    [[nodiscard]] std::uint64_t rebuilds() const { return rebuilds_; }

protected:
    reducer_host();
    std::uint64_t rebuilds_ = 0;

private:
    friend unsigned rebuild_pending_reducers(build_context& ctx, reconciler& rec);
    void unlink();

    reducer_host* prev_pending_ = nullptr;
    reducer_host* next_pending_ = nullptr;
    bool pending_ = false;
};

/// Rebuilds every reducer that asked since the last call, and every reducer those rebuilds
/// construct, in request order. UI affinity, inside the frame's build scope (`ui_root::tick`
/// calls it after the describe). Returns how many rebuilt.
unsigned rebuild_pending_reducers(build_context& ctx, reconciler& rec);

/// Whether any reducer is waiting for a rebuild.
[[nodiscard]] bool reducers_pending();

/// Host for a piece of screen state.
///
/// State lives here; the retained tree below is derived from it. A dispatched action
/// mutates state, the build function re-describes the subtree on the next UI frame, and the
/// reconciler folds the description onto the existing nodes — so the damage is the nodes
/// whose props changed. Nothing outside holds a node pointer, which is what keeps "UI is a
/// function of state" true instead of aspirational.
template<typename State, typename Action>
class reducer_node final : public reducer_host {
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

    /// False: nothing layout reads changed here (the subtree marks itself when rebuilt).
    bool apply(const props& p) {
        reduce_ = p.reduce;
        // The initial state seeds the first mount only — a rebuild must not stomp
        // state the user has since changed.
        if (!seeded_) {
            state_ = p.initial;
            seeded_ = true;
            request_rebuild();
        }
        if (build_ != p.build) {
            build_ = p.build;
            request_rebuild();
        }
        return false;
    }

    /// Apply an action; the subtree is described again on the next UI frame.
    void dispatch(const Action& action) {
        if (!reduce_)
            return;
        reduce_(state_, action);
        request_rebuild();
    }

    [[nodiscard]] const State& state() const { return state_; }

    void rebuild(build_context& ctx, reconciler& rec) override {
        if (!build_)
            return;
        build_scope scope{ctx};
        child described = build_(state_, *this);
        if (ctx.overflowed())
            return; // keep the previous tree rather than reconcile a truncated one
        ++rebuilds_;
        ui_node* const previous = child_at(0);
        const bool reused = previous && described && previous->type() == described->type &&
                            previous->key() == described->key;
        ui_node* settled = rec.apply(previous, described);
        place_child(0, settled);
        set_child_count(settled ? 1u : 0u);
        if (!reused && (previous || settled))
            mark_layout_dirty();
    }

    /// The rebuild, when one is owed (what the root does through the pending list).
    void rebuild_if_dirty(build_context& ctx, reconciler& rec) {
        if (rebuild_pending())
            rebuild(ctx, rec);
    }

    [[nodiscard]] bool dirty() const { return rebuild_pending(); }

    /// A reducer's size is its subtree's.
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override {
        const ui_node* c = child_at(0);
        return c ? c->flex_item_style() : nullptr;
    }
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override {
        const ui_node* content = child_at(0);
        return content ? content->measure_cached(c, hint, sizing_mode::content) : c.min;
    }

private:
    State state_{};
    reduce_fn reduce_ = nullptr;
    build_fn build_ = nullptr;
    bool seeded_ = false;
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

namespace {
/// The UI thread's reducers owing a rebuild, oldest first (UI affinity).
lx::ui::reducer_host* g_pending_head = nullptr;
lx::ui::reducer_host* g_pending_tail = nullptr;
} // namespace

lx::ui::reducer_host::reducer_host() { set_flag(node_flag::opaque_children, true); }

lx::ui::reducer_host::~reducer_host() { unlink(); }

void lx::ui::reducer_host::unlink() {
    if (!pending_)
        return;
    if (prev_pending_)
        prev_pending_->next_pending_ = next_pending_;
    else
        g_pending_head = next_pending_;
    if (next_pending_)
        next_pending_->prev_pending_ = prev_pending_;
    else
        g_pending_tail = prev_pending_;
    prev_pending_ = next_pending_ = nullptr;
    pending_ = false;
}

void lx::ui::reducer_host::request_rebuild() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!pending_) {
        pending_ = true;
        prev_pending_ = g_pending_tail;
        next_pending_ = nullptr;
        if (g_pending_tail)
            g_pending_tail->next_pending_ = this;
        else
            g_pending_head = this;
        g_pending_tail = this;
    }
    ui_damage().request(invalidation::rebuild);
}

unsigned lx::ui::rebuild_pending_reducers(build_context& ctx, reconciler& rec) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    unsigned rebuilt = 0;
    // A rebuild may construct reducers (they queue themselves at the tail) or destroy queued
    // ones (they unlink): take the head each time. A build that dispatches into its own
    // reducer would never drain; past the cap the rest wait for the next frame.
    constexpr unsigned k_max_rebuilds_per_frame = 4096;
    while (reducer_host* h = g_pending_head) {
        if (rebuilt == k_max_rebuilds_per_frame)
            break;
        h->unlink();
        h->rebuild(ctx, rec);
        ++rebuilt;
    }
    return rebuilt;
}

bool lx::ui::reducers_pending() { return g_pending_head != nullptr; }
