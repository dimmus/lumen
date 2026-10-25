module;

export module lx.ui:overlay;

import std;
import lx.foundation;
import lx.layout;
import lx.runtime;

import :element;
import :events;
import :focus;
import :interact;
import :invalidate;
import :node;
import :theme;

// ── Overlays: GPUI's deferred draw on the retained tree (ADR-010 §5.6, §7 Phase 3) ────────
//
// Popovers, menus, tooltips, modals and toasts are `overlay_node`s anywhere in the tree. In
// the flow an overlay node takes no space (`position: absolute`, measured 0 × 0) and draws
// nothing; while it is open it is registered with the UI thread's `overlay_layer`, which —
// after the main tree's layout and emit, in `ui_root::tick` — lays its content (its one child)
// out against the window, places it beside an anchor (the overlay node's parent, a surface
// point, or the window itself) with GPUI's anchored rules: flip to the other side when it
// does not fit, then shift to stay `margin` px inside the window (Zed's
// `snap_to_window_with_margin(px(8.))`), and emits it after the main tree, so the overlap
// grid orders it above everything it covers. Overlays are hit-tested before the tree,
// topmost (last opened) first; an occluding one (a modal) blocks everything below it, scrim
// or not. A press outside an overlay dismisses it (`dismiss_outside`), as does Escape
// (`dismiss_escape`): the topmost one, an occluding overlay taking the press. A modal or menu traps the focus in its content
// while it is open (`focus_manager::trap`) and gives it back when it closes. Activating an
// item inside a menu closes the menu chain it is in. Every content node's bounds are surface
// coordinates (`node_flag::layer` on the overlay node stops `surface_rect` there), so their
// damage, hover and focus work as anywhere else.
//
// State: `overlay_props::open` opens and closes the overlay on its edges (an app's reducer
// holds it, and `on_dismiss` tells the app a user dismissed it); an overlay may also open
// itself (`open_at`: a tooltip after its hover delay, a context menu at the pointer, a
// submenu from its item) — what a user dismisses stays closed until the prop opens it again.
//
// Fixed capacity: `overlay_layer::k_capacity` overlays open at once, `overflow_action::
// reject` past that (the overlay stays closed; `rejected()` counts it). Nothing allocates:
// registration is a slot, layout and emit walk the slots. UI affinity.

export namespace lx::ui {

enum class overlay_kind : std::uint8_t { popover, menu, tooltip, modal, toast };

/// Where an overlay goes relative to its anchor (or the window).
enum class overlay_side : std::uint8_t {
    below,
    above,
    right,
    left,
    /// Centred horizontally, `overlay_placement::window_top` px below the window's top (a
    /// modal: Zed's modal layer, `top_20()`).
    window_top,
    /// Centred horizontally, `margin` px above the window's bottom (toasts).
    window_bottom,
};

/// Along the cross axis: the anchor's start edge, centre or end edge.
enum class overlay_align : std::uint8_t { start, center, end };

struct overlay_placement {
    overlay_side side = overlay_side::below;
    overlay_align align = overlay_align::start;
    /// px between the anchor and the overlay along the side.
    int gap = 0;
    /// Added after the side and alignment (Zed's popover offset: −5 px with a start anchor,
    /// the menu's 4 px padding and 1 px border).
    lx::point2i offset{};
    /// px the overlay keeps inside the window.
    int margin = 8;
    /// `window_top`'s distance from the window's top: 5 rem (`top_20`).
    int window_top = 80;
    /// Go to the other side (and the other end along the cross axis) when it does not fit.
    bool flip = true;
    /// Then slide it to stay `margin` inside the window.
    bool shift = true;
};

/// The rect of a `size` overlay beside `anchor` in `window` (surface coordinates). Pure.
[[nodiscard]] lx::rect2i place_overlay(lx::rect2i anchor, lx::size2i size, lx::rect2i window,
                                       const overlay_placement& placement);

struct overlay_props {
    overlay_kind kind = overlay_kind::popover;
    /// Opens on false → true, closes on true → false.
    bool open = false;
    overlay_placement placement{};
    /// Anchored at `point` (surface) rather than at the overlay node's parent.
    bool anchor_point = false;
    lx::point2i point{};
    bool dismiss_outside = true;
    bool dismiss_escape = true;
    /// The focus stays in the content while it is open (a modal, a menu).
    bool trap_focus = false;
    /// Blocks the pointer below it over the whole window, taking a press outside its content
    /// (and dismissing on it, with `dismiss_outside`): Zed's modal layer, scrim or not.
    bool occlude = false;
    /// Paints `elevated_surface.background` faded out by 0.2 over the window under it (Zed's
    /// modal layer when a modal asks for `fade_out_background`). Paint only.
    bool scrim = false;
    /// The pointer passes through it (a tooltip).
    bool hit_through = false;
    /// Fade in over this many ms (§5.5: 100–150, ease-out; 0: none).
    std::uint16_t fade_ms = 0;
    /// A user dismissed it (outside press, Escape, an item activated): called with `value`.
    handler on_dismiss{};
    int value = 0;
};

class overlay_layer;

/// An overlay in the tree: zero-size in the flow, its one child laid out and drawn by the
/// overlay layer while it is open.
class overlay_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    overlay_node();
    ~overlay_node() override;

    /// False: nothing the flow reads changes (the content is laid out by the layer).
    bool apply(const overlay_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    [[nodiscard]] hit_result hit_test(lx::point2i p) const override;
    /// A submenu: left arrow closes it (the focus goes back to its item).
    bool on_focus_key(key_event event, focus_manager& focus) override;

    /// Shows it (registers it with the layer). False when the layer is full.
    bool open();
    /// Shows it anchored at a surface point (a context menu, a tooltip).
    bool open_at(lx::point2i surface);
    /// Hides it, and every overlay open inside it; no handler.
    void close();
    /// A user dismissed it: `close` and `on_dismiss(value)`.
    void dismiss();
    [[nodiscard]] bool is_open() const { return open_; }
    [[nodiscard]] overlay_kind kind() const { return props_.kind; }
    [[nodiscard]] const overlay_props& props() const { return props_; }
    [[nodiscard]] ui_node* content() const { return child_at(0); }
    /// Inside another overlay's content (a submenu).
    [[nodiscard]] bool nested() const;
    /// The anchor in surface coordinates: the point, or the parent's bounds.
    [[nodiscard]] lx::rect2i anchor_rect() const;

private:
    friend class overlay_layer;
    overlay_props props_{};
    transition fade_{};
    lx::point2i point_{};
    bool open_ = false;
    bool prop_open_ = false;
    bool at_point_ = false;
};

/// The UI thread's open overlays, bottom to top (open order).
class overlay_layer final : public node_observer {
public:
    static constexpr unsigned k_capacity = 16;

    overlay_layer() = default;
    ~overlay_layer() override = default;

    /// The tree whose overlays are live (`ui_root` keeps it current). Entries of another
    /// tree are dropped.
    void set_root(ui_node* root);
    /// Where traps go (`ui_root`'s focus manager).
    void set_focus(focus_manager* focus) { focus_ = focus; }
    /// The last pointer position (a tooltip opens there).
    void set_pointer(lx::point2i at) { pointer_ = at; }
    [[nodiscard]] lx::point2i pointer() const { return pointer_; }

    /// Lays every open overlay out against `window` (after the main tree's layout): the
    /// content measured within the window less the margins, placed by `place_overlay`.
    /// Traps the focus of overlays that just opened.
    void layout(lx::rect2i window);
    /// Draws them, bottom to top, after the main tree.
    void emit(draw_scope& out) const;

    /// What is under `at`: an interactive node of the topmost overlay containing it
    /// (`interactive`), or its deepest node; `blocked` when an overlay covers `at` (its
    /// content, or a scrim) so nothing below may take it.
    [[nodiscard]] ui_node* hit(lx::point2i at, bool interactive, bool& blocked) const;
    /// A button press at `at`: dismisses the overlays it is outside of, top down, stopping at
    /// the first it is inside (or on whose anchor it is). True when a scrim took it.
    bool press(lx::point2i at);
    /// Escape: dismisses the topmost overlay that Escape dismisses. True when one did.
    bool escape();

    /// A control activated (`control_node::activate`); `after_input` closes the menu chain
    /// it is in, unless it opens an overlay itself (a submenu's item).
    void note_activation(ui_node& node);
    void after_input();

    [[nodiscard]] unsigned open_count() const;
    /// The topmost live overlay, or null.
    [[nodiscard]] overlay_node* top() const;
    [[nodiscard]] std::uint64_t rejected() const { return rejected_; }
    [[nodiscard]] lx::rect2i window() const { return window_; }

    void node_destroyed(ui_node& node) override;

private:
    friend class overlay_node;
    bool add(overlay_node& node);
    void remove(overlay_node& node);
    [[nodiscard]] bool live(const overlay_node& node) const;
    /// Closes every open overlay inside `node`'s content.
    void close_inside(const overlay_node& node);

    struct entry {
        overlay_node* node = nullptr;
        bool trapped = false;
        bool fresh = true;
    };
    entry entries_[k_capacity]{};
    unsigned count_ = 0;
    std::uint64_t rejected_ = 0;
    ui_node* root_ = nullptr;
    focus_manager* focus_ = nullptr;
    ui_node* activated_ = nullptr;
    lx::point2i pointer_{};
    lx::rect2i window_{};
};

[[nodiscard]] overlay_layer& ui_overlays();

/// An overlay over `content` (null: nothing to show).
[[nodiscard]] child overlay(const overlay_props& props, child content, element_key key = 0);

/// `parent`'s child overlay of `kind`, or null (a control's tooltip or submenu).
[[nodiscard]] overlay_node* child_overlay(const ui_node& parent, overlay_kind kind);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] lx::rect2i lx_ui_ov_shrink(lx::rect2i r, int m) {
    return {r.x + m, r.y + m, std::max(r.width - 2 * m, 0), std::max(r.height - 2 * m, 0)};
}

/// Where along one axis a `len` box goes for an anchor [a0, a0 + alen) by `align`.
[[nodiscard]] int lx_ui_ov_align(lx::ui::overlay_align align, int a0, int alen, int len) {
    switch (align) {
    case lx::ui::overlay_align::center: return a0 + (alen - len) / 2;
    case lx::ui::overlay_align::end: return a0 + alen - len;
    case lx::ui::overlay_align::start:
    default: return a0;
    }
}

[[nodiscard]] lx::ui::overlay_align lx_ui_ov_flip(lx::ui::overlay_align a) {
    return a == lx::ui::overlay_align::start ? lx::ui::overlay_align::end
         : a == lx::ui::overlay_align::end   ? lx::ui::overlay_align::start
                                             : a;
}

/// How far [p, p + len) sticks out of [lo, hi): 0 when inside.
[[nodiscard]] int lx_ui_ov_overflow(int p, int len, int lo, int hi) {
    return std::max(lo - p, 0) + std::max(p + len - hi, 0);
}

lx::ui::overlay_layer* g_overlay_layer = nullptr;

[[nodiscard]] lx::rect2i lx_ui_ov_union(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width);
    const int y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

/// What an overlay's content paints (surface coordinates): every visible node's bounds with
/// its shadows' reach (`ink_bounds`), not descending into overlays of its own (a submenu is
/// its own layer entry).
[[nodiscard]] lx::rect2i lx_ui_ov_painted(const lx::ui::ui_node& n) {
    if (!n.visible() || n.has_flag(lx::ui::node_flag::layer))
        return {};
    lx::rect2i r = lx::ui::ink_bounds(n);
    for (unsigned i = 0; i < n.child_count(); ++i)
        if (const lx::ui::ui_node* c = n.child_at(i))
            r = lx_ui_ov_union(r, lx_ui_ov_painted(*c));
    return r;
}

} // namespace

lx::rect2i lx::ui::place_overlay(lx::rect2i a, lx::size2i s, lx::rect2i w,
                                 const overlay_placement& p) {
    const int lo_x = w.x + p.margin;
    const int hi_x = w.x + w.width - p.margin;
    const int lo_y = w.y + p.margin;
    const int hi_y = w.y + w.height - p.margin;
    int x = 0;
    int y = 0;
    switch (p.side) {
    case overlay_side::window_top:
        x = w.x + (w.width - s.width) / 2 + p.offset.x;
        y = w.y + p.window_top + p.offset.y;
        break;
    case overlay_side::window_bottom:
        x = w.x + (w.width - s.width) / 2 + p.offset.x;
        y = hi_y - s.height + p.offset.y;
        break;
    case overlay_side::below:
    case overlay_side::above: {
        // Main axis: y. Cross axis: x by the alignment.
        const int below = a.y + a.height + p.gap + p.offset.y;
        const int above = a.y - p.gap - s.height + p.offset.y;
        const bool want_below = p.side == overlay_side::below;
        y = want_below ? below : above;
        if (p.flip) {
            const int here = lx_ui_ov_overflow(y, s.height, lo_y, hi_y);
            const int other_y = want_below ? above : below;
            const int there = lx_ui_ov_overflow(other_y, s.height, lo_y, hi_y);
            if (here > 0 && there < here)
                y = other_y;
        }
        // GPUI's `SwitchAnchor`: the overlay hangs from the anchor's other end when that fits
        // better (a menu near the right edge opens leftwards).
        overlay_align al = p.align;
        int dx = p.offset.x;
        x = lx_ui_ov_align(al, a.x, a.width, s.width) + dx;
        if (p.flip && al != overlay_align::center) {
            const int here = lx_ui_ov_overflow(x, s.width, lo_x, hi_x);
            const int other = lx_ui_ov_align(lx_ui_ov_flip(al), a.x, a.width, s.width) - dx;
            if (here > 0 && lx_ui_ov_overflow(other, s.width, lo_x, hi_x) < here)
                x = other;
        }
        break;
    }
    case overlay_side::right:
    case overlay_side::left: {
        const int right = a.x + a.width + p.gap + p.offset.x;
        const int left = a.x - p.gap - s.width - p.offset.x;
        const bool want_right = p.side == overlay_side::right;
        x = want_right ? right : left;
        if (p.flip) {
            const int here = lx_ui_ov_overflow(x, s.width, lo_x, hi_x);
            const int other_x = want_right ? left : right;
            const int there = lx_ui_ov_overflow(other_x, s.width, lo_x, hi_x);
            if (here > 0 && there < here)
                x = other_x;
        }
        const int dy = p.offset.y;
        y = lx_ui_ov_align(p.align, a.y, a.height, s.height) + dy;
        if (p.flip && p.align != overlay_align::center) {
            const int here = lx_ui_ov_overflow(y, s.height, lo_y, hi_y);
            const int other = lx_ui_ov_align(lx_ui_ov_flip(p.align), a.y, a.height, s.height) - dy;
            if (here > 0 && lx_ui_ov_overflow(other, s.height, lo_y, hi_y) < here)
                y = other;
        }
        break;
    }
    }
    if (p.shift) {
        // Slide inside the margins; a box wider (taller) than the room keeps its start edge.
        if (x + s.width > hi_x)
            x = hi_x - s.width;
        if (x < lo_x)
            x = lo_x;
        if (y + s.height > hi_y)
            y = hi_y - s.height;
        if (y < lo_y)
            y = lo_y;
    }
    return {x, y, s.width, s.height};
}

// ── overlay_node ──

lx::ui::element_type_id lx::ui::overlay_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::overlay_node::type() const { return static_type(); }
const char* lx::ui::overlay_node::type_name() const { return "overlay"; }

lx::ui::overlay_node::overlay_node() {
    set_flag(node_flag::layer, true);
    set_visible(false);
}

lx::ui::overlay_node::~overlay_node() {
    // The layer forgets it through `node_destroyed` (the node is referenced once opened).
}

bool lx::ui::overlay_node::apply(const overlay_props& p) {
    const bool was = prop_open_;
    props_ = p;
    prop_open_ = p.open;
    if (p.open && !was)
        (void)(p.anchor_point ? open_at(p.point) : open());
    else if (!p.open && was)
        close();
    else if (open_ && p.anchor_point)
        point_ = p.point;
    return false;
}

lx::size2i lx::ui::overlay_node::measure(lx::layout::constraints c, size_hint) const {
    return c.min;
}

lx::size2i lx::ui::overlay_node::measure_content(lx::layout::constraints c, size_hint) const {
    return c.min;
}

void lx::ui::overlay_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
}

void lx::ui::overlay_node::emit(draw_scope&) const {}

const lx::layout::flex_style* lx::ui::overlay_node::flex_item_style() const {
    static const lx::layout::flex_style k_absolute = [] {
        lx::layout::flex_style s{};
        s.position = lx::layout::positioning::absolute;
        s.inset = {lx::layout::px(0.f), lx::layout::auto_length, lx::layout::auto_length,
                   lx::layout::px(0.f)};
        s.width = s.height = lx::layout::px(0.f);
        return s;
    }();
    return &k_absolute;
}

lx::ui::hit_result lx::ui::overlay_node::hit_test(lx::point2i) const { return hit_result::pass; }

bool lx::ui::overlay_node::on_focus_key(key_event event, focus_manager&) {
    if (!event.pressed || !open_ || props_.kind != overlay_kind::menu || !nested())
        return false;
    if (event.key != key::left)
        return false;
    dismiss();
    return true;
}

bool lx::ui::overlay_node::nested() const {
    for (const ui_node* p = parent(); p; p = p->parent())
        if (p->has_flag(node_flag::layer))
            return true;
    return false;
}

lx::rect2i lx::ui::overlay_node::anchor_rect() const {
    if (at_point_)
        return {point_.x, point_.y, 0, 0};
    const ui_node* p = parent();
    return p ? surface_bounds(*p) : lx::rect2i{};
}

bool lx::ui::overlay_node::open() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    at_point_ = props_.anchor_point;
    if (open_)
        return true;
    if (!ui_overlays().add(*this))
        return false; // overflow_action::reject: stays closed
    open_ = true;
    set_visible(true);
    if (props_.fade_ms)
        fade_.start(0.f, 1.f, props_.fade_ms);
    else
        fade_.set(1.f);
    ui_damage().request(invalidation::paint);
    return true;
}

bool lx::ui::overlay_node::open_at(lx::point2i surface) {
    point_ = surface;
    const bool was = open_;
    const bool ok = open();
    at_point_ = true;
    if (was && ok) {
        // Moved while open (a second right click): lay it out again there.
        if (ui_node* c = content())
            invalidate_paint(*c);
        ui_damage().request(invalidation::paint);
    }
    return ok;
}

void lx::ui::overlay_node::close() {
    if (!open_)
        return;
    overlay_layer& layer = ui_overlays();
    layer.close_inside(*this);
    open_ = false;
    if (ui_node* c = content())
        detail::damage_rect(lx_ui_ov_painted(*c));
    if (props_.scrim)
        detail::damage_rect(layer.window());
    layer.remove(*this);
    set_visible(false);
    fade_.set(0.f);
}

void lx::ui::overlay_node::dismiss() {
    if (!open_)
        return;
    close();
    if (props_.on_dismiss)
        props_.on_dismiss(props_.value);
}

lx::ui::child lx::ui::overlay(const overlay_props& props, child content, element_key key) {
    return with_children(describe<overlay_node, overlay_props>(props, key),
                         {&content, content ? 1u : 0u});
}

lx::ui::overlay_node* lx::ui::child_overlay(const ui_node& parent, overlay_kind kind) {
    for (unsigned i = 0; i < parent.child_count(); ++i) {
        ui_node* c = parent.child_at(i);
        if (c && c->type() == overlay_node::static_type()) {
            auto* o = static_cast<overlay_node*>(c);
            if (o->kind() == kind)
                return o;
        }
    }
    return nullptr;
}

// ── overlay_layer ──

lx::ui::overlay_layer& lx::ui::ui_overlays() {
    // Never destroyed: nodes may outlive any static they could be torn down with.
    if (!g_overlay_layer)
        g_overlay_layer = new overlay_layer{};
    return *g_overlay_layer;
}

bool lx::ui::overlay_layer::live(const overlay_node& n) const {
    if (!n.open_ || !n.visible())
        return false;
    if (!root_)
        return true;
    const ui_node* r = &n;
    while (r->parent())
        r = r->parent();
    return r == root_;
}

void lx::ui::overlay_layer::set_root(ui_node* root) {
    if (root == root_)
        return;
    root_ = root;
    // Another tree: its overlays are not this one's (they stay registered as closed).
    for (unsigned i = 0; i < count_;) {
        overlay_node& n = *entries_[i].node;
        const ui_node* r = &n;
        while (r->parent())
            r = r->parent();
        if (root && r != root) {
            n.open_ = false;
            n.set_visible(false);
            for (unsigned j = i; j + 1 < count_; ++j)
                entries_[j] = entries_[j + 1];
            --count_;
        } else {
            ++i;
        }
    }
}

bool lx::ui::overlay_layer::add(overlay_node& node) {
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].node == &node)
            return true;
    if (count_ == k_capacity) {
        ++rejected_; // overflow_action::reject
        return false;
    }
    node.mark_referenced();
    entries_[count_++] = {&node, false, true};
    return true;
}

void lx::ui::overlay_layer::remove(overlay_node& node) {
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].node != &node)
            continue;
        const bool trapped = entries_[i].trapped;
        for (unsigned j = i; j + 1 < count_; ++j)
            entries_[j] = entries_[j + 1];
        --count_;
        if (trapped && focus_)
            if (ui_node* c = node.content())
                focus_->release(*c);
        return;
    }
}

void lx::ui::overlay_layer::close_inside(const overlay_node& node) {
    const ui_node* content = node.content();
    if (!content)
        return;
    // Top down, so a sub-submenu closes before its submenu.
    for (unsigned i = count_; i > 0; --i) {
        if (i > count_)
            continue;
        overlay_node* o = entries_[i - 1].node;
        if (o == &node)
            continue;
        for (const ui_node* p = o->parent(); p; p = p->parent())
            if (p == content) {
                o->close();
                break;
            }
    }
}

void lx::ui::overlay_layer::node_destroyed(ui_node& node) {
    if (activated_ == &node)
        activated_ = nullptr;
    for (unsigned i = 0; i < count_; ++i) {
        if (entries_[i].node != &node)
            continue;
        for (unsigned j = i; j + 1 < count_; ++j)
            entries_[j] = entries_[j + 1];
        --count_;
        return;
    }
}

void lx::ui::overlay_layer::layout(lx::rect2i window) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    window_ = window;
    // Overlay content resolves its percentages against the window.
    const percent_base_scope base{static_cast<float>(window.width), static_cast<float>(window.height)};
    for (unsigned i = 0; i < count_; ++i) {
        entry& e = entries_[i];
        overlay_node& n = *e.node;
        ui_node* content = n.content();
        if (!content || !live(n))
            continue;
        const overlay_placement& pl = n.props_.placement;
        const lx::rect2i room = lx_ui_ov_shrink(window, pl.margin);
        const lx::size2i size = content->measure_cached(
            {{0, 0}, {std::max(room.width, 0), std::max(room.height, 0)}}, size_hint::preferred);
        const lx::rect2i r = place_overlay(n.anchor_rect(), size, window, pl);
        content->place(r);
        if (e.fresh) {
            // Shown now: it paints even where it was laid out before (closed, then opened).
            e.fresh = false;
            detail::damage_rect(lx_ui_ov_union(r, lx_ui_ov_painted(*content)));
            if (n.props_.scrim)
                detail::damage_rect(window);
            if (n.props_.trap_focus && focus_ && !e.trapped)
                e.trapped = focus_->trap(*content);
        }
    }
}

void lx::ui::overlay_layer::emit(draw_scope& out) const {
    for (unsigned i = 0; i < count_; ++i) {
        const overlay_node& n = *entries_[i].node;
        const ui_node* content = n.content();
        if (!content || !live(n) || entries_[i].fresh)
            continue;
        const float a = n.props_.fade_ms ? animate(*content, n.fade_) : 1.f;
        if (n.props_.fade_ms && n.fade_.running(ui_now_ns())) {
            // The fade repaints what the content paints next frame — its shadows past its
            // bounds, and the scrim over the whole window.
            damage_ledger& ledger = ui_damage();
            ledger.add(n.props_.scrim ? window_ : lx_ui_ov_painted(*content));
        }
        if (n.props_.scrim) {
            // Zed's modal layer: `elevated_surface_background.fade_out(0.2)` over the window.
            quad_style q{};
            q.background = active_theme().color(tok::elevated_surface_background);
            q.background.a *= 0.8f * a;
            out.quad(window_, q);
        }
        const bool fade = a < 1.f;
        if (fade)
            out.push_opacity(a);
        content->emit(out);
        if (fade)
            out.pop();
    }
}

lx::ui::ui_node* lx::ui::overlay_layer::hit(lx::point2i at, bool interactive,
                                            bool& blocked) const {
    blocked = false;
    for (unsigned i = count_; i > 0; --i) {
        overlay_node& n = *entries_[i - 1].node;
        ui_node* content = n.content();
        if (!content || !live(n) || n.props_.hit_through)
            continue;
        if (content->bounds().contains(at)) {
            blocked = true;
            return interactive ? hit_interactive(*content, at) : hit_node(*content, at);
        }
        if (n.props_.occlude) {
            blocked = true;
            return nullptr;
        }
    }
    return nullptr;
}

bool lx::ui::overlay_layer::press(lx::point2i at) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    for (unsigned i = count_; i > 0; --i) {
        if (i > count_)
            continue; // a dismissal above closed more than one
        overlay_node& n = *entries_[i - 1].node;
        if (!live(n))
            continue;
        const ui_node* content = n.content();
        if (content && content->bounds().contains(at) && n.props_.kind != overlay_kind::tooltip)
            return false;
        // A press on the anchor of a popover or menu is the trigger's: it toggles it.
        if (!n.at_point_ && n.props_.kind != overlay_kind::modal &&
            n.props_.kind != overlay_kind::tooltip && n.parent() &&
            surface_bounds(*n.parent()).contains(at))
            return false;
        if (n.props_.occlude) {
            if (n.props_.dismiss_outside)
                n.dismiss();
            return true;
        }
        if (n.props_.dismiss_outside || n.props_.kind == overlay_kind::tooltip)
            n.dismiss();
    }
    return false;
}

bool lx::ui::overlay_layer::escape() {
    for (unsigned i = count_; i > 0; --i) {
        overlay_node& n = *entries_[i - 1].node;
        if (!live(n) || !n.props_.dismiss_escape)
            continue;
        n.dismiss();
        return true;
    }
    return false;
}

void lx::ui::overlay_layer::note_activation(ui_node& node) {
    node.mark_referenced();
    activated_ = &node;
}

void lx::ui::overlay_layer::after_input() {
    ui_node* a = activated_;
    activated_ = nullptr;
    if (!a || child_overlay(*a, overlay_kind::menu))
        return;
    // The outermost menu of the chain the item is in.
    overlay_node* outer = nullptr;
    for (ui_node* p = a->parent(); p; p = p->parent()) {
        if (p->type() != overlay_node::static_type())
            continue;
        auto* o = static_cast<overlay_node*>(p);
        if (o->kind() != overlay_kind::menu)
            break;
        outer = o;
    }
    if (outer && outer->is_open())
        outer->dismiss();
}

unsigned lx::ui::overlay_layer::open_count() const {
    unsigned n = 0;
    for (unsigned i = 0; i < count_; ++i)
        n += live(*entries_[i].node) ? 1u : 0u;
    return n;
}

lx::ui::overlay_node* lx::ui::overlay_layer::top() const {
    for (unsigned i = count_; i > 0; --i)
        if (live(*entries_[i - 1].node))
            return entries_[i - 1].node;
    return nullptr;
}
