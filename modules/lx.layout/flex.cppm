module;

import lx.foundation;

export module lx.layout:flex;

import std;

// ── lx.layout:flex — the CSS flexbox subset Taffy gives GPUI (ADR-010 §4.7) ─────────────
//
// One container at a time: `solve_flex` lays out the direct children of one flex container
// whose style it is given, asking the caller for each child's style and, where the style
// leaves a size to the content, for the child's measurement (`flex_children`). It knows no
// tree: lx.ui's flex container calls it from `measure` and `arrange`, and a child that is a
// flex container answers the measurement by calling it again. It allocates nothing — the
// per-item and per-line state lives in spans the caller provides (`flex_scratch`; one item
// and one line per child is always enough, so a caller with a fixed child cap sizes them
// to that cap and can never overflow them).
//
// The algorithm is CSS Flexible Box Layout Level 1 §9 in the order the spec numbers it and
// in the shape of Taffy's `src/compute/flexbox.rs` (whose behaviour its Chrome-generated
// fixtures pin down): items and their flex base sizes (§9.2), lines (§9.3), the freeze loop
// for flexible lengths (§9.7), cross sizes (§9.4), main- and cross-axis alignment with auto
// margins (§9.5–9.6), then absolutely positioned children (§4.1). Where it diverges from
// Taffy, the code says so where it does; the deliberate omissions are:
//
// - `align-items: baseline` is not offered: a baseline needs a baseline query on the child
//   (lx.ui's measure contract has none), and synthesising one from the border box would
//   only look like support.
// - Intrinsic main sizes (§9.9.1) are each item's max- or min-content contribution summed
//   per line — Chrome's behaviour and, once its flex fractions cancel, Taffy's — not the
//   spec's largest-flex-fraction rule.
// - The automatic minimum size of an item is the content-based one only: no transferred
//   size through `aspect-ratio`, and it is computed lazily — only when the line shrinks or
//   the base size did not come from the content, the cases where it can bind (when the
//   base size is the max-content size it is at least the min-content size already).
// - No `display: none`, `box-sizing: content-box`, scrollbar gutters, RTL, `order`,
//   `visibility: collapse`, `flex-wrap: balance`, or sizing keywords (`min-content`,
//   `fit-content`, `stretch`) — lengths are px, rem, percent or auto.
// - Percent padding, border and margin resolve against the container's inner width (CSS),
//   or zero while it is indefinite (Taffy does the same).
//
// Coordinates are floats in the same unit as px lengths; positions are relative to the
// container's border-box origin. Rounding to whole pixels is the caller's (lx.ui rounds
// edges, as Taffy does, so neighbours never gap or overlap).

export namespace lx::layout {

/// What a `length` is measured in.
enum class unit : std::uint8_t { px = 0, rem = 1, percent = 2, automatic = 3 };

/// A 32-bit length (ADR-010 §4.8): 2 bits of unit and a 30-bit value, the value being an
/// IEEE float with its two lowest mantissa bits given to the unit (21 stored mantissa bits:
/// every integer up to 2^21 and every 1/64 fraction below 2^15 is exact). Percent stores
/// the percentage (50 for 50 %). The zero bit pattern is `0px`.
struct length {
    std::uint32_t bits = 0;

    [[nodiscard]] static constexpr length make(unit u, float value) {
        return {(std::bit_cast<std::uint32_t>(value) & ~3u) | static_cast<std::uint32_t>(u)};
    }
    [[nodiscard]] constexpr unit kind() const { return static_cast<unit>(bits & 3u); }
    [[nodiscard]] constexpr float value() const { return std::bit_cast<float>(bits & ~3u); }
    [[nodiscard]] constexpr bool is_auto() const { return kind() == unit::automatic; }
    constexpr bool operator==(const length&) const = default;
};

[[nodiscard]] constexpr length px(float v) {
    return length::make(unit::px, v);
}
[[nodiscard]] constexpr length rem(float v) {
    return length::make(unit::rem, v);
}
[[nodiscard]] constexpr length percent(float v) {
    return length::make(unit::percent, v);
}
inline constexpr length auto_length = length::make(unit::automatic, 0.f);

/// The value of `l` in px: `rem_px` per rem, `percent_base` for 100 %. NaN — "indefinite",
/// the engine's `None` — for `auto`, and for a percentage of an indefinite (NaN) base.
[[nodiscard]] constexpr float resolve(length l, float rem_px, float percent_base) {
    switch (l.kind()) {
    case unit::px:
        return l.value();
    case unit::rem:
        return l.value() * rem_px;
    case unit::percent:
        return percent_base * l.value() / 100.f; // NaN base stays NaN
    case unit::automatic:
    default:
        return std::numeric_limits<float>::quiet_NaN();
    }
}

/// Four edges in CSS order.
struct edges {
    length top{};
    length right{};
    length bottom{};
    length left{};
    constexpr bool operator==(const edges&) const = default;
};

[[nodiscard]] constexpr edges uniform(length l) {
    return {l, l, l, l};
}

enum class flex_direction : std::uint8_t { row, column, row_reverse, column_reverse };
enum class flex_wrap : std::uint8_t { no_wrap, wrap, wrap_reverse };

/// `justify-content` and `align-content`. `flex_start`/`flex_end` follow the flex direction
/// (and `wrap-reverse` for lines); `start`/`end` are the physical left/top and right/bottom
/// (left-to-right, top-to-bottom writing mode only). `stretch` is `align-content`'s initial
/// value and acts as `flex_start` in `justify-content`.
enum class content_align : std::uint8_t {
    flex_start,
    flex_end,
    start,
    end,
    center,
    stretch,
    space_between,
    space_around,
    space_evenly,
};

/// `align-items` and `align-self` (`automatic` is `align-self: auto`: the container's
/// `align-items`). No `baseline` (see the file comment).
enum class item_align : std::uint8_t {
    automatic,
    flex_start,
    flex_end,
    start,
    end,
    center,
    stretch
};

enum class positioning : std::uint8_t { relative, absolute };

/// `scroll` lets the content exceed the box and reports the content size; the scroll offset
/// is applied by the box's owner, never by layout. `hidden` and `scroll` also make an item's
/// automatic minimum size 0 (CSS: a scroll container does not grow to its content).
enum class overflow : std::uint8_t { visible, hidden, scroll };

/// One box's layout style: its properties as a flex container and as a flex item, with
/// CSS's initial values (`flex: 0 1 auto`, `align-items: stretch`, `align-content:
/// stretch`, sizes `auto`). Sizes are border-box (`box-sizing: border-box`, GPUI's and
/// Tailwind's default). `max_*` `auto` is `none`; `min_*` `auto` is the automatic minimum
/// size. `inset` places an absolutely positioned box in its container's padding box and
/// offsets a relatively positioned one. `aspect_ratio` (width / height) applies when one
/// side is definite and the other is auto; 0 is none.
struct flex_style {
    flex_direction direction = flex_direction::row;
    flex_wrap wrap = flex_wrap::no_wrap;
    content_align justify_content = content_align::flex_start;
    content_align align_content = content_align::stretch;
    item_align align_items = item_align::stretch;
    item_align align_self = item_align::automatic;
    positioning position = positioning::relative;
    overflow overflow_x = overflow::visible;
    overflow overflow_y = overflow::visible;
    float flex_grow = 0.f;
    float flex_shrink = 1.f;
    float aspect_ratio = 0.f;
    length flex_basis = auto_length;
    length width = auto_length;
    length height = auto_length;
    length min_width = auto_length;
    length min_height = auto_length;
    length max_width = auto_length;
    length max_height = auto_length;
    length row_gap{};
    length column_gap{};
    edges margin{};
    edges padding{};
    edges border{};
    edges inset{auto_length, auto_length, auto_length, auto_length};

    constexpr bool operator==(const flex_style&) const = default;
};

/// CSS initial values, for children that carry no style.
inline constexpr flex_style k_default_flex_style{};

struct flex_size {
    float width = 0.f;
    float height = 0.f;
};

/// What the engine asks of a child it has to measure. A known side is the child's final
/// border-box size on that axis; the child must return it. An unknown side is the child's
/// content size within `available_*` (+infinity: unbounded, the max-content size). With
/// `min_content_width` the width is asked at its min-content size (the narrowest the
/// content can wrap to) instead. Heights have no min-content form: in a horizontal writing
/// mode a box's min-content height is its max-content height at the width it is given.
/// `parent_*` is the container's content-box size (NaN where indefinite): the base for the
/// child's own percentages (`flex_input::parent_*` when the child is a container itself).
struct child_query {
    float known_width = std::numeric_limits<float>::quiet_NaN();
    float known_height = std::numeric_limits<float>::quiet_NaN();
    float available_width = std::numeric_limits<float>::infinity();
    float available_height = std::numeric_limits<float>::infinity();
    float parent_width = std::numeric_limits<float>::quiet_NaN();
    float parent_height = std::numeric_limits<float>::quiet_NaN();
    bool min_content_width = false;
    /// Set for in-flow items: answer with the content size (`flex_input::content_size`).
    /// Clear for absolutely positioned children, whose own size styles apply.
    bool content_size = true;
};

/// The caller's view of one container's children. `style` may return null (CSS initial
/// values); `measure` answers a `child_query` with the child's border-box size.
struct flex_children {
    void* context = nullptr;
    unsigned count = 0;
    const flex_style* (*style)(void* context, unsigned index) = nullptr;
    flex_size (*measure)(void* context, unsigned index, const child_query& query) = nullptr;
};

/// What the container itself is asked: its border-box size where the caller fixes it
/// (`known_*`), otherwise its content size within `available_*` (+infinity: max-content),
/// or its min-content width. `parent_*` is the percent base for the container's own
/// size, min and max (NaN: percentages of them are `auto`). `content_size` asks for the
/// size of the content alone, the container's own width, height, min, max and aspect ratio
/// ignored (Taffy's `SizingMode::ContentSize`): what a parent flex container asks of an
/// item, whose size styles it applies itself.
struct flex_input {
    float known_width = std::numeric_limits<float>::quiet_NaN();
    float known_height = std::numeric_limits<float>::quiet_NaN();
    float available_width = std::numeric_limits<float>::infinity();
    float available_height = std::numeric_limits<float>::infinity();
    bool min_content_width = false;
    float parent_width = std::numeric_limits<float>::quiet_NaN();
    float parent_height = std::numeric_limits<float>::quiet_NaN();
    float rem_px = 16.f;
    bool content_size = false;
};

/// The container's border-box size, and its content size: the extent of its children's
/// margin boxes plus its end padding, measured from its padding box's origin and never
/// smaller than the padding box (CSS Overflow 3 §2.2's scrollable overflow for a flex
/// container). A scroll owner's range is `content - padding box` on each axis. `inner_*` is
/// the container's content-box size (NaN where indefinite): its children's percent base.
struct flex_output {
    float width = 0.f;
    float height = 0.f;
    float content_width = 0.f;
    float content_height = 0.f;
    float inner_width = std::numeric_limits<float>::quiet_NaN();
    float inner_height = std::numeric_limits<float>::quiet_NaN();
};

/// Per-item state of one solve; engine-private, caller-owned storage.
struct flex_item_state {
    float margin[4];   ///< top, right, bottom, left
    float size[2];     ///< style width / height (border box), NaN: auto
    float min_size[2]; ///< specified minimum, NaN: automatic
    float max_size[2]; ///< NaN: none
    float pb[2];       ///< padding + border, horizontal / vertical
    float rel[2];      ///< `position: relative` offset
    float grow;
    float shrink;
    float basis;       ///< flex base size (border box)
    float min_main;    ///< resolved minimum main size, NaN until needed
    float hyp[2];      ///< hypothetical sizes (border box)
    float target[2];   ///< used sizes (border box)
    float query_cross; ///< the cross size a measurement is told (stretch), NaN: none
    float cross_avail; ///< definite cross space for measurement, or ±inf (max/min-content)
    float violation;
    float offset_main;
    float offset_cross;
    std::uint16_t child;
    std::uint8_t auto_margins; ///< bit per edge, CSS order
    item_align align;
    bool frozen;
    bool basis_from_content;
    bool clip_main; ///< overflow not visible on the main axis: automatic minimum is 0
    bool scroll_container;
};

struct flex_line_state {
    unsigned first = 0;
    unsigned count = 0;
    float cross = 0.f;
    float offset = 0.f;
};

/// Caller-provided scratch: at least one item and one line per child.
struct flex_scratch {
    std::span<flex_item_state> items;
    std::span<flex_line_state> lines;
};

/// Lay out the children of one flex container. `out[i]` receives child `i`'s border box
/// relative to the container's border-box origin (absolutely positioned children
/// included). Fails only when `out` or `scratch` is shorter than `children.count`; then
/// nothing is written. Allocates nothing.
[[nodiscard]] lx::result<flex_output> solve_flex(const flex_style& container,
                                                 const flex_input& input,
                                                 const flex_children& children,
                                                 std::span<lx::rect2f> out, flex_scratch scratch);

} // namespace lx::layout

// ── implementation ──────────────────────────────────────────────────────────────

namespace {

using lx::layout::content_align;
using lx::layout::flex_item_state;
using lx::layout::flex_line_state;
using lx::layout::flex_style;
using lx::layout::item_align;
using lx::layout::length;

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float k_inf = std::numeric_limits<float>::infinity();

[[nodiscard]] constexpr bool def(float v) {
    return v == v;
}
[[nodiscard]] constexpr float or_(float a, float b) {
    return def(a) ? a : b;
}
[[nodiscard]] constexpr float fmax(float a, float b) {
    return a > b ? a : b;
}
[[nodiscard]] constexpr float fmin(float a, float b) {
    return a < b ? a : b;
}
/// `a` raised to `b` when `b` is definite (NaN `a` stays NaN).
[[nodiscard]] constexpr float maybe_max(float a, float b) {
    return def(a) && def(b) ? fmax(a, b) : a;
}
[[nodiscard]] constexpr float maybe_min(float a, float b) {
    return def(a) && def(b) ? fmin(a, b) : a;
}
/// CSS clamping: the max applies first, the min wins over it.
[[nodiscard]] constexpr float maybe_clamp(float v, float lo, float hi) {
    return maybe_max(maybe_min(v, hi), lo);
}

/// Edge indices in CSS order, per physical axis (0: horizontal, 1: vertical).
[[nodiscard]] constexpr unsigned start_edge(unsigned axis) {
    return axis == 0 ? 3u : 0u;
}
[[nodiscard]] constexpr unsigned end_edge(unsigned axis) {
    return axis == 0 ? 1u : 2u;
}

[[nodiscard]] float resolve_or_zero(length l, float rem, float base) {
    const float v = lx::layout::resolve(l, rem, base);
    return def(v) ? v : 0.f;
}

void resolve_edges(const lx::layout::edges& e, float rem, float base, float out[4]) {
    out[0] = resolve_or_zero(e.top, rem, base);
    out[1] = resolve_or_zero(e.right, rem, base);
    out[2] = resolve_or_zero(e.bottom, rem, base);
    out[3] = resolve_or_zero(e.left, rem, base);
}

/// Fills an unknown side from the other through `ratio` (width / height).
void apply_aspect(float size[2], float ratio) {
    if (!(ratio > 0.f))
        return;
    if (def(size[0]) && !def(size[1]))
        size[1] = size[0] / ratio;
    else if (def(size[1]) && !def(size[0]))
        size[0] = size[1] * ratio;
}

/// Available space on one axis: a definite value, or ±infinity for max-/min-content.
[[nodiscard]] constexpr bool is_definite(float space) {
    return space > -k_inf && space < k_inf;
}
constexpr float k_min_content = -k_inf;
constexpr float k_max_content = k_inf;

/// Taffy's `apply_alignment_fallback` (CSS Box Alignment 3 §4.3, §5.3): distributed
/// alignment with no positive free space, or with one subject only, falls back to a *safe*
/// `flex_start` (`stretch`, `space-between`) or `center` (`space-around`, `space-evenly`),
/// and a safe alignment that would overflow aligns to `start`.
[[nodiscard]] content_align fallback(content_align mode, float free, unsigned n) {
    bool safe = false;
    if (n <= 1 || free <= 0.f) {
        switch (mode) {
        case content_align::stretch:
        case content_align::space_between:
            mode = content_align::flex_start;
            safe = true;
            break;
        case content_align::space_around:
        case content_align::space_evenly:
            mode = content_align::center;
            safe = true;
            break;
        default:
            break;
        }
    }
    if (free <= 0.f && safe)
        mode = content_align::start;
    return mode;
}

/// Taffy's `compute_alignment_offset`: the space before subject `first` / between subjects.
/// `reversed` swaps which physical end `flex_start` and `flex_end` mean.
[[nodiscard]] float align_offset(content_align mode, float free, unsigned n, float gap,
                                 bool reversed, bool first) {
    if (first) {
        switch (mode) {
        case content_align::start:
            return 0.f;
        case content_align::end:
            return free;
        case content_align::flex_start:
            return reversed ? free : 0.f;
        case content_align::flex_end:
            return reversed ? 0.f : free;
        case content_align::center:
            return free / 2.f;
        case content_align::stretch:
        case content_align::space_between:
            return 0.f;
        case content_align::space_around:
            return free >= 0.f ? free / static_cast<float>(n) / 2.f : free / 2.f;
        case content_align::space_evenly:
            return free >= 0.f ? free / static_cast<float>(n + 1) : free / 2.f;
        }
        return 0.f;
    }
    const float f = fmax(free, 0.f);
    switch (mode) {
    case content_align::space_between:
        return gap + f / static_cast<float>(n - 1);
    case content_align::space_around:
        return gap + f / static_cast<float>(n);
    case content_align::space_evenly:
        return gap + f / static_cast<float>(n + 1);
    default:
        return gap;
    }
}

/// One container's solve: its constants and the child-measurement plumbing.
struct solver {
    const flex_style& cs;
    const lx::layout::flex_children& ch;
    float rem = 16.f;
    bool is_row = true;
    bool reverse = false;
    bool wrap = false;
    bool wrap_reverse = false;
    unsigned m = 0; ///< main axis (0: horizontal)
    unsigned c = 1; ///< cross axis
    float padding[4]{};
    float border[4]{};
    float inset_sum[2]{}; ///< padding + border per axis
    float own_min[2]{};   ///< the container's min / max (border box), NaN: none
    float own_max[2]{};
    float outer[2]{}; ///< the container's border-box size, NaN until known
    float inner[2]{}; ///< its content-box size, NaN until known
    bool known_main = false;
    float avail[2]{}; ///< available inner space (definite, or ±inf)
    float gap[2]{};   ///< main, cross

    [[nodiscard]] const flex_style& style_of(unsigned i) const {
        const flex_style* s = ch.style ? ch.style(ch.context, i) : nullptr;
        return s ? *s : lx::layout::k_default_flex_style;
    }

    /// Measures child `i` with sizes given in flow axes. Unknown sides get the available
    /// space; a min-content width is passed as such, a min-content height as max-content.
    [[nodiscard]] lx::layout::flex_size ask(unsigned i, float known_main_size, float known_cross,
                                            float avail_main, float avail_cross) const {
        lx::layout::child_query q{};
        const float kw = is_row ? known_main_size : known_cross;
        const float kh = is_row ? known_cross : known_main_size;
        const float aw = is_row ? avail_main : avail_cross;
        const float ah = is_row ? avail_cross : avail_main;
        q.known_width = kw;
        q.known_height = kh;
        q.min_content_width = !def(kw) && aw == k_min_content;
        q.available_width = is_definite(aw) ? fmax(aw, 0.f) : k_inf;
        q.available_height = is_definite(ah) ? fmax(ah, 0.f) : k_inf;
        q.parent_width = inner[0];
        q.parent_height = inner[1];
        if (!ch.measure)
            return {def(kw) ? kw : 0.f, def(kh) ? kh : 0.f};
        return ch.measure(ch.context, i, q);
    }

    [[nodiscard]] static float main_of(lx::layout::flex_size s, unsigned axis) {
        return axis == 0 ? s.width : s.height;
    }

    [[nodiscard]] float margin_sum(const flex_item_state& it, unsigned axis) const {
        return it.margin[start_edge(axis)] + it.margin[end_edge(axis)];
    }

    [[nodiscard]] bool stretched(const flex_item_state& it) const {
        const unsigned cross_bits = (1u << start_edge(c)) | (1u << end_edge(c));
        return it.align == item_align::stretch && !def(it.size[c]) &&
               (it.auto_margins & cross_bits) == 0;
    }

    /// The resolved minimum main size (§4.5): the specified minimum, or the automatic one —
    /// 0 for a scroll container, else the min-content main size capped by the specified size
    /// and the max — never below padding + border. Measures at most once per solve.
    float min_main(flex_item_state& it) const {
        if (def(it.min_main))
            return it.min_main;
        float v = it.min_size[m];
        if (!def(v)) {
            if (it.clip_main) {
                v = 0.f;
            } else {
                const float min_content =
                    main_of(ask(it.child, k_nan, it.query_cross,
                                is_row ? k_min_content : k_max_content, it.cross_avail),
                            m);
                v = maybe_min(maybe_min(min_content, it.size[m]), it.max_size[m]);
            }
        }
        it.min_main = fmax(v, it.pb[m]);
        return it.min_main;
    }

    [[nodiscard]] float clamp_main(flex_item_state& it, float v, bool need_min) const {
        v = maybe_min(v, it.max_size[m]);
        float lo = it.min_main;
        if (need_min)
            lo = min_main(it);
        else if (!def(lo))
            lo = fmax(or_(it.min_size[m], 0.f), it.pb[m]); // the automatic minimum left unresolved
        return fmax(v, lo);
    }
};

} // namespace

lx::result<lx::layout::flex_output>
lx::layout::solve_flex(const flex_style& cs, const flex_input& in, const flex_children& ch,
                       std::span<lx::rect2f> out, flex_scratch scratch) {
    const unsigned n = ch.count;
    if (out.size() < n || scratch.items.size() < n || scratch.lines.size() < (n > 0 ? n : 1u))
        return lx::invalid_argument("lx::layout::solve_flex: scratch shorter than the child count");

    solver s{cs, ch};
    s.rem = in.rem_px;
    s.is_row = cs.direction == flex_direction::row || cs.direction == flex_direction::row_reverse;
    s.reverse = cs.direction == flex_direction::row_reverse ||
                cs.direction == flex_direction::column_reverse;
    s.wrap = cs.wrap != flex_wrap::no_wrap;
    s.wrap_reverse = cs.wrap == flex_wrap::wrap_reverse;
    s.m = s.is_row ? 0u : 1u;
    s.c = 1u - s.m;
    const unsigned m = s.m;
    const unsigned c = s.c;
    const float rem = s.rem;

    // ── The container's own box ──────────────────────────────────────────────────
    resolve_edges(cs.padding, rem, in.parent_width, s.padding);
    resolve_edges(cs.border, rem, in.parent_width, s.border);
    s.inset_sum[0] = s.padding[1] + s.padding[3] + s.border[1] + s.border[3];
    s.inset_sum[1] = s.padding[0] + s.padding[2] + s.border[0] + s.border[2];
    const float parent[2] = {in.parent_width, in.parent_height};
    float style_size[2] = {k_nan, k_nan};
    s.own_min[0] = s.own_min[1] = s.own_max[0] = s.own_max[1] = k_nan;
    const float aspect = in.content_size ? 0.f : cs.aspect_ratio;
    if (!in.content_size) {
        s.own_min[0] = resolve(cs.min_width, rem, parent[0]);
        s.own_min[1] = resolve(cs.min_height, rem, parent[1]);
        s.own_max[0] = resolve(cs.max_width, rem, parent[0]);
        s.own_max[1] = resolve(cs.max_height, rem, parent[1]);
        style_size[0] = resolve(cs.width, rem, parent[0]);
        style_size[1] = resolve(cs.height, rem, parent[1]);
        apply_aspect(style_size, aspect);
    }
    const float known_in[2] = {in.known_width, in.known_height};
    for (unsigned a = 0; a < 2; ++a) {
        float v = known_in[a];
        if (!def(v))
            v = maybe_clamp(style_size[a], s.own_min[a], s.own_max[a]);
        s.outer[a] = maybe_max(v, s.inset_sum[a]);
    }
    if (aspect > 0.f) {
        if (def(s.outer[0]) && !def(s.outer[1]))
            s.outer[1] = fmax(s.outer[0] / aspect, s.inset_sum[1]);
        else if (def(s.outer[1]) && !def(s.outer[0]))
            s.outer[0] = fmax(s.outer[1] * aspect, s.inset_sum[0]);
    }
    s.known_main = def(s.outer[m]);
    const bool known_cross = def(s.outer[c]);
    for (unsigned a = 0; a < 2; ++a)
        s.inner[a] = def(s.outer[a]) ? fmax(s.outer[a] - s.inset_sum[a], 0.f) : k_nan;

    // Available inner space (§9.2 step 2).
    const float avail_in[2] = {in.available_width, in.available_height};
    for (unsigned a = 0; a < 2; ++a) {
        if (def(s.inner[a]))
            s.avail[a] = s.inner[a];
        else if (a == 0 && in.min_content_width)
            s.avail[a] = k_min_content;
        else if (is_definite(avail_in[a]))
            s.avail[a] = fmax(avail_in[a] - s.inset_sum[a], 0.f);
        else
            s.avail[a] = k_max_content;
    }
    {
        const float col = resolve_or_zero(cs.column_gap, rem, s.inner[0]);
        const float row = resolve_or_zero(cs.row_gap, rem, s.inner[1]);
        s.gap[0] = s.is_row ? col : row;
        s.gap[1] = s.is_row ? row : col;
    }

    // ── §9.1–9.2: items, flex base sizes, hypothetical main sizes ─────────────────
    flex_item_state* items = scratch.items.data();
    unsigned count = 0;
    const float pct_main = s.known_main ? s.inner[m] : k_nan; // Taffy: main % needs a definite main
    for (unsigned i = 0; i < n; ++i) {
        const flex_style& st = s.style_of(i);
        if (st.position == positioning::absolute)
            continue;
        flex_item_state& it = items[count++];
        it = {};
        it.child = static_cast<std::uint16_t>(i);
        const float pct[2] = {m == 0 ? pct_main : s.inner[0], m == 1 ? pct_main : s.inner[1]};
        resolve_edges(st.margin, rem, s.inner[0], it.margin);
        const length* me[4] = {&st.margin.top, &st.margin.right, &st.margin.bottom,
                               &st.margin.left};
        for (unsigned e = 0; e < 4; ++e)
            if (me[e]->is_auto())
                it.auto_margins = static_cast<std::uint8_t>(it.auto_margins | (1u << e));
        float pad[4];
        float bor[4];
        resolve_edges(st.padding, rem, s.inner[0], pad);
        resolve_edges(st.border, rem, s.inner[0], bor);
        it.pb[0] = pad[1] + pad[3] + bor[1] + bor[3];
        it.pb[1] = pad[0] + pad[2] + bor[0] + bor[2];
        it.size[0] = resolve(st.width, rem, pct[0]);
        it.size[1] = resolve(st.height, rem, pct[1]);
        apply_aspect(it.size, st.aspect_ratio);
        it.min_size[0] = resolve(st.min_width, rem, pct[0]);
        it.min_size[1] = resolve(st.min_height, rem, pct[1]);
        it.max_size[0] = resolve(st.max_width, rem, pct[0]);
        it.max_size[1] = resolve(st.max_height, rem, pct[1]);
        for (unsigned a = 0; a < 2; ++a)
            it.size[a] =
                maybe_max(maybe_clamp(it.size[a], it.min_size[a], it.max_size[a]), it.pb[a]);
        it.grow = st.flex_grow;
        it.shrink = st.flex_shrink;
        it.align = st.align_self == item_align::automatic ? cs.align_items : st.align_self;
        if (it.align == item_align::automatic)
            it.align = item_align::stretch;
        const overflow ov_main = m == 0 ? st.overflow_x : st.overflow_y;
        it.clip_main = ov_main != overflow::visible;
        it.scroll_container =
            st.overflow_x != overflow::visible || st.overflow_y != overflow::visible;
        if (st.position == positioning::relative) {
            const float l = resolve(st.inset.left, rem, s.inner[0]);
            const float r = resolve(st.inset.right, rem, s.inner[0]);
            const float t = resolve(st.inset.top, rem, s.inner[1]);
            const float b = resolve(st.inset.bottom, rem, s.inner[1]);
            it.rel[0] = def(l) ? l : (def(r) ? -r : 0.f);
            it.rel[1] = def(t) ? t : (def(b) ? -b : 0.f);
        }
        it.min_main = k_nan;

        // The cross space a measurement sees, and the stretched cross size it is told
        // (Taffy's `determine_flex_base_size`: a stretched item is measured at the cross
        // space it will fill, so wrapping content wraps at the right width).
        const float ms_cross = s.margin_sum(it, c);
        if (is_definite(s.avail[c]))
            it.cross_avail = maybe_clamp(fmax(or_(s.inner[c], s.avail[c]) - ms_cross, 0.f),
                                         it.min_size[c], it.max_size[c]);
        else
            it.cross_avail = s.avail[c];
        it.query_cross = it.size[c];
        if (!def(it.query_cross) && s.stretched(it) && is_definite(it.cross_avail))
            it.query_cross = it.cross_avail;

        // Flex base size (§9.2 step 3).
        const float basis = resolve(st.flex_basis, rem, pct_main);
        bool basis_is_size = false;
        if (def(basis)) {
            it.basis = basis;
        } else if (st.flex_basis.is_auto() && def(it.size[m])) {
            it.basis = it.size[m];
            basis_is_size = true;
        } else if (st.aspect_ratio > 0.f && def(it.size[c])) {
            it.basis = s.is_row ? it.size[c] * st.aspect_ratio : it.size[c] / st.aspect_ratio;
        } else {
            const float avail_main = s.avail[m] == k_min_content ? k_min_content : k_max_content;
            it.basis =
                solver::main_of(s.ask(i, k_nan, it.query_cross, avail_main, it.cross_avail), m);
            it.basis_from_content = true;
        }
        it.basis = fmax(it.basis, it.pb[m]);
        // The automatic minimum cannot bind on a base size that is the max-content size (it
        // is at least the min-content size) or the item's own definite size (it is at most
        // that, §4.5); any other base size needs it now. A shrinking line resolves it then.
        it.hyp[m] = s.clamp_main(it, it.basis, !it.basis_from_content && !basis_is_size);
    }

    // ── §9.3: lines ─────────────────────────────────────────────────────────────
    flex_line_state* lines = scratch.lines.data();
    unsigned line_count = 0;
    {
        float limit = k_max_content;
        if (s.wrap) {
            if (s.known_main) {
                limit = s.inner[m];
            } else if (def(s.own_max[m])) {
                const float max_inner = fmax(s.own_max[m] - s.inset_sum[m], 0.f);
                limit = is_definite(s.avail[m]) ? fmin(s.avail[m], max_inner) : max_inner;
                if (def(s.own_min[m]))
                    limit = fmax(limit, s.own_min[m] - s.inset_sum[m]);
            } else if (!s.is_row) {
                // Taffy `wrap_column_auto_height_does_not_wrap_against_available_space`: an
                // auto-height column does not wrap against the available height.
                limit = k_max_content;
            } else {
                limit = s.avail[m];
            }
        }
        if (!s.wrap || limit == k_max_content || count == 0) {
            lines[line_count++] = {0, count, 0.f, 0.f};
        } else if (limit == k_min_content) {
            for (unsigned i = 0; i < count; ++i)
                lines[line_count++] = {i, 1, 0.f, 0.f};
        } else {
            unsigned first = 0;
            float used = 0.f;
            for (unsigned i = 0; i < count; ++i) {
                const float outer_main = items[i].hyp[m] + s.margin_sum(items[i], m);
                const float add = (i == first ? 0.f : s.gap[0]) + outer_main;
                if (i != first && used + add > limit) {
                    lines[line_count++] = {first, i - first, 0.f, 0.f};
                    first = i;
                    used = outer_main;
                } else {
                    used += add;
                }
            }
            lines[line_count++] = {first, count - first, 0.f, 0.f};
        }
    }

    // ── §9.2 step 4: the container's main size, when it is not known ──────────────
    if (!s.known_main) {
        float longest = 0.f;
        for (unsigned l = 0; l < line_count; ++l) {
            const flex_line_state& line = lines[l];
            float sum = line.count > 1 ? s.gap[0] * static_cast<float>(line.count - 1) : 0.f;
            float largest = 0.f;
            for (unsigned k = line.first; k < line.first + line.count; ++k) {
                flex_item_state& it = items[k];
                const float ms = s.margin_sum(it, m);
                float contribution = 0.f;
                if (is_definite(s.avail[m])) {
                    // Taffy: base size raised to the specified minimum.
                    contribution = fmax(maybe_max(it.basis, it.min_size[m]) + ms, it.pb[m]);
                } else {
                    // Max-/min-content contribution (Taffy's `determine_container_main_size`
                    // for an indefinite main size; §9.9.1, see the file comment).
                    const float pref = it.size[m];
                    const float res_min =
                        def(it.min_main) ? it.min_main : fmax(or_(it.min_size[m], 0.f), it.pb[m]);
                    const float clamping_basis = s.is_row ? maybe_max(it.basis, pref) : it.basis;
                    const float fb_min = it.shrink == 0.f ? clamping_basis : k_nan;
                    const float fb_max = it.grow == 0.f ? clamping_basis : k_nan;
                    float lo = or_(maybe_max(it.min_size[m], fb_min), fb_min);
                    lo = def(lo) ? fmax(lo, res_min) : res_min;
                    float hi = or_(maybe_min(it.max_size[m], fb_max), fb_max);
                    if (!def(hi))
                        hi = k_inf;
                    if (def(pref) && (hi <= lo || hi <= pref)) {
                        contribution = fmax(fmin(pref, hi), lo) + ms;
                    } else if (hi <= lo) {
                        contribution = lo + ms;
                    } else if (it.scroll_container) {
                        contribution = it.basis + ms;
                    } else if (def(pref)) {
                        const float inner_size = fmax(pref, it.pb[m]);
                        contribution =
                            maybe_clamp((s.is_row ? inner_size : fmax(inner_size, it.basis)) + ms,
                                        it.min_size[m], it.max_size[m]);
                    } else {
                        // The content size under the container's own sizing mode: the same
                        // query as a content-based flex base size, so reuse that.
                        const float measured =
                            it.basis_from_content
                                ? it.basis
                                : solver::main_of(s.ask(it.child, k_nan, it.query_cross, s.avail[m],
                                                        it.cross_avail),
                                                  m);
                        contribution =
                            maybe_clamp((s.is_row ? measured : fmax(measured, it.basis)) + ms,
                                        it.min_size[m], it.max_size[m]);
                    }
                }
                sum += contribution;
                largest = fmax(largest, contribution);
            }
            if (s.wrap && s.is_row)
                sum = fmax(sum, largest);
            longest = fmax(longest, sum);
        }
        float size = longest + s.inset_sum[m];
        if (line_count > 1 && is_definite(s.avail[m]))
            size = fmax(size, s.avail[m] + s.inset_sum[m]);
        size = fmax(maybe_clamp(size, s.own_min[m], s.own_max[m]), s.inset_sum[m]);
        s.outer[m] = size;
        s.inner[m] = fmax(size - s.inset_sum[m], 0.f);
        // Percentage gaps resolve against the size just found (Taffy re-resolves them too).
        const length main_gap = s.is_row ? cs.column_gap : cs.row_gap;
        s.gap[0] = resolve_or_zero(main_gap, rem, s.inner[m]);
    }
    const float inner_main = s.inner[m];

    // ── §9.7: resolve flexible lengths, line by line ─────────────────────────────
    for (unsigned l = 0; l < line_count; ++l) {
        const flex_line_state& line = lines[l];
        flex_item_state* li = items + line.first;
        const unsigned ln = line.count;
        const float gaps = ln > 1 ? s.gap[0] * static_cast<float>(ln - 1) : 0.f;
        float hyp_sum = 0.f;
        for (unsigned k = 0; k < ln; ++k)
            hyp_sum += li[k].hyp[m] + s.margin_sum(li[k], m);
        const bool growing = gaps + hyp_sum < inner_main;
        const bool shrinking = gaps + hyp_sum > inner_main;

        // Step 2: freeze inflexible items at their hypothetical size. CSS freezes an item
        // whose flex factor *for this pass* is 0; Taffy only one with both factors 0, which
        // lands on the same sizes through the violation loop.
        for (unsigned k = 0; k < ln; ++k) {
            flex_item_state& it = li[k];
            it.target[m] = it.hyp[m];
            it.frozen = (!growing && !shrinking) || (growing && it.grow == 0.f) ||
                        (shrinking && it.shrink == 0.f) || (growing && it.basis > it.hyp[m]) ||
                        (shrinking && it.basis < it.hyp[m]);
        }
        if (!growing && !shrinking)
            continue;

        // Step 3: initial free space.
        float used = gaps;
        for (unsigned k = 0; k < ln; ++k)
            used += (li[k].frozen ? li[k].target[m] : li[k].basis) + s.margin_sum(li[k], m);
        const float initial_free = inner_main - used;

        // Step 4: the loop. Each round freezes at least one item, so it ends in ≤ ln rounds.
        for (unsigned round = 0; round <= ln; ++round) {
            float items_space = 0.f;
            float sum_grow = 0.f;
            float sum_shrink = 0.f;
            bool all_frozen = true;
            for (unsigned k = 0; k < ln; ++k) {
                const flex_item_state& it = li[k];
                if (it.frozen) {
                    items_space += it.target[m] + s.margin_sum(it, m);
                } else {
                    all_frozen = false;
                    items_space += it.basis + s.margin_sum(it, m);
                    sum_grow += it.grow;
                    sum_shrink += it.shrink;
                }
            }
            if (all_frozen)
                break;
            float free = inner_main - gaps - items_space;
            if (growing && sum_grow < 1.f)
                free = fmin(initial_free * sum_grow, free);
            else if (shrinking && sum_shrink < 1.f)
                free = fmax(initial_free * sum_shrink, free);

            if (free != 0.f && def(free)) {
                if (growing && sum_grow > 0.f) {
                    for (unsigned k = 0; k < ln; ++k)
                        if (!li[k].frozen)
                            li[k].target[m] = li[k].basis + free * (li[k].grow / sum_grow);
                } else if (shrinking && sum_shrink > 0.f) {
                    float sum_scaled = 0.f;
                    for (unsigned k = 0; k < ln; ++k)
                        if (!li[k].frozen)
                            sum_scaled += (li[k].basis - li[k].pb[m]) * li[k].shrink;
                    if (sum_scaled > 0.f) {
                        for (unsigned k = 0; k < ln; ++k) {
                            flex_item_state& it = li[k];
                            if (!it.frozen)
                                it.target[m] = it.basis + free * ((it.basis - it.pb[m]) *
                                                                  it.shrink / sum_scaled);
                        }
                    }
                }
            } else {
                for (unsigned k = 0; k < ln; ++k)
                    if (!li[k].frozen)
                        li[k].target[m] = li[k].basis;
            }

            // Fix min/max violations. A growing line cannot push a content-based item below
            // its automatic minimum, so that is only measured when shrinking.
            float total_violation = 0.f;
            for (unsigned k = 0; k < ln; ++k) {
                flex_item_state& it = li[k];
                if (it.frozen)
                    continue;
                const float clamped = fmax(s.clamp_main(it, it.target[m], shrinking), 0.f);
                it.violation = clamped - it.target[m];
                it.target[m] = clamped;
                total_violation += it.violation;
            }
            for (unsigned k = 0; k < ln; ++k) {
                flex_item_state& it = li[k];
                if (it.frozen)
                    continue;
                if (total_violation > 0.f)
                    it.frozen = it.violation > 0.f;
                else if (total_violation < 0.f)
                    it.frozen = it.violation < 0.f;
                else
                    it.frozen = true;
            }
        }
    }

    // ── §9.4: cross sizes ───────────────────────────────────────────────────────
    const bool line_cross_known = !s.wrap && known_cross;
    for (unsigned k = 0; k < count; ++k) {
        flex_item_state& it = items[k];
        if (line_cross_known && s.stretched(it)) {
            it.hyp[c] = 0.f; // the line's cross size, filled in step 11
            continue;
        }
        float cross = it.size[c];
        if (!def(cross)) {
            const float avail_main = fmax(s.outer[m] - s.margin_sum(it, m), 0.f);
            float avail_cross = it.cross_avail;
            if (is_definite(avail_cross))
                avail_cross = fmax(avail_cross, it.pb[c]);
            cross =
                solver::main_of(s.ask(it.child, it.target[m], k_nan, avail_main, avail_cross), c);
            cross = maybe_clamp(cross, it.min_size[c], it.max_size[c]);
        }
        it.hyp[c] = fmax(cross, it.pb[c]);
    }

    // Step 8: line cross sizes.
    const float cross_inset = s.inset_sum[c];
    if (line_cross_known) {
        lines[0].cross =
            fmax(maybe_clamp(s.outer[c], s.own_min[c], s.own_max[c]) - cross_inset, 0.f);
    } else {
        for (unsigned l = 0; l < line_count; ++l) {
            float cross = 0.f;
            for (unsigned k = lines[l].first; k < lines[l].first + lines[l].count; ++k)
                cross = fmax(cross, items[k].hyp[c] + s.margin_sum(items[k], c));
            lines[l].cross = cross;
        }
        if (!s.wrap) {
            const float lo = def(s.own_min[c]) ? s.own_min[c] - cross_inset : k_nan;
            const float hi = def(s.own_max[c]) ? s.own_max[c] - cross_inset : k_nan;
            lines[0].cross = maybe_clamp(lines[0].cross, lo, hi);
        }
    }

    // Step 9: `align-content: stretch` grows the lines into a definite (or minimum) cross.
    const float cross_gaps = line_count > 1 ? s.gap[1] * static_cast<float>(line_count - 1) : 0.f;
    if (cs.align_content == content_align::stretch) {
        const float base = or_(s.outer[c], s.own_min[c]);
        const float min_inner =
            def(base) ? fmax(maybe_clamp(base, s.own_min[c], s.own_max[c]) - cross_inset, 0.f)
                      : 0.f;
        float total = cross_gaps;
        for (unsigned l = 0; l < line_count; ++l)
            total += lines[l].cross;
        if (total < min_inner) {
            const float add = (min_inner - total) / static_cast<float>(line_count);
            for (unsigned l = 0; l < line_count; ++l)
                lines[l].cross += add;
        }
    }

    // Step 11: used cross sizes.
    for (unsigned l = 0; l < line_count; ++l) {
        for (unsigned k = lines[l].first; k < lines[l].first + lines[l].count; ++k) {
            flex_item_state& it = items[k];
            if (s.stretched(it)) {
                const float v = fmax(lines[l].cross - s.margin_sum(it, c), 0.f);
                it.target[c] = fmax(maybe_clamp(v, it.min_size[c], it.max_size[c]), it.pb[c]);
            } else {
                it.target[c] = it.hyp[c];
            }
        }
    }

    // ── §9.5: main-axis alignment ───────────────────────────────────────────────
    for (unsigned l = 0; l < line_count; ++l) {
        const flex_line_state& line = lines[l];
        flex_item_state* li = items + line.first;
        const unsigned ln = line.count;
        float used = ln > 1 ? s.gap[0] * static_cast<float>(ln - 1) : 0.f;
        unsigned autos = 0;
        const unsigned ms = start_edge(m);
        const unsigned me = end_edge(m);
        for (unsigned k = 0; k < ln; ++k) {
            used += li[k].target[m] + s.margin_sum(li[k], m);
            autos += ((li[k].auto_margins >> ms) & 1u) + ((li[k].auto_margins >> me) & 1u);
        }
        float free = inner_main - used;
        if (free > 0.f && autos > 0) {
            const float each = free / static_cast<float>(autos);
            for (unsigned k = 0; k < ln; ++k) {
                if (li[k].auto_margins & (1u << ms))
                    li[k].margin[ms] = each;
                if (li[k].auto_margins & (1u << me))
                    li[k].margin[me] = each;
            }
            free = 0.f;
        }
        content_align mode = cs.justify_content;
        if (mode == content_align::stretch)
            mode = content_align::flex_start;
        mode = fallback(mode, free, ln);
        // Items are placed in visual order from the main-start padding edge; a reversed
        // direction walks them backwards, so `flex_start` packs against the far end.
        for (unsigned v = 0; v < ln; ++v) {
            flex_item_state& it = s.reverse ? li[ln - 1 - v] : li[v];
            it.offset_main = align_offset(mode, free, ln, s.gap[0], s.reverse, v == 0);
        }
    }

    // ── §9.6: cross-axis alignment ──────────────────────────────────────────────
    for (unsigned l = 0; l < line_count; ++l) {
        const unsigned cs_edge = start_edge(c);
        const unsigned ce_edge = end_edge(c);
        for (unsigned k = lines[l].first; k < lines[l].first + lines[l].count; ++k) {
            flex_item_state& it = items[k];
            const float free = lines[l].cross - it.target[c] - s.margin_sum(it, c);
            const bool a_start = (it.auto_margins & (1u << cs_edge)) != 0;
            const bool a_end = (it.auto_margins & (1u << ce_edge)) != 0;
            if (a_start || a_end) {
                // §9.6 step 13: positive free space goes to the auto margins; with none,
                // an auto start margin is 0 (Taffy hands out negative space too).
                const float f = fmax(free, 0.f);
                if (a_start && a_end) {
                    it.margin[cs_edge] = f / 2.f;
                    it.margin[ce_edge] = f / 2.f;
                } else if (a_start) {
                    it.margin[cs_edge] = f;
                } else {
                    it.margin[ce_edge] = f;
                }
                it.offset_cross = 0.f;
                continue;
            }
            switch (it.align) {
            case item_align::start:
                it.offset_cross = 0.f;
                break;
            case item_align::end:
                it.offset_cross = free;
                break;
            case item_align::center:
                it.offset_cross = free / 2.f;
                break;
            case item_align::flex_end:
                it.offset_cross = s.wrap_reverse ? 0.f : free;
                break;
            case item_align::flex_start:
            case item_align::stretch:
            case item_align::automatic:
            default:
                it.offset_cross = s.wrap_reverse ? free : 0.f;
                break;
            }
        }
    }

    // Step 15: the container's cross size.
    float total_line_cross = 0.f;
    for (unsigned l = 0; l < line_count; ++l)
        total_line_cross += lines[l].cross;
    if (!known_cross) {
        float size = total_line_cross + cross_gaps + cross_inset;
        size = fmax(maybe_clamp(size, s.own_min[c], s.own_max[c]), cross_inset);
        s.outer[c] = size;
        s.inner[c] = fmax(size - cross_inset, 0.f);
    }

    // Step 16: align the lines (`align-content`).
    {
        const float free = s.inner[c] - total_line_cross - cross_gaps;
        const content_align mode = fallback(cs.align_content, free, line_count);
        for (unsigned v = 0; v < line_count; ++v) {
            flex_line_state& line = s.wrap_reverse ? lines[line_count - 1 - v] : lines[v];
            line.offset = align_offset(mode, free, line_count, s.gap[1], s.wrap_reverse, v == 0);
        }
    }

    // ── Final positions (Taffy's `final_layout_pass`) ────────────────────────────
    const float pad_origin[2] = {s.border[3], s.border[0]};
    const float content_start[2] = {s.padding[3] + s.border[3], s.padding[0] + s.border[0]};
    float extent[2] = {0.f, 0.f}; // children's margin-box far edges, from the padding-box origin
    float total_cross = content_start[c];
    for (unsigned v = 0; v < line_count; ++v) {
        const flex_line_state& line = s.wrap_reverse ? lines[line_count - 1 - v] : lines[v];
        float total_main = content_start[m];
        for (unsigned w = 0; w < line.count; ++w) {
            flex_item_state& it = items[line.first + (s.reverse ? line.count - 1 - w : w)];
            const float main_pos =
                total_main + it.offset_main + it.margin[start_edge(m)] + it.rel[m];
            const float cross_pos =
                total_cross + line.offset + it.offset_cross + it.margin[start_edge(c)] + it.rel[c];
            total_main += it.offset_main + s.margin_sum(it, m) + it.target[m];
            lx::rect2f& r = out[it.child];
            r.x = m == 0 ? main_pos : cross_pos;
            r.y = m == 0 ? cross_pos : main_pos;
            r.width = it.target[0];
            r.height = it.target[1];
            extent[0] = fmax(extent[0], r.x + r.width + it.margin[1] - pad_origin[0]);
            extent[1] = fmax(extent[1], r.y + r.height + it.margin[2] - pad_origin[1]);
        }
        total_cross += line.offset + line.cross;
    }

    // ── Absolutely positioned children (CSS Flexbox §4.1, Taffy's absolute pass) ──
    // The containing block is the padding box; insets and percentages resolve against it.
    const float cb[2] = {fmax(s.outer[0] - s.border[1] - s.border[3], 0.f),
                         fmax(s.outer[1] - s.border[0] - s.border[2], 0.f)};
    for (unsigned i = 0; i < n; ++i) {
        const flex_style& st = s.style_of(i);
        if (st.position != positioning::absolute)
            continue;
        float margin[4];
        const length* me[4] = {&st.margin.top, &st.margin.right, &st.margin.bottom,
                               &st.margin.left};
        for (unsigned e = 0; e < 4; ++e)
            margin[e] = me[e]->is_auto() ? k_nan : resolve_or_zero(*me[e], rem, cb[0]);
        float pad[4];
        float bor[4];
        resolve_edges(st.padding, rem, cb[0], pad);
        resolve_edges(st.border, rem, cb[0], bor);
        const float pb[2] = {pad[1] + pad[3] + bor[1] + bor[3], pad[0] + pad[2] + bor[0] + bor[2]};
        const float left = resolve(st.inset.left, rem, cb[0]);
        const float right = resolve(st.inset.right, rem, cb[0]);
        const float top = resolve(st.inset.top, rem, cb[1]);
        const float bottom = resolve(st.inset.bottom, rem, cb[1]);
        float size[2] = {resolve(st.width, rem, cb[0]), resolve(st.height, rem, cb[1])};
        apply_aspect(size, st.aspect_ratio);
        float min_size[2] = {resolve(st.min_width, rem, cb[0]), resolve(st.min_height, rem, cb[1])};
        float max_size[2] = {resolve(st.max_width, rem, cb[0]), resolve(st.max_height, rem, cb[1])};
        for (unsigned a = 0; a < 2; ++a) {
            min_size[a] = maybe_max(or_(min_size[a], pb[a]), pb[a]);
            size[a] = maybe_clamp(size[a], min_size[a], max_size[a]);
        }
        const float m_l = or_(margin[3], 0.f), m_r = or_(margin[1], 0.f);
        const float m_t = or_(margin[0], 0.f), m_b = or_(margin[2], 0.f);
        if (!def(size[0]) && def(left) && def(right)) {
            size[0] = fmax(cb[0] - m_l - m_r - left - right, 0.f);
            apply_aspect(size, st.aspect_ratio);
            for (unsigned a = 0; a < 2; ++a)
                size[a] = maybe_clamp(size[a], min_size[a], max_size[a]);
        }
        if (!def(size[1]) && def(top) && def(bottom)) {
            size[1] = fmax(cb[1] - m_t - m_b - top - bottom, 0.f);
            apply_aspect(size, st.aspect_ratio);
            for (unsigned a = 0; a < 2; ++a)
                size[a] = maybe_clamp(size[a], min_size[a], max_size[a]);
        }
        if (!def(size[0]) || !def(size[1])) {
            lx::layout::child_query q{};
            q.known_width = size[0];
            q.known_height = size[1];
            q.available_width = maybe_clamp(cb[0], min_size[0], max_size[0]);
            q.available_height = maybe_clamp(cb[1], min_size[1], max_size[1]);
            q.parent_width = s.inner[0];
            q.parent_height = s.inner[1];
            q.content_size = false;
            const flex_size got = ch.measure ? ch.measure(ch.context, i, q) : flex_size{};
            if (!def(size[0]))
                size[0] = maybe_clamp(got.width, min_size[0], max_size[0]);
            if (!def(size[1]))
                size[1] = maybe_clamp(got.height, min_size[1], max_size[1]);
        }
        // Auto margins share what the insets leave when both insets on the axis are set
        // (CSS 2 §10.3.7, §10.6.4); otherwise they are 0.
        const bool both_w = def(left) && def(right);
        const bool both_h = def(top) && def(bottom);
        const float free_w = fmax(cb[0] - size[0] - m_l - m_r - (both_w ? left + right : 0.f), 0.f);
        const float free_h = fmax(cb[1] - size[1] - m_t - m_b - (both_h ? top + bottom : 0.f), 0.f);
        const unsigned autos_w =
            both_w ? (def(margin[3]) ? 0u : 1u) + (def(margin[1]) ? 0u : 1u) : 0u;
        const unsigned autos_h =
            both_h ? (def(margin[0]) ? 0u : 1u) + (def(margin[2]) ? 0u : 1u) : 0u;
        const float res[4] = {
            def(margin[0]) ? margin[0] : (autos_h ? free_h / static_cast<float>(autos_h) : 0.f),
            def(margin[1]) ? margin[1] : (autos_w ? free_w / static_cast<float>(autos_w) : 0.f),
            def(margin[2]) ? margin[2] : (autos_h ? free_h / static_cast<float>(autos_h) : 0.f),
            def(margin[3]) ? margin[3] : (autos_w ? free_w / static_cast<float>(autos_w) : 0.f),
        };
        const float start_inset[2] = {left, top};
        const float end_inset[2] = {right, bottom};
        float pos[2];
        for (unsigned a = 0; a < 2; ++a) {
            const unsigned se = start_edge(a);
            const unsigned ee = end_edge(a);
            if (def(start_inset[a])) {
                pos[a] = pad_origin[a] + start_inset[a] + res[se];
            } else if (def(end_inset[a])) {
                pos[a] = pad_origin[a] + cb[a] - end_inset[a] - res[ee] - size[a];
            } else {
                // The static position: where a sole flex item would go (CSS Flexbox §4.1).
                const float lo = content_start[a] + res[se];
                const float hi =
                    s.outer[a] -
                    (a == 0 ? s.padding[1] + s.border[1] : s.padding[2] + s.border[2]) - size[a] -
                    res[ee];
                int where = 0; // 0 start, 1 end, 2 center
                if (a == m) {
                    switch (cs.justify_content) {
                    case content_align::end:
                        where = 1;
                        break;
                    case content_align::flex_end:
                        where = s.reverse ? 0 : 1;
                        break;
                    case content_align::flex_start:
                    case content_align::stretch:
                    case content_align::space_between:
                        where = s.reverse ? 1 : 0;
                        break;
                    case content_align::center:
                    case content_align::space_around:
                    case content_align::space_evenly:
                        where = 2;
                        break;
                    default:
                        where = 0;
                        break;
                    }
                } else {
                    item_align al =
                        st.align_self == item_align::automatic ? cs.align_items : st.align_self;
                    switch (al) {
                    case item_align::end:
                        where = 1;
                        break;
                    case item_align::center:
                        where = 2;
                        break;
                    case item_align::flex_end:
                        where = s.wrap_reverse ? 0 : 1;
                        break;
                    case item_align::start:
                        where = 0;
                        break;
                    default:
                        where = s.wrap_reverse ? 1 : 0;
                        break;
                    }
                }
                pos[a] = where == 0 ? lo : (where == 1 ? hi : (lo + hi) / 2.f);
            }
        }
        lx::rect2f& r = out[i];
        r.x = pos[0];
        r.y = pos[1];
        r.width = size[0];
        r.height = size[1];
        extent[0] = fmax(extent[0], r.x + r.width + res[1] - pad_origin[0]);
        extent[1] = fmax(extent[1], r.y + r.height + res[2] - pad_origin[1]);
    }

    flex_output result{};
    result.width = s.outer[0];
    result.height = s.outer[1];
    result.content_width = fmax(extent[0] + (count > 0 ? s.padding[1] : 0.f), cb[0]);
    result.content_height = fmax(extent[1] + (count > 0 ? s.padding[2] : 0.f), cb[1]);
    result.inner_width = s.inner[0];
    result.inner_height = s.inner[1];
    return result;
}
