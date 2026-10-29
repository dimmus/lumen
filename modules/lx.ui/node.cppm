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

/// The percent base of the node being measured or arranged: its parent's content box, NaN
/// where it is indefinite — what CSS resolves the node's own percentage padding, border,
/// size, min and max against (`layout::flex_input::parent_*`). A parent sets it for its
/// children with a `percent_base_scope`: a flex container its content box (from the
/// engine's `child_query::parent_*` while it measures, its arranged inner size while it
/// places), a decorator, scroll area, list or overlay layer the box it lays its children
/// out in; a wrapper that is its child's box (opacity, clip, grow, sizing, a reducer)
/// leaves it as it is. Outside any scope (`ui_root` sets the window's) it is NaN. A
/// measurement that read it (`note_percent_base_read`, a container with such a percentage)
/// is cached under it. UI affinity.
struct percent_base {
    float width = std::numeric_limits<float>::quiet_NaN();
    float height = std::numeric_limits<float>::quiet_NaN();
};

[[nodiscard]] percent_base current_percent_base();
/// The measurement in progress resolved a percentage against `current_percent_base`.
void note_percent_base_read();
/// Whether `s` resolves a percentage against its own percent base: its padding or border,
/// and (unless `content`, the sizing a flex parent applies itself) its size, min or max.
[[nodiscard]] bool style_reads_percent_base(const layout::flex_style& s, bool content);

/// Sets the percent base for the measurements and placements made while it lives.
class percent_base_scope {
public:
    percent_base_scope(float width, float height);
    ~percent_base_scope();
    percent_base_scope(const percent_base_scope&) = delete;
    percent_base_scope& operator=(const percent_base_scope&) = delete;

private:
    percent_base saved_{};
    bool saved_read_ = false;
};

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

// ── Device scale (ADR-010 §4.10) ──
//
// The UI lays out in logical pixels; a window on a 1.25 output renders into a buffer of
// ceil(logical × 1.25) device pixels and `draw_scope` projects every primitive once, at
// emission, as `scene::output_projection` does for the compositor's outputs. Each edge is
// rounded to the nearest device pixel on its own, so two rects sharing a logical edge share
// the device edge and the projection of an intersection is the intersection of the
// projections. Lengths that are not edges scale exactly (radii, blur, spread), except border
// widths, which snap to whole device pixels (at least 1) as Chromium and Firefox draw them:
// a 1 px border stays one crisp device pixel at 1.25 instead of a 1.25 px smear. Text and
// icons rasterise at device resolution and place their pixels in device space themselves
// (`draw_scope::device_*`), so a glyph samples its tile 1:1 at any scale.

/// The device scale the UI thread lays out and emits for (device px per logical px): what
/// text and icons rasterise at. `ui_root::tick` sets it from the root's scale; 1 otherwise.
/// UI affinity.
[[nodiscard]] float ui_device_scale();
void set_ui_device_scale(float scale);
/// A logical coordinate in device pixels (nearest, halves up).
[[nodiscard]] int project_coord(int v, float scale);
/// A logical rect in device pixels, edge by edge (`project_coord`).
[[nodiscard]] lx::rect2i project_rect(lx::rect2i r, float scale);
/// The smallest logical rect whose projection covers device rect `r` (outward): how buffer
/// damage enters the logical ledger.
[[nodiscard]] lx::rect2i unproject_rect(lx::rect2i r, float scale);

/// Draw-emission target. Nodes append `scene::draw_command`s (textured or solid surface
/// draws) and ADR-010 primitives; they never rasterize. The clip/opacity/origin stack is
/// applied on the way in so children cannot escape their parent's bounds: every emitter
/// offsets by the origin, drops what the clip hides, writes the clip as the content mask,
/// multiplies colours by the opacity and takes its order from the list's overlap grid.
///
/// Rects in, the clip and the origin are logical; at `scale` ≠ 1 every emitter projects to
/// device pixels on the way out (the list, its grid and the instances are device pixels).
/// The `device_*` emitters take device rects already offset (`device_rect`) for nodes that
/// place their own pixels: glyph runs and icon rasters.
class draw_scope {
public:
    draw_scope(lx::scene::draw_list& out, lx::rect2i clip, float scale = 1.f);

    void push_clip(lx::rect2i clip);
    /// `push_clip` of a rect in the node's layout space: offset by the origin first. A node
    /// clipping to its own bounds uses this, so it clips where it is drawn inside a scrolled
    /// viewport (whose content is drawn under a `push_origin`).
    void push_clip_local(lx::rect2i clip);
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
    /// The clip is empty: no emitter can draw anything.
    [[nodiscard]] bool clip_empty() const;
    /// `local` (a rect in the node's layout space, offset by the origin) misses the clip:
    /// what an emitter checks before work it would draw nothing with.
    [[nodiscard]] bool culled(lx::rect2i local) const;
    [[nodiscard]] float opacity() const;
    [[nodiscard]] unsigned emitted() const;
    /// The offset every emitter adds to its bounds (the sum of `push_origin`s).
    [[nodiscard]] lx::point2i origin() const;

    // ── Device space ──

    /// Device px per logical px (1: the two are the same).
    [[nodiscard]] float scale() const { return scale_; }
    /// `local` offset by the origin and projected: where the scope draws it, device px.
    [[nodiscard]] lx::rect2i device_rect(lx::rect2i local) const;
    /// The clip in device px.
    [[nodiscard]] lx::rect2i device_clip() const { return top().dclip; }
    /// `device` (device px) misses the clip.
    [[nodiscard]] bool device_culled(lx::rect2i device) const;
    /// `sprite` over a device rect (a raster placed 1:1: an icon), identity params.
    void device_sprite(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i device,
                       lx::gfx::sprite_uv uv, lx::color color);
    /// `group_order` over a device rect.
    [[nodiscard]] std::uint32_t device_group_order(lx::rect2i device, std::uint32_t at_least = 0);
    /// `glyph` (with an order) over a device rect.
    void device_glyph(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i device,
                      lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color, std::uint32_t order);
    /// `underline` over a device rect, `thickness` in device px.
    void device_underline(lx::rect2i device, lx::color color, float thickness, bool wavy = false);

private:
    struct frame {
        /// The clip projected: what visibility is tested against and instances carry. First,
        /// with the other fields every emitter reads, as the unscaled frame had them.
        lx::rect2i dclip{};
        float opacity = 1.f;
        lx::point2i origin{};
        /// `dclip` as an instance writes it (x, y, w, h floats): converted once per push,
        /// not once per primitive.
        float clip_f[4]{};
        /// Logical (what `clip()` answers and `culled` tests).
        lx::rect2i clip{};
    };

    void push_frame(frame f);
    /// Offsets `bounds` by the origin into `placed` (projected at a scale) and clips it into
    /// `visible`; false when nothing of it is visible. Always inlined: it is every emitter's
    /// first step (S3 measured the call at +12 % of its emit).
    [[nodiscard]] [[gnu::always_inline]] bool place(lx::rect2i bounds, lx::rect2i& placed,
                                                    lx::rect2i& visible) const;
    /// `glyph`'s common tail, `placed` and `visible` from `place`.
    void emit_glyph(lx::gfx::primitive_kind kind, std::uint8_t page, lx::rect2i placed,
                    lx::rect2i visible, lx::gfx::sprite_uv uv, lx::gfx::packed_rgba color,
                    std::uint32_t order);
    /// `quad`'s common tail: `colour_scale` multiplies the packed colours (1: as they are).
    void emit_quad(lx::rect2i placed, lx::rect2i visible, const packed_quad_style& style,
                   float colour_scale);
    /// `quad(bounds, packed)` at a device scale (out of line: the unscaled path stays lean).
    [[gnu::noinline]] void quad_scaled(lx::rect2i bounds, const packed_quad_style& style);
    /// `emit_quad` at a device scale: the radii and border widths projected first.
    [[gnu::noinline]] void emit_quad_scaled(lx::rect2i placed, lx::rect2i visible,
                                            const packed_quad_style& style, float colour_scale);
    [[nodiscard]] frame& top();
    [[nodiscard]] const frame& top() const;
    /// `r` (logical, origin applied) in device px: `r` itself at scale 1.
    [[nodiscard]] lx::rect2i to_device(lx::rect2i r) const {
        return scaled_ ? project_rect(r, scale_) : r;
    }
    /// A logical length in device px (radii, blur, spread, thickness).
    [[nodiscard]] float to_device(float v) const { return scaled_ ? v * scale_ : v; }

    static constexpr unsigned k_max_depth = 32;

    lx::scene::draw_list* out_ = nullptr;
    frame stack_[k_max_depth]{};
    unsigned depth_ = 1;
    unsigned emitted_ = 0;
    float scale_ = 1.f;
    bool scaled_ = false;
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
    /// Its children are laid out, drawn and hit-tested by the overlay layer against the
    /// window (`lx.ui:overlay`), not by it: their bounds are surface coordinates, whatever
    /// viewport the node itself sits in.
    layer = 1u << 12,
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
/// A node that scrolls its content (`lx.ui:scroll`'s scroll area, `lx.ui:list`): what a
/// scrollbar node reads and drives. Offsets and sizes are px along the axis.
struct scroll_metrics {
    float offset = 0.f;   ///< how far the content is scrolled
    float viewport = 0.f; ///< the visible length
    float content = 0.f;  ///< the content's length (≥ the viewport when it overflows)
    [[nodiscard]] float max_offset() const { return content > viewport ? content - viewport : 0.f; }
};

class scroll_target {
public:
    /// Axis 0 is horizontal, 1 vertical.
    [[nodiscard]] virtual scroll_metrics scroll_metrics_along(unsigned axis) const = 0;
    /// Scroll to `offset` along `axis` (clamped); repaints the viewport, relays nothing out.
    virtual void scroll_to(unsigned axis, float offset) = 0;

protected:
    ~scroll_target() = default;
};

/// A node text can be dropped on (drag and drop from this or another client; lx.app's data
/// device drives it through `ui_root::drop_motion` / `drop_text`). Points are surface
/// coordinates. UI affinity.
class drop_target {
public:
    /// The drag is over the node at `at`: show where it would land; false when the node
    /// takes no text there (nothing is shown).
    virtual bool drop_hover(lx::point2i at) = 0;
    /// The drag left the node, or ended: the mark goes.
    virtual void drop_leave() = 0;
    /// Dropped at `at`: inserts `utf8`; false when refused.
    virtual bool drop_text(lx::point2i at, std::string_view utf8) = 0;

protected:
    ~drop_target() = default;
};

/// The drop target a drag is over (null: none). A target going away calls
/// `forget_drop_target` so the pointer never dangles. UI affinity.
[[nodiscard]] drop_target* current_drop_target();
void set_current_drop_target(drop_target* target);
void forget_drop_target(drop_target& target);

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
    /// cleared by `mark_layout_dirty`; an entry whose measurement read the percent base
    /// (`current_percent_base`) is keyed by it too. A key whose min is neither 0 nor its max
    /// on an axis is measured without the cache (`layout_counters::measure_uncached`); every
    /// query the layout passes make has one or the other. The entries live out of the node,
    /// in a 104-byte block of the UI thread's measure-cache pool that the node takes the first
    /// time it is measured and gives back when it dies: a node never measured (a leaf its
    /// parent places without asking) costs nothing, and the node stays the 80 bytes the emit
    /// walk reads (G3).
    [[nodiscard]] lx::size2i measure_cached(layout::constraints c, size_hint hint,
                                            sizing_mode mode = sizing_mode::inherent) const;
    /// `arrange` unless the node is clean and `bounds` are its bounds already: a clean
    /// subtree whose place did not move is not walked. A node with percentage padding or
    /// border (`flex_item_style`) is arranged anyway: its percent base may have moved.
    void place(lx::rect2i bounds);

    /// The node's style as a flex item, read by its parent flex container: null for CSS's
    /// initial values (`flex: 0 1 auto`, auto sizes and margins).
    [[nodiscard]] virtual const layout::flex_style* flex_item_style() const;
    /// Pass 3. Appends draw commands for this node and its children.
    virtual void emit(draw_scope& out) const;

    virtual bool on_mouse(mouse_event event);
    virtual bool on_key(key_event event);
    /// A pointer event routed to this node (`pointer_kind`): press, drag and release go to
    /// the pressed interactive node, wheel and secondary presses to the node under the
    /// pointer and then up its ancestors until one returns true. Default: not taken.
    virtual bool on_pointer(const pointer_event& event);
    /// Whether this node draws `child` translated and clipped (a scrolled viewport): `offset`
    /// is added to the child's layout coordinates to get this node's, and `clip` (in this
    /// node's coordinates) is what of it shows. Hit testing, damage and `surface_rect` follow
    /// it. Default: no (children are drawn where they are laid out).
    [[nodiscard]] virtual bool child_viewport(const ui_node& child, lx::point2i& offset,
                                              lx::rect2i& clip) const;
    /// The node as a scroll container, for its scrollbars; null when it is not one.
    [[nodiscard]] virtual scroll_target* as_scroll_target();
    /// The node as a drop target for dragged text; null when it is not one.
    [[nodiscard]] virtual drop_target* as_drop_target();
    [[nodiscard]] virtual hit_result hit_test(lx::point2i p) const;
    /// How far the node paints outside its bounds, px on every side: a drop shadow's reach
    /// (3σ + spread + offset). Damage covers it (`ink_bounds`). Default 0.
    [[nodiscard]] virtual int ink_outset() const;
    /// The pointer shape the node asks for while the pointer is over it; `automatic` defers
    /// to its parent (`resolve_cursor`). A style's `cursor`, a control's `pointer`, a text
    /// field's `text`.
    [[nodiscard]] virtual cursor_style cursor() const;

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
/// Adds `region` of `node`'s layout space, mapped to the surface (`surface_rect`).
void damage_node(const ui_node& node, lx::rect2i region);
/// Live nodes that override `child_viewport` with an offset (scroll containers) register
/// here, so that a tree without one maps nothing (UI affinity).
void viewport_created();
void viewport_destroyed();
} // namespace lx::ui::detail

export namespace lx::ui {

/// `r` in `node`'s layout space as the surface shows it: translated and clipped by every
/// ancestor viewport (`ui_node::child_viewport`) up to the root or a `node_flag::layer`
/// node (whose children are laid out in surface coordinates). Empty when a viewport hides
/// it all. Without viewports in the tree it is `r`.
[[nodiscard]] lx::rect2i surface_rect(const ui_node& node, lx::rect2i r);
/// `node`'s bounds as the surface shows them, not clipped by viewports: an overlay's anchor.
[[nodiscard]] lx::rect2i surface_bounds(const ui_node& node);
/// A surface point in `node`'s layout space (the inverse of the viewports' offsets).
[[nodiscard]] lx::point2i local_point(const ui_node& node, lx::point2i surface);
/// `node`'s bounds grown by its `ink_outset` (an empty box stays empty): what it paints.
[[nodiscard]] lx::rect2i ink_bounds(const ui_node& node);
/// The same grown by `node`'s ink outset (`r` in its layout space).
[[nodiscard]] lx::rect2i ink_rect(const ui_node& node, lx::rect2i r);
/// The pointer shape over `node`: the first non-`automatic` `cursor()` from it up to the
/// root, else `arrow` (also for no node: the pointer over nothing interactive).
[[nodiscard]] cursor_style resolve_cursor(const ui_node* node);

} // namespace lx::ui


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

namespace {

/// `ui_device_scale` (UI affinity: one per UI thread, as the ledger and the overlays are).
thread_local float g_ui_device_scale = 1.f;

/// A border width (device px) snapped to whole device pixels, at least one when nonzero.
[[nodiscard]] float lx_ui_snap_border(float device_px) {
    if (device_px <= 0.f)
        return 0.f;
    const float snapped = std::floor(device_px + 0.5f);
    return snapped < 1.f ? 1.f : snapped;
}

} // namespace

float lx::ui::ui_device_scale() { return g_ui_device_scale; }

void lx::ui::set_ui_device_scale(float scale) { g_ui_device_scale = scale > 0.f ? scale : 1.f; }

int lx::ui::project_coord(int v, float scale) {
    if (scale == 1.f)
        return v;
    return static_cast<int>(std::floor(static_cast<double>(v) * static_cast<double>(scale) + 0.5));
}

lx::rect2i lx::ui::project_rect(lx::rect2i r, float scale) {
    if (scale == 1.f)
        return r;
    const int x0 = project_coord(r.x, scale);
    const int y0 = project_coord(r.y, scale);
    const int x1 = project_coord(r.x + r.width, scale);
    const int y1 = project_coord(r.y + r.height, scale);
    return {x0, y0, x1 - x0, y1 - y0};
}

lx::rect2i lx::ui::unproject_rect(lx::rect2i r, float scale) {
    if (scale == 1.f || scale <= 0.f)
        return r;
    const double s = scale;
    const int x0 = static_cast<int>(std::floor(static_cast<double>(r.x) / s));
    const int y0 = static_cast<int>(std::floor(static_cast<double>(r.y) / s));
    const int x1 = static_cast<int>(std::ceil(static_cast<double>(r.x + r.width) / s));
    const int y1 = static_cast<int>(std::ceil(static_cast<double>(r.y + r.height) / s));
    return {x0, y0, x1 - x0, y1 - y0};
}

lx::ui::draw_scope::draw_scope(lx::scene::draw_list& out, lx::rect2i clip, float scale)
    : out_{&out} {
    // Every emitter writes the list's overlap grid, which is UI affinity (ADR-010 §4.12).
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    scale_ = scale > 0.f ? scale : 1.f;
    scaled_ = scale_ != 1.f;
    stack_[0] = {to_device(clip), 1.f, {}, {}, clip};
    lx_ui_write_rect(stack_[0].clip_f, stack_[0].dclip);
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
    next.dclip = to_device(next.clip);
    lx_ui_write_rect(next.clip_f, next.dclip);
    push_frame(next);
}

void lx::ui::draw_scope::push_clip_local(lx::rect2i clip) {
    const frame& f = top();
    push_clip({clip.x + f.origin.x, clip.y + f.origin.y, clip.width, clip.height});
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
    const lx::rect2i placed = to_device({dst.x + f.origin.x, dst.y + f.origin.y, dst.width, dst.height});
    const lx::rect2i visible = lx_ui_intersect(placed, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::scene::draw_command cmd{};
    cmd.texture = texture;
    cmd.src = src;
    cmd.dst = placed;
    cmd.clip = f.dclip;
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
    const lx::rect2i placed = to_device({dst.x + f.origin.x, dst.y + f.origin.y, dst.width, dst.height});
    const lx::rect2i visible = lx_ui_intersect(placed, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    // A solid draw (`draw_command::solid`, what every backend draws for a single-pixel
    // buffer): no texture, the colour premultiplied with the scope opacity folded in once
    // here (ADR-010 §4.5), so `opacity` and `tint` stay neutral.
    lx::scene::draw_command cmd{};
    cmd.dst = placed;
    cmd.clip = f.dclip;
    cmd.sort_key = lx::draw_sort_key::make(1, out_->size());
    cmd.solid = true;
    const float a = color.a * f.opacity;
    cmd.solid_color = lx::color::encoded(color.r * a, color.g * a, color.b * a, a, color.space,
                                         color.encoding);
    out_->push(cmd, out_->orders().assign(visible));
    ++emitted_;
}

[[gnu::always_inline]] bool lx::ui::draw_scope::place(lx::rect2i bounds, lx::rect2i& placed,
                                                      lx::rect2i& visible) const {
    const frame& f = top();
    placed = {bounds.x + f.origin.x, bounds.y + f.origin.y, bounds.width, bounds.height};
    if (scaled_) [[unlikely]]
        placed = project_rect(placed, scale_);
    visible = lx_ui_intersect(placed, f.dclip);
    return visible.width > 0 && visible.height > 0;
}

void lx::ui::draw_scope::quad(lx::rect2i bounds, const quad_style& style) {
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!out_ || !place(bounds, placed, visible))
        return;
    // The scope opacity is folded in at packing, before the premultiply: no rescale after.
    if (scaled_) [[unlikely]]
        emit_quad_scaled(placed, visible, pack_quad_style(style, top().opacity), 1.f);
    else
        emit_quad(placed, visible, pack_quad_style(style, top().opacity), 1.f);
}

void lx::ui::draw_scope::quad(lx::rect2i bounds, const packed_quad_style& style) {
    if (scaled_) [[unlikely]] {
        quad_scaled(bounds, style);
        return;
    }
    // The unscaled path, one test above what it was before scales existed: the hottest
    // emitter (S3: 10 000 a frame).
    if (!out_)
        return;
    const frame& f = top();
    const lx::rect2i placed{bounds.x + f.origin.x, bounds.y + f.origin.y, bounds.width,
                            bounds.height};
    const lx::rect2i visible = lx_ui_intersect(placed, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;
    emit_quad(placed, visible, style, f.opacity);
}

void lx::ui::draw_scope::quad_scaled(lx::rect2i bounds, const packed_quad_style& style) {
    lx::rect2i placed{};
    lx::rect2i visible{};
    if (!out_ || !place(bounds, placed, visible))
        return;
    emit_quad_scaled(placed, visible, style, top().opacity);
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

void lx::ui::draw_scope::emit_quad_scaled(lx::rect2i placed, lx::rect2i visible,
                                          const packed_quad_style& style, float colour_scale) {
    // Radii scale exactly; border widths snap to whole device pixels. Out of line: the
    // unscaled path (`emit_quad`) stays what it was.
    packed_quad_style s = style;
    for (unsigned i = 0; i < 4; ++i) {
        s.radii[i] = lx::gfx::to_half(lx::gfx::from_half(style.radii[i]) * scale_);
        s.border_widths[i] =
            lx::gfx::to_half(lx_ui_snap_border(lx::gfx::from_half(style.border_widths[i]) * scale_));
    }
    emit_quad(placed, visible, s, colour_scale);
}

void lx::ui::draw_scope::shadow(lx::rect2i bounds, const shadow_style& style) {
    if (!out_)
        return;
    const frame& f = top();
    const lx::rect2i placed =
        to_device({bounds.x + f.origin.x, bounds.y + f.origin.y, bounds.width, bounds.height});
    const float sigma = to_device(style.blur_sigma);
    const float spread = to_device(style.spread);
    // An outer shadow is visible wherever its blur reaches, not only under the box.
    lx::rect2i extent = placed;
    if (!style.inset) {
        const float reach_px = 3.f * sigma + spread;
        const int reach = reach_px > 0.f ? static_cast<int>(std::ceil(reach_px)) : 0;
        extent = {placed.x - reach, placed.y - reach, placed.width + 2 * reach,
                  placed.height + 2 * reach};
    }
    const lx::rect2i visible = lx_ui_intersect(extent, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::shadow_instance s{};
    lx_ui_write_rect(s.bounds, placed);
    for (unsigned i = 0; i < 4; ++i)
        s.clip[i] = f.clip_f[i];
    if (scaled_) {
        const float radii[4] = {style.corner_radii[0] * scale_, style.corner_radii[1] * scale_,
                                style.corner_radii[2] * scale_, style.corner_radii[3] * scale_};
        lx_ui_write_half4(s.radii, radii);
    } else {
        lx_ui_write_half4(s.radii, style.corner_radii);
    }
    lx_ui_write_rgba(s.color, style.color, f.opacity);
    s.sigma = sigma;
    s.spread = spread;
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
    const lx::rect2i placed =
        to_device({bounds.x + f.origin.x, bounds.y + f.origin.y, bounds.width, bounds.height});
    const lx::rect2i visible = lx_ui_intersect(placed, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::sprite_instance s{};
    lx_ui_write_rect(s.bounds, placed);
    for (unsigned i = 0; i < 4; ++i)
        s.clip[i] = f.clip_f[i];
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
    // A poly sprite's params are corner radii: lengths, projected like a quad's.
    if (scaled_ && kind == lx::gfx::primitive_kind::poly_sprite)
        for (float& p : params)
            p *= scale_;
    for (unsigned i = 0; i < 4; ++i)
        s.params[i] = lx::gfx::to_half(params[i]);
    out_->push_sprite(kind, page, s, visible, out_->orders().assign(visible));
    ++emitted_;
}

void lx::ui::draw_scope::underline(lx::rect2i bounds, lx::color color, float thickness,
                                   bool wavy) {
    const frame& f = top();
    device_underline(
        to_device({bounds.x + f.origin.x, bounds.y + f.origin.y, bounds.width, bounds.height}),
        color, to_device(thickness), wavy);
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
    for (unsigned i = 0; i < 4; ++i)
        s.clip[i] = f.clip_f[i];
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

lx::rect2i lx::ui::draw_scope::device_rect(lx::rect2i local) const {
    const frame& f = top();
    return to_device({local.x + f.origin.x, local.y + f.origin.y, local.width, local.height});
}

bool lx::ui::draw_scope::device_culled(lx::rect2i device) const {
    const lx::rect2i v = lx_ui_intersect(device, top().dclip);
    return v.width <= 0 || v.height <= 0;
}

void lx::ui::draw_scope::device_sprite(lx::gfx::primitive_kind kind, std::uint8_t page,
                                       lx::rect2i device, lx::gfx::sprite_uv uv, lx::color color) {
    if (!out_ || (kind != lx::gfx::primitive_kind::mono_sprite &&
                  kind != lx::gfx::primitive_kind::poly_sprite))
        return;
    const frame& f = top();
    const lx::rect2i visible = lx_ui_intersect(device, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;
    emit_glyph(kind, page, device, visible, uv,
               lx::gfx::premultiplied_half(lx_ui_faded(color, f.opacity)),
               out_->orders().assign(visible));
}

std::uint32_t lx::ui::draw_scope::device_group_order(lx::rect2i device, std::uint32_t at_least) {
    if (!out_)
        return 0;
    const lx::rect2i visible = lx_ui_intersect(device, top().dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return out_->orders().layer_base();
    return out_->orders().assign(visible, at_least);
}

void lx::ui::draw_scope::device_glyph(lx::gfx::primitive_kind kind, std::uint8_t page,
                                      lx::rect2i device, lx::gfx::sprite_uv uv,
                                      lx::gfx::packed_rgba color, std::uint32_t order) {
    if (!out_ || (kind != lx::gfx::primitive_kind::mono_sprite &&
                  kind != lx::gfx::primitive_kind::poly_sprite))
        return;
    const lx::rect2i visible = lx_ui_intersect(device, top().dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;
    emit_glyph(kind, page, device, visible, uv, color, order);
}

void lx::ui::draw_scope::device_underline(lx::rect2i device, lx::color color, float thickness,
                                          bool wavy) {
    if (!out_)
        return;
    const frame& f = top();
    const lx::rect2i visible = lx_ui_intersect(device, f.dclip);
    if (visible.width <= 0 || visible.height <= 0)
        return;

    lx::gfx::underline_instance u{};
    lx_ui_write_rect(u.bounds, device);
    for (unsigned i = 0; i < 4; ++i)
        u.clip[i] = f.clip_f[i];
    lx_ui_write_rgba(u.color, color, f.opacity);
    u.thickness = lx::gfx::to_half(thickness);
    u.wavy = wavy ? 1 : 0;
    out_->push_underline(u, visible, out_->orders().assign(visible));
    ++emitted_;
}

lx::point2i lx::ui::draw_scope::origin() const { return top().origin; }
lx::rect2i lx::ui::draw_scope::clip() const { return top().clip; }
bool lx::ui::draw_scope::clip_empty() const {
    const lx::rect2i& c = top().clip;
    return c.width <= 0 || c.height <= 0;
}
bool lx::ui::draw_scope::culled(lx::rect2i local) const {
    const frame& f = top();
    const int x = local.x + f.origin.x;
    const int y = local.y + f.origin.y;
    return local.width <= 0 || local.height <= 0 || f.clip.width <= 0 || f.clip.height <= 0 ||
           x + local.width <= f.clip.x || y + local.height <= f.clip.y ||
           x >= f.clip.x + f.clip.width || y >= f.clip.y + f.clip.height;
}
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
/// min is 0), bit 5 for `sizing_mode::content`, bit 6 when the entry is keyed by the
/// percent base (`base_*`, compared bit for bit: NaN matches NaN).
struct lx_ui_measure_block {
    static constexpr unsigned k_entries = 4;
    static constexpr std::uint8_t k_by_base = 0x40;
    struct entry {
        int max_width;
        int max_height;
        int width;
        int height;
        std::uint32_t base_width;
        std::uint32_t base_height;
    };
    std::uint8_t tags[k_entries];
    std::uint8_t next;
    entry entries[k_entries];
};
static_assert(sizeof(lx_ui_measure_block) == 104);

/// The percent base in force and whether the measurement in progress read it (UI affinity).
lx::ui::percent_base g_percent_base{};
bool g_percent_read = false;

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

lx::ui::percent_base lx::ui::current_percent_base() { return g_percent_base; }

void lx::ui::note_percent_base_read() { g_percent_read = true; }

bool lx::ui::style_reads_percent_base(const layout::flex_style& s, bool content) {
    const auto pct = [](layout::length l) { return l.kind() == layout::unit::percent; };
    const auto edges = [&](const layout::edges& e) {
        return pct(e.top) || pct(e.right) || pct(e.bottom) || pct(e.left);
    };
    if (edges(s.padding) || edges(s.border))
        return true;
    return !content && (pct(s.width) || pct(s.height) || pct(s.min_width) || pct(s.min_height) ||
                        pct(s.max_width) || pct(s.max_height));
}

lx::ui::percent_base_scope::percent_base_scope(float width, float height)
    : saved_{g_percent_base}, saved_read_{g_percent_read} {
    g_percent_base = {width, height};
    // What is measured under this base depends on it, not on the one outside: the reads
    // made here stay here.
    g_percent_read = false;
}

lx::ui::percent_base_scope::~percent_base_scope() {
    g_percent_base = saved_;
    g_percent_read = saved_read_;
}

lx::size2i lx::ui::ui_node::measure_cached(layout::constraints c, size_hint hint,
                                           sizing_mode mode) const {
    layout_counters& k = g_layout_counters;
    const std::uint32_t base_w = std::bit_cast<std::uint32_t>(g_percent_base.width);
    const std::uint32_t base_h = std::bit_cast<std::uint32_t>(g_percent_base.height);
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
            const std::uint8_t t = b.tags[e];
            if ((t & ~lx_ui_measure_block::k_by_base) != tag || m.max_width != c.max.width ||
                m.max_height != c.max.height)
                continue;
            if ((t & lx_ui_measure_block::k_by_base) != 0) {
                if (m.base_width != base_w || m.base_height != base_h)
                    continue;
                g_percent_read = true; // the caller's answer depends on the base too
            }
            ++k.measure_hits;
            return {m.width, m.height};
        }
    } else {
        ++k.measure_uncached;
    }
    ++k.measures;
    if (k.on_measure)
        k.on_measure(*this);
    // Whether this measurement reads the percent base: the caller's flag is kept aside and
    // gets this one's on the way out (the parent depends on what its pass-through child read).
    const bool outer_read = g_percent_read;
    g_percent_read = false;
    const lx::size2i size =
        mode == sizing_mode::content ? measure_content(c, hint) : measure(c, hint);
    const bool read = g_percent_read;
    g_percent_read = outer_read || read;
    if (cacheable && measure_slot_ != 0) {
        // Measuring children never takes this node's block, so `b` is still this node's.
        lx_ui_measure_block& b = pool.at(measure_slot_);
        const unsigned e = b.next;
        b.next = static_cast<std::uint8_t>((e + 1u) % lx_ui_measure_block::k_entries);
        b.entries[e] = {c.max.width, c.max.height, size.width, size.height, base_w, base_h};
        b.tags[e] = static_cast<std::uint8_t>(tag | (read ? lx_ui_measure_block::k_by_base : 0u));
    }
    return size;
}

void lx::ui::ui_node::place(lx::rect2i bounds) {
    layout_counters& k = g_layout_counters;
    if (!layout_dirty_ && bounds.x == bounds_.x && bounds.y == bounds_.y &&
        bounds.width == bounds_.width && bounds.height == bounds_.height) {
        // Percentage padding or border resolves against the parent's box, which may have
        // changed though this node's did not: arrange it again.
        const layout::flex_style* s = flex_item_style();
        if (!s || !style_reads_percent_base(*s, true)) {
            ++k.arrange_skips;
            return;
        }
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
        // What it paints: its shadows reach past the bounds.
        if (moved)
            detail::damage_node(*this, ink_rect(*this, before));
        detail::damage_node(*this, ink_rect(*this, bounds));
        set_flag(node_flag::repaint, false);
    }
}

void lx::ui::ui_node::mark_repaint() {
    detail::damage_node(*this, ink_bounds(*this));
    set_flag(node_flag::repaint, true);
}

bool lx::ui::ui_node::on_pointer(const pointer_event&) { return false; }

bool lx::ui::ui_node::child_viewport(const ui_node&, lx::point2i&, lx::rect2i&) const {
    return false;
}

lx::ui::scroll_target* lx::ui::ui_node::as_scroll_target() { return nullptr; }
lx::ui::drop_target* lx::ui::ui_node::as_drop_target() { return nullptr; }

namespace {
lx::ui::drop_target* g_drop_target = nullptr;
} // namespace

lx::ui::drop_target* lx::ui::current_drop_target() { return g_drop_target; }
void lx::ui::set_current_drop_target(drop_target* target) { g_drop_target = target; }
void lx::ui::forget_drop_target(drop_target& target) {
    if (g_drop_target == &target)
        g_drop_target = nullptr;
}

namespace {
/// Live viewport nodes (UI affinity): none, and nothing is mapped.
unsigned g_viewport_nodes = 0;
} // namespace

void lx::ui::detail::viewport_created() { ++g_viewport_nodes; }
void lx::ui::detail::viewport_destroyed() {
    if (g_viewport_nodes)
        --g_viewport_nodes;
}

lx::rect2i lx::ui::surface_rect(const ui_node& node, lx::rect2i r) {
    if (g_viewport_nodes == 0)
        return r;
    const ui_node* child = &node;
    for (const ui_node* p = node.parent(); p; child = p, p = p->parent()) {
        if (p->has_flag(node_flag::layer))
            break; // the layer's children are placed in surface coordinates
        lx::point2i offset{};
        lx::rect2i clip{};
        if (!p->child_viewport(*child, offset, clip))
            continue;
        r.x += offset.x;
        r.y += offset.y;
        r = lx_ui_intersect(r, clip);
        if (r.width <= 0 || r.height <= 0)
            return {};
    }
    return r;
}

lx::rect2i lx::ui::surface_bounds(const ui_node& node) {
    lx::rect2i r = node.bounds();
    if (g_viewport_nodes == 0)
        return r;
    const ui_node* child = &node;
    for (const ui_node* p = node.parent(); p; child = p, p = p->parent()) {
        if (p->has_flag(node_flag::layer))
            break;
        lx::point2i offset{};
        lx::rect2i clip{};
        if (p->child_viewport(*child, offset, clip)) {
            r.x += offset.x;
            r.y += offset.y;
        }
    }
    return r;
}

lx::point2i lx::ui::local_point(const ui_node& node, lx::point2i surface) {
    if (g_viewport_nodes == 0)
        return surface;
    const ui_node* child = &node;
    for (const ui_node* p = node.parent(); p; child = p, p = p->parent()) {
        if (p->has_flag(node_flag::layer))
            break;
        lx::point2i offset{};
        lx::rect2i clip{};
        if (p->child_viewport(*child, offset, clip)) {
            surface.x -= offset.x;
            surface.y -= offset.y;
        }
    }
    return surface;
}

void lx::ui::detail::damage_node(const ui_node& node, lx::rect2i region) {
    damage_rect(surface_rect(node, region));
}

int lx::ui::ui_node::ink_outset() const { return 0; }

lx::ui::cursor_style lx::ui::ui_node::cursor() const { return cursor_style::automatic; }

lx::ui::cursor_style lx::ui::resolve_cursor(const ui_node* node) {
    for (const ui_node* n = node; n; n = n->parent())
        if (const cursor_style c = n->cursor(); c != cursor_style::automatic)
            return c;
    return cursor_style::arrow;
}

lx::rect2i lx::ui::ink_rect(const ui_node& node, lx::rect2i r) {
    const int o = node.ink_outset();
    if (o <= 0 || r.width <= 0 || r.height <= 0)
        return r;
    return {r.x - o, r.y - o, r.width + 2 * o, r.height + 2 * o};
}

lx::rect2i lx::ui::ink_bounds(const ui_node& node) { return ink_rect(node, node.bounds()); }

int lx::ui::ui_node::focus_order() const { return 0; }

void lx::ui::ui_node::on_interaction_changed() { detail::damage_node(*this, ink_bounds(*this)); }

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
    // Nothing reaches the surface through an empty clip: a clipping container outside a
    // frame's damage (`emit_scope::damage`) walks none of its children.
    if (out.clip_empty())
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
