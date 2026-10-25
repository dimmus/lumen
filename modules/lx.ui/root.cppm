module;

import lx.foundation;
import lx.runtime;
import lx.scene;
import lx.layout;
import lx.trace;

export module lx.ui:root;

import std;

import :element;
import :events;
import :focus;
import :interact;
import :node;
import :reconcile;
import :reducer;
import :invalidate;
import :theme;

export namespace lx::ui {

using describe_fn = child (*)(build_context& ctx);

/// Owns the retained tree and drives one UI frame: rebuild, reconcile, layout, emit.
class ui_root {
public:
    explicit ui_root(lx::runtime::memory_arena& arena);

    void set_describer(describe_fn fn);
    void set_bounds(lx::rect2i bounds);

    /// Rebuild when dirty, then measure/arrange/emit into `draws`. True when a frame was
    /// produced. False — with `draws` untouched, no describe, no layout — when nothing asked
    /// for one: a tree already built, no describer or bounds change, an empty damage ledger
    /// (no paint, layout or animate request). ADR-010 §4.9's idle rule at the root.
    ///
    /// A produced frame consumes the ledger as it stood when the tick began, plus what the
    /// describe, reconcile and layout raise (a node the reconcile removed): `frame_damage`.
    /// What `emit` raises — an animation asking for its next frame — stays in the ledger for
    /// the next tick. A client whose commit fails puts `frame_damage` back (§4.9: the
    /// ledger is cleared only by a successful commit).
    bool tick(lx::scene::draw_list& draws);

    [[nodiscard]] ui_node* tree() const;
    [[nodiscard]] damage_ledger& damage();

    // ── Input (ADR-010 §4.9; Phase 4's window routes wl_pointer / wl_keyboard here) ──

    /// The tree's keyboard focus (tab order, traps, focus-visible).
    [[nodiscard]] focus_manager& focus() { return focus_; }
    /// The hovered and pressed nodes.
    [[nodiscard]] pointer_state& pointer() { return pointer_; }
    /// The pointer moved to `at` (surface coordinates): hover follows. True when the
    /// hovered node changed (it damaged itself).
    bool pointer_move(lx::point2i at);
    /// A button changed at the pointer's position: press, focus, activation on release.
    /// Falls back to the tree's `on_mouse` when no interactive node is there.
    bool pointer_button(mouse_button b, bool pressed);
    void pointer_leave();
    /// A key: tab moves the focus, others go to the focused node and its ancestors, then
    /// to the tree's `on_key`.
    bool key(key_event event);

    /// When the event loop must wake the UI next without input, CLOCK_MONOTONIC ns (a timer
    /// such as a tooltip's hover delay); 0: nothing pending. An animation asks for frames
    /// through the ledger instead.
    [[nodiscard]] std::uint64_t next_deadline_ns() const;
    /// The damage the last frame `tick` produced repaints (see `tick`).
    [[nodiscard]] const damage_ledger& frame_damage() const { return frame_damage_; }

    /// `CLOCK_MONOTONIC` nanoseconds each stage of the last frame `tick` produced took:
    /// what `lumen-ui-bench` attributes per stage (ADR-010 §8.1; the spans carry the same
    /// to a trace). `described` is false when the frame reused the tree without a
    /// describe, and `describe` / `reconcile` are then zero.
    struct stage_times {
        std::uint64_t describe = 0;
        std::uint64_t reconcile = 0;
        std::uint64_t layout = 0;
        std::uint64_t emit = 0;
        bool described = false;
        /// Reducers whose subtree this frame described again (only theirs, when the tree
        /// itself was not).
        unsigned reducers_rebuilt = 0;
    };
    [[nodiscard]] stage_times last_stage_times() const;

private:
    lx::runtime::memory_arena* arena_ = nullptr;
    describe_fn describe_ = nullptr;
    ui_node* root_ = nullptr;
    reconciler reconciler_{};
    lx::rect2i bounds_{};
    stage_times stage_times_{};
    damage_ledger frame_damage_{};
    focus_manager focus_{};
    pointer_state pointer_{};
    /// The `theme_generation` the tree was last resolved under.
    std::uint64_t theme_generation_ = 0;
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

lx::ui::ui_root::stage_times lx::ui::ui_root::last_stage_times() const { return stage_times_; }

namespace {

/// Adds `from`'s rects and requests to `into`.
void lx_ui_fold(lx::ui::damage_ledger& into, const lx::ui::damage_ledger& from) {
    for (unsigned i = 0; i < from.count(); ++i)
        into.add(from.rects()[i]);
    if (from.needs_paint())
        into.request(lx::ui::invalidation::paint);
    if (from.needs_layout())
        into.request(lx::ui::invalidation::layout);
    if (from.needs_animate())
        into.request(lx::ui::invalidation::animate);
    if (from.needs_rebuild())
        into.request(lx::ui::invalidation::rebuild);
}

/// Latches the frame clock for one tick (`ui_now_ns`), released on every return path.
struct lx_ui_frame_scope {
    lx_ui_frame_scope() { lx::ui::begin_ui_frame(); }
    ~lx_ui_frame_scope() { lx::ui::end_ui_frame(); }
    lx_ui_frame_scope(const lx_ui_frame_scope&) = delete;
    lx_ui_frame_scope& operator=(const lx_ui_frame_scope&) = delete;
};

/// Every node's layout dirty and its measure cache cleared.
void lx_ui_mark_tree_dirty(lx::ui::ui_node& node) {
    node.mark_layout_dirty();
    for (unsigned i = 0; i < node.child_count(); ++i)
        if (lx::ui::ui_node* c = node.child_at(i))
            lx_ui_mark_tree_dirty(*c);
}

[[nodiscard]] std::uint64_t lx_ui_ns_between(std::chrono::steady_clock::time_point from,
                                             std::chrono::steady_clock::time_point to) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

} // namespace

bool lx::ui::ui_root::tick(lx::scene::draw_list& draws) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!arena_ || !describe_)
        return false;

    const lx_ui_frame_scope frame_clock{};
    // Timers due by this frame's time run first: what they invalidate is this frame's.
    (void)ui_timers().fire_due(ui_now_ns());

    damage_ledger& ledger = ui_damage();
    // A new theme or density changes what every styled node resolves to: describe again
    // (each node re-resolves in `apply`), and lay out and repaint everything.
    if (const std::uint64_t generation = theme_generation(); generation != theme_generation_) {
        theme_generation_ = generation;
        dirty_ = true;
        if (root_) {
            lx_ui_mark_tree_dirty(*root_);
            ledger.add(bounds_);
            ledger.request(invalidation::paint);
        }
    }
    if (!dirty_ && root_ && ledger.empty() && !root_->layout_dirty())
        return false;

    // Take what this frame answers now, so a request raised while it is built is not lost.
    // Fixed-size copies: no allocation.
    const bool needs_layout = ledger.needs_layout();
    frame_damage_ = ledger;
    ledger.clear();

    arena_->reset();
    build_context ctx{*arena_};
    build_scope scope{ctx};

    using clock = std::chrono::steady_clock;
    stage_times_ = {};
    auto stage_start = clock::now();

    {
        lx::trace::scoped_span span{"ui", "ui.describe"};
        if (dirty_ || needs_layout) {
            child described = describe_(ctx);
            const auto described_at = clock::now();
            stage_times_.describe = lx_ui_ns_between(stage_start, described_at);
            stage_times_.described = true;
            if (!ctx.overflowed()) {
                root_ = reconciler_.apply(root_, described);
                dirty_ = false;
            }
            stage_start = clock::now();
            stage_times_.reconcile = lx_ui_ns_between(described_at, stage_start);
        }
    }
    if (reducers_pending()) {
        // Reducers that dispatched (or that this describe constructed) describe their own
        // subtree again — only theirs, the reconcile reusing every unchanged node.
        lx::trace::scoped_span span{"ui", "ui.reducers"};
        const auto rebuild_start = clock::now();
        stage_times_.reducers_rebuilt = rebuild_pending_reducers(ctx, reconciler_);
        if (reducers_pending())
            ledger.request(invalidation::rebuild); // past the per-frame cap: next frame
        stage_start = clock::now();
        stage_times_.reconcile += lx_ui_ns_between(rebuild_start, stage_start);
    }
    focus_.set_root(root_);

    if (!root_) {
        // No frame: whatever was asked is still owed.
        lx_ui_fold(ledger, frame_damage_);
        return false;
    }

    {
        lx::trace::scoped_span span{"ui", "ui.layout"};
        // The root is placed at the bounds: only the dirty path is measured and arranged
        // again (ADR-010 §4.7); a clean root at the same bounds is not walked at all.
        root_->place(bounds_);
    }
    {
        const auto now = clock::now();
        stage_times_.layout = lx_ui_ns_between(stage_start, now);
        stage_start = now;
    }

    // Raised before the emit (a removed node's bounds): this frame paints it.
    lx_ui_fold(frame_damage_, ledger);
    ledger.clear();

    {
        lx::trace::scoped_span span{"ui", "ui.emit"};
        // The list is rebuilt from scratch each tick; appending would grow it without
        // bound. The overlap grid covers the pixels the tree can reach.
        draws.begin({bounds_.x + bounds_.width, bounds_.y + bounds_.height});
        draw_scope scope_out{draws, bounds_};
        root_->emit(scope_out);
    }
    stage_times_.emit = lx_ui_ns_between(stage_start, clock::now());
    if (lx::trace::timeline_enabled())
        lx::trace::timeline_counter("ui.primitives", draws.primitive_count());

    // The ledger now holds only what the emit raised (an animation asking for its next
    // frame): the next tick's.
    return true;
}

lx::ui::ui_node* lx::ui::ui_root::tree() const { return root_; }

bool lx::ui::ui_root::pointer_move(lx::point2i at) {
    return pointer_.move(root_, at);
}

bool lx::ui::ui_root::pointer_button(mouse_button b, bool pressed) {
    if (pointer_.button(root_, b, pressed, &focus_))
        return true;
    if (!root_)
        return false;
    mouse_event e{};
    e.position = pointer_.position();
    e.button = b;
    e.pressed = pressed;
    return root_->on_mouse(e);
}

void lx::ui::ui_root::pointer_leave() { pointer_.leave(); }

bool lx::ui::ui_root::key(key_event event) {
    focus_.set_root(root_);
    if (focus_.handle_key(event))
        return true;
    return root_ && root_->on_key(event);
}

std::uint64_t lx::ui::ui_root::next_deadline_ns() const { return ui_timers().next_deadline(); }
