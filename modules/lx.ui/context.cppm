module;

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.ui:context;

import std;

import :interact;
import :invalidate;
import :node;
import :overlay;
import :reducer;

// ── Per-root state: one ui_context per ui_root (ADR-010 §7 Phase 5) ─────────────────────
//
// A tree's damage ledger, its open overlays, its node timers and its reducers waiting for a
// rebuild belong to the root that draws it, so several roots on one UI thread (a shell's
// panel, launcher and switcher; an app's windows) each wake, draw and hit-test only for
// what is theirs. Everything else stays thread-wide on purpose: the text and icon caches
// (`ui_resources`), the device scale set per tick, the frame clock, the percent base, the
// layout counters and the text-input statics.
//
// Routing, cheapest first:
// - Inside a root's entry point (`tick`, `wants_frame`, the pointer, key and drop entries,
//   timer firing) a `context_scope` names the root's context; `ui_damage()`, `ui_overlays()`
//   and `ui_timers()` return it. That is every frame-path invalidation: one thread-local load.
// - Outside any scope (a task posted to the UI strand, an icon landing, app code), a node's
//   invalidation walks `ui_node::parent()` to its tree's root and looks that up in the
//   thread's root table (`k_max_roots` entries; `overflow_action::warn_and_drop`).
// - A node in no registered tree (detached, or under a root the full table could not
//   take) falls back to the thread-default context while that context is in use (a root
//   built without one) or no root lives on the thread (a bare tree in a test or a tool), so
//   single-root users see what they always did. Otherwise its damage is dropped and counted
//   in `layout_counters::damage_dropped`: no root draws it.
//
// UI affinity; nothing here allocates after the thread-default context's first use.

export namespace lx::ui {

class ui_context;
/// Whether `ctx` is a live context of this thread with this `serial`.
[[nodiscard]] bool context_alive(const ui_context* ctx, std::uint64_t serial);

/// One root's damage ledger, overlay layer, timers and reducer queue. Owned by whoever owns
/// the root (the root borrows it) and must outlive it. Roots sharing a context share all of
/// it (the thread default serves every root built without one, as one tree per thread did).
/// Destroying it drops its queued reducers and its table entries. About 1.4 KiB, no heap.
class ui_context {
public:
    ui_context();
    ~ui_context();
    ui_context(const ui_context&) = delete;
    ui_context& operator=(const ui_context&) = delete;

    [[nodiscard]] damage_ledger& damage() { return damage_; }
    [[nodiscard]] const damage_ledger& damage() const { return damage_; }
    [[nodiscard]] overlay_layer& overlays() { return overlays_; }
    [[nodiscard]] timer_queue& timers() { return timers_; }
    [[nodiscard]] const timer_queue& timers() const { return timers_; }
    /// The reducers of this context waiting for a rebuild (lx.ui internal).
    [[nodiscard]] detail::reducer_queue& reducers() { return reducers_; }
    /// Whether a reducer of this context waits for a rebuild.
    [[nodiscard]] bool reducers_pending() const { return reducers_.head != nullptr; }
    /// Unique per thread for the thread's lifetime: tells a live context from a dead one
    /// reusing its address (an icon landing after its asker went).
    [[nodiscard]] std::uint64_t serial() const { return serial_; }

private:
    friend bool context_alive(const ui_context* ctx, std::uint64_t serial);

    damage_ledger damage_{};
    overlay_layer overlays_{};
    timer_queue timers_{};
    detail::reducer_queue reducers_{};
    std::uint64_t serial_ = 0;
    ui_context* prev_live_ = nullptr;
    ui_context* next_live_ = nullptr;
};

/// Names `ctx` as the current context for its lifetime, restoring the previous one (scopes
/// nest: a root ticked from another root's handler).
class context_scope {
public:
    explicit context_scope(ui_context& ctx);
    ~context_scope();
    context_scope(const context_scope&) = delete;
    context_scope& operator=(const context_scope&) = delete;

private:
    ui_context* saved_ = nullptr;
};

/// The UI thread's context for roots built without one, and for trees with no root.
/// Created on first use, never destroyed (nodes may outlive any static).
[[nodiscard]] ui_context& thread_default_context();
/// The innermost `context_scope`'s context, else the thread default.
[[nodiscard]] ui_context& active_context();
/// Where `node`'s invalidations go: the current scope's context; else its tree's registered
/// root's; else the thread default while that is in use or no root lives; else null.
[[nodiscard]] ui_context* context_of(const ui_node& node);

} // namespace lx::ui

namespace lx::ui::detail {

/// Roots the thread table holds at most; past that a root is not registered (logged once,
/// counted): its out-of-scope invalidations follow the detached-node rule.
inline constexpr unsigned k_max_roots = 32;

/// A root came (`owner` is the ui_root), with its context. False when the table is full.
bool register_root(const void* owner, ui_context& ctx);
/// The root's tree is now `tree` (null: none yet).
void set_root_tree(const void* owner, const ui_node* tree);
/// The root `owner` (on `ctx`) went.
void unregister_root(const void* owner, const ui_context& ctx);
/// Roots the table could not take since start-up.
[[nodiscard]] std::uint64_t roots_rejected();

} // namespace lx::ui::detail

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

constexpr lx::trace::category log_ui_context{"ui"};

struct lx_ui_root_entry {
    const void* owner = nullptr;
    const lx::ui::ui_node* tree = nullptr;
    lx::ui::ui_context* ctx = nullptr;
};

/// The innermost scope's context (null outside every scope).
constinit thread_local lx::ui::ui_context* t_current = nullptr;
constinit thread_local lx::ui::ui_context* t_default = nullptr;
/// Live contexts of this thread (intrusive) and the serial the next one takes.
constinit thread_local lx::ui::ui_context* t_live = nullptr;
constinit thread_local std::uint64_t t_next_serial = 1;
/// The root table, and how many live roots use the thread default (registered or not).
constinit thread_local lx_ui_root_entry t_roots[lx::ui::detail::k_max_roots]{};
constinit thread_local unsigned t_root_count = 0;
constinit thread_local unsigned t_live_roots = 0;
constinit thread_local unsigned t_default_roots = 0;
constinit thread_local std::uint64_t t_roots_rejected = 0;

/// What a node in no registered tree falls back to (see the partition comment).
[[nodiscard]] lx::ui::ui_context* lx_ui_detached_context() {
    if (t_default_roots > 0 || t_live_roots == 0)
        return &lx::ui::thread_default_context();
    return nullptr;
}

} // namespace

lx::ui::ui_context::ui_context() : serial_{t_next_serial++} {
    reducers_.ledger = &damage_;
    next_live_ = t_live;
    if (t_live)
        t_live->prev_live_ = this;
    t_live = this;
}

lx::ui::ui_context::~ui_context() {
    detail::drain_reducers(reducers_);
    if (t_current == this)
        t_current = nullptr;
    // Roots still naming it lose their entry (their owner destroyed the context first).
    for (unsigned i = 0; i < t_root_count;) {
        if (t_roots[i].ctx == this)
            t_roots[i] = t_roots[--t_root_count];
        else
            ++i;
    }
    if (prev_live_)
        prev_live_->next_live_ = next_live_;
    else if (t_live == this)
        t_live = next_live_;
    if (next_live_)
        next_live_->prev_live_ = prev_live_;
}

lx::ui::context_scope::context_scope(ui_context& ctx) : saved_{t_current} { t_current = &ctx; }

lx::ui::context_scope::~context_scope() { t_current = saved_; }

lx::ui::ui_context& lx::ui::thread_default_context() {
    if (!t_default)
        t_default = new ui_context{}; // never destroyed: nodes may outlive any static
    return *t_default;
}

lx::ui::ui_context& lx::ui::active_context() {
    return t_current ? *t_current : thread_default_context();
}

lx::ui::ui_context* lx::ui::context_of(const ui_node& node) {
    if (t_current)
        return t_current;
    const ui_node* top = &node;
    while (const ui_node* p = top->parent())
        top = p;
    for (unsigned i = 0; i < t_root_count; ++i)
        if (t_roots[i].tree == top)
            return t_roots[i].ctx;
    return lx_ui_detached_context();
}

bool lx::ui::context_alive(const ui_context* ctx, std::uint64_t serial) {
    for (const ui_context* c = t_live; c; c = c->next_live_)
        if (c == ctx)
            return c->serial_ == serial;
    return false;
}

bool lx::ui::detail::register_root(const void* owner, ui_context& ctx) {
    ++t_live_roots;
    if (&ctx == t_default)
        ++t_default_roots;
    if (t_root_count == k_max_roots) {
        // overflow_action::warn_and_drop: the root works, but what reaches its nodes from
        // outside a scope follows the detached-node rule.
        if (t_roots_rejected++ == 0)
            log_ui_context.warn("ui: root table full ({} roots); out-of-scope damage of later roots "
                                "is not routed",
                                k_max_roots);
        return false;
    }
    t_roots[t_root_count++] = {owner, nullptr, &ctx};
    return true;
}

void lx::ui::detail::set_root_tree(const void* owner, const ui_node* tree) {
    for (unsigned i = 0; i < t_root_count; ++i)
        if (t_roots[i].owner == owner) {
            t_roots[i].tree = tree;
            return;
        }
}

void lx::ui::detail::unregister_root(const void* owner, const ui_context& ctx) {
    for (unsigned i = 0; i < t_root_count; ++i)
        if (t_roots[i].owner == owner) {
            t_roots[i] = t_roots[--t_root_count];
            break;
        }
    if (t_live_roots)
        --t_live_roots;
    if (&ctx == t_default && t_default_roots)
        --t_default_roots;
}

std::uint64_t lx::ui::detail::roots_rejected() { return t_roots_rejected; }

// ── the accessors the lower partitions declare ──

lx::ui::damage_ledger& lx::ui::ui_damage() { return active_context().damage(); }

void lx::ui::detail::damage_rect(lx::rect2i region) { active_context().damage().add(region); }

lx::ui::damage_ledger* lx::ui::detail::damage_of(const ui_node& node) {
    if (ui_context* c = context_of(node))
        return &c->damage();
    ++ui_layout_counters().damage_dropped;
    return nullptr;
}

void lx::ui::detail::damage_rect_of(const ui_node& node, lx::rect2i region) {
    if (damage_ledger* d = damage_of(node))
        d->add(region);
}

lx::ui::timer_queue& lx::ui::ui_timers() { return active_context().timers(); }

lx::ui::timer_queue& lx::ui::detail::timers_of(const ui_node& node) {
    ui_context* c = context_of(node);
    return c ? c->timers() : active_context().timers();
}

lx::ui::overlay_layer& lx::ui::ui_overlays() { return active_context().overlays(); }

lx::ui::overlay_layer& lx::ui::detail::overlays_of(const ui_node& node) {
    ui_context* c = context_of(node);
    return c ? c->overlays() : active_context().overlays();
}

lx::ui::detail::reducer_queue& lx::ui::detail::active_reducers() {
    return active_context().reducers();
}

lx::ui::detail::reducer_queue* lx::ui::detail::reducers_of(const ui_node& node) {
    if (ui_context* c = context_of(node))
        return &c->reducers();
    ++ui_layout_counters().damage_dropped;
    return nullptr;
}
