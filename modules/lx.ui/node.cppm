module;

#include <stddef.h>

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.a11y;

export module lx.ui:node;

import std;

import :element;
import :events;
import :style;

export namespace lx::ui {

/// Which end of the constraint range a measure pass is asking about. Measuring for
/// `min` and `max` separately is what lets flex distribute slack without guessing.
/// `min` is the min-content width (the narrowest the content wraps to); a flex container
/// asks it for an item's automatic minimum size.
enum class size_hint : unsigned char { min, preferred, max };

/// Whether a measurement applies the node's own size styles (`inherent`, what any parent
/// asks) or measures the content alone (`content`: what a flex container asks of an item
/// whose width, height, min or max it applies itself — Taffy's `SizingMode::ContentSize`).
/// A node without size styles answers both the same (`ui_node::measure_content`).
enum class sizing_mode : unsigned char { inherent, content };

class ui_node;

/// What the layout passes did, summed since the counters were last cleared: UI affinity,
/// for tests and `lumen-ui-bench` (G2: a clock-cell change measures one cell's ancestors).
/// `measures` counts calls into a node's `measure` / `measure_content` (cache misses and
/// uncacheable keys); `arranges` calls into `arrange` that `place` did not skip. The hooks,
/// when set, see each such call.
struct layout_counters {
    std::uint64_t measures = 0;
    std::uint64_t measure_hits = 0;
    /// Keys the cache cannot hold (a min neither 0 nor the max), or a full cache pool.
    std::uint64_t measure_uncached = 0;
    std::uint64_t arranges = 0;
    std::uint64_t arrange_skips = 0;
    void (*on_measure)(const ui_node& node) = nullptr;
    void (*on_arrange)(const ui_node& node) = nullptr;
};

[[nodiscard]] layout_counters& ui_layout_counters();

/// Result of routing a point through the tree — window chrome needs to distinguish
/// "content" from "drag me" and "resize me" before the event reaches a widget.
enum class hit_result : unsigned char {
    pass,
    content,
    drag,
    resize_top,
    resize_bottom,
    resize_left,
    resize_right,
};

/// How a `draw_scope::quad` border is stroked.
enum class border_style : std::uint8_t { solid, dashed };

/// A rounded, bordered, solid or gradient rectangle (ADR-010 §4.2 `quad`). Colours are
/// straight alpha in the surface's blend space; `draw_scope` premultiplies them once and
/// folds the scope opacity in.
struct quad_style {
    lx::color background = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    /// Second stop of a two-stop linear gradient; none is a solid quad (`bg1 = bg0`).
    std::optional<lx::color> background_end{};
    /// Gradient direction in degrees, clockwise with y down: 0 runs left → right, 90 top →
    /// bottom (`gfx::ui_cpu_renderer` states the rule every backend follows). CSS's
    /// `linear-gradient(α)` is this angle + 90.
    float gradient_angle_deg = 0.f;
    lx::color border_color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    float corner_radii[4]{};  ///< top-left, top-right, bottom-right, bottom-left, px
    float border_widths[4]{}; ///< top, right, bottom, left, px
    border_style border = border_style::solid;
};

/// A `quad_style` converted once to the instance encoding (ADR-010 §4.5): f16 radii and
/// border widths, premultiplied f16 colours — the fields of `gfx::quad_instance` after its
/// bounds and clip. A retained node whose style rarely changes packs it when it does (in
/// `apply`) and emits the packed form every frame: `draw_scope::quad` then copies it with
/// no conversion at scope opacity 1, and below 1 only rescales the three colours.
struct packed_quad_style {
    std::uint16_t radii[4]{};          ///< f16, as `quad_style::corner_radii`
    std::uint16_t border_widths[4]{};  ///< f16, as `quad_style::border_widths`
    std::uint16_t background[4]{};     ///< premultiplied f16 rgba
    std::uint16_t background_end[4]{}; ///< premultiplied f16 rgba (= `background` when solid)
    std::uint16_t border_color[4]{};   ///< premultiplied f16 rgba
    std::uint16_t gradient_angle{};    ///< f16 degrees
    std::uint8_t border_style = 0;     ///< `gfx::quad_instance::border_style`
    std::uint8_t gradient_space = 0;   ///< `gfx::quad_instance::gradient_space`
};

/// `style` packed with `opacity` folded into each colour's alpha before premultiplying —
/// bit for bit what `draw_scope::quad(bounds, style)` writes in a scope of that opacity.
[[nodiscard]] packed_quad_style pack_quad_style(const quad_style& style, float opacity = 1.f);

/// A blurred rounded rectangle cast by a box, outside it or inset (ADR-010 §4.2 `shadow`).
struct shadow_style {
    lx::color color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    float corner_radii[4]{}; ///< as `quad_style`
    float blur_sigma = 0.f;  ///< Gaussian σ, px; an outer shadow reaches 3σ + spread
    float spread = 0.f;      ///< grows (negative: shrinks) the box before the blur, px
    bool inset = false;
};

/// Draw-emission target. Nodes append `scene::draw_command`s (textured or solid surface
/// draws) and ADR-010 primitives; they never rasterize. The clip/opacity/origin stack is
/// applied on the way in so children cannot escape their parent's bounds: every emitter
/// offsets by the origin, drops what the clip hides, writes the clip as the content mask,
/// multiplies colours by the opacity and takes its order from the list's overlap grid.
class draw_scope {
public:
    draw_scope(lx::scene::draw_list& out, lx::rect2i clip);

    void push_clip(lx::rect2i clip);
    void push_opacity(float opacity);
    void push_origin(lx::point2i delta);
    void pop();

    /// Append a textured quad. Rejected silently when fully clipped away.
    void quad(lx::texture_id texture, lx::rect2i dst, lx::rect2i src, lx::color tint);
    /// Append a solid fill — no texture binding needed on the render side.
    void fill(lx::rect2i dst, lx::color color);

    /// Append a rounded, bordered, solid or gradient rectangle over `bounds`.
    void quad(lx::rect2i bounds, const quad_style& style);
    /// The same from a style packed once (`pack_quad_style` at opacity 1): no half
    /// conversion at scope opacity 1; below it the colours are scaled from their halves,
    /// within one half-float ulp of what the unpacked overload writes.
    void quad(lx::rect2i bounds, const packed_quad_style& style);
    /// Append a shadow cast by `bounds` (what its blur reaches decides visibility).
    void shadow(lx::rect2i bounds, const shadow_style& style);
    /// Append an atlas sprite over `bounds`: `kind` is `mono_sprite`, `poly_sprite` or
    /// `path`, `page` the atlas page, `uv` the tile in atlas texels, `color` the tint of a
    /// mono sprite or path's coverage, the multiplier of a poly sprite. `params` are the
    /// per-corner radii of a `poly_sprite` or the rotation (cos, sin, −sin, cos) of a
    /// `mono_sprite`; all zero means none — square corners, the identity rotation.
    void sprite(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i bounds,
                lx::gfx::sprite_uv uv, lx::color color, std::array<float, 4> params = {});
    /// Append a straight or wavy line of `thickness` px across `bounds`.
    void underline(lx::rect2i bounds, lx::color color, float thickness, bool wavy = false);
    /// A glyph of a text run (`text_block_node`): `sprite` with the colour already packed —
    /// premultiplied halves with the scope opacity folded in by the caller, once per run
    /// (`gfx::premultiplied_half` of the colour at `opacity()` × its alpha) — and no params:
    /// the identity rotation for a `mono_sprite`, square corners for a `poly_sprite`. No half
    /// conversion per glyph.
    void glyph(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i bounds,
               lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color);
    /// The order a group of glyphs will share: the overlap grid's `assign` over `bounds`
    /// (offset by the origin, clipped), which must cover every glyph of the group, and at
    /// least `at_least`. Glyphs of one group (a text line's sprites) need no order among
    /// themselves — they batch in push order — so one grid lookup serves the group; chaining
    /// a block's groups (`at_least` = the previous group's order + 1) keeps them ascending,
    /// which leaves `batch_for_render` nothing to sort.
    [[nodiscard]] std::uint32_t group_order(lx::rect2i bounds, std::uint32_t at_least = 0);
    /// `glyph` with an order from `group_order` instead of one from the grid.
    void glyph(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i bounds,
               lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color, std::uint32_t order);

    [[nodiscard]] lx::rect2i clip() const;
    [[nodiscard]] float opacity() const;
    [[nodiscard]] unsigned emitted() const;
    /// The offset every emitter adds to its bounds (the sum of `push_origin`s).
    [[nodiscard]] lx::point2i origin() const;

private:
    struct frame {
        lx::rect2i clip{};
        float opacity = 1.f;
        lx::point2i origin{};
        /// `clip` as an instance writes it (x, y, w, h floats): converted once per push,
        /// not once per primitive.
        float clip_f[4]{};
    };

    void push_frame(frame f);
    /// Offsets `bounds` by the origin into `placed` and clips it into `visible`; false when
    /// nothing of it is visible.
    [[nodiscard]] bool place(lx::rect2i bounds, lx::rect2i& placed, lx::rect2i& visible) const;
    /// `glyph`'s common tail, `placed` and `visible` from `place`.
    void emit_glyph(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i placed,
                    lx::rect2i visible, lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color,
                    std::uint32_t order);
    /// `quad`'s common tail: `colour_scale` multiplies the packed colours (1: as they are).
    void emit_quad(lx::rect2i placed, lx::rect2i visible, const packed_quad_style& style,
                   float colour_scale);
    [[nodiscard]] frame& top();
    [[nodiscard]] const frame& top() const;

    static constexpr unsigned k_max_depth = 32;

    lx::scene::draw_list* out_ = nullptr;
    frame stack_[k_max_depth]{};
    unsigned depth_ = 1;
    unsigned emitted_ = 0;
};

class focus_manager;

/// What a node is, interaction-wise, and bookkeeping the passes keep on it: 16 bits in what
/// was padding (`sizeof(ui_node)` stays 80). The interaction bits are written by the focus
/// manager and the pointer state (`lx.ui:focus`, `:interact`) — a test may set them directly
/// to draw a state — and read by the node's `emit`.
enum class node_flag : std::uint16_t {
    none = 0,
    focusable = 1u << 0,       ///< takes keyboard focus (tab order, a pointer press)
    focus_scope = 1u << 1,     ///< the root of an active focus trap (a modal, a popover)
    focused = 1u << 2,         ///< has the focus
    focus_ring = 1u << 3,      ///< has it and shows the ring: it came by keyboard (focus-visible)
    hovered = 1u << 4,         ///< the interactive node under the pointer
    pressed = 1u << 5,         ///< pressed by the pointer, not yet released
    interactive = 1u << 6,     ///< takes hover, press and activation
    disabled = 1u << 7,        ///< interactive but inert: no hover, press, focus or activation
    opaque_children = 1u << 8, ///< the reconciler leaves its children alone (a reducer's subtree)
    repaint = 1u << 9,         ///< damage the bounds again when the next arrange lays it out
    referenced = 1u << 10,     ///< a `node_observer` may hold a pointer to it
    hover_intent = 1u << 11,   ///< wants `on_timer(k_timer_hover_intent)` after a 500 ms hover
};

[[nodiscard]] constexpr node_flag operator|(node_flag a, node_flag b) {
    return static_cast<node_flag>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

/// Something that keeps `ui_node` pointers across frames — the focused node, the hovered and
/// pressed ones, a timer's target, a trap's scope — and must forget one when it dies. A node
/// is told to report its death by `ui_node::mark_referenced` (a flag bit; nodes nobody refers
/// to cost nothing). Observers form an intrusive list: registering allocates nothing. UI
/// affinity.
class node_observer {
public:
    node_observer(const node_observer&) = delete;
    node_observer& operator=(const node_observer&) = delete;

    /// `node` is being destroyed (its derived parts already are): drop every pointer to it.
    virtual void node_destroyed(ui_node& node) = 0;

protected:
    node_observer();
    virtual ~node_observer();

private:
    friend class ui_node;
    node_observer* prev_ = nullptr;
    node_observer* next_ = nullptr;
};

/// Retained UI node. Lives across frames and owns its children; descriptors are
/// reconciled onto it rather than replacing it, so scroll offsets, carets, and
/// animation state survive a rebuild.
///
/// The child pointers live out of line, in a span from the UI thread's retained-tree pool
/// (power-of-two sizes up to `k_max_children` slots, recycled through free lists), so a
/// node is its own fields and nothing more: S3's 10 651 nodes fit in cache, where a
/// 64-slot inline array made each one 640 B and the tree 7 MB. The span is taken or grown
/// only when the child count does — a shape change — and returned when the node dies.
class ui_node {
public:
    virtual ~ui_node();

    /// Nodes come from the UI thread's retained-tree pool, with their child spans: dense
    /// and in construction order, the order layout and emit walk them. Sized delete — the
    /// virtual destructor passes the dynamic type's size. A node type must not be
    /// over-aligned (more than 16 bytes).
    [[nodiscard]] static void* operator new(std::size_t bytes);
    static void operator delete(void* node, std::size_t bytes);

    ui_node(const ui_node&) = delete;
    ui_node& operator=(const ui_node&) = delete;

    [[nodiscard]] virtual element_type_id type() const = 0;
    [[nodiscard]] virtual const char* type_name() const;

    /// Pass 1. Must not mutate observable state — it is called repeatedly per pass. A node
    /// measures its children through `measure_cached`.
    [[nodiscard]] virtual lx::size2i measure(layout::constraints c, size_hint hint) const;
    /// Pass 1 for a flex container's item (`sizing_mode::content`): the content's size, the
    /// node's own width, height, min and max not applied. Defaults to `measure`; a node
    /// with size styles (`flex_item_style`) overrides it.
    [[nodiscard]] virtual lx::size2i measure_content(layout::constraints c, size_hint hint) const;
    /// Pass 2. Assigns final geometry and recurses. A node places its children through
    /// `place`.
    virtual void arrange(lx::rect2i bounds);

    /// `measure` (or `measure_content`) through this node's measure cache (ADR-010 §4.7,
    /// Yoga's shape): four `(constraints, hint, mode) → size` entries, replaced round-robin,
    /// cleared by `mark_layout_dirty`. A key whose min is neither 0 nor its max on an axis is
    /// measured without the cache (counted in `layout_counters::measure_uncached`); every
    /// query the layout passes make has one or the other. The entries live out of the node,
    /// in a 72-byte block of the UI thread's measure-cache pool that the node takes the first
    /// time it is measured and gives back when it dies: a node never measured (a leaf its
    /// parent places without asking) costs nothing, and the node stays the 80 bytes the emit
    /// walk reads (G3).
    [[nodiscard]] lx::size2i measure_cached(layout::constraints c, size_hint hint,
                                            sizing_mode mode = sizing_mode::inherent) const;
    /// `arrange` unless the node is clean and `bounds` are its bounds already: a clean
    /// subtree whose place did not move is not walked.
    void place(lx::rect2i bounds);

    /// The node's style as a flex item, read by its parent flex container: null for CSS's
    /// initial values (`flex: 0 1 auto`, auto sizes and margins).
    [[nodiscard]] virtual const layout::flex_style* flex_item_style() const;
    /// Pass 3. Appends draw commands for this node and its children.
    virtual void emit(draw_scope& out) const;

    virtual bool on_mouse(mouse_event event);
    virtual bool on_key(key_event event);
    [[nodiscard]] virtual hit_result hit_test(lx::point2i p) const;

    [[nodiscard]] virtual a11y::node accessibility_node() const;

    // ── Interaction (lx.ui:focus, :interact) ─────────────────────────────────

    [[nodiscard]] bool has_flag(node_flag f) const {
        return (flags_ & static_cast<std::uint16_t>(f)) != 0;
    }
    /// Sets or clears flag bits without side effects: the focus manager's and the pointer
    /// state's (which then call `on_interaction_changed`), a node's own in `apply`
    /// (`focusable`, `interactive`, `disabled`), a test's to draw a state.
    void set_flag(node_flag f, bool on) {
        if (on)
            flags_ = static_cast<std::uint16_t>(flags_ | static_cast<std::uint16_t>(f));
        else
            flags_ = static_cast<std::uint16_t>(flags_ & ~static_cast<std::uint16_t>(f));
    }
    [[nodiscard]] std::uint16_t flags() const { return flags_; }
    /// A `node_observer` is about to keep this node's address: the node reports its death.
    void mark_referenced() { set_flag(node_flag::referenced, true); }

    /// Its place in the tab sequence: 0 in tree order, a positive index before every 0 in
    /// ascending order (HTML's `tabindex`), negative: focusable by pointer, program or arrow
    /// keys but skipped by tab.
    [[nodiscard]] virtual int focus_order() const;
    /// Focus, the ring, hover or press changed on this node (or, for a node painted from an
    /// ancestor's state, on that ancestor). The default damages the bounds.
    virtual void on_interaction_changed();
    /// A key the focus manager routes to the focused node, then to each ancestor until one
    /// takes it (true): activation keys, arrow navigation in a group.
    virtual bool on_focus_key(key_event event, focus_manager& focus);
    /// A click released over the node, or space / enter while it has the focus. False when
    /// it does nothing (no handler, disabled).
    virtual bool activate();
    /// A timer scheduled for this node (`ui_timers::schedule`) came due, on the UI thread at
    /// the start of a frame tick.
    virtual void on_timer(std::uint32_t tag);

    /// The node's look changed with its layout (a label's text): damages its bounds now and
    /// again where the next arrange puts it.
    void mark_repaint();

    static constexpr unsigned k_max_children = 64;

    // ── Tree structure (owned children) ──────────────────────────────────────
    void adopt(ui_node* node);
    void set_child_at(unsigned index, ui_node* node);
    void truncate_children(unsigned count);
    void release_children();

    // ── Reconciler primitives ────────────────────────────────────────────────
    // These move children without destroying them so a keyed rearrangement cannot
    // free a node it is about to reuse. Ownership passes to the caller and back. They do
    // not mark the layout dirty: the caller does, when the children it settles on differ
    // from the ones it took.

    /// Move every child into `out` and leave this node childless. Returns how many.
    [[nodiscard]] unsigned detach_children(ui_node** out, unsigned capacity);
    /// Make room for `count` children (at most `k_max_children`) without changing the
    /// count, so the slots a reconcile is about to fill exist before it fills them.
    void reserve_children(unsigned count);
    /// Assign a slot without deleting the previous occupant.
    void place_child(unsigned index, ui_node* node);
    void set_child_count(unsigned count);

    [[nodiscard]] unsigned child_count() const;
    [[nodiscard]] ui_node* child_at(unsigned index) const;
    [[nodiscard]] ui_node* parent() const;

    // ── Identity / state ─────────────────────────────────────────────────────
    void set_key(element_key key);
    [[nodiscard]] element_key key() const;

    void set_visible(bool visible);
    [[nodiscard]] bool visible() const;

    void set_style(style_ref style);
    [[nodiscard]] style_ref style() const;

    [[nodiscard]] lx::rect2i bounds() const;

    /// Layout must run again before the next emit: clears this node's measure cache and
    /// marks it and its ancestors dirty (their measurements depended on it; the walk stops
    /// below an ancestor already dirty), so the layout passes walk exactly the path down to
    /// it. Called by `invalidate_layout`, by the reconciler when props or children change,
    /// and by a node whose measure inputs did.
    void mark_layout_dirty();
    [[nodiscard]] bool layout_dirty() const;
    void clear_layout_dirty();

protected:
    ui_node() = default;

    ui_node* parent_ = nullptr;
    /// `child_capacity_` slots from the retained-tree pool; null while it is zero. Slots at
    /// and past `child_count_` hold null or a pointer the reconciler has yet to count.
    ui_node** children_ = nullptr;
    unsigned child_count_ = 0;
    unsigned child_capacity_ = 0;
    element_key key_ = 0;
    lx::rect2i bounds_{};
    style_ref style_{};
    bool visible_ = true;
    bool layout_dirty_ = true;
    /// `node_flag` bits, in what was padding.
    std::uint16_t flags_ = 0;

private:
    /// The node's block in the measure-cache pool, 1-based; 0: none yet. It fills what was
    /// padding after the flags.
    mutable std::uint32_t measure_slot_ = 0;
};

/// Describe a retained node of type `NodeT` carrying `PropsT`. Props are copied into
/// the frame arena; `apply` writes them onto the retained node during reconciliation.
///
/// An `apply` that returns `bool` says whether the props changed anything layout reads
/// (true: the reconciler marks the node's layout dirty; false: nothing did, or the node
/// marked itself); one that returns nothing is taken to have changed it every time.
template<typename NodeT, typename PropsT>
[[nodiscard]] child describe(const PropsT& props, element_key key = 0) {
    build_context* ctx = build_context::current();
    if (!ctx)
        return nullptr;
    auto* stored = ctx->alloc<PropsT>();
    auto* node = ctx->alloc<element>();
    if (!stored || !node)
        return nullptr;
    *stored = props;
    node->type = NodeT::static_type();
    node->key = key;
    node->props = stored;
    node->construct = []() -> ui_node* { return new NodeT{}; };
    node->apply = [](ui_node& target, const void* raw) {
        auto& n = static_cast<NodeT&>(target);
        const PropsT& p = *static_cast<const PropsT*>(raw);
        // A layout change is taken to change the look too: the node is damaged where it was
        // and where the arrange puts it. A paint-only change damages itself in `apply`.
        if constexpr (std::is_same_v<decltype(n.apply(p)), bool>) {
            if (n.apply(p)) {
                n.mark_layout_dirty();
                n.mark_repaint();
            }
        } else {
            n.apply(p);
            n.mark_layout_dirty();
            n.mark_repaint();
        }
    };
    return node;
}

/// Attach children to an already-described element.
[[nodiscard]] child with_children(child parent, std::span<const child> items);

} // namespace lx::ui

namespace lx::ui::detail {
/// Adds `region` to the UI thread's damage ledger (`ui_damage`, defined in :invalidate,
/// which this partition cannot import).
void damage_rect(lx::rect2i region);
} // namespace lx::ui::detail


// ── draw_scope ──────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] lx::rect2i lx_ui_intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    const int y1 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

/// `c` at `opacity` times its own alpha (straight alpha in, straight alpha out).
[[nodiscard]] lx::color lx_ui_faded(lx::color c, float opacity) {
    c.a *= opacity;
    return c;
}

/// `c` premultiplied, as half floats: the instance encoding (ADR-010 §4.5).
void lx_ui_write_rgba(std::uint16_t (&out)[4], lx::color c, float opacity) {
    const lx::gfx::packed_rgba p = lx::gfx::premultiplied_half(lx_ui_faded(c, opacity));
    out[0] = p.r;
    out[1] = p.g;
    out[2] = p.b;
    out[3] = p.a;
}

void lx_ui_write_rect(float (&out)[4], lx::rect2i r) {
    out[0] = static_cast<float>(r.x);
    out[1] = static_cast<float>(r.y);
    out[2] = static_cast<float>(r.width);
    out[3] = static_cast<float>(r.height);
}

void lx_ui_write_half4(std::uint16_t (&out)[4], const float (&v)[4]) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = lx::gfx::to_half(v[i]);
}

void lx_ui_copy_half4(std::uint16_t (&out)[4], const std::uint16_t (&in)[4]) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = in[i];
}

/// A premultiplied half colour at `scale` × its coverage: every channel scales alike.
void lx_ui_scale_half4(std::uint16_t (&out)[4], const std::uint16_t (&in)[4], float scale) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = lx::gfx::to_half(lx::gfx::from_half(in[i]) * scale);
}

} // namespace

lx::ui::packed_quad_style lx::ui::pack_quad_style(const quad_style& style, float opacity) {
    packed_quad_style p{};
    lx_ui_write_half4(p.radii, style.corner_radii);
    lx_ui_write_half4(p.border_widths, style.border_widths);
    lx_ui_write_rgba(p.background, style.background, opacity);
    lx_ui_write_rgba(p.background_end,
                     style.background_end ? *style.background_end : style.background, opacity);
    lx_ui_write_rgba(p.border_color, style.border_color, opacity);
    p.gradient_angle = lx::gfx::to_half(style.gradient_angle_deg);
    p.border_style = style.border == border_style::dashed ? 1 : 0;
    return p;
}

lx::ui::draw_scope::draw_scope(lx::scene::draw_list& out, lx::rect2i clip) : out_{&out} {
    // Every emitter writes the list's overlap grid, which is UI affinity (ADR-010 §4.12).
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    stack_[0] = {clip, 1.f, {}, {}};
    lx_ui_write_rect(stack_[0].clip_f, clip);
}

void lx::ui::draw_scope::push_frame(frame f) {
    if (depth_ >= k_max_depth) {
        // Deeper nesting than the fixed stack allows: keep the innermost frame rather
        // than growing, so a pathological tree cannot allocate during a frame.
        stack_[k_max_depth - 1] = f;
        return;
    }
    stack_[depth_++] = f;
}

lx::ui::draw_scope::frame& lx::ui::draw_scope::top() { return stack_[depth_ - 1]; }
const lx::ui::draw_scope::frame& lx::ui::draw_scope::top() const { return stack_[depth_ - 1]; }

void lx::ui::draw_scope::push_clip(lx::rect2i clip) {
    frame next = top();
    next.clip = lx_ui_intersect(next.clip, clip);
    lx_ui_write_rect(next.clip_f, next.clip);
    push_frame(next);
}

void lx::ui::draw_scope::push_opacity(float opacity) {
    frame next = top();
    next.opacity *= opacity;
    push_frame(next);
}

void lx::ui::draw_scope::push_origin(lx::point2i delta) {
    frame next = top();
    next.origin = next.origin + delta;
    push_frame(next);
}

void lx::ui::draw_scope::pop() {
    if (depth_ > 1)
        --depth_;
}

void lx::ui::draw_scope::quad(lx::texture_id texture, lx::rect2i dst, lx::rect2i src,
                              lx::color tint) {
    if (!out_)
        return;
    const frame& f = top();
    lx::rect2i placed = dst;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    const lx::rect2i visible = lx_ui_intersect(placed, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::scene::draw_command cmd{};
    cmd.texture = texture;
    cmd.src = src;
    cmd.dst = placed;
    cmd.clip = f.clip;
    cmd.tint = tint;
    cmd.opacity = f.opacity;
    cmd.sort_key = lx::draw_sort_key::make(1, out_->size());
    out_->push(cmd, out_->orders().assign(visible));
    ++emitted_;
}

void lx::ui::draw_scope::fill(lx::rect2i dst, lx::color color) {
    if (!out_)
        return;
    const frame& f = top();
    lx::rect2i placed = dst;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    const lx::rect2i visible = lx_ui_intersect(placed, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    // A solid draw (`draw_command::solid`, what every backend draws for a single-pixel
    // buffer): no texture, the colour premultiplied with the scope opacity folded in once
    // here (ADR-010 §4.5), so `opacity` and `tint` stay neutral.
    lx::scene::draw_command cmd{};
    cmd.dst = placed;
    cmd.clip = f.clip;
    cmd.sort_key = lx::draw_sort_key::make(1, out_->size());
    cmd.solid = true;
    const float a = color.a * f.opacity;
    cmd.solid_color = lx::color::encoded(color.r * a, color.g * a, color.b * a, a, color.space,
                                         color.encoding);
    out_->push(cmd, out_->orders().assign(visible));
    ++emitted_;
}

bool lx::ui::draw_scope::place(lx::rect2i bounds, lx::rect2i& placed,
                               lx::rect2i& visible) const {
    const frame& f = top();
    placed = bounds;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    visible = lx_ui_intersect(placed, f.clip);
    return visible.width > 0 && visible.height > 0;
}

void lx::ui::draw_scope::quad(lx::rect2i bounds, const quad_style& style) {
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!out_ || !place(bounds, placed, visible))
        return;
    // The scope opacity is folded in at packing, before the premultiply: no rescale after.
    emit_quad(placed, visible, pack_quad_style(style, top().opacity), 1.f);
}

void lx::ui::draw_scope::quad(lx::rect2i bounds, const packed_quad_style& style) {
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!out_ || !place(bounds, placed, visible))
        return;
    emit_quad(placed, visible, style, top().opacity);
}

void lx::ui::draw_scope::emit_quad(lx::rect2i placed, lx::rect2i visible,
                                   const packed_quad_style& style, float colour_scale) {
    const frame& f = top();
    const std::uint32_t order = out_->orders().assign(visible);
    ++emitted_;
    // Written in place in the list's instance array: no staged instance, no copy.
    lx::gfx::quad_instance* q = out_->append_quad(visible, order);
    if (!q)
        return;
    lx_ui_write_rect(q->bounds, placed);
    for (unsigned i = 0; i < 4; ++i)
        q->clip[i] = f.clip_f[i];
    if (colour_scale == 1.f) {
        // The packed style is the instance from `radii` up to `pad`, field for field.
        using inst = lx::gfx::quad_instance;
        constexpr std::size_t base = offsetof(inst, radii);
        static_assert(sizeof(packed_quad_style) == offsetof(inst, pad) - base);
        static_assert(offsetof(packed_quad_style, border_widths) == offsetof(inst, border_widths) - base &&
                      offsetof(packed_quad_style, background) == offsetof(inst, background) - base &&
                      offsetof(packed_quad_style, background_end) == offsetof(inst, background_end) - base &&
                      offsetof(packed_quad_style, border_color) == offsetof(inst, border_color) - base &&
                      offsetof(packed_quad_style, gradient_angle) == offsetof(inst, gradient_angle) - base &&
                      offsetof(packed_quad_style, border_style) == offsetof(inst, border_style) - base &&
                      offsetof(packed_quad_style, gradient_space) == offsetof(inst, gradient_space) - base);
        std::memcpy(q->radii, &style, sizeof(packed_quad_style));
    } else {
        lx_ui_copy_half4(q->radii, style.radii);
        lx_ui_copy_half4(q->border_widths, style.border_widths);
        lx_ui_scale_half4(q->background, style.background, colour_scale);
        lx_ui_scale_half4(q->background_end, style.background_end, colour_scale);
        lx_ui_scale_half4(q->border_color, style.border_color, colour_scale);
        q->gradient_angle = style.gradient_angle;
        q->border_style = style.border_style;
        q->gradient_space = style.gradient_space;
    }
    q->pad = 0;
}

void lx::ui::draw_scope::shadow(lx::rect2i bounds, const shadow_style& style) {
    if (!out_)
        return;
    const frame& f = top();
    lx::rect2i placed = bounds;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    // An outer shadow is visible wherever its blur reaches, not only under the box.
    lx::rect2i extent = placed;
    if (!style.inset) {
        const float reach_px = 3.f * style.blur_sigma + style.spread;
        const int reach = reach_px > 0.f ? static_cast<int>(std::ceil(reach_px)) : 0;
        extent = {placed.x - reach, placed.y - reach, placed.width + 2 * reach,
                  placed.height + 2 * reach};
    }
    const lx::rect2i visible = lx_ui_intersect(extent, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::shadow_instance s{};
    lx_ui_write_rect(s.bounds, placed);
    lx_ui_write_rect(s.clip, f.clip);
    lx_ui_write_half4(s.radii, style.corner_radii);
    lx_ui_write_rgba(s.color, style.color, f.opacity);
    s.sigma = style.blur_sigma;
    s.spread = style.spread;
    s.flags = style.inset ? 1u : 0u;
    out_->push_shadow(s, visible, out_->orders().assign(visible));
    ++emitted_;
}

void lx::ui::draw_scope::sprite(lx::gfx::primitive_kind kind, std::uint8_t page,
                                lx::rect2i bounds, lx::gfx::sprite_uv uv, lx::color color,
                                std::array<float, 4> params) {
    if (!out_)
        return;
    if (kind != lx::gfx::primitive_kind::mono_sprite &&
        kind != lx::gfx::primitive_kind::poly_sprite && kind != lx::gfx::primitive_kind::path)
        return;
    const frame& f = top();
    lx::rect2i placed = bounds;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    const lx::rect2i visible = lx_ui_intersect(placed, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::sprite_instance s{};
    lx_ui_write_rect(s.bounds, placed);
    lx_ui_write_rect(s.clip, f.clip);
    s.uv[0] = uv.u0;
    s.uv[1] = uv.v0;
    s.uv[2] = uv.u1;
    s.uv[3] = uv.v1;
    s.page = page;
    lx_ui_write_rgba(s.color, color, f.opacity);
    // A zero rotation matrix collapses a glyph and means nothing: it is "no rotation".
    if (kind == lx::gfx::primitive_kind::mono_sprite && params[0] == 0.f && params[1] == 0.f &&
        params[2] == 0.f && params[3] == 0.f)
        params = {1.f, 0.f, 0.f, 1.f};
    for (unsigned i = 0; i < 4; ++i)
        s.params[i] = lx::gfx::to_half(params[i]);
    out_->push_sprite(kind, page, s, visible, out_->orders().assign(visible));
    ++emitted_;
}

void lx::ui::draw_scope::underline(lx::rect2i bounds, lx::color color, float thickness,
                                   bool wavy) {
    if (!out_)
        return;
    const frame& f = top();
    lx::rect2i placed = bounds;
    placed.x += f.origin.x;
    placed.y += f.origin.y;
    const lx::rect2i visible = lx_ui_intersect(placed, f.clip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::underline_instance u{};
    lx_ui_write_rect(u.bounds, placed);
    lx_ui_write_rect(u.clip, f.clip);
    lx_ui_write_rgba(u.color, color, f.opacity);
    u.thickness = lx::gfx::to_half(thickness);
    u.wavy = wavy ? 1 : 0;
    out_->push_underline(u, visible, out_->orders().assign(visible));
    ++emitted_;
}

void lx::ui::draw_scope::glyph(lx::gfx::primitive_kind kind, std::uint8_t page,
                               lx::rect2i bounds, lx::gfx::sprite_uv uv,
                               lx::gfx::packed_rgba color) {
    if (!out_ || (kind != lx::gfx::primitive_kind::mono_sprite &&
                  kind != lx::gfx::primitive_kind::poly_sprite))
        return;
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!place(bounds, placed, visible))
        return;
    emit_glyph(kind, page, placed, visible, uv, color, out_->orders().assign(visible));
}

std::uint32_t lx::ui::draw_scope::group_order(lx::rect2i bounds, std::uint32_t at_least) {
    if (!out_)
        return 0;
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!place(bounds, placed, visible))
        return out_->orders().layer_base();
    return out_->orders().assign(visible, at_least);
}

void lx::ui::draw_scope::glyph(lx::gfx::primitive_kind kind, std::uint8_t page,
                               lx::rect2i bounds, lx::gfx::sprite_uv uv,
                               lx::gfx::packed_rgba color, std::uint32_t order) {
    if (!out_ || (kind != lx::gfx::primitive_kind::mono_sprite &&
                  kind != lx::gfx::primitive_kind::poly_sprite))
        return;
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!place(bounds, placed, visible))
        return;
    emit_glyph(kind, page, placed, visible, uv, color, order);
}

void lx::ui::draw_scope::emit_glyph(lx::gfx::primitive_kind kind, std::uint8_t page,
                                    lx::rect2i placed, lx::rect2i visible,
                                    lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color,
                                    std::uint32_t order) {
    const frame& f = top();
    lx::gfx::sprite_instance s{};
    lx_ui_write_rect(s.bounds, placed);
    lx_ui_write_rect(s.clip, f.clip);
    s.uv[0] = uv.u0;
    s.uv[1] = uv.v0;
    s.uv[2] = uv.u1;
    s.uv[3] = uv.v1;
    s.page = page;
    s.color[0] = color.r;
    s.color[1] = color.g;
    s.color[2] = color.b;
    s.color[3] = color.a;
    if (kind == lx::gfx::primitive_kind::mono_sprite) {
        s.params[0] = 0x3C00u; // f16 1: the identity rotation (cos, sin, −sin, cos)
        s.params[3] = 0x3C00u;
    }
    out_->push_sprite(kind, page, s, visible, order);
    ++emitted_;
}

lx::point2i lx::ui::draw_scope::origin() const { return top().origin; }
lx::rect2i lx::ui::draw_scope::clip() const { return top().clip; }
float lx::ui::draw_scope::opacity() const { return top().opacity; }
unsigned lx::ui::draw_scope::emitted() const { return emitted_; }

// ── retained-tree pool ──────────────────────────────────────────────────────────

namespace {

/// The storage of every retained node on the UI thread and of their child-pointer spans,
/// in 16-byte size classes up to 512 bytes (larger blocks go to the global heap). A
/// released block goes on its class's free list and the next block of that class is that
/// one, so a tree whose shape changes recycles storage and one that keeps its shape
/// touches none (ADR-010 G5). Fresh blocks are carved from 64 KB chunks in the order the
/// reconciler takes them — a node, its span, its first child … — which is the order
/// layout and emit walk the tree: one dense run with no allocator headers between nodes.
///
/// UI affinity owns retained trees (ui-architecture.md §8), so there is one pool and no
/// lock. It is never destroyed: a retained tree may outlive any static it could be torn
/// down with, and the chunks stay reachable from it.
class lx_ui_tree_pool {
public:
    static constexpr std::size_t k_grain = 16;
    static constexpr std::size_t k_max_block = 512;

    [[nodiscard]] void* take(std::size_t bytes) {
        if (bytes > k_max_block)
            return ::operator new(bytes);
        const std::size_t cls = class_of(bytes);
        if (void* block = free_[cls]) {
            free_[cls] = *static_cast<void**>(block);
            return block;
        }
        const std::size_t size = (cls + 1) * k_grain;
        if (static_cast<std::size_t>(end_ - next_) < size) {
            // A new chunk: its first word links it to the previous one, so the pool keeps
            // every chunk reachable. The rest of the old chunk is left unused.
            auto* chunk = static_cast<std::byte*>(::operator new(k_chunk_bytes));
            *reinterpret_cast<void**>(chunk) = chunks_;
            chunks_ = chunk;
            next_ = chunk + k_grain;
            end_ = chunk + k_chunk_bytes;
        }
        void* block = next_;
        next_ += size;
        return block;
    }

    void give_back(void* block, std::size_t bytes) {
        if (!block)
            return;
        if (bytes > k_max_block) {
            ::operator delete(block);
            return;
        }
        const std::size_t cls = class_of(bytes);
        *static_cast<void**>(block) = free_[cls];
        free_[cls] = block;
    }

private:
    static constexpr std::size_t k_chunk_bytes = 64 * 1024;
    static constexpr std::size_t k_classes = k_max_block / k_grain;
    static_assert(alignof(std::max_align_t) <= k_grain);

    [[nodiscard]] static std::size_t class_of(std::size_t bytes) {
        return bytes == 0 ? 0 : (bytes - 1) / k_grain;
    }

    void* free_[k_classes]{};
    void* chunks_ = nullptr;
    std::byte* next_ = nullptr;
    std::byte* end_ = nullptr;
};

[[nodiscard]] lx_ui_tree_pool& lx_ui_tree_storage() {
    static lx_ui_tree_pool* const pool = new lx_ui_tree_pool{};
    return *pool;
}

/// A child span of `capacity` slots (a power of two, ≤ `ui_node::k_max_children`), in bytes.
[[nodiscard]] std::size_t lx_ui_span_bytes(unsigned capacity) {
    return std::size_t{capacity} * sizeof(lx::ui::ui_node*);
}

} // namespace

namespace {

/// UI affinity, like the trees it counts.
lx::ui::layout_counters g_layout_counters{};

/// One node's measure cache. A tag is 0 when its entry is empty, else bit 0 set, the hint
/// in bits 1–2, bit 3 / 4 set when the key's min width / height equals its max (else that
/// min is 0), bit 5 for `sizing_mode::content`.
struct lx_ui_measure_block {
    static constexpr unsigned k_entries = 4;
    struct entry {
        int max_width;
        int max_height;
        int width;
        int height;
    };
    std::uint8_t tags[k_entries];
    std::uint8_t next;
    entry entries[k_entries];
};
static_assert(sizeof(lx_ui_measure_block) == 72);

/// The UI thread's measure caches: blocks in 1 024-block chunks, recycled through a free
/// list (linked through the first entry), so a tree that keeps its shape takes none. At
/// most 4 096 chunks (4 Mi measured nodes alive at once); past that a node goes uncached
/// (`layout_counters::measure_uncached`), the overflow rule of memory-management.md §3.
/// Never destroyed, like the tree pool: a retained tree may outlive any static.
class lx_ui_measure_pool {
public:
    static constexpr std::uint32_t k_per_chunk = 1024;
    static constexpr std::uint32_t k_max_chunks = 4096;

    /// A cleared block, 1-based; 0 when the pool is full.
    [[nodiscard]] std::uint32_t take() {
        std::uint32_t slot = free_;
        if (slot != 0) {
            free_ = static_cast<std::uint32_t>(at(slot).entries[0].max_width);
        } else {
            if (used_ % k_per_chunk == 0) {
                const std::uint32_t chunk = used_ / k_per_chunk;
                if (chunk >= k_max_chunks)
                    return 0;
                chunks_[chunk] = static_cast<lx_ui_measure_block*>(
                    ::operator new(sizeof(lx_ui_measure_block) * k_per_chunk));
            }
            slot = ++used_;
        }
        lx_ui_measure_block& b = at(slot);
        b.tags[0] = b.tags[1] = b.tags[2] = b.tags[3] = 0;
        b.next = 0;
        return slot;
    }

    void give_back(std::uint32_t slot) {
        at(slot).entries[0].max_width = static_cast<int>(free_);
        free_ = slot;
    }

    [[nodiscard]] lx_ui_measure_block& at(std::uint32_t slot) {
        return chunks_[(slot - 1) / k_per_chunk][(slot - 1) % k_per_chunk];
    }

private:
    lx_ui_measure_block* chunks_[k_max_chunks]{};
    std::uint32_t used_ = 0;
    std::uint32_t free_ = 0;
};

[[nodiscard]] lx_ui_measure_pool& lx_ui_measure_storage() {
    static lx_ui_measure_pool* const pool = new lx_ui_measure_pool{};
    return *pool;
}

} // namespace

// ── ui_node ─────────────────────────────────────────────────────────────────────

void* lx::ui::ui_node::operator new(std::size_t bytes) { return lx_ui_tree_storage().take(bytes); }

void lx::ui::ui_node::operator delete(void* node, std::size_t bytes) {
    lx_ui_tree_storage().give_back(node, bytes);
}

namespace {
/// The UI thread's node observers, an intrusive list (no allocation to register).
lx::ui::node_observer* g_node_observers = nullptr;
} // namespace

lx::ui::node_observer::node_observer() {
    next_ = g_node_observers;
    if (next_)
        next_->prev_ = this;
    g_node_observers = this;
}

lx::ui::node_observer::~node_observer() {
    if (prev_)
        prev_->next_ = next_;
    else if (g_node_observers == this)
        g_node_observers = next_;
    if (next_)
        next_->prev_ = prev_;
    prev_ = next_ = nullptr;
}

lx::ui::ui_node::~ui_node() {
    if (has_flag(node_flag::referenced)) {
        // Each observer drops its pointers; one may unregister another, so walk carefully.
        for (node_observer* o = g_node_observers; o;) {
            node_observer* next = o->next_;
            o->node_destroyed(*this);
            o = next;
        }
    }
    release_children();
    lx_ui_tree_storage().give_back(children_, lx_ui_span_bytes(child_capacity_));
    if (measure_slot_ != 0)
        lx_ui_measure_storage().give_back(measure_slot_);
}

void lx::ui::ui_node::reserve_children(unsigned count) {
    if (count <= child_capacity_)
        return;
    if (count > k_max_children)
        count = k_max_children;
    const unsigned capacity = std::bit_ceil(count);
    lx_ui_tree_pool& pool = lx_ui_tree_storage();
    auto** grown = static_cast<ui_node**>(pool.take(lx_ui_span_bytes(capacity)));
    for (unsigned i = 0; i < capacity; ++i)
        grown[i] = i < child_capacity_ ? children_[i] : nullptr;
    pool.give_back(children_, lx_ui_span_bytes(child_capacity_));
    children_ = grown;
    child_capacity_ = capacity;
}

const char* lx::ui::ui_node::type_name() const { return "ui_node"; }

lx::ui::layout_counters& lx::ui::ui_layout_counters() {
    return g_layout_counters;
}

lx::size2i lx::ui::ui_node::measure_cached(layout::constraints c, size_hint hint,
                                           sizing_mode mode) const {
    layout_counters& k = g_layout_counters;
    auto tag = static_cast<std::uint8_t>(1u | (static_cast<unsigned>(hint) << 1) |
                                         (mode == sizing_mode::content ? 0x20u : 0u));
    bool cacheable = true;
    if (c.min.width == c.max.width)
        tag = static_cast<std::uint8_t>(tag | 0x08u);
    else if (c.min.width != 0)
        cacheable = false;
    if (c.min.height == c.max.height)
        tag = static_cast<std::uint8_t>(tag | 0x10u);
    else if (c.min.height != 0)
        cacheable = false;
    lx_ui_measure_pool& pool = lx_ui_measure_storage();
    if (cacheable && measure_slot_ == 0)
        measure_slot_ = pool.take(); // the first measurement of this node: a layout change
    if (cacheable && measure_slot_ != 0) {
        const lx_ui_measure_block& b = pool.at(measure_slot_);
        for (unsigned e = 0; e < lx_ui_measure_block::k_entries; ++e) {
            const lx_ui_measure_block::entry& m = b.entries[e];
            if (b.tags[e] == tag && m.max_width == c.max.width && m.max_height == c.max.height) {
                ++k.measure_hits;
                return {m.width, m.height};
            }
        }
    } else {
        ++k.measure_uncached;
    }
    ++k.measures;
    if (k.on_measure)
        k.on_measure(*this);
    const lx::size2i size =
        mode == sizing_mode::content ? measure_content(c, hint) : measure(c, hint);
    if (cacheable && measure_slot_ != 0) {
        // Measuring children never takes this node's block, so `b` is still this node's.
        lx_ui_measure_block& b = pool.at(measure_slot_);
        const unsigned e = b.next;
        b.next = static_cast<std::uint8_t>((e + 1u) % lx_ui_measure_block::k_entries);
        b.entries[e] = {c.max.width, c.max.height, size.width, size.height};
        b.tags[e] = tag;
    }
    return size;
}

void lx::ui::ui_node::place(lx::rect2i bounds) {
    layout_counters& k = g_layout_counters;
    if (!layout_dirty_ && bounds.x == bounds_.x && bounds.y == bounds_.y &&
        bounds.width == bounds_.width && bounds.height == bounds_.height) {
        ++k.arrange_skips;
        return;
    }
    ++k.arranges;
    if (k.on_arrange)
        k.on_arrange(*this);
    const lx::rect2i before = bounds_;
    arrange(bounds);
    layout_dirty_ = false;
    // A node the layout moved repaints where it was and where it is; one whose look changed
    // with its layout (`mark_repaint`) where it is. An ancestor re-arranged in place is not
    // damaged: its own pixels did not change.
    const bool moved = before.x != bounds.x || before.y != bounds.y ||
                       before.width != bounds.width || before.height != bounds.height;
    if (moved || has_flag(node_flag::repaint)) {
        if (moved)
            detail::damage_rect(before);
        detail::damage_rect(bounds);
        set_flag(node_flag::repaint, false);
    }
}

void lx::ui::ui_node::mark_repaint() {
    detail::damage_rect(bounds_);
    set_flag(node_flag::repaint, true);
}

int lx::ui::ui_node::focus_order() const { return 0; }

void lx::ui::ui_node::on_interaction_changed() { detail::damage_rect(bounds_); }

bool lx::ui::ui_node::on_focus_key(key_event, focus_manager&) { return false; }

bool lx::ui::ui_node::activate() { return false; }

void lx::ui::ui_node::on_timer(std::uint32_t) {}

lx::size2i lx::ui::ui_node::measure_content(layout::constraints c, size_hint hint) const {
    return measure(c, hint);
}

const lx::layout::flex_style* lx::ui::ui_node::flex_item_style() const {
    return nullptr;
}

lx::size2i lx::ui::ui_node::measure(layout::constraints c, size_hint hint) const {
    // Default: wrap the largest child measurement, clamped to the constraint range.
    lx::size2i out{};
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i])
            continue;
        const auto child_size = children_[i]->measure_cached(c, hint);
        if (child_size.width > out.width)
            out.width = child_size.width;
        if (child_size.height > out.height)
            out.height = child_size.height;
    }
    if (out.width < c.min.width) out.width = c.min.width;
    if (out.height < c.min.height) out.height = c.min.height;
    if (out.width > c.max.width) out.width = c.max.width;
    if (out.height > c.max.height) out.height = c.max.height;
    return out;
}

void lx::ui::ui_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i])
            children_[i]->place(bounds);
    }
}

void lx::ui::ui_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i])
            children_[i]->emit(out);
    }
}

bool lx::ui::ui_node::on_mouse(mouse_event event) {
    // Topmost-first: later siblings paint over earlier ones, so they get the event first.
    for (unsigned i = child_count_; i > 0; --i) {
        ui_node* c = children_[i - 1];
        if (!c || !c->visible_)
            continue;
        if (c->bounds_.contains(event.position) && c->on_mouse(event))
            return true;
    }
    return false;
}

bool lx::ui::ui_node::on_key(key_event event) {
    for (unsigned i = 0; i < child_count_; ++i) {
        if (children_[i] && children_[i]->on_key(event))
            return true;
    }
    return false;
}

lx::ui::hit_result lx::ui::ui_node::hit_test(lx::point2i p) const {
    for (unsigned i = child_count_; i > 0; --i) {
        const ui_node* c = children_[i - 1];
        if (!c || !c->visible_ || !c->bounds_.contains(p))
            continue;
        if (const auto hit = c->hit_test(p); hit != hit_result::pass)
            return hit;
    }
    return hit_result::pass;
}

lx::a11y::node lx::ui::ui_node::accessibility_node() const {
    return {a11y::role::unknown, "", "", "", 0, const_cast<ui_node*>(this)};
}

void lx::ui::ui_node::adopt(ui_node* node) {
    if (!node || child_count_ >= k_max_children)
        return;
    reserve_children(child_count_ + 1);
    node->parent_ = this;
    children_[child_count_++] = node;
    mark_layout_dirty();
}

void lx::ui::ui_node::set_child_at(unsigned index, ui_node* node) {
    if (index >= k_max_children)
        return;
    reserve_children(index + 1);
    if (children_[index] && children_[index] != node)
        delete children_[index];
    children_[index] = node;
    if (node)
        node->parent_ = this;
    if (index >= child_count_)
        child_count_ = index + 1;
    mark_layout_dirty();
}

void lx::ui::ui_node::truncate_children(unsigned count) {
    for (unsigned i = count; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    if (count < child_count_) {
        child_count_ = count;
        mark_layout_dirty();
    }
}

void lx::ui::ui_node::release_children() {
    for (unsigned i = 0; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    child_count_ = 0;
}

unsigned lx::ui::ui_node::detach_children(ui_node** out, unsigned capacity) {
    if (!out)
        return 0;
    const unsigned moved = child_count_ < capacity ? child_count_ : capacity;
    for (unsigned i = 0; i < moved; ++i) {
        out[i] = children_[i];
        children_[i] = nullptr;
    }
    // Anything beyond the caller's capacity would leak, so destroy it here.
    for (unsigned i = moved; i < child_count_; ++i) {
        delete children_[i];
        children_[i] = nullptr;
    }
    child_count_ = 0;
    return moved;
}

void lx::ui::ui_node::place_child(unsigned index, ui_node* node) {
    if (index >= k_max_children)
        return;
    reserve_children(index + 1);
    children_[index] = node;
    if (node)
        node->parent_ = this;
}

void lx::ui::ui_node::set_child_count(unsigned count) {
    reserve_children(count);
    child_count_ = count < k_max_children ? count : k_max_children;
}

unsigned lx::ui::ui_node::child_count() const { return child_count_; }

lx::ui::ui_node* lx::ui::ui_node::child_at(unsigned index) const {
    return index < child_count_ ? children_[index] : nullptr;
}

lx::ui::ui_node* lx::ui::ui_node::parent() const { return parent_; }

void lx::ui::ui_node::set_key(element_key key) { key_ = key; }
lx::ui::element_key lx::ui::ui_node::key() const { return key_; }
void lx::ui::ui_node::set_visible(bool visible) { visible_ = visible; }
bool lx::ui::ui_node::visible() const { return visible_; }
void lx::ui::ui_node::set_style(style_ref style) { style_ = style; }
lx::ui::style_ref lx::ui::ui_node::style() const { return style_; }
lx::rect2i lx::ui::ui_node::bounds() const { return bounds_; }

void lx::ui::ui_node::mark_layout_dirty() {
    // Every ancestor's measurement depended on this one. The walk stops below an ancestor
    // already dirty (Yoga's rule): the passes keep "dirty ⇒ parent dirty" — a node is
    // cleaned only by its own arrange, its parent's arrange places every child, and the
    // reconciler marks a parent whose children changed — so that ancestor's own ancestors
    // are dirty and their caches empty already. 10 000 leaves of one parent cost one walk.
    ui_node* n = this;
    for (;;) {
        n->layout_dirty_ = true;
        if (n->measure_slot_ != 0) {
            lx_ui_measure_block& b = lx_ui_measure_storage().at(n->measure_slot_);
            b.tags[0] = b.tags[1] = b.tags[2] = b.tags[3] = 0;
        }
        if (n->parent_ == nullptr || n->parent_->layout_dirty_)
            break;
        n = n->parent_;
    }
}
bool lx::ui::ui_node::layout_dirty() const { return layout_dirty_; }
void lx::ui::ui_node::clear_layout_dirty() { layout_dirty_ = false; }

lx::ui::child lx::ui::with_children(child parent, std::span<const child> items) {
    const auto count = static_cast<unsigned>(items.size());
    if (!parent || count == 0)
        return parent;
    build_context* ctx = build_context::current();
    if (!ctx)
        return parent;
    const element** slots = ctx->alloc_children(count);
    if (!slots)
        return parent;
    for (unsigned i = 0; i < count; ++i)
        slots[i] = items[i];
    // Descriptors are arena-owned and single-use, so patching in place is safe.
    auto* mutable_parent = const_cast<element*>(parent);
    mutable_parent->children = slots;
    mutable_parent->child_count = count;
    return parent;
}
