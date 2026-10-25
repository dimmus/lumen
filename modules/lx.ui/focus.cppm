module;

import lx.foundation;
import lx.runtime;

export module lx.ui:focus;

import std;

import :events;
import :node;
import :invalidate;

// ── Keyboard focus over the retained tree (ADR-010 §5.5, §7 Phase 3) ────────────────────
//
// One node has the focus: `node_flag::focused`, set and cleared only here. Focusable nodes
// carry `node_flag::focusable` (a control sets it in `apply`); a disabled or invisible one is
// skipped. Tab and shift-tab walk the tab sequence — tree order (pre-order, a parent before
// its children), with a positive `focus_order()` ahead of the rest in ascending order and a
// negative one left out (HTML's tabindex) — and wrap at either end. A trap (`trap`, a modal
// or popover in stage 4) confines the sequence to its subtree until it is released, then
// gives the focus back to the node that had it.
//
// Focus-visible, as GPUI decides it: the ring (`node_flag::focus_ring`) shows only while the
// focus came by keyboard — tab, an arrow key, any key handled while focused — and goes away
// when the pointer presses (focus moved by a click is focus without a ring). Programmatic
// focus keeps the modality of the last input. The ring is drawn by the node itself as a 1 px
// `border.focused` border on its own rounded box (`draw_focus_ring`, §5.5: no glow, no
// offset), separate from selection. Every change calls the node's `on_interaction_changed`
// (a control damages its bounds), so a focus move repaints exactly the two nodes.
//
// Arrow-key navigation inside a group (a tab bar, a status bar, a list or a menu) is
// `move_within` / `move_to_edge` over the group's focusable descendants, negative orders
// included (the roving-tabindex pattern: one stop in the tab sequence, arrows within).
//
// Allocation: none. Each walk is O(nodes) over the tree with no scratch; the trap stack is
// fixed at `k_max_traps` with `overflow_action::reject`. UI affinity.

export namespace lx::ui {

struct focus_style {
    bool high_contrast = false;
    /// Removes every transition: `focus_manager::set_style` sets `set_reduced_motion`.
    bool reduced_motion = false;
};

/// §5.5: `reduced_motion` removes all transitions (lx.ui:interact's `transition` jumps to its
/// end). UI affinity, process-wide.
[[nodiscard]] bool ui_reduced_motion();
void set_reduced_motion(bool reduced);

/// Where a focus change came from: decides the ring.
enum class focus_cause : std::uint8_t { keyboard, pointer, programmatic };

/// Direction for `move_within`'s callers: which arrow keys a group answers.
enum class nav_axis : std::uint8_t { none, horizontal, vertical, both };

class focus_manager final : public node_observer {
public:
    static constexpr unsigned k_max_traps = 8;

    focus_manager() = default;
    ~focus_manager() override = default;

    /// The tree the sequence walks (the root's tree; `ui_root` keeps it current).
    void set_root(ui_node* root);
    [[nodiscard]] ui_node* root() const { return root_; }
    void set_style(focus_style style);
    [[nodiscard]] const focus_style& style() const { return style_; }

    [[nodiscard]] ui_node* focused() const { return focused_; }
    /// The focused node shows its ring (focus came by keyboard).
    [[nodiscard]] bool focus_visible() const { return focused_ && ring_; }

    /// Focus `node` (focusable, enabled, visible): false otherwise. The ring shows for
    /// `keyboard`, hides for `pointer`, and follows the last input for `programmatic`. A node
    /// outside the active trap is refused.
    bool focus(ui_node& node, focus_cause cause = focus_cause::programmatic);
    /// Nothing focused.
    void blur();

    /// Tab / shift-tab: the next or previous stop of the tab sequence (inside the active
    /// trap), wrapping. With nothing focused, the first or the last. False when there is no
    /// stop at all.
    bool focus_next();
    bool focus_previous();

    /// A key event: tab / shift-tab move the focus; any other key goes to the focused node's
    /// `on_focus_key`, then each ancestor's, until one takes it. Every key press marks the
    /// keyboard as the last input (the ring shows). True when something took it.
    bool handle_key(key_event event);
    /// A pointer press on `target` (null: on nothing focusable): its nearest focusable
    /// ancestor-or-self takes the focus without a ring; nothing focusable blurs.
    void pointer_down(ui_node* target);

    /// Arrow navigation: the focus moves `delta` stops among `group`'s focusable descendants
    /// (tree order, negative orders included), wrapping when `wrap`. With the focus outside
    /// the group, the first (delta > 0) or the last stop. Keyboard cause.
    bool move_within(ui_node& group, int delta, bool wrap = true);
    /// Home / end: the first or the last stop in `group`.
    bool move_to_edge(ui_node& group, bool last);

    /// Confine the tab sequence to `scope`'s subtree and focus its first stop; `release`
    /// restores the focus the trap took. False when `k_max_traps` are active (rejected).
    bool trap(ui_node& scope);
    void release(ui_node& scope);
    [[nodiscard]] ui_node* active_trap() const;

    /// Where a magnifier or screen reader should look: called with the focused node's bounds
    /// on every focus change (null hook: nothing). The compositor's zoom follows it.
    void set_magnifier_hook(void (*hook)(void* context, lx::rect2i bounds), void* context);
    void notify_compositor_magnifier(lx::rect2i focus_bounds);

    /// Focus changes since construction (tests).
    [[nodiscard]] std::uint64_t changes() const { return changes_; }

    void node_destroyed(ui_node& node) override;

private:
    void set_focus(ui_node* node, bool ring);
    void set_ring(bool ring);
    [[nodiscard]] bool in_scope(const ui_node& node) const;
    [[nodiscard]] ui_node* step(bool forward);

    ui_node* root_ = nullptr;
    ui_node* focused_ = nullptr;
    bool ring_ = false;
    /// The last input was the keyboard: programmatic focus shows the ring.
    bool keyboard_modality_ = false;
    focus_style style_{};
    struct trap_entry {
        ui_node* scope = nullptr;
        ui_node* restore = nullptr;
    };
    trap_entry traps_[k_max_traps]{};
    unsigned trap_count_ = 0;
    void (*magnifier_)(void*, lx::rect2i) = nullptr;
    void* magnifier_context_ = nullptr;
    std::uint64_t changes_ = 0;
};

/// A node can take the focus now: focusable, not disabled, it and its ancestors visible.
[[nodiscard]] bool can_focus(const ui_node& node);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

using lx::ui::node_flag;
using lx::ui::ui_node;

[[nodiscard]] bool lx_ui_is_ancestor(const ui_node& ancestor, const ui_node* node) {
    for (const ui_node* n = node; n; n = n->parent())
        if (n == &ancestor)
            return true;
    return false;
}

/// A tab-sequence key: positive orders first, ascending; then tree order.
struct lx_ui_seq_key {
    long long order = 0;
    long long pos = 0;
    [[nodiscard]] bool operator<(const lx_ui_seq_key& o) const {
        return order != o.order ? order < o.order : pos < o.pos;
    }
};

/// One pre-order walk of `scope`: the current node's key and the wrap target (the first
/// stop going forward, the last going back).
struct lx_ui_seq_walk {
    const ui_node* current = nullptr;
    bool forward = true;
    bool all_focusable = false; ///< arrow navigation: negative orders count too
    long long pos = 0;
    bool have_current = false;
    lx_ui_seq_key current_key{};
    ui_node* edge = nullptr;
    lx_ui_seq_key edge_key{};

    void visit(ui_node& n) {
        if (!n.visible())
            return;
        const long long here = pos++;
        if (&n == current) {
            // The current node has a place in the sequence even when it is not a stop (a
            // negative order, disabled since): tab goes on from where it is.
            const int o = n.focus_order();
            have_current = true;
            current_key = {all_focusable || o <= 0 ? (1ll << 40) : o, here};
        } else if (n.has_flag(node_flag::focusable) && !n.has_flag(node_flag::disabled)) {
            const int o = n.focus_order();
            if (o >= 0 || all_focusable) {
                const lx_ui_seq_key k{all_focusable || o <= 0 ? (1ll << 40) : o, here};
                if (!edge || (forward ? k < edge_key : edge_key < k)) {
                    edge = &n;
                    edge_key = k;
                }
            }
        }
        for (unsigned i = 0; i < n.child_count(); ++i)
            if (ui_node* c = n.child_at(i))
                visit(*c);
    }
};

/// The neighbour of the node with key `cur` in `scope`: a second walk with the key known.
struct lx_ui_seq_next {
    lx_ui_seq_key cur{};
    bool forward = true;
    bool all_focusable = false;
    long long pos = 0;
    ui_node* best = nullptr;
    lx_ui_seq_key best_key{};

    void visit(ui_node& n) {
        if (!n.visible())
            return;
        const long long here = pos++;
        if (n.has_flag(node_flag::focusable) && !n.has_flag(node_flag::disabled)) {
            const int o = n.focus_order();
            if (o >= 0 || all_focusable) {
                const lx_ui_seq_key k{all_focusable || o <= 0 ? (1ll << 40) : o, here};
                const bool after = forward ? cur < k : k < cur;
                if (after && (!best || (forward ? k < best_key : best_key < k))) {
                    best = &n;
                    best_key = k;
                }
            }
        }
        for (unsigned i = 0; i < n.child_count(); ++i)
            if (ui_node* c = n.child_at(i))
                visit(*c);
    }
};

/// `node`'s neighbour in `scope`'s sequence (wrapping), or the edge when `node` is not in it.
[[nodiscard]] ui_node* lx_ui_neighbour(ui_node& scope, const ui_node* node, bool forward,
                                       bool all_focusable, bool wrap) {
    lx_ui_seq_walk w{};
    w.current = node;
    w.forward = forward;
    w.all_focusable = all_focusable;
    w.visit(scope);
    if (!w.have_current)
        return w.edge;
    lx_ui_seq_next n{};
    n.cur = w.current_key;
    n.forward = forward;
    n.all_focusable = all_focusable;
    n.visit(scope);
    if (n.best)
        return n.best;
    return wrap ? w.edge : nullptr;
}

} // namespace

bool lx::ui::can_focus(const ui_node& node) {
    if (!node.has_flag(node_flag::focusable) || node.has_flag(node_flag::disabled))
        return false;
    for (const ui_node* n = &node; n; n = n->parent())
        if (!n->visible())
            return false;
    return true;
}

void lx::ui::focus_manager::set_root(ui_node* root) { root_ = root; }

namespace {
bool g_reduced_motion = false;
} // namespace

bool lx::ui::ui_reduced_motion() { return g_reduced_motion; }
void lx::ui::set_reduced_motion(bool reduced) { g_reduced_motion = reduced; }

void lx::ui::focus_manager::set_style(focus_style style) {
    style_ = style;
    set_reduced_motion(style.reduced_motion);
}

void lx::ui::focus_manager::set_ring(bool ring) {
    if (!focused_ || ring_ == ring) {
        ring_ = ring;
        return;
    }
    ring_ = ring;
    focused_->set_flag(node_flag::focus_ring, ring);
    focused_->on_interaction_changed();
}

void lx::ui::focus_manager::set_focus(ui_node* node, bool ring) {
    if (node == focused_) {
        set_ring(ring);
        return;
    }
    if (ui_node* old = focused_) {
        old->set_flag(node_flag::focused, false);
        old->set_flag(node_flag::focus_ring, false);
        old->on_interaction_changed();
    }
    focused_ = node;
    ring_ = node && ring;
    ++changes_;
    if (node) {
        node->mark_referenced();
        node->set_flag(node_flag::focused, true);
        node->set_flag(node_flag::focus_ring, ring_);
        node->on_interaction_changed();
        notify_compositor_magnifier(node->bounds());
    }
}

bool lx::ui::focus_manager::in_scope(const ui_node& node) const {
    const ui_node* trap = active_trap();
    return !trap || lx_ui_is_ancestor(*trap, &node);
}

bool lx::ui::focus_manager::focus(ui_node& node, focus_cause cause) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!can_focus(node) || !in_scope(node))
        return false;
    bool ring = keyboard_modality_;
    if (cause == focus_cause::keyboard) {
        keyboard_modality_ = true;
        ring = true;
    } else if (cause == focus_cause::pointer) {
        keyboard_modality_ = false;
        ring = false;
    }
    set_focus(&node, ring);
    return true;
}

void lx::ui::focus_manager::blur() { set_focus(nullptr, false); }

lx::ui::ui_node* lx::ui::focus_manager::active_trap() const {
    return trap_count_ ? traps_[trap_count_ - 1].scope : nullptr;
}

lx::ui::ui_node* lx::ui::focus_manager::step(bool forward) {
    ui_node* scope = active_trap();
    if (!scope)
        scope = root_;
    if (!scope)
        return nullptr;
    const ui_node* from = focused_ && lx_ui_is_ancestor(*scope, focused_) ? focused_ : nullptr;
    return lx_ui_neighbour(*scope, from, forward, false, true);
}

bool lx::ui::focus_manager::focus_next() {
    ui_node* n = step(true);
    return n && focus(*n, focus_cause::keyboard);
}

bool lx::ui::focus_manager::focus_previous() {
    ui_node* n = step(false);
    return n && focus(*n, focus_cause::keyboard);
}

bool lx::ui::focus_manager::handle_key(key_event event) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (!event.pressed)
        return false;
    keyboard_modality_ = true;
    if (event.key == key::tab)
        return (event.modifiers & k_mod_shift) ? focus_previous() : focus_next();
    if (!focused_)
        return false;
    // A key handled while focused shows the ring (focus-visible follows the keyboard).
    set_ring(true);
    for (ui_node* n = focused_; n; n = n->parent()) {
        if (n->on_focus_key(event, *this))
            return true;
        if (!focused_)
            break; // the handler blurred (or destroyed) the focus
    }
    return false;
}

void lx::ui::focus_manager::pointer_down(ui_node* target) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    keyboard_modality_ = false;
    ui_node* n = target;
    while (n && !can_focus(*n))
        n = n->parent();
    if (n && in_scope(*n)) {
        set_focus(n, false);
        return;
    }
    // A press on nothing focusable: focus leaves (inside a trap it stays put).
    if (!active_trap())
        set_focus(nullptr, false);
    else
        set_ring(false);
}

bool lx::ui::focus_manager::move_within(ui_node& group, int delta, bool wrap) {
    if (delta == 0)
        return false;
    const bool forward = delta > 0;
    const ui_node* from = focused_ && lx_ui_is_ancestor(group, focused_) ? focused_ : nullptr;
    ui_node* target = nullptr;
    for (int i = 0, n = delta > 0 ? delta : -delta; i < n; ++i) {
        ui_node* next = lx_ui_neighbour(group, from, forward, true, wrap);
        if (!next)
            break;
        target = next;
        from = next;
    }
    return target && focus(*target, focus_cause::keyboard);
}

bool lx::ui::focus_manager::move_to_edge(ui_node& group, bool last) {
    ui_node* target = lx_ui_neighbour(group, nullptr, !last, true, true);
    return target && focus(*target, focus_cause::keyboard);
}

bool lx::ui::focus_manager::trap(ui_node& scope) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (trap_count_ == k_max_traps)
        return false; // overflow_action::reject: a ninth nested modal does not trap
    scope.mark_referenced();
    scope.set_flag(node_flag::focus_scope, true);
    traps_[trap_count_++] = {&scope, focused_};
    if (!focused_ || !lx_ui_is_ancestor(scope, focused_)) {
        if (ui_node* first = lx_ui_neighbour(scope, nullptr, true, false, true))
            set_focus(first, keyboard_modality_);
        else
            set_focus(nullptr, false);
    }
    return true;
}

void lx::ui::focus_manager::release(ui_node& scope) {
    for (unsigned i = trap_count_; i > 0; --i) {
        if (traps_[i - 1].scope != &scope)
            continue;
        ui_node* restore = traps_[i - 1].restore;
        for (unsigned j = i - 1; j + 1 < trap_count_; ++j)
            traps_[j] = traps_[j + 1];
        --trap_count_;
        scope.set_flag(node_flag::focus_scope, false);
        if (restore && can_focus(*restore) && in_scope(*restore))
            set_focus(restore, keyboard_modality_);
        else if (focused_ && lx_ui_is_ancestor(scope, focused_))
            set_focus(nullptr, false);
        return;
    }
}

void lx::ui::focus_manager::set_magnifier_hook(void (*hook)(void*, lx::rect2i), void* context) {
    magnifier_ = hook;
    magnifier_context_ = context;
}

void lx::ui::focus_manager::notify_compositor_magnifier(lx::rect2i focus_bounds) {
    if (magnifier_)
        magnifier_(magnifier_context_, focus_bounds);
}

void lx::ui::focus_manager::node_destroyed(ui_node& node) {
    if (focused_ == &node) {
        focused_ = nullptr;
        ring_ = false;
        ++changes_;
    }
    if (root_ == &node)
        root_ = nullptr;
    for (unsigned i = 0; i < trap_count_; ++i)
        if (traps_[i].restore == &node)
            traps_[i].restore = nullptr;
    for (unsigned i = trap_count_; i > 0; --i) {
        if (traps_[i - 1].scope != &node)
            continue;
        for (unsigned j = i - 1; j + 1 < trap_count_; ++j)
            traps_[j] = traps_[j + 1];
        --trap_count_;
    }
}
