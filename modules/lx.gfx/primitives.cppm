module;

import lx.foundation;

export module lx.gfx:primitives;

import std;

export namespace lx::gfx {

// ── Primitive kinds (ADR-010 §4.2) ──────────────────────────────────────────────
//
// The seven kinds every backend draws. The declaration order is the tie-break inside one
// `order` value (GPUI's): a shadow sorts under the quad it belongs to, and `surface`
// (today's `blit_command`) last.

enum class primitive_kind : std::uint8_t {
    shadow,
    quad,
    path,
    underline,
    mono_sprite,
    poly_sprite,
    surface,
};

inline constexpr unsigned k_primitive_kinds = 7;

/// Primitives per view (surfaces keep the compositor's own `k_max_draw_commands` cap).
inline constexpr unsigned k_max_primitives = 16384;
/// Runs of equal `(kind, page)` after the sort: one instanced draw each.
inline constexpr unsigned k_max_batches = 1024;
/// The overlap grid's cell, before doubling for a view larger than `k_max_order_cells`.
inline constexpr unsigned k_order_cell_px = 64;
/// 8192 × 8192 px at 64 px cells; a larger view doubles the cell until it fits.
inline constexpr unsigned k_max_order_cells = 16384;
/// Nesting of `order_grid::push_layer`.
inline constexpr unsigned k_max_order_layers = 32;

// ── Half floats ─────────────────────────────────────────────────────────────────
//
// IEEE 754 binary16, round-to-nearest-even, without intrinsics so the packing is the
// same on every host and in `constexpr`. A finite input never produces an infinity: past
// 65504 it clamps to the largest finite half, which is what a colour or a radius wants.
// NaN stays NaN (quiet), ±inf stays ±inf, subnormals round like everything else.
//
// `to_half_reference` is the conversion written out case by case; `to_half` is the one
// emitters call, bit-identical to it for every float (test_primitives checks the two
// against each other) but branch-light where colours, radii and widths live.

/// Every float, case by case: what `to_half` must equal bit for bit.
[[nodiscard]] constexpr std::uint16_t to_half_reference(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16u) & 0x8000u);
    const std::uint32_t exp = (bits >> 23u) & 0xFFu;
    std::uint32_t mant = bits & 0x7FFFFFu;
    if (exp == 0xFFu)
        return static_cast<std::uint16_t>(sign | (mant != 0 ? 0x7E00u : 0x7C00u));
    // Rebias 127 → 15.
    const int e = static_cast<int>(exp) - 112;
    if (e >= 31)
        return static_cast<std::uint16_t>(sign | 0x7BFFu);
    if (e <= 0) {
        // Below 2^-25 rounds to zero; up to 2^-14 is a half subnormal (value = m · 2^-24).
        if (e < -10)
            return sign;
        mant |= 0x800000u;
        const unsigned shift = static_cast<unsigned>(14 - e);
        std::uint32_t half_mant = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half_mant & 1u)))
            ++half_mant; // may carry into the exponent: exactly the smallest normal
        return static_cast<std::uint16_t>(sign | half_mant);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10u) | (mant >> 13u);
    const std::uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u)))
        ++half;
    if (half >= 0x7C00u)
        half = 0x7BFFu; // rounding carried into infinity: clamp
    return static_cast<std::uint16_t>(sign | half);
}

/// `to_half_reference` with one branch for the common case: a magnitude in the normal
/// half range or above it (finite) rebiases and rounds in integer arithmetic, clamping past
/// 65504 to the largest finite half; zero and anything below half the smallest subnormal
/// is a signed zero; subnormals, infinities and NaN take the reference path.
[[nodiscard]] constexpr std::uint16_t to_half(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16u) & 0x8000u);
    std::uint32_t mag = bits & 0x7FFFFFFFu;
    // [2^-14, +inf): unsigned wrap puts everything below 2^-14 past the range.
    if (mag - 0x38800000u < 0x7F800000u - 0x38800000u) [[likely]] {
        // 65504 is the largest finite half; everything from it up (65520 would round to
        // infinity) clamps to it.
        mag = mag < 0x477FE000u ? mag : 0x477FE000u;
        // Rebias 127 → 15 (subtract 112 << 23), round to nearest even on the 13 dropped
        // bits; a carry out of the mantissa lands in the exponent, which is correct.
        const std::uint32_t odd = (mag >> 13u) & 1u;
        return static_cast<std::uint16_t>(sign | ((mag - 0x38000000u + 0x0FFFu + odd) >> 13u));
    }
    if (mag < 0x33000000u) // below 2^-25 (zero included): rounds to zero
        return sign;
    return to_half_reference(value);
}

[[nodiscard]] constexpr float from_half(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16u;
    const std::uint32_t exp = (h >> 10u) & 0x1Fu;
    const std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits = sign;
    if (exp == 0x1Fu) {
        bits |= 0x7F800000u | (mant << 13u);
    } else if (exp == 0) {
        if (mant != 0) {
            // Subnormal: shift the leading one into the implicit position.
            std::uint32_t m = mant;
            std::uint32_t e = 113;
            while ((m & 0x400u) == 0) {
                m <<= 1u;
                --e;
            }
            bits |= (e << 23u) | ((m & 0x3FFu) << 13u);
        }
    } else {
        bits |= ((exp + 112u) << 23u) | (mant << 13u);
    }
    return std::bit_cast<float>(bits);
}

/// Four halves of premultiplied rgba — the colour encoding of every instance below.
struct packed_rgba {
    std::uint16_t r = 0;
    std::uint16_t g = 0;
    std::uint16_t b = 0;
    std::uint16_t a = 0;
};

/// `color` premultiplied by its alpha and packed to halves: colours are premultiplied once,
/// at emission (ADR-010 §4.5). No colour-space conversion happens here — the caller supplies
/// the colour in the space the target surface blends in (`color::space` / `encoding` are
/// not consulted), so a theme compiled for sRGB-space blending passes straight through.
[[nodiscard]] constexpr packed_rgba premultiplied_half(lx::color c) {
    return {to_half(c.r * c.a), to_half(c.g * c.a), to_half(c.b * c.a), to_half(c.a)};
}

// ── GPU instances (ADR-010 §4.2) ────────────────────────────────────────────────
//
// Uploaded as they are into an SSBO (Vulkan and GLES 3.1) or walked by the CPU renderer,
// so every layout is std430-compatible scalar: only `float`, 32-bit and narrower unsigned
// integers, no vector or matrix types, every field explicit, no implicit padding, alignment
// 4. A shader reads a `f16×4` as two `uint` words (`unpackHalf2x16`), and a `u16×4` or
// `u8, u8, u16` group as one word. Coordinates are output pixels; `bounds` and `clip` are
// x, y, w, h. Arrays are indexed in the order the comment names.

/// Rounded rectangle: per-corner radius, per-edge border width, solid or two-stop gradient
/// background, solid or dashed border. 80 bytes.
struct quad_instance {
    float bounds[4]{};   ///< x, y, w, h
    float clip[4]{};     ///< x, y, w, h — the content mask
    std::uint16_t radii[4]{};          ///< f16: top-left, top-right, bottom-right, bottom-left
    std::uint16_t border_widths[4]{};  ///< f16: top, right, bottom, left
    std::uint16_t background[4]{};     ///< premultiplied f16 rgba, gradient stop 0
    std::uint16_t background_end[4]{}; ///< premultiplied f16 rgba, gradient stop 1 (= stop 0 when solid)
    std::uint16_t border_color[4]{};   ///< premultiplied f16 rgba
    std::uint16_t gradient_angle{};    ///< f16 degrees: 0 left → right, 90 top → bottom (`:ui_cpu`)
    std::uint8_t border_style = 0;     ///< 0 solid, 1 dashed
    std::uint8_t gradient_space = 0;   ///< 0 sRGB, 1 Oklab
    std::uint32_t pad = 0;
};
static_assert(sizeof(quad_instance) == 80);
static_assert(alignof(quad_instance) == 4);
static_assert(std::is_trivially_copyable_v<quad_instance> && std::is_standard_layout_v<quad_instance>);

/// Blurred rounded rectangle, outer or inset. 64 bytes. `bounds` is the box the shadow is
/// cast by; an outer shadow reaches `3 · sigma + spread` past it.
struct shadow_instance {
    float bounds[4]{};
    float clip[4]{};
    std::uint16_t radii[4]{}; ///< f16: top-left, top-right, bottom-right, bottom-left
    std::uint16_t color[4]{}; ///< premultiplied f16 rgba
    float sigma = 0.f;        ///< Gaussian blur, px
    float spread = 0.f;       ///< grows (or, negative, shrinks) the box before blurring, px
    std::uint32_t flags = 0;  ///< bit 0: inset
    std::uint32_t pad = 0;
};
static_assert(sizeof(shadow_instance) == 64);
static_assert(alignof(shadow_instance) == 4);
static_assert(std::is_trivially_copyable_v<shadow_instance> && std::is_standard_layout_v<shadow_instance>);

/// Atlas tile × colour: a glyph or icon (`mono_sprite`, R8 coverage), an image or colour
/// emoji (`poly_sprite`, RGBA8) or a path's coverage tile (`path`). 64 bytes, one layout
/// for the three kinds. The last eight bytes are `params`: per-corner radii (top-left,
/// top-right, bottom-right, bottom-left) for `poly_sprite`, a 2 × 2 rotation (cos, sin,
/// −sin, cos) for `mono_sprite`, unused for `path`. §4.2 lists 60 bytes; `pad1` makes the
/// 64 the ADR's size table asks for.
struct sprite_instance {
    float bounds[4]{};
    float clip[4]{};
    std::uint16_t uv[4]{};  ///< atlas texels: u0, v0, u1, v1
    std::uint8_t page = 0;  ///< atlas page (sampler array index)
    std::uint8_t flags = 0; ///< bit 0: grayscale (poly)
    std::uint16_t pad0 = 0;
    std::uint16_t color[4]{};  ///< premultiplied f16 rgba (mono: tint; poly: multiplier; path: fill)
    std::uint16_t params[4]{}; ///< f16: radii (poly) or rotation (mono)
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(sprite_instance) == 64);
static_assert(alignof(sprite_instance) == 4);
static_assert(std::is_trivially_copyable_v<sprite_instance> && std::is_standard_layout_v<sprite_instance>);

/// Straight or wavy line of a thickness. 48 bytes.
struct underline_instance {
    float bounds[4]{};
    float clip[4]{};
    std::uint16_t color[4]{};  ///< premultiplied f16 rgba
    std::uint16_t thickness{}; ///< f16 px
    std::uint16_t wavy = 0;    ///< 0 straight, 1 sine
    std::uint32_t pad = 0;
};
static_assert(sizeof(underline_instance) == 48);
static_assert(alignof(underline_instance) == 4);
static_assert(std::is_trivially_copyable_v<underline_instance> &&
              std::is_standard_layout_v<underline_instance>);

/// Atlas texel box of a sprite, as `draw_scope::sprite` takes it.
struct sprite_uv {
    std::uint16_t u0 = 0;
    std::uint16_t v0 = 0;
    std::uint16_t u1 = 0;
    std::uint16_t v1 = 0;
};

// ── The sorted stream (ADR-010 §4.3) ────────────────────────────────────────────

/// One entry of a view's primitive stream, sorted by `(order, kind, page)` for batching.
/// `index` points into the typed instance array of `kind` (`mono_sprite`, `poly_sprite` and
/// `path` share the sprite array); for `surface` it indexes the `draw_command` array.
struct primitive_ref {
    std::uint32_t order = 0;
    primitive_kind kind = primitive_kind::quad;
    std::uint8_t page = 0;
    std::uint16_t reserved = 0;
    std::uint32_t index = 0;
};
static_assert(sizeof(primitive_ref) == 12);
static_assert(std::is_trivially_copyable_v<primitive_ref> && std::is_standard_layout_v<primitive_ref>);

/// A run of refs of one `(kind, page)`: one instanced draw. `first` and `count` index the
/// sorted ref stream; `bounds` is the union of the run's (clipped) bounds, so a damage rect
/// that misses it skips the draw.
struct batch {
    primitive_kind kind = primitive_kind::quad;
    std::uint8_t page = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    lx::rect2i bounds{};
};
static_assert(std::is_trivially_copyable_v<batch>);

/// The overlap grid that replaces GPUI's bounds tree: a fixed array of cells over the view,
/// each holding the highest order drawn into it. A primitive's order is one more than the
/// highest order under it, written back into the cells it covers; two primitives that do
/// not overlap share an order and so batch together whatever the paint order between them.
/// O(covered cells) per primitive, no allocation, UI affinity (one grid per draw list).
class order_grid {
public:
    order_grid() { reset({}); }

    /// Clears every cell and the layer stack for a view of `view_px`. The cell is 64 px,
    /// doubled while the view needs more than `k_max_order_cells`. An empty size is
    /// unbounded: the grid covers 8192 × 8192 px and anything past that is off-view. Cells
    /// are only cleared when something was assigned since the last reset (or the view
    /// outgrew the cells known to be zero) — a frame that assigns no order costs no memset. `scene::draw_list` assigns none for a
    /// list without primitives (the compositor's scene, before it draws chrome).
    void reset(lx::size2i view_px);

    /// The order for a primitive covering `bounds` (clamped to the view), written back into
    /// the covered cells: `max(layer base, 1 + max over the cells)`. An empty or off-view
    /// rect returns the current layer base and writes nothing.
    [[nodiscard]] std::uint32_t assign(lx::rect2i bounds);

    /// Lifts the base order above everything assigned so far: an overlay, popup or drag
    /// cannot interleave with what is under it (GPUI's `push_layer`). At most
    /// `k_max_order_layers` deep; deeper pushes keep the innermost base.
    void push_layer();
    void pop_layer();

    [[nodiscard]] std::uint32_t layer_base() const { return base_; }
    [[nodiscard]] unsigned layer_depth() const { return depth_; }
    /// The highest order assigned since `reset`.
    [[nodiscard]] std::uint32_t max_order() const { return max_; }
    [[nodiscard]] unsigned cell_px() const { return cell_px_; }
    [[nodiscard]] unsigned cells_x() const { return cells_x_; }
    [[nodiscard]] unsigned cells_y() const { return cells_y_; }
    [[nodiscard]] lx::size2i view() const { return view_; }

private:
    static constexpr int k_unbounded_px = 8192;

    std::uint32_t cells_[k_max_order_cells]{};
    std::uint32_t layer_bases_[k_max_order_layers]{};
    std::uint32_t base_ = 0;
    std::uint32_t max_ = 0;
    /// `cells_[0, cleared_)` are known to be zero while `max_ == 0` (value-initialised, or
    /// cleared by the last `reset` that had to).
    unsigned cleared_ = k_max_order_cells;
    unsigned depth_ = 0;
    unsigned cell_px_ = k_order_cell_px;
    unsigned shift_ = 6;
    unsigned cells_x_ = 0;
    unsigned cells_y_ = 0;
    lx::size2i view_{};
};

} // namespace lx::gfx


void lx::gfx::order_grid::reset(lx::size2i view_px) {
    view_ = view_px;
    if (view_.width <= 0 || view_.height <= 0)
        view_ = {k_unbounded_px, k_unbounded_px};
    cell_px_ = k_order_cell_px;
    shift_ = 6;
    const auto w = static_cast<unsigned>(view_.width);
    const auto h = static_cast<unsigned>(view_.height);
    for (;;) {
        cells_x_ = (w + cell_px_ - 1) / cell_px_;
        cells_y_ = (h + cell_px_ - 1) / cell_px_;
        if (static_cast<unsigned long long>(cells_x_) * cells_y_ <= k_max_order_cells)
            break;
        cell_px_ *= 2;
        ++shift_;
    }
    const unsigned used = cells_x_ * cells_y_;
    if (max_ != 0 || used > cleared_) {
        for (unsigned i = 0; i < used; ++i)
            cells_[i] = 0;
        // Cells past `used` may hold orders from a larger earlier view: only these are known.
        cleared_ = used;
    }
    base_ = 0;
    max_ = 0;
    depth_ = 0;
}

std::uint32_t lx::gfx::order_grid::assign(lx::rect2i bounds) {
    if (bounds.width <= 0 || bounds.height <= 0)
        return base_;
    const long long x0 = bounds.x > 0 ? bounds.x : 0;
    const long long y0 = bounds.y > 0 ? bounds.y : 0;
    const long long bx1 = static_cast<long long>(bounds.x) + bounds.width;
    const long long by1 = static_cast<long long>(bounds.y) + bounds.height;
    const long long x1 = bx1 < view_.width ? bx1 : view_.width;
    const long long y1 = by1 < view_.height ? by1 : view_.height;
    if (x1 <= x0 || y1 <= y0)
        return base_;
    const unsigned cx0 = static_cast<unsigned>(x0) >> shift_;
    const unsigned cy0 = static_cast<unsigned>(y0) >> shift_;
    const unsigned cx1 = static_cast<unsigned>(x1 - 1) >> shift_;
    const unsigned cy1 = static_cast<unsigned>(y1 - 1) >> shift_;
    if (cx0 == cx1 && cy0 == cy1) {
        // One cell — most primitives of a UI (a glyph, an icon, a small quad): read, bump,
        // write back, without the row loops.
        std::uint32_t& cell = cells_[cy0 * cells_x_ + cx0];
        const std::uint32_t order = cell + 1 > base_ ? cell + 1 : base_;
        cell = order;
        max_ = order > max_ ? order : max_;
        return order;
    }
    std::uint32_t highest = 0;
    for (unsigned cy = cy0; cy <= cy1; ++cy) {
        const std::uint32_t* row = cells_ + cy * cells_x_;
        for (unsigned cx = cx0; cx <= cx1; ++cx)
            if (row[cx] > highest)
                highest = row[cx];
    }
    const std::uint32_t order = highest + 1 > base_ ? highest + 1 : base_;
    for (unsigned cy = cy0; cy <= cy1; ++cy) {
        std::uint32_t* row = cells_ + cy * cells_x_;
        for (unsigned cx = cx0; cx <= cx1; ++cx)
            row[cx] = order;
    }
    if (order > max_)
        max_ = order;
    return order;
}

void lx::gfx::order_grid::push_layer() {
    if (depth_ >= k_max_order_layers)
        return;
    layer_bases_[depth_++] = base_;
    base_ = max_ + 1;
}

void lx::gfx::order_grid::pop_layer() {
    if (depth_ == 0)
        return;
    base_ = layer_bases_[--depth_];
}
