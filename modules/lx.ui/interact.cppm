module;

import lx.foundation;
import lx.runtime;

export module lx.ui:interact;

import std;

import :events;
import :focus;
import :invalidate;
import :node;
import :theme;

// ── Pointer state, handlers, timers and motion (ADR-010 §4.9, §5.5) ─────────────────────
//
// Pointer: `pointer_state` keeps the hovered and the pressed node — the topmost interactive,
// enabled node under the pointer (`hit_interactive`) — in `node_flag::hovered` / `pressed`,
// and calls `on_interaction_changed` on each node whose state moved, so a hover repaints that
// control and nothing else (§5.5: hover is a background change only; the control picks its
// background token from the flags in `emit`). A release over the node it pressed activates it.
// A press moves the focus there without a ring (`focus_manager::pointer_down`).
//
// Handlers: `handler` is a trivially copyable callable — a function pointer, a reducer's
// `action_binding`, any trivially copyable lambda of up to 24 bytes — stored inline in props,
// so describing an event handler allocates nothing and needs no destructor.
//
// Time: `ui_now_ns()` is the frame clock — the time `ui_root::tick` latched for the frame it is
// building, else CLOCK_MONOTONIC (`set_ui_clock` replaces it in tests). `ui_timers()` is the UI
// thread's fixed timer table (32 entries, `overflow_action::reject`): a due timer calls its
// node's `on_timer` at the start of the next tick, and `ui_root::next_deadline_ns` tells the
// event loop when to wake — a delay requests no frames while it waits (G1). Hover intent (the
// 500 ms before a tooltip, §5.5) is such a timer, set for nodes that ask for it
// (`node_flag::hover_intent`).
//
// Motion: a `transition` is a retained tween (opacity, a fade: 100–150 ms ease-out); `animate`
// samples it in `emit` and, while it runs, asks for the next frame and repaints the node's
// bounds then. A settled transition asks for nothing, so an idle tree requests no frame.
// `reduced_motion` makes every transition jump to its end.
//
// All UI affinity.

export namespace lx::ui {

/// A trivially copyable event callback carried in props: a function pointer (taking nothing
/// or the event's value), or a trivially copyable callable of up to `k_storage` bytes taking
/// nothing or an `int` (a reducer's `action_binding`, a lambda capturing a pointer or two).
/// The value is the control's: a toggle's new state (1 / 0), a tab's index, a button's 0.
class handler {
public:
    static constexpr std::size_t k_storage = 24;

    constexpr handler() = default;
    handler(void (*fn)()); // NOLINT(google-explicit-constructor)
    handler(void (*fn)(int)); // NOLINT(google-explicit-constructor)
    template<typename F>
        requires(!std::is_pointer_v<F> && !std::is_same_v<std::remove_cvref_t<F>, handler> &&
                 std::is_trivially_copyable_v<F> && sizeof(F) <= k_storage &&
                 alignof(F) <= alignof(void*) &&
                 (std::is_invocable_v<const F&> || std::is_invocable_v<const F&, int>))
    handler(const F& f) { // NOLINT(google-explicit-constructor)
        std::memcpy(storage_, &f, sizeof(F));
        invoke_ = [](const std::byte* s, int value) {
            // The bytes were copied from an F (trivially copyable: memcpy began its lifetime).
            const F& fn = *std::launder(reinterpret_cast<const F*>(s));
            if constexpr (std::is_invocable_v<const F&, int>)
                fn(value);
            else {
                (void)value;
                fn();
            }
        };
    }

    void operator()(int value = 0) const {
        if (invoke_)
            invoke_(storage_, value);
    }
    [[nodiscard]] explicit operator bool() const { return invoke_ != nullptr; }

private:
    void (*invoke_)(const std::byte*, int) = nullptr;
    alignas(void*) std::byte storage_[k_storage]{};
};
static_assert(std::is_trivially_copyable_v<handler>);

// ── Frame clock and motion ──

/// The frame clock, ns on CLOCK_MONOTONIC (or the test clock): the time the running tick
/// latched, else now.
[[nodiscard]] std::uint64_t ui_now_ns();
/// Replace the clock (null: CLOCK_MONOTONIC). Tests drive time with it.
void set_ui_clock(std::uint64_t (*clock)());
/// `ui_root::tick` latches the frame time for the frame it builds, and lets it go.
void begin_ui_frame();
void end_ui_frame();

/// Zed's motion (§5.5): popovers and tooltips fade in over 100–150 ms ease-out, a tooltip
/// after a 500 ms hover; nothing bounces.
inline constexpr std::uint32_t k_motion_fast_ms = 100;
inline constexpr std::uint32_t k_motion_ms = 150;
inline constexpr std::uint32_t k_hover_intent_ms = 500;

enum class easing : std::uint8_t { linear, ease_out, ease_in_out };

[[nodiscard]] float ease(easing curve, float t);

/// A retained tween of one value. Value semantics, 24 bytes, no allocation.
struct transition {
    std::uint64_t start_ns = 0;
    std::uint32_t duration_ms = 0;
    float from = 0.f;
    float to = 0.f;
    easing curve = easing::ease_out;

    /// From `from` to `to` over `ms`, starting at the frame clock (`ui_reduced_motion`: at
    /// `to` at once).
    void start(float from_value, float to_value, std::uint32_t ms = k_motion_ms,
               easing c = easing::ease_out);
    /// To `to_value` from wherever it is now.
    void retarget(float to_value, std::uint32_t ms = k_motion_ms, easing c = easing::ease_out);
    /// At `v`, not moving.
    void set(float v);
    [[nodiscard]] float value(std::uint64_t now_ns) const;
    [[nodiscard]] float value() const { return value(ui_now_ns()); }
    [[nodiscard]] bool running(std::uint64_t now_ns) const;
    [[nodiscard]] bool running() const { return running(ui_now_ns()); }
};

/// `t`'s value for the frame being built, from a node's `emit`: while it runs, asks for the
/// next frame (`invalidation::animate`) and repaints `node`'s bounds then; settled, nothing.
float animate(const ui_node& node, const transition& t);

// ── Timers ──

/// Timer tags `ui_node::on_timer` receives from the toolkit; a node's own start at 256.
inline constexpr std::uint32_t k_timer_hover_intent = 1;
inline constexpr std::uint32_t k_timer_user = 256;

/// The UI thread's deadlines for nodes: (node, tag) → when. Fixed capacity
/// (`overflow_action::reject`: `schedule` says false and counts it); a node that dies takes
/// its timers with it.
class timer_queue final : public node_observer {
public:
    static constexpr unsigned k_capacity = 32;

    timer_queue() = default;
    ~timer_queue() override = default;

    /// Fires `node.on_timer(tag)` `delay_ns` from the frame clock; replaces the same
    /// (node, tag). False when full.
    bool schedule(ui_node& node, std::uint32_t tag, std::uint64_t delay_ns);
    void cancel(ui_node& node, std::uint32_t tag);
    void cancel_all(ui_node& node);
    /// Calls every timer due at `now_ns`, earliest first. Returns how many fired.
    unsigned fire_due(std::uint64_t now_ns);
    /// The earliest deadline, 0 when none is set.
    [[nodiscard]] std::uint64_t next_deadline() const;
    [[nodiscard]] unsigned size() const { return count_; }
    [[nodiscard]] std::uint64_t rejected() const { return rejected_; }

    void node_destroyed(ui_node& node) override;

private:
    struct entry {
        std::uint64_t deadline = 0;
        ui_node* node = nullptr;
        std::uint32_t tag = 0;
    };
    entry entries_[k_capacity]{};
    unsigned count_ = 0;
    std::uint64_t rejected_ = 0;
};

[[nodiscard]] timer_queue& ui_timers();

// ── Pointer ──

/// The topmost visible, interactive, enabled node under `at` in `root`'s tree (later
/// siblings paint over earlier ones, so they win), or null. `at` is in `root`'s layout space;
/// a viewport (`ui_node::child_viewport`) translates it for its children and hides what it
/// clips. Overlay subtrees (`node_flag::layer`) are skipped: the overlay layer tests them.
[[nodiscard]] ui_node* hit_interactive(ui_node& root, lx::point2i at);
/// The deepest visible node under `at` (any node, interactive or not), by the same rules:
/// where a wheel or a secondary press starts its walk up the ancestors.
[[nodiscard]] ui_node* hit_node(ui_node& root, lx::point2i at);

/// What the pointer is over: `interactive` false asks for `hit_node`'s answer. `ui_root`
/// installs one that tests the overlay layer first.
using pointer_resolver = ui_node* (*)(void* context, ui_node* root, lx::point2i at,
                                      bool interactive);

class pointer_state final : public node_observer {
public:
    /// Presses this close in time and space count as one double (triple) click.
    static constexpr std::uint64_t k_multi_click_ns = 400'000'000ull;
    static constexpr int k_multi_click_px = 4;

    pointer_state() = default;
    ~pointer_state() override = default;

    /// Replaces the hit test (null: `hit_interactive` / `hit_node` on the root).
    void set_resolver(pointer_resolver resolver, void* context) {
        resolver_ = resolver;
        resolver_context_ = context;
    }
    /// The keyboard modifiers pointer events report (`ui_root` keeps them current).
    void set_modifiers(unsigned modifiers) { modifiers_ = modifiers; }
    [[nodiscard]] unsigned modifiers() const { return modifiers_; }
    /// What `resolver` (or the plain hit test) finds at `at`.
    [[nodiscard]] ui_node* resolve(ui_node* root, lx::point2i at, bool interactive) const;

    /// The pointer is at `at` over `root`'s tree: the hover moves to what is there. True
    /// when the hovered node changed.
    bool move(ui_node* root, lx::point2i at);
    /// The pointer left the surface: nothing is hovered (a press stays until its release).
    void leave();
    /// A button press or release at the last position. Left press: the hovered node is
    /// pressed, the focus moves to it without a ring; left release over the node it pressed:
    /// `activate`. True when an interactive node took it.
    bool button(ui_node* root, mouse_button b, bool pressed, focus_manager* focus);
    /// Hit test again at the last position (the tree changed under a still pointer).
    bool refresh(ui_node* root) { return inside_ ? move(root, position_) : false; }

    [[nodiscard]] ui_node* hovered() const { return hovered_; }
    [[nodiscard]] ui_node* pressed() const { return pressed_; }
    [[nodiscard]] lx::point2i position() const { return position_; }

    void node_destroyed(ui_node& node) override;

private:
    void set_hovered(ui_node* node);

    ui_node* hovered_ = nullptr;
    ui_node* pressed_ = nullptr;
    lx::point2i position_{};
    bool inside_ = false;
    pointer_resolver resolver_ = nullptr;
    void* resolver_context_ = nullptr;
    unsigned modifiers_ = 0;
    // Multi-click counting: the last press's time, place and node, and its count.
    std::uint64_t last_press_ns_ = 0;
    lx::point2i last_press_at_{};
    const ui_node* last_press_node_ = nullptr;
    unsigned clicks_ = 0;
};

// ── Focus ring ──

/// The focus ring of §5.5 over `bounds` (corner radii px, top-left clockwise): a `width` px
/// (1 by default) `border.focused` border on the box itself, no glow, no offset.
void draw_focus_ring(draw_scope& out, lx::rect2i bounds, const float (&radii)[4],
                     float width = 1.f);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

lx::ui::handler::handler(void (*fn)()) {
    if (!fn)
        return;
    std::memcpy(storage_, &fn, sizeof(fn));
    invoke_ = [](const std::byte* s, int) {
        void (*f)() = nullptr;
        std::memcpy(&f, s, sizeof(f));
        f();
    };
}

lx::ui::handler::handler(void (*fn)(int)) {
    if (!fn)
        return;
    std::memcpy(storage_, &fn, sizeof(fn));
    invoke_ = [](const std::byte* s, int value) {
        void (*f)(int) = nullptr;
        std::memcpy(&f, s, sizeof(f));
        f(value);
    };
}

namespace {

std::uint64_t (*g_ui_clock)() = nullptr;
std::uint64_t g_frame_ns = 0;
bool g_in_frame = false;

[[nodiscard]] std::uint64_t lx_ui_clock_now() {
    if (g_ui_clock)
        return g_ui_clock();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

std::uint64_t lx::ui::ui_now_ns() { return g_in_frame ? g_frame_ns : lx_ui_clock_now(); }

void lx::ui::set_ui_clock(std::uint64_t (*clock)()) { g_ui_clock = clock; }

void lx::ui::begin_ui_frame() {
    g_frame_ns = lx_ui_clock_now();
    g_in_frame = true;
}

void lx::ui::end_ui_frame() { g_in_frame = false; }

float lx::ui::ease(easing curve, float t) {
    t = std::clamp(t, 0.f, 1.f);
    switch (curve) {
    case easing::linear:
        return t;
    case easing::ease_out: {
        // Cubic ease-out: fast start, settles gently (GPUI's `ease_out_quint` family).
        const float u = 1.f - t;
        return 1.f - u * u * u;
    }
    case easing::ease_in_out:
        return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
    }
    return t;
}

void lx::ui::transition::start(float from_value, float to_value, std::uint32_t ms, easing c) {
    from = from_value;
    to = to_value;
    curve = c;
    start_ns = ui_now_ns();
    duration_ms = ui_reduced_motion() ? 0 : ms;
}

void lx::ui::transition::retarget(float to_value, std::uint32_t ms, easing c) {
    start(value(), to_value, ms, c);
}

void lx::ui::transition::set(float v) {
    from = to = v;
    duration_ms = 0;
    start_ns = 0;
}

float lx::ui::transition::value(std::uint64_t now_ns) const {
    if (duration_ms == 0 || now_ns <= start_ns)
        return duration_ms == 0 ? to : from;
    const double t = static_cast<double>(now_ns - start_ns) / (duration_ms * 1e6);
    if (t >= 1.0)
        return to;
    return from + (to - from) * ease(curve, static_cast<float>(t));
}

bool lx::ui::transition::running(std::uint64_t now_ns) const {
    return duration_ms != 0 && now_ns < start_ns + std::uint64_t{duration_ms} * 1000000ull;
}

float lx::ui::animate(const ui_node& node, const transition& t) {
    const std::uint64_t now = ui_now_ns();
    if (t.running(now)) {
        damage_ledger& ledger = ui_damage();
        ledger.add(surface_rect(node, ink_bounds(node)));
        ledger.request(invalidation::animate);
    }
    return t.value(now);
}

// ── timer_queue ──

bool lx::ui::timer_queue::schedule(ui_node& node, std::uint32_t tag, std::uint64_t delay_ns) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    const std::uint64_t deadline = ui_now_ns() + delay_ns;
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].node == &node && entries_[i].tag == tag) {
            entries_[i].deadline = deadline;
            return true;
        }
    }
    if (count_ == k_capacity) {
        ++rejected_; // overflow_action::reject
        return false;
    }
    node.mark_referenced();
    entries_[count_++] = {deadline, &node, tag};
    return true;
}

void lx::ui::timer_queue::cancel(ui_node& node, std::uint32_t tag) {
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].node == &node && entries_[i].tag == tag) {
            entries_[i] = entries_[--count_];
            return;
        }
    }
}

void lx::ui::timer_queue::cancel_all(ui_node& node) {
    for (unsigned i = 0; i < count_;) {
        if (entries_[i].node == &node)
            entries_[i] = entries_[--count_];
        else
            ++i;
    }
}

unsigned lx::ui::timer_queue::fire_due(std::uint64_t now_ns) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    unsigned fired = 0;
    // Earliest first; a handler may schedule or cancel (the table is re-scanned each time).
    for (unsigned guard = 0; guard < k_capacity * 2; ++guard) {
        unsigned best = count_;
        for (unsigned i = 0; i < count_; ++i)
            if (entries_[i].deadline <= now_ns &&
                (best == count_ || entries_[i].deadline < entries_[best].deadline))
                best = i;
        if (best == count_)
            break;
        const entry e = entries_[best];
        entries_[best] = entries_[--count_];
        e.node->on_timer(e.tag);
        ++fired;
    }
    return fired;
}

std::uint64_t lx::ui::timer_queue::next_deadline() const {
    std::uint64_t d = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (d == 0 || entries_[i].deadline < d)
            d = entries_[i].deadline;
    return d;
}

void lx::ui::timer_queue::node_destroyed(ui_node& node) { cancel_all(node); }

lx::ui::timer_queue& lx::ui::ui_timers() {
    // Never destroyed: nodes may outlive any static they could be torn down with.
    static timer_queue* const q = new timer_queue{};
    return *q;
}

// ── pointer ──

namespace {

/// `hit_interactive` / `hit_node`: children topmost first, each in its own space.
[[nodiscard]] lx::ui::ui_node* lx_ui_hit(lx::ui::ui_node& root, lx::point2i at,
                                         bool interactive) {
    using lx::ui::node_flag;
    if (!root.visible() || !root.bounds().contains(at))
        return nullptr;
    for (unsigned i = root.child_count(); i > 0; --i) {
        lx::ui::ui_node* c = root.child_at(i - 1);
        if (!c || c->has_flag(node_flag::layer))
            continue;
        lx::point2i p = at;
        lx::point2i offset{};
        lx::rect2i clip{};
        if (root.child_viewport(*c, offset, clip)) {
            if (!clip.contains(at))
                continue;
            p = {at.x - offset.x, at.y - offset.y};
        }
        if (lx::ui::ui_node* hit = lx_ui_hit(*c, p, interactive))
            return hit;
    }
    if (!interactive)
        return &root;
    if (root.has_flag(node_flag::interactive) && !root.has_flag(node_flag::disabled))
        return &root;
    return nullptr;
}

} // namespace

lx::ui::ui_node* lx::ui::hit_interactive(ui_node& root, lx::point2i at) {
    return lx_ui_hit(root, at, true);
}

lx::ui::ui_node* lx::ui::hit_node(ui_node& root, lx::point2i at) {
    return lx_ui_hit(root, at, false);
}

lx::ui::ui_node* lx::ui::pointer_state::resolve(ui_node* root, lx::point2i at,
                                                bool interactive) const {
    if (resolver_)
        return resolver_(resolver_context_, root, at, interactive);
    if (!root)
        return nullptr;
    return interactive ? hit_interactive(*root, at) : hit_node(*root, at);
}

void lx::ui::pointer_state::set_hovered(ui_node* node) {
    if (node == hovered_)
        return;
    if (ui_node* old = hovered_) {
        old->set_flag(node_flag::hovered, false);
        if (old->has_flag(node_flag::hover_intent))
            ui_timers().cancel(*old, k_timer_hover_intent);
        old->on_interaction_changed();
    }
    hovered_ = node;
    if (node) {
        node->mark_referenced();
        node->set_flag(node_flag::hovered, true);
        if (node->has_flag(node_flag::hover_intent))
            (void)ui_timers().schedule(*node, k_timer_hover_intent,
                                       std::uint64_t{k_hover_intent_ms} * 1000000ull);
        node->on_interaction_changed();
    }
}

bool lx::ui::pointer_state::move(ui_node* root, lx::point2i at) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    const bool moved = at.x != position_.x || at.y != position_.y;
    position_ = at;
    inside_ = true;
    ui_node* const before = hovered_;
    set_hovered(resolve(root, at, true));
    if (pressed_ && moved) {
        // A drag: the pressed node follows the pointer wherever it goes (a scrollbar's thumb,
        // a text selection).
        pointer_event e{};
        e.kind = pointer_kind::drag;
        e.at = at;
        e.modifiers = modifiers_;
        e.clicks = clicks_ ? clicks_ : 1;
        (void)pressed_->on_pointer(e);
    }
    return hovered_ != before;
}

void lx::ui::pointer_state::leave() {
    inside_ = false;
    set_hovered(nullptr);
}

bool lx::ui::pointer_state::button(ui_node* root, mouse_button b, bool down,
                                   focus_manager* focus) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (b != mouse_button::left)
        return false;
    if (down) {
        if (inside_)
            set_hovered(resolve(root, position_, true));
        ui_node* target = hovered_;
        if (focus)
            focus->pointer_down(target);
        if (!target)
            return false;
        if (pressed_ && pressed_ != target) {
            pressed_->set_flag(node_flag::pressed, false);
            pressed_->on_interaction_changed();
        }
        // Double and triple clicks: presses on the same node, close in time and space.
        const std::uint64_t now = ui_now_ns();
        const int dx = position_.x - last_press_at_.x;
        const int dy = position_.y - last_press_at_.y;
        if (clicks_ > 0 && target == last_press_node_ && now - last_press_ns_ <= k_multi_click_ns &&
            dx * dx + dy * dy <= k_multi_click_px * k_multi_click_px)
            clicks_ = clicks_ >= 3 ? 1 : clicks_ + 1;
        else
            clicks_ = 1;
        last_press_ns_ = now;
        last_press_at_ = position_;
        last_press_node_ = target;
        pressed_ = target;
        target->mark_referenced();
        target->set_flag(node_flag::pressed, true);
        target->on_interaction_changed();
        pointer_event e{};
        e.kind = pointer_kind::press;
        e.at = position_;
        e.modifiers = modifiers_;
        e.clicks = clicks_;
        if (pressed_ == target)
            (void)target->on_pointer(e);
        return true;
    }
    ui_node* target = pressed_;
    if (!target)
        return false;
    pressed_ = nullptr;
    target->set_flag(node_flag::pressed, false);
    target->on_interaction_changed();
    {
        pointer_event e{};
        e.kind = pointer_kind::release;
        e.at = position_;
        e.modifiers = modifiers_;
        e.clicks = clicks_ ? clicks_ : 1;
        (void)target->on_pointer(e);
    }
    // A click: released over the node it pressed (moving off and back still counts).
    if (hovered_ == target && !target->has_flag(node_flag::disabled))
        (void)target->activate();
    return true;
}

void lx::ui::pointer_state::node_destroyed(ui_node& node) {
    if (hovered_ == &node)
        hovered_ = nullptr;
    if (pressed_ == &node)
        pressed_ = nullptr;
    if (last_press_node_ == &node)
        last_press_node_ = nullptr;
}

// ── focus ring ──

void lx::ui::draw_focus_ring(draw_scope& out, lx::rect2i bounds, const float (&radii)[4],
                             float width) {
    quad_style q{};
    q.border_color = active_theme().color(tok::border_focused);
    for (unsigned i = 0; i < 4; ++i) {
        q.corner_radii[i] = radii[i];
        q.border_widths[i] = width;
    }
    out.quad(bounds, q);
}
